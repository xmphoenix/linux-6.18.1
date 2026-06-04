# ARM64 NUMA 初始化与内存管理数据结构分析

## 1. 两大核心数据结构

### 1.1 memblock.memory — 物理内存全局账本

`memblock` 是启动早期的物理内存管理器，`memblock.memory` 记录系统中**所有可用的物理内存范围**。

```c
// include/linux/memblock.h
struct memblock {
    struct memblock_type memory;     // 所有物理 RAM 范围
    struct memblock_type reserved;   // 已预留的范围（内核镜像、DTB、页表等）
};

struct memblock_type {
    unsigned long cnt;               // 区域数量
    struct memblock_region *regions; // 区域数组
};

struct memblock_region {
    phys_addr_t base;               // 起始物理地址
    phys_addr_t size;               // 大小
    int nid;                        // NUMA 节点 ID（初始为 NUMA_NO_NODE）
    // ...
};
```

**特点**：
- 覆盖所有物理 RAM，不关心 NUMA 归属（初始时 `nid = NUMA_NO_NODE`）
- 经过 `arm64_memblock_init()` 裁剪，只保留实际可用的范围
- 包含 reserved 子系统，跟踪已预留内存

### 1.2 numa_meminfo — NUMA 拓扑信息

`numa_meminfo` 记录**每段物理内存属于哪个 NUMA 节点**。

```c
// include/linux/numa_memblks.h
struct numa_memblk {
    u64   start;     // 物理地址起始
    u64   end;       // 物理地址结束
    int   nid;       // 所属 NUMA 节点 ID
};

struct numa_meminfo {
    int              nr_blks;                       // 块数量
    struct numa_memblk  blk[NR_NODE_MEMBLKS];       // 块数组（最多 MAX_NUMNODES×2 = 32）
};

// mm/numa_memblks.c
static struct numa_meminfo numa_meminfo __initdata_or_meminfo;
static struct numa_meminfo numa_reserved_meminfo __initdata_or_meminfo;
```

**特点**：
- 来自设备树或 ACPI 的原始 NUMA 描述
- 核心信息是"物理地址 → 节点"的映射
- 只在启动阶段使用，最终信息合并到 memblock 后使命完成

### 1.3 两者对比

| 属性 | `memblock.memory` | `numa_meminfo` |
|------|-------------------|----------------|
| **记录内容** | 物理内存范围 + 可用性 | 物理内存范围 + **节点归属** |
| **数据来源** | DTB `/memory` 节点 | DTB `/memory` 的 `numa-node-id` 属性 |
| **填充时机** | `arm64_memblock_init()` | `of_numa_parse_memory_nodes()` |
| **是否裁剪** | 是（超出线性映射、物理位宽等被裁掉） | 否（原始记录，需后续清理） |
| **生命周期** | 贯穿启动阶段，直到 buddy 接管 | NUMA 初始化完成后使命结束 |
| **初始 nid** | `NUMA_NO_NODE` (-1) | 设备树指定的 nid |

## 2. NUMA 初始化完整流程

### 2.1 调用链

