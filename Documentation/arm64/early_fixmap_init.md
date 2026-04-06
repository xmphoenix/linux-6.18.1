# early_fixmap_init 详解

## 1. 函数概述

`early_fixmap_init()` 定义在 `arch/arm64/mm/fixmap.c`，是 ARM64 内核启动早期用于**初始化 fixmap 区域页表骨架**的函数。它在 `setup_arch()` 最早期被调用，为 fixmap 虚拟地址区间建立从 PGD 到 PTE 的完整页表遍历路径，但**不填写最终的 PTE 映射**。

## 2. 什么是 Fixmap

Fixmap 是内核虚拟地址空间中一段**编译时固定的虚拟地址区域**（`FIXADDR_TOT_START` ~ `FIXADDR_TOP`），用于在内核启动早期（内存分配器尚未就绪时）提供临时的虚拟地址映射能力。典型用途包括：

- 映射设备树（FDT）以读取硬件配置
- 早期 I/O 映射（如串口调试输出，earlycon）
- 早期 PCI 配置空间访问
- 早期 ioremap（`FIX_BTMAP`）
- 内核页表创建时的临时映射（`FIX_PTE` ~ `FIX_PGD`）

## 3. 为什么需要这个函数

### 3.1 为什么不能动态分配页表？

`early_fixmap_init` 的调用时机在 `setup_arch()` 的最早期，此时：

- **`memblock` 尚未初始化**（设备树还没解析，物理内存布局未知）
- **buddy 分配器 / slab 更不存在**
- 内核连自己有多少物理内存都不知道——这些信息**在设备树里**，而设备树本身就要靠 fixmap 映射才能读取

这是一个**鸡生蛋的依赖链**：

```
要分配内存 → 需要知道物理内存布局 → 需要解析设备树(FDT)
要解析设备树 → 需要将 FDT 物理地址映射到虚拟地址 → 需要 fixmap
要建立 fixmap → 需要页表内存 → ???
```

所以页表必须是**编译时静态分配在 BSS 段**的 `bm_pte`/`bm_pmd`/`bm_pud`，不依赖任何运行时分配器。

### 3.2 为什么不在 head.S 里提前映射好？

`head.S` 中的汇编代码（`__create_page_tables`）确实建立了初始页表，但它只映射了：

1. **identity mapping**（物理地址 = 虚拟地址，用于开启 MMU 的过渡）
2. **内核镜像本身的线性映射**（`_text` ~ `_end`）

不提前映射 fixmap 的原因：

- **head.S 追求最小化**：汇编代码难以维护，只做开启 MMU 的最低限度工作
- **fixmap 的目标物理地址在编译时未知**：fixmap 是一套"虚拟地址固定、物理地址按需填入"的机制。head.S 阶段不知道 FDT 在哪个物理地址、UART 在哪个物理地址
- **只需建骨架即可**：`early_fixmap_init` 做的事情很轻量——只是把静态页表数组串联到内核页表结构中

### 3.3 为什么不用已有的线性映射？

MMU 开启后，内核代码通过 `swapper_pg_dir` 页表访问，但此时线性映射（`PAGE_OFFSET` 起始）也尚未完整建立。`virt_to_phys` / `lm_alias` 等转换函数的前提条件还不满足——这也是代码注释中说必须用 `__pa_symbol`（编译时计算偏移）而非 `virt_to_phys`（依赖线性映射）的原因。

## 4. 当前内核配置

| 参数 | 值 |
|------|-----|
| VA_BITS | 52 |
| PAGE_SHIFT | 12 (4KB 页) |
| CONFIG_PGTABLE_LEVELS | 5 |
| PGDIR_SHIFT | 48 (PGD 覆盖 256TB) |
| P4D_SHIFT | 39 (P4D 覆盖 512GB) |
| PUD_SHIFT | 30 (PUD 覆盖 1GB) |
| PMD_SHIFT | 21 (PMD 覆盖 2MB) |
| PTRS_PER_PGD | 16 |
| PTRS_PER_P4D / PUD / PMD / PTE | 512 |
| PTDESC_TABLE_SHIFT | 9 |

52-bit VA 地址分解：

