# ARM64 Linux 6.18.1 — 从 head.S 到 start_kernel 的 PGD 页表全览

## 概述

ARM64 内核从启动到进入 `start_kernel()` 共使用 **6 个 PGD 页表**，全部在链接脚本
`arch/arm64/kernel/vmlinux.lds.S` 中静态分配。这些页表在启动过程中依次创建、加载、
切换，最终由 `swapper_pg_dir`（即 `init_mm.pgd`）作为内核地址空间的永久页表。

---

## 一、链接脚本中的定义

### `.rodata.text` 段（只读，各 PAGE_SIZE = 4KB）

```
// arch/arm64/kernel/vmlinux.lds.S:223-235

idmap_pg_dir       = .;  . += PAGE_SIZE;        // 运行时恒等映射
tramp_pg_dir       = .;  . += PAGE_SIZE;        // KPTI 蹦床 (条件编译)
reserved_pg_dir    = .;  . += PAGE_SIZE;        // 空占位
swapper_pg_dir     = .;  . += PAGE_SIZE;        // 最终内核页表
```

### `.init.data` 段（可写，多页）

```
// arch/arm64/kernel/vmlinux.lds.S:262-264

__pi_init_idmap_pg_dir = .;  . += INIT_IDMAP_DIR_SIZE;
__pi_init_idmap_pg_end = .;
```

### BSS 段（零初始化，多页）

```
// arch/arm64/kernel/vmlinux.lds.S:337-340

__pi_init_pg_dir = .;  . += INIT_DIR_SIZE;
__pi_init_pg_end = .;
```

---

## 二、6 个 PGD 详解

### 1. `init_idmap_pg_dir` — 启动恒等映射 (TTBR0)

| 属性 | 值 |
|------|----|
| **大小** | `INIT_IDMAP_DIR_SIZE`（多页，含多级页表） |
| **所在段** | `.init.data` |
| **创建时机** | `primary_entry` → `__pi_create_init_idmap()` |
| **加载位置** | TTBR0_EL1 |
| **代码位置** | `arch/arm64/kernel/head.S:92` / `arch/arm64/kernel/pi/map_range.c` |

**作用**：VA=PA 恒等映射，让 MMU 开启瞬间代码地址不变。映射范围：
- 内核代码段（ROX，只读可执行）
- 内核数据段（RW，读写）
- FDT 设备树（RW）

**退役时机**：`setup_arch()` → `cpu_uninstall_idmap()` 后被 `idmap_pg_dir` 替代。

---

### 2. `reserved_pg_dir` — 空占位页表 (TTBR1 临时 / TTBR0 安全中间态)

| 属性 | 值 |
|------|----|
| **大小** | PAGE_SIZE（4KB），内容全零 |
| **所在段** | `.rodata.text` |
| **加载时机** | `__primary_switch` → `__enable_mmu` |
| **加载位置** | TTBR1_EL1（启动时）/ TTBR0_EL1（运行时切换中间态） |
| **代码位置** | `arch/arm64/kernel/head.S:508` |

**作用**：
1. **启动时**：MMU 首次开启时 TTBR1 必须指向有效页表，但此时内核虚拟地址映射尚未建立，用空页表做占位
2. **运行时 TTBR0 中间态**：`cpu_uninstall_idmap()` / `cpu_install_idmap()` 切换 TTBR0 时，先切到 `reserved_pg_dir` 作为安全过渡
3. **运行时 TTBR1 中间态**：`idmap_cpu_replace_ttbr1` 替换 TTBR1 时，先切到 `reserved_pg_dir` 防止 TLB 冲突
4. **SW_TTBR0_PAN**：`init_task.thread_info.ttbr0` 的默认值

---

### 3. `init_pg_dir` — 临时内核映射 (TTBR1 过渡)

| 属性 | 值 |
|------|----|
| **大小** | `INIT_DIR_SIZE`（多页，BSS 段零初始化） |
| **所在段** | BSS |
| **创建时机** | `__pi_early_map_kernel()` |
| **加载位置** | TTBR1_EL1 |
| **代码位置** | `arch/arm64/kernel/pi/map_kernel.c:87-101` |

**作用**：临时的内核虚拟地址映射。用 `map_segment()` 逐段映射：

