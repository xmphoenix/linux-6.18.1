# ARM64 early_fixmap_init() 深度分析

> 基于 Linux 6.18.1, ARM64, 4KB 页 / 48-bit VA / 4 级页表配置

---

## 1. 概述

`early_fixmap_init()` 在 `setup_arch()` 早期被调用，负责在 `swapper_pg_dir` 中建立 fixmap 区域的 **页表骨架**（PGD → PUD → PMD → PTE 的链路），但 **不填入任何最终映射**。后续通过 `__set_fixmap()` 按需将物理地址写入 PTE 条目。

```
setup_arch()
  └─ early_fixmap_init()        ← 建立骨架
  └─ early_ioremap_init()       ← 初始化 early_ioremap 基础设施
  └─ fixmap_remap_fdt()         ← 第一个使用者：映射 FDT
  └─ ...
  └─ paging_init()              ← 建立完整的 swapper_pg_dir
```

---

## 2. Fixmap 地址范围

### 2.1 关键宏定义

| 宏 | 值 (4KB/48-bit) | 来源 |
|------|------|------|
| `FIXADDR_TOP` | `0xFFFF_FFFF_FF80_0000` | `memory.h:54` → `-UL(SZ_8M)` |
| `__end_of_fixed_addresses` | ~数百（取决于 CONFIG） | `fixmap.h` enum |
| `FIXADDR_TOT_SIZE` | `__end_of_fixed_addresses << PAGE_SHIFT` ≈ 数 MB | `fixmap.h:95` |
| `FIXADDR_TOT_START` | `FIXADDR_TOP - FIXADDR_TOT_SIZE` | `fixmap.h:96` |

### 2.2 fixmap 枚举槽位（从高地址到低地址增长）

```
FIXADDR_TOP = 0xFFFF_FFFF_FF80_0000
  │
  │  FIX_HOLE                    ← 第 0 号，紧贴 FIXADDR_TOP
  │  FIX_FDT_END ... FIX_FDT    ← FDT 窗口 (MAX_FDT_SIZE + 1 页)
  │  FIX_EARLYCON_MEM_BASE      ← early console
  │  FIX_TEXT_POKE0              ← text poke
  │  FIX_VNCR_END ... FIX_VNCR  ← KVM (如果启用)
  │  FIX_APEI_GHES_*            ← GHES (如果启用)
  │  FIX_ENTRY_TRAMP_TEXT*       ← KPTI trampoline (如果启用)
  │  __end_of_permanent_fixed_addresses
  │  FIX_BTMAP_END ... FIX_BTMAP_BEGIN  ← early_ioremap (7 slots × 64 pages)
  │  FIX_PTE, FIX_PMD, FIX_PUD, FIX_P4D, FIX_PGD  ← 页表创建临时窗口
  │  __end_of_fixed_addresses
  │
  ▼
FIXADDR_TOT_START ≈ 0xFFFF_FFFF_FB3x_xxxx
```

**注意**：fixmap 的虚拟地址是 **从高往低增长** 的。`__fix_to_virt(idx) = FIXADDR_TOP - ((idx) << PAGE_SHIFT)`。

### 2.3 在页表中的索引

| 页表层级 | 索引 | 说明 |
|---------|------|------|
| PGD | **511** | bits [47:39] = 0x1FF，内核虚拟地址空间最后一个条目 |
| PUD | **511** | bits [38:30] = 0x1FF |
| PMD | 多个 | 跨越约 3 个 2MB PMD 条目（取决于 fixmap 总大小） |

---

## 3. 页表存储位置

### 3.1 分配方式对比

| 页表 | 分配方式 | 所在段 | 分配位置 |
|------|---------|--------|---------|
| `swapper_pg_dir` (PGD) | linker script 手动分配 | `.rodata` 后 | `vmlinux.lds.S:235` |
| `bm_pud` | C static 变量 | `.bss..page_aligned` | `fixmap.c:37` |
| `bm_pmd` | C static 变量 | `.bss..page_aligned` | `fixmap.c:36` |
| `bm_pte[N]` | C static 变量 | `.bss..page_aligned` | `fixmap.c:34` |

### 3.2 实际 vmlinux 符号地址

