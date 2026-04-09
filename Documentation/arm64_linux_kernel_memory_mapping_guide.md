# ARM64 Linux Kernel 内存映射总览：Early Boot、Init Idmap 与 GDB 验证

## 开篇先记住

这份 init idmap 最容易混淆的地方，不是页表级数，而是 **到底在用哪一套地址**。

先直接记结论：

> `create_init_idmap()` 不是把 `vmlinux` 里的高链接地址直接映射到物理地址；它是先用链接符号确定镜像边界，再把镜像当前实际所在的物理地址区间做成一份 `VA = PA` 的临时页表。

也就是说，这里要同时区分 3 套地址：

1. 链接虚拟地址
	这是 `vmlinux` 符号视角，用来描述 `_stext`、`__initdata_begin`、`_end` 这些边界。
2. 当前实际物理地址
	这是内核镜像当前真正被加载到内存里的位置。
3. init idmap 运行时虚拟地址
	对这份 early idmap 来说，直接取第 2 项本身，所以运行时满足 **VA = PA**。

对当前 build，这三个关键边界可以直接对照成：

```text
symbol / linked VA                    actual PA              runtime idmap VA
_stext            0xffff800080010000  0x40210000             0x40210000
__initdata_begin  0xffff800081540000  0x41740000             0x41740000
_end              0xffff800082330000  0x42530000             0x42530000
```

所以真正写进 init idmap 页表的关系是：

```text
0x40210000 -> 0x40210000
0x41740000 -> 0x41740000
0x4252f000 -> 0x4252f000
```

而不是：

```text
0xffff800080010000 -> 0x40210000
```

后面这种“高地址 VA -> 物理地址”的关系，属于后续正式内核页表（例如 `swapper_pg_dir`）的工作，不是这份 init idmap 的工作。

因此，后面阅读本文时可以始终带着这句话：

> 链接地址只负责告诉我们镜像边界；真正写进 init idmap 页表左边的 VA，是镜像当前实际物理地址那一套数值。

再记一个同样关键的时序点：

> `create_init_idmap()` 执行时，这份页表还只是“写在内存里”，还没有装进 `TTBR0`；直到后面的 `__enable_mmu()`，`__pi_init_idmap_pg_dir` 这张 L0 根表才真正被写入 `TTBR0` 并开始生效。

所以这里要分清两个阶段：

1. 建表阶段
	`create_init_idmap()` 只是把 L0/L1/L2/L3 内容写到 `__pi_init_idmap_pg_dir` 这块内存里。