```
map_segment(init_pg_dir, ..., _text,           _stext,          data_prot);   // 非执行
map_segment(init_pg_dir, ..., _stext,          _etext,          text_prot);   // 代码段
map_segment(init_pg_dir, ..., __start_rodata,  __inittext_begin, data_prot);  // 只读数据
map_segment(init_pg_dir, ..., __inittext_begin, __inittext_end, text_prot);   // init 代码
map_segment(init_pg_dir, ..., __initdata_begin, __initdata_end, data_prot);   // init 数据
map_segment(init_pg_dir, ..., _data,           _end,            data_prot);   // 数据+BSS
```

**与 `swapper_pg_dir` 的关系**：`swapper_pg_dir` 只有 4KB（仅根级 PGD），
`memcpy(swapper_pg_dir, init_pg_dir, PAGE_SIZE)` 只拷贝根级 PGD 条目。
子级页表（PUD/PMD/PTE）仍然位于 `init_pg_dir` 的空间中，被 `swapper_pg_dir`
的 PGD 条目引用。后续 `paging_init()` → `map_mem()` 会重新分配子级页表
（从 memblock 分配），让 `swapper_pg_dir` 的条目指向新的页表页。

**退役时机**：`map_kernel.c:136` 中 memcpy 后弃用，`.init` 段后续被 `free_initmem()` 释放。

---

### 4. `swapper_pg_dir` — 最终内核页表 (TTBR1 永久) ← `init_mm.pgd`

| 属性 | 值 |
|------|----|
| **大小** | PAGE_SIZE（4KB，仅根级 PGD） |
| **所在段** | `.rodata.text` |
| **生效时机** | `map_kernel.c:136-138` memcpy + `idmap_cpu_replace_ttbr1()` |
| **加载位置** | TTBR1_EL1（永久） |
| **关联** | `init_mm.pgd = swapper_pg_dir` |

**作用**：**内核地址空间的永久页表**，这就是 `init_mm.pgd` 指向的页表。

**生命周期**：
1. `early_map_kernel()` 中从 `init_pg_dir` 拷贝根级 PGD 页表
2. `paging_init()` 进一步完善（添加全部物理内存映射、vmalloc 区域等）
3. 运行时所有内核线程共享，vmalloc/ioremap/kmap 等操作都修改此页表
4. 用户进程切换到内核态时，TTBR1 始终指向 `swapper_pg_dir`

---

### 5. `idmap_pg_dir` — 运行时恒等映射 (TTBR0)

| 属性 | 值 |
|------|----|
| **大小** | PAGE_SIZE（4KB 根 PGD + 静态 `idmap_ptes` 子级页表） |
| **所在段** | `.rodata.text` |
| **创建时机** | `paging_init()` → `create_idmap()` |
| **加载位置** | TTBR0_EL1 |
| **代码位置** | `arch/arm64/mm/mmu.c:1325-1349` |

**映射范围**：仅 `.idmap.text` 段（`__idmap_text_start` → `__idmap_text_end`，
约 4KB 的关键函数），远小于 `init_idmap_pg_dir`（映射整个内核+FDT）。

**作用**：运行时场景下需要恒等映射的地方（详见下文第八章 TTBR 切换机制）。

**与 `init_idmap_pg_dir` 的对比**：

| | `init_idmap_pg_dir` | `idmap_pg_dir` |
|--|---|---|
| **大小** | `INIT_IDMAP_DIR_SIZE`（多页） | `PAGE_SIZE` 根 + 静态 `idmap_ptes` |
| **映射内容** | 整个内核镜像 + FDT | 仅 `.idmap.text` 段 (~4KB) |
| **所在段** | `.init.data`（启动后释放） | `.rodata.text`（永久） |
| **创建者** | `__pi_create_init_idmap()` | `paging_init()` → `create_idmap()` |
| **生命周期** | 启动临时 | 永久 |

**为什么不复用 `init_idmap_pg_dir`**：
1. 太大 — 映射整个内核的恒等映射在运行时完全没必要
2. 在 `.init.data` 段 — 启动完成后 `free_initmem()` 会释放其内存
3. 安全性 — 运行时恒等映射越小越好，减少攻击面

---

### 6. `tramp_pg_dir` — KPTI 蹦床页表 (TTBR1)

| 属性 | 值 |
|------|----|
| **大小** | PAGE_SIZE（4KB） |
| **所在段** | `.rodata.text` |
| **条件编译** | `CONFIG_UNMAP_KERNEL_AT_EL0`（Meltdown/KPTI 防御） |
| **加载位置** | TTBR1_EL1（用户态时） |