```
符号                    虚拟地址                  大小      所在段
──────────────────────────────────────────────────────────────────
swapper_pg_dir          0xffff_8000_8145_f000     4KB       linker script (紧跟 .text)
__bss_start             0xffff_8000_8227_c000     ---       BSS 段起始
bm_pud                  0xffff_8000_8227_f000     4KB       .bss..page_aligned
bm_pmd                  0xffff_8000_8228_0000     4KB       .bss..page_aligned
bm_pte[0]              0xffff_8000_8228_1000     4KB       .bss..page_aligned
bm_pte[1]              0xffff_8000_8228_2000     4KB       .bss..page_aligned
bm_pte[2]              0xffff_8000_8228_3000     4KB       .bss..page_aligned
```

**PGD 与 PUD/PMD/PTE 相距约 14MB**，完全不连续。这无关紧要 —— MMU 硬件通过页表描述符里的物理地址定位下一级表。

### 3.3 BSS 数组维度

```c
// PTRS_PER_PTE = PTRS_PER_PMD = PTRS_PER_PUD = 512 (4KB 页)

static pud_t bm_pud[512]    __page_aligned_bss;   // 512 × 8B = 4KB = 1 页
static pmd_t bm_pmd[512]    __page_aligned_bss;   // 512 × 8B = 4KB = 1 页
static pte_t bm_pte[3][512] __page_aligned_bss;   // 3 × 512 × 8B = 12KB = 3 页
```

`NR_BM_PTE_TABLES = 3` 由宏计算得出：

```c
#define NR_BM_PTE_TABLES  SPAN_NR_ENTRIES(FIXADDR_TOT_START, FIXADDR_TOP, PMD_SHIFT)
// = ((FIXADDR_TOP - 1) >> 21) - (FIXADDR_TOT_START >> 21) + 1
// fixmap 范围跨 3 个 2MB PMD 条目 → 需要 3 个 PTE 页
```

`NR_BM_PMD_TABLES = 1`（fixmap 只跨 1 个 1GB PUD 条目），代码中有 `static_assert` 验证。

### 3.4 内存布局示意图

```
低地址                                                              高地址
────────────────────────────────────────────────────────────────────────
│ .text          │ PGD 区域        │ .init          │ .data │ .bss            │
│                │                 │                │       │                  │
│ 内核代码       │ idmap_pg_dir    │ init_idmap_    │       │ bm_pud   (4KB)  │
│                │ tramp_pg_dir    │   pg_dir       │       │ bm_pmd   (4KB)  │
│                │ reserved_pg_dir │ init_pg_dir    │       │ bm_pte[0](4KB)  │
│                │ swapper_pg_dir★│                │       │ bm_pte[1](4KB)  │
│                │                 │                │       │ bm_pte[2](4KB)  │
────────────────────────────────────────────────────────────────────────
           0x8145c000       0x81460000                  0x8227f000
```

---

## 4. 执行前状态

`early_fixmap_init()` 在 `setup_arch()` 中调用时：

- `swapper_pg_dir` 已通过 `memcpy` 从 `init_pg_dir` 拷入根 PGD 条目
- PGD 中 **只有内核映像的条目**（PGD index ≈ 256）
- **fixmap 区域的 PGD[511] 为空**
- `bm_pud`/`bm_pmd`/`bm_pte` 全在 BSS 段，**初始全零**
- **linear map 尚未建立**（paging_init 还没跑）
- 只能用 `__pa_symbol()` (基于 `kimage_voffset`) 做地址转换，不能用 `virt_to_phys()` (依赖 linear map)

---

## 5. 执行过程（4 步建立骨架）

### 5.1 入口：early_fixmap_init()

```c
void __init early_fixmap_init(void)
{
    unsigned long addr = FIXADDR_TOT_START;
    unsigned long end = FIXADDR_TOP;

    pgd_t *pgdp = pgd_offset_k(addr);        // → &swapper_pg_dir[511]
    p4d_t *p4dp = p4d_offset_kimg(pgdp, addr); // 4级: P4D 折叠, p4dp == pgdp
    early_fixmap_init_pud(p4dp, addr, end);
}
```

- `pgd_offset_k(addr)` = `init_mm.pgd + pgd_index(addr)` = `&swapper_pg_dir[511]`
- 4 级页表下 P4D 被折叠：`p4d_offset_kimg` 直接返回 `pgdp` 本身（强转类型）

### 5.2 第 1 步：填充 PGD[511] → bm_pud