```
┌──────────┬──────────┬──────────┬──────────┬──────────┬────────────┐
│ PGD [4]  │ P4D [9]  │ PUD [9]  │ PMD [9]  │ PTE [9]  │ Offset[12] │
│ bit51-48 │ bit47-39 │ bit38-30 │ bit29-21 │ bit20-12 │  bit11-0   │
└──────────┴──────────┴──────────┴──────────┴──────────┴────────────┘
```

## 5. 静态页表数组

```c
static pte_t bm_pte[NR_BM_PTE_TABLES][PTRS_PER_PTE] __page_aligned_bss;
static pmd_t bm_pmd[PTRS_PER_PMD] __page_aligned_bss __maybe_unused;
static pud_t bm_pud[PTRS_PER_PUD] __page_aligned_bss __maybe_unused;
```

### 5.1 为什么 bm_pte 是二维数组？

fixmap 区域可能跨越多个 PMD 条目（每个 PMD 覆盖 2MB），每个 PMD 条目需要指向**一张独立的、4KB 对齐的 PTE 页表**。

- **第一维 `NR_BM_PTE_TABLES`**：需要几张 PTE 页表（= 跨了几个 2MB 段）
- **第二维 `PTRS_PER_PTE`（= 512）**：每张 PTE 页表的条目数

使用方式：`bm_pte[i]` 选第 i 张表，`bm_pte[i][j]` 选表内第 j 个条目。

### 5.2 为什么 bm_pmd 和 bm_pud 是一维数组？

`static_assert(NR_BM_PMD_TABLES == 1)` 保证了 fixmap 区域落在同一个 1GB PUD 范围内，只需一张 PMD 表和一张 PUD 表。

### 5.3 为什么每张表必须是 512 个条目？

ARM64 MMU 硬件用虚拟地址中对应的 9 位作为索引直接跳转到页表条目：

```
硬件行为：entry = table_base[VA[对应bit段]]
```

硬件不知道哪些条目"有用"，所以必须提供完整的 512 条目（4KB）页表。未使用的条目为 0（无效），MMU 遍历到时会产生 page fault。

## 6. Fixmap 地址范围计算

### 6.1 enum fixed_addresses

当前配置启用了 CONFIG_KVM(NR_CPUS=512)、CONFIG_ACPI_APEI_GHES、CONFIG_ARM_SDE_INTERFACE、CONFIG_UNMAP_KERNEL_AT_EL0、CONFIG_RELOCATABLE：

```
FIX_HOLE                     = 0
FIX_FDT_END                  = 1
FIX_FDT                      = 514    (1 + DIV_ROUND_UP(2MB,4KB) + 1)
FIX_EARLYCON_MEM_BASE        = 515
FIX_TEXT_POKE0               = 516
FIX_VNCR_END                 = 517
FIX_VNCR                     = 1029   (517 + NR_CPUS)
FIX_APEI_GHES_IRQ            = 1030
FIX_APEI_GHES_SEA            = 1031
FIX_APEI_GHES_SDEI_NORMAL    = 1032
FIX_APEI_GHES_SDEI_CRITICAL  = 1033
FIX_ENTRY_TRAMP_TEXT4         = 1034
FIX_ENTRY_TRAMP_TEXT3         = 1035
FIX_ENTRY_TRAMP_TEXT2         = 1036
FIX_ENTRY_TRAMP_TEXT1         = 1037
__end_of_permanent_fixed_addresses = 1038

FIX_BTMAP_END                = 1038
TOTAL_FIX_BTMAPS             = 448   (64 × 7)
FIX_BTMAP_BEGIN              = 1485
FIX_PTE ~ FIX_PGD            = 1486..1490

__end_of_fixed_addresses     = 1491
```

### 6.2 地址计算

```
FIXADDR_TOP       = 0xFFFF_FFFF_FF80_0000  (-8MB)
FIXADDR_TOT_SIZE  = 1491 × 4KB = 0x5D_3000 (≈5.83MB)
FIXADDR_TOT_START = 0xFFFF_FFFF_FF22_D000
```

### 6.3 页表索引

| 级别 | FIXADDR_TOT_START | FIXADDR_TOP-1 |
|------|---|---|
| PGD [51:48] | **15** | **15** |
| P4D [47:39] | **511** | **511** |
| PUD [38:30] | **511** | **511** |
| PMD [29:21] | **505** | **507** |
| PTE [20:12] | 45 | 511 |

