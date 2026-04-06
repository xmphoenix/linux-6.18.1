# ARM64 Linux `map_range()` 函数深度分析

> 基于 Linux 6.18.1，`arch/arm64/kernel/pi/map_range.c`

---

## 一、函数定位

`map_range()` 是 ARM64 内核启动阶段的**核心页表填充函数**，位于位置无关（PI）代码中，在 MMU 关闭状态下运行。通过递归方式，从指定页表级别开始逐级建立虚拟地址到物理地址的映射。

它被 `create_init_idmap()` 调用，用于创建**恒等映射（identity map）**，也被 `early_map_kernel()` 调用创建内核虚拟地址映射。

---

## 二、函数签名与参数

```c
void __init map_range(phys_addr_t *pte, u64 start, u64 end, phys_addr_t pa,
                      pgprot_t prot, int level, pte_t *tbl, bool may_use_cont,
                      u64 va_offset)
```

| 参数 | 类型 | 含义 |
|------|------|------|
| `pte` | `phys_addr_t *` | 页表页分配器指针（bump allocator），指向下一个空闲页表页的物理地址 |
| `start` | `u64` | 映射的虚拟地址起始 |
| `end` | `u64` | 映射的虚拟地址结束（不包含） |
| `pa` | `phys_addr_t` | `start` 对应的物理地址 |
| `prot` | `pgprot_t` | 页表属性（RO/RW/ROX 等） |
| `level` | `int` | 当前页表级别（0=PGD, 1=PUD, 2=PMD, 3=PTE） |
| `tbl` | `pte_t *` | 当前级别的页表基地址 |
| `may_use_cont` | `bool` | 是否允许使用连续页属性（TLB 优化） |
| `va_offset` | `u64` | VA 与 PA 之间的偏移（identity map 时为 0） |

---

## 三、关键常量推导（4KB 页面配置）

```c
PTDESC_ORDER       = 3          // 每个页表条目 2^3 = 8 字节（64位描述符）
PAGE_SHIFT         = 12         // 页面 2^12 = 4KB
PTDESC_TABLE_SHIFT = 12 - 3 = 9 // 每级解析 9 位 VA
PTRS_PER_PTE       = 2^9 = 512  // 每级 512 个条目，512×8 = 4KB = 一页
IDMAP_VA_BITS      = 48
IDMAP_LEVELS       = (48-3-1)/9 = 4
IDMAP_ROOT_LEVEL   = 4 - 4 = 0  // 从 level 0（PGD）开始
```

### 各级别参数

| level | 名称 | lshift | 每条目覆盖 | lmask |
|-------|------|--------|-----------|-------|
| 0 | PGD | 27 | 512GB | `0x7F_FFFF_FFFF` |
| 1 | PUD | 18 | 1GB | `0x3FFF_FFFF` |
| 2 | PMD | 9 | 2MB | `0x1F_FFFF` |
| 3 | PTE | 0 | 4KB | `0xFFF` |

### VA 48位地址划分

```
[47    39][38    30][29    21][20    12][11     0]
  9 bits    9 bits    9 bits    9 bits   12 bits
  L0索引    L1索引    L2索引    L3索引   页内偏移
  (PGD)     (PUD)     (PMD)     (PTE)
```

---

## 四、初始化代码：各变量含义

```c
u64 cmask = (level == 3) ? CONT_PTE_SIZE - 1 : U64_MAX;
```
连续页掩码。只有 level 3（PTE 级）支持连续页属性，其他级别设为 `U64_MAX` 使检查永远不通过。

```c
ptdesc_t protval = pgprot_val(prot) & ~PTE_TYPE_MASK;
```
提取保护位，清除类型位（bit[1:0]），后面根据级别重新设置。

```c
int lshift = (3 - level) * PTDESC_TABLE_SHIFT;
```
本级条目覆盖范围的位移量。

```c
u64 lmask = (PAGE_SIZE << lshift) - 1;
```
本级地址对齐掩码，用于检测地址是否对齐到本级边界。

```c
tbl += (start >> (lshift + PAGE_SHIFT)) % PTRS_PER_PTE;
```
计算 `start` 在本级页表中的索引，将 `tbl` 指向对应条目。

