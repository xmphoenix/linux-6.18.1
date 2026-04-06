# ARM64 head.S 内存初始化与映射全流程

> 基于 Linux 6.18.1，`arch/arm64/kernel/head.S`，4KB 页面 / 48 位 VA 配置

---

## 一、总览：从上电到 start_kernel 的内存相关事件

```
primary_entry
 │
 ├── record_mmu_state          ← 检测 MMU/Cache 状态
 ├── preserve_boot_args        ← 保存启动参数 + D-cache invalidate
 │
 ├── 设置临时栈 (early_init_stack)
 ├── __pi_create_init_idmap    ← 【1】创建恒等映射页表
 ├── dcache_inval/clean        ← 【2】页表缓存一致性处理
 │
 ├── init_kernel_el            ← EL2→EL1 降级（关MMU/Cache）
 │
 ├── __cpu_setup               ← 【3】配置 MAIR/TCR（MMU参数准备）
 │
 └── __primary_switch
      ├── __enable_mmu         ← 【4】装载 TTBR0/TTBR1 + 开 MMU
      ├── __pi_early_map_kernel← 【5】创建内核虚拟地址映射
      ├── br __primary_switched← 【6】跳入虚拟地址空间
      │
      └── __primary_switched
           ├── init_cpu_task    ← 切换到 init_task 正式栈
           ├── kimage_voffset   ← 【7】计算虚拟-物理偏移
           └── bl start_kernel  ← 进入 C 世界
```

---

## 二、入口状态

```
Bootloader 交接时:
  MMU  = OFF
  D-cache = OFF
  I-cache = ON 或 OFF
  x0 = FDT 物理地址
  x1-x3 = 0（按协议）
  PC = 内核加载的物理地址（如 0x4020_0000）
```

所有地址都是物理地址，CPU 直接用 PA 访问内存。

---

## 三、详细分析

### 【1】创建恒等映射页表（Identity Map）

```asm
adrp    x0, __pi_init_idmap_pg_dir   // 页表存储区物理地址
mov     x1, xzr                       // clrmask=0
bl      __pi_create_init_idmap
```

#### 调用的 C 函数：`create_init_idmap()`

```c
// arch/arm64/kernel/pi/map_range.c
map_range(&ptep, _stext, __initdata_begin, _stext, ROX, level=0, pg_dir, false, 0);
map_range(&ptep, __initdata_begin, _end, __initdata_begin, RW, level=0, pg_dir, false, 0);
```

#### 恒等映射的核心特征

- **VA = PA**：虚拟地址等于物理地址
- **`va_offset = 0`**：没有虚拟地址偏移
- **`_stext` 是物理地址**：声明为 `extern char _stext[]`，PI 代码用 `-fpie` 编译，`(u64)_stext` 通过 `adrp+add` 获得 PC 相对地址 → MMU 关闭时即物理地址
- 两段映射：代码段 ROX + 数据段 RW

#### 页表结构（48 位 VA，4KB 页面）

```
4 级页表: L0(PGD) → L1(PUD) → L2(PMD) → L3(PTE)
每级 512 个条目 × 8 字节 = 4KB = 一页

典型结果（内核在 0x4020_0000 ~ 0x40E0_0000）:
  PGD[0] → PUD 表（表描述符）
  PUD[1] → PMD 表（表描述符）
  PMD[1..56] = 2MB block 描述符（直接映射，无 L3）
```

#### map_range() 递归逻辑

| level | 行为 | 条目类型 |
|-------|------|---------|
| 0 (PGD) | 无条件递归 | 表描述符 (PMD_TYPE_TABLE) |
| 1 (PUD) | 无条件递归 | 表描述符 |
| 2 (PMD) | 2MB 对齐 → block 描述符；否则递归到 L3 | PMD_TYPE_SECT 或表描述符 |
| 3 (PTE) | 写 4KB page 描述符（终止） | PTE_TYPE_PAGE |

#### Bump Allocator

```c
phys_addr_t ptep = pg_dir + PAGE_SIZE;  // 从 PGD 后面开始分配
// 每次需要新页表页: *tbl = __pte(*pte | TABLE | UXN); *pte += 4096;
```