fixmap 跨了 **3 个 PMD 条目**（505、506、507），即 `NR_BM_PTE_TABLES = 3`。

## 7. 函数调用链与执行流程

```
early_fixmap_init()
  │  addr = FIXADDR_TOT_START, end = FIXADDR_TOP
  │  pgdp = pgd_offset_k(addr)         → PGD[15]
  │  p4dp = p4d_offset_kimg(pgdp, addr) → P4D[511]
  │
  └─ early_fixmap_init_pud(p4dp, addr, end)
       │  检查 P4D 条目是否已被内核映射占用
       │  如果 P4D 为空: __p4d_populate → 写入 __pa_symbol(bm_pud)
       │  pudp = pud_offset_kimg → PUD[511]
       │
       └─ early_fixmap_init_pmd(pudp, addr, end)
            │  如果 PUD 为空: __pud_populate → 写入 __pa_symbol(bm_pmd)
            │  pmdp = pmd_offset_kimg → PMD[505]
            │
            │  循环（addr 从 FIXADDR_TOT_START 到 FIXADDR_TOP）:
            │
            ├─ early_fixmap_init_pte(pmdp=&bm_pmd[505], addr)
            │    __pmd_populate → 写入 __pa_symbol(bm_pte[0])
            │
            ├─ early_fixmap_init_pte(pmdp=&bm_pmd[506], addr)
            │    __pmd_populate → 写入 __pa_symbol(bm_pte[1])
            │
            └─ early_fixmap_init_pte(pmdp=&bm_pmd[507], addr)
                 __pmd_populate → 写入 __pa_symbol(bm_pte[2])
```

## 8. 最终页表映射结果

### 8.1 结构图

```
swapper_pg_dir (PGD)
  │
  PGD[15] ──→ P4D 表（可能已由 head.S 建立）
                │
                P4D[511]
                │  值: __pa_symbol(bm_pud) | P4D_TYPE_TABLE | P4D_TABLE_AF
                ▼
              bm_pud[512 条目]
                │
                bm_pud[511]
                │  值: __pa_symbol(bm_pmd) | PUD_TYPE_TABLE | PUD_TABLE_AF
                ▼
              bm_pmd[512 条目]
                │
                ├─ bm_pmd[505]
                │    值: __pa_symbol(bm_pte[0]) | PMD_TYPE_TABLE | PMD_TABLE_AF
                │    覆盖: 0xFFFF_FFFF_FF20_0000 ~ 0xFFFF_FFFF_FF3F_FFFF (2MB)
                │         ▼
                │       bm_pte[0][512 条目] ← 全部为 0（空）
                │
                ├─ bm_pmd[506]
                │    值: __pa_symbol(bm_pte[1]) | PMD_TYPE_TABLE | PMD_TABLE_AF
                │    覆盖: 0xFFFF_FFFF_FF40_0000 ~ 0xFFFF_FFFF_FF5F_FFFF (2MB)
                │         ▼
                │       bm_pte[1][512 条目] ← 全部为 0（空）
                │
                └─ bm_pmd[507]
                     值: __pa_symbol(bm_pte[2]) | PMD_TYPE_TABLE | PMD_TABLE_AF
                     覆盖: 0xFFFF_FFFF_FF60_0000 ~ 0xFFFF_FFFF_FF7F_FFFF (2MB)
                          ▼
                        bm_pte[2][512 条目] ← 全部为 0（空）

其余 bm_pud[0..510]  = 0（无效）
其余 bm_pmd[0..504], bm_pmd[508..511] = 0（无效）
```

### 8.2 写入汇总

| 页表级别 | 写入位置 | 写入内容 | 标志位 |
|---------|---------|---------|-------|
| P4D | P4D 表中对应条目 | `__pa_symbol(bm_pud)` | `P4D_TYPE_TABLE \| P4D_TABLE_AF` |
| PUD | `bm_pud[511]` | `__pa_symbol(bm_pmd)` | `PUD_TYPE_TABLE \| PUD_TABLE_AF` |
| PMD | `bm_pmd[505]` | `__pa_symbol(bm_pte[0])` | `PMD_TYPE_TABLE \| PMD_TABLE_AF` |
| PMD | `bm_pmd[506]` | `__pa_symbol(bm_pte[1])` | `PMD_TYPE_TABLE \| PMD_TABLE_AF` |
| PMD | `bm_pmd[507]` | `__pa_symbol(bm_pte[2])` | `PMD_TYPE_TABLE \| PMD_TABLE_AF` |
| **PTE** | **未写入** | **全部保持 0** | — |

