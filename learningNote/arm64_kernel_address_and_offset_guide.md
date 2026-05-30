# Linux ARM64 内核地址与 Offset 说明

基于当前工作树 `Linux 6.18.1`，并结合 ARM64 当前配置进行说明：

- `CONFIG_ARM64_4K_PAGES=y`
- `CONFIG_ARM64_VA_BITS_52=y`
- `CONFIG_ARM64_VA_BITS=52`
- `CONFIG_PGTABLE_LEVELS=5`
- `CONFIG_RANDOMIZE_BASE=y`
- `CONFIG_KASAN is not set`

本文重点解释 ARM64 内核里最常见也最容易混淆的一组地址和 offset：

- `VA_BITS` / `VA_BITS_MIN` / `vabits_actual`
- `PAGE_OFFSET`
- `PAGE_END`
- `MODULES_VADDR` / `MODULES_END` / `MODULES_VSIZE`
- `KIMAGE_VADDR`
- `KERNEL_START` / `KERNEL_END`
- `VMALLOC_START` / `VMALLOC_END`
- `VMEMMAP_RANGE` / `VMEMMAP_SIZE` / `vmemmap`
- `VMEMMAP_START` / `VMEMMAP_END`
- `FIXADDR_TOP` / `FIXADDR_START`
- `PHYS_OFFSET` / `memstart_addr`
- `PHYS_PFN_OFFSET`
- `DIRECT_MAP_PHYSMEM_END`
- `MIN_MEMBLOCK_ADDR` / `MAX_MEMBLOCK_ADDR`
- `kaslr_offset()` / `kaslr_enabled()`
- `kimage_voffset`
- `__pa()` / `__pa_symbol()` / `__va()` / `__virt_to_phys()` / `__phys_to_virt()`

---

## 目录

<details>
<summary><a href="#1-先建立整体模型">1. 先建立整体模型</a></summary>

