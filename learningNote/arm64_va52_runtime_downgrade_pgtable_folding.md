# ARM64 52-bit VA 运行时降级与页表折叠说明

## 1. 问题背景

当前内核编译配置为：

- `CONFIG_ARM64_4K_PAGES=y`
- `CONFIG_ARM64_VA_BITS_52=y`
- `CONFIG_PGTABLE_LEVELS=5`

从编译配置看，这是一个：

- `4K` 页粒度
- `52-bit` 虚拟地址
- `5` 级页表

的 ARM64 内核。

但是，**编译配置支持 52-bit/5 级页表**，并不等于**运行时一定实际启用 52-bit/5 级页表**。  
最终是否真的能启用，还取决于硬件是否支持 `LPA2`。

---

## 2. 本次环境的最终结论

通过启动日志已经确认：

```text
ARM64_PGTABLE_STATUS: config_va_bits=52 config_levels=5 runtime_va_bits=48 runtime_levels=4 downgraded
```

这条日志说明：

- 编译时配置：`52-bit VA`、`5` 级页表
- 运行时实际：`48-bit VA`、`4` 级页表
- 最终发生了降级：`downgraded`

因此，本机当前真实运行状态是：

```text
PGD -> PUD -> PMD -> PTE
```

而不是：

```text
PGD -> P4D -> PUD -> PMD -> PTE
```

也就是说，`P4D` 在运行时被 folded 掉了。

---

## 3. 为什么 52-bit 配置最终会降级成 48-bit

### 3.1 编译时上限不等于运行时结果

对于 ARM64，这里的 `52-bit` 是一个**可选上限**。  
如果硬件不支持 `LPA2`，那么内核启动时会回退到最小可保证的 VA 宽度。

对当前 `4K page` 场景来说：

- 编译时目标：`52-bit`
- 运行时不支持 `LPA2`
- 最终回退到：`48-bit`

### 3.2 启动早期会明确降级

ARM64 早期启动代码会根据硬件能力决定最终 `va_bits`。

如果硬件不支持 `LPA2`，则：

- `va_bits = VA_BITS_MIN`
- 同时减少一级页表根级数

对当前 `4K` 配置，`VA_BITS_MIN = 48`。

所以最终结果就是：

- `runtime_va_bits = 48`
- `runtime_levels = 4`

---

## 4. 为什么 52-bit VA 不能直接用 4 级页表完整覆盖

在 `4K` 页粒度下：

- 页内偏移：`12 bit`
- 每级页表索引：`9 bit`

因此：

- `4` 级页表最多覆盖：`12 + 4 * 9 = 48 bit`
- `5` 级页表最多覆盖：`12 + 5 * 9 = 57 bit`

ARM64 在 `4K + 52-bit VA` 场景下，需要借助第 5 级来覆盖 52-bit 地址空间。

所以：

- `48-bit VA` -> `4` 级够用
- `52-bit VA` -> `4K` 场景下必须 `5` 级

结论是：

**如果当前机器不支持 52-bit VA，那么内核不是“继续保留 52-bit 地址，只用 4 级硬撑”，而是直接把运行时 VA 宽度降成 48-bit。**

---

## 5. 为什么降级成 48-bit 后还会看到 P4D folding

很多人容易误解这一点：

> 既然已经降成 48-bit/4 级了，为什么还要有 “P4D folded” 这种逻辑？

答案是：

**folding 是软件接口层的统一机制，不是额外多建了一层硬件页表。**

### 5.1 硬件上已经是 4 级

当运行时不支持 `LPA2` 时，实际使用的硬件页表就是：

```text
PGD -> PUD -> PMD -> PTE
```

不会再去建立真实的 `P4D` 页表页。

### 5.2 软件接口上仍保留 `p4d`

Linux 为了统一页表遍历接口，会把逻辑访问路径保持为：

```text
pgd -> p4d -> pud -> pmd -> pte
```

如果某一级在当前运行模式下不存在，就把这一层 folded 掉。

这带来的好处是：

- 同一套通用页表代码可同时兼容 3/4/5 级
- 同一个 5 级编译内核，可以在不同硬件上运行成 5 级或 4 级
- 上层代码不需要大量写分叉逻辑

### 5.3 folded 的含义

folded 并不是：

```text
再额外多走一层硬件页表
```

而是：

```text
少一级真实硬件页表 + 保留一级软件接口
```

这是理解 ARM64 运行时页表降级逻辑的关键。

---