### 8.3 内存开销

| 静态数组 | 大小 | 有效条目 |
|---------|------|---------|
| bm_pud[512] | 4KB | 1 个（[511]） |
| bm_pmd[512] | 4KB | 3 个（[505][506][507]） |
| bm_pte[3][512] | 12KB (3×4KB) | 0 个（等待按需填入） |
| **总计** | **20KB** | |

## 9. MMU 映射变化

**执行后 MMU 的实际地址映射没有变化**——CPU 走到 PTE 级别时发现条目为 0，仍然会触发 page fault。

变化的是：从 PGD 到 PTE 的**页表遍历路径被打通了**。后续调用 `__set_fixmap` 时只需往 `bm_pte[i][j]` 写入一个 PTE 条目，就能立刻建立完整的虚拟→物理映射：

```c
// __set_fixmap 中，只需一步即可完成映射：
__set_pte(ptep, pfn_pte(phys >> PAGE_SHIFT, flags));
```

## 10. 启动时间线

```
head.S:  开启 MMU，仅映射内核镜像
  │
  ▼
start_kernel → setup_arch
  │
  ├─ early_fixmap_init()        ← 用静态 BSS 页表建 fixmap 骨架
  ├─ fixmap_remap_fdt()         ← 通过 fixmap 映射设备树
  ├─ 解析设备树，获知物理内存布局
  ├─ memblock_init              ← 内存分配器才开始可用
  ├─ paging_init()              ← 建立完整线性映射
  │   ...
  └─ mm_init()                  ← buddy/slab 分配器初始化
```

## 11. 关键设计要点

1. **不能用常规 `p*d_populate`**：常规函数隐式调用 `virt_to_phys`，不能用于内核符号地址。此时 `lm_alias` 也不可用，所以必须用 `__p*d_populate` + `__pa_symbol` 手动填入物理地址。

2. **只建骨架，不做最终映射**：该函数只把各级页表串联起来，PTE 条目留空。后续通过 `__set_fixmap` 按需将物理页框映射到对应的 fixmap 虚拟地址。

3. **静态分配，零依赖**：所有页表放在 `__page_aligned_bss`，不依赖任何内存分配器，可以在内核启动最早期执行。

4. **PGD/P4D 共享处理**：如果内核映射和 fixmap 共享同一个顶级页表条目，`early_fixmap_init_pud` 会检测并复用已有条目，而非覆盖。

## 12. FIX_PTE / FIX_PMD / FIX_PUD / FIX_P4D / FIX_PGD 槽位详解

### 12.1 它们是什么

`FIX_PTE` ~ `FIX_PGD` 是 `enum fixed_addresses` 中的 5 个普通 fixmap 槽位（索引号 1486~1490），每个对应一个 4KB 的虚拟地址窗口。它们**不是页表本身**，而是用来**临时映射页表页**的虚拟地址窗口。

### 12.2 与 bm_pte/bm_pmd/bm_pud 的区别

| | bm_pte / bm_pmd / bm_pud | FIX_PTE ~ FIX_PGD |
|---|---|---|
| **角色** | fixmap 的页表骨架（基础设施） | fixmap 的使用者（5 个普通槽位） |
| **是什么** | 静态 BSS 段的页表数组 | enum 索引号（编译时常量） |
| **作用** | 让所有 fixmap 虚拟地址能被 MMU 翻译 | 提供 5 个可复用的临时映射窗口 |
| **类比** | 水管系统 | 水龙头 |

### 12.3 为什么需要它们

`paging_init()` 要为所有物理内存建立线性映射，需要分配大量页表页。但 memblock 分配出的物理页**还没有虚拟地址映射**（线性映射正在建设中），CPU 无法直接访问。

解决方法：通过 fixmap 槽位临时映射这些物理页，写入内容后释放。