- [1.1 物理地址](#11-物理地址)
- [1.2 线性映射地址](#12-线性映射地址)
- [1.3 内核镜像地址](#13-内核镜像地址)

</details>

<details>
<summary><a href="#2-编译期固定宏的规范定义">2. 编译期固定宏的规范定义</a></summary>

- [2.1 `VA_BITS`](#21-va_bits)
- [2.2 `VA_BITS_MIN`](#22-va_bits_min)
- [2.3 `PAGE_OFFSET`](#23-page_offset)
- [2.4 `PAGE_END`](#24-page_end)
- [2.5 `MODULES_VADDR`、`MODULES_END` 和 `MODULES_VSIZE`](#25-modules_vaddrmodules_end-和-modules_vsize)
- [2.6 `KIMAGE_VADDR`](#26-kimage_vaddr)
- [2.7 `KERNEL_START` 和 `KERNEL_END`](#27-kernel_start-和-kernel_end)
- [2.8 `VMALLOC_START` 和 `VMALLOC_END`](#28-vmalloc_start-和-vmalloc_end)
- [2.9 `VMEMMAP_RANGE`、`VMEMMAP_SIZE`、`VMEMMAP_START`、`VMEMMAP_END`](#29-vmemmap_rangevmemmap_sizevmemmap_startvmemmap_end)
- [2.10 `PCI_IO_START` 和 `PCI_IO_END`](#210-pci_io_start-和-pci_io_end)
- [2.11 `FIXADDR_TOP`、`FIXADDR_START`、`FIXADDR_TOT_START`](#211-fixaddr_topfixaddr_startfixaddr_tot_start)

</details>

<details>
<summary><a href="#3-运行时变量的规范定义">3. 运行时变量的规范定义</a></summary>

- [3.1 `memstart_addr` 与 `PHYS_OFFSET`](#31-memstart_addr-与-phys_offset)
- [3.2 `vabits_actual`](#32-vabits_actual)
- [3.3 `MIN_MEMBLOCK_ADDR` 和 `MAX_MEMBLOCK_ADDR`](#33-min_memblock_addr-和-max_memblock_addr)
- [3.4 `kaslr_offset()` 和 `kaslr_enabled()`](#34-kaslr_offset-和-kaslr_enabled)
- [3.5 `kimage_voffset`](#35-kimage_voffset)

</details>

<details>
<summary><a href="#4-arm64-的两套地址换算规则">4. ARM64 的两套地址换算规则</a></summary>

- [4.1 线性映射换算](#41-线性映射换算)
- [4.2 内核镜像换算](#42-内核镜像换算)
- [4.3 ARM64 如何区分一个 VA 属于哪一类](#43-arm64-如何区分一个-va-属于哪一类)
- [4.4 `__virt_to_phys()` 的真实逻辑](#44-__virt_to_phys-的真实逻辑)
- [4.5 `__phys_to_kimg()` 的意义](#45-__phys_to_kimg-的意义)
- [4.6 为什么 `__pa_symbol()` 和 `__pa()` 不能混用](#46-为什么-__pa_symbol-和-__pa-不能混用)

</details>

<details>
<summary><a href="#5-linear-mapkernel-imagevmalloc-三类-va-的判别方法">5. linear map、kernel image、vmalloc 三类 VA 的判别方法</a></summary>

- [5.1 第一优先级：先判断它是不是 linear map](#51-第一优先级先判断它是不是-linear-map)
- [5.2 第二优先级：判断它是不是 built-in kernel image 地址](#52-第二优先级判断它是不是-built-in-kernel-image-地址)
- [5.3 第三优先级：判断它是不是 vmalloc 地址](#53-第三优先级判断它是不是-vmalloc-地址)
- [5.4 模块地址为什么容易和 vmalloc 混淆](#54-模块地址为什么容易和-vmalloc-混淆)
- [5.5 最实用的判断顺序](#55-最实用的判断顺序)
- [5.6 为什么不能把 `vmalloc` 地址直接传给 `__virt_to_phys()`](#56-为什么不能把-vmalloc-地址直接传给-__virt_to_phys)
- [5.7 一句话决策法](#57-一句话决策法)

</details>

<details>
<summary><a href="#6-pfnstruct-pagevmemmap-三者关系">6. PFN、`struct page`、`vmemmap` 三者关系</a></summary>

</details>

<details>
<summary><a href="#7-几个实际推导例子">7. 几个实际推导例子</a></summary>

- [7.1 由 `MODULES_VADDR` 推到 `MODULES_END`](#71-由-modules_vaddr-推到-modules_end)
- [7.2 由 `PAGE_END` 推到 `DIRECT_MAP_PHYSMEM_END`](#72-由-page_end-推到-direct_map_physmem_end)
- [7.3 为什么 `vmemmap + PHYS_PFN_OFFSET` 对应 direct map 的第一个 `struct page`](#73-为什么-vmemmap--phys_pfn_offset-对应-direct-map-的第一个-struct-page)
- [7.4 一个 kernel symbol 如何转成物理地址](#74-一个-kernel-symbol-如何转成物理地址)
- [7.5 同一个物理页可能有两种不同语义的虚拟地址](#75-同一个物理页可能有两种不同语义的虚拟地址)

</details>

<details>
<summary><a href="#8-启动阶段这些地址和-offset-是怎么建立起来的">8. 启动阶段这些地址和 offset 是怎么建立起来的</a></summary>

- [8.1 `early_map_kernel()` 先建立早期 kernel image 映射](#81-early_map_kernel-先建立早期-kernel-image-映射)
- [8.2 `__primary_switched` 保存 `kimage_voffset`](#82-__primary_switched-保存-kimage_voffset)
- [8.3 `arm64_memblock_init()` 建立 `memstart_addr`，从而建立 `PHYS_OFFSET`](#83-arm64_memblock_init-建立-memstart_addr从而建立-phys_offset)
- [8.4 `paging_init()` 建立完整内存映射](#84-paging_init-建立完整内存映射)
- [8.5 `setup_arch()` 里的大致顺序](#85-setup_arch-里的大致顺序)
- [8.6 这条时序的实际意义](#86-这条时序的实际意义)

</details>

<details>
<summary><a href="#9-当前内核配置下的关键数值">9. 当前内核配置下的关键数值</a></summary>

</details>

<details>
<summary><a href="#10-地址布局关系图">10. 地址布局关系图</a></summary>

</details>

<details>
<summary><a href="#11-最容易犯错的几个点">11. 最容易犯错的几个点</a></summary>

- [11.1 `KIMAGE_VADDR` 不是 `_text`](#111-kimage_vaddr-不是-_text)
- [11.2 `PHYS_OFFSET` 不是简单的 DRAM 起始地址别名](#112-phys_offset-不是简单的-dram-起始地址别名)
- [11.3 `__pa_symbol()` 不是普通 `virt_to_phys()`](#113-__pa_symbol-不是普通-virt_to_phys)
- [11.4 一个内核虚拟地址不一定属于 linear map](#114-一个内核虚拟地址不一定属于-linear-map)
- [11.5 52-bit 内核不代表运行时一定真在 52-bit VA 下运行](#115-52-bit-内核不代表运行时一定真在-52-bit-va-下运行)

</details>

<details>
<summary><a href="#12-建议的源码阅读路径">12. 建议的源码阅读路径</a></summary>

</details>

<details>
<summary><a href="#13-一句话总结">13. 一句话总结</a></summary>

</details>

---

## 1. 先建立整体模型

ARM64 内核里至少要区分三类地址空间，否则几乎一定会把这些宏看乱：

### 1.1 物理地址

就是 CPU/MMU 看到的真实物理地址，例如 DRAM 的物理起始地址。

### 1.2 线性映射地址

也叫 direct map 或 linear map。它把一段连续物理内存按固定公式映射到内核虚拟地址空间。其核心基址就是 `PAGE_OFFSET`。

线性映射的换算公式是：

```c
virt = phys - PHYS_OFFSET + PAGE_OFFSET
phys = virt - PAGE_OFFSET + PHYS_OFFSET
```

### 1.3 内核镜像地址

这是内核映像自身 `.text/.rodata/.data/.bss` 所在的虚拟地址区域，不等同于 linear map。它的编译期基准是 `KIMAGE_VADDR`，实际运行时还可能叠加 KASLR 偏移。

这一类地址和物理地址之间的换算核心是 `kimage_voffset`：

```c
phys = virt - kimage_voffset
virt = phys + kimage_voffset
```

所以，ARM64 里最重要的理解是：

- 线性映射地址靠 `PAGE_OFFSET` 和 `PHYS_OFFSET` 转换。
- 内核镜像地址靠 `kimage_voffset` 转换。
- 这两套换算不是一回事。

---

## 2. 编译期固定宏的规范定义

这些定义主要来自：

- `arch/arm64/include/asm/memory.h`
- `arch/arm64/include/asm/pgtable.h`
- `arch/arm64/include/asm/fixmap.h`

### 2.1 `VA_BITS`

定义：

```c
#define VA_BITS (CONFIG_ARM64_VA_BITS)
```

含义：内核支持的最大虚拟地址位数，不是运行时一定实际使用的位数。

当前配置中：

```c
VA_BITS = 52
```

### 2.2 `VA_BITS_MIN`

定义：

```c
#if VA_BITS > 48
#ifdef CONFIG_ARM64_16K_PAGES
#define VA_BITS_MIN (47)
#else
#define VA_BITS_MIN (48)
#endif
#else
#define VA_BITS_MIN (VA_BITS)
#endif
```

含义：单一内核二进制为了兼容硬件回退时，必须保证的最小 VA 位数。

当前配置是 4K 页且 `VA_BITS=52`，因此：

```c
VA_BITS_MIN = 48
```

### 2.3 `PAGE_OFFSET`

定义：

```c
#define _PAGE_OFFSET(va) (-(1UL << (va)))
#define PAGE_OFFSET (_PAGE_OFFSET(VA_BITS))
```

规范含义：线性映射区的起始虚拟地址，也是 TTBR1 内核地址空间底部的起点。

对当前配置：

```c
PAGE_OFFSET = -(1UL << 52)
            = 0xfff0000000000000
```

这也是 `Documentation/arch/arm64/memory.rst` 强调的点：52-bit VA 内核为了支持早期 48-bit fallback，仍然保持 `PAGE_OFFSET` 固定在 52-bit 的位置，以便优化 `phys_to_virt()` / `virt_to_phys()`。

### 2.4 `PAGE_END`

定义：

```c
#define _PAGE_END(va) (-(1UL << ((va) - 1)))
```

若未启用 KASAN：

```c
#define PAGE_END (_PAGE_END(VA_BITS_MIN))
```

规范含义：线性映射区的结束位置，超过它之后就是其他内核专用映射区域。

当前配置未启用 KASAN，因此：

```c
PAGE_END = -(1UL << 47)
         = 0xffff800000000000
```

注意：

- `PAGE_END` 是 linear map 的结束。
- 若启用 KASAN，`PAGE_END` 可能变成 `KASAN_SHADOW_START`，不再等于 `_PAGE_END(VA_BITS_MIN)`。

### 2.5 `MODULES_VADDR`、`MODULES_END` 和 `MODULES_VSIZE`

定义：

```c
#define MODULES_VADDR (_PAGE_END(VA_BITS_MIN))
#define MODULES_VSIZE (SZ_2G)
#define MODULES_END   (MODULES_VADDR + MODULES_VSIZE)
```

规范含义：

- `MODULES_VADDR` 是模块区起点
- `MODULES_VSIZE` 是模块区总预留大小
- `MODULES_END` 是模块区终点

当前配置下：

```c
MODULES_VADDR = 0xffff800000000000
MODULES_VSIZE = 0x0000000080000000  // 2GB
MODULES_END   = 0xffff800080000000
```

也就是说，在当前无 KASAN 的配置里：

```c
MODULES_VADDR == PAGE_END
```

但这只是当前配置下成立，不是永远成立。

### 2.6 `KIMAGE_VADDR`

定义：

```c
#define KIMAGE_VADDR (MODULES_END)
```

规范含义：内核镜像虚拟地址区域的编译期基准起点。

当前配置下：

```c
KIMAGE_VADDR = 0xffff800080000000
```

关键点：

- `KIMAGE_VADDR` 不是 `_text` 的最终运行时地址。
- 在启用 KASLR/可重定位内核时，实际 `_text` 虚拟地址通常是：

```c
&_text = KIMAGE_VADDR + kaslr_offset
```

代码里也有对应定义：

```c
static inline unsigned long kaslr_offset(void)
{
    return (u64)&_text - KIMAGE_VADDR;
}
```

### 2.7 `KERNEL_START` 和 `KERNEL_END`

定义：

```c
#define KERNEL_START _text
#define KERNEL_END   _end
```

规范含义：

- `KERNEL_START` 是内核镜像中第一个核心符号地址，本质上就是 `_text`
- `KERNEL_END` 是内核镜像结束符号，本质上就是 `_end`

它们描述的是“当前 kernel image 实际覆盖的符号边界”，而不是整个镜像区的编译期预留范围。

所以要区分：

- `KIMAGE_VADDR` 是镜像区的编译期基准
- `KERNEL_START`/`KERNEL_END` 是当前镜像实际内容边界

在源码里，像 `memblock_reserve(__pa_symbol(_text), _end - _text)` 这类写法，依赖的正是这组实际边界符号。

### 2.8 `VMALLOC_START` 和 `VMALLOC_END`

定义来自 `arch/arm64/include/asm/pgtable.h`：

```c
#define VMALLOC_START (MODULES_END)
```

如果 `VA_BITS == VA_BITS_MIN`：

```c
#define VMALLOC_END (VMEMMAP_START - SZ_8M)
```

如果 `VA_BITS > VA_BITS_MIN`：

```c
#define VMEMMAP_UNUSED_NPAGES ((_PAGE_OFFSET(vabits_actual) - PAGE_OFFSET) >> PAGE_SHIFT)
#define VMALLOC_END (VMEMMAP_START + VMEMMAP_UNUSED_NPAGES * sizeof(struct page) - SZ_8M)
```

规范含义：

- `VMALLOC_START` 是 vmalloc/ioremap/vmap 等区域的起点。
- `VMALLOC_END` 取决于当前实际 `vabits_actual`，因为 52-bit 内核可能在某些机器上回退成 48-bit 运行。

当前配置下可确定：

```c
VMALLOC_START = MODULES_END = KIMAGE_VADDR = 0xffff800080000000
```

但 `VMALLOC_END` 不能仅靠编译配置唯一确定，它还依赖运行时的 `vabits_actual` 以及 `sizeof(struct page)`。

这里的 `VMEMMAP_UNUSED_NPAGES` 也值得单独理解一下。它的含义是：

- 如果内核按 52-bit VA 构建，但实际机器只跑在更小的 `vabits_actual`
- 那么 `PAGE_OFFSET` 到 `_PAGE_OFFSET(vabits_actual)` 之间会出现一段“未实际使用的高位 direct-map 差额”
- 这段差额折算成页数后，就是 `VMEMMAP_UNUSED_NPAGES`

它直接参与 `VMALLOC_END` 的计算，本质上是在 52-bit/48-bit 兼容布局里回收一部分原本为了大地址空间预留的 vmemmap 余量。

### 2.9 `VMEMMAP_RANGE`、`VMEMMAP_SIZE`、`VMEMMAP_START`、`VMEMMAP_END`

定义：

```c
#define VMEMMAP_RANGE (_PAGE_END(VA_BITS_MIN) - PAGE_OFFSET)
#define VMEMMAP_SIZE  ((VMEMMAP_RANGE >> PAGE_SHIFT) * sizeof(struct page))

#define VMEMMAP_END   (-UL(SZ_1G))
#define VMEMMAP_START (VMEMMAP_END - VMEMMAP_SIZE)
```

规范含义：

- `VMEMMAP_RANGE` 是需要被 `struct page` 元数据覆盖的整个 direct map 物理覆盖范围
- `VMEMMAP_SIZE` 是为此需要预留的 `struct page` 数组总大小
- `VMEMMAP_START/END` 是这个数组在内核虚拟地址空间中的摆放位置

这说明 `vmemmap` 不是“给现有内存随便开一块 metadata 区”，而是从一开始就按“整个 linear region 需要多少 `struct page` 描述符”来反推大小。

其中当前可直接确定：

```c
VMEMMAP_END = 0xffffffffc0000000
```

而 `VMEMMAP_START` 取决于 `VMEMMAP_SIZE`，后者又依赖 `sizeof(struct page)`，因此最好把它视为“由当前内核类型系统和配置共同决定的编译期常量”。

在 `arch/arm64/include/asm/pgtable.h` 里还定义了一个非常常用的辅助宏：

```c
#define vmemmap ((struct page *)VMEMMAP_START - (memstart_addr >> PAGE_SHIFT))
```

它的含义不是“vmemmap 区起始地址”，而是一个便于按 PFN 直接索引 `struct page` 的基准指针。也就是之后常见的：

```c
vmemmap + pfn
```

就能定位到该 PFN 对应的 `struct page`。

### 2.10 `PCI_IO_START` 和 `PCI_IO_END`

定义：

```c
#define PCI_IO_SIZE  SZ_16M
#define PCI_IO_START (VMEMMAP_END + SZ_8M)
#define PCI_IO_END   (PCI_IO_START + PCI_IO_SIZE)
```

当前配置下：

```c
PCI_IO_START = 0xffffffffc0800000
PCI_IO_END   = 0xffffffffc1800000
```

### 2.11 `FIXADDR_TOP`、`FIXADDR_START`、`FIXADDR_TOT_START`

定义：

```c
#define FIXADDR_TOP (-UL(SZ_8M))
```

当前配置下：

```c
FIXADDR_TOP = 0xffffffffff800000
```

其余两个来自 `arch/arm64/include/asm/fixmap.h`：

```c
#define FIXADDR_SIZE      (__end_of_permanent_fixed_addresses << PAGE_SHIFT)
#define FIXADDR_START     (FIXADDR_TOP - FIXADDR_SIZE)
#define FIXADDR_TOT_SIZE  (__end_of_fixed_addresses << PAGE_SHIFT)
#define FIXADDR_TOT_START (FIXADDR_TOP - FIXADDR_TOT_SIZE)
```

规范含义：

- `FIXADDR_TOP` 是 fixmap 顶端。
- `FIXADDR_START` 是永久 fixmap 槽位的起点。
- `FIXADDR_TOT_START` 包含 early boot 临时 fixmap 槽位在内的总起点。

这些值和枚举 `enum fixed_addresses` 强相关，所以它们是编译期常量，但会随配置变化。

---

## 3. 运行时变量的规范定义

编译期常量还不够，ARM64 还要在启动时建立两组关键运行时变量：

- `memstart_addr`
- `vabits_actual`
- `kimage_voffset`

### 3.1 `memstart_addr` 与 `PHYS_OFFSET`

定义：

```c
extern s64 memstart_addr;
#define PHYS_OFFSET ({ VM_BUG_ON(memstart_addr & 1); memstart_addr; })
```

规范含义：

- `memstart_addr` 是线性映射起点 `PAGE_OFFSET` 对应的物理地址。
- `PHYS_OFFSET` 只是对运行时变量 `memstart_addr` 的宏包装。

这意味着：

- `PHYS_OFFSET` 不是纯编译期常量。
- 它也不一定简单等于“系统第一段 DRAM 的起始物理地址”。
- 它真正表示的是：`PAGE_OFFSET` 这一虚拟地址在物理内存里对齐并裁剪后所对应的基址。

#### `memstart_addr` 在哪里确定

它在 `arch/arm64/mm/init.c:arm64_memblock_init()` 中初始化，关键过程是：

```c
memstart_addr = round_down(memblock_start_of_DRAM(), ARM64_MEMSTART_ALIGN);
```

随后会根据以下因素继续调整：

- 线性映射窗口能否覆盖全部内存
- 内核镜像本身是否位于高地址
- 是否需要裁剪超出 linear region 的物理内存
- 52-bit 内核在 48-bit 实际硬件上回退时，是否需要把 linear map 的物理基址上移

代码里对应的关键逻辑是：

```c
if (IS_ENABLED(CONFIG_ARM64_VA_BITS_52) && (vabits_actual != 52))
    memstart_addr -= _PAGE_OFFSET(vabits_actual) - _PAGE_OFFSET(52);
```

这个逻辑非常关键，它解释了为什么 `PAGE_OFFSET` 可以固定为 52-bit 位置，但 `PHYS_OFFSET` 依然能在 48-bit fallback 下保持转换正确。

#### `PHYS_PFN_OFFSET`

定义：

```c
#define PHYS_PFN_OFFSET (PHYS_OFFSET >> PAGE_SHIFT)
```

规范含义：linear map 起始物理地址对应的 PFN 编号。

它不是“系统中第一个物理页框一定是哪个 PFN”的全局真理，而是 ARM64 内核在 direct map 视角下，把 `PAGE_OFFSET` 对应到哪个起始 PFN。

这也是很多页框和 `struct page` 相关换算的基础，因为 PFN 语义本来就是：

```c
PFN = phys_addr >> PAGE_SHIFT
```

所以 `PHYS_PFN_OFFSET` 本质上就是：

```c
PHYS_PFN_OFFSET = memstart_addr / PAGE_SIZE
```

#### `DIRECT_MAP_PHYSMEM_END`

定义：

```c
#define DIRECT_MAP_PHYSMEM_END __pa(PAGE_END - 1)
```

规范含义：linear map 最后一个虚拟字节 `PAGE_END - 1` 对应的物理地址，也就是 direct map 可覆盖的最高物理地址上界。

它表达的不是“实际 DRAM 一定到这里结束”，而是“当前这套 linear map 最多能覆盖到的物理末端”。

所以它很适合用来理解：

- 这套 direct map 的理论物理覆盖上限在哪里
- 为什么某些物理内存可能因为超出 linear region 而被裁剪

### 3.2 `vabits_actual`

定义：

```c
#if VA_BITS > 48
#define vabits_actual (64 - ((read_tcr() >> 16) & 63))
#else
#define vabits_actual ((u64)VA_BITS)
#endif
```

规范含义：当前机器上实际正在使用的 VA 位数。

所以：

- `VA_BITS` 是编译期最大值。
- `VA_BITS_MIN` 是编译期最小兼容值。
- `vabits_actual` 是运行时真实值。

它直接影响：

- `VMALLOC_END` 的计算
- 52-bit 内核回退到 48-bit 时 `memstart_addr` 的调整
- 早期布局代码对实际 TTBR1 地址空间大小的判断

### 3.3 `MIN_MEMBLOCK_ADDR` 和 `MAX_MEMBLOCK_ADDR`

定义：

```c
#define MIN_MEMBLOCK_ADDR 0
#define MAX_MEMBLOCK_ADDR U64_MAX
```

规范含义：ARM64 在最早期 memblock 发现阶段允许接受的物理地址搜索范围。

它们不是最终可映射范围，也不是 direct map 边界，而是内存发现阶段的“先放开、后裁剪”策略。源码里也有对应注释：

```c
Allow all memory at the discovery stage. We will clip it later.
```

所以这两个宏表达的是：

- 早期先不急着根据 linear map 能力裁掉内存
- 等 `arm64_memblock_init()` 里 `memstart_addr`、linear region、kernel image 位置都确定后，再做真正裁剪

### 3.4 `kaslr_offset()` 和 `kaslr_enabled()`

定义：

```c
static inline unsigned long kaslr_offset(void)
{
    return (u64)&_text - KIMAGE_VADDR;
}
```

以及：

```c
static inline bool kaslr_enabled(void)
```

规范含义：

- `kaslr_offset()` 返回当前内核镜像实际虚拟基址相对 `KIMAGE_VADDR` 的偏移量
- `kaslr_enabled()` 返回当前启动是否实际启用了 KASLR

这里要注意两点：

- 当前配置 `CONFIG_RANDOMIZE_BASE=y` 只表示“内核支持 KASLR”
- `kaslr_enabled()` 才表示“这次启动实际上有没有打开随机化”

因此：

- 编译配置是能力
- `kaslr_enabled()` 是本次运行状态
- `kaslr_offset()` 是实际生效后的偏移结果

### 3.5 `kimage_voffset`

定义：

```c
extern u64 kimage_voffset;
```

语义：内核镜像虚拟地址和其物理地址之间的差值。

换算公式：

```c
phys = virt - kimage_voffset
virt = phys + kimage_voffset
```

#### `kimage_voffset` 在哪里建立

它在 `arch/arm64/kernel/head.S` 的 `__primary_switched` 中保存：

```asm
adrp    x4, _text
sub     x4, x4, x0
str_l   x4, kimage_voffset, x5
```

这里的语义是：

- `adrp x4, _text` 取 `_text` 的虚拟地址页基址。
- `x0` 在这条路径上持有与当前镜像对应的物理地址基准。
- 两者相减后得到“内核镜像 VA 到 PA 的偏移差”。

所以 `kimage_voffset` 描述的不是 linear map，而是 kernel image mapping。

#### 它与 KASLR 的关系

在 `arch/arm64/kernel/pi/map_kernel.c` 中，早期映射内核时会先求：

```c
va_base = KIMAGE_VADDR + kaslr_offset;
```

因此在启用 KASLR 时，实际的镜像虚拟基址不是固定 `KIMAGE_VADDR`，而是 `KIMAGE_VADDR + kaslr_offset`。`kimage_voffset` 最终把这个实际 VA 与实际 PA 的对应关系固化下来。

---

## 4. ARM64 的两套地址换算规则

这是理解所有地址宏最重要的一节。

### 4.1 线性映射换算

定义：

```c
#define __lm_to_phys(addr) (((addr) - PAGE_OFFSET) + PHYS_OFFSET)
#define __phys_to_virt(x)  ((unsigned long)((x) - PHYS_OFFSET) | PAGE_OFFSET)
```

适用对象：

- direct map / linear map 中的内核虚拟地址
- 例如通过 page allocator、memblock、普通页框直接映射得到的地址

### 4.2 内核镜像换算

定义：

```c
#define __kimg_to_phys(addr) ((addr) - kimage_voffset)
#define __phys_to_kimg(x)    ((unsigned long)((x) + kimage_voffset))
```

适用对象：

- `_text`、`_stext`、`__start_rodata`、`_data`、`_end` 这类内核镜像符号
- 以及任何位于 kernel image mapping 里的地址

### 4.3 ARM64 如何区分一个 VA 属于哪一类

定义：

```c
#define __is_lm_address(addr) (((u64)(addr) - PAGE_OFFSET) < (PAGE_END - PAGE_OFFSET))
```

如果落在 `[PAGE_OFFSET, PAGE_END)` 区间，就认为它属于 linear map；否则按 kernel image mapping 处理。

### 4.4 `__virt_to_phys()` 的真实逻辑

定义：

```c
#define __virt_to_phys_nodebug(x) ({
    phys_addr_t __x = (phys_addr_t)(__tag_reset(x));
    __is_lm_address(__x) ? __lm_to_phys(__x) : __kimg_to_phys(__x);
})
```

这说明 ARM64 的 `virt_to_phys` 不是单公式，而是二选一：

- 在线性映射里，用 `PAGE_OFFSET`/`PHYS_OFFSET`

- 在镜像映射里，用 `kimage_voffset`

### 4.5 `__phys_to_kimg()` 的意义

定义：

```c
#define __phys_to_kimg(x) ((unsigned long)((x) + kimage_voffset))
```

它是 `__kimg_to_phys()` 的反向形式，含义是：把一个属于 kernel image 对应物理范围的物理地址，换回 kernel image 虚拟地址。

这和 `__phys_to_virt()` 的差别非常重要：

- `__phys_to_virt()` 回到 linear map
- `__phys_to_kimg()` 回到 kernel image mapping

同一个物理页，在 ARM64 上可能同时拥有这两种不同语义的虚拟地址。

### 4.6 为什么 `__pa_symbol()` 和 `__pa()` 不能混用

定义：

```c
#define __pa_symbol_nodebug(x) __kimg_to_phys((phys_addr_t)(x))
```

也就是说：

- `__pa_symbol(sym)` 专门给内核符号用，本质上走的是 `kimage_voffset`
- `__pa(x)` / `virt_to_phys(x)` 是针对一般内核虚拟地址的，它会先判断是否属于 linear map

在 early boot 阶段，很多代码只能安全地对内核镜像符号使用 `__pa_symbol()`，因为此时完整 linear map 还没有建立好。

这也是 ARM64 早期引导代码里大量出现 `__pa_symbol(_text)`、`__pa_symbol(_end)` 的根本原因。

---

## 5. linear map、kernel image、vmalloc 三类 VA 的判别方法

这一节回答一个非常实际的问题：当你在日志、回溯、页表打印、崩溃现场里看到一个内核虚拟地址时，应该先把它归到哪一类，然后再决定用哪套转换方式。

核心原则不是“先套公式”，而是“先分类，再换算”。

### 5.1 第一优先级：先判断它是不是 linear map

ARM64 自己给出的判定条件是：

```c
#define __is_lm_address(addr) (((u64)(addr) - PAGE_OFFSET) < (PAGE_END - PAGE_OFFSET))
```

也就是：

```c
addr in [PAGE_OFFSET, PAGE_END)
```

只要满足这个条件，就把它视为 linear map 地址。

这时应该使用的换算是：

```c
phys = addr - PAGE_OFFSET + PHYS_OFFSET
virt = phys - PHYS_OFFSET + PAGE_OFFSET
```

典型来源包括：

- page allocator 直接映射出来的普通内核内存
- memblock 早期保留下来的 RAM 在线性映射中的地址
- `page_to_virt()`、`__va()` 一类得到的 direct-map 地址

### 5.2 第二优先级：判断它是不是 built-in kernel image 地址

这类地址最可靠的判别方法，不是单纯看它是否落在某个大区间，而是看它是否来自内核镜像本身的符号和段。

最常见的特征是：

- 来自 `_text`、`_stext`、`__start_rodata`、`_data`、`_end` 等 vmlinux 内建符号
- 来自内建函数或内建全局对象的地址
- 落在当前运行时的 `[KERNEL_START, KERNEL_END)` 区间内

这时应该使用：

```c
phys = addr - kimage_voffset
virt = phys + kimage_voffset
```

或者直接对符号使用：

```c
__pa_symbol(sym)
```

这里最容易犯错的一点是：

- `KIMAGE_VADDR` 是镜像区编译期基准
- 真实镜像地址还会受到 `kaslr_offset` 影响

所以判断 kernel image 时，最好依赖“符号来源”或 `[KERNEL_START, KERNEL_END)`，而不是只看 `addr >= KIMAGE_VADDR`。

### 5.3 第三优先级：判断它是不是 vmalloc 地址

通用层给出的判断接口是：

```c
bool is_vmalloc_addr(const void *x)
```

其实现本质就是：

```c
addr >= VMALLOC_START && addr < VMALLOC_END
```

在 [mm/vmalloc.c](mm/vmalloc.c#L79) 里定义如下：

```c
bool is_vmalloc_addr(const void *x)
{
     unsigned long addr = (unsigned long)kasan_reset_tag(x);
     return addr >= VMALLOC_START && addr < VMALLOC_END;
}
```

一旦判定为 vmalloc 地址，就不要再试图用 `PAGE_OFFSET/PHYS_OFFSET` 或 `kimage_voffset` 做简单算术换算。

原因是：

- vmalloc/vmap/ioremap 区域通常不是线性连续物理页
- 它们的 VA 到 PA 关系由页表和 `vm_struct`/`vmap_area` 元数据决定
- 同一段 vmalloc 虚拟区间背后可能对应离散的物理页集合

因此，vmalloc 地址要用的不是“固定偏移公式”，而是“页表走查或 vmap 元数据查询”。

常见接口包括：

- `find_vm_area(addr)`
- `vmalloc_to_page(addr)`
- `vmalloc_to_pfn(addr)`

例如 [mm/vmalloc.c](mm/vmalloc.c#L779) 里的 `vmalloc_to_page()` 就是直接按页表层级走查该地址。

### 5.4 模块地址为什么容易和 vmalloc 混淆

模块地址区间是：

```c
[MODULES_VADDR, MODULES_END)
```

而 vmalloc 区间是：

```c
[VMALLOC_START, VMALLOC_END)
```

在 ARM64 当前布局中：

```c
VMALLOC_START = MODULES_END
```

所以模块区和 vmalloc 区是紧挨着的，但它们不是同一段地址区间。

不过通用内核里经常把“模块地址或 vmalloc 地址”放在一个判别接口里处理：

```c
int is_vmalloc_or_module_addr(const void *x)
```

其实现见 [mm/vmalloc.c](mm/vmalloc.c#L758)：

```c
if (addr >= MODULES_VADDR && addr < MODULES_END)
     return 1;
return is_vmalloc_addr(x);
```

这说明从“通用内核辅助接口”的角度看，模块区经常被视为 vmalloc-like 高位映射区域，而不是 linear map。

### 5.5 最实用的判断顺序

如果你手里只有一个地址 `addr`，最稳妥的判断顺序是：

```c
1. 先看 __is_lm_address(addr)
    -> true: 它是 linear map，走 PAGE_OFFSET/PHYS_OFFSET

2. 否则看它是不是 built-in kernel image 的符号或位于 [KERNEL_START, KERNEL_END)
    -> 是: 走 kimage_voffset / __pa_symbol()

3. 否则看 is_vmalloc_addr(addr)
    -> true: 它是 vmalloc/vmap/ioremap 一类地址，不能做简单算术转换

4. 否则再看 is_vmalloc_or_module_addr(addr)
    -> true: 很可能是模块区或 vmalloc-like 高位映射

5. 如果以上都不是
    -> 再考虑 fixmap、vmemmap、PCI_IO、特殊映射等专用区域
```

### 5.6 为什么不能把 `vmalloc` 地址直接传给 `__virt_to_phys()`

这是最关键的一条工程规则。

`__virt_to_phys_nodebug()` 的逻辑只有两类：

```c
__is_lm_address(__x) ? __lm_to_phys(__x) : __kimg_to_phys(__x)
```

也就是说：

- 它能正确处理 linear map
- 它也能正确处理 kernel image
- 但它并不会专门识别 vmalloc

所以如果把一个 vmalloc 地址直接塞给它，在非 debug 路径下往往会错误地落到 `__kimg_to_phys()`，结果没有语义保证。

换句话说：

- `linear map` 和 `kernel image` 是“可用固定偏移公式”的两类
- `vmalloc` 不是第三个偏移公式，而是“必须查页表/查 vmalloc 元数据”的一类

### 5.7 一句话决策法

看到一个 ARM64 内核 VA 时，可以先这么想：

- 落在 `[PAGE_OFFSET, PAGE_END)`：它是 linear map
- 来自 vmlinux 内建符号：它是 kernel image
- 来自 `vmalloc/vmap/ioremap`，或者 `is_vmalloc_addr()` 为真：它是 vmalloc-like 映射

只有前两类适合直接做固定偏移换算；第三类通常不适合。

---

## 6. PFN、`struct page`、`vmemmap` 三者关系

如果只记一个关系式，建议记这个：

```c
struct page *page = vmemmap + pfn;
```

这里背后的逻辑是：

- PFN 是物理页号，等于 `phys >> PAGE_SHIFT`
- `PHYS_PFN_OFFSET` 是 linear map 基准物理地址对应的 PFN
- `vmemmap` 是一个经过偏移修正后的基准指针
- 所以 `vmemmap + pfn` 可以直接落到该 PFN 对应的 `struct page`

也就是说，`vmemmap` 本质上是“把 PFN 空间投影成 `struct page` 数组索引空间”的桥。

---

## 7. 几个实际推导例子

前面的定义如果不落到算式上，很容易停留在“知道名字但不会用”的状态。下面给几组最常见的推导。

### 7.1 由 `MODULES_VADDR` 推到 `MODULES_END`

当前配置下：

```c
MODULES_VADDR = 0xffff800000000000
MODULES_VSIZE = 0x0000000080000000
MODULES_END   = MODULES_VADDR + MODULES_VSIZE
                            = 0xffff800080000000
```

这一步的意义很直接：模块区固定预留 `2GB`，内核镜像区编译期基址 `KIMAGE_VADDR` 就紧接在它后面。

### 7.2 由 `PAGE_END` 推到 `DIRECT_MAP_PHYSMEM_END`

定义是：

```c
#define DIRECT_MAP_PHYSMEM_END __pa(PAGE_END - 1)
```

如果 `PAGE_END - 1` 落在线性映射里，那么按 linear-map 公式：

```c
phys = virt - PAGE_OFFSET + PHYS_OFFSET
```

代入当前固定常量：

```c
PAGE_END - 1 = 0xffff7fffffffffff
PAGE_OFFSET  = 0xfff0000000000000
```

得到：

```c
DIRECT_MAP_PHYSMEM_END
    = (PAGE_END - 1) - PAGE_OFFSET + PHYS_OFFSET
    = 0x000f7fffffffffff + PHYS_OFFSET
```

这个结果表达的是：当前 direct map 最多能覆盖到 `PHYS_OFFSET` 之上的 `0x000f7fffffffffff` 这个物理偏移位置。

### 7.3 为什么 `vmemmap + PHYS_PFN_OFFSET` 对应 direct map 的第一个 `struct page`

因为：

```c
PHYS_PFN_OFFSET = PHYS_OFFSET >> PAGE_SHIFT
```

而：

```c
struct page *page = vmemmap + pfn;
```

所以当：

```c
pfn = PHYS_PFN_OFFSET
```

就得到：

```c
vmemmap + PHYS_PFN_OFFSET
```

它正好对应 linear map 起始物理页框的 `struct page` 描述符。

### 7.4 一个 kernel symbol 如何转成物理地址

对 `_text`、`_end` 这类 kernel image symbol，不应该先假设它在线性映射里，而应该走：

```c
__pa_symbol(sym) = __kimg_to_phys(sym) = sym - kimage_voffset
```

如果实际运行时：

```c
&_text = KIMAGE_VADDR + kaslr_offset
```

那么：

```c
__pa_symbol(_text) = &_text - kimage_voffset
```

也就是说，`kimage_voffset` 把“带 KASLR 的实际镜像虚拟地址”拉回到它真正装载的物理地址。

### 7.5 同一个物理页可能有两种不同语义的虚拟地址

假设某个物理地址 `phys` 同时：

- 被 linear map 覆盖
- 也属于内核镜像装载区的一部分

那么它可能同时对应：

```c
linear_map_va = __phys_to_virt(phys)
kimage_va     = __phys_to_kimg(phys)
```

这两个地址不要求相等。区别在于：

- `__phys_to_virt()` 回到 direct map
- `__phys_to_kimg()` 回到 kernel image mapping

这也是 ARM64 上“同一物理页有多个内核虚拟别名”最基础的来源之一。

---

## 8. 启动阶段这些地址和 offset 是怎么建立起来的

把几个宏分开背不难，真正难的是弄清楚它们在启动时是谁先建立、谁依赖谁。

### 8.1 `early_map_kernel()` 先建立早期 kernel image 映射

在 [arch/arm64/kernel/pi/map_kernel.c](arch/arm64/kernel/pi/map_kernel.c#L241) 中，`early_map_kernel()` 会先决定：

- 当前 early boot 使用的 `va_bits`
- 是否启用 KASLR
- 早期内核镜像的虚拟基址 `va_base`

关键代码是：

```c
va_base = KIMAGE_VADDR + kaslr_offset;
map_kernel(kaslr_offset, va_base - pa_base, root_level);
```

这一步建立的是“内核镜像先能跑起来”的早期映射，不是完整的 linear map。

### 8.2 `__primary_switched` 保存 `kimage_voffset`

在 [arch/arm64/kernel/head.S](arch/arm64/kernel/head.S#L220) 中，切换到内核虚拟地址执行后，会立刻保存：

```asm
adrp    x4, _text
sub     x4, x4, x0
str_l   x4, kimage_voffset, x5
```

这一步的结果是：kernel image 的 VA/PA 差值被固定到 `kimage_voffset`。

从这之后，内核镜像符号就可以可靠地通过 `__pa_symbol()` 做地址转换。

### 8.3 `arm64_memblock_init()` 建立 `memstart_addr`，从而建立 `PHYS_OFFSET`

在 [arch/arm64/mm/init.c](arch/arm64/mm/init.c#L185) 中，`arm64_memblock_init()` 会根据：

- DRAM 起始地址
- linear region 可覆盖范围
- kernel image 位置
- 是否 52-bit 回退到更小 `vabits_actual`

最终确定：

```c
memstart_addr
```

而：

```c
PHYS_OFFSET = memstart_addr
```

所以真正意义上的 linear-map 基准物理地址，是在这一步才稳定下来的。

### 8.4 `paging_init()` 建立完整内存映射

在 [arch/arm64/mm/mmu.c](arch/arm64/mm/mmu.c#L1351) 中：

```c
void __init paging_init(void)
{
        map_mem(swapper_pg_dir);
        memblock_allow_resize();
        create_idmap();
        declare_kernel_vmas();
}
```

这一步会把完整内存映射补齐，尤其是 direct map 的正式建立和完善。

### 8.5 `setup_arch()` 里的大致顺序

在 [arch/arm64/kernel/setup.c](arch/arm64/kernel/setup.c#L332) 中，相关主线顺序很清楚：

```c
arm64_memblock_init();
paging_init();
bootmem_init();
```

把前面的早期路径合起来，可以粗略理解为：

```text
early_map_kernel()
    -> 先把 kernel image 映射起来
__primary_switched
    -> 记录 kimage_voffset
start_kernel / setup_arch()
    -> arm64_memblock_init()
    -> 确定 memstart_addr / PHYS_OFFSET
    -> paging_init()
    -> 建立完整 direct map 和其他内核映射
```

### 8.6 这条时序的实际意义

这条链路解释了一个经常让人困惑的问题：为什么 early boot 代码喜欢用 `__pa_symbol()`，而不是普通 `virt_to_phys()`？

答案就是：

- `kimage_voffset` 建得更早
- `PHYS_OFFSET`/完整 linear map 建得更晚

所以在很多 very early boot 路径上，能稳定依赖的是 kernel image mapping，而不是完整 direct map。

---

## 9. 当前内核配置下的关键数值

在当前工作树配置下，可以直接确定以下值：

| 名称 | 定义 | 当前值 |
| --- | --- | --- |
| `VA_BITS` | `CONFIG_ARM64_VA_BITS` | `52` |
| `VA_BITS_MIN` | 52-bit + 4K pages 回退下限 | `48` |
| `PAGE_OFFSET` | `-(1UL << 52)` | `0xfff0000000000000` |
| `PAGE_END` | `-(1UL << 47)` | `0xffff800000000000` |
| `MODULES_VADDR` | `_PAGE_END(VA_BITS_MIN)` | `0xffff800000000000` |
| `MODULES_VSIZE` | `2GB` | `0x0000000080000000` |
| `MODULES_END` | `MODULES_VADDR + 2G` | `0xffff800080000000` |
| `KIMAGE_VADDR` | `MODULES_END` | `0xffff800080000000` |
| `VMALLOC_START` | `MODULES_END` | `0xffff800080000000` |
| `VMEMMAP_END` | `-1GB` | `0xffffffffc0000000` |
| `PCI_IO_START` | `VMEMMAP_END + 8MB` | `0xffffffffc0800000` |
| `PCI_IO_END` | `PCI_IO_START + 16MB` | `0xffffffffc1800000` |
| `FIXADDR_TOP` | `-8MB` | `0xffffffffff800000` |

以下几个值不能只靠 `.config` 唯一写死：

- `PHYS_OFFSET`，因为它来自运行时 `memstart_addr`
- `kimage_voffset`，因为它取决于镜像实际装载物理地址和 KASLR 偏移
- `vabits_actual`，因为它取决于实际硬件能力和启动路径
- `VMALLOC_END`，因为它依赖 `vabits_actual`
- `VMEMMAP_START`，因为它依赖 `sizeof(struct page)` 以及 `VMEMMAP_SIZE`

---

## 10. 地址布局关系图

在当前配置语义下，可以把 TTBR1 内核地址空间粗略理解成：

```text
low kernel VA
PAGE_OFFSET                           线性映射起点
    |
    |<---------------- linear map ---------------->|
    |
PAGE_END == MODULES_VADDR             线性映射结束 / 模块区开始
    |
    |<---- modules (2GB) ---->|
    |
KIMAGE_VADDR == MODULES_END           内核镜像区编译期基址
    |
    |<---- kernel image / vmalloc / vmap / ioremap ... ---->|
    |
...                                   vmemmap / PCI IO / fixmap 等高端保留区
    |
VMEMMAP_END
PCI_IO_START
PCI_IO_END
FIXADDR_TOP
high kernel VA
```

注意：这个图是逻辑关系图，不是严格按比例绘制。

---

## 11. 最容易犯错的几个点

### 11.1 `KIMAGE_VADDR` 不是 `_text`

`KIMAGE_VADDR` 是镜像区编译期基准，而 `_text` 的实际 VA 可能因为 KASLR 变成：

```c
&_text = KIMAGE_VADDR + kaslr_offset
```

### 11.2 `PHYS_OFFSET` 不是简单的 DRAM 起始地址别名

它是 `PAGE_OFFSET` 这条 linear map 起点在物理地址上的对应值，会经过对齐、裁剪，以及 52-bit 到 48-bit fallback 调整。

### 11.3 `__pa_symbol()` 不是普通 `virt_to_phys()`

`__pa_symbol()` 是给 kernel image symbol 用的，本质走 `kimage_voffset`。早期 boot code 依赖它，是因为那时 linear map 还不完整。

### 11.4 一个内核虚拟地址不一定属于 linear map

ARM64 必须先判定地址是否位于 `[PAGE_OFFSET, PAGE_END)`，否则就必须走 kernel image 的换算逻辑。

### 11.5 52-bit 内核不代表运行时一定真在 52-bit VA 下运行

当前内核是按 52-bit VA 构建的，但 `vabits_actual` 仍然可能在某些机器上回退到 48。ARM64 的很多宏和布局设计，正是为了兼容这件事。

---

## 12. 建议的源码阅读路径

如果要把这些概念彻底吃透，建议按下面顺序读：

1. `arch/arm64/include/asm/memory.h`
2. `arch/arm64/include/asm/pgtable.h`
3. `arch/arm64/include/asm/fixmap.h`
4. `Documentation/arch/arm64/memory.rst`
5. `arch/arm64/kernel/pi/map_kernel.c`
6. `arch/arm64/kernel/head.S`
7. `arch/arm64/mm/init.c`
8. `arch/arm64/mm/mmu.c`

按这个顺序读，基本能把 ARM64 的“编译期地址布局 + 启动期偏移建立 + 运行时地址转换”完整串起来。

---

## 13. 一句话总结

ARM64 内核地址布局的核心不是去背很多宏，而是抓住两条主线：

- linear map 用 `PAGE_OFFSET + PHYS_OFFSET`
- kernel image 用 `kimage_voffset`

其它像 `KIMAGE_VADDR`、`MODULES_VADDR`、`VMALLOC_START`、`VMEMMAP_END`、`FIXADDR_TOP`，本质上都是围绕这两条主线组织整个 TTBR1 内核地址空间。