```
setup_arch()
  │
  ├── arm64_memblock_init()              [第1次读 DTB /memory]
  │     ├── memblock 注册所有物理 RAM
  │     ├── 裁剪超出范围的内存
  │     ├── 预留内核镜像、DTB 等
  │     └── 结果: memblock.memory = 实际可用 RAM (nid = NUMA_NO_NODE)
  │
  ├── bootmem_init()
  │     └── arch_numa_init()
  │           └── numa_init(of_numa_init)
  │                 │
  │                 ├── ① 清场
  │                 │     nodes_clear(numa_nodes_parsed)
  │                 │     nodes_clear(node_possible_map)
  │                 │     nodes_clear(node_online_map)
  │                 │
  │                 ├── ② numa_memblks_init(of_numa_init, false)
  │                 │     │
  │                 │     ├── 清空所有状态
  │                 │     │   memset(&numa_meminfo, 0, ...)
  │                 │     │   memblock_set_node(全部, NUMA_NO_NODE)
  │                 │     │   numa_reset_distance()
  │                 │     │
  │                 │     ├── of_numa_init()         [第2次读 DTB /memory]
  │                 │     │   ├── of_numa_parse_cpu_nodes()
  │                 │     │   │   读取每个 CPU 的 numa-node-id
  │                 │     │   │   → node_set(nid, numa_nodes_parsed)
  │                 │     │   │
  │                 │     │   ├── of_numa_parse_memory_nodes()
  │                 │     │   │   读取每段内存的 numa-node-id
  │                 │     │   │   → numa_add_memblk(nid, start, end)
  │                 │     │   │   → 存入 numa_meminfo
  │                 │     │   │
  │                 │     │   └── of_numa_parse_distance_map()
  │                 │     │       读取节点间距离矩阵
  │                 │     │       → numa_set_distance(from, to, dist)
  │                 │     │       → 存入 numa_distance[] 一维数组
  │                 │     │
  │                 │     ├── numa_cleanup_meminfo()
  │                 │     │   对齐 numa_meminfo 与 memblock 的差异
  │                 │     │   裁剪 / 合并 / 冲突检测
  │                 │     │
  │                 │     └── numa_register_meminfo()
  │                 │         ★ 将 numa_meminfo 的 nid 写入 memblock ★
  │                 │         memblock_set_node(start, size, &memblock.memory, nid)
  │                 │         → memblock.memory.regions[].nid 不再是 NUMA_NO_NODE
  │                 │
  │                 ├── ③ numa_register_nodes()
  │                 │     for_each_node_mask(nid, numa_nodes_parsed):
  │                 │       setup_node_data(nid)    → 分配 pgdat
  │                 │       node_set_online(nid)    → 标记节点在线
  │                 │
  │                 └── ④ setup_node_to_cpumask_map()
  │                       建立 CPU → NUMA 节点的映射表
  │
  └── paging_init()
        └── map_mem() → 建立线性映射（使用 memblock 中的 nid 信息）
```

### 2.2 数据流转过程

```
DTB (设备树二进制文件)
  │
  │     /memory@80000000 {
  │         reg = <0x8000_0000, 2GB>;
  │         numa-node-id = <0>;
  │     };
  │     /memory@100000000 {
  │         reg = <0x1_0000_0000, 2GB>;
  │         numa-node-id = <1>;
  │     };
  │
  ╔═══════════════════════════════════════════════════════════╗
  ║ 第1次读取: arm64_memblock_init()                         ║
  ╚═══════════════════════════════════════════════════════════╝
  │
  ▼
  memblock.memory (裁剪后):
  ┌──────────────────────────────────────────────────┐
  │ region[0]: base=0x8000_0000 size=2GB nid=-1      │
  │ region[1]: base=0x1_0000_0000 size=2GB nid=-1    │  ← nid 未知
  └──────────────────────────────────────────────────┘
  │
  ╔═══════════════════════════════════════════════════════════╗
  ║ 第2次读取: of_numa_parse_memory_nodes()                  ║
  ╚═══════════════════════════════════════════════════════════╝
  │
  ▼
  numa_meminfo (原始):
  ┌──────────────────────────────────────────────────┐
  │ blk[0]: start=0x8000_0000 end=0x1_0000_0000 nid=0│
  │ blk[1]: start=0x1_0000_0000 end=0x1_8000_0000 nid=1│
  └──────────────────────────────────────────────────┘
  │
  ╔═══════════════════════════════════════════════════════════╗
  ║ 清理: numa_cleanup_meminfo()                             ║
  ╚═══════════════════════════════════════════════════════════╝
  │  裁剪使 numa_meminfo 与 memblock.memory 对齐
  │  合并同一节点的相邻块
  │  检测不同节点的重叠冲突
  │
  ▼
  numa_meminfo (清理后):
  ┌──────────────────────────────────────────────────┐
  │ blk[0]: start=0x8000_0000 end=0x1_0000_0000 nid=0│  ← 与 memblock 一致
  │ blk[1]: start=0x1_0000_0000 end=0x1_8000_0000 nid=1│
  └──────────────────────────────────────────────────┘
  │
  ╔═══════════════════════════════════════════════════════════╗
  ║ 合并: numa_register_meminfo()                            ║
  ╚═══════════════════════════════════════════════════════════╝
  │  memblock_set_node(start, size, &memblock.memory, nid)
  │
  ▼
  memblock.memory (最终):
  ┌──────────────────────────────────────────────────┐
  │ region[0]: base=0x8000_0000 size=2GB nid=0       │  ← nid 已填入
  │ region[1]: base=0x1_0000_0000 size=2GB nid=1     │  ← nid 已填入
  └──────────────────────────────────────────────────┘

  此后 memblock 同时知道"有哪些物理内存"和"每段属于哪个节点"
```