```c
// arch/arm64/mm/mmu.c 中的实际使用
pte_phys = pgtable_alloc(TABLE_PTE);     // 分配物理页，如 0x4200_0000
ptep = pte_set_fixmap(pte_phys);          // FIX_PTE 映射该物理页
// 展开: __set_fixmap(FIX_PTE, 0x4200_0000, PAGE_KERNEL)
// bm_pte 中 FIX_PTE 对应的条目写入 0x4200_0000

init_clear_pgtable(ptep);                // 通过虚拟地址清零物理页
ptep[index] = pfn_pte(...);              // 写入 PTE 条目

pte_clear_fixmap();                       // 释放映射，下次可映射其他物理页
// 展开: __set_fixmap(FIX_PTE, 0, FIXMAP_PAGE_CLEAR)
```

### 12.4 完整调用链

```
paging_init()
  └─ map_mem()
       └─ __create_pgd_mapping()
            │
            ├─ alloc_init_p4d()                          ← 使用 FIX_P4D
            │    p4dp = p4d_set_fixmap(p4d_phys)
            │    init_clear_pgtable(p4dp)
            │    __pgd_populate(pgdp, p4d_phys, ...)
            │    ...
            │    p4d_clear_fixmap()
            │    │
            │    └─ alloc_init_pud()                     ← 使用 FIX_PUD
            │         pudp = pud_set_fixmap(pud_phys)
            │         init_clear_pgtable(pudp)
            │         __p4d_populate(p4dp, pud_phys, ...)
            │         ...
            │         pud_clear_fixmap()
            │         │
            │         └─ alloc_init_cont_pmd()           ← 使用 FIX_PMD
            │              pmdp = pmd_set_fixmap(pmd_phys)
            │              init_clear_pgtable(pmdp)
            │              __pud_populate(pudp, pmd_phys, ...)
            │              ...
            │              pmd_clear_fixmap()
            │              │
            │              └─ alloc_init_cont_pte()      ← 使用 FIX_PTE
            │                   ptep = pte_set_fixmap(pte_phys)
            │                   init_clear_pgtable(ptep)
            │                   __pmd_populate(pmdp, pte_phys, ...)
            │                   ...
            │                   pte_clear_fixmap()
```

### 12.5 为什么需要 5 个独立槽位

建页表是**递归嵌套**的。在 `alloc_init_cont_pmd` 中通过 FIX_PMD 映射着一个 PMD 表页并正在写入时，会调用 `alloc_init_cont_pte`，此时又需要通过 FIX_PTE 映射另一个 PTE 表页。多个映射**同时存在**：

```
alloc_init_p4d:       FIX_P4D → 映射着 P4D 表页   ← 还在使用中
  alloc_init_pud:       FIX_PUD → 映射着 PUD 表页  ← 还在使用中
    alloc_init_cont_pmd:  FIX_PMD → 映射着 PMD 表页 ← 还在使用中
      alloc_init_cont_pte:  FIX_PTE → 映射着 PTE 表页 ← 正在写入
```

5 个槽位 = 5 级页表各一个，允许嵌套调用时同时映射 5 个不同的物理页，互不干扰。

### 12.6 与 bm_pte 的映射关系

FIX_PTE ~ FIX_PGD 在 bm_pte 数组中的位置（以 FIX_PTE 为例）：

```
bm_pte[2][pte_index(__fix_to_virt(FIX_PTE))]  → FIX_PTE 槽位对应的 PTE 条目
bm_pte[2][pte_index(__fix_to_virt(FIX_PMD))]  → FIX_PMD 槽位对应的 PTE 条目
bm_pte[2][pte_index(__fix_to_virt(FIX_PUD))]  → FIX_PUD 槽位对应的 PTE 条目
bm_pte[2][pte_index(__fix_to_virt(FIX_P4D))]  → FIX_P4D 槽位对应的 PTE 条目
bm_pte[2][pte_index(__fix_to_virt(FIX_PGD))]  → FIX_PGD 槽位对应的 PTE 条目
```

当 `pte_set_fixmap(phys)` 被调用时，就是往 `bm_pte` 中对应的那个 PTE 条目写入物理地址。`bm_pte` 是道路，`FIX_PTE` 是通过这条路到达的一扇门——每次调用时门后连接的物理页都不一样，是一个**可复用的临时窗口**。
