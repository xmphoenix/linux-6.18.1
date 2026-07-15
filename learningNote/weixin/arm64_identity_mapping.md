# ARM64 Identity Mapping 深度解析
## MMU OFF 阶段的恒等映射

> **📖 导读：**ARM64 Linux 内核从 `primary_entry` 开始执行时，*MMU 处于关闭状态*——CPU 发出的所有地址都是物理地址。要安全地开启 MMU，必须首先建立一个 **Identity Mapping（恒等映射）**：VA = PA 的页表映射。本文从"为什么需要"出发，逐层拆解恒等映射的硬件背景、映射范围、映射粒度以及完整的生命周期。

## 一、核心问题：为什么要恒等映射？

考虑 MMU 开启瞬间发生的事情。在 `__enable_mmu` 中执行 `set_sctlr_el1` 写入 SCTLR_EL1.M=1 的那一刻：

```
set_sctlr_el1 x0      ← ★ 写入 SCTLR_EL1，M 位置 1，MMU 开启！
ret                    ← 下一条指令的 PC 值...

问题：ret 指令的 PC 是物理地址。MMU 开启后，CPU 会
      将这条指令的 PC 当作 VA 送进页表 walker 去翻译。
```

**如果没有任何映射让物理地址等于虚拟地址，那么硬件 Table Walker 会发现第一条指令所在的页根本找不到映射，立刻触发 Translation Fault，CPU 直接崩溃。**

因此，恒等映射的功能就是：*保证 MMU 开启的那一个时钟周期之后，物理地址能通过页表翻译回自身，让 CPU 无缝继续执行下去*。

> **一句话总结：**Identity Mapping = 物理地址空间到虚拟地址空间的"过桥映射"。MMU 一开，CPU 从物理地址世界进入虚拟地址世界，过桥期间两者必须是同一个地址。

## 二、硬件背景：48-bit IDMAP 地址空间

虽然本文的配置是 `VA_BITS=52`、`CONFIG_PGTABLE_LEVELS=5`（主内核页表为 PGD→P4D→PUD→PMD→PTE 五级），但**恒等映射并不需要覆盖 52-bit 的完整地址空间**。

ARM64 规范允许 TTBR0 和 TTBR1 使用不同的地址宽度。恒等映射使用 TTBR0，其地址宽度由 TCR_EL1.T0SZ 字段控制：

```
TCR_T0SZ = 64 - IDMAP_VA_BITS

// IDMAP_VA_BITS = 48 → T0SZ = 16
// TTBR0 地址空间的最高 16 位必须全 0 或全 1
// 恒等页表只需覆盖 48-bit 地址空间
```

关键宏定义（`arch/arm64/include/asm/kernel-pgtable.h`）：

```
#define IDMAP_VA_BITS      48
#define IDMAP_LEVELS       ARM64_HW_PGTABLE_LEVELS(IDMAP_VA_BITS)
#define IDMAP_ROOT_LEVEL   (4 - IDMAP_LEVELS)
```

展开计算（`PAGE_SHIFT=12`，`PTDESC_TABLE_SHIFT=9`）：

```
IDMAP_LEVELS = ((48 - 3 - 1) / 9) = 44 / 9 = 4
IDMAP_ROOT_LEVEL = 4 - 4 = 0

// 恒等页表的遍历层级：
// level 0 (P4D) → level 1 (PUD) → level 2 (PMD) → level 3 (PTE)
// 共 4 级，根表位于 P4D 层，不需要顶级 PGD（level -1）
```

**这意味着**：恒等映射是 48-bit 四级页表（P4D→PUD→PMD→PTE），而非主内核的 52-bit 五级页表。恒等映射的根表 `init_idmap_pg_dir` 只有一页（4KB），位于 P4D 层级。

| 对比维度 | 主内核映射 (swapper_pg_dir) | 恒等映射 (init_idmap_pg_dir) |
| --- | --- | --- |
| VA 位数 | 52-bit | 48-bit |
| 页表级数 | 5 级 | 4 级 |
| 根表层级 | level -1 (PGD) | level 0 (P4D) |
| 遍历路径 | PGD→P4D→PUD→PMD→PTE | P4D→PUD→PMD→PTE |
| 使用哪个 TTBR | TTBR1_EL1 | TTBR0_EL1 |
| 内存布局 | 高地址虚拟空间 (0xFFFF...) | VA = PA（物理地址范围） |