页表页从 `init_idmap_pg_dir` 区域连续分配，无需复杂内存管理。

---

### 【2】页表缓存一致性

```asm
cbnz    x19, 0f          // MMU 开着？跳到 clean 分支
dmb     sy                // 确保页表写入完成
dcache_inval_poc(pg_dir, ptep)  // invalidate 页表区域
b       1f

0:  dcache_clean_poc(__idmap_text_start, __idmap_text_end)  // clean 代码区
```

| 情况 | 操作 | 原因 |
|------|------|------|
| MMU 关 (x19=0) | **invalidate** 页表区域 | 写入走 NC，但缓存可能有推测预取的旧行 |
| MMU 开 (x19≠0) | **clean** `.idmap.text` 代码区 | 后续要关 MMU 执行代码，需写回主存 |

---

### 【3】__cpu_setup — MMU 参数准备

**此时 MMU 仍然关闭**，但提前配置好翻译规则：

#### MAIR_EL1 — 内存属性索引表

```
5 种内存类型:
  index 0 (MT_NORMAL):       0xFF — Write-Back 可缓存
  index 1 (MT_NORMAL_TAGGED): 0xFF — 带 MTE 标签（初始同普通）
  index 2 (MT_NORMAL_NC):    0x44 — Non-Cacheable
  index 3 (MT_DEVICE_nGnRnE): 0x00 — 设备内存（最严格）
  index 4 (MT_DEVICE_nGnRE):  0x04 — 设备内存（允许提前应答）
```

页表条目通过 `AttrIndx[2:0]` 选择使用哪种类型。

#### TCR_EL1 — 翻译控制寄存器

```
T0SZ = 16    → TTBR0 翻译 48 位 VA（用户空间/恒等映射）
T1SZ = 16    → TTBR1 翻译 48 位 VA（内核空间）
TG0/TG1      → 4KB 粒度
IRGN/ORGN    → PTW 缓存策略 = Write-Back Write-Allocate
SH0/SH1      → Inner Shareable（多核一致性）
IPS          → 从 CPU 读取物理地址位数
ASID16       → 16 位 ASID
HA           → 硬件 Access Flag 更新（如果 CPU 支持）
```

#### PIE — 权限间接扩展（ARMv8.9+，如果支持）

```
PIR_EL1   → EL1 权限索引映射
PIRE0_EL1 → EL0 权限索引映射
TCR2_EL1  → 使能 PIE
```

#### 返回值

```asm
mov_q   x0, INIT_SCTLR_EL1_MMU_ON  // SCTLR 目标值（M+C+I）
ret                                  // 返回，但不写入 SCTLR！
```

**MAIR 和 TCR 在 MMU 关闭时写入不起作用**，只是提前存好。SCTLR 值通过 x0 传给 `__enable_mmu`。

---

### 【4】__enable_mmu — 开启 MMU

#### 入口参数

```
x0 = SCTLR_EL1 值（M=1, C=1, I=1）
x1 = reserved_pg_dir（空页表，TTBR1 占位）
x2 = init_idmap_pg_dir（恒等映射页表）
```

#### 执行序列

```asm
① 检查 CPU 粒度支持          mrs x3, ID_AA64MMFR0_EL1
                               不支持 → 死循环

② 装载 TTBR0                 msr ttbr0_el1, x2  (恒等映射)

③ 装载 TTBR1                 msr ttbr1_el1, x1  (空页表占位)

④ 开启 MMU                   msr sctlr_el1, x0  ← M=1, MMU 开启！
                               isb
                               ic  iallu           ← 清 I-cache
                               dsb nsh
                               isb
```

#### 开 MMU 瞬间的地址翻译

```
msr sctlr_el1, x0  执行后:
  PC = 0x4020_xxxx（物理地址）
  bit[55] = 0 → 硬件选择 TTBR0
  TTBR0 = 恒等映射: VA 0x4020_xxxx → PA 0x4020_xxxx ✓
  下一条指令 (isb) 正常取到
```

**这就是恒等映射存在的全部意义**：让 MMU 开启后 PC 的物理地址值仍然能翻译成功。

#### 开 MMU 后的状态