```c
// early_fixmap_init_pud()
p4d = READ_ONCE(*p4dp);           // 读 swapper_pg_dir[511], 为 0 (空)
if (p4d_none(p4d))
    __p4d_populate(p4dp, __pa_symbol(bm_pud), P4D_TYPE_TABLE | P4D_TABLE_AF);
```

**写入：**
```
swapper_pg_dir[511] = PA(bm_pud) | 0x3    ← TABLE descriptor + AF bit
```

### 5.3 第 2 步：填充 bm_pud[511] → bm_pmd

```c
// early_fixmap_init_pmd()
pudp = pud_offset_kimg(p4dp, addr);  // → &bm_pud[511]
pud = READ_ONCE(*pudp);              // 为 0 (空)
if (pud_none(pud))
    __pud_populate(pudp, __pa_symbol(bm_pmd), PUD_TYPE_TABLE | PUD_TABLE_AF);
```

**写入：**
```
bm_pud[511] = PA(bm_pmd) | 0x3    ← TABLE descriptor + AF bit
```

### 5.4 第 3 步：循环填充 bm_pmd[idx] → bm_pte[i]

```c
pmdp = pmd_offset_kimg(pudp, addr);   // → &bm_pmd[pmd_index(FIXADDR_TOT_START)]
do {
    next = pmd_addr_end(addr, end);
    early_fixmap_init_pte(pmdp, addr); // 对每个 PMD 条目填 PTE
} while (pmdp++, addr = next, addr != end);
```

循环 3 次（`NR_BM_PTE_TABLES = 3`），每次：

```c
// early_fixmap_init_pte()
pmd = READ_ONCE(*pmdp);            // 为 0
if (pmd_none(pmd)) {
    ptep = bm_pte[BM_PTE_TABLE_IDX(addr)];
    __pmd_populate(pmdp, __pa_symbol(ptep), PMD_TYPE_TABLE | PMD_TABLE_AF);
}
```

**写入（3 次循环）：**
```
bm_pmd[idx+0] = PA(bm_pte[0]) | 0x3
bm_pmd[idx+1] = PA(bm_pte[1]) | 0x3
bm_pmd[idx+2] = PA(bm_pte[2]) | 0x3
```

### 5.5 第 4 步：PTE 层 —— 不填充

**所有 `bm_pte[*][*]` 保持为 0**。只建骨架，具体物理映射留给 `__set_fixmap()` 按需填入。

---

## 6. 执行后的页表结构

```
swapper_pg_dir (PGD)
├─ [0..255]    = (可能为空或内核映像条目)
├─ [256..]     = PA(内核映像的 PUD 表) | TABLE    ← 之前已有
├─ ...
└─ [511]       = PA(bm_pud) | TABLE | AF          ← ★ 新写入
                   │
                   ▼
                   bm_pud[512 entries]
                   ├─ [0..510]  = 0 (空)
                   └─ [511]     = PA(bm_pmd) | TABLE | AF    ← ★ 新写入
                                    │
                                    ▼
                                    bm_pmd[512 entries]
                                    ├─ [0..N-1]    = 0 (空)
                                    ├─ [idx+0]     = PA(bm_pte[0]) | TABLE | AF  ← ★
                                    ├─ [idx+1]     = PA(bm_pte[1]) | TABLE | AF  ← ★
                                    ├─ [idx+2]     = PA(bm_pte[2]) | TABLE | AF  ← ★
                                    └─ [idx+3..511]= 0 (空)
                                                       │
                                                       ▼
                                                       bm_pte[3][512]
                                                       └─ 全部 = 0  ← 等 __set_fixmap() 填充
```

**总计写入：1 (PGD) + 1 (PUD) + 3 (PMD) = 5 个页表描述符。**

---

## 7. 地址转换：为什么用 _kimg 而非 _virt

### 7.1 问题

`early_fixmap_init()` 运行时 **linear map 还没建立**（`paging_init()` 还没跑），所以不能用 `virt_to_phys()`（它依赖 linear map 的 `PAGE_OFFSET - PHYS_OFFSET` 偏移）。

### 7.2 两套地址转换

| 接口 | 公式 | 适用时机 |
|------|------|---------|
| `__pa_symbol(x)` / `__kimg_to_phys(x)` | `VA - kimage_voffset` | 内核映像内符号，任何时候可用 |
| `virt_to_phys(x)` / `__virt_to_phys(x)` | `VA - PAGE_OFFSET + PHYS_OFFSET` | linear map 地址，paging_init 后可用 |