## 三、create_init_idmap()：两段映射建立恒等页表

恒等映射的建表函数位于 `arch/arm64/kernel/pi/map_range.c`，不过我们不需要深入建造算法本身——只需理解它做了什么、映射了什么。

```
asmlinkage phys_addr_t __init create_init_idmap(pgd_t *pg_dir, ptdesc_t clrmask)
{
    // ① 页表页分配指针：从根页表之后的一页开始
    phys_addr_t ptep = (phys_addr_t)pg_dir + PAGE_SIZE;

    // ② 准备两个权限模板
    pgprot_t text_prot = PAGE_KERNEL_ROX;   // 代码段：只读 + 可执行
    pgprot_t data_prot = PAGE_KERNEL;        // 数据段：可读写

    pgprot_val(text_prot) &= ~clrmask;       // clrmask = 0（head.S 传入 x1 = xzr）
    pgprot_val(data_prot) &= ~clrmask;

    // ③ ★ 第一轮：映射代码段（_stext → __initdata_begin）
    map_range(&ptep,
              (u64)_stext,              // start: 内核代码段起始 VA
              (u64)__initdata_begin,    // end:   init 数据段起始 VA
              (phys_addr_t)_stext,      // pa:    起始物理地址（=VA，恒等！）
              text_prot,                // prot:  PAGE_KERNEL_ROX
              IDMAP_ROOT_LEVEL,         // level: 0（从 P4D 层开始）
              (pte_t *)pg_dir,          // tbl:   根页表基址
              false,                    // may_use_cont: false
              0);                       // va_offset: 0（恒等映射，无偏移）

    // ④ ★ 第二轮：映射数据段（__initdata_begin → _end）
    map_range(&ptep,
              (u64)__initdata_begin,    // start: init 数据段起始
              (u64)_end,                // end:   内核镜像末尾
              (phys_addr_t)__initdata_begin,  // pa: 起始物理地址（=VA）
              data_prot,                // prot:  PAGE_KERNEL
              IDMAP_ROOT_LEVEL,         // level: 0
              (pte_t *)pg_dir,          // tbl:   同一个根页表
              false,                    // may_use_cont: false
              0);                       // va_offset: 0

    // ⑤ 返回已使用的页表空间的末尾物理地址
    return ptep;
}
```

四个关键设计：

- **两段映射共用一个根页表**：text 和 data 写入同一个 `init_idmap_pg_dir`，由 P4D 的不同条目分别索引

- **分两段是为了权限隔离**：代码段 ROX（读+执行），数据段 RW（读+写）。这是 **W^X 安全策略**在启动阶段的最早落地

- **页表页来自预分配池**：MMU OFF 条件下没有 buddy 分配器，页表页直接从根页表之后的物理连续内存中递进取用（每次 `*pte += 4096`）

- **`va_offset=0` 意味着恒等**：在递归建表过程中，下级页表的地址就是物理地址，无需任何转换

## 四、映射范围与粒度：一张图说清楚

### 4.1 恒等映射到底映射了哪些内容？

从代码直接可知：映射范围是 **`_stext → _end`**，即整个内核镜像。按链接脚本顺序逐段拆解：

| 地址区间 | 权限 | 包含的内容（按链接脚本顺序） |
| --- | --- | --- |
| `_stext` → `_etext` | **ROX**（只读+可执行） | `.text` — 内核代码段正文（含 IRQ/SOFTIRQ/ENTRY/SCHED/LOCK/KPROBES 等子段） |
| `_etext` → `__init_begin` | **ROX** | `RO_DATA` — 只读数据（含异常表、pci_fixup、内置固件等） |
| （同上区间） | **ROX** | HYPERVISOR_RODATA_SECTIONS + `.got` / `.got.plt` |
| （同上区间） | **ROX** | `.rodata.text` — 特殊代码段：trampoline / hibernate / kexec / `.idmap.text` |
| （同上区间） | **ROX** | 页表目录占位页（`idmap_pg_dir` · `tramp_pg_dir` · `reserved_pg_dir` · `swapper_pg_dir`） |
| `__init_begin` → `__inittext_end` | **ROX** | `.init.text` — 内核 init 代码段 |
| `__inittext_end` → `__initdata_begin` | **RO** | `.altinstructions` · UNWIND_DATA — 替代指令表 + 展开表 |
| `__initdata_begin` → `_end` | **RW**（可读写） | `init_idmap_pg_dir` — 恒等页表目录本身 |
| （同上区间） | **RW** | `.init.data` — 内核 init 数据段（INIT_DATA/INIT_SETUP/INIT_CALLS 等） |
| （同上区间） | **RW** | `.exit.data` · `.data` · `.bss` · 杂项 — 内核主数据段及 BSS 等 |