**作用**：KPTI（Kernel Page Table Isolation）的核心组件。在用户态运行时，
TTBR1 指向 `tramp_pg_dir` 而非 `swapper_pg_dir`，只映射极少量的蹦床代码
（entry/exit trampoline），防止用户态通过 Meltdown 攻击读取内核内存。

用户态 → 内核态切换流程：
```
用户态:       TTBR1 = tramp_pg_dir    (仅映射蹦床代码)
  ↓ 异常/中断
蹦床代码:     TTBR1 切换为 swapper_pg_dir  (完整内核映射)
  ↓
内核态:       TTBR1 = swapper_pg_dir  (正常工作)
```

---

## 三、启动时序中 PGD 的使用顺序

```
阶段    操作                              TTBR0 (低地址/恒等映射)        TTBR1 (高地址/内核)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 ①  primary_entry (MMU 关)               —                              —
 ②  create_init_idmap()                  构建 init_idmap_pg_dir          —
 ③  __enable_mmu (MMU 开)               加载 init_idmap_pg_dir          加载 reserved_pg_dir
 ④  early_map_kernel()                   不变                           构建 init_pg_dir → TTBR1
 ⑤  memcpy + 切换                        不变                           init_pg_dir → swapper_pg_dir → TTBR1
 ⑥  __primary_switched (虚拟地址)         不变                           swapper_pg_dir
 ⑦  start_kernel → setup_arch            不变                           swapper_pg_dir
 ⑧  cpu_uninstall_idmap()               TTBR0 → reserved_pg_dir        swapper_pg_dir
 ⑨  paging_init()                        构建 idmap_pg_dir              完善 swapper_pg_dir
```

---

## 四、关键流转图

```
MMU OFF                        MMU ON (恒等映射)                    MMU ON (虚拟地址)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
              ┌─────────────────┐
primary_entry │create_init_     │    __enable_mmu
  x0 = FDT   │idmap()          │─── TTBR0 = init_idmap_pg_dir ──┐
              └─────────────────┘    TTBR1 = reserved_pg_dir      │
                                          │                        │
                                          ▼                        │
                                     early_map_kernel()            │
                                     构建 init_pg_dir              │
                                     TTBR1 = init_pg_dir ─────────┤
                                          │                        │
                                          ▼                        │
                                     memcpy → swapper_pg_dir       │
                                     TTBR1 = swapper_pg_dir ──────┤
                                          │                        │
                                          ▼                        │
                                     ldr x8, =__primary_switched   │
                                     br x8  ──────────────────────►│
                                                                   ▼
                                                        __primary_switched
                                                        (虚拟地址空间)
                                                                   │
                                                                   ▼
                                                             start_kernel()
                                                                   │
                                                        ┌──────────┴──────────┐
                                                        ▼                     ▼
                                                  setup_arch()          其他初始化
                                                        │
                                          ┌─────────────┼─────────────┐
                                          ▼             ▼             ▼
                                   cpu_uninstall_  paging_init()  init_mm.pgd
                                   idmap()         完善 swapper   = swapper_pg_dir
                                   TTBR0→reserved  + 建 idmap
```

---

## 五、`swapper_pg_dir` 与 `init_mm` 的关系

```c
// mm/init-mm.c
struct mm_struct init_mm = {
    .pgd = swapper_pg_dir,    // 指向最终内核页表
    ...
};
```

`init_mm` 是内核地址空间的唯一 `mm_struct`：
- **所有内核线程** 共享 `init_mm`（内核线程的 `active_mm = &init_mm`）
- **vmalloc / ioremap** 在 `init_mm.pgd`（即 `swapper_pg_dir`）上操作
- **用户进程** 内核态时 TTBR1 也指向 `swapper_pg_dir`，只有 TTBR0 指向各进程自己的页表
- **fork** 时新进程的内核部分页表从 `swapper_pg_dir` 复制

---

## 六、页表大小计算

### `INIT_IDMAP_DIR_SIZE`（init_idmap_pg_dir）

```
// arch/arm64/include/asm/kernel-pgtable.h
IDMAP_VA_BITS = 48
IDMAP_LEVELS  = 4  (L0 → L3)
覆盖范围: 内核镜像 + FDT
```

### `INIT_DIR_SIZE`（init_pg_dir）