## 3. 辅助数据结构

### 3.1 NUMA 节点位图

```c
nodemask_t numa_nodes_parsed;     // 解析阶段发现的节点
nodemask_t node_possible_map;     // 可能存在的节点 (= node_states[N_POSSIBLE])
nodemask_t node_online_map;       // 当前在线的节点 (= node_states[N_ONLINE])
```

展开后都是 `struct { unsigned long bits[1]; }`（16 位有效，对应最多 16 个节点）。

### 3.2 NUMA 距离矩阵

```c
int numa_distance_cnt;            // 节点数 N
static u8 *numa_distance;        // N×N 一维数组，模拟二维距离矩阵
```

访问方式: `numa_distance[from * cnt + to]`

```
距离矩阵示例 (2 个节点):
          to
       node0  node1
from  [  10     20  ]     10 = LOCAL_DISTANCE (本地)
      [  20     10  ]     20 = REMOTE_DISTANCE (远端)
```

由 `of_numa_parse_distance_map()` 从设备树的 `distance-map` 节点读取。

### 3.3 pgdat (per-node 数据)

```c
// NUMA 注册阶段，为每个在线节点分配:
setup_node_data(nid, start_pfn, end_pfn)
```

`pg_data_t` (即 `struct pglist_data`) 是每个 NUMA 节点的核心管理结构，包含该节点的 zone 信息、页框范围、伙伴系统的 free_area 等。

## 4. DTB 中的 NUMA 描述

```dts
/* CPU 节点归属 */
cpus {
    cpu@0 { numa-node-id = <0>; };
    cpu@1 { numa-node-id = <0>; };
    cpu@2 { numa-node-id = <1>; };
    cpu@3 { numa-node-id = <1>; };
};

/* 内存节点归属 */
memory@80000000 {
    device_type = "memory";
    reg = <0x0 0x80000000 0x0 0x40000000>;   /* 1GB */
    numa-node-id = <0>;
};

memory@140000000 {
    device_type = "memory";
    reg = <0x1 0x40000000 0x0 0x40000000>;   /* 1GB */
    numa-node-id = <1>;
};

/* 节点间距离 */
distance-map {
    compatible = "numa-distance-map-v1";
    distance-matrix = <0 0 10>,   /* node0 → node0: 10 */
                      <0 1 20>,   /* node0 → node1: 20 */
                      <1 0 20>,   /* node1 → node0: 20 */
                      <1 1 10>;   /* node1 → node1: 10 */
};
```

三个解析函数分别读取这三部分:

| DTB 内容 | 解析函数 | 存储位置 |
|---------|---------|---------|
| CPU 的 `numa-node-id` | `of_numa_parse_cpu_nodes()` | `numa_nodes_parsed` 位图 |
| 内存的 `numa-node-id` + `reg` | `of_numa_parse_memory_nodes()` | `numa_meminfo` |
| `distance-matrix` | `of_numa_parse_distance_map()` | `numa_distance[]` 数组 |

## 5. memblock 中的 nid 字段详解

### 5.1 nid 存储位置

`nid` 存储在 `struct memblock_region` 中，且被 `#ifdef CONFIG_NUMA` 条件编译包裹：

```c
// include/linux/memblock.h
struct memblock_region {
    phys_addr_t base;
    phys_addr_t size;
    enum memblock_flags flags;
#ifdef CONFIG_NUMA
    int nid;                    // 仅在 CONFIG_NUMA=y 时存在
#endif
};
```

访问方式也分两种情况：

```c
#ifdef CONFIG_NUMA
// 有 NUMA 时：读写实际字段
static inline void memblock_set_region_node(struct memblock_region *r, int nid)
{ r->nid = nid; }

static inline int memblock_get_region_node(const struct memblock_region *r)
{ return r->nid; }

#else
// 无 NUMA 时：写操作为空，读操作永远返回 0
static inline void memblock_set_region_node(struct memblock_region *r, int nid)
{ }

static inline int memblock_get_region_node(const struct memblock_region *r)
{ return 0; }      // 所有内存都属于 node 0
#endif
```

