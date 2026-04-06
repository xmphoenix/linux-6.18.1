# ARM64 Linux 恒等映射与 MMU 开启：关键细节与设计总结

> 基于 Linux 6.18.1，arch/arm64

---

## 一、问题的根源

ARM64 内核链接在高虚拟地址（如 `0xffff_8000_8000_0000`），但 bootloader 加载到的是低物理地址（如 `0x4020_0000`）。MMU 开启前 PC 是物理地址，开启后 CPU 立刻用 PC 值查页表。**这中间存在一个断裂点**——如果页表中只有虚拟地址映射，PC 的物理地址值查不到，CPU 立即 fault。

---

## 二、解决方案：两张页表同时生效

ARM64 硬件将虚拟地址空间一分为二，**由地址 bit[55] 决定查哪张页表**：

| 地址范围 | bit[55] | 页表寄存器 | 用途 |
|---------|---------|-----------|------|
| `0x0000_xxxx_xxxx_xxxx` | 0 | TTBR0_EL1 | 恒等映射（identity map） / 用户空间 |
| `0xffff_xxxx_xxxx_xxxx` | 1 | TTBR1_EL1 | 内核映射（kernel map） |

这是**硬件自动选择**，不需要软件切换。两张页表从 MMU 开启那一刻起同时存在。

---

## 三、恒等映射的本质

恒等映射就是：**虚拟地址 = 物理地址**。

```
页表内容：VA 0x4020_0000 → PA 0x4020_0000
```

它的唯一目的：让**开启 MMU 前后 PC 值都能在页表中找到**。因为 PC 在 MMU 开启前是物理地址，开启后 CPU 拿同一个值去查页表，恒等映射保证查得到。

### 恒等映射中的"虚拟地址"到底是什么值？

**是物理地址。** 此时 MMU 关闭，所有符号通过 PC 相对寻址获取，PC 是物理地址，所以算出的值也是物理地址。恒等映射里 VA 和 PA 在数值上完全相同，都等于内核在内存中的实际加载地址。

---

## 四、关键执行流程

```
┌────────────────────────────────────────────────────────────────────┐
│ ① create_init_idmap()                                              │
│    建恒等映射页表：VA=PA                                             │
│    代码段: 0x4020_0000 → 0x4020_0000 (ROX)                         │
│    数据段: 0x4090_0000 → 0x4090_0000 (RW)                          │
│    使用 2MB 段映射（PMD block descriptor）                           │
│    页表存在 init_idmap_pg_dir                                       │
├────────────────────────────────────────────────────────────────────┤
│ ② __pi_early_map_kernel()                                          │
│    建内核映射页表：高虚拟地址 → 物理地址                               │
│    0xffff_8000_8xxx → 0x4020_xxxx                                   │
│    页表存在 init_pg_dir                                             │
├────────────────────────────────────────────────────────────────────┤
│ ③ __enable_mmu()                                                   │
│    TTBR0 ← init_idmap_pg_dir （恒等映射）                           │
│    TTBR1 ← init_pg_dir       （内核映射）                           │
│    写 SCTLR_EL1.M = 1        （MMU 开启！）                         │
│                                                                     │
│    此刻 PC = 0x4020_xxxx → bit[55]=0 → 查 TTBR0 → 恒等映射 ✓      │
├────────────────────────────────────────────────────────────────────┤
│ ④ ldr x8, =__primary_switched  // x8 = 0xffff_8000_8146_a7f4      │
│    br  x8                                                          │
│                                                                     │
│    PC 变为 0xffff... → bit[55]=1 → 查 TTBR1 → 内核映射 ✓           │
│    从此运行在内核虚拟地址空间                                         │
├────────────────────────────────────────────────────────────────────┤
│ ⑤ 后续清理                                                         │
│    TTBR0 被替换为用户进程页表                                        │
│    init_idmap_pg_dir 内存被回收（标记为 __initdata）                 │
└────────────────────────────────────────────────────────────────────┘
```

### 对应源码位置

| 步骤 | 源码 |
|------|------|
| ① 建恒等映射 | `arch/arm64/kernel/pi/map_range.c: create_init_idmap()` |
| ② 建内核映射 | `arch/arm64/kernel/pi/map_kernel.c: __pi_early_map_kernel()` |
| ③ 开启 MMU | `arch/arm64/kernel/head.S: __enable_mmu` (line 459) |
| ④ 跳转到虚拟地址 | `arch/arm64/kernel/head.S: __primary_switch` (line 508) |
| ⑤ 内核正式启动 | `arch/arm64/kernel/head.S: __primary_switched` (line 220) |

---

## 五、两种关键寻址方式

这是整个机制能工作的编程技巧：

| 指令 | 机制 | MMU off 时得到 | 用途 |
|------|------|---------------|------|
| `adrp x0, sym` | PC + 偏移 **（计算）** | 物理地址 | 访问物理内存中的数据/页表 |
| `ldr x8, =sym` | 从字面量池读常量 **（查表）** | 链接时虚拟地址 | 获取虚拟地址用于跳转到内核空间 |

### `adrp` — PC 相对寻址

```asm
adrp x2, __pi_init_idmap_pg_dir
; 硬件执行: x2 = PC + 编码偏移
; PC 是物理地址 → x2 是物理地址
```

偏移量是链接器计算的 `symbol - 当前指令` 的相对值。无论加载到哪个物理地址，PC 相对寻址都能正确找到目标。

### `ldr =` — 字面量池加载

```asm
ldr x8, =__primary_switched
; 汇编器翻译为:
;   ldr x8, [PC, #offset_to_pool]   // 从代码旁边读 8 字节
;
; 字面量池内容（链接器填入）:
;   .quad 0xffff80008146a7f4         // __primary_switched 的链接地址
```