## 6. 为什么 folded 不能简单理解为 “PGD == P4D”

表面上看，少了一层，好像“直接把 `PGD` 当成 `P4D` 用”就行了。  
但 Linux 这里做得更细。

`pgd_to_folded_p4d()` 的核心逻辑不是简单返回当前 `pgdp`，而是：

1. 先找到 `pgdp` 所在的那一整页页表页起始地址
2. 再根据 `p4d_index(addr)`，在这一页里找逻辑上的 `p4d` 槽位

也就是说 folded 的本质是：

- **同一张页表页**
- 被解释成上一层与折叠层共享的逻辑视图

而不是：

- 当前 `pgdp` 指针本身就等于 `p4dp`

因此，不能粗暴地把 folded 理解为：

```text
PGD 和 P4D 完全是同一个指针
```

更准确的说法是：

```text
P4D 在软件接口上存在，但在底层存储上被并入顶层页表页中。
```

### 6.1 一个具体虚拟地址的单步例子

为了把 folded 的行为看得更清楚，下面选一个具体虚拟地址：

```text
va = 0x0003456789abcd
```

之所以选这个地址，是因为它同时落在 `48-bit` 和 `52-bit` 都可表示的范围内，  
因此可以用同一个地址对比：

- 真正 `5` 级页表
- `P4D folded` 后的 `4` 级页表

#### 6.1.1 在 4K 页下的地址切分

对 `4K page`：

- 页内偏移：`12 bit`
- 每级页表索引：`9 bit`

因此，在 `5` 级页表配置下，这个地址可以切成：

```text
| [51:48] | [47:39] | [38:30] | [29:21] | [20:12] | [11:0] |
|   PGD   |   P4D   |   PUD   |   PMD   |   PTE   | offset |
```

这个具体地址的各级索引为：

- `pgd_index = 0x0`
- `p4d_index = 0x6`
- `pud_index = 0x115`
- `pmd_index = 0x13c`
- `pte_index = 0x9a`
- `offset = 0xbcd`

下面重点看 `pgd -> p4d` 这一段。

#### 6.1.2 情况一：真正启用 5 级页表

先做：

```text
pgdp = pgd_offset(mm, va)
```

由于 `pgd_index = 0x0`，因此：

```text
pgdp = &mm->pgd[0]
```

然后做：

```text
p4dp = p4d_offset(pgdp, va)
```

在真 `5` 级页表下，`pgtable_l5_enabled() == true`，  
此时 `pgd[0]` 里存放的是“下一张独立 P4D 页表页”的地址，  
因此 `p4d_offset()` 的含义相当于：

```text
p4dp = &p4d_page[0x6]
```

也就是说：

- `pgdp` 指向的是顶层 `PGD` 对象中的第 0 项
- `p4dp` 指向的是**另一张独立 P4D 页表页**中的第 6 项

示意如下：

```text
PGD object
+----+----+----+----+ ...
|[0] |[1] |[2] |[3] | ...
+----+----+----+----+ ...
  |
  +--> 指向独立的 P4D page

P4D page
+----+----+----+----+----+----+----+ ...
|[0] |[1] |[2] |[3] |[4] |[5] |[6] | ...
+----+----+----+----+----+----+----+ ...
                                ^
                                |
                              p4dp
```

#### 6.1.3 情况二：`P4D folded`，运行时按 4 级页表工作

如果运行时已经降级成 `48-bit VA + 4-level page table`，  
那么 `pgtable_l5_enabled() == false`。

这时：

- `P4D` 不再有独立的真实页表页
- 顶层页表本身是一整页
- `p4d_offset()` 会把这张顶层页表页重新解释成逻辑上的 `p4d_t[]`

此时第一步仍然是：

```text
pgdp = pgd_offset(mm, va)
```

由于 `pgd_index = 0x0`，仍然有：

```text
pgdp = &top_level_page[0]
```

但第二步就不一样了。  
此时 `p4d_offset()` 会走 folded 路径，等价含义是：

```text
p4dp = &((p4d_t *)top_level_page_base)[0x6]
```

也就是说：

- `pgdp` 在这张页里的第 `0` 项
- `p4dp` 在同一张页里的第 `6` 项

示意如下：

```text
Top-level page
+----+----+----+----+----+----+----+----+ ...
|[0] |[1] |[2] |[3] |[4] |[5] |[6] |[7] | ...
+----+----+----+----+----+----+----+----+ ...
  ^                             ^
  |                             |
 pgdp                          p4dp
```

#### 6.1.4 这个例子说明了什么