2. 生效阶段
	在 [arch/arm64/kernel/head.S](arch/arm64/kernel/head.S#L509) 的 `__primary_switch` 调用 [arch/arm64/kernel/head.S](arch/arm64/kernel/head.S#L459) 的 `__enable_mmu()` 时，`x2 = __pi_init_idmap_pg_dir`，随后才写入 `ttbr0_el1`。

还要再补一句覆盖范围，避免把它误认为“把整个 head.S 都映射了”：

- 它覆盖的是 `[_stext, _end)` 这段当前启动阶段需要访问的 kernel image
- 它不覆盖 `_text` 到 `_stext` 前面的 `.head.text`
- 但它确实覆盖了后续还要执行的 `.idmap.text` / `.rodata.text` / `.init.text` / `.init.data` 等内容

## 1. 目标

`create_init_idmap()` 的作用不是创建最终的内核页表，而是为 **MMU 打开前后的过渡阶段** 创建一份最小可用的 **identity map**：

- VA = PA
- 只覆盖当前启动阶段需要访问的内核镜像范围
- 文本区使用 `ROX`
- 数据区使用 `RW`

对应代码位于：

- [arch/arm64/kernel/pi/map_range.c](arch/arm64/kernel/pi/map_range.c#L91)
- [arch/arm64/kernel/head.S](arch/arm64/kernel/head.S#L92)


## 2. 关键代码

```c
asmlinkage phys_addr_t __init create_init_idmap(pgd_t *pg_dir, ptdesc_t clrmask)
{
	phys_addr_t ptep = (phys_addr_t)pg_dir + PAGE_SIZE; /* MMU is off */
	pgprot_t text_prot = PAGE_KERNEL_ROX;
	pgprot_t data_prot = PAGE_KERNEL;

	pgprot_val(text_prot) &= ~clrmask;
	pgprot_val(data_prot) &= ~clrmask;

	map_range(&ptep, (u64)_stext, (u64)__initdata_begin,
		  (phys_addr_t)_stext, text_prot, IDMAP_ROOT_LEVEL,
		  (pte_t *)pg_dir, false, 0);
	map_range(&ptep, (u64)__initdata_begin, (u64)_end,
		  (phys_addr_t)__initdata_begin, data_prot, IDMAP_ROOT_LEVEL,
		  (pte_t *)pg_dir, false, 0);

	return ptep;
}
```

这段代码把内核镜像切成两段：

1. `[_stext, __initdata_begin)`
   映射为 `ROX`
2. `[__initdata_begin, _end)`
   映射为 `RW`

两段都是 **identity map**，也就是：

```text
VA == PA
```


## 3. 当前构建下的页表级数

当前 kernel 配置：

- `CONFIG_ARM64_4K_PAGES=y`
- `CONFIG_PGTABLE_LEVELS=5`

但 init idmap 使用固定的：

- `IDMAP_VA_BITS = 48`
- `IDMAP_LEVELS = 4`
- `IDMAP_ROOT_LEVEL = 0`

因此这份 init idmap：

- 从 **level 0** 开始建表
- level 0、1 不直接放叶子映射
- **level 2** 优先放 2MB PMD block
- 必要时下钻到 **level 3**，放 4KB PTE


### 3.1 这几个结论是从哪里来的

这三条并不是从 `.config` 里直接读取出来的，而是从 ARM64 页表相关头文件的宏定义推导出来的。

首先看 [arch/arm64/include/asm/kernel-pgtable.h](arch/arm64/include/asm/kernel-pgtable.h#L29-L31)：

```c
#define IDMAP_VA_BITS		48
#define IDMAP_LEVELS		ARM64_HW_PGTABLE_LEVELS(IDMAP_VA_BITS)
#define IDMAP_ROOT_LEVEL	(4 - IDMAP_LEVELS)
```

这里可以直接得到：

- `IDMAP_VA_BITS = 48`
- `IDMAP_LEVELS` 需要继续通过 `ARM64_HW_PGTABLE_LEVELS()` 计算
- `IDMAP_ROOT_LEVEL = 4 - IDMAP_LEVELS`

然后看 [arch/arm64/include/asm/pgtable-hwdef.h](arch/arm64/include/asm/pgtable-hwdef.h#L8-L32)：

```c
#define PTDESC_ORDER 3
#define PTDESC_TABLE_SHIFT	(PAGE_SHIFT - PTDESC_ORDER)

#define ARM64_HW_PGTABLE_LEVELS(va_bits) \
	(((va_bits) - PTDESC_ORDER - 1) / PTDESC_TABLE_SHIFT)
```

对当前构建：

- `CONFIG_ARM64_4K_PAGES=y`
- 所以 `PAGE_SHIFT = 12`
- `PTDESC_ORDER = 3`
- `PTDESC_TABLE_SHIFT = 12 - 3 = 9`

代入 `va_bits = 48`：

$$
IDMAP\_LEVELS = \frac{48 - 3 - 1}{9} = \frac{44}{9} = 4
$$

这里是整数除法，因此结果为 `4`。

再代回去：

$$
IDMAP\_ROOT\_LEVEL = 4 - IDMAP\_LEVELS = 4 - 4 = 0
$$

所以最终结论是：

- `IDMAP_VA_BITS = 48`
- `IDMAP_LEVELS = 4`
- `IDMAP_ROOT_LEVEL = 0`


### 3.2 为什么这和 `CONFIG_PGTABLE_LEVELS=5` 不矛盾

当前 `.config` 里确实有：

- `CONFIG_PGTABLE_LEVELS=5`

但这表示的是：

- **整个 kernel** 支持的页表级数配置是 5 级

而 `create_init_idmap()` 用的是一套专门服务于 early boot 的 idmap 规则：

- 它固定使用 `IDMAP_VA_BITS = 48`
- 对 4KB granule 来说，覆盖 48-bit VA 只需要 4 级硬件翻译层

所以更准确地说：

- `CONFIG_PGTABLE_LEVELS=5` 是系统整体能力
- `IDMAP_LEVELS=4` 是这份 early identity map 的实际翻译层数

这就是为什么在 GDB 里看到的 init idmap 结构是：

- L0
- L1
- L2
- L3

而不是再往上多出一层新的 idmap 级别。


### 3.3 5 级内核页表配置 vs 4 级 init idmap

这里还有一个很容易混淆的点：

- `CONFIG_PGTABLE_LEVELS=5` 说的是整个内核的常规页表能力
- `IDMAP_LEVELS=4` 说的是 early boot 这份 idmap 实际使用的硬件翻译层数

在当前 4KB granule 下，这两套视角可以这样对照：

```text
Linux 常规命名          ARM64 硬件级别      单个表项覆盖范围
PGD                     level -1            256 TB
P4D                     level 0             512 GB
PUD                     level 1             1 GB
PMD                     level 2             2 MB
PTE                     level 3             4 KB
```

而 `create_init_idmap()` 固定从：

- `IDMAP_ROOT_LEVEL = 0`

开始建表，所以它直接使用的是：

```text
level 0 -> level 1 -> level 2 -> level 3
```

也就是说：

- 常规 5 级 kernel page table 可以包含一个更高的硬件顶层 `level -1`
- init idmap 因为只覆盖 48-bit VA，不需要这一层
- 所以 GDB 里看到的根表是 `L0`，而不是再多出一个 `L-1`

这里最好始终区分两套名字：

- `L0/L1/L2/L3` 是硬件翻译级别
- `PGD/P4D/PUD/PMD/PTE` 是 Linux 常规类型命名

在 early boot 的 `map_range()` 路径里，真正决定当前“按哪一层来解释”的，是 `level` 参数，而不是 C 类型名字本身。


### 3.4 `IDMAP_ROOT_LEVEL = 0` 到底是什么意思

这句话的准确含义不是“没有根表”，而是：

- 这份 early idmap 的页表遍历从 **硬件 level 0** 开始
- 不需要额外再包一层 **硬件 level -1** 顶层表

这件事最好用“同一个地址走两种遍历”的方式来看。

先取你现在一直在看的地址：

```text
_stext = 0xffff800080010000
```

对 4KB granule，每级索引都是 9 bit，所以如果把所有可能级别都写出来，索引位段是：

```text
level -1 index = (va >> 48) & 0x1ff
level  0 index = (va >> 39) & 0x1ff
level  1 index = (va >> 30) & 0x1ff
level  2 index = (va >> 21) & 0x1ff
level  3 index = (va >> 12) & 0x1ff
```

代入 `_stext = 0xffff800080010000`，得到：

```text
level -1 : 511
level  0 : 256
level  1 : 2
level  2 : 0
level  3 : 16
```

### 3.4.1 如果“真的”要走 5 级硬件页表，会是什么样

如果某份页表真的需要 5 级硬件翻译，那么对这个地址，walk 会是：

```text
root(level -1)[511]
	-> level 0[256]
		-> level 1[2]
			-> level 2[0]
				-> level 3[16]
```

这里最上面那一页就是所谓的：

- **level -1 顶层页表**

它并不是什么“负一层的神秘页表”，只是 ARM64 为了描述“最多 5 级硬件页表”而采用的编号方式。

对 4KB granule，硬件最多支持 5 级，所以内核宏把它们统一编号成：

```text
level -1, level 0, level 1, level 2, level 3
```

也就是说：

- `level -1` 只是“比 L0 再高一层”的那一页根表
- 它消耗的是地址的更高 9 个索引 bit，也就是位 `[56:48]`
- 在这个例子里，对应索引正好是 `511`

### 3.4.2 但当前这份 init idmap 实际不是这么走的

对当前这份 init idmap：

- `IDMAP_VA_BITS = 48`
- `IDMAP_LEVELS = 4`
- `IDMAP_ROOT_LEVEL = 0`

这表示硬件遍历只消费到 48-bit VA 所需的 4 级，所以它实际走的是：

```text
root(level 0)[256]
	-> level 1[2]
		-> level 2[0]
			-> level 3[16]
```

注意这里和上面的区别只有一个，但非常关键：

- 5 级 walk 先看 `level -1[511]`
- 当前 init idmap 直接从 `level 0[256]` 开始

换句话说：

- 不是说地址没有高位
- 而是说这份 48-bit idmap 的页表遍历根本不会去消费位 `[56:48]` 作为额外一级索引
- 所以也就不需要单独再准备一页“level -1 root table”

### 3.4.3 那 `pgd_t *pg_dir` 为什么还能传进来

这里最容易混淆的是 Linux 的类型名和 ARM64 硬件层级不是一回事。

`create_init_idmap()` 的原型是：

```c
asmlinkage phys_addr_t __init create_init_idmap(pgd_t *pg_dir, ptdesc_t clrmask)
```

但在 [arch/arm64/kernel/pi/map_range.c](arch/arm64/kernel/pi/map_range.c#L102) 里它调用时是：

```c
map_range(..., IDMAP_ROOT_LEVEL, (pte_t *)pg_dir, ...)
```

也就是说：

- `pg_dir` 在 C 类型上叫 `pgd_t *`
- 但真正怎么解释这页内存，是由 `level` 参数决定的

对当前 init idmap：

- `level = IDMAP_ROOT_LEVEL = 0`

所以它的真实语义就是：

- 把 `pg_dir` 这一页当作 **L0 root table** 来填
- 然后按需继续分配 L1、L2、L3

因此正确理解应该是：

- 不是“没有根表”
- 而是“根表就是 L0”
- `pgd_t *` 在这里只是“root page pointer”的 Linux 类型名，并不强迫它一定对应硬件 `level -1`

### 3.4.4 用 Linux 常规命名再对照一次

在当前内核的常规 5 级配置里，名字和硬件层级大致对应成：

```text
PGD -> hardware level -1
P4D -> hardware level 0
PUD -> hardware level 1
PMD -> hardware level 2
PTE -> hardware level 3
```

而对这份 4 级 init idmap，更贴近实际的理解是：

- 传进来的 `pg_dir` 虽然类型叫 `pgd_t *`
- 但它在硬件意义上扮演的是 **L0 root table**

所以你在 GDB 里看到：

- 根页地址 `__pi_init_idmap_pg_dir`
- 第一个有效项是 `L0[256]`

这恰好说明：

- 它当然有根表
- 只是这个根表不是“再外包一层的 level -1 表”
- 而是直接从 L0 开始


## 4. 当前镜像的实际边界

本次分析基于当前 `vmlinux` 的实际符号地址：

- `_stext = 0xffff800080010000`
- `__initdata_begin = 0xffff800081540000`
- `_end = 0xffff800082330000`

因此整体映射范围是：

```text
0xffff800080010000 -> 0xffff800082330000
```


## 5. 最终映射关系

### 5.1 总体规律

这份 init idmap 不是纯 block 映射，也不是纯 page 映射，而是：

- 主体使用 **level 2 / 2MB PMD block**
- 三个特殊窗口使用 **level 3 / 4KB PTE**

这三个特殊窗口分别是：

1. 起始不对齐窗口
2. text/data 权限切换窗口
3. 尾部不对齐窗口


### 5.1.1 先看一张 2MB / 4KB 总图

如果只盯住这三个真实边界：

- `_stext = 0xffff800080010000`
- `__initdata_begin = 0xffff800081540000`
- `_end = 0xffff800082330000`

那么整份映射可以先按 2MB 窗口画成下面这样。

先用 `vmlinux` 链接符号地址视角看：

```text
2MB windows (linked-address view)

0xffff800080000000 - 0xffff800080200000  [ L2[0]  ]  split to 4KB PTE
	0xffff800080000000 - 0xffff800080010000  hole
	0xffff800080010000 - 0xffff800080200000  ROX
					 ^
					 _stext

0xffff800080200000 - 0xffff800081400000  [ L2[1]..L2[9] ]  ROX 2MB blocks

0xffff800081400000 - 0xffff800081600000  [ L2[10] ]  split to 4KB PTE
	0xffff800081400000 - 0xffff800081540000  ROX
	0xffff800081540000 - 0xffff800081600000  RW
					 ^
					 __initdata_begin

0xffff800081600000 - 0xffff800082200000  [ L2[11]..L2[16] ]  RW 2MB blocks

0xffff800082200000 - 0xffff800082400000  [ L2[17] ]  split to 4KB PTE
	0xffff800082200000 - 0xffff800082330000  RW
	0xffff800082330000 - 0xffff800082400000  hole
					 ^
					 _end
```

如果站在“运行时 idmap 数值地址”视角看，同一张图就是：

```text
2MB windows (runtime idmap view, VA = PA)

0x40200000 - 0x40400000  [ L2[0]  ]  split to 4KB PTE
	0x40200000 - 0x40210000  hole
	0x40210000 - 0x40400000  ROX

0x40400000 - 0x41600000  [ L2[1]..L2[9] ]   ROX 2MB blocks

0x41600000 - 0x41800000  [ L2[10] ]  split to 4KB PTE
	0x41600000 - 0x41740000  ROX
	0x41740000 - 0x41800000  RW

0x41800000 - 0x42400000  [ L2[11]..L2[16] ] RW 2MB blocks

0x42400000 - 0x42600000  [ L2[17] ]  split to 4KB PTE
	0x42400000 - 0x42530000  RW
	0x42530000 - 0x42600000  hole
```

这张图已经把核心结构说完了：

- 只有 3 个 2MB 窗口不能直接 block 映射
- 其余中间大段都直接压成 2MB PMD block


### 5.1.2 再把 3 个特殊窗口按 4KB 页展开

上面那张图只说明“哪几个 2MB 窗口被拆开了”，下面再把这 3 个窗口内部按 4KB 页看一遍。

```text
L2[0]  = 0x40200000 - 0x40400000
	pages   0 ..  15   invalid
	pages  16 .. 511   ROX

L2[10] = 0x41600000 - 0x41800000
	pages   0 .. 319   ROX
	pages 320 .. 511   RW

L2[17] = 0x42400000 - 0x42600000
	pages   0 .. 303   RW
	pages 304 .. 511   invalid
```

如果换回你熟悉的高地址符号边界，就是：

```text
L2[0]  : 0xffff800080000000 - 0xffff800080200000
	0xffff800080000000 - 0xffff800080010000  invalid
	0xffff800080010000 - 0xffff800080200000  ROX

L2[10] : 0xffff800081400000 - 0xffff800081600000
	0xffff800081400000 - 0xffff800081540000  ROX
	0xffff800081540000 - 0xffff800081600000  RW

L2[17] : 0xffff800082200000 - 0xffff800082400000
	0xffff800082200000 - 0xffff800082330000  RW
	0xffff800082330000 - 0xffff800082400000  invalid
```

所以最简洁的总结就是：

- `_stext` 把第一个 2MB 窗口切成了 `hole + ROX`
- `__initdata_begin` 把中间那个 2MB 窗口切成了 `ROX + RW`
- `_end` 把最后一个 2MB 窗口切成了 `RW + hole`


### 5.2 逐段展开

#### 1) 起始窗口

```text
0xffff800080000000 - 0xffff800080200000
```

其中：

- `0xffff800080000000 - 0xffff800080010000`：无映射
- `0xffff800080010000 - 0xffff800080200000`：同地址 PA，`ROX`，L3 PTE

原因：`_stext` 不在 2MB 边界上。


#### 2) text 主体

```text
0xffff800080200000 - 0xffff800081400000
```

映射关系：

- 同地址 PA，`ROX`，L2 PMD block

一共 9 个 2MB block。


#### 3) 权限切换窗口

```text
0xffff800081400000 - 0xffff800081600000
```

映射关系：

- `0xffff800081400000 - 0xffff800081540000`：同地址 PA，`ROX`，L3 PTE
- `0xffff800081540000 - 0xffff800081600000`：同地址 PA，`RW`，L3 PTE

原因：这一整个 2MB 窗口中间恰好跨过 `__initdata_begin`，同一个 PMD block 无法同时承载两种权限，只能拆成 4KB PTE。


#### 4) data 主体

```text
0xffff800081600000 - 0xffff800082200000
```

映射关系：

- 同地址 PA，`RW`，L2 PMD block

一共 6 个 2MB block。


#### 5) 末尾窗口

```text
0xffff800082200000 - 0xffff800082400000
```

其中：

- `0xffff800082200000 - 0xffff800082330000`：同地址 PA，`RW`，L3 PTE
- `0xffff800082330000 - 0xffff800082400000`：无映射

原因：`_end` 不在 2MB 边界上。


### 5.3 最终可直接引用的映射表

```text
0xffff800080010000 - 0xffff800080200000  -> same PA, ROX, L3 PTE
0xffff800080200000 - 0xffff800081400000  -> same PA, ROX, L2 PMD block
0xffff800081400000 - 0xffff800081540000  -> same PA, ROX, L3 PTE
0xffff800081540000 - 0xffff800081600000  -> same PA, RW,  L3 PTE
0xffff800081600000 - 0xffff800082200000  -> same PA, RW,  L2 PMD block
0xffff800082200000 - 0xffff800082330000  -> same PA, RW,  L3 PTE
```

这里这张表是从“`vmlinux` 链接符号视角”写的，目的是方便你把 `_stext`、`__initdata_begin`、`_end` 这些高地址符号边界和页表结构对起来。

但如果只谈“这份 init idmap 运行时到底映射了什么数值地址”，则更准确的说法是：

- 它映射的是当前 kernel image 所在的那段物理地址区间
- 对这段 idmap 来说，运行时满足 **数值上的 VA = PA**


### 5.4 把 3 个特殊 2MB 窗口按 4KB 展开

如果完全站在“运行时 idmap 数值视角”去看，3 个特殊 2MB 窗口分别是：

```text
0x40200000 - 0x40400000   起始不对齐窗口
0x41600000 - 0x41800000   text/data 权限切换窗口
0x42400000 - 0x42600000   尾部不对齐窗口
```

它们之所以不能直接用 2MB block，是因为：

- 第一个窗口前面有 hole
- 第二个窗口中间切权限
- 第三个窗口后面有 hole

下面把它们按 4KB 页展开。

#### 5.4.1 起始不对齐窗口：`0x40200000 - 0x40400000`

这一整个 2MB 窗口对应 L2[0]，但真正有效的映射不是从窗口起点开始，而是从 `_stext` 所在页开始：

```text
L3 index 0..15    -> invalid
L3 index 16..511  -> ROX page mapping
```

也就是：

```text
0x40200000 - 0x40210000   invalid
0x40210000 - 0x40400000   ROX, 4KB PTE, VA = PA
```

这里的 `0x40210000` 就是当前 build 下 `_stext` 的运行时数值地址。


#### 5.4.2 权限切换窗口：`0x41600000 - 0x41800000`

这一整个 2MB 窗口对应 L2[10]。它没有地址对齐问题，但中间在 `__initdata_begin` 处分成了两种权限，所以只能拆成 4KB PTE：

```text
L3 index 0..319    -> ROX page mapping
L3 index 320..511  -> RW  page mapping
```

也就是：

```text
0x41600000 - 0x41740000   ROX, 4KB PTE, VA = PA
0x41740000 - 0x41800000   RW,  4KB PTE, VA = PA
```

这里的关键分界点是：

```text
0x41740000 = __initdata_begin 的运行时数值地址
```


#### 5.4.3 尾部不对齐窗口：`0x42400000 - 0x42600000`

这一整个 2MB 窗口对应 L2[17]。它的前半段有效，后半段超出了 `_end`，所以也必须拆成 4KB PTE：

```text
L3 index 0..303    -> RW page mapping
L3 index 304..511  -> invalid
```

也就是：

```text
0x42400000 - 0x42530000   RW, 4KB PTE, VA = PA
0x42530000 - 0x42600000   invalid
```

这里的 `0x42530000` 就是当前 build 下 `_end` 的运行时数值地址。


#### 5.4.4 中间那些 2MB block 具体落在哪些窗口

除了这 3 个特殊窗口外，其余都直接用 L2 block：

```text
L2[1]  -> 0x40400000 - 0x40600000   ROX block
L2[2]  -> 0x40600000 - 0x40800000   ROX block
L2[3]  -> 0x40800000 - 0x40a00000   ROX block
L2[4]  -> 0x40a00000 - 0x40c00000   ROX block
L2[5]  -> 0x40c00000 - 0x40e00000   ROX block
L2[6]  -> 0x40e00000 - 0x41000000   ROX block
L2[7]  -> 0x41000000 - 0x41200000   ROX block
L2[8]  -> 0x41200000 - 0x41400000   ROX block
L2[9]  -> 0x41400000 - 0x41600000   ROX block

L2[11] -> 0x41800000 - 0x41a00000   RW block
L2[12] -> 0x41a00000 - 0x41c00000   RW block
L2[13] -> 0x41c00000 - 0x41e00000   RW block
L2[14] -> 0x41e00000 - 0x42000000   RW block
L2[15] -> 0x42000000 - 0x42200000   RW block
L2[16] -> 0x42200000 - 0x42400000   RW block
```

所以把整份运行时 idmap 压缩成一句话，就是：

- 大部分是 2MB block
- 只有 `0x40200000-0x40400000`、`0x41600000-0x41800000`、`0x42400000-0x42600000` 这 3 个窗口退化成 4KB PTE


## 6. 页表页的分配结果

对当前镜像，这次 `create_init_idmap()` 实际消耗了 6 页页表页：

```text
__pi_init_idmap_pg_dir + 0x0000 : L0 root
__pi_init_idmap_pg_dir + 0x1000 : L1 table
__pi_init_idmap_pg_dir + 0x2000 : L2 table
__pi_init_idmap_pg_dir + 0x3000 : L3 table for L2[0]
__pi_init_idmap_pg_dir + 0x4000 : L3 table for L2[10]
__pi_init_idmap_pg_dir + 0x5000 : L3 table for L2[17]
return ptep = __pi_init_idmap_pg_dir + 0x6000
```

也就是说：

- 1 页 L0
- 1 页 L1
- 1 页 L2
- 3 页 L3


## 7. 本次调试得到的物理布局

GDB 已验证当前构建下的 init idmap 物理布局如下：

```text
pg_dir   : 0x41740000
l1 table : 0x41741000
l2 table : 0x41742000
l3 head  : 0x41743000
l3 mid   : 0x41744000
l3 tail  : 0x41745000
pg_end   : 0x41748000
```

因此返回值预期应为：

```text
0x41746000 = 0x41740000 + 0x6000
```

这与实际 GDB 观察一致，说明页表页分配数量正确。


### 7.1 先区分两种视角：运行时 idmap 视角 vs 链接符号视角

这里非常容易混淆，必须先把两种“地址说法”分开。

#### 7.1.1 运行时 idmap 视角

如果只问：

- CPU 在打开 MMU 后通过这份 init idmap 实际访问的是什么地址？

那么答案是：

- 访问的是当前 kernel image 所在的那段物理地址
- 对这份 idmap 本身来说，**数值上的 VA = PA**

例如当前 build：

```text
0x40210000 -> 0x40210000
0x41740000 -> 0x41740000
0x4252f000 -> 0x4252f000
```

这才是“identity map”最严格的含义。

#### 7.1.2 链接符号 / GDB 分析视角

但你在源码、`nm vmlinux`、`objdump`、GDB 符号里看到的 `_stext`、`__initdata_begin`、`_end`，对应的是内核镜像的链接地址视角，例如：

```text
_stext = 0xffff800080010000
__initdata_begin = 0xffff800081540000
_end = 0xffff800082330000
```

为了把这些高地址符号和当前物理装载位置对应起来，调试时才会用到下面这个换算：

```text
PA = 0x40200000 + (VA - 0xffff800080000000)
```

这条公式的作用只是：

- 把 `vmlinux` 的链接地址符号换算成当前机器上实际装载的物理地址
- 方便你做 GDB 符号分析

它不是在否定 idmap，也不是说这份页表真的在做“高 VA -> 不同 PA”的普通内核映射。

更准确地说：

- idmap 的运行时数值地址是低地址物理区间，满足 `VA = PA`
- 文档里出现的 `0xffff8000...` 是为了和 `vmlinux` 符号对齐而引入的“链接视角”

对你当前这次 QEMU/GDB 环境，内核镜像的物理装载基址是：

```text
phys_base = 0x40200000
virt_base = 0xffff800080000000
```

因此在“链接符号视角”里，高地址符号到当前物理装载地址的对应关系可以直接写成：

```text
PA = 0x40200000 + (VA - 0xffff800080000000)
```

把前面的 6 段边界代入后，得到更直观的“链接地址边界 -> 当前物理地址边界”对照表：

```text
VA 0xffff800080010000 - 0xffff800080200000  -> PA 0x40210000 - 0x40400000  ROX  L3 PTE
VA 0xffff800080200000 - 0xffff800081400000  -> PA 0x40400000 - 0x41600000  ROX  L2 PMD block
VA 0xffff800081400000 - 0xffff800081540000  -> PA 0x41600000 - 0x41740000  ROX  L3 PTE
VA 0xffff800081540000 - 0xffff800081600000  -> PA 0x41740000 - 0x41800000  RW   L3 PTE
VA 0xffff800081600000 - 0xffff800082200000  -> PA 0x41800000 - 0x42400000  RW   L2 PMD block
VA 0xffff800082200000 - 0xffff800082330000  -> PA 0x42400000 - 0x42530000  RW   L3 PTE
```

如果只抓住一句话，那就是：

- 对当前 build，这份 idmap 运行时映射的是当前物理装载区间本身
- 文档里这张高地址到低地址的表，只是为了把 `vmlinux` 符号边界翻译成当前机器上的实际物理位置


### 7.2 这几张页表页里具体有什么

下面按“哪一页表里有哪些有效项”来展开。

#### 7.2.1 L0 根表：只有 `L0[256]` 有效

L0 根表物理地址：

```text
0x41740000
```

它里面只有一个有效项：

```text
L0[256] = table -> 0x41741000
raw desc = 0x1000000041741003
```

其余项都应该是 `invalid`。

这表示：

- 整个映射范围都落在同一个 L0 项里
- 下一层表在 `0x41741000`


#### 7.2.2 L1 表：只有 `L1[2]` 有效

L1 表物理地址：

```text
0x41741000
```

它里面也只有一个有效项：

```text
L1[2] = table -> 0x41742000
raw desc = 0x1000000041742003
```

其余项都应该是 `invalid`。

这表示：

- 整个映射范围都落在同一个 1GB 窗口里
- 具体的 2MB 切分都在下面的 L2 表完成


#### 7.2.3 L2 表：3 个 table 项 + 中间两大段 block 项

L2 表物理地址：

```text
0x41742000
```

它的有效项分布是：

```text
L2[0]   = table -> 0x41743000   raw = 0x1000000041743003
L2[1]   = ROX block             covers VA 0xffff800080200000 - 0xffff800080400000
...
L2[9]   = ROX block             covers VA 0xffff800081200000 - 0xffff800081400000
L2[10]  = table -> 0x41744000   raw = 0x1000000041744003
L2[11]  = RW  block             covers VA 0xffff800081600000 - 0xffff800081800000
...
L2[16]  = RW  block             covers VA 0xffff800082000000 - 0xffff800082200000
L2[17]  = table -> 0x41745000   raw = 0x1000000041745003
```

其余项都应该是 `invalid`。

这张表已经完整反映了整个 idmap 的策略：

- `0` 号窗口因为起始地址不按 2MB 对齐，下钻到 L3
- `1..9` 压成 ROX PMD block
- `10` 号窗口因为中间切权限，下钻到 L3
- `11..16` 压成 RW PMD block
- `17` 号窗口因为 `_end` 不按 2MB 对齐，下钻到 L3


#### 7.2.4 第一个 L3 表：起始不对齐窗口

L3 head 表物理地址：

```text
0x41743000
```

内容分布：

```text
idx 0..15    invalid
idx 16..511  valid ROX PTE
```

它覆盖的映射是：

```text
VA 0xffff800080010000 - 0xffff800080200000
PA 0x40210000 - 0x40400000
```

也就是说：

- `L3[15]` 还是 hole
- `L3[16]` 才是 `_stext` 对应的第一页


#### 7.2.5 第二个 L3 表：权限切换窗口

L3 mid 表物理地址：

```text
0x41744000
```

内容分布：

```text
idx 0..319    valid ROX PTE
idx 320..511  valid RW  PTE
```

它覆盖的映射是：

```text
VA 0xffff800081400000 - 0xffff800081540000  -> PA 0x41600000 - 0x41740000  ROX
VA 0xffff800081540000 - 0xffff800081600000  -> PA 0x41740000 - 0x41800000  RW
```

这里最关键的分界点就是：

```text
L3[320] 对应 __initdata_begin
```

所以：

- `L3[319]` 还是 ROX
- `L3[320]` 开始就变成 RW


#### 7.2.6 第三个 L3 表：尾部不对齐窗口

L3 tail 表物理地址：

```text
0x41745000
```

内容分布：

```text
idx 0..303    valid RW PTE
idx 304..511  invalid
```

它覆盖的映射是：

```text
VA 0xffff800082200000 - 0xffff800082330000
PA 0x42400000 - 0x42530000
```

也就是说：

- `L3[303]` 还是最后一个有效页
- `L3[304]` 就已经越过 `_end`，因此必须是 `invalid`


### 7.3 关于 raw PTE/block 值应该怎么看

上面给出的 table descriptor raw value 是固定可直接核对的：

```text
table desc = next_table_phys | 0x3 | UXN(table)
```

而 L2 block 和 L3 PTE 的 raw value 除了物理地址以外，还带权限和属性位：

- `ROX` 对应 `PAGE_KERNEL_ROX`
- `RW` 对应 `PAGE_KERNEL`

这些位最终来自 [arch/arm64/include/asm/pgtable-prot.h](arch/arm64/include/asm/pgtable-prot.h#L47) 之后的定义。

对调试来说，更实用的做法通常不是手算每一个十六进制常量，而是：

- 先验证 table 项是否指向了正确的下一页表物理地址
- 再验证 block/PTE 是否覆盖了正确的 VA/PA 范围
- 最后只在需要时再对比 `ROX` 和 `RW` 项的 raw bits 差异


## 8. GDB 验证步骤

### 8.1 停在 `create_init_idmap()`

```gdb
source ~/.gdbinit
mmu_off_setup
hbreak *$pa_pi_create_init_idmap
c
wherepc
```


### 8.2 验证返回值

```gdb
finish
p/x $x0
p/x $pa_pi_init_idmap_pg_dir
p/x $x0 - $pa_pi_init_idmap_pg_dir
```

预期：

```text
$x0 - $pa_pi_init_idmap_pg_dir = 0x6000
```


### 8.3 验证 L0 / L1 / L2 骨架

```gdb
x/gx $pa_pi_init_idmap_pg_dir + 8*256
x/gx $pa_pi_init_idmap_l1 + 8*2
x/18gx $pa_pi_init_idmap_l2
```

预期：

- `L0[256]` 是 table
- `L1[2]` 是 table
- `L2[0]` 是 table
- `L2[1..9]` 是 block
- `L2[10]` 是 table
- `L2[11..16]` 是 block
- `L2[17]` 是 table


### 8.4 验证第一个 L3：起始不对齐窗口

```gdb
x/gx $pa_pi_init_idmap_l3_head + 8*15
x/gx $pa_pi_init_idmap_l3_head + 8*16
```

预期：

- `15`：invalid
- `16`：valid page


### 8.5 验证第二个 L3：权限切换窗口

```gdb
x/gx $pa_pi_init_idmap_l3_mid + 8*319
x/gx $pa_pi_init_idmap_l3_mid + 8*320
```

预期：

- `319`：ROX PTE
- `320`：RW PTE

这一步是验证“中间不是地址不齐，而是权限切换导致下钻”的关键。


### 8.6 验证第三个 L3：尾部不对齐窗口

```gdb
x/gx $pa_pi_init_idmap_l3_tail + 8*303
x/gx $pa_pi_init_idmap_l3_tail + 8*304
```

预期：

- `303`：valid page
- `304`：invalid


### 8.7 直接查看各张页表页的 raw 内容

如果你想直接看“页表里到底写了什么”，可以按下面顺序 dump。

先看 L0 / L1 / L2 的关键项：

```gdb
x/gx $pa_pi_init_idmap_pg_dir + 8*256
x/gx $pa_pi_init_idmap_l1 + 8*2
x/20gx $pa_pi_init_idmap_l2
```

当前 build 下，table descriptor 预期值应该分别是：

```text
L0[256] = 0x1000000041741003
L1[2]   = 0x1000000041742003
L2[0]   = 0x1000000041743003
L2[10]  = 0x1000000041744003
L2[17]  = 0x1000000041745003
```

如果想把 table descriptor 里指向的下一层表物理地址直接抠出来，可以这样看：

```gdb
p/x (*(unsigned long long *)($pa_pi_init_idmap_pg_dir + 8*256)) & 0x0000fffffffff000
p/x (*(unsigned long long *)($pa_pi_init_idmap_l1 + 8*2)) & 0x0000fffffffff000
p/x (*(unsigned long long *)($pa_pi_init_idmap_l2 + 8*0)) & 0x0000fffffffff000
p/x (*(unsigned long long *)($pa_pi_init_idmap_l2 + 8*10)) & 0x0000fffffffff000
p/x (*(unsigned long long *)($pa_pi_init_idmap_l2 + 8*17)) & 0x0000fffffffff000
```

预期分别得到：

```text
0x41741000
0x41742000
0x41743000
0x41744000
0x41745000
```


### 8.8 按页表页验证每一段映射

先验证起始 L3：

```gdb
x/20gx $pa_pi_init_idmap_l3_head
x/gx $pa_pi_init_idmap_l3_head + 8*15
x/gx $pa_pi_init_idmap_l3_head + 8*16
```

预期：

- `0..15` 都应该是 `0`
- `16` 开始变成有效 ROX PTE

再验证中间权限切换 L3：

```gdb
x/8gx $pa_pi_init_idmap_l3_mid + 8*316
x/gx $pa_pi_init_idmap_l3_mid + 8*319
x/gx $pa_pi_init_idmap_l3_mid + 8*320
```

预期：

- `319` 有效，且仍是 ROX
- `320` 有效，但 raw bits 会变成 RW

最后验证尾部 L3：

```gdb
x/8gx $pa_pi_init_idmap_l3_tail + 8*300
x/gx $pa_pi_init_idmap_l3_tail + 8*303
x/gx $pa_pi_init_idmap_l3_tail + 8*304
```

预期：

- `303` 还是有效 RW PTE
- `304` 开始清零


### 8.9 验证 VA / PA 对应关系

如果想把“表项里的物理地址”和“前面算出的 VA/PA 对照关系”对起来，可以抓几个代表性页做 spot check。

例如检查：

- `_stext` 对应页
- `__initdata_begin` 前后两页
- `_end` 前最后一页

命令可以写成：

```gdb
set $pte_stext = *(unsigned long long *)($pa_pi_init_idmap_l3_head + 8*16)
set $pte_mid_rox = *(unsigned long long *)($pa_pi_init_idmap_l3_mid + 8*319)
set $pte_mid_rw = *(unsigned long long *)($pa_pi_init_idmap_l3_mid + 8*320)
set $pte_tail = *(unsigned long long *)($pa_pi_init_idmap_l3_tail + 8*303)

p/x $pte_stext & 0x0000fffffffff000
p/x $pte_mid_rox & 0x0000fffffffff000
p/x $pte_mid_rw & 0x0000fffffffff000
p/x $pte_tail & 0x0000fffffffff000
```

当前 build 下预期应分别得到：

```text
0x40210000
0x4173f000
0x41740000
0x4252f000
```

它们分别对应：

- `_stext` 所在第一页
- `__initdata_begin` 前最后一页
- `__initdata_begin` 本身所在第一页
- `_end` 前最后一页


### 8.7 为什么正好是 `L0[256] / L1[2] / L2[0,10,17]`

对当前 4KB granule：

- 每级页表索引宽度都是 9 bit
- page offset 是 12 bit

所以各级索引可以直接按下面的位段取：

```text
L0 index = (va >> 39) & 0x1ff
L1 index = (va >> 30) & 0x1ff
L2 index = (va >> 21) & 0x1ff
L3 index = (va >> 12) & 0x1ff
```

对这次 idmap 的三个关键边界做计算：

```text
_stext            = 0xffff800080010000 -> L0[256], L1[2], L2[0],  L3[16]
__initdata_begin  = 0xffff800081540000 -> L0[256], L1[2], L2[10], L3[320]
_end - 1          = 0xffff80008232ffff -> L0[256], L1[2], L2[17], L3[303]
```

这就把前面看到的 GDB 结果完全解释通了：

- 整个映射范围都落在同一个 `L0[256]`
- 整个映射范围也都落在同一个 `L1[2]`
- 起始碎片从 `L2[0]` 开始
- 权限切换发生在 `L2[10]` 这个 2MB 窗口里
- 尾部碎片落在 `L2[17]`

所以最终你会看到：

- `L2[0]` 是 table，因为 `_stext` 不按 2MB 对齐
- `L2[1..9]` 是 block，因为这部分可以完整压成 2MB PMD block
- `L2[10]` 是 table，因为 `__initdata_begin` 在这个 2MB 窗口中间切换权限
- `L2[11..16]` 是 block，因为这部分又恢复成完整 2MB RW block
- `L2[17]` 是 table，因为 `_end` 没有落在 2MB 边界上

也可以换一个更直观的理解方式：

- `L1[2]` 对应一个 1GB 窗口
- 这次映射全都落在这个 1GB 窗口内部
- 在这个窗口里，按 2MB 为单位编号，范围正好从编号 `0` 延伸到编号 `17`
- 其中只有 `0`、`10`、`17` 三个窗口不能直接整块映射

这正是前面“三个 L3 窗口 + 中间大段 PMD block”的来源。


## 9. 最后的结论

`create_init_idmap()` 在当前构建下创建的是一份 **identity map**，其核心特点是：

1. 从 level 0 开始建表
2. 主要使用 **level 2 / 2MB PMD block**
3. 只有 3 个特殊 2MB 窗口下钻到 **level 3 / 4KB PTE**
4. text 区映射为 `ROX`
5. data 区映射为 `RW`

所以更准确的一句话是：

> `create_init_idmap()` 不是简单地“把内核全部做成段映射”，而是“用 2MB PMD block 作为主体，在起始、权限切换、尾部三个特殊窗口退化为 4KB PTE”，从而为 MMU 打开前后的过渡提供最小且正确的 identity map。


## 10. Section 边界与映射窗口的对应关系

为了把链接脚本和页表结果对起来，可以把几个关键边界列出来：

- `_stext = 0xffff800080010000`
- `_etext = 0xffff800080f00000`
- `__init_begin = 0xffff800081460000`
- `__inittext_begin = 0xffff800081460000`
- `__inittext_end = 0xffff800081540000`
- `__initdata_begin = 0xffff800081540000`
- `__pi_init_idmap_pg_dir = 0xffff800081540000`
- `__pi_init_idmap_pg_end = 0xffff800081548000`
- `__initdata_end = 0xffff800082120000`
- `_data = 0xffff800082120000`
- `_end = 0xffff800082330000`

结合前面的 6 段映射，可以得到下面这个更贴近源码布局的视图。

### 10.1 `ROX` 区间覆盖了哪些 section

```text
0xffff800080010000 - 0xffff800080200000   L3 PTE   ROX
0xffff800080200000 - 0xffff800081400000   L2 PMD   ROX
0xffff800081400000 - 0xffff800081540000   L3 PTE   ROX
```

这几段合起来正好覆盖：

- `.text` 的后半段到 `_etext`
- RO data 区
- `.rodata.text`
- `reserved_pg_dir`
- `swapper_pg_dir`
- `.init.text`
- `.exit.text`
- `.altinstructions`

也就是说，从 `_stext` 到 `__initdata_begin` 之前，整片都被视为“text 类权限区”，统一映射成 `ROX`。


### 10.2 `RW` 区间覆盖了哪些 section

```text
0xffff800081540000 - 0xffff800081600000   L3 PTE   RW
0xffff800081600000 - 0xffff800082200000   L2 PMD   RW
0xffff800082200000 - 0xffff800082330000   L3 PTE   RW
```

这几段覆盖：

- `__pi_init_idmap_pg_dir` 预留区
- `.init.data`
- `__initdata_end` 之后的普通 `.data`
- `.bss`
- 直到 `_end`

其中最值得单独记住的是：

- `__pi_init_idmap_pg_dir` 本身位于 `RW` 区间
- 也就是说，页表内存自己也被 identity map 成了可写数据区


### 10.3 一个最简洁的心智图

```text
_stext
	│
	├─ L3 ROX  : 起始不对齐碎片
	├─ L2 ROX  : text / rodata / rodata.text 主体
	├─ L3 ROX  : __initdata_begin 前的尾段
	├─ L3 RW   : __initdata_begin 后的头段
	├─ L2 RW   : init.data / data / bss 主体
	└─ L3 RW   : _end 前的尾碎片
```

这张图最能反映 `create_init_idmap()` 的真实策略：

- 按权限分两大段
- 每段内部尽量压成 2MB block
- 碰到边界或权限切换时退化成 4KB page


## 11. `vmlinux.lds.S` 里这些页表保留区分别是干什么的

你摘出来的这一段，其实正好把 ARM64 early boot 里几类最关键的页表都排在了一起：

```text
idmap_pg_dir
tramp_pg_dir
reserved_pg_dir
swapper_pg_dir
__init_begin / __inittext_begin
...
__initdata_begin
__pi_init_idmap_pg_dir
__pi_init_idmap_pg_end
```

它们都不是“普通数据变量”，而是链接脚本里直接预留出来、供早期启动代码按固定地址访问的页表内存。

### 11.1 先看它们在镜像里的相对位置

在 [arch/arm64/kernel/vmlinux.lds.S](arch/arm64/kernel/vmlinux.lds.S#L223) 这一带，布局顺序是：

```text
.rodata.text
	-> idmap_pg_dir
	-> tramp_pg_dir        (可选，CONFIG_UNMAP_KERNEL_AT_EL0)
	-> reserved_pg_dir
	-> swapper_pg_dir

ALIGN(SEGMENT_ALIGN)
	-> __init_begin
	-> __inittext_begin
	-> .init.text / .exit.text / .altinstructions / unwind

ALIGN(SEGMENT_ALIGN)
	-> __inittext_end
	-> __initdata_begin
	-> __pi_init_idmap_pg_dir
	-> __pi_init_idmap_pg_end
	-> .init.data ...
```

这里有两个很关键的分界：

- 第一组 `idmap_pg_dir/reserved_pg_dir/swapper_pg_dir` 位于 `__init_begin` 之前，属于早期就要直接拿来切换 MMU 的静态页表根页
- 第二组 `__pi_init_idmap_pg_dir .. __pi_init_idmap_pg_end` 位于 `__initdata_begin` 之后，是给 PI/early C 代码动态“现填现用”的一整段页表工作区

换句话说：

- 前者更像“固定根页”
- 后者更像“启动阶段临时构表池”


### 11.2 `idmap_pg_dir`：常规 idmap 的根页

`idmap_pg_dir` 是内核正式 early MM 初始化阶段使用的 **identity map 根表**。

它主要服务于：

- `.idmap.text` 里的代码执行
- CPU suspend/resume、secondary boot、某些 TTBR 切换场景
- `cpu_install_idmap()` 这类需要临时把 TTBR0 指到 idmap 的路径

相关代码可以直接看到：

- [arch/arm64/kernel/head.S](arch/arm64/kernel/head.S#L374) 次级 CPU 启动时把 `idmap_pg_dir` 作为 `__enable_mmu()` 的 `x2`
- [arch/arm64/mm/mmu.c](arch/arm64/mm/mmu.c#L1331) `create_idmap()` 用 `idmap_pg_dir` 填这份常规 idmap
- [arch/arm64/include/asm/mmu_context.h](arch/arm64/include/asm/mmu_context.h#L118) `cpu_install_idmap()` 会切到 `idmap_pg_dir`

它映射的不是整个内核镜像，而是主要保证 `.idmap.text` 和必要辅助对象能在切换 MMU 时被 VA=PA 地访问。


### 11.3 `tramp_pg_dir`：entry trampoline 的专用页表

`tramp_pg_dir` 只在 `CONFIG_UNMAP_KERNEL_AT_EL0` 打开时存在。

它的用途是：

- 为 entry trampoline 建一份极小的专用映射
- 在异常入口/返回时，在 `tramp_pg_dir` 和 `swapper_pg_dir` 之间切换

对应代码：

- [arch/arm64/mm/mmu.c](arch/arm64/mm/mmu.c#L1284) `map_entry_trampoline()` 先 `memset(tramp_pg_dir, 0, PGD_SIZE)` 再建立 trampoline 映射
- [arch/arm64/kernel/entry.S](arch/arm64/kernel/entry.S#L621) 和 [arch/arm64/kernel/entry.S](arch/arm64/kernel/entry.S#L641) 明确写着在 `tramp_pg_dir` 与 `swapper_pg_dir` 之间切换

这份页表不是给普通内核执行路径用的，而是给 KPTI/trampoline 这一小段过渡代码用的。


### 11.4 `reserved_pg_dir`：故意让 TTBR0“什么都翻译不了”的保留根页

`reserved_pg_dir` 的作用非常特殊：

- 它不是为了“映射什么”
- 恰恰相反，它是为了让 TTBR0 暂时 **不提供有效翻译**

源码里写得很直白：

- [arch/arm64/include/asm/mmu_context.h](arch/arm64/include/asm/mmu_context.h#L41) `Set TTBR0 to reserved_pg_dir. No translations will be possible via TTBR0.`

这里最关键的一点是：

- 内核里**没有**像 `create_init_idmap()`、`map_kernel()`、`__create_pgd_mapping()` 这样专门给 `reserved_pg_dir` 建一份有效映射表。

也就是说，`reserved_pg_dir` 的设计目标就不是“存放一套可用映射”，而是：

- 预留一张页表根页
- 让它保持为空表/零表
- 在需要隔离 TTBR0 或 TTBR1 时，把寄存器先切到这张空根页表上

从链接脚本 [arch/arm64/kernel/vmlinux.lds.S](arch/arm64/kernel/vmlinux.lds.S#L231) 也能看出来，它只是被简单预留了 1 页：

```text
reserved_pg_dir = .;
. += PAGE_SIZE;
```

而我检查了当前代码路径，也没有找到任何针对 `reserved_pg_dir` 的显式建表或写表操作。

所以更准确的理解是：

- `idmap_pg_dir` / `__pi_init_idmap_pg_dir` / `init_pg_dir` / `swapper_pg_dir` 这些都会被真正填入页表项
- `reserved_pg_dir` 不承担这个职责，它本质上就是“合法但空”的根页表

它主要用于：

- 在安装/卸载 idmap 前后，先把 TTBR0 切到一个安全的空表
- 避免 TLB 和不同 `TCR_EL1.T0SZ` 配置之间发生投机或残留转换干扰

对应路径：

- [arch/arm64/kernel/head.S](arch/arm64/kernel/head.S#L509) `__primary_switch` 打开 MMU 时，TTBR1 传的是 `reserved_pg_dir`
- [arch/arm64/include/asm/mmu_context.h](arch/arm64/include/asm/mmu_context.h#L104) `cpu_uninstall_idmap()` 和 [arch/arm64/include/asm/mmu_context.h](arch/arm64/include/asm/mmu_context.h#L115) `cpu_install_idmap()` 都先切到 `reserved_pg_dir`

所以可以把它理解成：

- 一个“空的、占位的、隔离用”的 TTBR0 根页
- 用来在关键切换点把用户半边地址空间暂时封住


### 11.5 `swapper_pg_dir`：内核正式线性映射/内核映射的根页

`swapper_pg_dir` 是 ARM64 内核最核心的正式内核页表根页之一。

它承载的是：

- 内核镜像映射
- 线性映射（linear map）
- 后续绝大多数 kernel VA 访问

对应路径：

- [arch/arm64/mm/mmu.c](arch/arm64/mm/mmu.c#L1353) `paging_init()` 通过 `map_mem(swapper_pg_dir)` 建立正式 kernel 映射
- [arch/arm64/kernel/pi/map_kernel.c](arch/arm64/kernel/pi/map_kernel.c#L136) 会把初始页表复制/转移到 `swapper_pg_dir`
- [arch/arm64/kernel/head.S](arch/arm64/kernel/head.S#L374) 次级 CPU 在 `__enable_mmu()` 中把 `swapper_pg_dir` 装入 TTBR1

简化理解：

- `idmap_pg_dir` 负责“过桥”
- `swapper_pg_dir` 才是“正式长期居住的主内核页表”


### 11.6 `__pi_init_idmap_pg_dir .. __pi_init_idmap_pg_end`：primary boot 专用的早期构表工作区

这是你这次分析的主角。

它和前面的 `idmap_pg_dir` 最大的区别是：

- `idmap_pg_dir` 是一个固定的根页符号
- `__pi_init_idmap_pg_dir .. __pi_init_idmap_pg_end` 是一整段连续页表内存池

在 [arch/arm64/kernel/head.S](arch/arm64/kernel/head.S#L92) 里，`primary_entry` 会把 `__pi_init_idmap_pg_dir` 传给 `__pi_create_init_idmap`。随后：

- 根页放在 `__pi_init_idmap_pg_dir`
- 后续新分配的 L1/L2/L3 页从后面的连续空间里顺延切出来
- 返回值 `ptep` 指向已使用区末尾

所以这段内存不是只放 1 页根表，而是要容纳：

- L0 root
- 若干 L1/L2/L3 subordinate tables

这就是为什么链接脚本这里不是简单 `+ PAGE_SIZE`，而是：

```text
. += INIT_IDMAP_DIR_SIZE
```

因为它要预留的是一整段“可能增长”的 init idmap 构表空间。


### 11.7 为什么 `__pi_init_idmap_pg_dir` 放在 `__initdata_begin` 后面

这是因为它在权限上更像数据而不是代码：

- `create_init_idmap()` 会在 MMU off 阶段直接往这里写页表项
- 这块内存本身也需要被映射成 `RW`

所以它被布置在：

- `__initdata_begin`

之后，并且正好成为前面那条权限切换边界：

- `[_stext, __initdata_begin)` -> `ROX`
- `[__initdata_begin, _end)` -> `RW`

这也是为什么中间那个特殊 2MB 窗口必须拆成 L3：

- 窗口前半段还是 `ROX`
- 从 `__initdata_begin` 开始就变成 `RW`
- 而 `__pi_init_idmap_pg_dir` 正好就落在这个切换点上


### 11.8 把这些页表根页和映射场景放在一张表里看

```text
符号                     主要用途
idmap_pg_dir             常规 idmap 根页，服务 .idmap.text / TTBR0 过渡
tramp_pg_dir             entry trampoline 专用页表（KPTI）
reserved_pg_dir          TTBR0 的空/保留根页，切换时隔离用
swapper_pg_dir           正式内核页表根页，承载 kernel/linear map
__pi_init_idmap_pg_dir   primary boot 期间动态构造 init idmap 的根页
__pi_init_idmap_pg_end   上述构表工作区的末尾
```

如果只记一句话，可以记成：

- `swapper_pg_dir` 是正式内核页表
- `idmap_pg_dir` 是常规过渡 idmap
- `reserved_pg_dir` 是切换时的空占位页表
- `tramp_pg_dir` 是 trampoline 专用页表
- `__pi_init_idmap_pg_dir` 是 primary boot 最早阶段临时现建的那份 init idmap


### 11.9 primary boot 里 TTBR0 / TTBR1 是怎么切换的

如果把你这次关心的几份页表按启动时序串起来，主线可以概括成下面这几步。

#### 阶段 A：MMU 还没开，先手工创建 `__pi_init_idmap_pg_dir`

在 [arch/arm64/kernel/head.S](arch/arm64/kernel/head.S#L92) 的 `primary_entry` 里：

- `x0 = __pi_init_idmap_pg_dir`
- 调用 `__pi_create_init_idmap`
- 于是得到一份覆盖当前内核镜像最小范围的 early identity map

这一步发生时：

- MMU 还没开
- 只是把页表内容先写到内存里


#### 阶段 B：第一次开 MMU，TTBR0 用 `__pi_init_idmap_pg_dir`，TTBR1 用 `reserved_pg_dir`

紧接着在 [arch/arm64/kernel/head.S](arch/arm64/kernel/head.S#L509) 的 `__primary_switch` 里：

- `x1 = reserved_pg_dir`
- `x2 = __pi_init_idmap_pg_dir`
- 调用 `__enable_mmu`

而 [arch/arm64/kernel/head.S](arch/arm64/kernel/head.S#L459) 的 `__enable_mmu` 会做：

- `ttbr0_el1 <- x2`
- `ttbr1_el1 <- x1`

这里 `ttbr0_el1 <- x2` 同样不是“把整张页表内容装进去”，而是：

- 把 **init idmap 根页表的物理基地址** 装进 `TTBR0_EL1`
- 让硬件后续从这张根表开始做 page table walk

对应代码是：

```asm
phys_to_ttbr x2, x2
msr ttbr0_el1, x2
```

也就是说，写进 `TTBR0_EL1` 的内容本质上是：

- `__pi_init_idmap_pg_dir` 这张根页表的物理地址
- 再经过 `phys_to_ttbr()` 按架构要求整理成 TTBR 格式

对你当前 build：

- `__pi_init_idmap_pg_dir` 的物理地址是 `0x41740000`
- 当前又打开了 `CONFIG_ARM64_PA_BITS_52=y`

而 [arch/arm64/include/asm/pgtable.h](arch/arm64/include/asm/pgtable.h#L1605) 里的 `phys_to_ttbr()` 是：

```c
#define phys_to_ttbr(addr) (((addr) | ((addr) >> 46)) & TTBR_BADDR_MASK_52)
```

对这个具体地址，整理后的 TTBR 值仍然就是：

```text
TTBR0_EL1 base = 0x41740000
```

所以这一步装进去的不是 L0/L1/L2/L3 的页表项内容本身，而是：

- “根页表在哪一页”的基地址信息

随后硬件真正做翻译时，才会：

1. 从 `TTBR0_EL1` 取出根页表基地址
2. 访问 `__pi_init_idmap_pg_dir`
3. 查 L0 entry
4. 再逐级走到 L1/L2/L3

这里 `ttbr1_el1 <- x1` 不是简单一条裸 `msr`，而是通过 [arch/arm64/include/asm/assembler.h](arch/arm64/include/asm/assembler.h#L454) 的 `load_ttbr1` 宏完成：

```asm
.macro load_ttbr1, pgtbl, tmp1, tmp2
	phys_to_ttbr tmp1, pgtbl
	offset_ttbr1 tmp1, tmp2
	msr ttbr1_el1, tmp1
	isb
.endm
```

它做了 4 件事：

1. 把页表根的物理地址整理成 TTBR 格式。
2. 在某些 52-bit VA 场景下，必要时给 TTBR1 基地址补偏移。
3. 把结果写入 `ttbr1_el1`。
4. 用 `isb` 保证后续取指/翻译看到新的 TTBR1 值。

所以第一次打开 MMU 后，状态其实是：

```text
TTBR0 -> __pi_init_idmap_pg_dir
TTBR1 -> reserved_pg_dir
```

这很关键，因为这说明：

- 刚开 MMU 时，CPU 仍然主要依赖 identity map 继续跑过渡代码
- 高地址那一半暂时还没有切到正式 kernel page table
- `reserved_pg_dir` 在这里的具体作用是：给 TTBR1 提供一张“合法但暂不承担正式高地址内核映射”的安全占位根页表

换句话说，这一步不是让 TTBR1 立刻去承担完整 kernel high VA 访问，而是先满足两件事：

- MMU 打开时 TTBR1 必须已经装入一个合法的根页表地址
- 在正式 kernel page table 建好之前，TTBR1 最好不要暴露出不完整或错误的高地址映射

所以这里先装 `reserved_pg_dir`，等 [arch/arm64/kernel/pi/map_kernel.c](arch/arm64/kernel/pi/map_kernel.c#L231) 的 `early_map_kernel()` 建好 `init_pg_dir` / `swapper_pg_dir` 之后，再通过 `idmap_cpu_replace_ttbr1()` 逐步替换进去。

这里还有一个非常实际的原因：

- `init_pg_dir` 在这个时刻其实还没准备好，不能直接拿来给 TTBR1 用。

更准确地说，有 3 个原因：

1. 它还没建出来。
	`init_pg_dir` 的真正构造发生在后面的 [arch/arm64/kernel/pi/map_kernel.c](arch/arm64/kernel/pi/map_kernel.c#L34) `map_kernel()` 里，而 `__enable_mmu()` 发生在这之前。
2. 它所在的 BSS/early page table 区还要在 `early_map_kernel()` 里清零。
	也就是说，在 `__enable_mmu()` 这一刻，`init_pg_dir` 还不是一张可直接信任的完整页表。
3. 它的最终内容依赖运行时决定。
	例如 `va_bits`、`root_level`、`kaslr_offset`、`rodata=off`、BTI/SCS、LPA2 等因素，都是在 `early_map_kernel()` 里才综合决定的。

所以这一步不能“直接把 TTBR1 指到 init_pg_dir”，因为那样要么指向一张尚未构造完成的表，要么暴露出不正确的高地址映射。

因此正确时序一定是：

```text
TTBR1 = reserved_pg_dir   // 先安全占位
build init_pg_dir         // 后面再真正构造第二次映射
TTBR1 = init_pg_dir       // 构造完成后再切过去
TTBR1 = swapper_pg_dir    // 最终切到正式根页表
```


#### 阶段 C：进入 `__pi_early_map_kernel()`，构造正式 kernel 映射到 `init_pg_dir`

随后 [arch/arm64/kernel/head.S](arch/arm64/kernel/head.S#L518) 调用 `__pi_early_map_kernel`。

在 [arch/arm64/kernel/pi/map_kernel.c](arch/arm64/kernel/pi/map_kernel.c#L231) 的 `early_map_kernel()` 里，会进一步调用 `map_kernel()`。

`map_kernel()` 做的事情是：

- 在 `init_pg_dir` 这块临时页表工作区里建立真正的 early kernel mapping
- 按 `_text/_stext/_etext/__inittext_begin/__initdata_begin/_data/_end` 等边界分段映射
- 必要时处理 KASLR、LPA2、BTI、SCS、relocatable kernel 等因素

这里的 `init_pg_dir` 不是你前面分析的 `__pi_init_idmap_pg_dir`，而是另一块 BSS 里的临时页表池：

- [arch/arm64/kernel/vmlinux.lds.S](arch/arm64/kernel/vmlinux.lds.S#L301) `__pi_init_pg_dir`
- [arch/arm64/kernel/vmlinux.lds.S](arch/arm64/kernel/vmlinux.lds.S#L303) `__pi_init_pg_end`


#### 阶段 D：在 idmap 代码里把 TTBR1 从 `reserved_pg_dir` 切到 `init_pg_dir`

`map_kernel()` 建好这份临时正式映射后，会调用：

- [arch/arm64/kernel/pi/map_kernel.c](arch/arm64/kernel/pi/map_kernel.c#L99) `idmap_cpu_replace_ttbr1((phys_addr_t)init_pg_dir)`

而 [arch/arm64/mm/proc.S](arch/arm64/mm/proc.S#L179) 的 `idmap_cpu_replace_ttbr1()` 明确是在 `.idmap.text` 中执行的，它会：

1. 先把 `ttbr1_el1` 置到 `reserved_pg_dir`
2. 刷新相关 TLB
3. 再把 `ttbr1_el1` 切到新的目标页表

所以这一阶段之后，状态变成：

```text
TTBR0 -> __pi_init_idmap_pg_dir
TTBR1 -> init_pg_dir
```

这一步的本质是：

- 仍然站在 TTBR0 提供的 identity map 上执行安全过渡代码
- 但 TTBR1 已经切到了“真正能跑内核虚拟地址”的临时 kernel page table


#### 阶段 E：把 `init_pg_dir` 的根页复制到 `swapper_pg_dir`，再把 TTBR1 切到 `swapper_pg_dir`

在 [arch/arm64/kernel/pi/map_kernel.c](arch/arm64/kernel/pi/map_kernel.c#L136) 附近，`map_kernel()` 最后会：

- 把 root page table 拷贝到 `swapper_pg_dir`
- 再调用 `idmap_cpu_replace_ttbr1((phys_addr_t)swapper_pg_dir)`

于是最终得到：

```text
TTBR0 -> __pi_init_idmap_pg_dir
TTBR1 -> swapper_pg_dir
```

到这一步，正式内核页表根已经就位。


#### 阶段 E.1：为什么切到 `swapper_pg_dir` 时不用把整套页表重新建一遍

这里最容易产生的误解是：

> 前面建表时一直在用 `init_pg_dir`，而且下级页表页也都是从 `init_pg_dir` 后面的工作区里分配出来的；那现在 TTBR1 切到 `swapper_pg_dir`，是不是整棵页表树都要重新做一份？

答案是：**不用。这里只复制根页，不重建下级页表。**

先看 `map_kernel()` 一开始怎么分配页表页：

```c
static void __init map_kernel(u64 kaslr_offset, u64 va_offset, int root_level)
{
		phys_addr_t pgdp = (phys_addr_t)init_pg_dir + PAGE_SIZE;
		...
		map_segment(init_pg_dir, &pgdp, va_offset, ...);
		...
}
```

也就是说：

- `init_pg_dir` 的第 1 页，是根页
- `init_pg_dir + PAGE_SIZE` 往后那一整段，是“早期页表页分配池”
- `map_segment()` / `map_range()` 在需要下级页表时，会从 `pgdp` 指向的位置继续切新页出来

所以前面建出来的结构，实际上长这样：

```text
init_pg_dir region

+----------------------+  <- init_pg_dir
| root page            |
+----------------------+
| next-level table #1  |
+----------------------+
| next-level table #2  |
+----------------------+
| next-level table #3  |
+----------------------+
| ...                  |
```

也就是说，`init_pg_dir` 这个名字在早期阶段其实同时指两层含义：

- 狭义上：根页本身
- 广义上：整块“根页 + 下级页表池”的工作区

而 `swapper_pg_dir` 不是这块工作区，它只是最终正式使用的**根页位置**。


#### 阶段 E.2：代码为什么只复制 1 页就够了

最后切换前，代码做的是：

```c
/* Copy the root page table to its final location */
memcpy((void *)swapper_pg_dir + va_offset, init_pg_dir, PAGE_SIZE);
dsb(ishst);
idmap_cpu_replace_ttbr1((phys_addr_t)swapper_pg_dir);
```

这里 `memcpy(..., PAGE_SIZE)` 明确告诉你：

- 只复制 **1 页**
- 复制的就是 `init_pg_dir` 的**根页**
- 不是把 `init_pg_dir .. init_pg_end` 那整块工作区全量复制到 `swapper_pg_dir`

之所以只复制根页就够，是因为页表项里存放的是：

- 下级页表页的**物理地址**
- 再加上一些 descriptor 属性位

它存的不是“相对 `init_pg_dir` 的偏移”。

所以当根页里的某个表项原本写着：

```text
init_pg_dir[index] = 0x41xx0000 | TABLE bits
```

这个 `0x41xx0000` 指向的是某个已经分配好的下级页表页的物理地址。把这 8 字节原样复制到 `swapper_pg_dir[index]` 后，含义完全不变：

```text
swapper_pg_dir[index] = 0x41xx0000 | TABLE bits
```

此时新的 TTBR1 根虽然换了，但页表 walk 继续走到的仍然是**同一批下级页表页**。


#### 阶段 E.3：结合当前 build 举一个具体例子

你前面已经算过，当前 build 里第二次映射的高地址范围全都落在：

```text
L0[256]
	-> L1[2]
```

而且其中：

- `L2[1..6]` 这类完整 2MB 窗口，可以直接是 block
- `L2[10]` 这种跨权限边界的 2MB 窗口，必须再下钻到 L3

比如拿这个窗口：

```text
0xffff800081400000 - 0xffff800081600000   (对应物理 0x41600000 - 0x41800000)
```

它包含了：

- `__inittext_begin = 0xffff800081460000`
- `__inittext_end   = 0xffff800081540000`

这 2MB 窗口内部有 RW/ROX/RW 三段权限变化，所以不能用 1 个 L2 block，只能让 `L2[10]` 指向一个 L3 页表页。

也就是说，在建表结束后，概念上会是这样：

```text
init_pg_dir[L0[256]]  -> table page A
page A[L1[2]]         -> table page B
page B[L2[10]]        -> table page C
page C[L3[96..319]]   -> 一串 4KB PTE
```

这里：

- `table page A/B/C` 都是前面从 `pgdp` 那个分配池里切出来的真实物理页
- 它们所在物理内存仍然就在 `init_pg_dir` 后面的工作区中

然后最后一步：

```c
memcpy((void *)swapper_pg_dir + va_offset, init_pg_dir, PAGE_SIZE);
```

只把根页拷过去，于是变成：

```text
swapper_pg_dir[L0[256]] -> table page A
table page A[L1[2]]     -> table page B
table page B[L2[10]]    -> table page C
table page C[L3[96..319]] -> 一串 4KB PTE
```

你可以看到：

- 变的只有“最顶层根页放在哪里”
- `A/B/C` 这些下级页表页完全没变
- 因此 TTBR1 一切过去就能直接沿着同一棵树继续走

这就是为什么这里**不需要重新建页表**。


#### 阶段 E.4：再看一个 2MB block 的例子会更直观

再看当前 build 里这个完整窗口：

```text
0x40400000 - 0x40600000
	-> 0xffff800080200000 - 0xffff800080400000
```

它对应的是一个完整的 `ROX` 2MB 区间，所以这里的 L2 项不需要再指向 L3，而是直接就是 block descriptor。

概念上相当于：

```text
init_pg_dir[L0[256]] -> table page A
page A[L1[2]]        -> table page B
page B[L2[1]]        -> 2MB block mapping to 0x40400000
```

最后切到 `swapper_pg_dir` 后只是变成：

```text
swapper_pg_dir[L0[256]] -> table page A
page A[L1[2]]           -> table page B
page B[L2[1]]           -> 2MB block mapping to 0x40400000
```

这里甚至连 “再往下的页表页” 都不存在，因为 `L2[1]` 本来就是 block。复制根页后，它自然继续有效。


#### 阶段 E.5：为什么代码设计成“先在 `init_pg_dir` 工作，再发布到 `swapper_pg_dir`”

这样设计的好处是：

1. `init_pg_dir` 有整块连续工作区，适合动态分配下级页表页。
2. `map_kernel()` 中间还可能二次改权限、做 reloc/SCS/BTI 相关处理，先在工作根上改完更方便。
3. 等页表结构稳定后，只要把根页复制到 `swapper_pg_dir`，就完成“正式发布”。

所以 `init_pg_dir -> swapper_pg_dir` 的关系，不是：

```text
先建一棵树 A
再重新建一棵树 B
```

而是：

```text
先在 init_pg_dir 工作区里把整棵树建好
再把这棵树的“根指针页”复制到 swapper_pg_dir
让 swapper_pg_dir 接管同一棵树
```

如果只记一句话，最准确的说法是：

> 切到 `swapper_pg_dir` 时，切换的是“根页位置”，不是“整棵页表树的物理存放位置”。


#### 阶段 F：跳到高地址内核映射，后续再逐步卸掉 early idmap

`__pi_early_map_kernel()` 返回后，`head.S` 会继续跳到高地址的 `__primary_switched` 路径运行。

从启动主线的角度看，这时已经完成了最重要的切换：

- 低地址过渡执行仍可由早期 idmap 兜底
- 高地址内核执行已经由 `swapper_pg_dir` 承载

后续更一般性的安装/卸载 idmap，则由：

- [arch/arm64/include/asm/mmu_context.h](arch/arm64/include/asm/mmu_context.h#L100) `cpu_uninstall_idmap()`
- [arch/arm64/include/asm/mmu_context.h](arch/arm64/include/asm/mmu_context.h#L112) `cpu_install_idmap()`

这两条路径负责在运行期需要时，把 TTBR0 在 `reserved_pg_dir` 与 `idmap_pg_dir` 之间安全切换。


#### 这一整段最简洁的时序图

```text
MMU off
	create_init_idmap(__pi_init_idmap_pg_dir)

Enable MMU:
	TTBR0 = __pi_init_idmap_pg_dir
	TTBR1 = reserved_pg_dir

early_map_kernel():
	build init_pg_dir
	TTBR1 = init_pg_dir
	copy root to swapper_pg_dir
	TTBR1 = swapper_pg_dir

later runtime transitions:
	TTBR0 <-> reserved_pg_dir / idmap_pg_dir
```

如果只针对你当前的主问题，可以把它浓缩成一句话：

> `__pi_init_idmap_pg_dir` 是“第一次开 MMU 时立刻可用的最小 TTBR0 identity map”，而 `swapper_pg_dir` 才是随后接管正式内核虚拟地址空间的 TTBR1 根页表。


## 13. 可直接粘贴的 GDB 检查脚本

下面这组命令适合在 `create_init_idmap()` 返回后直接执行，用来一次性验证页表结构。

```gdb
idmapinfo
p/x $x0
p/x $pa_pi_init_idmap_pg_dir
p/x $x0 - $pa_pi_init_idmap_pg_dir

printf "\n== L0 / L1 / L2 ==\n"
x/gx $pa_pi_init_idmap_pg_dir + 8*256
x/gx $pa_pi_init_idmap_l1 + 8*2
x/18gx $pa_pi_init_idmap_l2

printf "\n== L3 head ==\n"
x/gx $pa_pi_init_idmap_l3_head + 8*15
x/gx $pa_pi_init_idmap_l3_head + 8*16

printf "\n== L3 mid ==\n"
x/gx $pa_pi_init_idmap_l3_mid + 8*319
x/gx $pa_pi_init_idmap_l3_mid + 8*320

printf "\n== L3 tail ==\n"
x/gx $pa_pi_init_idmap_l3_tail + 8*303
x/gx $pa_pi_init_idmap_l3_tail + 8*304
```

执行后可以按下面的规则判断：

- `L0[256]` 和 `L1[2]` 应该是 table descriptor
- `L2[0] / L2[10] / L2[17]` 应该是 table descriptor
- `L2[1..9]` 和 `L2[11..16]` 应该是 block descriptor
- `L3 head[15]` invalid，`L3 head[16]` valid
- `L3 mid[319]` 和 `L3 mid[320]` 都 valid，但权限不同
- `L3 tail[303]` valid，`L3 tail[304]` invalid


## 14. 调试时最容易踩的坑

### 14.1 不要直接拿 `__pi_init_idmap_pg_dir` 做数值运算

在 MMU-off 阶段，如果同时加载了 `file vmlinux` 和 `add-symbol-file` 的重定位符号，GDB 对链接脚本符号的解释容易混到链接时虚拟地址那一份。

因此更稳的做法是直接用 `~/.gdbinit` 里定义好的物理变量：

- `$pa_pi_init_idmap_pg_dir`
- `$pa_pi_init_idmap_l1`
- `$pa_pi_init_idmap_l2`
- `$pa_pi_init_idmap_l3_head`
- `$pa_pi_init_idmap_l3_mid`
- `$pa_pi_init_idmap_l3_tail`


### 14.2 `x0` 返回值才是最硬的验证点之一

如果 `create_init_idmap()` 返回时：

```text
$x0 = $pa_pi_init_idmap_pg_dir + 0x6000
```

那就已经非常有力地说明：

- 页表确实创建成功
- 页表页数量符合预期
- 三个 L3 窗口的分析大概率是对的


## 15. `map_range()` 逐行解析

如果这份文档里只选一个最值得彻底看懂的函数，那就是 [arch/arm64/kernel/pi/map_range.c](arch/arm64/kernel/pi/map_range.c#L24) 的 `map_range()`。

它本质上做的事情可以先压缩成一句话：

> 从当前 level 的页表出发，尽量用当前 level 能容纳的最大粒度去映射；如果当前 chunk 不能整块映射，就分配下一层页表并递归下钻。


### 15.1 先看它解决的核心问题

给它一组参数：

- 一个 VA 区间 `[start, end)`
- 对应的起始 PA `pa`
- 权限 `prot`
- 当前层级 `level`
- 当前层级页表 `tbl`
- 可继续分配的空闲页表页游标 `pte`

它要做的是：

1. 找到 `start` 在当前页表 `tbl` 里落到哪个 entry。
2. 看当前层级能不能直接用一个 leaf/block 描述这一小段。
3. 如果能，就直接写 block/page descriptor。
4. 如果不能，就给这个 entry 分配下一层页表，再递归处理。
5. 处理完这一小段后，推进到下一小段，直到覆盖完整个 `[start, end)`。


### 15.2 带注释的“逐行版源码”

下面这版不是改源码，而是把原函数按理解顺序逐行加解释。

```c
void __init map_range(phys_addr_t *pte, u64 start, u64 end, phys_addr_t pa,
		      pgprot_t prot, int level, pte_t *tbl, bool may_use_cont,
		      u64 va_offset)
{
	/*
	 * 只有在 level 3 时，才有可能使用 contiguous PTE。
	 * 如果不是 level 3，这个掩码就先设成 U64_MAX，等价于“别启用 cont”。
	 */
	u64 cmask = (level == 3) ? CONT_PTE_SIZE - 1 : U64_MAX;

	/*
	 * protval 是最终要写进 leaf/block 描述符里的权限属性位，
	 * 但先把最低两位 type bits 去掉，因为 type bits 取决于当前 level：
	 *   - level 2 用 PMD_TYPE_SECT
	 *   - level 3 用 PTE_TYPE_PAGE
	 */
	ptdesc_t protval = pgprot_val(prot) & ~PTE_TYPE_MASK;

	/*
	 * lshift 决定“当前 level 的一个 entry 覆盖多大范围”。
	 * 对 4KB granule：
	 *   level 0 -> lshift = 27 -> entry covers 512GB
	 *   level 1 -> lshift = 18 -> entry covers 1GB
	 *   level 2 -> lshift =  9 -> entry covers 2MB
	 *   level 3 -> lshift =  0 -> entry covers 4KB
	 */
	int lshift = (3 - level) * PTDESC_TABLE_SHIFT;

	/*
	 * lmask 是“当前 level 一个 entry 覆盖范围减 1”的掩码。
	 * 后面会用它来：
	 *   - 找当前 chunk 所在的 entry 边界
	 *   - 判断当前 chunk 是否能在本层直接映射
	 */
	u64 lmask = (PAGE_SIZE << lshift) - 1;

	/*
	 * start 和 pa 都先按页对齐。
	 * map_range() 最小粒度也不会小于 4KB。
	 */
	start &= PAGE_MASK;
	pa    &= PAGE_MASK;

	/*
	 * 把 tbl 指针直接推进到“覆盖 start 的那个 entry”。
	 * 对应的索引公式本质上就是：
	 *   index = (start >> level_shift) & 0x1ff
	 * 这里只是把它写成了通用形式。
	 */
	tbl += (start >> (lshift + PAGE_SHIFT)) % PTRS_PER_PTE;

	/*
	 * 如果这次不是清 mapping（protval != 0），
	 * 就补上当前层的 type bits：
	 *   - level 2 写 block descriptor
	 *   - level 3 写 page descriptor
	 * level 0 / 1 不会走到直接写 leaf 的分支。
	 */
	if (protval)
		protval |= (level == 2) ? PMD_TYPE_SECT : PTE_TYPE_PAGE;

	/*
	 * 每次循环只处理“当前 tbl entry 能覆盖的一小段 chunk”。
	 */
	while (start < end) {
		/*
		 * next = 当前 chunk 的结束地址。
		 * 它取两者较小值：
		 *   1. 当前 level entry 的结束边界
		 *   2. 整个映射区间 end（按页向上对齐）
		 *
		 * 所以每轮循环都只在一个 entry 范围内工作。
		 */
		u64 next = min((start | lmask) + 1, PAGE_ALIGN(end));

		/*
		 * 什么时候必须继续下钻？
		 *
		 * 1. level < 2
		 *    也就是当前在 L0/L1。
		 *    这两层在这条路径里不允许直接放 leaf，必须继续下一级。
		 *
		 * 2. 当前在 level 2，但这个 2MB chunk 不能直接 block 映射。
		 *    判定条件是 (start | next | pa) & lmask 非零。
		 *    它等价于检查：
		 *      - start 是否按当前块大小对齐
		 *      - next  是否刚好落在当前块边界
		 *      - pa    是否按当前块大小对齐
		 *
		 *    只要三者有一个不满足，就说明当前 chunk 不能整块映射，
		 *    只能拆到下一层。
		 */
		if (level < 2 || (level == 2 && (start | next | pa) & lmask)) {
			/*
			 * 当前 entry 还没有下一层页表时，先分配一张新页表页。
			 */
			if (pte_none(*tbl)) {
				/*
				 * *pte 指向当前可用的下一张空闲页表页。
				 * 这里把这个物理页写成 table descriptor，挂到当前 entry 上。
				 */
				*tbl = __pte(__phys_to_pte_val(*pte) |
					     PMD_TYPE_TABLE | PMD_TABLE_UXN);

				/*
				 * 分配完这一页后，游标前进到下一张空闲页表页。
				 */
				*pte += PTRS_PER_PTE * sizeof(pte_t);
			}

			/*
			 * 递归下钻到下一层。
			 * __pte_to_phys(*tbl) 取出下一层页表的物理地址；
			 * 再加上 va_offset，得到当前执行环境下能访问它的地址。
			 *
			 * 在 create_init_idmap() 这条路径里，va_offset = 0，
			 * 因为 MMU 还没开，直接按物理地址访问即可。
			 */
			map_range(pte, start, next, pa, prot, level + 1,
				  (pte_t *)(__pte_to_phys(*tbl) + va_offset),
				  may_use_cont, va_offset);
		} else {
			/*
			 * 到这里说明当前 chunk 可以在本层直接映射。
			 *
			 * 如果允许 contiguous 并且 start/pa 都满足 cont 对齐，
			 * 就把 PTE_CONT 打开。
			 *
			 * 但注意：create_init_idmap() 传进来的 may_use_cont=false，
			 * 所以这条路径对当前 init idmap 实际不会生效。
			 */
			if (((start | pa) & cmask) == 0 && may_use_cont)
				protval |= PTE_CONT;

			/*
			 * 如果剩余范围已经不足一个完整 contiguous group，
			 * 就把 PTE_CONT 再清掉，避免错误跨界。
			 */
			if ((end & ~cmask) <= start)
				protval &= ~PTE_CONT;

			/*
			 * 直接写 leaf/block 描述符。
			 *
			 * level 2 -> PMD block
			 * level 3 -> PTE page
			 */
			*tbl = __pte(__phys_to_pte_val(pa) | protval);
		}

		/*
		 * 本轮 chunk 处理完，推进到下一个 chunk。
		 */
		pa += next - start;
		start = next;
		tbl++;
	}
}
```


### 15.3 这段函数里最重要的 5 个算法

#### 15.3.1 算法一：按“当前 level entry 边界”切 chunk

这一行是第一关键点：

```c
u64 next = min((start | lmask) + 1, PAGE_ALIGN(end));
```

它的作用是把整个大区间 `[start, end)` 切成一段一段“小 chunk”，并且保证：

- 每个 chunk 都完全落在当前 level 的单个 entry 覆盖范围内
- 因此每轮循环只需要决定“当前这个 entry 是写 leaf，还是挂下一层表”

这是整个函数能写成统一递归框架的核心。


#### 15.3.2 算法二：能在当前层直接映射，就绝不下钻

判定逻辑是：

```c
if (level < 2 || (level == 2 && (start | next | pa) & lmask))
```

意思是：

- L0/L1 永远不能直接叶子映射，所以一定下钻
- 到了 L2，只有当当前 2MB chunk 满足“VA 起点对齐、VA 终点卡边界、PA 对齐”时，才允许直接 block
- 否则继续下钻到 L3

你这次看到的 3 个特殊窗口，正是因为这个判定失败：

- 起始窗口：`start` 不对齐
- 中间窗口：`next` 不落在同权限的完整 2MB 边界上
- 尾部窗口：`next` 不对齐到 2MB 窗口末端


#### 15.3.3 算法三：子页表用 bump-pointer 顺序分配

这一段是第二关键点：

```c
if (pte_none(*tbl)) {
	*tbl = __pte(__phys_to_pte_val(*pte) | PMD_TYPE_TABLE | PMD_TABLE_UXN);
	*pte += PTRS_PER_PTE * sizeof(pte_t);
}
```

它的策略非常简单：

- `*pte` 永远指向“下一张空闲页表页”
- 需要下一层表时，就直接把这一页挂上去
- 然后把 `*pte` 增加一个 `PAGE_SIZE`

所以 `create_init_idmap()` 最后返回的 `ptep`，本质上就是：

- “页表内存池已经用到了哪里”


#### 15.3.4 算法四：同一套代码同时支持 block 和 page 映射

这一行体现了它的通用性：

```c
protval |= (level == 2) ? PMD_TYPE_SECT : PTE_TYPE_PAGE;
```

含义是：

- 如果当前停在 L2，就写 2MB block descriptor
- 如果当前停在 L3，就写 4KB page descriptor

因此 `map_range()` 本身并不预先决定“这次一定做 block”还是“一定做 page”，而是：

- 先尝试在当前层直接映射
- 不行就递归到下一层
- 最后自然得到“能 block 的地方 block，不能 block 的地方 page”


#### 15.3.5 算法五：递归时只处理当前 chunk，不会跨界污染

递归调用是：

```c
map_range(pte, start, next, pa, prot, level + 1, ...)
```

注意传进去的不是整个 `[start, end)`，而是当前这一小段 `[start, next)`。

这保证了：

- 每一层递归都只在当前 entry 的覆盖范围内工作
- 不会把相邻 entry 的内容混到一起
- 处理完当前 chunk 后，外层循环再进入下一个 entry

这也是为什么这个递归函数虽然短，但逻辑很稳。


### 15.4 套到你当前这次 `create_init_idmap()` 上，实际发生了什么

对这次调用：

- `level = IDMAP_ROOT_LEVEL = 0`
- `va_offset = 0`
- `may_use_cont = false`

所以可以把实际行为简化成：

1. 从 L0 根表开始。
2. L0/L1 一定递归下钻。
3. 到 L2 后，尽量做 2MB block。
4. 遇到不对齐或权限切换窗口，就分配一张 L3 表继续细分成 4KB PTE。
5. 最终得到：
   - `L2[1..9]` 和 `L2[11..16]` 是 block
   - `L2[0]`、`L2[10]`、`L2[17]` 是 table

也就是说，`create_init_idmap()` 的最终页表结构，其实就是 `map_range()` 这套“当前层能直接放就直接放，否则下钻”的算法自然推导出来的结果。


### 15.5 一个最好记的心智模型

把 `map_range()` 想成一个“自顶向下切蛋糕”的递归过程最容易记：

- 先看当前大块能不能整块切
- 能整块切，就直接落一个 block/page
- 不能整块切，就把这一块切细，交给下一层继续处理
- 每次都只处理当前这一个 chunk
- 子页表页按顺序从内存池里一张一张往后拿

如果只保留一句话，那就是：

> `map_range()` 的算法本质是“按当前层 entry 边界切 chunk，能在本层直接映射就直接映射，否则分配下一层页表递归处理”。


## 16. 第二次映射：`__pi_early_map_kernel()`

关键点先说：

> 按启动流程讲，第二次映射阶段可以概括成 `__pi_early_map_kernel()`；但更准确地说，它包含两部分：先扩展 TTBR0 那边仍在使用的 early idmap（主要给 FDT），然后在 TTBR1 一侧建立真正的 early kernel mapping。

也就是说：

- 第一次映射：`create_init_idmap()`
	在 `__pi_init_idmap_pg_dir` 里构造一份 `VA = PA` 的临时 idmap，供 TTBR0 使用。
- 第二次映射：`__pi_early_map_kernel()`
	在 `init_pg_dir` 里构造“当前物理装载地址 -> 最终高地址 kernel VA”的映射，供 TTBR1 使用。


### 16.1 它在启动流程里的位置

主路径在 [arch/arm64/kernel/head.S](arch/arm64/kernel/head.S#L509) 之后非常清楚：

```text
primary_entry
  -> __pi_create_init_idmap          // 第一次映射，构造 TTBR0 用的 early idmap
  -> __enable_mmu                    // TTBR0 = __pi_init_idmap_pg_dir, TTBR1 = reserved_pg_dir
  -> __pi_early_map_kernel           // 第二次映射，开始构造 TTBR1 的 kernel mapping
  -> __primary_switched              // 跳入正式高地址 kernel VA 执行
```

所以如果只从“映射阶段”来数：

- 第一阶段是 `create_init_idmap()`
- 第二阶段就是 `__pi_early_map_kernel()`


### 16.2 但 `__pi_early_map_kernel()` 里其实又分两步

看 [arch/arm64/kernel/pi/map_kernel.c](arch/arm64/kernel/pi/map_kernel.c#L231) 的 `early_map_kernel()`：

1. `map_fdt(fdt)`
	给 FDT 所在的物理区间补一段 idmap，仍然挂在 `init_idmap_pg_dir` 上，也就是 TTBR0 这边。
2. `map_kernel(kaslr_offset, va_offset, root_level)`
	在 `init_pg_dir` 上构造真正的 early kernel mapping，目标是后面写入 TTBR1。

所以更准确地说：

- `__pi_early_map_kernel()` 不是只做“一张新表”
- 而是先补 TTBR0 侧还要继续用到的映射
- 再把 TTBR1 侧真正的内核高地址映射建出来


### 16.3 第二次映射和第一次映射的本质区别

第一次映射 `create_init_idmap()` 的本质是：

- 运行时使用低地址物理区间本身做 VA
- 所以 `VA = PA`

第二次映射 `map_kernel()` 的本质则是：

- 左边 VA 变成最终要使用的高地址 kernel VA
- 右边 PA 仍然指向当前内核镜像实际所在的物理装载位置

所以第二次映射已经不是 idmap，而是：

- `kernel virtual address -> current physical image location`

这就是为什么第一次映射让 TTBR0 生效，而第二次映射最终要切到 TTBR1。


### 16.4 `map_segment()` 一句话说透第二次映射

第二次映射最关键的一行不在 `map_range()`，而在 [arch/arm64/kernel/pi/map_kernel.c](arch/arm64/kernel/pi/map_kernel.c#L19) 的 `map_segment()`：

```c
map_range(pgd, ((u64)start + va_offset) & ~PAGE_OFFSET,
	  ((u64)end + va_offset) & ~PAGE_OFFSET, (u64)start,
	  prot, root_level, (pte_t *)pg_dir, may_use_cont, 0);
```

它的含义非常直接：

- 页表左边写入的新 VA = `start + va_offset`
- 页表右边写入的 PA = `start`

所以第二次映射的算法本质就是：

> 以当前运行中的低地址物理映像为 PA 源区间，再加上 `va_offset`，把它搬到最终高地址 kernel VA 空间去。


### 16.5 当前 build 下第二次映射的地址关系

基于你当前这次 build：

- `_text  = 0xffff800080000000`
- `_stext = 0xffff800080010000`
- `_etext = 0xffff800080f00000`
- `__start_rodata = 0xffff800080f00000`
- `__inittext_begin = 0xffff800081460000`
- `__inittext_end = 0xffff800081540000`
- `__initdata_begin = 0xffff800081540000`
- `__initdata_end = 0xffff800082120000`
- `_data = 0xffff800082120000`
- `_end  = 0xffff800082330000`

当前物理装载边界对应为：

```text
_text             -> 0x40200000
_stext            -> 0x40210000
_etext            -> 0x41100000
__start_rodata    -> 0x41100000
__inittext_begin  -> 0x41660000
__inittext_end    -> 0x41740000
__initdata_begin  -> 0x41740000
__initdata_end    -> 0x42320000
_data             -> 0x42320000
_end              -> 0x42530000
```

对当前无额外 KASLR 位移的观察，第二次映射使用的：

```text
va_offset = 0xffff80003fe00000
```

因此它做的事情可以概括成：

```text
0x40200000 + 0xffff80003fe00000 -> 0xffff800080000000
0x40210000 + 0xffff80003fe00000 -> 0xffff800080010000
0x41740000 + 0xffff80003fe00000 -> 0xffff800081540000
0x42530000 + 0xffff80003fe00000 -> 0xffff800082330000
```

也就是说：

- 第一次映射是低地址 `VA = PA`
- 第二次映射才把这些低地址物理内容接到最终高地址内核虚拟地址上


### 16.6 当前 build 下第二次映射实际覆盖哪些段

`map_kernel()` 在 [arch/arm64/kernel/pi/map_kernel.c](arch/arm64/kernel/pi/map_kernel.c#L76) 之后依次映射了这些区间：

```text
1. [_text, _stext)                -> PAGE_KERNEL      (RW, NX)
2. [_stext, _etext)               -> text_prot        (通常最终为 ROX)
3. [__start_rodata, __inittext_begin) -> PAGE_KERNEL  (RW, NX)
4. [__inittext_begin, __inittext_end) -> text_prot    (通常最终为 ROX)
5. [__initdata_begin, __initdata_end) -> PAGE_KERNEL  (RW)
6. [_data, _end)                  -> PAGE_KERNEL      (RW)
```

把当前 build 的实际地址代进去，可以得到：

```text
runtime PA/current low VA                 final kernel VA                     permission
0x40200000 - 0x40210000                  0xffff800080000000 - 0xffff800080010000   RW, NX
0x40210000 - 0x41100000                  0xffff800080010000 - 0xffff800080f00000   text_prot
0x41100000 - 0x41660000                  0xffff800080f00000 - 0xffff800081460000   RW, NX
0x41660000 - 0x41740000                  0xffff800081460000 - 0xffff800081540000   text_prot
0x41740000 - 0x42320000                  0xffff800081540000 - 0xffff800082120000   RW
0x42320000 - 0x42530000                  0xffff800082120000 - 0xffff800082330000   RW
```

这里最容易忽略的点有两个：

1. `[_text, _stext)` 这小段不是执行正文，所以按数据权限映射。
2. `__start_rodata` 到 `__inittext_begin` 在这个早期阶段也先按 `PAGE_KERNEL` 映射，不等于后续最终只读保护已经全部到位。


### 16.6.1 如果像第一次 idmap 一样按 2MB / 4KB 来看，`init_pg_dir` 最终是什么结构

如果把第二次映射也像前面 `create_init_idmap()` 一样按 2MB 窗口展开，那么当前 build 的最终 `init_pg_dir` 映射结果可以压缩成：

- 主体仍然是 **level 2 / 2MB block**
- 但会有 **4 个特殊 2MB 窗口** 下钻成 **level 3 / 4KB PTE**

这 4 个特殊窗口分别是：

1. `_text -> _stext` 的 RW/ROX 切换窗口
2. `_etext -> __start_rodata` 附近的 ROX/RW 切换窗口
3. `__inittext_begin -> __inittext_end -> __initdata_begin` 这组边界所在窗口
4. `_end` 的尾部不对齐窗口

换成当前运行时的物理/低地址视角，就是下面这张图：

```text
2MB windows for final init_pg_dir mapping

0x40200000 - 0x40400000  [ L2[0]  ]  split to 4KB PTE
	0x40200000 - 0x40210000  RW, NX
	0x40210000 - 0x40400000  ROX
					  ^
					  _stext

0x40400000 - 0x41000000  [ L2[1]..L2[6] ]  ROX 2MB blocks

0x41000000 - 0x41200000  [ L2[7] ]  split to 4KB PTE
	0x41000000 - 0x41100000  ROX
	0x41100000 - 0x41200000  RW, NX
					  ^
					  _etext == __start_rodata

0x41200000 - 0x41600000  [ L2[8]..L2[9] ]  RW 2MB blocks

0x41600000 - 0x41800000  [ L2[10] ]  split to 4KB PTE
	0x41600000 - 0x41660000  RW, NX
	0x41660000 - 0x41740000  ROX
	0x41740000 - 0x41800000  RW
					  ^             ^
					  __inittext_begin  __inittext_end/__initdata_begin

0x41800000 - 0x42400000  [ L2[11]..L2[16] ]  RW 2MB blocks

0x42400000 - 0x42600000  [ L2[17] ]  split to 4KB PTE
	0x42400000 - 0x42530000  RW
	0x42530000 - 0x42600000  hole
					  ^
					  _end
```

所以和第一次 init idmap 相比：

- 第一次映射只有 3 个特殊窗口
- 第二次映射有 4 个特殊窗口

多出来的那个窗口，本质上就是：

- `_etext == __start_rodata` 这一处权限发生了从 text `ROX` 到 rodata `RW,NX` 的切换


### 16.6.2 当前 build 下第二次映射的最终分段结果

如果直接把最终有效映射压成区间表，当前 build 下可以写成：

```text
0x40200000 - 0x40210000  -> 0xffff800080000000 - 0xffff800080010000  RW, NX,  L3 PTE
0x40210000 - 0x41000000  -> 0xffff800080010000 - 0xffff800080f00000  ROX,     mixed: L3 head + L2 block
0x41000000 - 0x41100000  -> 0xffff800080f00000 - 0xffff800081000000  ROX,     L3 PTE
0x41100000 - 0x41600000  -> 0xffff800081000000 - 0xffff800081500000  RW, NX,  mixed: L3 tail + L2 block
0x41600000 - 0x41660000  -> 0xffff800081500000 - 0xffff800081560000  RW, NX,  L3 PTE
0x41660000 - 0x41740000  -> 0xffff800081560000 - 0xffff800081640000  ROX,     L3 PTE
0x41740000 - 0x42400000  -> 0xffff800081640000 - 0xffff800082300000  RW,       mixed: L3 head + L2 block
0x42400000 - 0x42530000  -> 0xffff800082300000 - 0xffff800082430000  RW,       L3 PTE
```

上面这张表里，左边是当前物理/低地址执行视角，右边是第二次映射建立后的最终高地址 kernel VA。

如果只想记最核心的公式，就是：

```text
final kernel VA = current physical image address + va_offset
```


### 16.6.3 为什么第二次映射的特殊窗口比第一次更多

第一次 `create_init_idmap()` 只按两种权限划分：

- text = ROX
- data = RW

所以它只需要处理：

- 起始不对齐
- 中间一次权限切换
- 尾部不对齐

第二次 `map_kernel()` 则更细：

- `_text -> _stext` 是 `RW,NX`
- `_stext -> _etext` 是 `ROX`
- `__start_rodata -> __inittext_begin` 是 `RW,NX`
- `__inittext_begin -> __inittext_end` 是 `ROX`
- `__initdata_begin -> _end` 是 `RW`

因此它会在更多 2MB 窗口内出现“同一窗口里混合多段权限”的情况，所以特殊窗口自然更多。


### 16.6.4 顶层级数还要带一个运行时条件

这里还要记一个运行时条件：

- 配置上 `CONFIG_PGTABLE_LEVELS=5`
- `early_map_kernel()` 初始 `root_level = 4 - CONFIG_PGTABLE_LEVELS = -1`

但因为当前配置还打开了：

- `CONFIG_ARM64_LPA2=y`

所以如果启动 CPU **不支持** LPA2，代码会在 [arch/arm64/kernel/pi/map_kernel.c](arch/arm64/kernel/pi/map_kernel.c#L244) 附近执行：

```c
va_bits = VA_BITS_MIN;
root_level++;
```

也就是把根层从 `-1` 提到 `0`。

不过要注意：

- 这会影响顶层骨架是 5 级还是 4 级
- 但不会改变上面这些 2MB / 4KB 特殊窗口的低层结果

因为你前面关心的这些窗口，本质上都发生在 L2 / L3 这两层。


### 16.7 第二次映射还有一个“两遍建图”细节

`map_kernel()` 不是总是一遍就结束，它可能走 twopass：

```c
twopass = IS_ENABLED(CONFIG_RELOCATABLE) || enable_scs;
prot = twopass ? data_prot : text_prot;
```

意思是：

- 如果内核需要 relocation，或需要做 SCS/BTI 相关处理
- 那第一遍会先把 text 按更宽松的权限映射出来
- 等 relocation / patching 做完后，再把 text / inittext 重新 remap 成最终的 `text_prot`

所以第二次映射在语义上要分成：

1. 临时可修改的 early kernel mapping
2. 处理完 relocation/patching 之后的最终 early kernel mapping

这也是为什么代码里会看到：

- `unmap_segment(init_pg_dir, ..., _stext, _etext, ...)`
- 然后再 `map_segment(..., _stext, _etext, text_prot, ...)`


### 16.8 第二次映射完成后，TTBR1 是怎么切过去的

阶段上可以分成两跳：

1. `idmap_cpu_replace_ttbr1((phys_addr_t)init_pg_dir)`
	先把 TTBR1 从 `reserved_pg_dir` 切到刚刚构造好的临时 kernel mapping。
2. `memcpy((void *)swapper_pg_dir + va_offset, init_pg_dir, PAGE_SIZE)`
	把 root page table 拷到 `swapper_pg_dir`。
3. `idmap_cpu_replace_ttbr1((phys_addr_t)swapper_pg_dir)`
	再把 TTBR1 从 `init_pg_dir` 切到最终的 `swapper_pg_dir`。

所以第二次映射完成之后，真正稳定下来的状态是：

```text
TTBR0 -> __pi_init_idmap_pg_dir
TTBR1 -> swapper_pg_dir
```


### 16.9 第二次映射最好记的一句话

如果把第一次和第二次映射放在一起比较，最值得记住的是：

> 第一次映射解决“MMU 一开先别死，先靠 TTBR0 的 VA=PA idmap 活下来”；第二次映射解决“把当前物理镜像接到最终高地址 kernel VA 上，并把 TTBR1 切过去”。


## 17. `__enable_mmu()` 逐行解析

如果说 `map_range()` 负责“页表怎么长出来”，那么 [arch/arm64/kernel/head.S](arch/arm64/kernel/head.S#L459) 的 `__enable_mmu()` 负责的就是：

- 把哪张根页表挂到 `TTBR0`
- 把哪张根页表挂到 `TTBR1`
- 最后在什么时刻真正打开 MMU

最值得先记住的一句话是：

> `__enable_mmu()` 不是在“建页表”，而是在“把已经建好的页表根地址写进 TTBR，然后打开 MMU，让翻译正式生效”。


### 17.1 这个函数的输入在当前主启动路径里分别是什么

从 [arch/arm64/kernel/head.S](arch/arm64/kernel/head.S#L509) 的 `__primary_switch` 来看：

```asm
adrp x1, reserved_pg_dir
adrp x2, __pi_init_idmap_pg_dir
bl   __enable_mmu
```

所以当前主启动路径下：

- `x0` = 预先准备好的 `SCTLR_EL1` 目标值
- `x1` = `reserved_pg_dir`
- `x2` = `__pi_init_idmap_pg_dir`

也就是说，这次调用的直接目标是：

- `TTBR0 <- __pi_init_idmap_pg_dir`
- `TTBR1 <- reserved_pg_dir`
- 然后打开 MMU


### 17.2 带注释的“逐行版源码”

下面这版不是改源码，而是按理解顺序给它加解释。

```asm
SYM_FUNC_START(__enable_mmu)
	/*
	 * 读出内存模型特性寄存器，后面要检查当前 CPU 是否支持
	 * 内核所要求的翻译粒度（例如 4KB granule）。
	 */
	mrs x3, ID_AA64MMFR0_EL1

	/*
	 * 从 ID_AA64MMFR0_EL1 中抽取 TGRAN 字段。
	 * 这个字段描述当前 CPU 对目标 granule 的支持情况。
	 */
	ubfx x3, x3, #ID_AA64MMFR0_EL1_TGRAN_SHIFT, 4

	/*
	 * 如果 granule 支持度低于最小要求，就不能继续打开 MMU。
	 */
	cmp  x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MIN
	b.lt __no_granule_support

	/*
	 * 如果 granule 值高于允许范围，同样不能继续。
	 */
	cmp  x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MAX
	b.gt __no_granule_support

	/*
	 * 把 x2 里的根页表物理地址整理成 TTBR 格式。
	 * 当前主启动路径里，x2 = __pi_init_idmap_pg_dir。
	 */
	phys_to_ttbr x2, x2

	/*
	 * 把 TTBR0_EL1 指到 init idmap 的根页表。
	 * 注意：这里装进去的是“根页表基地址”，不是整张页表内容。
	 */
	msr ttbr0_el1, x2

	/*
	 * 把 TTBR1_EL1 指到 x1 所给的页表。
	 * 当前主启动路径里，x1 = reserved_pg_dir。
	 * load_ttbr1 宏内部会：
	 *   1. phys_to_ttbr
	 *   2. offset_ttbr1 （必要时处理 52-bit VA 场景）
	 *   3. msr ttbr1_el1
	 *   4. isb
	 */
	load_ttbr1 x1, x1, x3

	/*
	 * 这一步才是真正打开 MMU。
	 * 前面的 TTBR0/TTBR1 只是把“将来要用哪张表”准备好；
	 * 到这里 SCTLR_EL1.M 置位后，页表翻译才正式开始生效。
	 */
	set_sctlr_el1 x0

	ret
SYM_FUNC_END(__enable_mmu)
```


### 17.3 这段函数里最重要的 4 个动作

#### 17.3.1 动作一：先确认当前 CPU 支持所需 granule

前 4 条指令：

```asm
mrs  x3, ID_AA64MMFR0_EL1
ubfx x3, x3, #ID_AA64MMFR0_EL1_TGRAN_SHIFT, 4
cmp  x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MIN
cmp  x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MAX
```

它们的目的不是翻译地址，而是“在打开 MMU 之前先确认 CPU 硬件真的支持当前页表粒度”。

如果不支持，后面页表再正确也没法用，所以直接跳去 `__no_granule_support` 停机。


#### 17.3.2 动作二：把 TTBR0 指向 early idmap 根页表

这两句：

```asm
phys_to_ttbr x2, x2
msr ttbr0_el1, x2
```

当前主启动路径下，`x2 = __pi_init_idmap_pg_dir`。

对你当前 build：

- `__pi_init_idmap_pg_dir` 物理地址 = `0x41740000`
- 当前又有 `CONFIG_ARM64_PA_BITS_52=y`

所以写进 `TTBR0_EL1` 的本质内容是：

```text
TTBR0 root base = 0x41740000
```

这一步的含义一定要说准确：

- 不是“把 L0/L1/L2/L3 页表项装进寄存器”
- 而是“告诉硬件：TTBR0 那边的根页表从 0x41740000 这一页开始”

后续硬件真正做翻译时，才会：

1. 从 `TTBR0_EL1` 读到根页表基地址
2. 访问 `__pi_init_idmap_pg_dir`
3. 走 L0 -> L1 -> L2 -> L3


#### 17.3.3 动作三：把 TTBR1 指向当前阶段的安全占位页表

这句：

```asm
load_ttbr1 x1, x1, x3
```

当前主启动路径下，`x1 = reserved_pg_dir`。

所以这里的真实语义是：

```text
TTBR1 root base = reserved_pg_dir
```

它的目的不是“马上提供正式高地址 kernel VA 映射”，而是：

- MMU 一开时，TTBR1 也必须先有一张合法根表
- 但真正的 `init_pg_dir` / `swapper_pg_dir` 还没建好
- 所以先给 TTBR1 一张空的安全占位页表，也就是 `reserved_pg_dir`


#### 17.3.4 动作四：到 `set_sctlr_el1 x0` 才算真正生效

这是整个函数里最容易被忽略的点。

很多人会把：

```asm
msr ttbr0_el1, x2
msr ttbr1_el1, x1
```

理解成“页表已经开始工作了”，其实还差最后一步：

```asm
set_sctlr_el1 x0
```

更准确地说：

- 前面只是“把将要使用的页表根地址写进 TTBR”
- 到这里才是真正打开 MMU
- 从这一刻开始，后续取指和数据访问才进入页表翻译路径

所以如果只问“init idmap 具体什么时候开始生效”，答案应该是：

- `msr ttbr0_el1, x2` 把它挂到 TTBR0 上
- `set_sctlr_el1 x0` 才让它真正开始生效


### 17.4 当前主启动路径下，这个函数执行前后寄存器语义怎么变

可以把它简化成下面这张表：

```text
进入 __enable_mmu() 之前：
    x1 = reserved_pg_dir
    x2 = __pi_init_idmap_pg_dir
    MMU = off

执行 msr ttbr0_el1, x2 之后：
    TTBR0_EL1 = init idmap root base
    MMU 仍然 = off

执行 load_ttbr1 x1, x1, x3 之后：
    TTBR1_EL1 = reserved_pg_dir root base
    MMU 仍然 = off

执行 set_sctlr_el1 x0 之后：
    MMU = on
    TTBR0 开始使用 __pi_init_idmap_pg_dir
    TTBR1 开始使用 reserved_pg_dir
```


### 17.5 和前后两阶段放在一起看，最好记的时序

如果把 `create_init_idmap()`、`__enable_mmu()`、`__pi_early_map_kernel()` 连起来，主线就是：

```text
MMU off:
    create_init_idmap(__pi_init_idmap_pg_dir)
    // 只是把页表写进内存

__enable_mmu():
    TTBR0 = __pi_init_idmap_pg_dir
    TTBR1 = reserved_pg_dir
    MMU on

__pi_early_map_kernel():
    build init_pg_dir
    TTBR1 = init_pg_dir
    copy root to swapper_pg_dir
    TTBR1 = swapper_pg_dir
```

如果只留一句话，建议记成：

> `create_init_idmap()` 负责“把 TTBR0 需要的页表写到内存里”，`__enable_mmu()` 负责“把这张根表地址写进 TTBR0/TTBR1 并真正打开 MMU”。


## 18. `idmap_cpu_replace_ttbr1()` 逐行解析

如果 `__enable_mmu()` 解决的是“第一次把 TTBR1 指到一个合法根页表”，那么 [arch/arm64/mm/proc.S](arch/arm64/mm/proc.S#L179) 的 `idmap_cpu_replace_ttbr1()` 解决的就是：

- 在 MMU 已经打开以后
- 如何把 TTBR1 从旧页表安全切换到新页表
- 同时避免旧的和新的 table walk 结果在 TLB 里混在一起

最值得先记住的一句话是：

> `idmap_cpu_replace_ttbr1()` 不是直接把 TTBR1 改成新页表就结束了，而是先切到 `reserved_pg_dir` 这个安全空表，清掉旧 TLB，再切到真正的新页表。


### 18.1 它在启动流程里具体在哪两次被调用

在前面第二次映射 [arch/arm64/kernel/pi/map_kernel.c](arch/arm64/kernel/pi/map_kernel.c#L91) 里，`map_kernel()` 会调用它两次：

1. `idmap_cpu_replace_ttbr1((phys_addr_t)init_pg_dir)`
	把 TTBR1 从 `reserved_pg_dir` 切到刚构造好的临时 kernel mapping。
2. `idmap_cpu_replace_ttbr1((phys_addr_t)swapper_pg_dir)`
	把 TTBR1 从 `init_pg_dir` 再切到最终的 `swapper_pg_dir`。

所以它正好就是启动过程中 TTBR1 的“换表器”。


### 18.2 带注释的“逐行版源码”

先看它本体：

```asm
.macro __idmap_cpu_set_reserved_ttbr1, tmp1, tmp2
	adrp    tmp1, reserved_pg_dir
	phys_to_ttbr tmp2, tmp1
	offset_ttbr1 tmp2, tmp1
	msr     ttbr1_el1, tmp2
	isb
	tlbi    vmalle1
	dsb     nsh
	isb
.endm

SYM_TYPED_FUNC_START(idmap_cpu_replace_ttbr1)
	__idmap_cpu_set_reserved_ttbr1 x1, x3

	offset_ttbr1 x0, x3
	msr ttbr1_el1, x0
	isb

	ret
SYM_FUNC_END(idmap_cpu_replace_ttbr1)
```

按理解顺序解释，就是：

```asm
SYM_TYPED_FUNC_START(idmap_cpu_replace_ttbr1)
	/*
	 * 第一步：不要直接从“旧 TTBR1”切到“新 TTBR1”。
	 * 先把 TTBR1 放到 reserved_pg_dir 这张空根页表上。
	 */
	__idmap_cpu_set_reserved_ttbr1 x1, x3

	/*
	 * 第二步：把传进来的目标页表基地址 x0 按 TTBR1 规则做必要调整。
	 * 尤其是在某些 52-bit VA 场景下，TTBR1 可能需要 offset_ttbr1。
	 */
	offset_ttbr1 x0, x3

	/*
	 * 第三步：把 TTBR1 真正切到目标页表。
	 */
	msr ttbr1_el1, x0

	/*
	 * 第四步：用 isb 保证后续执行看到新的 TTBR1。
	 */
	isb

	ret
SYM_FUNC_END(idmap_cpu_replace_ttbr1)
```


### 18.3 为什么它必须先切到 `reserved_pg_dir`

这里最核心的逻辑就在宏 `__idmap_cpu_set_reserved_ttbr1`。

它做了：

```asm
adrp reserved_pg_dir
phys_to_ttbr
offset_ttbr1
msr ttbr1_el1, tmp2
isb
tlbi vmalle1
dsb nsh
isb
```

也就是说：

1. 先把 TTBR1 指到 `reserved_pg_dir`
2. 确保流水线看到这个切换
3. 清掉旧的 EL1 TLB 项
4. 等 TLB 失效真正完成

为什么不直接：

```asm
msr ttbr1_el1, new_table
```

因为那样会有一个风险：

- TLB 里还残留着旧页表 walk 得到的结果
- 同时你又把 TTBR1 切到了新页表
- 这样硬件可能会在某个过渡窗口里观察到“旧表和新表混合”的翻译结果

所以正确的安全顺序是：

```text
旧 TTBR1
  -> reserved_pg_dir
  -> invalidate TLB
  -> 新 TTBR1
```

这就是它为什么一定要先经过 `reserved_pg_dir`。


### 18.4 为什么这个函数必须从 TTBR0 映射下执行

源码注释里写得很明确：

> It can only be executed from a TTBR0 mapping.

原因其实很直接：

- 这个函数正在主动改 `TTBR1_EL1`
- 如果当前代码本身还依赖 TTBR1 提供的高地址 kernel 映射去取指
- 那你在切 TTBR1 的过程中，代码脚下的地址翻译基础就被你自己改掉了

这显然不安全。

所以它必须在 `.idmap.text` 中执行，也就是：

- 代码本身靠 TTBR0 的 idmap 存活
- 然后在这个“稳定的脚手架”上安全地替换 TTBR1

这也是为什么启动路径里：

- 先有 `__pi_init_idmap_pg_dir` 挂到 TTBR0
- 再在 idmap 代码里去换 TTBR1


### 18.5 这段函数里最重要的 3 个动作

#### 18.5.1 动作一：先把 TTBR1 切到空表

第一动作不是“切到新表”，而是：

- 先切到 `reserved_pg_dir`

这一步的价值是：

- 给 TTBR1 提供一张合法但空的根页表
- 把旧高地址映射先隔离开


#### 18.5.2 动作二：全局失效旧 TLB 结果

宏里这几句：

```asm
tlbi vmalle1
dsb nsh
isb
```

是这段代码的安全核心。

它的意义是：

- 旧 TTBR1 对应的 TLB 缓存结果不能继续带着进入新页表时代
- 所以必须先做一次完整的 EL1 TLB 失效


#### 18.5.3 动作三：再切到真正的新页表

等旧 TLB 结果清掉后，才执行：

```asm
offset_ttbr1 x0, x3
msr ttbr1_el1, x0
isb
```

这一步才是“把 TTBR1 切到目标页表”。

在启动时两次分别对应：

- `x0 = init_pg_dir`
- `x0 = swapper_pg_dir`


### 18.6 套到启动流程里，状态变化一眼看懂

第一次替换：

```text
TTBR0 = __pi_init_idmap_pg_dir
TTBR1 = reserved_pg_dir

build init_pg_dir

idmap_cpu_replace_ttbr1(init_pg_dir):
    TTBR1 -> reserved_pg_dir
    TLBI old entries
    TTBR1 -> init_pg_dir
```

第二次替换：

```text
TTBR0 = __pi_init_idmap_pg_dir
TTBR1 = init_pg_dir

copy root to swapper_pg_dir

idmap_cpu_replace_ttbr1(swapper_pg_dir):
    TTBR1 -> reserved_pg_dir
    TLBI old entries
    TTBR1 -> swapper_pg_dir
```


### 18.7 最好记的心智模型

把 `idmap_cpu_replace_ttbr1()` 想成一个“换桥墩”的动作最容易记：

- 你不能一边站在旧桥墩上，一边直接把桥墩换成新的
- 你得先站到一个临时安全平台上
- 把旧桥相关状态清理干净
- 再切到新桥墩

在这里：

- 临时安全平台 = `reserved_pg_dir`
- 旧桥相关状态 = 旧 TTBR1 的 TLB 结果
- 新桥墩 = `init_pg_dir` 或 `swapper_pg_dir`

如果只保留一句话，那就是：

> `idmap_cpu_replace_ttbr1()` 的本质是“站在 TTBR0 的 idmap 脚手架上，先切到 `reserved_pg_dir` 并清旧 TLB，再把 TTBR1 安全切到新页表”。


## 19. 按启动流程的 GDB 验证清单

如果前面这些分析都想在一次调试里串起来，最好的办法不是零散地下断点，而是按启动流程顺着看 5 个关键点：

1. `create_init_idmap()` 入口
2. `create_init_idmap()` 返回
3. `__enable_mmu()` 入口
4. `__pi_early_map_kernel()` / `map_kernel()` 期间的 TTBR1 替换
5. `__primary_switched` 之后的高地址内核执行

下面这份清单默认你已经在用当前这份 `~/.gdbinit`，其中已经定义了：

- `mmu_off_setup`
- `mmu_on_setup`
- `wherepc`
- `idmapinfo`
- 一组 `$pa_*` 物理地址变量


### 19.1 第一阶段：MMU off，验证第一次映射

先启动 GDB：

```gdb
source ~/.gdbinit
mmu_off_setup
c
```

这一阶段会停在若干物理断点上。最关键的是：

- `*$pa_pi_create_init_idmap`
- `*$pa_enable_mmu`
- `*$pa_primary_switch`


#### 19.1.1 停在 `create_init_idmap()` 入口

到达 `create_init_idmap()` 后，先看当前位置和寄存器：

```gdb
wherepc
info reg x0 x1 x2 x30 sp pc
```

这里当前主启动路径下最关键的是：

- `x0` 指向 `__pi_init_idmap_pg_dir`
- MMU 仍然是 off

如果要直接确认传进来的根页表物理地址：

```gdb
p/x $x0
p/x $pa_pi_init_idmap_pg_dir
```

预期两者一致。


#### 19.1.2 验证 `create_init_idmap()` 返回值和页表布局

在函数返回处：

```gdb
finish
p/x $x0
idmapinfo
```

预期：

```text
$x0 = $pa_pi_init_idmap_pg_dir + 0x6000
```

然后继续检查第一次映射那 6 页页表：

```gdb
x/gx $pa_pi_init_idmap_pg_dir + 8*256
x/gx $pa_pi_init_idmap_l1 + 8*2
x/18gx $pa_pi_init_idmap_l2
x/gx $pa_pi_init_idmap_l3_head + 8*16
x/gx $pa_pi_init_idmap_l3_mid + 8*319
x/gx $pa_pi_init_idmap_l3_mid + 8*320
x/gx $pa_pi_init_idmap_l3_tail + 8*303
```

这一组检查对应的是：

- TTBR0 还没生效前，页表已经被完整写进内存
- 但此时只是“建表完成”，不是“翻译已经生效”


### 19.2 第二阶段：停在 `__enable_mmu()`，看 TTBR0/TTBR1 如何生效

继续运行到 `__enable_mmu()`：

```gdb
c
wherepc
info reg x0 x1 x2 x3 pc
```

这里当前主路径下预期是：

- `x1 = reserved_pg_dir`
- `x2 = __pi_init_idmap_pg_dir`

也就是说即将发生：

```text
TTBR0 <- __pi_init_idmap_pg_dir
TTBR1 <- reserved_pg_dir
```

如果要单步看生效点，建议用 `si` 而不是 `next`：

```gdb
si
si
si
```

在执行完：

- `msr ttbr0_el1, x2`
- `load_ttbr1 x1, x1, x3`

之后，可以查看系统寄存器：

```gdb
info reg ttbr0_el1 ttbr1_el1 sctlr_el1 tcr_el1
```

重点观察：

- `ttbr0_el1` 已经指向 `__pi_init_idmap_pg_dir`
- `ttbr1_el1` 已经指向 `reserved_pg_dir`
- 但在 `set_sctlr_el1 x0` 之前，MMU 还没真正开始工作

继续单步越过 `set_sctlr_el1 x0` 后，再看一次：

```gdb
si
info reg ttbr0_el1 ttbr1_el1 sctlr_el1
```

这一步之后，前面第一阶段建出来的 TTBR0 idmap 才真正开始生效。


### 19.3 第三阶段：停在 `__pi_early_map_kernel()`，开始观察第二次映射

继续运行：

```gdb
c
```

到 `__primary_switched` 时，`~/.gdbinit` 会自动卸载物理重定位符号并回到高地址符号视角。此后建议装上 MMU-on 断点：

```gdb
mmu_on_setup
hbreak __pi_early_map_kernel
hbreak map_kernel
hbreak idmap_cpu_replace_ttbr1
c
```

在 `__pi_early_map_kernel()` 里，先看：

```gdb
wherepc
info reg x0 x1
```

这里要记住：

- TTBR0 仍然靠第一次映射支撑
- 现在开始建的是 TTBR1 一侧的第二次映射


### 19.4 第四阶段：验证 `init_pg_dir` 建好后 TTBR1 如何替换

最关键的观察点是 `idmap_cpu_replace_ttbr1()` 两次被调用的时候。

第一次停住时：

```gdb
wherepc
info reg x0 ttbr1_el1
```

预期：

- `x0 = init_pg_dir` 的物理地址
- 这是第一次把 TTBR1 从 `reserved_pg_dir` 切到可用的临时 kernel mapping

第二次停住时再看：

```gdb
wherepc
info reg x0 ttbr1_el1
```

预期：

- `x0 = swapper_pg_dir` 的物理地址
- 这是第二次把 TTBR1 从 `init_pg_dir` 切到最终的正式根页表

如果想明确验证“不是直接旧表 -> 新表，而是先切到 reserved 再清 TLB 再切新表”，可以在 `idmap_cpu_replace_ttbr1()` 里单步：

```gdb
layout asm
si
si
si
si
```

重点看这几步：

- `__idmap_cpu_set_reserved_ttbr1`
- `tlbi vmalle1`
- `msr ttbr1_el1, x0`


### 19.5 第五阶段：验证最终稳定状态

等 `map_kernel()` 跑完、系统进入高地址内核执行后，再检查一次系统寄存器：

```gdb
info reg ttbr0_el1 ttbr1_el1 sctlr_el1 tcr_el1
```

从启动主线角度，此时最重要的结论应该是：

```text
TTBR0 -> __pi_init_idmap_pg_dir   // 仍是 early idmap 脚手架
TTBR1 -> swapper_pg_dir           // 正式内核页表根
MMU    = on
```


### 19.6 一条龙最小命令顺序

如果你只想按最少命令复现整个流程，可以直接按下面顺序走：

```gdb
source ~/.gdbinit
mmu_off_setup
c                     # 到 create_init_idmap / enable_mmu 等物理断点
wherepc
finish                # 从 create_init_idmap 返回时看 x0
idmapinfo
c                     # 到 __primary_switched
mmu_on_setup
hbreak __pi_early_map_kernel
hbreak map_kernel
hbreak idmap_cpu_replace_ttbr1
c
wherepc
info reg ttbr0_el1 ttbr1_el1 sctlr_el1
```

如果中间要精看某个切换点，就在对应函数里切到：

```gdb
layout asm
si
```


### 19.7 这套 GDB 流程最好记的一句话

如果把这套调试流程压成一句话，建议记成：

> 先在 MMU-off 阶段确认“页表写进内存了”，再在 `__enable_mmu()` 确认“TTBR 根地址装进寄存器了”，最后在 `idmap_cpu_replace_ttbr1()` 确认“TTBR1 从 reserved 切到了 init，再切到了 swapper”。


## 20. `swapper_pg_dir` 接手之后：`paging_init()` / `map_mem()` 又做了什么

前面那条主线到这里其实只完成了两件关键事：

1. 让 CPU 已经能站在高地址内核 VA 上继续执行
2. 让 `TTBR1` 的正式根页变成 `swapper_pg_dir`

但这还不等于“整个正式内核地址空间已经全部建完”。

真正继续把长期内核映射补齐的入口，是：

- [arch/arm64/mm/mmu.c](arch/arm64/mm/mmu.c#L1351) `paging_init()`
- [arch/arm64/mm/mmu.c](arch/arm64/mm/mmu.c#L1353) `map_mem(swapper_pg_dir)`


### 20.1 先说结论：`map_mem()` 不是重做前面的 kernel image mapping

`map_kernel()` 和 `map_mem()` 容易混，但职责不同：

- `map_kernel()`：早期临时阶段，把“当前物理装载的内核镜像”接到最终高地址 kernel VA 上，让内核先跑起来。
- `map_mem()`：进入常规内存初始化后，在 `swapper_pg_dir` 上补齐正式 **linear map**，并处理 kernel image 在线性别名中的保护属性。

所以 `map_mem()` 不是“把前面 `init_pg_dir -> swapper_pg_dir` 那棵树再重建一遍”，而是：

```text
已有：kernel image 的高地址执行映射
继续补：整个 RAM 的 linear map
继续调：kernel text/rodata 在线性别名里的权限
```


### 20.2 `paging_init()` 入口很短，但语义非常重

代码只有几行：

```c
void __init paging_init(void)
{
	map_mem(swapper_pg_dir);

	memblock_allow_resize();

	create_idmap();
	declare_kernel_vmas();
}
```

这一段可以压成：

1. 用 `swapper_pg_dir` 建正式内核内存映射
2. 建常规运行期用的 `idmap_pg_dir`
3. 把内核几个主要 VA 区间登记成 VMA 元数据

所以从生命周期看：

- `map_kernel()` 解决“先跑起来”
- `paging_init()` 解决“开始进入长期稳定布局”


### 20.3 `map_mem()` 最关键的动作：把所有 memblock RAM 建成 linear map

`map_mem()` 的核心代码是：

```c
/* map all the memory banks */
for_each_mem_range(i, &start, &end) {
	if (start >= end)
		break;

	__map_memblock(pgdp, start, end, pgprot_tagged(PAGE_KERNEL), flags);
}
```

这里的意思非常直接：

- 遍历 memblock 记录下来的所有物理 RAM 段
- 对每一段都建立 `__phys_to_virt(pa)` 形式的 linear mapping
- 根页表就是 `swapper_pg_dir`

也就是说，从这一刻开始，内核要的不是仅仅：

```text
物理内核镜像 0x40200000..0x42530000
	-> 高地址 _text.._end
```

而是更大范围的：

```text
整片 RAM 的 phys addr
	-> linear map 区域的 kernel VA
```

这就是 ARM64 内核后面大量 `phys_to_virt()` / `virt_to_phys()`、页分配器、memblock、伙伴系统初始化等代码能正常工作的基础。


### 20.4 为什么 `map_mem()` 要先把 kernel image 标成 NOMAP

你会看到 `map_mem()` 一开始做了：

```c
phys_addr_t kernel_start = __pa_symbol(_text);
phys_addr_t kernel_end = __pa_symbol(__init_begin);

memblock_mark_nomap(kernel_start, kernel_end - kernel_start);
```

后面扫 `for_each_mem_range()` 时，这段区间就会被暂时跳过。

原因是注释里写得很清楚：

> 不要给 kernel text/rodata 建一个可写别名。

如果不先跳过，按普通 RAM 统一去做 linear map，会把：

- 内核 `.text`
- rodata
- 到 `__init_begin` 前这一整段镜像内容

一起按 `PAGE_KERNEL` 那类普通线性映射方式映进去，这样会产生一个“写权限过宽”的 linear alias。

这和前面 `map_kernel()` 里为 `_stext.._etext`、`__start_rodata..__inittext_begin` 精细区分权限的目标冲突。


### 20.5 `map_mem()` 如何单独处理 kernel image 的 linear alias

跳过 kernel image 后，`map_mem()` 又单独补了一次：

```c
__map_memblock(pgdp, kernel_start, kernel_end,
			   PAGE_KERNEL, NO_CONT_MAPPINGS);
memblock_clear_nomap(kernel_start, kernel_end - kernel_start);
```

这里的含义是：

1. 在线性映射区域里，仍然要给 kernel image 建一个 linear alias
2. 但这次是单独处理，不跟普通 RAM 一起粗暴合并
3. 先映成 `PAGE_KERNEL`、禁止 contiguous mappings，后面再进一步收紧权限

注意这里建立的是：

- **kernel image 的 linear alias**

不是前面 `map_kernel()` 给你做的那份：

- **kernel image 的最终高地址执行映射**

这两套 VA 是并存的，职责不同。


### 20.6 为什么说这里新增的是“linear alias”，不是“重新建立执行映射”

把这两类映射分开看最清楚：

第一类，前面 `map_kernel()` 建的：

```text
0x40210000 -> 0xffff800080010000   (_stext 高地址执行映射)
```

第二类，后面 `map_mem()` 建的：

```text
0x40210000 -> __phys_to_virt(0x40210000)   (linear alias)
```

这两者都可能指向同一份物理内容，但 VA 不同、用途不同：

- 前者给内核正文按链接地址/重定位后的高地址执行
- 后者给“整片物理内存”的线性访问模型服务

所以你可以把 `swapper_pg_dir` 理解成最终同时承载两大类内容：

1. kernel image 的高地址映射
2. 全部 RAM 的 linear map

后面还会在这个正式根页表上继续挂更多长期区域。


### 20.7 `mark_rodata_ro()`：linear alias 的保护还会继续收紧

`map_mem()` 给 kernel image linear alias 先建出来后，并没有一步到位完成最终只读保护。

后面还有：

- [arch/arm64/mm/mmu.c](arch/arm64/mm/mmu.c#L1136) `mark_rodata_ro()`

它会进一步把：

- `__start_rodata .. __init_begin` 标成只读
- `_text .. _stext` 标成只读

也就是说，这条线是：

```text
map_mem()           -> 先给 linear alias 建立可用映射
mark_rodata_ro()    -> 再把不该写的部分收紧成只读
```

所以 `map_mem()` 不是最终权限的终点，而是正式内核页表继续完善的起点。


### 20.8 这时 `swapper_pg_dir` 里到底比刚接手 TTBR1 时多了什么

和刚从 `init_pg_dir` 切过来的那个瞬间相比，`paging_init()` / `map_mem()` 最本质的新增内容是：

1. 整个 memblock RAM 的 linear map
2. kernel image 在线性映射区域里的 alias
3. 后续 `create_idmap()` 所需的常规 idmap 支撑
4. 后续 VMA 元数据登记

而不是：

1. 再重新创建一遍 `_text.._end` 的高地址执行映射
2. 再重建一棵新的早期 kernel image 页表树

所以时序上可以压成：

```text
map_kernel()
	-> 先解决“高地址内核怎么跑起来”

copy root to swapper_pg_dir
TTBR1 = swapper_pg_dir
	-> 正式根页表接管

paging_init()
	-> 在正式根页表上补齐长期内核映射
```


### 20.9 最值得记的一句话

如果只记一句话，建议记成：

> `init_pg_dir -> swapper_pg_dir` 解决的是“谁来当正式 TTBR1 根”，而 `map_mem(swapper_pg_dir)` 解决的是“正式根页表接手后，如何把整个 RAM 的 linear map 和长期内核映射补齐”。


## 21. TTBR0 / TTBR1 基地址变迁全时序

如果只想先抓住结论，最值得记的是下面这张总表：

```text
Primary CPU early boot

MMU off
	TTBR0 = 不关心/尚未由这条路径建立语义
	TTBR1 = 不关心/尚未由这条路径建立语义

__enable_mmu()
	TTBR0 = __pi_init_idmap_pg_dir
	TTBR1 = reserved_pg_dir

map_kernel() built
	TTBR0 = __pi_init_idmap_pg_dir
	TTBR1 = init_pg_dir

root copied + replace_ttbr1()
	TTBR0 = __pi_init_idmap_pg_dir
	TTBR1 = swapper_pg_dir

Secondary CPU boot

__enable_mmu()
	TTBR0 = idmap_pg_dir
	TTBR1 = swapper_pg_dir

Runtime idmap install/uninstall

install:
	TTBR0 = reserved_pg_dir -> idmap_pg_dir
	TTBR1 = swapper_pg_dir (通常不变)

uninstall:
	TTBR0 = reserved_pg_dir -> current->mm->pgd 或继续 reserved
	TTBR1 = swapper_pg_dir (通常不变)
```

下面把这张表彻底展开，并把“符号地址 / 物理地址 / 寄存器实际值”这三层分开讲清楚。


### 21.1 先分清 3 种“基地址”含义

讨论 TTBR 时，最容易混的是下面 3 层：

1. **链接符号地址**

	 例如你在 `vmlinux` 里看到：

	 ```text
	 swapper_pg_dir        = 0xffff80008145f000
	 __pi_init_idmap_pg_dir = 0xffff800081540000
	 ```

	 这是链接视角/内核高地址符号视角。

2. **实际物理根页地址**

	 TTBR 真正关心的是这张根页表所在的物理页。例如你当前 build：

	 ```text
	 __pi_init_idmap_pg_dir  phys = 0x41740000
	 idmap_pg_dir            phys = 0x4165c000
	 reserved_pg_dir         phys = 0x4165e000
	 swapper_pg_dir          phys = 0x4165f000
	 __pi_init_pg_dir        phys = 0x42515000
	 ```

3. **写进 TTBR 寄存器的编码值**

	 真正写寄存器前，还会经过：

	 - [arch/arm64/include/asm/assembler.h](arch/arm64/include/asm/assembler.h#L606) `phys_to_ttbr`
	 - [arch/arm64/include/asm/assembler.h](arch/arm64/include/asm/assembler.h#L589) `offset_ttbr1`

	 也就是说，寄存器里存的不是“C 语言符号地址”，而是：

	 ```text
	 TTBR value = phys_to_ttbr(root_phys)
	 TTBR1 value = offset_ttbr1(phys_to_ttbr(root_phys))   // 仅部分配置会额外变动
	 ```

对你当前这份内核：

- `CONFIG_ARM64_PA_BITS_52=y`
- `CONFIG_ARM64_VA_BITS_52=y`
- `CONFIG_ARM64_LPA2=y`

所以：

- `phys_to_ttbr()` 会按 52-bit PA 规则整理地址
- 但对你这里这些低物理地址，整理后数值没有变化
- `offset_ttbr1()` 在当前配置下是 no-op，因为它只在 `CONFIG_ARM64_VA_BITS_52 && !CONFIG_ARM64_LPA2` 时才真正改值

所以在**你当前 build 的这几个根页表地址上**，我们可以近似直接把：

```text
TTBR register value == root table physical address
```

这只是当前配置和当前地址下成立，逻辑上仍然要记住中间还有 `phys_to_ttbr()` / `offset_ttbr1()` 这两步。


### 21.2 主 CPU 早期启动：TTBR0 / TTBR1 分阶段怎么变

主启动路径的关键代码链是：

- [arch/arm64/kernel/head.S](arch/arm64/kernel/head.S#L508) `__primary_switch`
- [arch/arm64/kernel/head.S](arch/arm64/kernel/head.S#L459) `__enable_mmu`
- [arch/arm64/kernel/pi/map_kernel.c](arch/arm64/kernel/pi/map_kernel.c#L39) `map_kernel`
- [arch/arm64/mm/proc.S](arch/arm64/mm/proc.S#L174) `idmap_cpu_replace_ttbr1`

按阶段展开如下。


#### 阶段 P0：MMU 还没开，先把页表写到内存里

在 [arch/arm64/kernel/head.S](arch/arm64/kernel/head.S#L92) `primary_entry` 里，内核先调用 `__pi_create_init_idmap` 去填：

- `__pi_init_idmap_pg_dir`

这一步结束后，状态是：

```text
内存里已经有一份可用的 early init idmap
但 TTBR0/TTBR1 还没有被这条主路径正式装入新的根页表语义
```

这里故意不要把“此时 TTBR0/TTBR1 一定等于某个值”说死，因为在 MMU off 场景下，关键点不在寄存器旧值，而在于：**当前代码还没有依赖这两个寄存器开始翻译。**


#### 阶段 P1：第一次开 MMU

在 [arch/arm64/kernel/head.S](arch/arm64/kernel/head.S#L508) `__primary_switch`：

```asm
adrp    x1, reserved_pg_dir
adrp    x2, __pi_init_idmap_pg_dir
bl      __enable_mmu
```

进入 [arch/arm64/kernel/head.S](arch/arm64/kernel/head.S#L459) `__enable_mmu` 后：

```asm
phys_to_ttbr x2, x2
msr ttbr0_el1, x2
load_ttbr1 x1, x1, x3
set_sctlr_el1 x0
```

所以第一次开 MMU 的寄存器基地址变化是：

```text
TTBR0: 未依赖 -> __pi_init_idmap_pg_dir
TTBR1: 未依赖 -> reserved_pg_dir
```

结合当前 build 的实际值：

```text
TTBR0_EL1 <- 0x41740000   (__pi_init_idmap_pg_dir)
TTBR1_EL1 <- 0x4165e000   (reserved_pg_dir)
```

这一步最关键的语义是：

- `TTBR0` 开始承担“低地址 idmap 脚手架”
- `TTBR1` 只是拿一张合法空表先占位


#### 阶段 P2：`map_kernel()` 建好临时正式 kernel 页表后，TTBR1 切到 `init_pg_dir`

在 [arch/arm64/kernel/pi/map_kernel.c](arch/arm64/kernel/pi/map_kernel.c#L99)：

```c
idmap_cpu_replace_ttbr1((phys_addr_t)init_pg_dir);
```

而 [arch/arm64/mm/proc.S](arch/arm64/mm/proc.S#L181) 的底层行为是：

```asm
ttbr1_el1 <- reserved_pg_dir
tlbi vmalle1
ttbr1_el1 <- init_pg_dir
```

所以从“阶段结果”看：

```text
TTBR0 = __pi_init_idmap_pg_dir   (不变)
TTBR1 = init_pg_dir              (第一次正式接管高地址内核映射)
```

对当前 build：

```text
TTBR0_EL1 = 0x41740000
TTBR1_EL1 = 0x42515000   (__pi_init_pg_dir / init_pg_dir)
```

注意这里 `TTBR1` 虽然最终落在 `init_pg_dir`，但切换过程里一定会先回到 `reserved_pg_dir` 做一次 break-before-make。


#### 阶段 P3：把根页复制到 `swapper_pg_dir` 后，TTBR1 再切一次

在 [arch/arm64/kernel/pi/map_kernel.c](arch/arm64/kernel/pi/map_kernel.c#L136) 到 [arch/arm64/kernel/pi/map_kernel.c](arch/arm64/kernel/pi/map_kernel.c#L138)：

```c
memcpy((void *)swapper_pg_dir + va_offset, init_pg_dir, PAGE_SIZE);
dsb(ishst);
idmap_cpu_replace_ttbr1((phys_addr_t)swapper_pg_dir);
```

阶段结果：

```text
TTBR0 = __pi_init_idmap_pg_dir
TTBR1 = swapper_pg_dir
```

对当前 build：

```text
TTBR0_EL1 = 0x41740000
TTBR1_EL1 = 0x4165f000   (swapper_pg_dir)
```

这时主 CPU 已经完成：

- `TTBR0` 仍保留最早那份 early init idmap 脚手架
- `TTBR1` 则已经稳定在正式根页 `swapper_pg_dir`


#### 阶段 P4：进入 `paging_init()`，正式映射继续扩展，但 TTBR 基地址不因为 `map_mem()` 再改一次

在 [arch/arm64/mm/mmu.c](arch/arm64/mm/mmu.c#L1351) `paging_init()` 里：

```c
map_mem(swapper_pg_dir);
create_idmap();
```

这里做的是：

- 在 `swapper_pg_dir` 指向的那棵正式页表树上继续补 linear map
- 创建常规运行期的 `idmap_pg_dir`

但它**不是**再把 TTBR1 改到另一个根，因此阶段结果仍然是：

```text
TTBR0 = __pi_init_idmap_pg_dir   // 此时仍是早期脚手架
TTBR1 = swapper_pg_dir
```

也就是说，`map_mem()` 扩展的是 `swapper_pg_dir` 所代表的页表内容，不是更换 `TTBR1` 根页表基地址本身。


### 21.3 次级 CPU 启动路径和主 CPU 不同：它直接拿现成结果

次级 CPU 的关键代码在 [arch/arm64/kernel/head.S](arch/arm64/kernel/head.S#L364) `secondary_startup`：

```asm
adrp    x1, swapper_pg_dir
adrp    x2, idmap_pg_dir
bl      __enable_mmu
```

这和主 CPU 最大的不同是：

- 主 CPU 第一次开 MMU 时，`TTBR0 = __pi_init_idmap_pg_dir`，`TTBR1 = reserved_pg_dir`
- 次级 CPU 起来时，主 CPU 已经把正式环境准备好了，所以它直接：

```text
TTBR0 = idmap_pg_dir
TTBR1 = swapper_pg_dir
```

对当前 build：

```text
TTBR0_EL1 <- 0x4165c000   (idmap_pg_dir)
TTBR1_EL1 <- 0x4165f000   (swapper_pg_dir)
```

所以可以把它压成一句话：

> 主 CPU 要自己搭桥；次级 CPU 直接走主 CPU 已经搭好的桥。


### 21.4 运行期 `TTBR0` 的常规切换：`reserved_pg_dir <-> idmap_pg_dir <-> current->mm->pgd`

启动完成后，`TTBR1` 通常长期稳定在 `swapper_pg_dir`，而 `TTBR0` 才是经常切换的那一个。

最典型的是 [arch/arm64/include/asm/mmu_context.h](arch/arm64/include/asm/mmu_context.h#L100) `cpu_uninstall_idmap()` 和 [arch/arm64/include/asm/mmu_context.h](arch/arm64/include/asm/mmu_context.h#L111) `cpu_install_idmap()`。


#### 安装 idmap：`cpu_install_idmap()`

代码顺序是：

```c
cpu_set_reserved_ttbr0();
local_flush_tlb_all();
cpu_set_idmap_tcr_t0sz();
cpu_switch_mm(lm_alias(idmap_pg_dir), &init_mm);
```

所以从 TTBR0 基地址角度看：

```text
TTBR0 = reserved_pg_dir   ->   idmap_pg_dir
TTBR1 = swapper_pg_dir    ->   swapper_pg_dir   (通常不变)
```

这里 `reserved_pg_dir` 的作用就是切换前的安全隔离根页。


#### 卸载 idmap：`cpu_uninstall_idmap()`

代码顺序是：

```c
cpu_set_reserved_ttbr0();
local_flush_tlb_all();
cpu_set_default_tcr_t0sz();

if (mm != &init_mm && !system_uses_ttbr0_pan())
		cpu_switch_mm(mm->pgd, mm);
```

所以结果是：

```text
TTBR0 = idmap_pg_dir      ->   reserved_pg_dir   ->   current->mm->pgd
```

或者如果当前不该装普通用户 pgd，就停在：

```text
TTBR0 = reserved_pg_dir
```

这条路径最关键的认识是：

- `TTBR0` 在运行期承担“用户/临时 idmap/空表隔离”这类动态角色
- `TTBR1` 则大体保持 `swapper_pg_dir` 这条稳定主线


### 21.5 运行期还有一条可选的 `TTBR1` 变迁线：`tramp_pg_dir <-> swapper_pg_dir`

如果启用了 KPTI/entry trampoline，运行期还有一条额外的 `TTBR1` 切换线，这个你前面也已经碰到过：

- [arch/arm64/kernel/entry.S](arch/arm64/kernel/entry.S#L621)
- [arch/arm64/kernel/entry.S](arch/arm64/kernel/entry.S#L641)

它的语义不是 early boot 构表，而是：

```text
异常入口/返回时
TTBR1 = tramp_pg_dir  <->  swapper_pg_dir
```

这条线属于运行期隔离/异常入口机制，和前面主 CPU 启动时的：

```text
reserved_pg_dir -> init_pg_dir -> swapper_pg_dir
```

不是一回事，但两者都属于“TTBR 基地址会变，而且变化的核心是根页表地址而不是页表项内容本身”。


### 21.6 把你当前 build 的实际基地址串成一条时间线

如果直接用你这份内核当前的实际值，把主 CPU 最关键的几步写成时间线，就是：

```text
P0  MMU off
		TTBR0 = 不依赖
		TTBR1 = 不依赖

P1  __enable_mmu()
		TTBR0 = 0x41740000   (__pi_init_idmap_pg_dir)
		TTBR1 = 0x4165e000   (reserved_pg_dir)

P2  idmap_cpu_replace_ttbr1(init_pg_dir)
		TTBR0 = 0x41740000
		TTBR1 = 0x42515000   (__pi_init_pg_dir / init_pg_dir)

P3  idmap_cpu_replace_ttbr1(swapper_pg_dir)
		TTBR0 = 0x41740000
		TTBR1 = 0x4165f000   (swapper_pg_dir)

P4  paging_init()/map_mem()
		TTBR0 = 0x41740000   (基地址本身不因 map_mem 改变)
		TTBR1 = 0x4165f000

S1  secondary_startup -> __enable_mmu()
		TTBR0 = 0x4165c000   (idmap_pg_dir)
		TTBR1 = 0x4165f000   (swapper_pg_dir)
```

这张表里最值得你特别记住的是两点：

1. 主 CPU 和次级 CPU 的第一次 `TTBR0/TTBR1` 装载并不相同。
2. `map_mem(swapper_pg_dir)` 改的是 `swapper_pg_dir` 这棵树的内容，不是把 TTBR1 再换到别的根页。


### 21.7 一句话记忆法

如果把 TTBR0 / TTBR1 的基地址变迁压成一句话，可以记成：

> 主 CPU 用 `TTBR0=__pi_init_idmap_pg_dir` 把自己先带过桥，再让 `TTBR1` 依次从 `reserved_pg_dir` 切到 `init_pg_dir`、再切到 `swapper_pg_dir`；而次级 CPU 起来时，已经可以直接用 `TTBR0=idmap_pg_dir`、`TTBR1=swapper_pg_dir`。


### 21.8 运行期进程切换时，TTBR0 / TTBR1 到底改了什么

前面讲的是“根页表基地址换成谁”。到了运行期，事情会更细一点：

- `TTBR0` 经常真的换 base
- `TTBR1` 大多数时候 **base 不换**，只是同一个 `swapper_pg_dir` 上挂不同 ASID

这条路径的核心代码在 [arch/arm64/mm/context.c](arch/arm64/mm/context.c#L335) `cpu_do_switch_mm()`：

```c
unsigned long ttbr1 = read_sysreg(ttbr1_el1);
unsigned long asid = ASID(mm);
unsigned long ttbr0 = phys_to_ttbr(pgd_phys);

...

/* Set ASID in TTBR1 since TCR.A1 is set */
ttbr1 &= ~TTBR_ASID_MASK;
ttbr1 |= FIELD_PREP(TTBR_ASID_MASK, asid);

cpu_set_reserved_ttbr0_nosync();
write_sysreg(ttbr1, ttbr1_el1);
write_sysreg(ttbr0, ttbr0_el1);
isb();
```

这里最该抓住的一点是注释那句：

```text
Set ASID in TTBR1 since TCR.A1 is set
```

也就是说，运行期对 `TTBR1_EL1` 的常规更新，很多时候并不是：

```text
swapper_pg_dir -> 别的根页表
```

而是：

```text
swapper_pg_dir + old ASID -> swapper_pg_dir + new ASID
```

所以如果你只盯着“TTBR1 寄存器值变了没”，容易误以为它换了根页表；但更精确的说法是：

- **TTBR1 的寄存器值可能变**
- 但其中变的往往是 ASID 位，不是 base address 部分


#### 21.8.1 `cpu_do_switch_mm()` 的阶段图

把这段代码按动作拆开，就是：

```text
1. 从当前 TTBR1 读出寄存器值
2. 保留 TTBR1 的 base（通常仍是 swapper_pg_dir）
3. 只把其中的 ASID 字段替换成新 mm 的 ASID
4. TTBR0 先临时切到 reserved_pg_dir
5. 再把 TTBR0 切到新 mm->pgd
```

对应的逻辑图：

```text
TTBR1 : [ swapper_pg_dir base | old ASID ]
					  |
					  v
		[ swapper_pg_dir base | new ASID ]

TTBR0 : [ old user pgd ]
		   -> [ reserved_pg_dir ]
		   -> [ new user pgd ]
```

所以运行期进程切换的典型形态是：

```text
TTBR0 变 base
TTBR1 多数情况下不变 base，只变 ASID
```


#### 21.8.2 为什么 `TTBR0` 要先切到 `reserved_pg_dir`

你会注意到 [arch/arm64/mm/context.c](arch/arm64/mm/context.c#L352) 没有直接：

```c
write_sysreg(ttbr0, ttbr0_el1);
```

而是先：

```c
cpu_set_reserved_ttbr0_nosync();
write_sysreg(ttbr1, ttbr1_el1);
write_sysreg(ttbr0, ttbr0_el1);
```

这和前面 early boot 用 `reserved_pg_dir` 做隔离的思想完全一致：

- 先让 `TTBR0` 暂时失效到一张安全空表
- 再更新和新地址空间相关的 TTBR 信息
- 最后再把新的 `TTBR0` 装进去

所以 `reserved_pg_dir` 不只是 early boot 才有用，它在运行期的 TTBR0 切换里同样承担“安全过桥板”的职责。


### 21.9 如果启用 SW PAN，内核入口/出口时 `TTBR0` 还会额外抖动一次

如果开启的是软件模拟 PAN 路径，也就是：

```text
CONFIG_ARM64_SW_TTBR0_PAN=y
```

那么每次内核入口/退出，`TTBR0` 还会多出一条非常典型的变迁线。

关键代码在：

- [arch/arm64/include/asm/asm-uaccess.h](arch/arm64/include/asm/asm-uaccess.h#L13)
- [arch/arm64/kernel/entry.S](arch/arm64/kernel/entry.S#L474)


#### 21.9.1 进入内核时：把 `TTBR0` 改成 `reserved_pg_dir`

在 [arch/arm64/kernel/entry.S](arch/arm64/kernel/entry.S#L474) 的 `__swpan_entry_el1`：

```asm
mrs x21, ttbr0_el1
...
__uaccess_ttbr0_disable x21
```

而 [arch/arm64/include/asm/asm-uaccess.h](arch/arm64/include/asm/asm-uaccess.h#L13) 的 `__uaccess_ttbr0_disable` 本体是：

```asm
mrs tmp1, ttbr1_el1                 // swapper_pg_dir
bic tmp1, tmp1, #TTBR_ASID_MASK
sub tmp1, tmp1, #RESERVED_SWAPPER_OFFSET
msr ttbr0_el1, tmp1                // set reserved TTBR0_EL1
```

也就是说，它不是从别处重新找一份 `reserved_pg_dir`，而是直接从当前 `ttbr1_el1` 的 `swapper_pg_dir` 值减去固定偏移 `RESERVED_SWAPPER_OFFSET`，算出 `reserved_pg_dir` 对应的 base。

从效果上看：

```text
内核入口前:  TTBR0 = 当前用户/内核低半区 pgd
内核入口后:  TTBR0 = reserved_pg_dir
TTBR1      : 仍然 = swapper_pg_dir
```

这样做的目的就是：

- 在内核态默认不让低半区 TTBR0 翻译生效
- 只有真正需要访问用户空间时，才临时重新打开


#### 21.9.2 返回用户态或临时允许 uaccess 时：再把 `TTBR0` 恢复回来

在 [arch/arm64/include/asm/asm-uaccess.h](arch/arm64/include/asm/asm-uaccess.h#L22) 的 `__uaccess_ttbr0_enable`：

```asm
get_current_task tmp1
ldr tmp1, [tmp1, #TSK_TI_TTBR0]     // load saved TTBR0_EL1
...
msr ttbr1_el1, tmp2                 // set the active ASID
msr ttbr0_el1, tmp1                 // set the non-PAN TTBR0_EL1
```

也就是说，恢复过程不是“重新现算一个用户 pgd”，而是从当前 task 的 `thread_info->ttbr0` 里取出先前保存好的 TTBR0 值。

所以 SW PAN 路径下，最常见的动态循环是：

```text
用户/可访问状态
	TTBR0 = current->thread_info.ttbr0
	TTBR1 = swapper_pg_dir (+ current ASID)

进入内核后默认封住用户访问
	TTBR0 = reserved_pg_dir
	TTBR1 = swapper_pg_dir (+ current ASID)

真正需要 uaccess / 返回 EL0
	TTBR0 = current->thread_info.ttbr0
	TTBR1 = swapper_pg_dir (+ current ASID)
```


### 21.10 从“寄存器是否变化”提升到“base 是否变化”来理解

到这里，建议你把 TTBR 变化分成两类来看：

第一类：**base 真的变了**

- `TTBR0: __pi_init_idmap_pg_dir -> idmap_pg_dir -> current->mm->pgd -> reserved_pg_dir`
- `TTBR1: reserved_pg_dir -> init_pg_dir -> swapper_pg_dir -> tramp_pg_dir`

第二类：**寄存器值变了，但 base 没变，只是附带字段变了**

- `TTBR1: swapper_pg_dir + ASID(A) -> swapper_pg_dir + ASID(B)`
- `TTBR0: current->mm->pgd + old ASID/CNP -> current->mm->pgd + new ASID/CNP`

如果用这个视角再回头看代码，就会清楚很多：

- early boot 主要是在换 **base/root**
- 运行期调度更多是在换 **ASID / 访问状态**


### 21.11 这一整条运行期路径最好记的一句话

如果把运行期 TTBR 行为压成一句话，可以记成：

> 启动完成后，`TTBR1` 通常长期站在 `swapper_pg_dir` 这条主线上，运行期更多是在改它的 ASID；而真正频繁在 `reserved_pg_dir`、`idmap_pg_dir`、`current->mm->pgd` 之间来回切的，是 `TTBR0`。


## 22. 到了 `start_kernel()` 之后，fixmap 还挂在 `swapper_pg_dir` 下吗

结论先说：**是的。**

到了 `start_kernel()` 这条常规内核执行阶段，ARM64 的 fixmap 并没有另起一套顶层 pgd；它仍然属于正式内核页表树，而这棵树的顶层根就是 `swapper_pg_dir`。


### 22.1 为什么可以直接说 fixmap 走的还是 `swapper_pg_dir`

这件事最核心的两条证据是：

1. `init_mm.pgd = swapper_pg_dir`
2. `pgd_offset_k(addr)` 走的是 `init_mm.pgd`

第一条在 [mm/init-mm.c](mm/init-mm.c#L27)：

```c
struct mm_struct init_mm = {
	...
	.pgd = swapper_pg_dir,
	...
};
```

第二条在 [include/linux/pgtable.h](include/linux/pgtable.h#L175)：

```c
#define pgd_offset_k(address) pgd_offset(&init_mm, (address))
```

所以只要某段内核地址翻译逻辑是从 `pgd_offset_k()` 开始走，那它顶层用的就是：

```text
init_mm.pgd == swapper_pg_dir
```


### 22.2 `early_fixmap_init()` 就是从内核正式 pgd 入口接进去的

在 [arch/arm64/kernel/setup.c](arch/arm64/kernel/setup.c#L278) 的 `setup_arch()`，内核很早就会调用：

```c
early_fixmap_init();
```

而 [arch/arm64/mm/fixmap.c](arch/arm64/mm/fixmap.c#L104) 的 `early_fixmap_init()` 本体是：

```c
void __init early_fixmap_init(void)
{
	unsigned long addr = FIXADDR_TOT_START;
	unsigned long end = FIXADDR_TOP;

	pgd_t *pgdp = pgd_offset_k(addr);
	p4d_t *p4dp = p4d_offset_kimg(pgdp, addr);

	early_fixmap_init_pud(p4dp, addr, end);
}
```

这段代码已经把关系写死了：

- fixmap 区域从 `pgd_offset_k(addr)` 开始找顶层入口
- `pgd_offset_k(addr)` 又等于 `pgd_offset(&init_mm, addr)`
- `init_mm.pgd` 又等于 `swapper_pg_dir`

所以 fixmap 的“顶层归属”可以直接压成：

```text
fixmap VA
  -> init_mm.pgd
  -> swapper_pg_dir
```

这就是为什么你前面那句“到了 start_kernel 里面 fixmap 映射理论上 pgd 还是 swapper_pg_dir”是成立的。


### 22.3 fixmap 不是单独的 pgd，而是正式内核页表树里的一个固定虚拟窗口

fixmap 更准确的理解不是“一套独立页表”，而是：

- 在内核高地址空间里预留一段固定 VA 区域
- 提前为这段区域准备好下级页表页
- 需要时只改这段窗口里的 PTE/PMD 等条目，把它临时指向某个物理页

在 [arch/arm64/mm/fixmap.c](arch/arm64/mm/fixmap.c#L28) 到 [arch/arm64/mm/fixmap.c](arch/arm64/mm/fixmap.c#L31)，你会看到它准备了专门的：

- `bm_pte`
- `bm_pmd`
- `bm_pud`

这些是 **fixmap 自己的下级页表页**。但注意，这里的“自己”只是说：

- fixmap 区域有专用的下级表页

并不是说：

- fixmap 拥有另一套独立的顶层 pgd 根

顶层入口仍然是 `init_mm.pgd == swapper_pg_dir`。


### 22.4 `__set_fixmap()` 实际改的是什么

`__set_fixmap()` 在 [arch/arm64/mm/fixmap.c](arch/arm64/mm/fixmap.c#L119)：

```c
void __set_fixmap(enum fixed_addresses idx, phys_addr_t phys, pgprot_t flags)
{
	unsigned long addr = __fix_to_virt(idx);
	pte_t *ptep;

	ptep = fixmap_pte(addr);

	if (pgprot_val(flags))
		__set_pte(ptep, pfn_pte(phys >> PAGE_SHIFT, flags));
	else {
		__pte_clear(&init_mm, addr, ptep);
		flush_tlb_kernel_range(addr, addr + PAGE_SIZE);
	}
}
```

所以它做的事情是：

1. 根据 fixmap 槽位编号算出固定虚拟地址
2. 找到这个固定虚拟地址对应的 fixmap PTE
3. 把这个 PTE 指到目标物理页上，或者清掉它

换句话说，`__set_fixmap()` 改的是：

```text
swapper_pg_dir 下面 fixmap 那条分支里的某个末级表项
```

不是：

```text
把 CPU 切到另一套 pgd 去跑
```


### 22.5 为什么又会看到 `pgd_set_fixmap()` / `pte_set_fixmap()` 这种名字

这里最容易再混一次。

在 [arch/arm64/include/asm/pgtable.h](arch/arm64/include/asm/pgtable.h#L891)、[arch/arm64/include/asm/pgtable.h](arch/arm64/include/asm/pgtable.h#L1060)、[arch/arm64/include/asm/pgtable.h](arch/arm64/include/asm/pgtable.h#L1252)，你会看到：

```c
#define pte_set_fixmap(addr) ((pte_t *)set_fixmap_offset(FIX_PTE, addr))
...
static inline pud_t *pud_set_fixmap(unsigned long addr)
...
#define pgd_set_fixmap(addr) ((pgd_t *)set_fixmap_offset(FIX_PGD, addr))
```

这些 helper 的真正意思不是：

```text
切换到一个叫 pte_set_fixmap/pgd_set_fixmap 的新页表根
```

而是：

```text
把“某一张页表页的物理地址”临时映射到 fixmap 预留槽位上
让内核代码能通过一个稳定虚拟地址去读写这张页表页
```

也就是说，这些 helper 只是：

- **借 fixmap 这扇窗户**
- 去访问某个页表页本身

它们不是新的地址空间，也不是新的 TTBR 根。


### 22.6 `set_swapper_pgd()` 是最典型的例子

在 [arch/arm64/mm/mmu.c](arch/arm64/mm/mmu.c#L67) `set_swapper_pgd()`：

```c
fixmap_pgdp = pgd_set_fixmap(__pa_symbol(pgdp));
WRITE_ONCE(*fixmap_pgdp, pgd);
pgd_clear_fixmap();
```

这里发生的事情是：

1. `pgdp` 这张目标 pgd 页先通过 `FIX_PGD` 槽位映进来
2. CPU 通过这个临时 fixmap VA 拿到 `fixmap_pgdp`
3. 把新的 pgd 项写进去
4. 再把这个临时 fixmap 槽位清掉

所以这条路径的本质是：

```text
用 fixmap 临时映射“页表页本身”
以便修改 swapper_pg_dir 里的内容
```

它不是：

```text
让 fixmap 运行在另一套 pgd 下
```


### 22.7 把“普通 fixmap 映射”和“fixmap helper”分开记

建议你把这两类东西严格分开：

第一类，**普通 fixmap 映射**：

- 比如 `FIX_FDT`
- 比如 `FIX_ENTRY_TRAMP_TEXT*`
- 比如 early_ioremap 用的那些 boot-time slots

它们的意思是：

```text
某个固定内核虚拟地址槽位
-> 暂时映射某个目标物理页/物理区间
```

第二类，**fixmap helper 窗口**：

- `FIX_PTE`
- `FIX_PMD`
- `FIX_PUD`
- `FIX_P4D`
- `FIX_PGD`

它们的意思是：

```text
把“页表页本身”临时映射进来
方便内核去读写页表结构
```

这两类都属于 fixmap，但两者共同的顶层前提都一样：

```text
它们都还是挂在 init_mm.pgd == swapper_pg_dir 下面
```


### 22.8 一句话压缩这件事

如果只记一句话，建议记成：

> `start_kernel()` 之后，fixmap 不是另一套 pgd，而是 `swapper_pg_dir` 下面的一段固定内核虚拟窗口；`pgd_set_fixmap/pte_set_fixmap` 这些 helper 只是借这段窗口临时映射“页表页本身”，方便改表。


## 23. `setup_arch()` 里的 `FIX_FDT` / `early_ioremap` / fixmap 关系

如果继续沿着 `start_kernel()` 往里走，最值得补的一段其实是：

- `early_fixmap_init()`
- `early_ioremap_init()`
- `setup_machine_fdt()`
- `cpu_uninstall_idmap()`
- `paging_init()`
- `early_ioremap_reset()`

因为这几步正好把“fixmap 到底靠哪套页表工作”这件事彻底坐实。


### 23.1 `setup_arch()` 的关键顺序

在 [arch/arm64/kernel/setup.c](arch/arm64/kernel/setup.c#L278) 往后，关键顺序是：

```c
early_fixmap_init();
early_ioremap_init();

setup_machine_fdt(__fdt_pointer);

...

cpu_uninstall_idmap();

...

paging_init();

...

early_ioremap_reset();
```

把它翻成一句人话：

1. 先把 fixmap 这扇固定虚拟窗口接到正式内核页表树里
2. 再把 early_ioremap 那些 boot-time 临时槽位初始化好
3. 用这些能力处理 FDT 等早期对象
4. 然后把早期 TTBR0 idmap 撤掉
5. 再继续扩展正式内核映射
6. 最后告诉 generic early_ioremap 代码：现在已经过了 `paging_init()` 阶段


### 23.2 `FIX_FDT` 为什么虽然叫 fixmap，但不是靠另一套 pgd

在 [arch/arm64/kernel/setup.c](arch/arm64/kernel/setup.c#L170) 的 `setup_machine_fdt()`：

```c
void *dt_virt = fixmap_remap_fdt(dt_phys, &size, PAGE_KERNEL);
```

而 [arch/arm64/mm/fixmap.c](arch/arm64/mm/fixmap.c#L137) 的 `fixmap_remap_fdt()` 做的关键事情是：

```c
const u64 dt_virt_base = __fix_to_virt(FIX_FDT);
...
create_mapping_noalloc(dt_phys_base, dt_virt_base, PAGE_SIZE, prot);
```

这里最容易误解的点是：

- 它使用了 `FIX_FDT` 这个 fixmap 固定虚拟地址
- 但它不是调用 `__set_fixmap(FIX_FDT, ...)` 去改一条独立的“fixmap 专用页表”

真正干活的是 `create_mapping_noalloc()`，而 [arch/arm64/mm/mmu.c](arch/arm64/mm/mmu.c#L938) 的实现是：

```c
__create_pgd_mapping(init_mm.pgd, phys, virt, size, prot, NULL,
					 NO_CONT_MAPPINGS);
```

所以 `FIX_FDT` 这条映射的本质是：

```text
在 init_mm.pgd == swapper_pg_dir 这棵正式内核页表树上
给 FIX_FDT 这个固定 VA 建一条映射
```

也就是说：

- 它用的是 fixmap 区域里的一个固定地址名字
- 但实际还是在 `swapper_pg_dir` 下面建映射


### 23.3 `early_ioremap` 用的是 fixmap 的另一组 boot-time 槽位

generic early_ioremap 代码在 [mm/early_ioremap.c](mm/early_ioremap.c#L59) `early_ioremap_setup()` 里，会把每个 boot-time slot 对应的固定虚拟地址算好。

后面真正建映射时，在 [mm/early_ioremap.c](mm/early_ioremap.c#L124) 往下：

```c
idx = FIX_BTMAP_BEGIN - NR_FIX_BTMAPS*slot;
while (nrpages > 0) {
	if (after_paging_init)
		__late_set_fixmap(idx, phys_addr, prot);
	else
		__early_set_fixmap(idx, phys_addr, prot);
	...
}
```

也就是说，`early_ioremap` 用的不是 `FIX_FDT`，而是：

- `FIX_BTMAP_BEGIN ... FIX_BTMAP_END`

这整组专门给 boot-time 临时 IO/mem 映射预留的槽位。

它和前面的 `FIX_FDT` 的区别可以压成：

1. `FIX_FDT` 是专门给 FDT 保留的固定窗口。
2. `FIX_BTMAP_*` 是 early_ioremap 通用临时槽位池。

但两者共同点不变：

```text
它们都还是内核 fixmap 区域的一部分
最终都站在 init_mm.pgd == swapper_pg_dir 这棵树上
```


### 23.4 为什么 `cpu_uninstall_idmap()` 之后 fixmap 还能继续工作

这里有一个很值得单独拎出来的点。

在 [arch/arm64/kernel/setup.c](arch/arm64/kernel/setup.c#L317) 附近，内核会执行：

```c
cpu_uninstall_idmap();
```

而它的注释说得很直白：

```text
TTBR0 is only used for the identity mapping at this stage.
Make it point to zero page to avoid speculatively fetching new entries.
```

也就是说，从这里开始，早期那条靠 `TTBR0` 的 idmap 桥已经要撤掉了。

但 fixmap 并不因此失效，原因很简单：

- fixmap 是 **kernel high VA**
- kernel high VA 这时走的是 **TTBR1**
- `TTBR1` 对应的正式根页表已经是 `swapper_pg_dir`

所以这一阶段可以理解成：

```text
TTBR0 的 early idmap 被卸掉
TTBR1 的 swapper_pg_dir 继续承载正常内核高地址访问
fixmap 作为内核高地址窗口，当然继续可用
```

这也是为什么“fixmap 依赖 swapper_pg_dir”这个结论，在 `cpu_uninstall_idmap()` 之后反而更明显。


### 23.5 `early_ioremap_reset()` 改变的是 generic early_ioremap 模式，不是顶层 pgd

在 [arch/arm64/kernel/setup.c](arch/arm64/kernel/setup.c#L340) 后面，内核会调用：

```c
early_ioremap_reset();
```

而 generic 实现在 [mm/early_ioremap.c](mm/early_ioremap.c#L36)：

```c
void __init early_ioremap_reset(void)
{
	after_paging_init = 1;
}
```

这一步的含义不是：

```text
从现在开始切到另一套页表根去做 early_ioremap
```

而只是：

```text
通知 generic early_ioremap 代码：paging_init() 已经过了
以后要走 __late_set_fixmap / __late_clear_fixmap 这一支
```

而在 ARM64 这里，[arch/arm64/include/asm/fixmap.h](arch/arm64/include/asm/fixmap.h#L111) 到 [arch/arm64/include/asm/fixmap.h](arch/arm64/include/asm/fixmap.h#L114) 又明确写着：

```c
#define __early_set_fixmap __set_fixmap
#define __late_set_fixmap  __set_fixmap
```

所以对 ARM64 来说，这里的“early/late”差别更多是 generic 框架语义，不是顶层 pgd 切换语义。


### 23.6 把这三条线放到一张图里看

如果把 `FIX_FDT`、`early_ioremap`、普通 fixmap helper 放到一张图里，你可以这么记：

```text
swapper_pg_dir (= init_mm.pgd)
	|
	+-- kernel normal mappings
	|
	+-- fixmap region
		 |
		 +-- FIX_FDT
		 |     -> setup_machine_fdt()/fixmap_remap_fdt() 使用
		 |
		 +-- FIX_BTMAP_* slots
		 |     -> early_ioremap()/early_memremap() 使用
		 |
		 +-- FIX_PTE / FIX_PMD / FIX_PUD / FIX_P4D / FIX_PGD
			   -> 临时映射“页表页本身”，方便改表
```

这张图里最重要的是：

- 三者用途不同
- 但顶层都没有脱离 `swapper_pg_dir`


### 23.7 一句话压缩这段关系

如果把这一段再压成一句话，建议记成：

> `FIX_FDT`、`early_ioremap`、`FIX_PTE/FIX_PGD` 这些都只是 `swapper_pg_dir` 下面 fixmap 窗口的不同用法；`cpu_uninstall_idmap()` 撤掉的是早期 `TTBR0` idmap，不是 fixmap 所在的正式内核页表。


## 24. 开篇先记住：`bootmem_init()` 不等于“切到 buddy”

这一段最容易被名字误导。

先直接记结论：

> 在 ARM64 Linux 6.18.1 里，真正把系统从 `memblock` 时代切到 buddy 的关键动作，不在 `bootmem_init()`，而在 [mm/mm_init.c](mm/mm_init.c#L2678) 的 `mm_core_init()` 里：
>
> `build_all_zonelists()`
> `-> memblock_free_all()`
> `-> mem_init()`
> `-> kmem_cache_init()`

所以：

- `arm64_memblock_init()`：裁剪物理内存、保留关键区间、让线性映射可成立。
- `paging_init()`：把正式内核页表里的线性映射和内核映射补齐。
- `bootmem_init()`：计算 PFN/zone/sparsemem/CMA/crashkernel 等元数据，**但此时 free memory 仍然主要由 memblock 记账**。
- `mm_core_init()`：把 memblock 里“可释放”的页批量交给 buddy，然后再启动 slab/slub。

如果画成时序图，最核心的骨架是：

```text
setup_arch()
	-> arm64_memblock_init()
	-> paging_init()
	-> bootmem_init()

start_kernel()
	-> mm_core_init()
		 -> build_all_zonelists(NULL)
		 -> memblock_free_all()
		 -> mem_init()
		 -> kmem_cache_init()
	-> kmem_cache_init_late()
	-> setup_per_cpu_pageset()
```

这张图里最重要的不是函数名，而是 **接力棒**：

1. `memblock` 先负责“我有哪些物理内存、哪些不能碰、哪些可早期分配”。
2. `paging_init()` 让这些物理页在正式内核页表里有稳定的 linear map/kernel map 视角。
3. buddy 接手“按页分配与回收”。
4. SLUB 再建立在 buddy 之上，提供“按对象分配与回收”。


## 25. `setup_arch()` 里真正的顺序：先 `paging_init()`，后 `bootmem_init()`

这个顺序非常值得单独记住，因为很多资料会把它脑补反。

在 [arch/arm64/kernel/setup.c](arch/arm64/kernel/setup.c#L286) 往后，关键顺序是：

```c
early_fixmap_init();
early_ioremap_init();
setup_machine_fdt(__fdt_pointer);
cpu_uninstall_idmap();

arm64_memblock_init();
paging_init();
...
bootmem_init();
```

也就是说：

- `arm64_memblock_init()` 先把“物理内存长什么样、哪些要保留、线性映射能覆盖多少”定下来。
- `paging_init()` 随后把 `swapper_pg_dir` 下的正式内核映射铺好。
- `bootmem_init()` 才基于这些前提去建立页分配器所需的全局元数据。

### 25.1 为什么 `paging_init()` 必须早于 `bootmem_init()`

因为 buddy/zone/sparsemem 这些后续设施，最终都建立在两个前提上：

1. 物理内存边界已经由 memblock 描述清楚。
2. 正式内核页表已经足够稳定，能让内核以高地址线性映射方式访问这些页对应的数据结构。

如果没有 `paging_init()`，你很难把后面大量依赖 `init_mm.pgd == swapper_pg_dir` 的内存元数据访问稳定下来。

所以更准确地说：

> `bootmem_init()` 不是在“裸物理内存”上直接起 buddy，而是在 memblock 已经描述好物理内存、`paging_init()` 已经准备好正式映射之后，继续把 buddy 所需元数据铺平。


## 26. `arm64_memblock_init()`：先把“物理世界”整理干净

`memblock` 本质上不是页分配器，而是 **早期物理内存描述器 + 早期分配器**。

在 ARM64 上，[arch/arm64/mm/init.c](arch/arm64/mm/init.c#L185) 的 `arm64_memblock_init()` 做的事情可以压成 5 条：

1. 去掉超出 CPU 支持 PA 位宽的物理内存。
2. 计算 `memstart_addr`，决定 linear map 从哪段物理地址开始对应 `PAGE_OFFSET`。
3. 如果 DRAM 太大，裁掉线性映射装不下的那部分物理内存。
4. 重新加入并保留 kernel image / initrd / reserved memory 这些必须可达的关键区间。
5. 把 kernel image 自己通过 `memblock_reserve(__pa_symbol(_text), _end - _text)` 标记成保留区。

这一阶段最关键的算法，不是“分配多少页”，而是：

```text
先让物理内存边界与 linear map 容量相容
再把必须保留的区间从一般可分配内存里剔出去
```

### 26.1 这一阶段和“内存映射”是什么关系

它和你前面整篇文档的映射主题并不割裂，反而是直接接上的。

前面讲的是：

- `__pi_init_idmap_pg_dir`
- `init_pg_dir`
- `swapper_pg_dir`
- fixmap / linear map / kernel map

而 `arm64_memblock_init()` 做的是：

- 决定 **哪些物理页最终会落入 linear map 的可管理范围**
- 决定 **哪些物理页虽然存在，但因为超出线性映射覆盖能力而被裁掉**
- 决定 **哪些物理页必须保留，不能进 buddy**

所以它其实是在给后面的正式页分配器划地盘。

### 26.2 为什么说 memblock 还不是 buddy

因为 memblock 管的是 **物理区间**，而不是 `struct page` 组织起来的自由页链表。

它更像：

- `memory` 数组：系统有哪些物理内存区间。
- `reserved` 数组：这些区间里哪些已经被占用或必须保留。
- 一些早期分配接口：从未保留的物理区间里切一段出来。

它还没有 buddy 的这些特征：

- 按 zone 管理。
- 按 order 合并与拆分。
- 按 `struct page` 维护页状态。
- 提供 `alloc_pages()` 这类通用页分配语义。

所以一定要把它理解成：

> `memblock` 负责“把地籍图和封控区画出来”；buddy 才负责“真正把可用页装进可分配体系”。


## 27. `bootmem_init()`：这一步在做什么，不在做什么

在 [arch/arm64/mm/init.c](arch/arm64/mm/init.c#L292) 的 `bootmem_init()`，核心顺序是：

```c
min = PFN_UP(memblock_start_of_DRAM());
max = PFN_DOWN(memblock_end_of_DRAM());

max_pfn = max_low_pfn = max;
min_low_pfn = min;

arch_numa_init();
kvm_hyp_reserve();
sparse_init();
zone_sizes_init();
dma_contiguous_reserve(...);
arch_reserve_crashkernel();
memblock_dump_all();
```

### 27.1 它真正做的事

这一步的核心产物是 **页管理元数据**，而不是“立刻把所有页塞进 buddy”。

具体看：

- `min_low_pfn/max_low_pfn/max_pfn`：给整个系统的可管理 PFN 范围定边界。
- `arch_numa_init()`：把 node 视角先立起来。
- `sparse_init()`：初始化 sparsemem 相关的 `struct page` 元数据框架。
- `zone_sizes_init()`：根据 DMA/DMA32/NORMAL 的物理限制，计算每个 zone 的上界，并调用 `free_area_init()`。
- `dma_contiguous_reserve()`：把 CMA 保留区从一般页分配体系外切出去。
- `arch_reserve_crashkernel()`：把 crashkernel 也保留下来。

### 27.2 `zone_sizes_init()` 的算法核心

ARM64 在 [arch/arm64/mm/init.c](arch/arm64/mm/init.c#L120) 的 `zone_sizes_init()` 里先算出：

- `ZONE_DMA` 上限
- `ZONE_DMA32` 上限
- `ZONE_NORMAL` 上限

然后调用 [mm/mm_init.c](mm/mm_init.c#L1824) 的 `free_area_init(max_zone_pfns)`。

`free_area_init()` 的关键工作不是“释放页”，而是：

1. 计算每个 zone 的 `lowest/highest_possible_pfn`。
2. 找出每个 node 上 `ZONE_MOVABLE` 的起点。
3. 为每个 node / zone 创建 `pgdat`、`zone`、memmap 等基础结构。

所以它回答的是：

```text
哪些 PFN 理论上属于哪个 zone
这些 zone 的管理结构该怎么搭起来
```

而不是：

```text
这些 PFN 现在已经进入 buddy 自由链表了
```

### 27.3 为什么函数叫 `bootmem_init()` 却不是 old bootmem allocator

这是历史命名遗留。

Linux 很早以前确实有 old bootmem allocator，但现在 ARM64 这条路径里真正承担 early allocator 角色的是 `memblock`。

所以今天读这个函数名，正确理解应该是：

> `bootmem_init()` = “启动阶段的内存管理初始化函数”

而不是：

> “从这里开始使用 old bootmem allocator”


## 28. `paging_init()`：它补的是“正式映射”，不是页分配策略

在 [arch/arm64/mm/mmu.c](arch/arm64/mm/mmu.c#L1351) 的 `paging_init()`，函数体非常短：

```c
map_mem(swapper_pg_dir);
memblock_allow_resize();
create_idmap();
declare_kernel_vmas();
```

但这一步非常硬核，因为它把“正式内核页表环境”建立完整。

### 28.1 `map_mem(swapper_pg_dir)` 的含义

它不是再造一个 allocator，而是：

- 把 memblock 认可的可映射物理内存放进 `swapper_pg_dir` 的线性映射里。
- 同时处理内核镜像、线性映射、属性更新等正式页表内容。

从 allocator 视角看，这一步的意义是：

> 后面 buddy 和 SLUB 所依赖的大多数普通页，终于都有了稳定的 kernel linear VA 视角。

### 28.2 `create_idmap()` 为什么还要保留 idmap

因为正式运行中仍然有一些极窄场景需要重新装 idmap，例如：

- CPU 热插拔或二次启动路径
- 某些 TTBR 切换 / suspend-resume / `.idmap.text` 执行路径

所以 `paging_init()` 不是“从此和 idmap 永别”，而是：

- 正式运行时以 `swapper_pg_dir` 为主
- 但仍然保留一份正式版 `idmap_pg_dir` 以备切换场景使用


## 29. 真正的 handoff：`mm_core_init()` 里 `memblock_free_all()` 把页交给 buddy

这一节是整条链路里最关键的接力点。

在 [mm/mm_init.c](mm/mm_init.c#L2678) 的 `mm_core_init()`：

```c
build_all_zonelists(NULL);
...
memblock_free_all();
mem_init();
kmem_cache_init();
```

### 29.1 `build_all_zonelists(NULL)` 先干什么

这一步先把 zone fallback 路径建出来。

也就是先回答：

```text
如果本 zone 没页了，应该往哪个 zone / node 退
```

这样后面 buddy 真开始接受自由页时，分配器视角就已经完整了。

### 29.2 `memblock_free_all()` 的核心算法

在 [mm/memblock.c](mm/memblock.c#L2339) 的 `memblock_free_all()`：

```c
free_unused_memmap();
reset_all_zones_managed_pages();
pages = free_low_memory_core_early();
totalram_pages_add(pages);
```

而 [mm/memblock.c](mm/memblock.c#L2291) 的 `free_low_memory_core_early()` 会：

```c
for_each_free_mem_range(...)
	count += __free_memory_core(start, end);
```

把它翻成人话就是：

1. 先看 memblock 的 `memory - reserved` 差集，找出早期阶段真正“空闲”的物理区间。
2. 对每个空闲区间，把对应页初始化并释放进 buddy。
3. 更新 zone 的 `managed_pages` 和系统总页数统计。

所以 buddy 的起点不是“凭空出现一堆空闲页”，而是：

> memblock 先把未保留物理区间枚举出来，再把这些页一批一批喂给 buddy。

### 29.3 哪些页不会在这里进入 buddy

至少要记住下面几类：

- kernel image 占用的页
- initrd 保留页
- FDT / reserved-memory
- CMA 预留区
- crashkernel 预留区
- KVM hyp 预留区

这些页在 memblock 里已经被标成 reserved，所以 `for_each_free_mem_range()` 根本不会把它们枚举出来。

### 29.4 `mem_init()` 在这里的角色

在 [mm/mm_init.c](mm/mm_init.c#L2672) 里，`mem_init()` 默认只是一个 weak hook；真正 architecture-specific 的补充逻辑可以由各架构覆写。

所以这一步在主线叙事里的意义更像是：

- 完成 memblock 释放后的体系内收尾
- 打印/整理内存初始化状态
- 为后续 slab 初始化提供稳定前提

不要把它误解成“又一个新的 allocator”。


## 30. buddy 起来之后，后面还要补哪些关键结构

buddy 在 `memblock_free_all()` 之后已经具备“可分配页”的核心能力，但还要继续补齐一些运行时关键件。

### 30.1 `setup_per_zone_wmarks()` / `calculate_min_free_kbytes()` 这些算法在解决什么

你前面读到的 [mm/page_alloc.c](mm/page_alloc.c#L6527) 一带，核心是：

- `calculate_min_free_kbytes()`
- `setup_per_zone_lowmem_reserve()`
- `setup_per_zone_wmarks()`
- `calculate_totalreserve_pages()`

它们解决的是 buddy 的 **运行水位** 问题，而不是“建不建链表”问题。

也就是：

- 每个 zone 至少要保留多少页，不能被普通路径耗尽。
- 低端 zone 要为高端分配失败时预留多少回退空间。
- kswapd / direct reclaim 应该在什么阈值上开始行动。

如果没有这些阈值，buddy 虽然能分配页，但在压力下会非常不稳定。

### 30.2 为什么 `setup_per_cpu_pageset()` 还要更晚执行

在 [init/main.c](init/main.c#L1071) 可以直接看到：

```c
kmem_cache_init_late();
...
setup_per_cpu_pageset();
```

而 [mm/page_alloc.c](mm/page_alloc.c#L5938) 这一带能看到：

- `zone_pcp_init()`
- `setup_zone_pageset()`
- `setup_per_cpu_pageset()`

这说明：

- buddy 的“全局自由区 + order 合并/拆分”能力已经先起来了。
- 之后才把每 CPU 的 page cache 热路径补齐，优化分配/释放的锁竞争。

也就是说：

> PCP 不是 buddy 的前置条件，而是 buddy 进入高并发正常运行状态前的性能增强层。


## 31. SLUB 怎么站起来：先自举两个 cache，再展开 `kmalloc-*`

这一段如果说得过于抽象，很容易看成鸡生蛋蛋生鸡。

先直接记结论：

> SLUB 并不是一上来就拥有完整的 `kmalloc-*` cache 家族；它先在 [mm/slub.c](mm/slub.c#L8466) 的 `kmem_cache_init()` 里自举出 `kmem_cache_node` 和 `kmem_cache` 这两个元 cache，然后才有能力批量创建普通 `kmalloc-*` caches。

### 31.1 `create_boot_cache()` 为什么重要

在 [mm/slab_common.c](mm/slab_common.c#L655) 的 `create_boot_cache()` 注释写得很直白：

```c
/* Create a cache during boot when no slab services are available yet */
```

这句话就是整个 SLUB 自举的关键。

它的含义是：

- 这时完整 slab 服务还没 fully online。
- 但我们必须先把“描述 slab cache 自己的对象”创建出来。
- 所以先走 boot-time 特殊创建路径。

### 31.2 `kmem_cache_init()` 的关键顺序

在 [mm/slub.c](mm/slub.c#L8466) 往后，顺序是：

```c
kmem_cache_node = &boot_kmem_cache_node;
kmem_cache = &boot_kmem_cache;

create_boot_cache(kmem_cache_node, ...);
slab_state = PARTIAL;
create_boot_cache(kmem_cache, ...);

kmem_cache = bootstrap(&boot_kmem_cache);
kmem_cache_node = bootstrap(&boot_kmem_cache_node);

setup_kmalloc_cache_index_table();
create_kmalloc_caches();
```

把它翻成一句人话：

1. 先临时拿两块静态 `boot_kmem_cache*` 结构做种子。
2. 用 boot cache 路径把 `kmem_cache_node` 和 `kmem_cache` 两种 cache 本身建起来。
3. 再把这两个 boot cache “bootstrap” 成真正挂入 `slab_caches` 链表、可继续服务别人的正式 cache。
4. 然后才有能力展开整套 `kmalloc-8`、`kmalloc-16`、`kmalloc-32` ... `kmalloc-*` 家族。

### 31.3 buddy 和 SLUB 的依赖关系到底是什么

最准确的说法是：

> buddy 提供“页”；SLUB 把这些页包装成 slab，再在 slab 中切对象。

也就是：

- buddy 的最小单位是 page / order。
- SLUB 的最小语义单位是 object。
- 一个 slab 背后仍然是从 buddy 拿来的一个或多个 page。

所以 `kmalloc(64)` 最终并没有绕开 buddy，它只是：

```text
kmalloc(64)
	-> 命中某个 kmalloc-64 cache
	-> 该 cache 若没有空闲 object，则向 buddy 申请新的 slab page(s)
	-> 再把 slab page 切成多个 64B object
```

### 31.4 从“内存映射”角度看 SLUB

这和你前面的页表主题仍然是连续的。

因为绝大多数普通 slab page：

- 本质上就是 buddy 管理的一页或若干页物理页
- 同时也位于 kernel linear map 中
- 所以很多 `kmalloc()` 返回地址，天然就是线性映射区里的普通 kernel VA

所以从地址形态上看，`kmalloc()` 对象大多数时候并没有什么神秘新映射，而是：

> buddy 先拿到 direct-mapped pages，SLUB 再在这些 direct-mapped pages 上切对象。


## 32. 一张图看懂 `memblock -> buddy -> SLUB`

```text
memblock phase
	管理单位: physical range
	关键数据: memory regions / reserved regions
	关键能力: 早期保留、早期切块分配
	映射前提: 依赖 early mapping 与随后建立的正式 linear map

		|
		|  mm_core_init()
		|  -> memblock_free_all()
		v

buddy phase
	管理单位: page / order
	关键数据: struct page / zone / free_area / zonelist
	关键能力: alloc_pages(), free_pages(), 合并拆分, watermarks, PCP
	映射前提: 页已经在 swapper_pg_dir 的 linear map 下可稳定访问

		|
		|  kmem_cache_init()
		v

SLUB phase
	管理单位: object
	关键数据: kmem_cache / kmem_cache_node / slab / freelist
	关键能力: kmalloc(), kmem_cache_alloc(), 对象缓存
	映射前提: slab page 仍然来自 buddy, 通常仍然落在线性映射区
```

如果只允许压成一句话，那就记：

> `memblock` 管“物理区间和保留”；buddy 管“页”；SLUB 管“对象”；三者不是平行关系，而是一层层站在前一层之上。


## 33. QEMU 下建议做的 6 个验证实验

下面这些实验我按“从易到难、从只看日志到写模块”排序。

### 33.1 实验 1：验证 memblock 的物理区间视角

目标：看清楚 memblock 眼里的 `memory` / `reserved` 到底是什么。

做法：

1. QEMU 启动参数里加 `earlycon ignore_loglevel loglevel=8 memblock=debug`。
2. 启动后抓 `dmesg`，重点看 memblock dump。
3. 对照：kernel image、initrd、reserved-memory、CMA、crashkernel 是否都从一般空闲区间中剔除了。

期望现象：

- 你能看到 memblock 先枚举物理内存，再单独列出 reserved 区间。
- 某些物理内存虽然存在，但因为 reserved，不会进入后续 buddy。

知识点验证：

> memblock 的基本视角是“区间差集”，不是“页链表”。

### 33.2 实验 2：验证 `paging_init()` 之后 linear map 已经稳定

目标：证明 buddy/SLUB 后续看到的普通页，大多数都已经在直接映射里。

做法：

1. 写一个简单模块，在 `init` 里调用 `alloc_pages(GFP_KERNEL, 0)`。
2. 打印：`page_to_phys(page)`、`page_to_virt(page)`、`virt_to_page(page_to_virt(page))`。
3. 检查 `virt_to_page(page_to_virt(page)) == page` 是否成立。

期望现象：

- 分配到的页能稳定往返于 `struct page <-> phys <-> linear VA`。

知识点验证：

> buddy 接手的普通内存页不是“没有虚拟地址”的裸页，而是已经挂在正式 linear map 下的页。

### 33.3 实验 3：验证 buddy 的 order 合并/拆分

目标：观察 buddy 真正的页级行为。

做法：

1. 启动后记录 `/proc/buddyinfo`。
2. 模块里循环执行：
   `alloc_pages(GFP_KERNEL, 4)` / `__free_pages(page, 4)`。
3. 再次读取 `/proc/buddyinfo`，对比对应 order 变化。

期望现象：

- 某些 order 的空闲块数量在分配后减少、释放后恢复。
- 如果连续施压，还能看到更高 order 被拆分为更低 order。

知识点验证：

> buddy 管理单位是 order block，不是单独某个 `kmalloc-*` cache。

### 33.4 实验 4：验证 PCP 只是热路径缓存，不是 allocator 根本

目标：区分 global buddy 与 per-cpu pageset。

做法：

1. 在 SMP QEMU 下跑多核压力小程序或模块，持续做 `alloc_pages(GFP_ATOMIC, 0)` / free。
2. 同时观察 `/proc/zoneinfo` 与 `/proc/vmstat`，关注与 PCP 相关统计。
3. 对照启动早期 `setup_per_cpu_pageset()` 的时序理解其出现位置。

期望现象：

- PCP 统计会变化，但其背后页源头仍然来自 zone/buddy。

知识点验证：

> PCP 是 buddy 的 per-cpu 快路径，不是取代 buddy 的新 allocator。

### 33.5 实验 5：验证 `kmalloc-*` cache 最终仍然向 buddy 要页

目标：证明 SLUB 和 buddy 不是并列关系。

做法：

1. 打开 `slub_debug`，例如启动参数加 `slub_debug=P,kmalloc-64`。
2. 模块里高频执行 `kmalloc(64, GFP_KERNEL)` / `kfree()`。
3. 观察 `/proc/slabinfo` 中 `kmalloc-64` 的对象数、slab 数变化。
4. 同时配合 `/proc/buddyinfo` 看页级库存变化。

期望现象：

- `kmalloc-64` 活跃对象增减时，底层 slab page 的数量也会变化。
- slab page 不够时，最终仍然会消耗 buddy 管理的页。

知识点验证：

> SLUB 分对象，但它吃的底层原料仍然是 buddy 提供的页。

### 33.6 实验 6：自定义 `kmem_cache`，验证“对象缓存”和“页缓存”分层

目标：亲手验证 `kmem_cache_create()` 和 `alloc_pages()` 的分层。

做法：

1. 模块里 `kmem_cache_create("mytest", 128, 0, SLAB_HWCACHE_ALIGN, NULL)`。
2. 分配一批对象，打印对象地址。
3. 对这些地址做 `virt_to_page()`，看它们是否落在若干共享 slab page 上。
4. 再销毁 cache。

期望现象：

- 多个 128B object 会聚集在同一页或少数几页上。

知识点验证：

> SLUB 不是“每对象对应一页”，而是“先拿页，再切对象”。


## 34. 如果你要写一个最小验证模块，建议观测这些字段

为了让实验更像“可落地验证”，建议模块里统一打印这几类信息：

- `struct page *page`
- `page_to_pfn(page)`
- `page_to_phys(page)`
- `page_to_virt(page)`
- `virt_to_page(ptr)`
- `compound_order(page_folio(page))` 或等价 order 信息
- `ksize(ptr)`（针对 `kmalloc` 对象）

如果想更进一步，还可以打印：

- `is_vmalloc_addr(ptr)`：确认对象是否来自 vmalloc 区。
- `virt_addr_valid(ptr)`：确认是否是普通内核可直达地址。

这样你就能把“物理页 / 线性映射地址 / slab 对象”三套视角同时放在一条日志里看。


## 35. 面试里最常见的 10 个问题和回答

### 35.1 `memblock` 和 buddy 的根本区别是什么？

答：

- `memblock` 管的是物理区间与保留关系，主要用于 early boot。
- buddy 管的是 `struct page`、zone、order free lists，提供正式页分配能力。
- 真正的 handoff 点通常是 `memblock_free_all()` 把 free mem ranges 释放给 buddy。

### 35.2 为什么 ARM64 不能一上来就只用 buddy？

答：

- 因为 buddy 依赖大量元数据先准备好：`struct page`、zone、zonelist、线性映射、保留区裁剪等。
- 在这些东西还没搭起来前，内核需要一个更简单、更偏物理区间视角的 early allocator，这就是 memblock。

### 35.3 `bootmem_init()` 在现在的内核里到底代表什么？

答：

- 它是启动阶段内存管理初始化函数名。
- 在 ARM64 当前主线里，它不是 old bootmem allocator 的入口。
- 它主要负责 PFN/NUMA/sparsemem/zone/CMA/crashkernel 等元数据准备。

### 35.4 `paging_init()` 和 `bootmem_init()` 谁先谁后，为什么？

答：

- 在 ARM64 `setup_arch()` 里，`paging_init()` 先于 `bootmem_init()`。
- 因为正式内核页表和 linear map 要先稳定，后面的页分配元数据访问才有可靠基础。

### 35.5 真正把 memblock 切换到 buddy 的代码点在哪里？

答：

- 关键点在 `mm_core_init()`。
- 尤其是 `memblock_free_all()`，它遍历 memblock 的 free memory ranges，把页正式释放进 buddy。

### 35.6 buddy 和 PCP 是什么关系？

答：

- PCP 是 per-cpu pageset，是 buddy 的性能优化层。
- 它缓存 order-0 等热点小页，减少 zone lock 竞争。
- 它不替代 buddy，本质上仍然从 buddy 衍生和回流。

### 35.7 buddy 和 SLUB 是什么关系？

答：

- buddy 分配页。
- SLUB 从 buddy 申请 slab page，再切成对象。
- `kmalloc()` 最终仍然间接依赖 buddy。

### 35.8 `kmem_cache_init()` 为什么要先创建 `kmem_cache` 自己？

答：

- 因为 slab allocator 需要先有“描述 cache 的 cache”。
- 所以通过 boot cache 路径先自举出 `kmem_cache_node` 和 `kmem_cache`，再展开普通 `kmalloc-*` caches。

### 35.9 `kmalloc()` 返回地址一般落在哪？

答：

- 对于普通小对象，通常落在 buddy 提供的 slab page 上，而这些页大多在线性映射区中。
- 所以很多 `kmalloc()` 地址本质上就是 direct-mapped kernel VA。

### 35.10 怎么在 QEMU 里证明你说的这套链路是真的？

答：

- 用 `memblock=debug` 看 memblock 区间和保留区。
- 用 `/proc/buddyinfo`、`/proc/zoneinfo` 看 buddy。
- 用 `/proc/slabinfo` 和 `slub_debug` 看 SLUB。
- 用模块打印 `page_to_virt/virt_to_page/ksize` 等，把页和对象层打通。


## 36. 最后压成 6 句话

1. `arm64_memblock_init()` 先把物理内存和 linear map 的边界整理干净。
2. `paging_init()` 把 `swapper_pg_dir` 下的正式 kernel map / linear map 建完整。
3. `bootmem_init()` 主要补齐 PFN、zone、sparsemem、CMA、crashkernel 这些元数据。
4. `mm_core_init()` 里的 `memblock_free_all()` 才是 memblock 向 buddy 的真正交接点。
5. buddy 负责页，PCP 是 buddy 的 per-cpu 热路径优化。
6. SLUB 负责对象，但它底下吃的仍然是 buddy 提供并由 linear map 可达的页。

> `setup_arch()` 之后的 `FIX_FDT`、`early_ioremap` 临时槽位、以及 `FIX_PTE/FIX_PGD` 这类 helper，本质上都是 `swapper_pg_dir` 下面 fixmap 区域的不同用法；它们不是新的 pgd，只是同一棵正式内核页表树上的不同窗口。


## 37. 已经准备好的验证模块：`kmodules/mem_allocator_lab`

上面第 33 节里提到的“写一个简单模块”，现在已经在当前仓库里落成了一个现成版本：

- [kmodules/mem_allocator_lab/mem_allocator_lab.c](kmodules/mem_allocator_lab/mem_allocator_lab.c)
- [kmodules/mem_allocator_lab/README.md](kmodules/mem_allocator_lab/README.md)

它在 `init` 时会做 3 件事：

1. `alloc_pages()`，验证 buddy 分配出来的页可以稳定往返 `page -> phys -> linear VA -> page`。
2. `kmalloc()`，验证普通小对象通常落在 direct-mapped slab page 上。
3. `kmem_cache_create()` + 多对象分配，验证 SLUB 是“先拿页，再切对象”。

### 37.1 构建命令

在仓库根目录执行：

```bash
ARCH=arm64 LLVM=1 make M=$PWD/kmodules/mem_allocator_lab modules
```

如果只想清理构建产物：

```bash
ARCH=arm64 LLVM=1 make M=$PWD/kmodules/mem_allocator_lab clean
```

### 37.2 运行命令

```bash
cat /proc/buddyinfo
cat /proc/slabinfo | grep -E 'kmalloc-|mem_allocator_lab'

sudo insmod kmodules/mem_allocator_lab/mem_allocator_lab.ko \
	page_order=2 kmalloc_bytes=96 cache_bytes=128 cache_objects=8

dmesg | grep mem_allocator_lab

sudo rmmod mem_allocator_lab
```

### 37.3 这能直接验证第 33 节里的哪些结论

- 对应 `33.2`：`alloc_pages` 日志会打印 `page_to_phys()`、`page_to_virt()`、`virt_to_page()` 的往返关系。
- 对应 `33.3`：改变 `page_order`，再对照 `/proc/buddyinfo`，可以观察 order block 的消耗与回收。
- 对应 `33.5`：`kmalloc` 日志和 `/proc/slabinfo` 一起看，可以观察小对象如何落在 slab page 上。
- 对应 `33.6`：多条 `cache_object` 日志能看到同一自定义 cache 的对象如何聚集在若干页内。

如果只允许压成一句话，可以记成：

> 现在这份文档里的 allocator 实验，不再只是“建议你自己写模块”，而是仓库里已经有一个可直接构建、直接插入、直接看日志的最小验证模块。


## 38. NUMA / sparsemem / zone / page：ARM64 内存管理基础设施到底各抽象了哪一层

这 4 个词最容易被混成一层，但它们其实是在抽象 **4 个不同维度**。

先直接记结论：

> `NUMA` 抽象“节点”；`zone` 抽象“分配约束下的页区”；`sparsemem` 抽象“稀疏 PFN 空间的分段组织”；`struct page` 抽象“单个物理页帧的运行时元数据”。

对当前这棵树：

- `.config` 打开了 `CONFIG_NUMA=y`
- `.config` 打开了 `CONFIG_SPARSEMEM=y`
- `.config` 打开了 `CONFIG_SPARSEMEM_VMEMMAP=y`
- `.config` 打开了 `CONFIG_ZONE_DMA=y`
- `.config` 打开了 `CONFIG_ZONE_DMA32=y`
- ARM64 没有 `ZONE_HIGHMEM` 这一套常规高端内存分层

所以这台内核的内存管理骨架可以先压成：

```text
system memory
	-> NUMA node (pglist_data / pgdat)
		-> zone (DMA / DMA32 / NORMAL / MOVABLE ...)
			-> PFN ranges
				-> sparsemem section
					-> struct page array entry per PFN
```

### 38.1 NUMA：抽象“距离”和“节点”

`NUMA` 这一层关心的不是页表，也不是页本身，而是：

- 哪些 CPU 和哪段内存更“近”
- 分配失败时应该先回退到哪个 node
- reclaim / balancing / memory tiering 应该按哪个 node 统计

Linux 里的核心对象是 [include/linux/mmzone.h](include/linux/mmzone.h#L1385) 的 `pglist_data`：

```c
typedef struct pglist_data {
	struct zone node_zones[MAX_NR_ZONES];
	struct zonelist node_zonelists[MAX_ZONELISTS];
	unsigned long node_start_pfn;
	unsigned long node_present_pages;
	unsigned long node_spanned_pages;
	int node_id;
	...
} pg_data_t;
```

这段定义直接说明：

- 一个 NUMA node 由一个 `pgdat` 表示
- 每个 node 下面有自己的 `node_zones[]`
- 每个 node 还维护自己的 `node_zonelists[]`，用于分配回退路径

所以 NUMA 这一层回答的是：

```text
“这是谁的内存？”
“优先在哪个 node 分配？”
“本 node 和其他 node 的 fallback 顺序是什么？”
```

### 38.2 zone：抽象“可分配约束”

`zone` 不是按物理连续性分，而是按 **分配约束** 分。

定义在 [include/linux/mmzone.h](include/linux/mmzone.h#L833) 的 `enum zone_type` 和 [include/linux/mmzone.h](include/linux/mmzone.h#L879) 的 `struct zone`：

- `ZONE_DMA`
- `ZONE_DMA32`
- `ZONE_NORMAL`
- `ZONE_MOVABLE`
- `ZONE_DEVICE`（如果打开）

当前配置至少会有：

- `ZONE_DMA`
- `ZONE_DMA32`
- `ZONE_NORMAL`

而 `struct zone` 本身持有的是 buddy allocator 真正关心的内容：

- `free_area[NR_PAGE_ORDERS]`
- 水位线 `_watermark[]`
- `managed_pages`
- `zone_start_pfn`
- `spanned_pages / present_pages`
- `per_cpu_pageset`

所以 zone 这一层回答的是：

```text
“这些页能不能满足 DMA / DMA32 / NORMAL 约束？”
“这个页区的 buddy free lists、watermarks、PCP 状态是什么？”
```

### 38.3 sparsemem：抽象“PFN 空间如何被稀疏组织”

`sparsemem` 解决的问题不是“怎么分配”，而是：

```text
物理 PFN 空间可能很大、很稀疏、甚至中间有洞
那 struct page 元数据该怎么组织，才能不要求整片连续 mem_map？
```

关键定义在 [include/linux/mmzone.h](include/linux/mmzone.h#L1851) 之后：

```c
#define PFN_SECTION_SHIFT   (SECTION_SIZE_BITS - PAGE_SHIFT)
#define PAGES_PER_SECTION   (1UL << PFN_SECTION_SHIFT)

struct mem_section {
	unsigned long section_mem_map;
	struct mem_section_usage *usage;
	...
};
```

对当前 ARM64 4KB 页配置，[arch/arm64/include/asm/sparsemem.h](arch/arm64/include/asm/sparsemem.h#L29) 给出：

```c
#define SECTION_SIZE_BITS 27
```

所以当前配置下：

- `SECTION_SIZE = 1 << 27 = 128MB`
- `PFN_SECTION_SHIFT = 27 - 12 = 15`
- `PAGES_PER_SECTION = 1 << 15 = 0x8000 = 32768 pages`

也就是说，在这台 ARM64 4KB 配置里：

> sparsemem 把 PFN 空间按 **128MB 一段** 来切，每段用一个 `mem_section` 描述它有没有内存、有没有 memmap、是否 online、对应的 `struct page` 数组起点在哪。

再结合 `CONFIG_SPARSEMEM_VMEMMAP=y`，含义就是：

- `struct page` 并不要求存在一个老式大平铺 `mem_map[]`
- section 的 memmap 会落到 `vmemmap` 这片专门的虚拟地址区域里

所以 sparsemem 这一层回答的是：

```text
“这个 PFN 属于哪个 section？”
“这个 section 是否 present/valid/online？”
“这个 section 对应的 struct page 元数据放在哪里？”
```

### 38.4 `struct page`：抽象“单个物理页帧对象”

`struct page` 定义在 [include/linux/mm_types.h](include/linux/mm_types.h#L72)。

它抽象的不是一个范围，而是：

> “某一个 PFN 当前在内核里被当成什么用、挂在哪个链表、引用计数是多少、是否在 buddy/slab/pagecache/anon 中。”

你可以把它理解成“单页对象头”。

这层里最核心的是：

- `flags`
- `_refcount`
- `mapping`
- `private`
- `buddy_list / pcp_list / lru`

所以 `struct page` 这一层回答的是：

```text
“这一页现在是空闲页、匿名页、页缓存页、slab 页，还是别的？”
“它的 refcount / flags / owner / 链表状态是什么？”
```

### 38.5 这 4 层之间到底怎么连起来

最准确的串法是：

1. `NUMA` 先把系统内存按 node 分组，对象是 `pgdat`。
2. `zone` 再把每个 node 内的页按分配约束切成 DMA/DMA32/NORMAL 等 zone。
3. `sparsemem` 再把全局 PFN 空间按 section 切段，解决 memmap 元数据的稀疏组织问题。
4. `struct page` 最后给每个实际 PFN 一个页级对象描述符。

它们不是互斥关系，而是不同视角叠加：

```text
同一个 PFN
	既属于某个 NUMA node
	又属于某个 zone
	还落在某个 sparsemem section
	最终还对应一个 struct page
```

### 38.6 从 PFN 走到 `struct page` 的路径

对当前配置，最典型的心智图是：

```text
PFN
	-> section_nr = pfn >> PFN_SECTION_SHIFT
	-> mem_section[section_nr]
	-> section_mem_map / vmemmap backing
	-> struct page for this PFN
	-> page flags 中又可反推出 node / zone
```

所以一定不要把 sparsemem 和 zone 混为一谈：

- sparsemem 关心的是 `memmap` 怎么组织
- zone 关心的是 buddy allocator 怎么分配

### 38.7 这几层在启动时是谁先建起来的

结合前面已经分析过的启动路径，它们的大致顺序是：

```text
arm64_memblock_init()
	-> arch_numa_init()
	-> sparse_init()
	-> zone_sizes_init()
		-> free_area_init()
	-> mm_core_init()
		-> build_all_zonelists()
		-> memblock_free_all()
```

这条顺序正好对应 4 层职责：

- `arch_numa_init()` 先把 node 视角立起来
- `sparse_init()` 先把 sparsemem/vmemmap 元数据框架立起来
- `zone_sizes_init()/free_area_init()` 把 zone 和 buddy 基础结构立起来
- `memblock_free_all()` 再把真正 free 的页喂给 buddy

如果只允许压成一句话，可以记成：

> `pgdat` 解决“节点”，`zone` 解决“分配约束”，`mem_section` 解决“稀疏 PFN 的元数据组织”，`struct page` 解决“单页对象状态”。


## 39. 当前配置下的 ARM64 内核地址范围图，以及“一个物理页被两个内核 VA 映射”这件事

先把当前和地址布局直接相关的配置钉死：

- `CONFIG_ARM64_4K_PAGES=y`
- `CONFIG_PGTABLE_LEVELS=5`
- `CONFIG_ARM64_VA_BITS=52`
- `CONFIG_ARM64_PA_BITS=52`
- `CONFIG_ARM64_LPA2=y`
- `CONFIG_RANDOMIZE_BASE=y`
- 没有启用 `CONFIG_ARM64_FORCE_52BIT`
- 当前这棵树没有启用 KASAN 阴影替换 `PAGE_END` 的那条路径

### 39.1 先看编译期确定的关键边界

这些宏定义来自 [arch/arm64/include/asm/memory.h](arch/arm64/include/asm/memory.h#L31) 和 [arch/arm64/include/asm/pgtable.h](arch/arm64/include/asm/pgtable.h#L18)。

当前配置下可以直接写死：

```text
PAGE_OFFSET   = 0xfff0000000000000
PAGE_END      = 0xffff800000000000
MODULES_VADDR = 0xffff800000000000
MODULES_END   = 0xffff800080000000
KIMAGE_VADDR  = 0xffff800080000000
VMEMMAP_END   = 0xffffffffc0000000
PCI_IO_START  = 0xffffffffc0800000
PCI_IO_END    = 0xffffffffc1800000
FIXADDR_TOP   = 0xffffffffff800000
```

而 `VMALLOC_END` 仍然取决于运行时 `vabits_actual` 和 `sizeof(struct page)`，所以它不能只靠 `.config` 唯一钉死到一个常数。

### 39.2 当前配置下的内核地址空间逻辑图

```text
TTBR1 kernel VA space

0xfff0000000000000  PAGE_OFFSET
	|
	|<----------------------- linear map ----------------------->|
	|
0xffff800000000000  PAGE_END == MODULES_VADDR
	|
	|<---------------- modules area (2GB) --------------------->|
	|
0xffff800080000000  MODULES_END == KIMAGE_VADDR
	|
	|<------ kernel image / vmalloc / vmap / ioremap ... ------>|
	|
0xffffffffc0000000  VMEMMAP_END
0xffffffffc0800000  PCI_IO_START
0xffffffffc1800000  PCI_IO_END
0xffffffffff800000  FIXADDR_TOP
```

这张图里最关键的分界是：

- `PAGE_OFFSET -> PAGE_END` 是 **linear map**
- `KIMAGE_VADDR` 起是 **kernel image / vmalloc family** 那一半

如果把它补成更具体的闭区间，可以先记这张表：

```text
[0xfff0000000000000, 0xffff7fffffffffff]  linear map
	size = 0x0f800000000000 = 3.875 PB

[0xffff800000000000, 0xffff80007fffffff]  modules area
	size = 0x0000000080000000 = 2 GB

[0xffff800080000000, 0xffffffffbfffffff]  kernel image / vmalloc / vmap / ioremap family
	size = 0x00007fff40000000

[0xffffffffc0000000, 0xffffffffc07fffff]  vmemmap tail end side
	size = 0x00800000 = 8 MB

[0xffffffffc0800000, 0xffffffffc17fffff]  PCI I/O
	size = 0x01000000 = 16 MB

[0xffffffffc1800000, 0xffffffffff7fffff]  fixmap 之前的高地址保留区
	size = 0x3de00000
```

这里要注意一件事：

- `VMEMMAP_END`、`PCI_IO_START/END`、`FIXADDR_TOP` 这些边界是可以直接写死的
- 但 `VMEMMAP_START` 和 `VMALLOC_END` 依赖 `vabits_actual` 以及 `sizeof(struct page)`，所以如果你要把 `vmemmap` 也精确写成完整闭区间，还需要再结合实际运行硬件的 `vabits_actual`

也就是说，当前文档里这张表已经把 **能仅凭当前配置和链接常量确定的边界** 都写死了；`vmalloc` 与 `vmemmap` 的完整分界仍然是运行时相关量。

### 39.2.1 当前 `vmlinux` 的实际内核镜像符号区间

除了上面的“逻辑窗口”，当前这份 `vmlinux` 的链接地址本身也可以直接列出来：

```text
_text                 = 0xffff800080000000
_stext                = 0xffff800080010000
__inittext_begin      = 0xffff800081460000
__inittext_end        = 0xffff800081540000
__initdata_begin      = 0xffff800081540000
_data                 = 0xffff800082120000
_end                  = 0xffff800082330000
```

所以当前链接视角下，整个 kernel image 本体至少可以先写成：

```text
[0xffff800080000000, 0xffff80008232ffff]  linked kernel image image-mapping window
	size = 0x02330000
```

如果再细分成你前面反复分析过的权限/阶段边界，大致是：

```text
[0xffff800080000000, 0xffff80008000ffff]  _text .. _stext-1
[0xffff800080010000, 0xffff80008145ffff]  _stext .. __inittext_begin-1
[0xffff800081460000, 0xffff80008153ffff]  __inittext_begin .. __inittext_end-1
[0xffff800081540000, 0xffff80008211ffff]  __initdata_begin .. _data-1
[0xffff800082120000, 0xffff80008232ffff]  _data .. _end-1
```

这几段刚好也对应你前面分析 `map_kernel()` 和 `map_range()` 时看到的那些边界来源：

- `_stext`
- `__inittext_begin`
- `__inittext_end`
- `__initdata_begin`
- `_data`
- `_end`

### 39.2.2 当前早期页表池在 image mapping 里的具体地址

你前面关心的几份页表根和临时构表池，现在也可以直接给出 image VA：

```text
idmap_pg_dir            = 0xffff80008145c000
tramp_pg_dir            = 0xffff80008145d000
reserved_pg_dir         = 0xffff80008145e000
swapper_pg_dir          = 0xffff80008145f000

__pi_init_idmap_pg_dir  = 0xffff800081540000
__pi_init_idmap_pg_end  = 0xffff800081548000
	=> size = 0x8000 = 32 KB

__pi_init_pg_dir        = 0xffff800082315000
__pi_init_pg_end        = 0xffff800082322000
	=> size = 0xd000 = 52 KB
```

这样一来，`__pi_init_idmap_pg_dir` 这段区域在最终高地址 image mapping 里的位置也完全清楚了：

```text
[0xffff800081540000, 0xffff800081547fff]  primary boot init-idmap page-table pool
```

### 39.3 你印象里的那件事是对的：最终内核镜像通常确实有两个不同的 kernel VA 指向同一物理内存

答案先给结论：

> 对 ARM64 来说，内核镜像物理页在正式运行阶段通常至少同时拥有两种 kernel VA 语义：
> 1. **kernel image mapping** 里的地址，例如 `_text/_stext/_data/_end`
> 2. **linear map** 里的 direct-map alias，即 `__va(__pa_symbol(x))`

这两种地址都能指向同一个物理页，但它们属于 **两套不同的虚拟地址窗口**。

#### 第一种：kernel image mapping

这是你平时在符号里看到的那套地址，例如：

- `_text`
- `_stext`
- `__start_rodata`
- `_data`

它们的基准来自：

- [arch/arm64/include/asm/memory.h](arch/arm64/include/asm/memory.h#L40) 的 `KIMAGE_VADDR`
- 运行时再叠加 `kaslr_offset()` 与 `kimage_voffset`

#### 第二种：linear map alias

这是 direct map 那套地址，也就是：

```c
lm_alias(x) = __va(__pa_symbol(x))
```

定义见 [include/linux/mm.h](include/linux/mm.h#L106)。

这意味着：

- 同一段 kernel image 物理页
- 既可以从 `_text` 这类 image VA 访问
- 也可以从 direct map VA 访问

#### 这两套地址为什么都存在

因为它们服务的目标不同：

- kernel image mapping：保证内核镜像本身位于固定高位语义地址区，可配合 KASLR、权限属性、模块区等整体布局。
- linear map：保证普通 RAM 都能通过统一公式快速映射为 kernel VA，供页分配器、slab、pagecache、memcpy、调试等通用途径使用。

### 39.4 还要再加一句：boot 早期其实甚至可能出现第三种临时 alias

在你前面已经分析透的 early boot 里，还有一条临时 idmap：

- `VA = PA`
- 只在 `__pi_init_idmap_pg_dir` / `idmap_pg_dir` 这些过渡场景使用

所以把时序放在一起看：

```text
boot early:
	identity map alias (VA = PA)

boot later / runtime:
	kernel image mapping alias
	+ linear map alias
```

### 39.5 一张图把“同一个物理页的多个 VA”画出来

```text
same physical page of kernel image

PA:  [ kernel image physical page ]
	  ^
	  |
	  +-- image VA:   _text / _stext / _data ...
	  |       (KIMAGE_VADDR + kaslr_offset family)
	  |
	  +-- linear VA:  lm_alias(_text) = __va(__pa_symbol(_text))
	  |       (PAGE_OFFSET family)
	  |
	  +-- early boot only: identity VA = PA
			  (__pi_init_idmap_pg_dir / idmap_pg_dir path)
```

如果你只问“最终内核镜像会不会被两个不同的 VA 指到同一物理页”，答案就是：

> 会。正式阶段最重要的两条就是 `kernel image mapping` 和 `linear map alias`。


## 40. 单个进程地址映射地址图：当前 ARM64 配置下该怎么理解

先直接记结论：

> 对单个 64-bit 进程来说，`mm_struct` 管的是整个用户地址空间，`vm_area_struct` 管的是一段一段 VMA，而 TTBR0 挂的是该进程自己的用户页表根；TTBR1 仍然是全局共享的 `swapper_pg_dir` 内核半边。

### 40.1 管理对象分别是谁

#### `mm_struct`

定义在 [include/linux/mm_types.h](include/linux/mm_types.h#L963)。

这里最关键的字段是：

- `mm_mt`：所有 VMA 的 maple tree
- `mmap_base`：mmap 区基址
- `task_size`：用户地址空间上界
- `pgd`：该进程 TTBR0 使用的用户页表根
- `map_count`：VMA 数量

#### `vm_area_struct`

定义在 [include/linux/mm_types.h](include/linux/mm_types.h#L813)。

它描述单段映射：

- `vm_start / vm_end`
- `vm_flags`
- `vm_page_prot`
- `vm_file`
- `vm_pgoff`
- `anon_vma`

所以可以把两者关系记成：

```text
mm_struct
	= 一个进程的整张用户地址空间总控对象

vm_area_struct
	= 这张地址空间里的某一段映射规则
```

### 40.2 当前配置下用户空间的两个上界概念

来自 [arch/arm64/include/asm/processor.h](arch/arm64/include/asm/processor.h#L55)：

```c
#define DEFAULT_MAP_WINDOW_64    (UL(1) << VA_BITS_MIN)
#define TASK_SIZE_64             (UL(1) << vabits_actual)
```

对当前配置：

- `VA_BITS_MIN = 48`
- 所以 `DEFAULT_MAP_WINDOW_64 = 0x0001000000000000`（256TB）
- 如果硬件实际跑满 52-bit，则 `TASK_SIZE_64 = 0x0010000000000000`（4PB）

但因为 **没有** 打开 `CONFIG_ARM64_FORCE_52BIT`，所以默认用户态窗口仍然以 48-bit 为主：

- `STACK_TOP_MAX = DEFAULT_MAP_WINDOW_64`
- `TASK_UNMAPPED_BASE = PAGE_ALIGN(DEFAULT_MAP_WINDOW / 4)`

也就是：

```text
STACK_TOP             = 0x0001000000000000
TASK_UNMAPPED_BASE    = 0x0000400000000000
ELF_ET_DYN_BASE       = 0x0000aaaaaaaaaaaa
```

### 40.3 这意味着什么

这意味着当前内核虽然支持更大的 52-bit 用户地址空间，但默认行为仍然是：

- 栈顶和常规 mmap window 仍然落在 48-bit 范围内
- 只有用户显式给出高 hint 时，`arch_get_mmap_end()/arch_get_mmap_base()` 才可能让某些映射进入 52-bit 区域

对应代码就在 [arch/arm64/include/asm/processor.h](arch/arm64/include/asm/processor.h#L96)。

### 40.4 单个进程的默认地址图

当前配置下，一个 **默认 64-bit 进程** 的用户空间可以这样理解：

```text
low user VA
0x0000000000000000
	|
	|  NULL guard / low unmapped area
	|
	|  main executable text/data/bss
	|  brk heap (grows up)
	|
	|  bottom-up legacy mmap base
	|  TASK_UNMAPPED_BASE = 0x0000400000000000
	|
	|  PIE / ET_DYN typical base
	|  ELF_ET_DYN_BASE = 0x0000aaaaaaaaaaaa
	|
	|  top-down mmap region
	|  shared libraries
	|  file mappings / anonymous mappings
	|  vvar / vdso near top
	|
	|  process stack (grows down)
	|
0x0001000000000000
	= STACK_TOP / DEFAULT_MAP_WINDOW_64

optional high-hint area only if userspace opts in:

0x0001000000000000 .. 0x0010000000000000
	= extra 52-bit user VA range
```

如果你要的是更具体的地址闭区间，可以直接记成：

```text
[0x0000000000000000, 0x00003fffffffffff]
	low user area，通常包含主程序 text/data/bss、brk 之前的低地址空间

[0x0000400000000000, 0x0000ffffffffffff]
	default 48-bit map window
	其中：
	TASK_UNMAPPED_BASE = 0x0000400000000000
	ELF_ET_DYN_BASE    = 0x0000aaaaaaaaaaaa
	STACK_TOP          = 0x0001000000000000

[0x0001000000000000, 0x000fffffffffffff]
	extra 52-bit user range
	默认不会大量使用，通常需要高地址 hint 才会进这片区间
```

注意这里的 `STACK_TOP = 0x0001000000000000` 是“栈顶上界锚点”，不是说栈页会正好映射到这个地址本身。实际用户栈通常是：

- 从 `randomize_stack_top(STACK_TOP)` 往下摆
- 再受 `RLIMIT_STACK`、guard gap、ASLR 影响

所以更准确的说法是：

```text
用户栈常驻区间 ~= [randomized_stack_base - stack_size, randomized_stack_base)
randomized_stack_base < 0x0001000000000000
```

### 40.4.1 把单进程常见区域再压成一张更具体的图

```text
0x0000000000000000
	|
	|  NULL / guard / unmapped
	|
	|  main executable text/data/bss
	|  brk heap grows up
	|
0x0000400000000000  TASK_UNMAPPED_BASE
	|
	|  mmap base lower anchor / legacy bottom-up starting point
	|
0x0000aaaaaaaaaaaa  ELF_ET_DYN_BASE
	|
	|  PIE main binary common load neighborhood
	|  shared libraries / anonymous mmap / file-backed mmap
	|  vvar / vdso near the top
	|
< randomized stack base < 0x0001000000000000
	|  main stack grows down
	|
0x0001000000000000  STACK_TOP / DEFAULT_MAP_WINDOW_64
	|
	|  extra 52-bit user VA area only when high hints opt in
	|
0x0010000000000000  TASK_SIZE_MAX
```

### 40.5 更贴近内核实现的单进程视图

```text
process mm_struct
	|
	+-- pgd
	|     -> TTBR0 user page tables for this process
	|
	+-- mm_mt
	|     -> maple tree of all VMAs
	|
	+-- vm_area_struct: text
	+-- vm_area_struct: data
	+-- vm_area_struct: heap/brk
	+-- vm_area_struct: shared libraries
	+-- vm_area_struct: anonymous mmap
	+-- vm_area_struct: vdso/vvar
	+-- vm_area_struct: stack

kernel half:
	TTBR1 = swapper_pg_dir
	shared by all processes
```

这张图里最关键的是：

- `mm->pgd` 只代表这个进程自己的 **用户半边**
- 内核半边不是每进程各自复制一份，而是统一走 `TTBR1/swapper_pg_dir`

### 40.6 `mmap_base` 和 stack 是怎么摆的

来自 [mm/util.c](mm/util.c#L432) 和 [mm/util.c](mm/util.c#L465)：

- top-down 布局时，`mmap_base = PAGE_ALIGN(STACK_TOP - gap - rnd)`
- `gap` 至少 `128MB`
- 还会再叠加 stack randomization 的 pad

所以运行时你在 `/proc/<pid>/maps` 里看到的库、匿名映射、JIT、线程栈等位置，不是一个固定常量，而是：

- 以 `STACK_TOP` 为锚点
- 减去 `rlimit stack`
- 减去 ASLR random factor
- 最终得到 `mm->mmap_base`

### 40.7 一句话压缩这张单进程图

如果只记一句话，建议记成：

> 一个 ARM64 进程的用户地址空间由 `mm_struct + vm_area_struct` 描述，默认主活动窗口仍是 48-bit；TTBR0 指向该进程自己的用户页表根，TTBR1 继续共享全局内核映射。