```
// arch/arm64/include/asm/kernel-pgtable.h
覆盖范围: KIMAGE_VADDR → _end
级数: CONFIG_PGTABLE_LEVELS (通常 4 级)
含: 根级 PGD + 各级中间页表 + 对齐补偿页
```

### 固定大小页表（各 PAGE_SIZE = 4KB）

| 页表 | 说明 |
|------|------|
| `swapper_pg_dir` | 仅根级 PGD，子级页表在 `paging_init()` 时动态分配 |
| `reserved_pg_dir` | 空页表，全零 |
| `idmap_pg_dir` | 仅根级，子级用静态 `idmap_ptes` 数组 |
| `tramp_pg_dir` | 仅映射蹦床代码 |

---

## 七、TTBR 寄存器与地址选择

ARM64 用虚拟地址 **bit[55]** 选择页表寄存器：

```
虚拟地址 bit[55] = 0  →  TTBR0_EL1  →  用户空间 / 恒等映射
虚拟地址 bit[55] = 1  →  TTBR1_EL1  →  内核空间
```

正常运行时：

```
TTBR0_EL1 = 用户进程页表 (或 reserved_pg_dir)    ← 0x0000_xxxx_xxxx_xxxx
TTBR1_EL1 = swapper_pg_dir                       ← 0xFFFF_xxxx_xxxx_xxxx
```

恒等映射（idmap）的物理地址属于低地址范围（如 `0x4020_xxxx`），bit[55]=0，
所以走 **TTBR0**。内核要使用恒等映射，**必须切换 TTBR0**。

---

## 八、运行时 TTBR 切换机制（idmap_pg_dir 的使用）

### 场景分类

运行时需要恒等映射的场景分为两大类：

#### 场景 A：MMU 关 → 开（CPU 断电后重新上电）

此时 TTBR0/TTBR1 寄存器值已丢失，不存在"切换"，而是**直接写入**两个 TTBR 后开 MMU。

**Secondary CPU 启动** — `arch/arm64/kernel/head.S:374-378`：
```asm
    bl      __cpu_setup              // 配置 MAIR/TCR/SCTLR
    adrp    x1, swapper_pg_dir       // x1 → TTBR1
    adrp    x2, idmap_pg_dir         // x2 → TTBR0
    bl      __enable_mmu             // 写入两个 TTBR，开 MMU
    ldr     x8, =__secondary_switched
    br      x8                       // 跳到内核虚拟地址
```

**CPU suspend/resume** — `arch/arm64/kernel/sleep.S:107-113`：
```asm
SYM_CODE_START(cpu_resume)
    bl      init_kernel_el
    bl      __cpu_setup              // CPU 断电后所有寄存器丢失，重新配置
    adrp    x1, swapper_pg_dir       // x1 → TTBR1
    adrp    x2, idmap_pg_dir         // x2 → TTBR0
    bl      __enable_mmu             // 写入两个 TTBR，开 MMU
    ldr     x8, =_cpu_resume
    br      x8                       // 跳到内核虚拟地址
```

执行时序：
```
① CPU 醒来，MMU 关，TTBR0/TTBR1 无意义
   PC = 物理地址

② __cpu_setup: 配置 TCR、MAIR，准备 SCTLR 值（MMU 仍关）

③ __enable_mmu:
   msr ttbr0_el1, x2     → TTBR0 = idmap_pg_dir
   msr ttbr1_el1, x1     → TTBR1 = swapper_pg_dir
   msr sctlr_el1, x0     → 开 MMU

   PC 仍是物理地址 → bit[55]=0 → TTBR0 → idmap_pg_dir (VA=PA) ✓

④ ldr x8, =_cpu_resume   → x8 = 内核虚拟地址 (0xFFFF...)
   br x8                  → bit[55]=1 → TTBR1 → swapper_pg_dir ✓
```

**关键**：这与首次启动 `__primary_switch` 的流程完全一样 — 从 MMU 关
→ 同时装入两个 TTBR → 开 MMU → 恒等映射中短暂执行 → 跳到内核虚拟地址。

#### 场景 B：MMU 已开，需要替换 TTBR1（在线切换内核页表）

当 MMU 已经开着，需要替换 TTBR1 指向的内核页表时，必须切换 TTBR0 到恒等映射，
在恒等映射中执行替换代码。因为：

- 当前代码运行在 TTBR1 映射的内核虚拟地址上
- 如果直接修改 TTBR1，下一条指令的页表映射可能消失 → fault

