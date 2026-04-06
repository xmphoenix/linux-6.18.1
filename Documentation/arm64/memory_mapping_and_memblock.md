# ARM64 内核内存映射与 memblock 初始化

> 基于 Linux 6.18.1，ARM64 架构  
> 配置：VA_BITS=52, CONFIG_PGTABLE_LEVELS=5, 4KB pages, 无 KASAN

---

## 目录

1. [arm64_memblock_init() 函数分析](#1-arm64_memblock_init-函数分析)
2. [linear_region_size 计算推导](#2-linear_region_size-计算推导)
3. [线性映射（Linear Mapping）详解](#3-线性映射linear-mapping详解)
4. [ARM64 内核地址空间的 7 种映射](#4-arm64-内核地址空间的-7-种映射)
5. [线性映射与其他映射的物理页重叠关系](#5-线性映射与其他映射的物理页重叠关系)
6. [同一物理页的多虚拟地址映射原理](#6-同一物理页的多虚拟地址映射原理)
7. [内核镜像映射 vs 线性映射](#7-内核镜像映射-vs-线性映射)
8. [线性映射的权限设计](#8-线性映射的权限设计)
9. [线性映射的使用场景](#9-线性映射的使用场景)
10. [线性映射的初始化流程](#10-线性映射的初始化流程)
11. [VA_BITS_MIN 设计原理](#11-va_bits_min-设计原理)
12. [PAGE_OFFSET、KIMAGE_VADDR 与链接地址的关系](#12-page_offsetkimage_vaddr-与链接地址的关系)

---

## 1. arm64_memblock_init() 函数分析

**文件**：`arch/arm64/mm/init.c`

### 1.1 核心目的

在 memblock 已经从 DTS 获得原始物理内存信息之后，对其进行**裁剪、调整和保留**，使之与内核的线性映射区（linear mapping）完美匹配，并为后续 `paging_init()` 建立最终页表做好准备。

### 1.2 执行步骤

```
memblock（来自DTS的原始物理内存）
    │
    ├─ ① 计算 linear_region_size
    ├─ ② KVM nVHE 特殊处理（截断到 51-bit）
    ├─ ③ 移除 > PHYS_MASK_SHIFT 的物理内存
    ├─ ④ 计算 memstart_addr（1GB 对齐）
    ├─ ⑤ 裁剪超出线性映射区的部分（上裁/下裁）
    ├─ ⑥ 52→48 bit VA 补偿
    ├─ ⑦ 应用 mem= 命令行限制
    ├─ ⑧ 处理 initrd
    ├─ ⑨ 保留内核镜像 (_text ~ _end)
    └─ ⑩ 扫描 DTS reserved-memory
    │
    ▼
memblock（精确匹配线性映射区，所有保留区已标记）
    → 交给 paging_init() → map_mem() 建立最终线性映射页表
```

### 1.3 各步骤详解

#### ① 计算线性映射区大小

```c
s64 linear_region_size = PAGE_END - _PAGE_OFFSET(vabits_actual);
```

- `PAGE_END = _PAGE_END(VA_BITS_MIN) = -(1UL << 47) = 0xFFFF_8000_0000_0000`
- `_PAGE_OFFSET(52) = -(1UL << 52) = 0xFFF0_0000_0000_0000`
- **结果**：`0xF_8000_0000_0000 = 3968 TB ≈ 3.875 PB`

#### ④ 计算 memstart_addr

```c
memstart_addr = round_down(memblock_start_of_DRAM(), ARM64_MEMSTART_ALIGN);
```

- `ARM64_MEMSTART_ALIGN = 1 << PUD_SHIFT = 1GB`（4KB pages 配置）
- `memstart_addr` 是物理内存到虚拟地址映射的基准点：
  - `virt = phys - memstart_addr + PAGE_OFFSET`

#### ⑥ 52→48 bit VA 补偿

```c
if (IS_ENABLED(CONFIG_ARM64_VA_BITS_52) && (vabits_actual != 52))
    memstart_addr -= _PAGE_OFFSET(vabits_actual) - _PAGE_OFFSET(52);
```

编译时 PAGE_OFFSET 按 52-bit 计算，但运行时只有 48-bit 可用时，需要调整 memstart_addr 使物理内存映射到 48-bit 可达的虚拟地址空间中。

---

## 2. linear_region_size 计算推导

### 2.1 宏定义展开

```c
#define VA_BITS              52
#define VA_BITS_MIN          48          // VA_BITS > 48 且非 16K pages
#define _PAGE_OFFSET(va)     (-(UL(1) << (va)))
#define _PAGE_END(va)        (-(UL(1) << ((va) - 1)))
#define PAGE_OFFSET          _PAGE_OFFSET(52)   // 编译时常量
#define PAGE_END             _PAGE_END(48)       // 无 KASAN 时
```

### 2.2 数值计算

| 值 | 十六进制 | 说明 |
|:---|:---------|:-----|
| `PAGE_END` | `0xFFFF_8000_0000_0000` | $-2^{47}$，线性映射区上界 |
| `_PAGE_OFFSET(52)` | `0xFFF0_0000_0000_0000` | $-2^{52}$，线性映射区下界 |
| **差值** | **`0x000F_8000_0000_0000`** | $2^{52} - 2^{47} = 31 \times 2^{47}$ |

$$\text{linear\_region\_size} = 31 \times 128\text{TB} = 3968\text{TB} \approx 3.875\text{PB}$$

### 2.3 为什么线性映射区远大于实际物理 RAM

1. **适应所有物理内存布局**：不同硬件的 DRAM 起始地址不同，物理内存可能不连续（有空洞），线性映射必须覆盖整个物理地址跨度
2. **虚拟地址空间是"免费"的**：大的只是地址编号范围，不消耗物理资源。只有有 RAM 的区域才建页表
3. **留好余量**：避免物理内存超出、热插拔等场景无法映射

---

## 3. 线性映射（Linear Mapping）详解

### 3.1 定义

线性映射是一种**物理地址到虚拟地址的固定偏移映射**：

$$\text{virt} = \text{phys} - \text{memstart\_addr} + \text{PAGE\_OFFSET}$$
$$\text{phys} = \text{virt} - \text{PAGE\_OFFSET} + \text{memstart\_addr}$$

"线性"的含义：物理地址连续 → 虚拟地址连续，偏移量固定不变。这就是 `__pa()` / `__va()` 宏的本质。

### 3.2 核心作用

- 让内核能高效访问**所有物理内存**
- `phys_to_virt` / `virt_to_phys` 转换只需加减一个常量，零开销
- 为 buddy allocator、slab allocator 等内存管理子系统提供基础

---

## 4. ARM64 内核地址空间的 7 种映射

### 4.1 虚拟地址布局

```
0xFFF0_0000_0000_0000  ┬─ PAGE_OFFSET          ─┐
                       │                         │ ① 线性映射区 (Linear Map)
                       │  ~3968 TB               │    phys ↔ virt 固定偏移
0xFFFF_8000_0000_0000  ┴─ PAGE_END              ─┘
                       ┬─ MODULES_VADDR          ─┐
                       │  2 GB                    │ ② 模块映射区 (Modules)
0xFFFF_8000_8000_0000  ┴─ MODULES_END            ─┘
                       ┬─ KIMAGE_VADDR           ─┐
                       │                          │ ③ 内核镜像映射 (Kernel Image)
                       │  _text ~ _end            │
                       ┴                         ─┘
                          ...
                       ┬─ VMALLOC_START          ─┐
                       │                          │ ④ vmalloc 区
                       ┴─ VMALLOC_END            ─┘
                       ┬─ VMEMMAP_START          ─┐
                       │                          │ ⑤ vmemmap 区 (struct page[])
0xFFFF_FFFF_C000_0000  ┴─ VMEMMAP_END           ─┘
                       ┬─ PCI_IO_START           ─┐
                       │  16 MB                   │ ⑥ PCI I/O 区
                       ┴─ PCI_IO_END             ─┘
0xFFFF_FFFF_FF80_0000  ┬─ FIXADDR_START         ─┐
                       │  ~6 MB                   │ ⑦ Fixmap 区
                       ┴─ FIXADDR_TOP            ─┘
0xFFFF_FFFF_FFFF_FFFF     地址空间顶端
```

### 4.2 各映射特点对比

| # | 映射区域 | 映射方式 | 特点 | 用途 |
|:--|:---------|:---------|:-----|:-----|
| ① | **线性映射** | phys + 固定偏移 | 覆盖全部物理 RAM，一对一 | 通用内存访问、page allocator、slab |
| ② | **模块映射** | vmalloc 式按需映射 | 距内核 image ±128M/2G | 内核模块 .ko 加载 |
| ③ | **内核镜像** | 独立页表映射 | text=RX, rodata=RO, data=RW | 内核代码和数据（CPU 取指执行） |
| ④ | **vmalloc** | 按需分配物理页，逐页映射 | 物理不连续，虚拟连续 | `vmalloc()`, `ioremap()`, `vmap()` |
| ⑤ | **vmemmap** | 稀疏映射 struct page 数组 | 只映射有物理 RAM 的部分 | struct page 元数据 |
| ⑥ | **PCI I/O** | 映射 PCI I/O 端口空间 | 16MB | PCI 设备 I/O 访问 |
| ⑦ | **fixmap** | 编译时确定虚拟地址，运行时改物理映射 | 临时/永久槽位 | early ioremap、FDT、早期页表操作 |

---

## 5. 线性映射与其他映射的物理页重叠关系

### 5.1 总结表

| 映射区域 | 映射的物理页是 RAM？ | 线性映射也覆盖该物理页？ |
|:---------|:--------------------|:------------------------|
| ③ 内核镜像 | 是 RAM | **是** — `map_mem()` 明确建立线性别名 |
| ② 模块映射 | 是 RAM（buddy 分配） | **是** — 物理页来自 buddy allocator |
| ④ vmalloc（分配内存） | 是 RAM（buddy 分配） | **是** — `vmalloc()` 从 buddy 拿物理页 |
| ④ ioremap（映射设备） | **不是 RAM，是设备寄存器** | **否** — 线性映射不覆盖 |
| ⑤ vmemmap | 是 RAM（memblock 分配） | **是** — struct page 数组本身是 RAM |
| ⑥ PCI I/O | **不是 RAM，是设备 I/O 空间** | **否** |
| ⑦ fixmap（映射 FDT/页表） | 是 RAM | 早期**否**（线性映射未建），后期**是** |
| ⑦ fixmap（early ioremap 设备） | 不是 RAM | **否** |

### 5.2 核心规则

**线性映射只覆盖 RAM（memblock.memory 中的物理地址），不覆盖设备 MMIO 空间。**

- 凡是从 buddy/memblock **分配物理页**的映射 → 该物理页必然也在线性映射中 → 同一物理页有两个虚拟地址
- 凡是映射**设备寄存器/I/O 端口**的 → 那些物理地址不在 RAM 中 → 线性映射不覆盖

---

## 6. 同一物理页的多虚拟地址映射原理

### 6.1 页表树的不同路径

TTBR1 只有一个，指向同一棵页表树（`swapper_pg_dir`），但树的不同分支可以指向同一个物理页：

```
TTBR1 → swapper_pg_dir
              │
     ┌────────┴────────┐
     │                  │
  路径 A               路径 B
  (KIMAGE 地址)        (线性映射地址)
  0xFFFF_8000_8xxx      0xFFF0_xxxx_xxxx
     │                  │
  PGD[15]             PGD[0]          ← 不同的 PGD 索引
     │                  │
  P4D → PUD → PMD → PTE   P4D → PUD → PMD → PTE
     │                  │
     ↓                  ↓
  phys=0x4020_0000     phys=0x4020_0000  ← 同一物理页
  权限: RX (可执行)     权限: RO+NX (不可执行)
```

### 6.2 原理

MMU 地址翻译：`虚拟地址 → 提取各级索引 → PGD[i] → P4D[j] → PUD[k] → PMD[l] → PTE[m] → 物理页`

两个不同虚拟地址的索引序列 `(i,j,k,l,m)` 不同，走页表树的不同分支。但两个分支末端的 PTE 里填的**物理页帧号可以相同**，只是**属性位不同**。

这是所有使用页表的架构（ARM64、x86、RISC-V）都天然支持的能力。

---

## 7. 内核镜像映射 vs 线性映射

### 7.1 本质上都是固定偏移映射

| | 线性映射 | 内核镜像映射 |
|:--|:---------|:------------|
| 公式 | `virt = phys - memstart_addr + PAGE_OFFSET` | `virt = phys - kimage_phys + KIMAGE_VADDR` |
| 偏移量 | `PAGE_OFFSET - memstart_addr`（固定常量） | `KIMAGE_VADDR - kimage_phys`（固定常量） |

硬件层面没有任何区别。区分它们是**软件设计选择**。

### 7.2 为什么要区分

1. **权限粒度不同**：
   - 线性映射：统一 RW+NX（1GB/2MB block mapping，追求效率）
   - 内核镜像：.text=RX, .rodata=RO, .data=RW（4KB page mapping，追求安全）

2. **KASLR 安全**：
   - 内核镜像地址被 KASLR 随机化，攻击者无法预测
   - 线性映射 PAGE_OFFSET 是编译时固定的

3. **生命周期不同**：
   - 内核镜像映射在 head.S 中最早建立（CPU 要取指执行）
   - 线性映射在 `paging_init()` 时才建立

---

## 8. 线性映射的权限设计

### 8.1 `map_mem()` 中的权限分配

```c
// 第一步：标记内核镜像为 NOMAP（先跳过）
memblock_mark_nomap(kernel_start, kernel_end - kernel_start);

// 第二步：映射所有普通物理 RAM
for_each_mem_range(i, &start, &end) {
    __map_memblock(pgdp, start, end,
                   pgprot_tagged(PAGE_KERNEL),   // RW + NX
                   flags);
}

// 第三步：单独映射内核镜像的线性别名
__map_memblock(pgdp, kernel_start, kernel_end,
               PAGE_KERNEL, NO_CONT_MAPPINGS);   // RW + NX（初始）
memblock_clear_nomap(kernel_start, kernel_end - kernel_start);
```

### 8.2 权限演变

| 线性映射中的区域 | 占比 | 初始权限 | 最终权限 |
|:----------------|:-----|:---------|:---------|
| 普通物理 RAM（绝大部分） | >99% | RW + NX | **RW + NX**（不变） |
| 内核 .text/.rodata 的线性别名 | 极小 | RW + NX | **RO + NX**（`mark_linear_text_alias_ro()` 修改） |

**关键点**：
- 整个线性映射区统一 **NX（不可执行）**，防止通过线性映射执行代码
- 绝大部分 RAM 是 **RW（可读可写）**，否则 `kmalloc`、page cache 等无法写入数据
- 只有内核代码的线性别名被特殊改为 RO，防止意外修改

---

## 9. 线性映射的使用场景

### 9.1 场景汇总

| 类别 | 具体场景 | 说明 |
|:-----|:---------|:-----|
| **内存分配** | `kmalloc()`, `alloc_pages()`, slab 缓存 | 返回值就是线性映射地址 |
| **地址转换** | `__va()`, `__pa()`, `page_address()` | 只对线性映射地址有效 |
| **内核数据结构** | 页表页、task_struct、inode、dentry | 全部分配在线性映射区 |
| **文件 I/O** | page cache、块设备 buffer | 文件数据缓存在线性映射区 |
| **网络** | skb 数据缓冲区 | `skb->data` 指向线性映射区 |
| **DMA** | CPU 侧数据访问 | 通过线性映射地址填数据，`__pa()` 转物理地址给设备 |
| **系统状态保存** | hibernate、kexec、crash dump | 通过线性映射遍历所有物理页 |
| **页表管理** | `__va(phys)` 访问页表页内容 | 分配页表页后用线性映射地址清零和填写 |

**一句话**：凡是内核代码中直接操作 RAM 内容的地方，用的都是线性映射地址。

---

## 10. 线性映射的初始化流程

### 10.1 调用链

```
setup_arch()                              // arch/arm64/kernel/setup.c
  └→ paging_init()                        // arch/arm64/mm/mmu.c
       └→ map_mem(swapper_pg_dir)         // arch/arm64/mm/mmu.c:1071
            │
            │  // 标记内核镜像为 NOMAP（跳过它）
            ├→ memblock_mark_nomap(kernel_start, ...)
            │
            │  // 遍历所有 memblock.memory 区域，建立线性映射
            ├→ for_each_mem_range() {
            │      __map_memblock(pgdp, start, end, PAGE_KERNEL, flags);
            │  }
            │
            │  // 单独为内核镜像建立线性映射别名
            ├→ __map_memblock(pgdp, kernel_start, kernel_end, ...)
            │
            └→ memblock_clear_nomap(...)
```

### 10.2 页表切换时间线

```
时间线:
head.S          → 建立 init_pg_dir（临时页表，只映射内核镜像）
                → 开启 MMU，CPU 使用 init_pg_dir 运行

start_kernel()  → setup_arch()
                   → arm64_memblock_init()  ← 整理 memblock
                   → paging_init()
                      → map_mem(swapper_pg_dir)  ← 在 swapper_pg_dir 中建立线性映射
                      → 切换到 swapper_pg_dir    ← 从此线性映射可用

此后            → buddy allocator、slab 等子系统初始化
                → 全部基于线性映射工作
```

---

## 11. VA_BITS_MIN 设计原理

### 11.1 定义

```c
#if VA_BITS > 48
#define VA_BITS_MIN    (48)    // 4K/64K pages
#else
#define VA_BITS_MIN    (VA_BITS)
#endif
```

### 11.2 为什么 VA_BITS > 48 时 VA_BITS_MIN = 48

内核编译时配置 `VA_BITS=52`，但运行时硬件可能不支持 52-bit VA：

| 硬件 | vabits_actual | 虚拟地址空间 |
|:-----|:-------------|:------------|
| 支持 LVA | 52 | $2^{52}$ |
| **不支持** LVA | **48** | $2^{48}$ |

**解决方案**：把所有非线性映射的地址布局（modules、vmalloc、vmemmap、fixmap、内核镜像）都放在 48-bit VA 能覆盖的范围内：

```
┌─ 0xFFFF_FFFF_FFFF_FFFF ─── 地址顶端
│  fixmap, PCI_IO, vmemmap, vmalloc, KIMAGE, modules
│  ← 全部在 48-bit 可达范围（VA_BITS_MIN=48 决定）
├─ 0xFFFF_8000_0000_0000 ─── _PAGE_END(48) = PAGE_END
│
│  ← 52-bit 比 48-bit 多出来的空间，全部给线性映射向低地址扩展
│
├─ 0xFFF0_0000_0000_0000 ─── _PAGE_OFFSET(52)
│  ← 只有 52-bit 硬件才能用到这里
│
└─ 0xFFFF_0000_0000_0000 ─── _PAGE_OFFSET(48)
```

**52-bit 的好处仅仅是线性映射区变大**。其他映射区域地址完全不受影响，保证二进制兼容性。

---

## 12. PAGE_OFFSET、KIMAGE_VADDR 与链接地址的关系

### 12.1 宏定义链

```c
#define PAGE_OFFSET       _PAGE_OFFSET(52)           = 0xFFF0_0000_0000_0000
#define PAGE_END          _PAGE_END(48)               = 0xFFFF_8000_0000_0000
#define MODULES_VADDR     _PAGE_END(48)               = 0xFFFF_8000_0000_0000
#define MODULES_VSIZE     SZ_2G                       = 0x0000_0000_8000_0000
#define MODULES_END       MODULES_VADDR + SZ_2G       = 0xFFFF_8000_8000_0000
#define KIMAGE_VADDR      MODULES_END                 = 0xFFFF_8000_8000_0000
```

### 12.2 链接脚本

```
// arch/arm64/kernel/vmlinux.lds.S
. = KIMAGE_VADDR;    // 链接起始地址 = 0xFFFF_8000_8000_0000
_text = .;            // 内核代码从这里开始
```

### 12.3 kimage_voffset

在 `__primary_switched`（head.S）中计算：

```asm
adrp    x4, _text           // _text 的运行时虚拟地址
sub     x4, x4, x0          // 减去 _text 的物理地址
str_l   x4, kimage_voffset   // 保存偏移量
```

`kimage_voffset = _text(virt) - _text(phys)` 是内核镜像映射的虚拟-物理偏移量。

### 12.4 三者关系图

```
                              编译时确定（仅取决于 VA_BITS_MIN=48）
                              ↓
0xFFFF_8000_8000_0000 = KIMAGE_VADDR = 内核链接地址
                      ↕
                      _text 链接在这里（可被 KASLR 随机偏移）
                      ↕
                      kimage_voffset = _text(virt) - _text(phys)


                              运行时根据 vabits_actual 确定
                              ↓
0xFFF0_0000_0000_0000 = PAGE_OFFSET（52-bit 硬件）
0xFFFF_0000_0000_0000 = PAGE_OFFSET（48-bit 硬件）
                      ↕
                      线性映射区起始
                      ↕
                      __va(phys) = phys - memstart_addr + PAGE_OFFSET
```

**关键点**：`KIMAGE_VADDR` 是编译时常量，不随 `vabits_actual` 变化。而 `PAGE_OFFSET` 的语义在不同硬件上不同，这就是 `arm64_memblock_init()` 需要补偿调整 `memstart_addr` 的原因。

---

## 附录：关键配置参数

| 参数 | 值 | 说明 |
|:-----|:---|:-----|
| `CONFIG_ARM64_VA_BITS` | 52 | 虚拟地址位数 |
| `CONFIG_PGTABLE_LEVELS` | 5 | 页表级数 |
| `CONFIG_ARM64_4K_PAGES` | y | 4KB 页 |
| `VA_BITS_MIN` | 48 | 最小兼容 VA 位数 |
| `PGDIR_SHIFT` | 48 | PGD 级偏移 |
| `P4D_SHIFT` | 39 | P4D 级偏移 |
| `PUD_SHIFT` | 30 | PUD 级偏移 |
| `PMD_SHIFT` | 21 | PMD 级偏移 |
| `PAGE_SHIFT` | 12 | 页偏移 |
| `ARM64_MEMSTART_ALIGN` | 1GB | memstart_addr 对齐粒度 |

---

## 附录：关键源文件

| 文件 | 内容 |
|:-----|:-----|
| `arch/arm64/mm/init.c` | `arm64_memblock_init()`, `bootmem_init()` |
| `arch/arm64/mm/mmu.c` | `map_mem()`, `paging_init()`, `__create_pgd_mapping_locked()` |
| `arch/arm64/include/asm/memory.h` | `PAGE_OFFSET`, `PAGE_END`, `KIMAGE_VADDR` 等宏 |
| `arch/arm64/kernel/vmlinux.lds.S` | 内核链接脚本，`. = KIMAGE_VADDR` |
| `arch/arm64/kernel/head.S` | 启动汇编，`kimage_voffset` 计算 |
| `arch/arm64/kernel/setup.c` | `setup_arch()` 启动流程 |