> **一句话：**恒等映射覆盖从内核第一条可执行指令到镜像末尾的全部内容。代码段 ROX、数据段 RW，这是 W^X 安全策略在启动阶段的最早落地。典型内核镜像大小约为 **30~50MB**。

### 4.2 映射粒度：4KB 还是 2MB？

答案：**两者都有，2MB block 是主体（90%+），4KB page 只在对齐边界出现。**

恒等映射的建表算法在 level 0 (P4D) 和 level 1 (PUD) 从不直接放叶子条目——一律分配下级页表。到了 level 2 (PMD)，**只有当起始虚拟地址、结束边界、物理地址三者都恰好对齐到 2MB 边界时，才放一个 2MB PMD block；任何一个条件不满足，就下沉到 level 3 放 4KB PTE page。**

以 QEMU 典型加载为例，内核物理地址 `0x40080000`，大小约 32MB：

```
_stext=0x40080000                                      __initdata_begin        _end
    │                                                       │                      │
    ├── 头部不对齐 ──┤                                      │                      │
    │  384 × 4KB    │  N₁ × 2MB PMD blocks (ROX)  │  过渡区  │  N₂ × 2MB (RW)   │尾部│
    │  PTE pages   │                              │ 4KB PTE │                  │4KB │
    │              │                              │ pages   │                  │PTE │
    └──────────────┴──────────────────────────────┴─────────┴──────────────────┴────┘

    0x40080000     0x40200000                                          最后一个2MB边界  _end
    (非2MB对齐!)   (第1个2MB边界)                                    (若不对齐，用4KB补齐)
```

| 区间 | 对齐情况 | 映射方式 | 覆盖范围 |
| --- | --- | --- | --- |
| `_stext` → 首个 2MB 边界 | ❌ `0x40080000 & 0x1FFFFF = 0x80000 ≠ 0` | **4KB PTE pages** × 384 | 约 1.5MB |
| 中间连续对齐区间 | ✅ start / pa / next 均对齐 | **2MB PMD blocks** × ~15 | ~30MB（主体） |
| `__initdata_begin` 边界 | ⚠️ 大概率不对齐 | **4KB PTE pages** × 若干 | ROX→RW 过渡区 |
| 尾部区间 | ⚠️ `_end` 不对齐时 | **4KB PTE pages** × 若干 | 最后不足 2MB 的部分 |

**数字结论：**

- **2MB PMD blocks** 覆盖了 *90% 以上* 的地址空间——这是恒等映射的主体

- **4KB PTE pages** 只出现在 3 处边界：开头的不对齐头部、text→data 权限切换处、镜像末尾

- 对于 32MB 的内核镜像，大约需要 **~15 个 2MB PMD block + ~400 个 4KB PTE page**

## 五、完整的页表结构图

以 QEMU 加载内核到物理地址 `0x40080000`、镜像约 32MB 为例，恒等页表的完整四层结构：

```
                    init_idmap_pg_dir (P4D, level 0)
                    ┌─────────────────────────────┐
        PA=0x40000000 → │ P4D[0]  ─────────────────────┐  ← 覆盖 0x0 - 0x7FFFFFFFF (512GB)
                         │ P4D[1]  = empty              │
                         │ ...                          │
                         │ P4D[511]= empty              │
                         └─────────────────────────────┘
                              │
                              ▼
                    PUD 页表 (level 1)
                    ┌─────────────────────────────┐
        PA=0x40001000 → │ PUD[0]  ─────────────────────┐  ← 覆盖 0x40000000 - 0x7FFFFFFF (1GB)
                         │ PUD[1]  = empty              │
                         │ ...                          │
                         └─────────────────────────────┘
                              │
                              ▼
                    PMD 页表 (level 2) — 主体叶子层
                    ┌─────────────────────────────┐
        PA=0x40002000 → │ PMD[0]  = block: ROX        │  ← 2MB block (PMD_TYPE_SECT)
                         │ PMD[1]  = block: ROX        │
                         │ ...                          │
                         │ PMD[14] = block: ROX        │  ← 代码段最后一个 2MB block
                         │ PMD[15] = block: RW         │  ← __initdata_begin 开始，权限切换
                         │ ...                          │
                         │ PMD[N]  = block: RW         │  ← _end 附近
                         │ PMD[N+1]= TABLE ──────────┐ │  ← 边界不对齐！下沉到 PTE
                         └─────────────────────────────┘
                              │                    │
                              ▼                    ▼
                       直接 2MB 映射          PTE 页表 (level 3)
                       (绝大部分区间)          ┌──────────────────┐
                                              │ PTE[0] = page... │ ← 4KB page (PTE_TYPE_PAGE)
                                              │ PTE[1] = page... │
                                              └──────────────────┘
                                              (仅段边界处需要)
```