**安装恒等映射** — `arch/arm64/include/asm/mmu_context.h:112-120`：
```c
static inline void cpu_install_idmap(void)
{
    cpu_set_reserved_ttbr0();          // TTBR0 → reserved_pg_dir (安全中间态)
    local_flush_tlb_all();             // 清 TLB
    cpu_set_idmap_tcr_t0sz();          // 调整 TCR.T0SZ 适配恒等映射

    cpu_switch_mm(lm_alias(idmap_pg_dir), &init_mm);  // TTBR0 → idmap_pg_dir
}
```

**替换 TTBR1** — `arch/arm64/mm/proc.S:179-187`（在 `.idmap.text` 段中）：
```asm
SYM_TYPED_FUNC_START(idmap_cpu_replace_ttbr1)
    __idmap_cpu_set_reserved_ttbr1 x1, x3   // TTBR1 → reserved_pg_dir + 清 TLB
    offset_ttbr1 x0, x3
    msr     ttbr1_el1, x0                   // TTBR1 → 新页表
    isb
    ret
SYM_FUNC_END(idmap_cpu_replace_ttbr1)
```

**卸载恒等映射** — `arch/arm64/include/asm/mmu_context.h:100-110`：
```c
static inline void cpu_uninstall_idmap(void)
{
    cpu_set_reserved_ttbr0();          // TTBR0 → reserved_pg_dir
    local_flush_tlb_all();
    cpu_set_default_tcr_t0sz();        // 恢复 TCR.T0SZ
    if (mm != &init_mm && ...)
        cpu_switch_mm(mm->pgd, mm);    // TTBR0 → 用户页表
}
```

**完整时序**：
```
                    TTBR0                          TTBR1
                    ─────                          ─────
初始:               用户页表/reserved               swapper_pg_dir (旧)
                                                    ↑ 内核代码在此执行

① cpu_install_idmap()
   TTBR0 切换:      → reserved → idmap_pg_dir      swapper_pg_dir (旧)
                      ↑ 恒等映射可用

② 调用 idmap_cpu_replace_ttbr1 (.idmap.text 段, VA=PA)
   CPU 通过 TTBR0:   idmap_pg_dir                  reserved → 新页表
                      ↑ 代码在此执行，不受 TTBR1 影响

③ 返回，跳回内核虚拟地址 (0xFFFF..., bit[55]=1)
                      idmap_pg_dir                  swapper_pg_dir (新)
                                                    ↑ 代码回到此执行

④ cpu_uninstall_idmap()
   TTBR0 切回:       → reserved → 用户页表          swapper_pg_dir (新)
```

### 三种场景总结

| 场景 | MMU 状态 | 操作 | 代码位置 |
|------|---------|------|---------|
| **首次启动** | 关 → 开 | 直接写 TTBR0=init_idmap + TTBR1=reserved，开 MMU | `head.S:508-510` |
| **Secondary CPU / resume** | 关 → 开 | 直接写 TTBR0=idmap + TTBR1=swapper，开 MMU | `head.S:374-377` / `sleep.S:107-110` |
| **运行时切换 TTBR1** | 已开 | 先切 TTBR0→idmap，在恒等映射中改 TTBR1，再切回 | `proc.S:179-187` |

前两种**不需要"切换"TTBR**（MMU 关着，寄存器值已丢失，直接写入即可）。
只有第三种才是真正的"在 MMU 开着的情况下替换正在使用的 TTBR1"，需要 idmap 作为安全跳板。

---

## 九、`init_idmap_pg_dir` 退役 → `idmap_pg_dir` 接替的代码链

### 第一步：卸载 `init_idmap_pg_dir`

`setup_arch()` 中（`arch/arm64/kernel/setup.c:317-321`）：
```c
    /* TTBR0 is only used for the identity mapping at this stage.
     * Make it point to zero page to avoid speculatively fetching new entries. */
    cpu_uninstall_idmap();
```

此时 TTBR0 从 `init_idmap_pg_dir` → `reserved_pg_dir`（空页表），
`init_idmap_pg_dir` 不再被任何 TTBR 引用。

### 第二步：构建 `idmap_pg_dir`

`paging_init()` 中（`arch/arm64/mm/mmu.c:1350-1362`）：
```c
void __init paging_init(void)
{
    map_mem(swapper_pg_dir);     // 完善 swapper_pg_dir
    memblock_allow_resize();
    create_idmap();              // ← 构建运行时 idmap_pg_dir
    declare_kernel_vmas();
}
```