这个例子最重要的结论是：

- folded 以后，`p4dp` 不是简单地等于 `pgdp`
- folded 也不是“当前 `PGD` 项直接当作 `P4D` 项”
- folded 的真实含义是：
  - 先根据 `pgdp` 找到**它所在的整张顶层页表页**
  - 再根据 `p4d_index(addr)` 找到这张页中的逻辑 `P4D` 槽位

因此，下面这种理解是不准确的：

```text
PGD == P4D
```

更准确的理解应该是：

```text
P4D folded 后，不再有独立的 P4D page；
但 p4d 这一层的接口仍然保留，只是被映射到顶层页表页中的另一个逻辑槽位。
```

### 6.1.5 上面这个例子的源码依据链

上面“`pgdp` 在这张页里的第 0 项、`p4dp` 在同一张页里的第 6 项”并不是示意图里的随意假设，  
而是可以直接从 Linux 页表 helper 的定义推出来。

#### 第一步：为什么 `pgdp` 是第 0 项

`pgd_offset()` 的核心定义如下：

```text
pgdp = mm->pgd + pgd_index(addr)
```

其中 `pgd_index()` 的含义是：

```text
pgd_index(addr) = (addr >> PGDIR_SHIFT) & (PTRS_PER_PGD - 1)
```

对当前例子地址：

```text
va = 0x0003456789abcd
```

在 `4K + CONFIG_PGTABLE_LEVELS=5` 的编译配置下：

- `PGDIR_SHIFT = 48`
- 所以 `pgd_index = (va >> 48) & 0xf`

由于这个地址本身小于 `2^48`，因此高位部分为 0，最终得到：

```text
pgd_index = 0
```

所以：

```text
pgdp = &mm->pgd[0]
```

这就是为什么前面说：

```text
pgdp 在这张页里的第 0 项
```

#### 第二步：为什么 `p4dp` 是第 6 项

folded 场景下，`p4d_offset()` 最终会走到 `pgd_to_folded_p4d()`，其关键逻辑可以概括为：

```text
p4dp = page_base + p4d_index(addr)
```

这里：

- `page_base = PTR_ALIGN_DOWN(pgdp, PAGE_SIZE)`
- `p4d_index(addr) = (addr >> P4D_SHIFT) & (PTRS_PER_P4D - 1)`

对同一个例子地址：

```text
va = 0x0003456789abcd
```

可以算出：

```text
p4d_index = 0x6
```

因此 folded 路径下：

```text
p4dp = &((p4d_t *)top_level_page_base)[6]
```

这就是为什么前面说：

```text
p4dp 在同一张页里的第 6 项
```

#### 第三步：为什么可以说“同一张页”

在当前系统里，运行时已经降级成：

```text
48-bit VA + 4-level page table
```

因此 `p4d` 是 folded 的，而顶层页表会按整页分配。  
也正因为如此，folded 路径里才会先做：

```text
PTR_ALIGN_DOWN(pgdp, PAGE_SIZE)
```

其意义就是：

```text
先根据 pgdp 找到它所在的那张顶层页表页的页基址
```

然后再根据 `p4d_index(addr)` 在这张页里定位逻辑上的 `p4d` 槽位。

#### 第四步：这条依据链最后说明了什么

因此，对示例地址：

```text
va = 0x0003456789abcd
```

可以严格推出：

- `pgd_index = 0`
- `p4d_index = 6`
- `pgdp = &top_level_page[0]`
- `p4dp = &top_level_page[6]`（folded 视角下）

这说明：

- `p4dp` 不是简单地等于 `pgdp`
- `PGD == P4D` 不是准确说法
- folded 的本质是“同一张页表页上的逻辑重解释”

### 6.1.6 `fixmap.c` 里 `pgdp` / `p4dp` 的特殊关系

在 `arch/arm64/mm/fixmap.c` 的早期 fixmap 初始化代码里，有这样两行：

```c
pgd_t *pgdp = pgd_offset_k(addr);
p4d_t *p4dp = p4d_offset_kimg(pgdp, addr);
```

这两行在 folded 场景下，正好是理解 “`pgdp` 和 `p4dp` 到底是什么关系” 的一个很好的实际例子。

#### 6.1.6.1 `pgdp` 是什么

`pgd_offset_k(addr)` 最终就是在 `init_mm` 的顶层页表里，取出 `addr` 对应的那一个 `PGD` 项：

```text
pgdp = &init_mm.pgd[pgd_index(addr)]
```