---

## 五、递归判断条件

```c
if (level < 2 || (level == 2 && (start | next | pa) & lmask))
```

| 条件 | 含义 |
|------|------|
| `level < 2` | L0、L1 **无条件递归**（必须创建下级表） |
| `level == 2 && 地址不对齐` | L2 但 start/next/pa 任一不是 2MB 对齐 → 递归到 L3 |
| `level == 2 && 全部对齐` | 直接写 2MB **block 描述符**（段映射） |
| `level == 3` | **永远不递归**，写 4KB page 描述符（递归终止） |

循环退出条件：`while (start < end)` — 当 `start == end` 时范围耗尽，函数返回，递归逐层回退。

---

## 六、分配下级页表（递归分支）

```c
if (pte_none(*tbl)) {
    *tbl = __pte(__phys_to_pte_val(*pte) |
                 PMD_TYPE_TABLE | PMD_TABLE_UXN);
    *pte += PTRS_PER_PTE * sizeof(pte_t);
}
```

### 逐行解释

1. **`pte_none(*tbl)`** — 检查当前条目是否为空（全 0）。两次 `map_range` 调用（代码段 + 数据段）可能复用同一个上级条目，已有的不重复分配。

2. **`__phys_to_pte_val(*pte)`** — 将分配器当前物理地址格式化为描述符地址字段（bit[47:12]）。

3. **`PMD_TYPE_TABLE`** — `0b11`，标记为表描述符，告诉 MMU 需要继续查下一级页表。

4. **`PMD_TABLE_UXN`** — bit[60]=1，用户态不可执行，防止用户空间执行内核代码。

5. **`*pte += 512 × 8 = 4096`** — 分配器前进 4KB（一个页表页大小）。

### 描述符格式

```
64位表描述符:
[63] [62:61] [60]  [59:48] [47:12]           [11:2] [1:0]
 0    00      1     0...0   下级页表物理页帧号   0      11
              UXN                                     TABLE
```

### 内存变化示意

```
操作前:                              操作后:
PGD[0] = 0（空）                    PGD[0] = 物理地址|UXN|0b11（表描述符）
*pte → pg_dir+0x1000               *pte → pg_dir+0x2000
pg_dir+0x1000: [空 4KB]            pg_dir+0x1000: [新 L1 表，全 0]
```

---

## 七、直接映射（叶节点分支）

```c
*tbl = __pte(__phys_to_pte_val(pa) | protval);
```

level 2 且 2MB 对齐时，写入 **block 描述符**（`PMD_TYPE_SECT = 0b01`）：

```
64位 block 描述符:
[47:21]           [20:12]  [11:2]     [1:0]
物理块基地址(2MB)   0        保护属性    01
                                      SECT
```

直接将 2MB 物理块映射到虚拟地址，不再需要 L3 页表。

---

## 八、恒等映射调用场景

```c
create_init_idmap(pg_dir, 0):
  map_range(&ptep, _stext, __initdata_begin, _stext, ROX, level=0, pg_dir)
  map_range(&ptep, __initdata_begin, _end, __initdata_begin, RW, level=0, pg_dir)
```

### `_stext` 为什么是物理地址？

- 声明为 `extern char _stext[]`（数组，非指针）
- PI 代码用 `-fpie` 编译
- `(u64)_stext` 编译为 `adrp + add`（PC 相对寻址）
- MMU 关闭时 PC 是物理地址 → 结果是物理地址

```c
/* MMU is off; pointer casts to phys_addr_t are safe */
```

### 两段映射

| 范围 | 属性 | 含义 |
|------|------|------|
| `_stext` → `__initdata_begin` | `PAGE_KERNEL_ROX` | 代码段：只读 + 可执行 |
| `__initdata_begin` → `_end` | `PAGE_KERNEL` | 数据段：可读写，不可执行 |

VA = PA（恒等映射），`va_offset = 0`。

---

## 九、完整递归执行流程

假设 `_stext = 0x4020_0000`，`__initdata_begin = 0x4090_0000`（112MB 代码段）：