`create_idmap()`（`arch/arm64/mm/mmu.c:1325-1349`）：
```c
static u8 idmap_ptes[IDMAP_LEVELS - 1][PAGE_SIZE]
    __aligned(PAGE_SIZE) __ro_after_init;   // 静态子级页表，初始化后只读

static void __init create_idmap(void)
{
    phys_addr_t start = __pa_symbol(__idmap_text_start);
    phys_addr_t end   = __pa_symbol(__idmap_text_end);
    phys_addr_t ptep  = __pa_symbol(idmap_ptes);

    __pi_map_range(&ptep, start, end, start, PAGE_KERNEL_ROX,
                   IDMAP_ROOT_LEVEL, (pte_t *)idmap_pg_dir, false,
                   __phys_to_virt(ptep) - ptep);
}
```

### 时间线

```
setup_arch()
    │
    ├── [setup.c:321]  cpu_uninstall_idmap()
    │       TTBR0: init_idmap_pg_dir → reserved_pg_dir (空)
    │       ← init_idmap_pg_dir 正式退役
    │
    ├── ... (efi_init, arm64_memblock_init 等)
    │
    └── [setup.c:333]  paging_init()
            │
            ├── map_mem(swapper_pg_dir)    // 完善内核页表
            │
            └── create_idmap()             // [mmu.c:1325]
                    idmap_pg_dir ← 映射 .idmap.text (VA=PA, ROX)
                    ← idmap_pg_dir 正式就绪
```

---

## 十、fixmap 页表骨架

`setup_arch()` 最早期调用 `early_fixmap_init()`（`arch/arm64/mm/fixmap.c:104-113`），
在 `swapper_pg_dir` 中为 fixmap 区域搭建页表骨架：

```
swapper_pg_dir (init_mm.pgd)
    └── PGD[fixmap 条目]
         └── P4D → bm_pud[]          (静态 BSS)
                    └── PUD → bm_pmd[]      (静态 BSS)
                               ├── PMD[0] → bm_pte[0][]  (全零，等待 __set_fixmap 填入)
                               └── PMD[1] → bm_pte[1][]  (全零)
```

页表骨架用静态 BSS 数组（`bm_pud`、`bm_pmd`、`bm_pte`），无需内存分配器。
PTE 条目全零，后续 `__set_fixmap(idx, phys, prot)` 按需填入。

首个使用者：`setup_machine_fdt()` → `fixmap_remap_fdt()` — 通过 fixmap 映射 FDT 设备树。

---

## 十一、内核映像 PUD/PMD/PTE 的分配——两阶段机制

PGD 是固定的 512 个条目（4KB 页），由链接脚本静态分配。但 PGD 指向的 **子级页表
（PUD/PMD/PTE）** 是怎么来的？答案是分 **两个阶段**，机制完全不同。

### 阶段一：早期启动（init_pg_dir）—— 链接时预分配 + 指针递增

`init_pg_dir` 在链接脚本中预留了一段**连续空间**，大小由宏 `INIT_DIR_SIZE` 在
编译时精确计算（覆盖整个内核映像需要多少页表页）。

在此 vmlinux 中实测 = **13 页（52KB）**：

```
vmlinux.lds.S:337-339:
    __pi_init_pg_dir = .;       ← 0xffff_8000_8231_5000
    . += INIT_DIR_SIZE;         ← 13 × 4KB = 52KB
    __pi_init_pg_end = .;       ← 0xffff_8000_8232_2000
```

布局：
```
init_pg_dir 空间 (13 页连续, BSS 段)
┌──────────────┐ +0x0000  (第 1 页)
│  PGD root    │ ← 512 个条目，后来 memcpy 到 swapper_pg_dir
├──────────────┤ +0x1000  (第 2 页)
│  PUD         │ ← PGD 条目指向这里
├──────────────┤ +0x2000  (第 3 页)
│  PMD         │ ← PUD 条目指向这里
├──────────────┤ +0x3000
│  PMD         │ ← 不同 segment 可能需要多个 PMD 页
├──────────────┤
│    ...       │ ← 按 map_segment() 调用顺序线性排列
├──────────────┤ +0xC000  (第 13 页)
│  (最后一页)   │
└──────────────┘ +0xD000
```

**分配方法**：没有任何分配器，就是一个物理地址指针在连续空间里递增。