### 7.3 kimage_voffset 的赋值

在 `head.S` 的 `__primary_switched`（第 233-235 行）中赋值：

```asm
adrp    x4, _text          // x4 = _text 的运行时物理地址 (通过 PC-relative)
sub     x4, x4, x0         // x4 = _text_PA - _text_VA（实际是负值）
                            // 注: x0 此时 = __pa(_text)，但上面已经是 PA
str_l   x4, kimage_voffset  // 存入全局变量
```

实际关系：`kimage_voffset = VA(kernel) - PA(kernel)`，所以 `PA = VA - kimage_voffset`。

### 7.4 代码中的体现

注释（`fixmap.c:103-107`）明确指出：

```c
/*
 * The p*d_populate functions call virt_to_phys implicitly so they can't be used
 * directly on kernel symbols (bm_p*d). This function is called too early to use
 * lm_alias so __p*d_populate functions must be used to populate with the
 * physical address from __pa_symbol.
 */
```

因此代码中：
- 取 PGD/PUD/PMD 指针用 `*_offset_kimg()`（通过 `__phys_to_kimg` 转换）
- 填充描述符用 `__p*d_populate()` + `__pa_symbol()`（直接写物理地址）
- **不用** 普通的 `p*d_populate()`（内部调用 `virt_to_phys`，此时会出错）

### 7.5 pgdp / p4dp 的地址性质

`early_fixmap_init()` 中的 `pgdp`、`p4dp`、`pudp`、`pmdp` 指针都是 **虚拟地址**（在内核映像映射 kimg 空间中），不是物理地址。CPU 可以直接解引用它们，因为此时内核映像的映射已经建好了（由 `init_pg_dir` → `swapper_pg_dir` 传递）。

---

## 8. 16K 页的特殊处理

```c
if (CONFIG_PGTABLE_LEVELS > 3 && !p4d_none(p4d) &&
    p4d_page_paddr(p4d) != __pa_symbol(bm_pud)) {
    BUG_ON(!IS_ENABLED(CONFIG_ARM64_16K_PAGES));
}
```

在 16KB 页 + 4 级页表配置下，内核映像和 fixmap 可能共享同一个顶层 PGD 条目。此时 PGD[idx] 已经被 kernel mapping 填充，指向一个由 `map_kernel` 创建的 PUD 表（不是 `bm_pud`）。代码允许这种情况但加了 `BUG_ON` 确保只在 16K 页模式下发生。

4KB 页 / 48-bit VA 下此分支不会触发，因为 kernel image（PGD ≈ 256）和 fixmap（PGD = 511）使用不同的 PGD 条目。

---

## 9. __set_fixmap()：填充 PTE

骨架建好后，通过 `__set_fixmap()` 映射具体物理页：

```c
void __set_fixmap(enum fixed_addresses idx, phys_addr_t phys, pgprot_t flags)
{
    unsigned long addr = __fix_to_virt(idx);   // idx → 虚拟地址
    pte_t *ptep = fixmap_pte(addr);            // 直接用 C 数组索引取 PTE 指针

    if (pgprot_val(flags))
        __set_pte(ptep, pfn_pte(phys >> PAGE_SHIFT, flags));  // 映射
    else {
        __pte_clear(&init_mm, addr, ptep);                     // 取消映射
        flush_tlb_kernel_range(addr, addr + PAGE_SIZE);
    }
}
```

**`fixmap_pte()` 直接用 C 数组索引，不走页表遍历**：

```c
static inline pte_t *fixmap_pte(unsigned long addr)
{
    return &bm_pte[BM_PTE_TABLE_IDX(addr)][pte_index(addr)];
}
```

这是因为 `bm_pte` 数组既是页表本身（MMU 硬件通过物理地址访问），又是内核可直接寻址的 BSS 变量（CPU 通过虚拟地址访问）。两者指向同一块物理内存。

---

## 10. 第一个使用者：fixmap_remap_fdt()