### 5.2 为什么要把 nid 写入 memblock？

NUMA 初始化完成后、buddy 接管之前，memblock 仍在大量使用，且**需要感知 NUMA 节点**：

```
NUMA 初始化完成 (nid 已写入 memblock)
  │
  │  后续启动步骤仍需要 memblock + nid:
  │
  ├── setup_node_data(nid)
  │     为每个节点分配 pgdat → memblock_alloc_node(size, nid)
  │     需要从对应节点分配内存，保证 pgdat 在本地内存中
  │
  ├── zone 初始化 / memmap_init_range()
  │     初始化 struct page 数组，需要知道每个页框属于哪个 node
  │     → memblock_search_pfn_nid(pfn) → 读取 region->nid
  │
  ├── sparse_init()
  │     分配 section 和 struct page 数组，按节点就近分配
  │
  ├── memmap_init_reserved_pages()
  │     设置 reserved 页的 struct page 信息，通过 memblock 查 nid
  │
  └── memblock_free_all()
        将空闲页释放给 buddy 分配器
        每个页的 struct page 需要设置正确的 nid
        → 此后 memblock 退役
```

如果不把 nid 写进 memblock，这些步骤就不知道该从哪个节点分配内存，会导致**跨节点分配**，在 NUMA 系统上造成性能下降。

### 5.3 为什么不在 memblock 初始化时就确定 nid？

这是一个自然的疑问——DTB 的 `/memory` 节点既有 `reg`（地址范围）又有 `numa-node-id`，为什么不一次性都读取？

**原因 1：memblock 初始化使用通用代码**

`arm64_memblock_init()` 底层调用的是架构无关的通用函数（如 `early_init_dt_add_memory_arch()`），被 ARM、ARM64、RISC-V 等所有架构共用。它只负责"发现物理内存"，不关心 NUMA。在通用代码中加入 NUMA 逻辑会破坏代码的清晰分层。

**原因 2：memblock 初始化后会大量修改 region**

```
arm64_memblock_init():
  ├── 扫描 DTB，添加所有 memory 区域         ← 如果此时就设了 nid
  ├── memblock_remove(): 裁掉超出物理位宽的   ← region 被拆分，nid 怎么继承？
  ├── memblock_remove(): 裁掉超出线性映射的   ← region 被删除，nid 丢失？
  ├── memblock_reserve(): 预留内核镜像        ← region 再次拆分
  └── memory_limit 处理                       ← region 被缩小
```

每次 `memblock_remove()` 或 `memblock_reserve()` 都可能拆分、合并、删除 region。如果一开始就设了 nid，每次拆分都要处理 nid 的继承，逻辑复杂且容易出错。等所有修改完成后，再统一用 `memblock_set_node()` 一次性设置，简单可靠。

**原因 3：NUMA 初始化自身需要 memblock 已就绪**

NUMA 初始化过程中需要分配内存（如距离矩阵 `numa_distance`），这依赖 memblock 已经可用：

```c
// numa_alloc_distance() 中:
numa_distance = memblock_alloc(size, PAGE_SIZE);   // 需要 memblock 已初始化
```

如果把 NUMA 和 memblock 初始化混在一起，就会产生循环依赖。

**原因 4：NUMA 初始化有重试机制**

```c
arch_numa_init()
  ├── numa_init(arch_acpi_numa_init)   // 尝试 ACPI
  ├── numa_init(of_numa_init)          // 尝试设备树
  └── numa_init(dummy_numa_init)       // 兜底: 单节点
```

每次重试都会清空并重置所有 NUMA 状态。如果 nid 在 memblock 初始化时就固定了，这种灵活的重试就无法实现。

**总结——职责分离的设计原则：**

```
阶段 1: arm64_memblock_init()           阶段 2: arch_numa_init()
─────────────────────────               ────────────────────────
职责: 发现物理内存                       职责: 确定 NUMA 拓扑
问题: "系统有哪些 RAM？"                问题: "每段 RAM 属于哪个节点？"
数据: base, size                        数据: nid
代码: 通用，所有架构共享                 代码: NUMA 专用
依赖: 无                                依赖: memblock 已就绪
修改: 大量裁剪/拆分 region              修改: 只设置 nid，不动 base/size
```