```
TTBR0 = init_idmap_pg_dir → 恒等映射 ✅ (VA=PA)
TTBR1 = reserved_pg_dir  → 空页表 ❌ (0xffff... 地址不可用)
MMU = ON, D-cache = ON, I-cache = ON
```

---

### 【5】__pi_early_map_kernel — 创建内核虚拟地址映射

```asm
bl  __pi_early_map_kernel   // 在 MMU 开启 + 恒等映射下执行
```

这个函数（PI 代码）做了：

1. **创建内核页表**：将内核映射到 `0xffff_8000_xxxx_xxxx` 虚拟地址空间
2. **写入 TTBR1_EL1**：用真正的内核页表替换之前的空 `reserved_pg_dir`
3. 可能执行内核重定位（KASLR 相关）

执行完后：

```
TTBR0 = init_idmap_pg_dir → 恒等映射 ✅
TTBR1 = 内核页表          → 内核虚拟映射 ✅ (新建立的)
```

---

### 【6】从物理地址跳入虚拟地址

```asm
ldr     x8, =__primary_switched    // 从字面量池读虚拟地址常量
adrp    x0, KERNEL_START           // PC 相对 → 物理地址
br      x8                          // 跳转到 0xffff... → TTBR1 接管
```

#### 两种寻址方式的关键配合

| 指令 | 方式 | 得到 | 用途 |
|------|------|------|------|
| `ldr x8, =sym` | 字面量池常量 | **虚拟地址** 0xffff... | 跳转目标，触发 TTBR1 |
| `adrp x0, sym` | PC + offset | **物理地址** 0x4020... | 传参给 __primary_switched |

#### 跳转瞬间

```
br x8 之前:  PC = 0x4020_xxxx (物理)  → TTBR0 恒等映射
br x8 之后:  PC = 0xffff_8000... (虚拟) → TTBR1 内核映射

从此，CPU 永远停留在虚拟地址世界。
恒等映射完成历史使命，后续 TTBR0 将被用户进程页表替换。
```

---

### 【7】__primary_switched — 计算 kimage_voffset

```asm
adrp    x4, _text              // x4 = _text 的虚拟地址（MMU 开，adrp 给虚拟地址）
sub     x4, x4, x0            // x4 = 虚拟地址 - 物理地址
str_l   x4, kimage_voffset, x5 // 保存到全局变量
```

- `kimage_voffset` 是后续 `virt_to_phys()` / `phys_to_virt()` 的转换基准
- 典型值：`0xffff_8000_8000_0000 - 0x4020_0000 = 0xffff_7FFF_C000_0000`

---

## 四、页表寄存器状态变迁

```
时间线          TTBR0_EL1              TTBR1_EL1               MMU
─────────────────────────────────────────────────────────────────
primary_entry    (未写入)               (未写入)                OFF
  │
create_idmap     (未写入)               (未写入)                OFF
  │              页表写入 init_idmap_pg_dir（物理内存）
  │
__cpu_setup      (未写入)               (未写入)                OFF
  │              写 MAIR/TCR（不生效，等 MMU 开）
  │
__enable_mmu     init_idmap_pg_dir      reserved_pg_dir         ON !!
  │              恒等映射 ✅             空页表 ❌
  │
early_map_kernel init_idmap_pg_dir      内核页表                ON
  │              恒等映射 ✅             内核映射 ✅
  │
br x8            ─── 跳入 0xffff... ─── bit[55]=1 → TTBR1 ──→
  │
__primary_switched  (不再使用)           内核映射 ✅              ON
  │
start_kernel     后续被用户进程页表替换  swapper_pg_dir          ON
```

---

## 五、TTBR0 / TTBR1 双页表机制

ARM64 硬件根据虚拟地址的 bit[55] 自动选择页表：

```
┌──────────────────────────────────────────────────────────┐
│  虚拟地址空间 (48位)                                       │
│                                                          │
│  0xFFFF_FFFF_FFFF_FFFF ┐                                 │
│                        │ bit[55]=1 → TTBR1_EL1           │
│  0xFFFF_0000_0000_0000 ┘ 内核空间 (256TB)                 │
│                                                          │
│  ════════ 中间空洞 (无效地址) ════════                      │
│                                                          │
│  0x0000_FFFF_FFFF_FFFF ┐                                 │
│                        │ bit[55]=0 → TTBR0_EL1           │
│  0x0000_0000_0000_0000 ┘ 用户空间 / 恒等映射 (256TB)      │
└──────────────────────────────────────────────────────────┘
```