CPU 从内存读出一个 8 字节整数（用 PC 相对寻址找到字面量池的物理位置），读出的值是链接器写死的虚拟地址。

### 对应 objdump 验证

```
ffff80008145b680:  ldr  x8, 0xffff80008145b698   // 从字面量池加载
ffff80008145b688:  br   x8                        // 跳转到虚拟地址

// 字面量池存储的值:
__primary_switched = 0xffff80008146a7f4
```

`adrp` 用来干活（操作物理内存），`ldr =` 用来跳转（进入虚拟地址世界）。

---

## 六、恒等映射的页表结构

### 页表分配机制

页表页不是定义在固定偏移位置的，而是 `map_range()` 递归过程中通过 `ptep` **线性分配器（bump allocator）** 按需分配的：

```c
phys_addr_t ptep = (phys_addr_t)pg_dir + PAGE_SIZE; // 从 PGD 之后开始分配

// map_range() 中每次需要新页表页时:
if (pte_none(*tbl)) {
    *tbl = __pte(__phys_to_pte_val(*pte) | PMD_TYPE_TABLE | PMD_TABLE_UXN);
    *pte += PTRS_PER_PTE * sizeof(pte_t);  // ptep 前进 4KB
}
```

### 映射类型

在 4KB 页面 + 2MB 对齐（ARM64 boot 规范要求）的典型配置下，恒等映射使用 **2MB 段映射（PMD block mapping）**：

- 页表只需 PGD → PUD → PMD 三级
- PMD 条目直接映射 2MB 物理块，不需要 PTE 级
- 由 `kernel-pgtable.h` 中的配置决定：

```c
// arch/arm64/include/asm/kernel-pgtable.h
#if defined(PMD_SIZE) && PMD_SIZE <= MIN_KIMG_ALIGN
#define SWAPPER_BLOCK_SHIFT   PMD_SHIFT      // 使用 2MB 段映射
#define SWAPPER_SKIP_LEVEL    1               // 跳过 PTE 级
```

### 预留空间

总大小由 `INIT_IDMAP_DIR_SIZE` 在链接脚本中预留：

```c
// arch/arm64/include/asm/kernel-pgtable.h
#define INIT_IDMAP_DIR_SIZE  ((INIT_IDMAP_DIR_PAGES + EARLY_IDMAP_EXTRA_PAGES) * PAGE_SIZE)

// arch/arm64/kernel/vmlinux.lds.S
__pi_init_idmap_pg_dir = .;
. += INIT_IDMAP_DIR_SIZE;
__pi_init_idmap_pg_end = .;
```

---

## 七、`__pi_` 前缀：位置无关代码

### 生成机制

```makefile
# arch/arm64/kernel/pi/Makefile
$(obj)/%.pi.o: OBJCOPYFLAGS := --prefix-symbols=__pi_
$(obj)/%.pi.o: $(obj)/%.o $(obj)/relacheck FORCE
	$(call if_changed,piobjcopy)
```

1. `map_range.c` 编译为 `map_range.o`（符号名：`create_init_idmap`）
2. `objcopy --prefix-symbols=__pi_` → `map_range.pi.o`（符号名：`__pi_create_init_idmap`）

### 编译选项

```makefile
KBUILD_CFLAGS := ... -fpie -Os ...
CFLAGS_map_range.o += -mstrict-align  # MMU off 时禁止非对齐访问
```

### 设计目的

- `-fpie` 确保所有地址引用都是 PC 相对的
- `__pi_` 前缀避免与内核主体代码中的同名符号冲突
- 明确标记这些符号在 MMU 关闭时可安全运行

---

## 八、为什么这样设计

### 1. 为什么不能直接跳到虚拟地址开 MMU？

MMU 开启是一条指令（`msr sctlr_el1, x0`），执行它时 PC 是物理地址，下一条指令 PC+4 也是物理地址。如果页表里只有虚拟地址映射，MMU 开启瞬间 CPU 就找不到下一条指令。

### 2. 为什么用 TTBR0 放恒等映射？

物理地址都是低地址（`0x4020_xxxx`，bit[55]=0），自然落在 TTBR0 管辖范围。内核虚拟地址都是高地址（`0xffff...`，bit[55]=1），落在 TTBR1。ARM64 的双页表硬件设计天然适合这个用途。

### 3. 为什么恒等映射用 2MB 段映射？

- bootloader 保证内核加载地址 2MB 对齐
- 段映射只需 PGD→PUD→PMD 三级，不需要 PTE 级
- 页表页数少，分配快，内存占用小
- 这只是临时映射，够用即可

### 4. 为什么恒等映射是临时的？

恒等映射占用了用户空间的虚拟地址范围（TTBR0），内核启动完成后 TTBR0 要交给用户进程使用。而且恒等映射的虚拟地址值等于物理地址，在运行时没有实际意义，纯粹是为了过渡。

### 5. 为什么用 `__pi_` 前缀的位置无关代码？

MMU 开启前，所有代码必须用 PC 相对寻址（PC 是物理地址，不是链接时虚拟地址）。`__pi_` 代码用 `-fpie` 编译，所有地址引用都是 PC 相对的，确保无论加载到哪个物理地址都能正确运行。

---

## 九、一句话总结

**恒等映射是一座桥：让 CPU 从"物理地址世界"安全走到"虚拟地址世界"。桥的两端分别由 TTBR0 和 TTBR1 承载，跨过桥之后（`br x8` 跳到 `0xffff...`），桥就被拆掉了。**