**典型数字**：32MB 镜像 = ~16 个 2MB PMD block + 不超过 400 个 4KB PTE page，页表总开销约 5~7 页（20~28KB）。

## 六、MMU 开启瞬间：从恒等映射到虚拟地址空间

恒等映射建好后，`__primary_switch` 负责完成最终的地址空间切换：

```
SYM_FUNC_START_LOCAL(__primary_switch)
    adrp x1, reserved_pg_dir
    adrp x2, __pi_init_idmap_pg_dir
    bl   __enable_mmu             // ← ★ 开启 MMU
    // 此时 MMU 已开，但 PC 仍是低地址
    // TTBR0 = init_idmap_pg_dir 保证 PA=VA 的翻译成立

    adrp x1, early_init_stack
    mov  sp, x1
    mov  x29, xzr
    mov  x0, x20                  // boot status
    mov  x1, x21                  // FDT
    bl   __pi_early_map_kernel    // ← ★ 建立内核虚拟映射（TTBR1）

    ldr  x8, =__primary_switched  // ← 加载高地址虚拟地址 (0xFFFF...)
    adrp x0, KERNEL_START
    br   x8                       // ← ★★★ 跳入高地址虚拟空间！
SYM_FUNC_END(__primary_switch)

```

三次关键跳跃，对应三套页表：

| 阶段 | TTBR0 | TTBR1 | 执行位置 |
| --- | --- | --- | --- |
| MMU OFF | - | - | 物理地址直接执行 |
| MMU ON（恒等映射期） | init_idmap_pg_dir | reserved_pg_dir（空） | PA=VA，低地址空间 |
| br x8 之后 | init_idmap_pg_dir | swapper_pg_dir | 高地址虚拟空间 (0xFFFF...) |

随后在 `setup_arch()` 中调用 `cpu_uninstall_idmap()` 把 TTBR0 指向空页表，**恒等映射被正式拆除**。之后仅在 CPU hotplug/suspend 时通过 `idmap_pg_dir`（`paging_init()` 中重建的最终恒等映射）短暂恢复。

## 七、小结

ARM64 Identity Mapping 涉及的关键认知点：

1. **MMU 开启瞬间是一次"地址空间穿越"**——PA 和 VA 在那一个时钟周期交汇，恒等映射是唯一的桥梁
2. **恒等映射是 48-bit 四级页表**（P4D→PUD→PMD→PTE），而非主内核的 52-bit 五级
3. **映射范围 = 整个内核镜像**（`_stext` → `_end`）：分 ROX 代码段和 RW 数据段，落地 W^X 安全策略
4. **2MB PMD block 是主体**（覆盖 90%+ 区间），4KB PTE page 只出现在头部不对齐、权限切换、尾部三处边界
5. **页表页来自预分配池**——MMU OFF 条件下没有 buddy，直接从链接脚本预留的物理连续内存中递进取用
6. **恒等映射用完即弃**：`cpu_uninstall_idmap()` 将其拆除，之后仅在 CPU hotplug/suspend 时通过 `idmap_pg_dir` 短暂恢复

---

基于 Linux 6.18.1 内核源码分析 | ARM64 架构 | `VA_BITS=52` `PGTABLE_LEVELS=5`

关键源文件：`arch/arm64/kernel/pi/map_range.c` · `arch/arm64/kernel/head.S` · `arch/arm64/include/asm/kernel-pgtable.h`