```c
void *__init fixmap_remap_fdt(phys_addr_t dt_phys, int *size, pgprot_t prot)
{
    const u64 dt_virt_base = __fix_to_virt(FIX_FDT);  // FDT 的 fixmap 虚拟地址

    // 1. 映射第一页，读取 FDT header
    create_mapping_noalloc(dt_phys_base, dt_virt_base, PAGE_SIZE, prot);
    // → 内部调用 __set_fixmap() 填充 bm_pte 中对应的 PTE

    // 2. 验证 FDT magic，获取 size
    *size = fdt_totalsize(dt_virt);

    // 3. 如果 FDT 跨页，映射剩余页
    create_mapping_noalloc(dt_phys_base, dt_virt_base, offset + *size, prot);

    return dt_virt;  // 返回 fixmap 空间中的虚拟地址
}
```

此时通过 fixmap 窗口（`FIX_FDT_END ~ FIX_FDT`）可以访问设备树，而不需要完整的 linear map。

---

## 11. 总结

| 要点 | 说明 |
|------|------|
| **目的** | 在 swapper_pg_dir 中建立 fixmap 区域的 4 级页表骨架 |
| **写入的描述符** | 5 个 (1 PGD + 1 PUD + 3 PMD)，全是 TABLE 描述符 |
| **PTE 层** | 全部保持为 0，由 `__set_fixmap()` 按需填充 |
| **页表存储** | PGD 在 linker script 分配区(.text 后)；PUD/PMD/PTE 在 BSS 段的静态数组 |
| **地址转换** | 只用 `__pa_symbol()` + `*_offset_kimg()`，不用 `virt_to_phys()` |
| **不用 virt_to_phys 的原因** | linear map 还没建立，只有 kimage_voffset 可用 |
| **调用时机** | `setup_arch()` 早期，在 paging_init() 之前 |
| **第一个使用者** | `fixmap_remap_fdt()` — 映射设备树 |
| **特殊情况** | 16K 页下 kernel 和 fixmap 可能共享 PGD 条目 |

---

## 12. 源码索引

| 文件 | 行号 | 内容 |
|------|------|------|
| `arch/arm64/mm/fixmap.c:104-113` | `early_fixmap_init()` | 入口函数 |
| `arch/arm64/mm/fixmap.c:79-101` | `early_fixmap_init_pud()` | PUD 层：填充 PGD→PUD，调用 PMD |
| `arch/arm64/mm/fixmap.c:57-73` | `early_fixmap_init_pmd()` | PMD 层：填充 PUD→PMD，循环调用 PTE |
| `arch/arm64/mm/fixmap.c:43-53` | `early_fixmap_init_pte()` | PTE 层：填充 PMD→PTE |
| `arch/arm64/mm/fixmap.c:34-37` | `bm_pte/pmd/pud` | 静态 BSS 页表数组定义 |
| `arch/arm64/mm/fixmap.c:117-131` | `__set_fixmap()` | 运行时填充/清除单个 PTE |
| `arch/arm64/mm/fixmap.c:133-168` | `fixmap_remap_fdt()` | 映射设备树 |
| `arch/arm64/include/asm/fixmap.h:34-89` | `enum fixed_addresses` | fixmap 槽位定义 |
| `arch/arm64/include/asm/fixmap.h:93-96` | `FIXADDR_*` 宏 | fixmap 地址范围 |
| `arch/arm64/include/asm/memory.h:54` | `FIXADDR_TOP` | `-UL(SZ_8M)` |
| `arch/arm64/include/asm/pgtable.h:1223` | `p4d_offset_kimg()` | 4 级折叠版（返回 pgdp） |
| `arch/arm64/include/asm/pgtable.h:1081` | `pud_offset_kimg()` | PUD 查找（kimg 版） |
| `arch/arm64/include/asm/pgtable.h:958` | `pmd_offset_kimg()` | PMD 查找（kimg 版） |
| `arch/arm64/include/asm/memory.h:354` | `__phys_to_kimg()` | `PA + kimage_voffset → VA` |
| `arch/arm64/include/asm/memory.h:341` | `__kimg_to_phys()` | `VA - kimage_voffset → PA` |
| `arch/arm64/mm/mmu.c:54` | `kimage_voffset` | 定义（`__ro_after_init`） |
| `arch/arm64/kernel/head.S:233-235` | `kimage_voffset` 赋值 | `__primary_switched` 中 |
| `arch/arm64/kernel/vmlinux.lds.S:235` | `swapper_pg_dir` | linker script 分配位置 |
| `include/linux/linkage.h:40` | `__page_aligned_bss` | `→ .bss..page_aligned` |
| `include/asm-generic/vmlinux.lds.h:771` | BSS 布局 | `*(.bss..page_aligned)` |