先把路修好（memblock 初始化），再给路编号（设置 nid）。

## 6. 嵌入式设备与 NUMA

### 6.1 大多数嵌入式设备只有一个 NUMA 节点

大多数嵌入式 ARM64 设备（开发板、手机、路由器等）的特征：

- 单个 SoC，单个内存控制器
- 所有 CPU 核心访问所有内存的延迟相同
- 设备树中没有 `numa-node-id` 属性
- 不存在 NUMA 问题

这种情况下 NUMA 初始化走的是回退路径：

```
arch_numa_init()
  ├── numa_init(of_numa_init)     → 失败 (DTB 中没有 numa-node-id)
  └── numa_init(dummy_numa_init)  → 成功: 所有内存归入 node 0
```

### 6.2 没有 CONFIG_NUMA 时的零开销

很多嵌入式内核会关闭 `CONFIG_NUMA`，此时：

- `struct memblock_region` 中的 `nid` 字段不存在（被 `#ifdef` 排除）
- `memblock_get_region_node()` 直接返回 0（编译器优化为常量）
- 整个 NUMA 初始化代码不参与编译
- 没有任何运行时开销

### 6.3 需要多节点 NUMA 的场景

真正的多节点 NUMA 只存在于服务器级 ARM64 平台：

| 平台 | NUMA 节点数 | 特征 |
|------|-----------|------|
| 华为鲲鹏 920 (双路) | 2~4 | 多 socket，各自连接独立 DDR |
| Ampere Altra (双路) | 2 | 双 socket，跨 socket 访问延迟更高 |
| Marvell ThunderX2 | 2 | 双 socket 服务器 |
| 普通嵌入式 SoC | 1 | 单内存控制器，无 NUMA |

## 7. 非 NUMA 系统的回退

如果设备树没有 `numa-node-id` 属性（大多数嵌入式 ARM64 平台），`of_numa_init()` 解析失败，最终回退到:

```c
// drivers/base/arch_numa.c
numa_init(dummy_numa_init);
```

```c
// drivers/base/arch_numa.c:283
static int __init dummy_numa_init(void)
{
    node_set(0, numa_nodes_parsed);    // 只有 node 0
    ...
}
```

所有 CPU 和所有内存都归入 node 0，等价于非 NUMA 系统。

## 8. 完整生命周期总结

```
┌────────────────────────────────────────────────────────────────┐
│                     启动早期                                   │
│                                                                │
│  arm64_memblock_init()                                         │
│    memblock.memory = [所有物理 RAM] (nid = -1, 未知归属)       │
│    memblock.reserved = [内核镜像, DTB, 页表...]               │
│                                                                │
├────────────────────────────────────────────────────────────────┤
│                     NUMA 初始化                                │
│                                                                │
│  of_numa_init()                                                │
│    numa_nodes_parsed = {哪些节点存在}                          │
│    numa_meminfo = {物理地址 → 节点映射}                       │
│    numa_distance = {节点间距离矩阵}                           │
│                                                                │
│  numa_cleanup_meminfo()                                        │
│    numa_meminfo 与 memblock 对齐                               │
│                                                                │
│  numa_register_meminfo()                                       │
│    ★ numa_meminfo 的 nid 写入 memblock.memory ★              │
│    memblock.memory.regions[].nid = 0, 1, ...                  │
│                                                                │
│  numa_register_nodes()                                         │
│    每个节点: setup_node_data() → 创建 pgdat                   │
│    node_set_online()                                           │
│                                                                │
├────────────────────────────────────────────────────────────────┤
│                     后续阶段                                   │
│                                                                │
│  paging_init() → map_mem()                                     │
│    建立线性映射（此时 memblock 已包含 nid 信息）               │
│                                                                │
│  memblock_free_all()                                           │
│    遍历 memblock，将空闲页释放给 buddy 分配器                  │
│    每个页的 struct page 记录所属 zone 和 node                  │
│                                                                │
│  此后: buddy + struct page 接管内存管理                        │
│    memblock 退役，numa_meminfo 退役                            │
│    所有信息已固化到 pgdat / zone / struct page 中              │
└────────────────────────────────────────────────────────────────┘
```