```c
// arch/arm64/kernel/pi/map_kernel.c:43
phys_addr_t pgdp = (phys_addr_t)init_pg_dir + PAGE_SIZE;  // 跳过 PGD，从第 2 页开始
map_segment(init_pg_dir, &pgdp, ...);  // 每次传入 &pgdp
map_segment(init_pg_dir, &pgdp, ...);  // pgdp 不断递增
```

在 `map_range()` 递归中，每需要一个新子表就从当前位置取一页：

```c
// arch/arm64/kernel/pi/map_range.c:62-65
if (pte_none(*tbl)) {
    *tbl = __pte(__phys_to_pte_val(*pte) | PMD_TYPE_TABLE);
    *pte += PTRS_PER_PTE * sizeof(pte_t);   // 指针前进 4KB → "分配"下一页
}
```

**INIT_DIR_SIZE** 的计算（`arch/arm64/include/asm/kernel-pgtable.h:57-61`）：

```c
#define EARLY_PAGES(lvls, vstart, vend, add) (1          /* PGD 根页 */
    + EARLY_LEVEL(3, ...)   /* PUD 表需要的页数 */
    + EARLY_LEVEL(2, ...)   /* PMD 表需要的页数 */
    + EARLY_LEVEL(1, ...))  /* PTE 表需要的页数 (block mapping 时为 0) */

#define INIT_DIR_SIZE  (PAGE_SIZE * (EARLY_PAGES(...) + EXTRA_PAGES))
```

编译器根据内核映像大小、页表级数、对齐要求，精确算出需要多少页。

**memcpy 后的隐含关系**：`memcpy(swapper_pg_dir, init_pg_dir, PAGE_SIZE)` 只拷贝
PGD 根页。PGD 描述符里的物理地址仍然指向 `init_pg_dir` 空间内的 PUD/PMD 页。
此时 `swapper_pg_dir` 的 PGD 是"自己的"，但子级页表是"借用" `init_pg_dir` 的。

### 阶段二：paging_init() —— memblock 动态分配

`paging_init()` → `map_mem()` 重建 `swapper_pg_dir` 的完整映射（覆盖全部物理内存的
linear map + 内核映像映射），此时用 **memblock 分配器** 为每一级页表动态分配新页：

```c
// arch/arm64/mm/mmu.c:115-125
static phys_addr_t __init early_pgtable_alloc(enum pgtable_type pgtable_type)
{
    phys = memblock_phys_alloc_range(PAGE_SIZE, PAGE_SIZE, 0,
                                     MEMBLOCK_ALLOC_NOLEAKTRACE);
    if (!phys)
        panic("Failed to allocate page table page\n");
    return phys;   // 返回物理地址
}
```

调用链：

```
paging_init()                                     [mmu.c:1351]
  └─ map_mem(swapper_pg_dir)                      [mmu.c:1071]
     └─ __create_pgd_mapping(..., early_pgtable_alloc, ...)
        └─ alloc_init_p4d()                       [mmu.c:395]
           ├─ pgtable_alloc(TABLE_P4D) → memblock 分配 1 页
           └─ alloc_init_pud()                    [mmu.c:332]
              ├─ pgtable_alloc(TABLE_PUD) → memblock 分配 1 页
              └─ alloc_init_cont_pmd()            [mmu.c:300]
                 ├─ pgtable_alloc(TABLE_PMD) → memblock 分配 1 页
                 └─ alloc_init_cont_pte()         [mmu.c:197]
                    └─ pgtable_alloc(TABLE_PTE) → memblock 分配 1 页
```

**巧妙之处：用 fixmap 临时窗口访问新分配的页表页**。

新分配的页表页可能还没有虚拟映射（linear map 正在创建中），
代码通过 `fixmap.h` 中的 `FIX_PTE`/`FIX_PMD`/`FIX_PUD`/`FIX_P4D`/`FIX_PGD`
五个专用槽位来临时映射它们：

```c
// alloc_init_cont_pte() 中 (mmu.c:210-215):
pte_phys = pgtable_alloc(TABLE_PTE);     // memblock 分配物理页
ptep = pte_set_fixmap(pte_phys);         // 通过 fixmap 映射为可访问的虚拟地址
init_clear_pgtable(ptep);                // 清零
ptep += pte_index(addr);
__pmd_populate(pmdp, pte_phys, pmdval);  // 把物理地址写入上级 PMD 描述符
```