```
map_range(level=0, start=0x4020_0000, end=0x4090_0000)
│
│ lshift=27, lmask=512GB-1
│ tbl = PGD[0]（index=0，因为地址 < 512GB）
│ level<2 → 无条件递归
│ PGD[0] 为空 → 分配 L1 表，ptep += 4KB
│ PGD[0] 写入表描述符 → 指向新 L1 表
│
└→ map_range(level=1, start=0x4020_0000, end=0x4090_0000)
   │
   │ lshift=18, lmask=1GB-1
   │ tbl = PUD[1]（index=1，因为 0x4020_0000>>30 = 1）
   │ level<2 → 无条件递归
   │ PUD[1] 为空 → 分配 L2 表，ptep += 4KB
   │ PUD[1] 写入表描述符 → 指向新 L2 表
   │
   └→ map_range(level=2, start=0x4020_0000, end=0x4090_0000)
      │
      │ lshift=9, lmask=2MB-1
      │ tbl = PMD[1]（index = 0x4020_0000>>21 % 512 = 1）
      │ 地址全部 2MB 对齐 → 直接写 block 描述符
      │
      │ 循环 56 次（112MB / 2MB = 56）:
      │   PMD[1]  = 0x4020_0000 | protval | SECT
      │   PMD[2]  = 0x4040_0000 | protval | SECT
      │   PMD[3]  = 0x4060_0000 | protval | SECT
      │   ...
      │   PMD[56] = 0x40E0_0000 | protval | SECT
      │   start == end → while 退出
      │
      └ 返回 level=1
   │
   │ start=end → while 退出
   └ 返回 level=0
│
│ start=end → while 退出
└ 返回 create_init_idmap
```

### 最终页表结构

```
pg_dir+0x0000 [PGD]:  [0] → pg_dir+0x1000
pg_dir+0x1000 [PUD]:  [1] → pg_dir+0x2000
pg_dir+0x2000 [PMD]:  [1]  = 0x4020_0000 (2MB block, ROX)
                       [2]  = 0x4040_0000 (2MB block, ROX)
                       ...
                       [56] = 0x40E0_0000 (2MB block, ROX)

ptep 最终 = pg_dir + 0x3000（共消耗 3 个页表页：PGD + PUD + PMD）
```

---

## 十、恒等映射的目的与生命周期

### 为什么需要恒等映射？

MMU 开启是一条指令（`msr sctlr_el1, x0`），执行时 PC 是物理地址（如 `0x4020_xxxx`），开启后 CPU 立刻用 PC 值查页表。如果页表中只有虚拟地址映射（`0xffff...`），PC 的物理地址值查不到 → Translation Fault → 崩溃。

恒等映射保证：VA `0x4020_xxxx` → PA `0x4020_xxxx`，MMU 开启后 PC 值仍然有效。

### ARM64 双页表机制

| 地址范围 | bit[55] | 页表寄存器 | 用途 |
|---------|---------|-----------|------|
| `0x0000_xxxx...` | 0 | TTBR0_EL1 | 恒等映射 / 用户空间 |
| `0xffff_xxxx...` | 1 | TTBR1_EL1 | 内核映射 |

硬件自动根据虚拟地址高位选择页表，两套映射**同时存在**。

### MMU 开启后的跳转

```asm
ldr  x8, =__primary_switched    // x8 = 0xffff_8000_8146_a7f4（链接虚拟地址）
br   x8                          // 跳转 → bit[55]=1 → 自动切到 TTBR1 内核映射
```

- `ldr =` 从字面量池读取链接时写死的虚拟地址常量
- `adrp` 用 PC 相对计算得到物理地址

两种寻址方式**配合使用**：`adrp` 操作物理内存，`ldr =` 获取虚拟地址跳转。

### 生命周期

```
① create_init_idmap()   — 建恒等映射
② __enable_mmu()        — TTBR0=恒等映射, TTBR1=内核映射, 开 MMU
③ br x8                 — 跳到 0xffff... 虚拟地址
④ 后续                  — TTBR0 替换为用户进程页表，恒等映射内存回收
```

**恒等映射是一座桥：让 CPU 从物理地址世界安全走到虚拟地址世界。跨过桥后，桥就被拆掉了。**