也就是说，`pgdp` 表示：

```text
当前地址对应的顶层页表项
```

#### 6.1.6.2 `p4dp` 在 folded 时是什么

`fixmap.c` 这里调用的不是普通 `p4d_offset()`，而是：

```text
p4d_offset_kimg(pgdp, addr)
```

它是专门给静态分配的内核页表用的 helper。  
在 folded 场景下，它最终会退化到：

```text
p4dp = pgd_to_folded_p4d(pgdp, addr)
```

其核心逻辑可以概括为：

```text
p4dp = (p4d_t *)page_base(pgdp) + p4d_index(addr)
```

也就是说，`p4dp` 不是来自：

```text
pgdp 指向的下一张独立 P4D 页表页
```

而是来自：

```text
pgdp 所在的这一整张顶层页表页
```

#### 6.1.6.3 这两者之间到底有什么特殊关系

folded 之后，`pgdp` 和 `p4dp` 的关系可以总结为：

1. **它们属于同一张顶层页表页**
2. **它们代表不同的逻辑层**
3. **它们通常不是同一个槽位，也不是同一个指针**

因此，folded 之后：

```text
pgdp 不是 “直接等于” p4dp
```

更准确地说：

```text
pgdp 是当前地址对应的顶层 PGD 项；
p4dp 是把 pgdp 所在整页重新解释为 p4d_t[] 之后，
再按 p4d_index(addr) 定位到的逻辑 P4D 项。
```

#### 6.1.6.4 为什么这里要强调 `p4d_offset_kimg()`

这里不是普通的运行时页表遍历，而是早期 fixmap 初始化路径。  
这时候操作的是静态分配的内核页表，因此专门使用了 `_kimg` 版本的 helper。

它的特殊点在于：

- **未 folded 时**：会去访问下一层真实页表页
- **folded 时**：不会寻找独立的下一层页表页，而是直接在 `pgdp` 所在页里做逻辑重解释

因此，这两行代码非常直观地体现了：

```text
folded != 真实多一层页表
folded = 在同一张顶层页表页上保留下一层的软件接口视图
```

### 6.2 是否所有虚拟地址都要“走一趟 folded”

这里还需要补充一个很容易混淆的问题：

> 当前内核编译配置是 52-bit，但硬件只支持 48-bit，是否意味着所有虚拟地址都要“走一趟 folded”？

答案是：

- **从软件页表遍历接口角度看，可以认为是全局 folded**
- **从硬件地址翻译角度看，并不存在“硬件多走一层 folded”**

#### 6.2.1 什么叫“全局 folded”

当前系统已经确认：

- `config_va_bits = 52`
- `config_levels = 5`
- `runtime_va_bits = 48`
- `runtime_levels = 4`

这意味着：

- 编译时支持 `P4D`
- 运行时真实硬件页表只有 `4` 级
- 因此 `p4d` 在软件接口层是 folded 的

这里的 folded 不是按“某个具体地址是否小于 48-bit”单独决定的，  
而是按**当前系统运行模式**统一决定的。

也就是说，在当前系统里：

```text
p4d 是全局 folded
```

因此，只要某段内核代码是通过通用页表 helper 去遍历页表，例如：

- `pgd_offset()`
- `p4d_offset()`
- `pud_offset()`

那么 `p4d_offset()` 都会落到 folded 路径。

#### 6.2.2 这不是说硬件真的多走了一层

要特别注意，folded 是 Linux 软件页表抽象的一部分，  
不是硬件 MMU 真正多走的一层页表。

当前系统运行时已经是：

```text
48-bit VA + 4-level page table
```

所以硬件实际执行的地址翻译仍然只是：

```text
PGD -> PUD -> PMD -> PTE
```

硬件并不知道 Linux 的 `p4d folded` 抽象，它只看到最终建立好的 4 级硬件页表。

#### 6.2.3 哪些场景会“走 folded 逻辑”

下面这些场景里，可以认为会进入 folded 逻辑：

- 内核软件手工遍历页表
- 缺页处理路径中访问页表
- 建立/修改页表
- 页表调试、页表转储、软件检查页表

而普通指令访问虚拟地址时，例如：

- CPU load/store
- 指令取值
- TLB 命中的地址访问

这些通常不会去执行 Linux 里的 `p4d_offset()` 之类的 C helper。

所以更准确的说法应该是：

```text
当前系统中，所有通过软件页表遍历接口处理的虚拟地址，
其 p4d 这一层都会按 folded 规则处理；
但硬件实际做的仍然只是 4 级地址翻译。
```