启动阶段的巧妙利用：
- 恒等映射的物理地址（如 `0x4020_0000`）bit[55]=0 → TTBR0
- 内核虚拟地址（`0xffff_8000_xxxx`）bit[55]=1 → TTBR1
- 两者**同时存在**，互不干扰

---

## 六、内存属性与页表描述符格式

### 表描述符（指向下级页表）

```
[63]  [62:61] [60]   [59:48] [47:12]           [11:2] [1:0]
 NS    AP      UXN    保留    下级表物理页帧号    保留    11
                                                       TABLE
```

### Block 描述符（L2, 2MB 直接映射）

```
[47:21]            [20:12] [11:10] [9:8] [7:6] [5]  [4:2]      [1:0]
物理块基地址(2MB)    0       SH     AP    AttrI  AF   保留        01
                                                                SECT
```

### Page 描述符（L3, 4KB）

```
[47:12]            [11:10] [9:8] [7:6] [5]  [4:2]      [1:0]
物理页帧号(4KB)      SH     AP    AttrI  AF   保留        11
                                                        PAGE
```

---

## 七、关键常量速查

| 常量 | 值 | 含义 |
|------|---|------|
| `PAGE_SHIFT` | 12 | 页面 4KB |
| `PTDESC_ORDER` | 3 | 描述符 8 字节 |
| `PTDESC_TABLE_SHIFT` | 9 | 每级解析 9 位 VA |
| `PTRS_PER_PTE` | 512 | 每级 512 个条目 |
| `IDMAP_VA_BITS` | 48 | 恒等映射 VA 位数 |
| `IDMAP_LEVELS` | 4 | 恒等映射页表级数 |
| `IDMAP_ROOT_LEVEL` | 0 | 从 PGD 开始 |
| `PMD_TYPE_TABLE` | 0b11 | 表描述符类型位 |
| `PMD_TYPE_SECT` | 0b01 | Block 描述符类型位 |
| `PTE_TYPE_PAGE` | 0b11 | Page 描述符类型位 |
| `PMD_TABLE_UXN` | bit[60] | 用户态不可执行 |

---

## 八、设计精髓总结

### 恒等映射是一座桥

```
         物理地址世界                    虚拟地址世界
        (MMU OFF)                      (MMU ON)
     ┌──────────────┐              ┌──────────────┐
     │ PC=0x4020... │              │ PC=0xffff... │
     │ 直接访问物理  │   恒等映射    │ TTBR1 翻译   │
     │ 内存          │ ═══桥═══▶   │ 内核页表      │
     └──────────────┘              └──────────────┘
      create_idmap                  early_map_kernel
      __cpu_setup                   __primary_switched
      __enable_mmu                  start_kernel
```

### ldr= 与 adrp 的配合

- **`adrp`**（PC 相对）：在物理地址世界获取物理地址，在虚拟地址世界获取虚拟地址
- **`ldr =`**（字面量池常量）：无论在哪个世界，都读取链接器写死的虚拟地址

两者配合实现了从物理世界到虚拟世界的跨越。

### 分步建表策略

1. **先建恒等映射**（MMU OFF 时，NC 写入） → 保证 MMU 开启瞬间能活
2. **开 MMU**，TTBR1 用空表占位 → 恒等映射接管执行
3. **MMU ON 下建内核映射**（缓存写入，更快） → TTBR1 生效
4. **跳到虚拟地址** → 恒等映射退役

这比一次性建好所有映射再开 MMU 更灵活——内核映射在 MMU 开启后创建，可以利用缓存加速，也能处理 KASLR 随机化。

### 为什么不一步到位？

恒等映射和内核映射的地址范围完全不同（`0x4020...` vs `0xffff...`），不可能用同一张页表同时覆盖。ARM64 的双 TTBR 机制完美解决了这个问题：TTBR0 管低地址（恒等映射），TTBR1 管高地址（内核映射），硬件自动切换。