这就是 **fixmap 中 `FIX_PTE`/`FIX_PMD`/`FIX_PUD` 槽位存在的原因** ——
它们不是给用户用的，而是给 `paging_init()` 在创建页表时临时映射新页表页用的。

### 阶段二完成后

`paging_init()` 完成后，`swapper_pg_dir` 的 **所有子级页表都由 memblock 新分配**，
不再引用 `init_pg_dir` 空间内的任何页。`init_pg_dir` 空间（BSS 段末尾）以及
`init_idmap_pg_dir` 空间（`.init.data` 段）后来都被 `free_initmem()` 释放为可用内存。

### 三种分配方式对比

| | fixmap 页表 | init_pg_dir (阶段一) | paging_init (阶段二) |
|---|---|---|---|
| **PGD** | linker script (`.rodata`) | linker script (BSS) | 复用 swapper_pg_dir |
| **PUD/PMD/PTE** | 静态 BSS 数组 (`bm_*`) | init_pg_dir 连续空间内指针递增 | memblock 动态分配 |
| **分配器** | 无（编译时） | 无（链接时预计算大小） | `memblock_phys_alloc_range()` |
| **位置** | 固定（BSS 段内） | 固定（BSS 段末尾连续 13 页） | 散布在物理内存各处 |
| **访问方式** | 直接用 kimg 虚拟地址 | 通过 va_offset 偏移计算 | 通过 fixmap 临时窗口 |
| **生命周期** | 永久 | 临时（paging_init 后废弃，free_initmem 释放） | 永久 |
| **为何如此** | 太早，无任何分配器 | 太早，无 memblock | memblock 已可用 |

### 时间线

```
阶段    swapper_pg_dir 子级页表来源        分配方式
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 ①  memcpy 后                             PUD/PMD 在 init_pg_dir 空间
                                           (指针递增，无分配器)

 ②  early_fixmap_init()                   fixmap 的 bm_pud/pmd/pte
                                           (BSS 静态数组)

 ③  paging_init → map_mem()               全部 PUD/PMD/PTE 由 memblock 新分配
                                           (通过 fixmap 临时窗口初始化)
                                           → init_pg_dir 空间中的旧表被废弃

 ④  free_initmem()                         init_pg_dir 空间归还为可用内存
```

---

## 十二、代码索引

| 文件 | 关键内容 |
|------|---------|
| `arch/arm64/kernel/vmlinux.lds.S:223-264,337-340` | 所有 PGD 的链接脚本分配 |
| `arch/arm64/kernel/head.S:85-100` | `primary_entry` → 创建 `init_idmap_pg_dir` |
| `arch/arm64/kernel/head.S:374-378` | `secondary_startup` → 加载 `idmap_pg_dir` + `swapper_pg_dir`，开 MMU |
| `arch/arm64/kernel/head.S:508-523` | `__primary_switch` → 加载 TTBR0/TTBR1，开启 MMU |
| `arch/arm64/kernel/sleep.S:107-113` | `cpu_resume` → 加载 `idmap_pg_dir` + `swapper_pg_dir`，开 MMU |
| `arch/arm64/kernel/pi/map_range.c` | `create_init_idmap()` + `map_range()` 递归建表 |
| `arch/arm64/kernel/pi/map_kernel.c:60-138` | `early_map_kernel()` → 构建 `init_pg_dir` → 拷贝到 `swapper_pg_dir` |
| `arch/arm64/mm/proc.S:160-188` | `__idmap_cpu_set_reserved_ttbr1` + `idmap_cpu_replace_ttbr1` |
| `arch/arm64/mm/mmu.c:1325-1362` | `create_idmap()` + `paging_init()` |
| `arch/arm64/mm/fixmap.c:104-113` | `early_fixmap_init()` — fixmap 页表骨架 |
| `arch/arm64/include/asm/mmu_context.h:100-120` | `cpu_uninstall_idmap()` / `cpu_install_idmap()` |
| `arch/arm64/include/asm/mmu_context.h:43-54` | `cpu_set_reserved_ttbr0()` |
| `arch/arm64/kernel/setup.c:287-333` | `setup_arch()` → fixmap → uninstall idmap → paging_init |
| `mm/init-mm.c:36` | `init_mm.pgd = swapper_pg_dir` |
| `arch/arm64/include/asm/kernel-pgtable.h` | PGD 大小宏定义 |
