# Linux ARM64 内存架构：NUMA / SPARSEMEM_VMEMMAP / Zone

> 适用内核版本：Linux 6.x，架构：ARM64 (AArch64)

---

## 目录

1. [总体架构层次](#1-总体架构层次)
2. [ARM64 内存拓扑类型](#2-arm64-内存拓扑类型)
3. [NUMA 详解](#3-numa-详解)
4. [物理内存模型](#4-物理内存模型)
5. [SPARSEMEM_VMEMMAP 详解](#5-sparsemem_vmemmap-详解)
6. [ARM64 虚拟地址空间与页大小](#6-arm64-虚拟地址空间与页大小)
7. [Zone 管理](#7-zone-管理)
8. [三层协作关系](#8-三层协作关系)
9. [THP 与 HugePage](#9-thp-与-hugepage)
10. [启动初始化流程](#10-启动初始化流程)
11. [关键 Kconfig 配置项](#11-关键-kconfig-配置项)
12. [参考文件索引](#12-参考文件索引)

---

## 1. 总体架构层次

```
┌─────────────────────────────────────────────────────┐
│               用户空间 / 内核分配请求                  │
└──────────────────────────┬──────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────┐
│                    NUMA 拓扑层                        │
│   pg_data_t (NODE_DATA)  ·  node_distance()          │
│   决策：从哪个节点分配内存                              │
└──────────────────────────┬──────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────┐
│                  Zone 管理层                          │
│   struct zone  ·  Buddy System  ·  per-cpu pageset   │
│   决策：从节点内哪个 zone、哪个 order 找空闲页          │
└──────────────────────────┬──────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────┐
│              SPARSEMEM_VMEMMAP 物理内存模型            │
│   struct page  ·  mem_section  ·  vmemmap[]          │
│   决策：pfn ↔ struct page 的 O(1) 双向转换            │
└─────────────────────────────────────────────────────┘
```

---

## 2. ARM64 内存拓扑类型

> **定义**：内存拓扑（Memory Topology）描述的是**系统中 CPU 与物理内存之间的距离关系**——即哪些 CPU 与哪些内存在物理上更近、访问延迟更低。它回答的问题是：
> "这块内存属于谁？哪个 CPU 访问它最快？"
>
> 内核依据拓扑关系将内存划分为若干 **node（节点）**，在分配内存时优先选择距当前 CPU 最近的 node，以最大化带宽、最小化延迟（NUMA-aware 分配）。

内存拓扑描述**处理器与内存之间的访问关系**，ARM64 支持以下两种类型：

### 2.1 UMA（Uniform Memory Access）

所有 CPU 访问所有内存的延迟相同，内核视为单一 node 0。

```
CPU0  CPU1  CPU2  CPU3
  └────┴────┴────┘
         │
      统一内存总线
         │
       DRAM
```

- `CONFIG_NUMA` 未开启时的默认模式
- 内核只有 `NODE_DATA(0)` 一个节点
- 典型平台：单 socket 嵌入式系统、开发板

### 2.2 NUMA（Non-Uniform Memory Access）

不同 CPU 访问不同内存区域的延迟不同，内存按**访问延迟**分组为若干 **node**。

```
  Socket 0                Socket 1
┌─────────────┐        ┌─────────────┐
│  CPU0 CPU1  │◄─慢───►│  CPU2 CPU3  │
│   Node 0    │        │   Node 1    │
│   Memory    │        │   Memory    │
└─────────────┘        └─────────────┘
   本地 10ns               跨节点 30ns
```

- `CONFIG_NUMA` 开启
- 每个节点有独立 `pg_data_t`（`NODE_DATA(nid)`）
- ARM64 通过 **ACPI SRAT 表**或 **Device Tree `numa-node-id`** 描述拓扑
- 典型平台：多 socket ARM64 服务器（如 Ampere、Kunpeng、Neoverse）

### 2.3 NUMA 距离矩阵

节点间访问代价由 ACPI SLIT 表或 DT `distance-matrix` 属性提供：

```
        Node0  Node1  Node2
Node0 [  10     20     30  ]
Node1 [  20     10     20  ]
Node2 [  30     20     10  ]
```

- `LOCAL_DISTANCE = 10`（同节点本地访问）
- 跨节点距离通常为 20、30、40…
- 内核用此矩阵构建 `zonelist` 备用节点回退顺序

---

## 3. NUMA 详解

> **定义**：NUMA（Non-Uniform Memory Access，非一致性内存访问）是一种**硬件内存拓扑模型**，也是内核对应的**软件管理框架**。
>
> - **硬件层面**：多 socket 系统中每个 socket 拥有本地内存，CPU 访问本地内存比访问远端 socket 的内存延迟更低、带宽更大。
> - **软件层面**：内核将物理内存按 node 分组，每个 node 用 `pg_data_t` 描述，分配器优先从当前 CPU 所在 node 取页，同时维护节点间距离矩阵（SLIT/distance-matrix）用于回退决策。
>
> **NUMA 描述的核心问题**：
> 1. 内存段 `[start_pfn, end_pfn)` 属于哪个 node？
> 2. CPU `x` 属于哪个 node？
> 3. node `a` 到 node `b` 的访问距离是多少？

### 3.1 设计思想

NUMA（Non-Uniform Memory Access）将物理内存按**访问延迟**分组为若干 **node**。
内核优先从当前 CPU 所在的本地节点分配内存，以减少跨节点访问延迟。

```
  Socket 0                Socket 1
┌─────────────┐        ┌─────────────┐
│  CPU0 CPU1  │◄─慢───►│  CPU2 CPU3  │
│   Node 0    │        │   Node 1    │
│   Memory    │        │   Memory    │
└─────────────┘        └─────────────┘
   本地 10ns               跨节点 30ns
```

### 3.2 关键数据结构

#### `pg_data_t`（`include/linux/mmzone.h`）
每个 NUMA 节点对应一个 `pg_data_t` 实例，是节点内存管理的核心：

```c
typedef struct pglist_data {
    struct zone     node_zones[MAX_NR_ZONES]; // 该节点的所有 zone
    struct zonelist node_zonelists[MAX_ZONELISTS]; // 备用节点分配顺序
    int             nr_zones;                // 有效 zone 数量
    unsigned long   node_start_pfn;          // 节点起始页帧号
    unsigned long   node_present_pages;      // 实际存在的页数
    unsigned long   node_spanned_pages;      // 包含空洞的总跨度
    int             node_id;                 // 节点 ID
    wait_queue_head_t kswapd_wait;
    struct task_struct *kswapd;              // 该节点的内存回收线程
    // ...
} pg_data_t;

// 访问宏
#define NODE_DATA(nid) (node_data[nid])
```

#### `zonelist`（备用节点回退链）

```c
struct zonelist {
    struct zoneref _zonerefs[MAX_ZONES_PER_ZONELIST + 1];
};
// 分配失败时按此链顺序尝试其他节点
```

#### NUMA 距离表

```c
// arch/arm64/mm/numa.c
u8 __node_distance(int from, int to);
// 来源：ACPI SLIT 表 或 DT distance-matrix 属性
// LOCAL_DISTANCE = 10，跨节点通常 20/30/40...
```

#### `cpu_to_node_map`（`drivers/base/arch_numa.c`）

```c
static int cpu_to_node_map[NR_CPUS] = { [0 ... NR_CPUS-1] = NUMA_NO_NODE };
// CPU → NUMA 节点的映射表，启动时由 DT/ACPI 填充
```

### 3.3 关键函数

| 函数 | 文件 | 作用 |
|---|---|---|
| `arch_numa_init()` | `drivers/base/arch_numa.c` | NUMA 初始化入口，选择 ACPI/DT/dummy 路径 |
| `numa_init(init_func)` | `drivers/base/arch_numa.c` | 清空节点集合，调用回调，注册节点 |
| `of_numa_init()` | `drivers/of/of_numa.c` | 从 Device Tree 解析 NUMA 拓扑 |
| `acpi_numa_init()` | `drivers/acpi/numa/srat.c` | 从 ACPI SRAT/SLIT 表解析 NUMA 拓扑 |
| `dummy_numa_init()` | `drivers/base/arch_numa.c` | 降级为单节点模式（兜底） |
| `numa_add_memblk()` | `mm/numa_memblks.c` | 注册 [start, end) 内存段属于某 nid |
| `numa_register_nodes()` | `drivers/base/arch_numa.c` | 调用 `setup_node_data()` 注册所有节点 |
| `node_distance(a, b)` | `include/linux/topology.h` | 查询两节点间访问距离 |
| `early_map_cpu_to_node()` | `drivers/base/arch_numa.c` | 早期建立 CPU → NUMA 节点映射 |

### 3.4 ARM64 初始化路径选择

```c
void __init arch_numa_init(void)
{
    if (!numa_off) {
        // ACPI 平台（x86 风格服务器）
        if (!acpi_disabled && !numa_init(arch_acpi_numa_init))
            return;
        // DT 平台（ARM64 典型路径）
        if (acpi_disabled && !numa_init(of_numa_init))
            return;
    }
    // 兜底：单节点 dummy 模式
    numa_init(dummy_numa_init);
}
```

### 3.5 NUMA 内存分配策略

用户态通过 `mbind()` / `set_mempolicy()` 系统调用，内核通过 `mm/mempolicy.c` 实现以下策略：

| 策略 | 宏定义 | 行为 |
|---|---|---|
| **默认** | `MPOL_DEFAULT` | 本地节点优先，不够再按 zonelist 顺序回退 |
| **绑定** | `MPOL_BIND` | 只从指定节点集合分配，失败则 OOM |
| **交织** | `MPOL_INTERLEAVE` | 跨节点轮询分配，适合大数据均衡访问 |
| **优先** | `MPOL_PREFERRED` | 优先指定节点，失败后允许回退其他节点 |
| **本地** | `MPOL_LOCAL` | 强制使用当前运行 CPU 所在节点 |

```c
// 内核内部分配时的 NUMA 感知
alloc_pages_node(nid, gfp_mask, order)  // 指定节点
alloc_pages(gfp_mask, order)            // 当前节点（默认）
```

---

## 4. 物理内存模型

> **定义**：物理内存模型（Physical Memory Model）描述的是内核**如何组织和索引所有物理内存页**，即建立从物理页帧号（PFN）到 `struct page` 描述符的映射关系。
>
> 它回答的问题是：
> "给定一个物理地址（或 PFN），如何快速找到对应的 `struct page`？反之，给定 `struct page *`，如何得到它的物理地址？"
>
> 选择哪种模型取决于系统物理地址空间的连续性：
> - 地址空间连续 → FLATMEM（简单线性数组）
> - 地址空间稀疏/有大量空洞 → SPARSEMEM（ARM64 实际情况）

物理内存模型解决的问题：**如何将物理页帧号（pfn）映射到 `struct page`**。

### 4.1 三种模型对比

| 模型 | ARM64 支持 | 特点 |
|---|---|---|
| **FLATMEM** | ✗ | 全局线性 `mem_map[]` 数组；要求内存完全连续；ARM64 不使用 |
| **DISCONTIGMEM** | ✗ | 每节点一个 `mem_map[]`；已废弃，被 SPARSEMEM 替代 |
| **SPARSEMEM** | ✓ | 按 section（128MB）分段管理；支持内存热插拔和稀疏地址 |
| **SPARSEMEM_VMEMMAP** | ✓ **默认** | 在 SPARSEMEM 基础上用 vmemmap 虚拟映射，pfn→page 为 O(1) |

ARM64 **强制使用 SPARSEMEM_VMEMMAP**，由 `arch/arm64/Kconfig` 中 `select SPARSEMEM_VMEMMAP` 决定。

### 4.2 模型演进关系

```
FLATMEM          — 简单但要求内存连续
    │ 无法处理稀疏地址
    ▼
DISCONTIGMEM     — 按节点分段，但管理粗糙（已废弃）
    │ 需要更细粒度
    ▼
SPARSEMEM        — 按 128MB section 管理，支持热插拔
    │ pfn→page 需要两次间接寻址，有性能开销
    ▼
SPARSEMEM_VMEMMAP — vmemmap 直接映射，pfn→page 为 O(1)，ARM64 默认
```

### 4.3 SPARSEMEM_VMEMMAP 的性能优势

```c
// FLATMEM（仅供参考）
#define pfn_to_page(pfn)  (mem_map + (pfn))          // O(1) 但要求连续

// SPARSEMEM（无 vmemmap）
#define pfn_to_page(pfn)  section_mem_map 间接两跳   // 有开销

// SPARSEMEM_VMEMMAP（ARM64 实际）
#define pfn_to_page(pfn)  (vmemmap + (pfn))          // O(1)，等效 FLATMEM 性能
```

---

## 5. SPARSEMEM_VMEMMAP 详解

> **定义**：SPARSEMEM_VMEMMAP 是 Linux 内核在 SPARSEMEM 基础上的优化变体，是 ARM64 **唯一使用的物理内存模型**实现。
>
> - **SPARSEMEM**：将物理地址空间按固定大小（128MB 为一个 section）切分，每段独立存在，天然支持稀疏地址和内存热插拔，但 `pfn → struct page` 需要两次间接寻址，有性能损耗。
> - **VMEMMAP 扩展**：通过在内核虚拟地址空间中预留一段连续区域 `vmemmap`，将所有 section 的 `struct page` 数组**虚拟地址连续地**映射进来，使得 `pfn → struct page` 退化为一次加法，性能与 FLATMEM 相当。
>
> **SPARSEMEM_VMEMMAP 描述的核心内容**：
> 1. 物理地址空间被切分为哪些 section？每个 section 对应的 `struct page` 数组在哪里？
> 2. `vmemmap[pfn]` → 直接寻址任意物理页的描述符（O(1)）
> 3. 哪些 section 存在（`SECTION_MARKED_PRESENT`），哪些是空洞？

### 5.1 设计思想

ARM64 物理地址空间存在**大量空洞**（MMIO、固件保留等），无法用线性数组表示所有 `struct page`。

SPARSEMEM 将物理地址空间切分为固定大小的 **section**（默认 128MB），每个 section 独立存在。
VMEMMAP 在此基础上将所有 `struct page` 映射到一段**连续虚拟地址区域**，实现 O(1) 的 `pfn → page` 转换。

```
物理地址（有空洞）             vmemmap 虚拟地址（连续）
┌──────────┐                 ┌──────────────────────┐
│ section0 │ ──────────────► │ struct page[0..N]    │
├──────────┤                 ├──────────────────────┤
│  (空洞)  │   不映射        │ struct page[N+1..M]  │
├──────────┤                 ├──────────────────────┤
│ section1 │ ──────────────► │ ...                  │
└──────────┘                 └──────────────────────┘
                                      ↑
                                  vmemmap 基址
                          pfn → &vmemmap[pfn]  O(1)
```

### 5.2 关键数据结构

#### `mem_section`（`include/linux/mmzone.h`）

```c
struct mem_section {
    unsigned long section_mem_map;
    // 低位存标志位（SECTION_MARKED_PRESENT 等）
    // 高位指向该 section 的 struct page 基址（vmemmap 模式下不直接用）

    struct mem_section_usage *usage; // pageblock 迁移类型位图
};

// 全局二维数组（稀疏两级索引）
extern struct mem_section mem_section[NR_SECTION_ROOTS][SECTIONS_PER_ROOT];

// section 大小：PAGES_PER_SECTION = 1 << (SECTION_SIZE_BITS - PAGE_SHIFT)
// ARM64 默认：SECTION_SIZE_BITS = 27，即每 section = 128MB
```

#### `struct page`（`include/linux/mm_types.h`）
每个物理页对应一个 `struct page`（40~64 字节），通过 vmemmap 连续排列：

```c
struct page {
    unsigned long flags;        // PG_locked / PG_dirty / PG_uptodate ...
    union {
        struct { // 匿名页/文件页
            struct list_head lru;
            struct address_space *mapping;
            pgoff_t index;
        };
        struct { // Buddy System 空闲页
            unsigned long private; // order
        };
        struct { // slab 页
            struct kmem_cache *slab_cache;
            void *freelist;
        };
    };
    atomic_t    _refcount;
    atomic_t    _mapcount;
    // ...
};
```

#### vmemmap（ARM64 虚拟地址布局）

```
vmemmap 基址（ARM64 48-bit VA，4K页）：
  VMEMMAP_START = 0xFFFFFFBDC0000000（示例）

pfn → struct page：
  page = vmemmap + pfn * sizeof(struct page)
  即：vmemmap[pfn]

struct page → pfn：
  pfn = (page - vmemmap) / sizeof(struct page)
  即：page - vmemmap
```

### 5.3 关键函数与宏

| 函数/宏 | 文件 | 作用 |
|---|---|---|
| `pfn_to_page(pfn)` | `include/asm-generic/memory_model.h` | pfn → struct page，O(1) |
| `page_to_pfn(page)` | `include/asm-generic/memory_model.h` | struct page → pfn，O(1) |
| `sparse_init()` | `mm/sparse.c` | 初始化所有 section，建立 vmemmap 映射 |
| `sparse_mem_map_populate()` | `mm/sparse-vmemmap.c` | 为 section 分配并映射 vmemmap 页表 |
| `vmemmap_populate()` | `arch/arm64/mm/mmu.c` | ARM64 实现：建立 vmemmap 物理映射 |
| `pfn_valid(pfn)` | `include/linux/mmzone.h` | 判断 pfn 是否对应存在的 section |
| `__section_nr(ms)` | `mm/sparse.c` | 获取 mem_section 的编号 |
| `memory_present()` | `mm/sparse.c` | 标记某 pfn 范围的内存存在 |

### 5.4 vmemmap 建立算法（ARM64）

```
sparse_init()
    └── sparse_init_nid()          // 按 node 初始化
            └── sparse_mem_map_populate()
                    └── vmemmap_populate(start, end, nid)
                            // ARM64 实现：
                            // 1. 按 PMD/PUD 对齐尝试用大页（2MB）映射
                            // 2. 不对齐则退回 4KB 页映射
                            // 3. 调用 vmemmap_alloc_block_buf() 分配物理页
                            // 4. 填充页表项（set_pmd / set_pte）
```

**大页优化**：vmemmap 区域会尽量使用 2MB 大页映射，降低页表层级，减少 TLB 压力。

---

## 6. ARM64 虚拟地址空间与页大小

### 6.1 虚拟地址空间布局（48-bit，4K 页）

```
高地址 0xFFFF_FFFF_FFFF_FFFF ─────────────────────────
                              fixmap（固定映射）
                              PCI I/O 空间
                              vmemmap（struct page 映射）
  内核空间                    vmalloc / ioremap 区域
  0xFFFF_0000_0000_0000       内核直接映射（PAGE_OFFSET）
                              内核代码 / 数据 / BSS
高地址 0xFFFF_0000_0000_0000 ─────────────────────────

          （空洞，非规范地址）

低地址 0x0000_FFFF_FFFF_FFFF ─────────────────────────
  用户空间                    栈（向下增长）
  0x0000_0000_0000_0000       代码段 / 数据段 / 堆
低地址 0x0000_0000_0000_0000 ─────────────────────────
```

ARM64 虚拟地址有效位数取决于页大小配置：

| 配置 | 用户/内核 VA 位数 | 页表级数 |
|---|---|---|
| 4KB 页 | 48-bit | 4级（L0~L3）|
| 4KB 页 + LPA2 | 52-bit | 5级 |
| 16KB 页 | 47-bit | 4级 |
| 64KB 页 | 42-bit | 3级 |

### 6.2 支持的页大小

| 页大小 | Kconfig | 普通大页尺寸 | 巨型大页尺寸 |
|---|---|---|---|
| **4KB** | `CONFIG_ARM64_4K_PAGES` | 2MB（PMD） | 1GB（PUD） |
| **16KB** | `CONFIG_ARM64_16K_PAGES` | 32MB（PMD） | 无 |
| **64KB** | `CONFIG_ARM64_64K_PAGES` | 512MB（PMD） | 无 |

**默认使用 4KB 页**，兼容性最好；64KB 页可减少页表层级，适合大内存服务器。

---

## 7. Zone 管理

> **定义**：Zone（内存区域）是内核在单个 NUMA node 内部对物理内存按**地址范围和用途**进行的二次分类。
>
> - 产生原因：不同硬件设备对 DMA 操作有地址限制（如某些设备只能访问 < 4GB 的地址），内核必须能单独管理这些受限内存。
> - 每个 Zone 维护独立的 **Buddy System 空闲页链表**，分配器根据 GFP 标志（`GFP_DMA`、`GFP_DMA32` 等）选择合适的 Zone。
>
> **Zone 描述的核心内容**：
> 1. 该地址范围内有多少可用物理页（`managed_pages`）？
> 2. 当前空闲多少页？水位（min/low/high watermark）是否健康？
> 3. 按 2 的幂次（order 0~10）组织的空闲块链表（`free_area[]`）状态如何？
>
> **与 NUMA 的关系**：NUMA 描述的是 node 粒度（哪块内存离哪个 CPU 近），Zone 描述的是 node 内部的地址约束分区；一个 node 包含多个 Zone，一个 Zone 不跨 node。

### 7.1 设计思想

由于历史遗留（DMA 限制）和硬件约束，物理内存按**地址范围**划分为 Zone，
Buddy System 在每个 Zone 内独立维护空闲页链表。

### 7.2 ARM64 Zone 布局

```
物理地址低 ──────────────────────────────────── 物理地址高
┌──────────┬──────────┬────────────────────────┐
│ ZONE_DMA │ZONE_DMA32│     ZONE_NORMAL         │
│ (可选)   │ (<4GB)   │  (其余所有内存)          │
└──────────┴──────────┴────────────────────────┘
```

| Zone | 地址范围 | 用途 |
|---|---|---|
| `ZONE_DMA` | 平台相关（如 < 1GB）| 旧式 DMA 设备限制 |
| `ZONE_DMA32` | < 4GB | 32-bit DMA 设备 |
| `ZONE_NORMAL` | 剩余所有内存 | 通用内存 |
| `ZONE_MOVABLE` | 可配置 | 热插拔/内存碎片整理 |

### 7.3 关键数据结构

#### `struct zone`（`include/linux/mmzone.h`）

```c
struct zone {
    unsigned long _watermark[NR_WMARK]; // min/low/high 水位线
    unsigned long watermark_boost;

    struct pglist_data *zone_pgdat;    // 所属节点
    struct per_cpu_pages __percpu *per_cpu_pageset; // per-CPU 页缓存

    unsigned long zone_start_pfn;      // zone 起始 pfn
    unsigned long managed_pages;       // Buddy 管理的页数
    unsigned long spanned_pages;       // 包含空洞的跨度
    unsigned long present_pages;       // 实际存在的页数

    // Buddy System 核心：11个 order 的空闲链表
    struct free_area free_area[MAX_ORDER];

    // 迁移类型（反碎片化）
    // MIGRATE_UNMOVABLE / MIGRATE_MOVABLE / MIGRATE_RECLAIMABLE

    spinlock_t lock;
    const char *name;
} ____cacheline_internodealigned_in_smp;
```

#### `struct free_area`（Buddy System）

```c
struct free_area {
    struct list_head free_list[MIGRATE_TYPES]; // 按迁移类型分链表
    unsigned long    nr_free;                  // 该 order 的空闲块数
};
// order 0: 4KB, order 1: 8KB, ..., order 10: 4MB (MAX_ORDER=11)
```

#### `struct per_cpu_pages`（per-CPU 页缓存）

```c
struct per_cpu_pages {
    spinlock_t lock;
    int count;           // 当前缓存页数
    int high;            // 高水位，超过则归还给 Buddy
    int batch;           // 每次批量操作数量
    struct list_head lists[NR_PCP_LISTS]; // 按迁移类型和 order 缓存
};
// 减少多核竞争 Buddy System 的 spinlock
```

### 7.4 Buddy System 算法

**核心思想**：将空闲页按 2 的幂次（order）组织，合并相邻的"伙伴"块。

```
分配 order=1（8KB）：
  free_area[1] 有空闲？
    是 → 直接取出，更新 nr_free
    否 → 从 free_area[2] 取一个 16KB 块，拆分为两个 8KB，
          一个返回，一个加入 free_area[1]

释放 order=0（4KB）的页 pfn：
  计算 buddy_pfn = pfn ^ (1 << order)
  buddy 也空闲？
    是 → 合并为 order=1 的块，递归检查更高 order
    否 → 加入 free_area[0]
```

**Buddy 计算公式**：
$$\text{buddy\_pfn} = \text{pfn} \oplus (1 \ll \text{order})$$

### 7.5 关键函数

| 函数 | 文件 | 作用 |
|---|---|---|
| `zone_sizes_init()` | `arch/arm64/mm/init.c` | ARM64 初始化各 zone 大小 |
| `free_area_init()` | `mm/page_alloc.c` | 初始化所有节点的 zone 和 free_area |
| `__alloc_pages()` | `mm/page_alloc.c` | 核心分配入口，含 NUMA 策略 |
| `rmqueue()` | `mm/page_alloc.c` | 从 zone 的 free_area 取页 |
| `__free_one_page()` | `mm/page_alloc.c` | 释放页并尝试合并 buddy |
| `get_page_from_freelist()` | `mm/page_alloc.c` | 按 zonelist 顺序找可用 zone |
| `alloc_pages_node(nid,...)` | `include/linux/gfp.h` | 指定 NUMA 节点分配 |
| `wakeup_kswapd()` | `mm/vmscan.c` | 空闲页低于水位时唤醒回收 |

### 7.6 内存分配器完整层次

```
用户/内核请求
     │
     ▼
  kmalloc / vmalloc / alloc_pages
     │
     ├── Slab/Slub        小对象（< 页大小）缓存分配器
     │       └── 从 Buddy 批量取页，切分为 object
     │
     ├── vmalloc           不连续虚拟内存映射
     │       └── 从 Buddy 取不连续物理页，建立页表
     │
     ├── CMA               连续内存分配（DMA 驱动用）
     │       └── 预留物理连续区域，运行时迁移普通页
     │
     ▼
  Buddy System（Zone 级分配器）
  ├── ZONE_DMA      (平台相关)
  ├── ZONE_DMA32    (< 4GB)
  └── ZONE_NORMAL   (普通内存)
     │
     ▼
  memblock              启动阶段早期分配器
  （sparse_init/numa_init 之前使用，mem_init 后交还 Buddy）
```

---

## 8. 三层协作关系

### 8.1 数据结构关联图

```
pg_data_t (NODE_DATA(nid))
│
├── node_zones[ZONE_DMA32]
│       ├── free_area[0..10]          ← Buddy System
│       ├── per_cpu_pageset           ← per-CPU 缓存
│       └── zone_start_pfn
│
├── node_zones[ZONE_NORMAL]
│       └── ...
│
└── node_start_pfn
        │
        ▼
    pfn_to_page(pfn) = vmemmap[pfn]   ← SPARSEMEM_VMEMMAP
        │
        ▼
    struct page                        ← 页描述符
        │
        └── page_to_nid(page)         ← 反查所属 node
```

### 8.2 分配一个页的完整路径

```
alloc_pages(gfp_mask, order)
    │
    ├─[1] NUMA 策略决策
    │      mempolicy → 选择目标 node
    │      node_zonelist → 按距离排序的 zone 候选链
    │
    ├─[2] 遍历 zonelist
    │      get_page_from_freelist()
    │      └── zone_watermark_ok()    检查水位
    │
    ├─[3] Buddy 分配
    │      rmqueue(zone, order, migratetype)
    │      └── __rmqueue()            从 free_area 取块
    │               └── expand()     拆分多余部分归还
    │
    └─[4] 返回 struct page *
           page_address(page) → 虚拟地址（通过 vmemmap 反算）
```

---

## 9. THP 与 HugePage

### 9.1 静态大页（HugePage）

通过 `hugetlbfs` 预先分配，用户通过 `mmap(MAP_HUGETLB)` 使用：

| 页大小 | 大页尺寸 | 映射级别 |
|---|---|---|
| 4KB | **2MB**（默认）| PMD（L2）|
| 4KB | **1GB** | PUD（L1）|
| 16KB | **32MB** | PMD |
| 64KB | **512MB** | PMD |

### 9.2 透明大页（THP，Transparent HugePage）

内核自动将连续普通页合并为大页，无需应用程序修改：

```
CONFIG_TRANSPARENT_HUGEPAGE=y

控制接口：
/sys/kernel/mm/transparent_hugepage/enabled
  [always]  madvise  never
```

- `always`：尽可能使用 THP
- `madvise`：仅对 `madvise(MADV_HUGEPAGE)` 的区域使用
- `never`：禁用

### 9.3 ARM64 Contiguous PTE（连续 PTE 优化）

ARM64 特有特性：将 **16 个连续 PTE** 合并为一个 `contiguous` hint，处理器 TLB 可以用一个条目覆盖整块区域：

```
普通 PTE：每个 4KB 页占一个 TLB 条目
Contiguous PTE：16 × 4KB = 64KB 共享一个 TLB 条目
                → TLB 效率提升 16x（针对该区域）
```

内核在建立页表时（`set_pte_at` 路径）自动判断是否设置 `PTE_CONT` 标志。

---

## 10. 启动初始化流程

```
start_kernel()
    │
    ├── setup_arch()
    │       ├── memblock_add()            // 注册物理内存段
    │       └── arm64_memblock_init()     // 预留特殊区域
    │
    ├── arch_numa_init()                  // [NUMA] 建立 pfn→nid 映射
    │       └── numa_init(of_numa_init)
    │               └── numa_memblks_init()
    │                       └── memblock_set_node() // memblock 打标
    │
    ├── sparse_init()                     // [SPARSEMEM] 初始化 section
    │       └── vmemmap_populate()        // [VMEMMAP] 建立页表映射
    │
    ├── zone_sizes_init()                 // [Zone] 初始化 pg_data_t/zone
    │       └── free_area_init()
    │               └── init_currently_empty_zone()
    │
    └── mem_init()                        // 释放 memblock→Buddy System
            └── memblock_free_all()       // 所有可用页加入 free_area
```

---

## 11. 关键 Kconfig 配置项

| 配置项 | 说明 |
|---|---|
| `CONFIG_NUMA` | 开启多 NUMA 节点支持 |
| `CONFIG_NUMA_EMU` | NUMA 模拟（调试用，x86 风格，ARM64 可选）|
| `CONFIG_SPARSEMEM` | 稀疏物理内存模型（ARM64 必选）|
| `CONFIG_SPARSEMEM_VMEMMAP` | vmemmap 加速，O(1) pfn↔page（ARM64 默认开启）|
| `CONFIG_MEMORY_HOTPLUG` | 内存热插拔（依赖 SPARSEMEM）|
| `CONFIG_MEMORY_HOTREMOVE` | 内存热拔除 |
| `CONFIG_TRANSPARENT_HUGEPAGE` | 透明大页支持 |
| `CONFIG_HUGETLB_PAGE` | 静态大页（HugeTLB）|
| `CONFIG_ARM64_4K_PAGES` | 4KB 页大小（默认）|
| `CONFIG_ARM64_16K_PAGES` | 16KB 页大小 |
| `CONFIG_ARM64_64K_PAGES` | 64KB 页大小 |
| `CONFIG_ARM64_VA_BITS` | 虚拟地址位数（39 / 48 / 52）|
| `CONFIG_ZONE_DMA` | 启用 ZONE_DMA（< 1GB）|
| `CONFIG_ZONE_DMA32` | 启用 ZONE_DMA32（< 4GB）|
| `CONFIG_CMA` | 连续内存分配器 |
| `CONFIG_DEBUG_PER_CPU_MAPS` | 开启 cpumask_of_node() 调试检查 |

---

## 12. 参考文件索引

| 文件 | 内容 |
|---|---|
| `drivers/base/arch_numa.c` | ARM64 NUMA 初始化主逻辑 |
| `drivers/of/of_numa.c` | Device Tree NUMA 解析 |
| `mm/sparse.c` | SPARSEMEM 核心实现 |
| `mm/sparse-vmemmap.c` | vmemmap 通用映射逻辑 |
| `arch/arm64/mm/mmu.c` | ARM64 vmemmap 页表建立 |
| `mm/page_alloc.c` | Buddy System / Zone 分配器 |
| `mm/numa_memblks.c` | NUMA memblock 桥接层 |
| `include/linux/mmzone.h` | zone / pg_data_t / free_area 定义 |
| `include/linux/mm_types.h` | struct page 定义 |
| `include/asm-generic/memory_model.h` | pfn_to_page / page_to_pfn 宏 |