---

## 7. 52-bit 编译、48-bit 运行时，符号地址为什么不会乱

另一个常见疑问是：

> 编译时按 52-bit 生成的符号地址，运行时降成 48-bit 后，虚拟地址不会全变吗？

答案是：**不会**。

原因不是“52-bit 地址 magically 还能继续全用”，而是 ARM64 在设计时就考虑了 fallback。

### 7.1 内核镜像区域放在 `VA_BITS_MIN` 兼容窗口

内核镜像的虚拟地址基准并不是放在“只有 52-bit 才可访问”的区域里。  
ARM64 把 `KIMAGE_VADDR`、模块区等放在一个兼容 `VA_BITS_MIN` 的窗口附近。

对当前 `4K` 场景，`VA_BITS_MIN = 48`。

所以当运行时从 52-bit 回退到 48-bit 时：

- 内核镜像区仍然在有效范围内
- `_text`、函数符号、模块窗口等不会因为回退而天然失效

### 7.2 启动时先决定最终 va_bits，再建立最终映射

运行过程不是：

```text
先固定按 52-bit 建好所有最终映射
然后硬把 CPU 改成 48-bit
```

而是：

```text
先探测硬件是否支持 52-bit/LPA2
如果不支持，就把实际 va_bits 降成 48
然后再按这个最终结果建立映射
```

所以不会出现“页表还保留着只对 52-bit 有效的最终布局，却突然切到 48-bit”这种错误场景。

### 7.3 地址换算使用运行时偏移

内核镜像区和物理地址之间的转换还依赖 `kimage_voffset` 等运行时偏移，  
并不是把所有符号死绑在一个绝对 52-bit 虚拟地址模式上。

因此：

- 编译是按 `52-bit capable kernel`
- 运行时可以退回 `48-bit active kernel VA`
- 这两者在 ARM64 上是兼容设计，而不是互相冲突

---

## 8. 如何证明当前机器最终只运行在 48-bit/4 级

### 8.1 启动日志

当前已经通过下面的日志确认：

```bash
dmesg | grep ARM64_PGTABLE_STATUS
```

输出：

```text
ARM64_PGTABLE_STATUS: config_va_bits=52 config_levels=5 runtime_va_bits=48 runtime_levels=4 downgraded
```

这是最直接的运行时证据。

### 8.2 GDB helper

为了进一步验证，代码里已经加入了 3 个 helper：

- `dbg_read_tcr_el1()`
- `dbg_read_vabits_actual()`
- `dbg_read_pgtable_levels()`

在 GDB 中可以直接查看：

```gdb
p/x dbg_read_tcr_el1()
p/d dbg_read_vabits_actual()
p/d dbg_read_pgtable_levels()
```

如果结果为：

- `dbg_read_vabits_actual() == 48`
- `dbg_read_pgtable_levels() == 4`

那么就说明运行时真实页表模式就是：

```text
48-bit VA + 4-level page table
```

### 8.3 通过 TCR_EL1 反推

对 ARM64：

```text
vabits_actual = 64 - TCR_EL1.T1SZ
```

因此当：

```text
runtime_va_bits = 48
```

时，对应：

```text
TCR_EL1.T1SZ = 16
```

这也能作为寄存器级别的佐证。

---

## 9. 最终总结

本次讨论的最终结论可以概括为下面几条：

1. 当前内核虽然编译成了 `52-bit VA`、`5` 级页表，但运行时实际只启用了 `48-bit VA`、`4` 级页表。
2. 原因是当前硬件或运行环境没有让内核启用 `LPA2`，因此 ARM64 启动时进行了降级。
3. 对 `4K page` 来说，`52-bit VA` 无法由 `4` 级页表完整覆盖，因此降级后必须同时回退到 `48-bit VA`。
4. `P4D folded` 不表示“还在使用 5 级硬件页表”，而是“硬件上已经退化成 4 级，但软件接口仍保留 p4d 这一层”。
5. “folded” 不是简单的 `PGD == P4D`，而是同一张页表页上的逻辑重解释。
6. 52-bit 编译、48-bit 运行不会导致符号地址混乱，因为 ARM64 的内核镜像布局和启动映射过程从一开始就考虑了 fallback 设计。

---

## 10. 一句最容易记住的话

```text
当前内核是 “52-bit capable, but 48-bit active”。
```

或者更口语一点：

```text
编译支持 52-bit，运行实际只有 48-bit。
```
