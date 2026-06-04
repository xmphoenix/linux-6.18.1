# Flash 存储器与 MTD/NAND 驱动框架深度分析指南

> 基于 Linux 6.18.1 内核源码分析，涵盖协议标准、硬件属性、代码框架、坏块管理、测试用例、适配指南及面试题

---

## 目录

<details>
<summary><a href="#1-flash-涉及的标准协议">1. Flash 涉及的标准协议</a></summary>

- [1.1 SPI 协议标准](#11-spi-协议标准)
- [1.2 JEDEC 标准](#12-jedec-标准)
- [1.3 ONFI (Open NAND Flash Interface)](#13-onfi-open-nand-flash-interface)
- [1.4 CFI (Common Flash Interface)](#14-cfi-common-flash-interface)
- [1.5 SPI NAND 标准命令集](#15-spi-nand-标准命令集)
- [1.6 SPI NOR 标准命令集](#16-spi-nor-标准命令集)

</details>

<details>
<summary><a href="#2-spi-flash-硬件属性分析">2. SPI Flash 硬件属性分析</a></summary>

- [2.1 存储组织结构](#21-存储组织结构)
- [2.2 内核中的内存组织数据结构](#22-内核中的内存组织数据结构)
- [2.3 Block Size（块大小）](#23-block-size块大小)
- [2.4 OTP (One-Time Programmable)](#24-otp-one-time-programmable)
- [2.5 ECC (Error Correction Code)](#25-ecc-error-correction-code)
- [2.6 Block 数据布局与坏块/有效块 Layout](#26-block-数据布局与坏块有效块-layout)
- [2.7 OOB 区域内容详细布局](#27-oob-区域内容详细布局)
- [2.8 坏块标记 (Bad Block Marker) 详解](#28-坏块标记-bad-block-marker-详解)
- [2.9 如何区分出厂坏块、首次烧写坏块和运行时坏块](#29-如何区分出厂坏块首次烧写坏块和运行时坏块)
- [2.10 SPI Flash 关键寄存器](#210-spi-flash-关键寄存器)
- [2.11 时钟频率：Datasheet 标称 vs SoC 实际提供](#211-时钟频率datasheet-标称-vs-soc-实际提供)
- [3.1 源码目录结构](#31-源码目录结构)
- [3.2 软件架构层次](#32-软件架构层次)
- [3.3 SPI NAND 核心操作流程](#33-spi-nand-核心操作流程)

</details>

<details>
<summary><a href="#4-nand-flash-与-mtd-驱动框架及数据结构">4. NAND Flash 与 MTD 驱动框架及数据结构</a></summary>

- [4.1 核心数据结构关系图](#41-核心数据结构关系图)
- [4.2 MTD 核心数据结构](#42-mtd-核心数据结构)
- [4.3 NAND 设备数据结构](#43-nand-设备数据结构)
- [4.4 ECC 引擎数据结构](#44-ecc-引擎数据结构)
- [4.5 SPI NAND 芯片描述结构](#45-spi-nand-芯片描述结构)

</details>

<details>
<summary><a href="#5-坏块处理机制与流程">5. 坏块处理机制与流程</a></summary>

- [5.1 坏块类型](#51-坏块类型)
- [5.2 BBT (Bad Block Table) 实现](#52-bbt-bad-block-table-实现)
- [5.3 坏块检测流程](#53-坏块检测流程)
- [5.4 坏块标记流程](#54-坏块标记流程)
- [5.5 擦除时的坏块保护](#55-擦除时的坏块保护)
- [5.6 BBT 惰性加载策略](#56-bbt-惰性加载策略)
- [5.7 坏块池 (Bad Block Reserve) 与 BMT (Bad Block Management Table)](#57-坏块池-bad-block-reserve-与-bmt-bad-block-management-table)

</details>

<details>
<summary><a href="#6-flash-经典测试-case">6. Flash 经典测试 Case</a></summary>

- [6.1 内核自带 MTD 测试模块](#61-内核自带-mtd-测试模块)
- [6.2 使用方法](#62-使用方法)
- [6.3 用户空间测试工具 (mtd-utils)](#63-用户空间测试工具-mtd-utils)
- [6.4 典型测试场景](#64-典型测试场景)

</details>

<details>
<summary><a href="#7-如何适配一个新的-flash">7. 如何适配一个新的 Flash</a></summary>

- [7.1 适配 SPI NAND 步骤](#71-适配-spi-nand-步骤)
- [7.2 适配检查清单](#72-适配检查清单)

</details>

<details>
<summary><a href="#8-qemu-上测试-flash-特性的-case-和驱动代码">8. QEMU 上测试 Flash 特性的 Case 和驱动代码</a></summary>

- [8.1 QEMU Flash 模拟支持](#81-qemu-flash-模拟支持)
- [8.2 QEMU 启动配置](#82-qemu-启动配置)
- [8.3 使用 mtd_nandsim 模拟 NAND Flash](#83-使用-mtd_nandsim-模拟-nand-flash)
- [8.4 QEMU + nandsim 测试脚本](#84-qemu--nandsim-测试脚本)
- [8.5 模拟坏块测试](#85-模拟坏块测试)

</details>

<details>
<summary><a href="#9-soc-bootrom-从-nand-flash-启动流程">9. SoC BootROM 从 NAND Flash 启动流程</a></summary>

- [9.1 上电启动全景](#91-上电启动全景)
- [9.2 BootROM 如何识别外部 Flash](#92-bootrom-如何识别外部-flash)
- [9.3 BootROM 如何读取 Flash 数据](#93-bootrom-如何读取-flash-数据)
- [9.4 BootROM 碰到坏块怎么处理](#94-bootrom-碰到坏块怎么处理)
- [9.5 BootROM vs Linux 内核 — 坏块处理对比](#95-bootrom-vs-linux-内核--坏块处理对比)
- [9.6 量产烧录时的坏块处理](#96-量产烧录时的坏块处理)
- [9.7 如果 Block 0 本身就是坏块怎么办](#97-如果-block-0-本身就是坏块怎么办)

</details>

<details>
<summary><a href="#10-面试问题与答案">10. 面试问题与答案</a></summary>

- [Q1: NOR Flash 和 NAND Flash 的区别是什么？](#q1-nor-flash-和-nand-flash-的区别是什么)
- [Q2: SPI NAND 的读取操作分为哪几步？为什么需要两步？](#q2-spi-nand-的读取操作分为哪几步为什么需要两步)
- [Q3: 什么是 ECC？SPI NAND 支持哪些 ECC 方式？](#q3-什么是-eccspi-nand-支持哪些-ecc-方式)
- [Q4: NAND Flash 为什么会有坏块？如何管理？](#q4-nand-flash-为什么会有坏块如何管理)
- [Q5: MTD 子系统的层次结构是怎样的？](#q5-mtd-子系统的层次结构是怎样的)
- [Q6: `mtd_info` 结构中 `erasesize` 和 `writesize` 分别代表什么？](#q6-mtd_info-结构中-erasesize-和-writesize-分别代表什么)
- [Q7: SPI Flash 支持的 Quad/Dual 模式有什么区别？命名含义是什么？](#q7-spi-flash-支持的-quaddual-模式有什么区别命名含义是什么)
- [Q8: 如何在 Linux 内核中为一款新的 SPI NAND Flash 添加支持？](#q8-如何在-linux-内核中为一款新的-spi-nand-flash-添加支持)
- [Q9: OOB (Out-Of-Band) 区域是做什么的？](#q9-oob-out-of-band-区域是做什么的)
- [Q10: 什么是磨损均衡 (Wear Leveling)？为什么 NAND Flash 需要它？](#q10-什么是磨损均衡-wear-leveling为什么-nand-flash-需要它)
- [Q11: SPI NAND 和 Raw NAND 的区别？](#q11-spi-nand-和-raw-nand-的区别)
- [Q12: 描述一下 `spinand_probe()` 函数的完整流程。](#q12-描述一下-spinand_probe-函数的完整流程)

</details>

<details>
<summary><a href="#附录-关键文件索引">附录: 关键文件索引</a></summary>

</details>

---

## 1. Flash 涉及的标准协议

### 1.1 SPI 协议标准

| 协议 | 说明 | 在内核中的体现 |
|------|------|----------------|
| **SPI (Serial Peripheral Interface)** | 基础串行通信协议，包含 MOSI/MISO/CLK/CS 四线 | `include/linux/spi/spi.h` |
| **Dual SPI** | 数据线扩展到 2 根（1S-1S-2S / 1S-2S-2S） | `SPINAND_PAGE_READ_FROM_CACHE_1S_1S_2S_OP` |
| **Quad SPI (QPI)** | 数据线扩展到 4 根（1S-1S-4S / 1S-4S-4S） | `SPINAND_PAGE_READ_FROM_CACHE_1S_4S_4S_OP` |
| **Octal SPI** | 数据线扩展到 8 根（1S-1S-8S / 1S-8S-8S） | `SPINAND_PAGE_READ_FROM_CACHE_1S_8S_8S_OP` |
| **DTR (Double Transfer Rate)** | 双倍传输速率，时钟上下沿均传输 | `SPINAND_PAGE_READ_FROM_CACHE_1S_1D_1D_OP` |

> **命名约定 xS-yS-zS**: x=命令线数, y=地址线数, z=数据线数。S=SDR(单沿), D=DTR(双沿)

### 1.2 JEDEC 标准

| 标准 | 说明 |
|------|------|
| **JEDEC JESD216 (SFDP)** | Serial Flash Discoverable Parameters，SPI NOR Flash 自描述参数标准 |
| **JEDEC JESD230 (ONFI)** | Open NAND Flash Interface，Raw NAND 标准接口规范 |
| **JEDEC ID** | 制造商 ID + 设备 ID 用于芯片识别 (`0x9f` READ_ID 命令) |

### 1.3 ONFI (Open NAND Flash Interface)

```
内核文件: include/linux/mtd/onfi.h
```
- 定义了 NAND Flash 的参数页格式
- 定义了时序模式（Timing Mode 0-5）
- 定义了标准命令集

### 1.4 CFI (Common Flash Interface)

```
内核文件: drivers/mtd/chips/cfi_util.c, cfi_probe.c
         include/linux/mtd/cfi.h
```
- 主要用于 NOR Flash（并行接口）
- 定义了 `cfi_cmdset_0001`（Intel/Sharp）, `cfi_cmdset_0002`（AMD/Spansion）, `cfi_cmdset_0020`（ST）

### 1.5 SPI NAND 标准命令集

```c
/* 来自 include/linux/mtd/spinand.h */
命令码 0xff  - RESET         (复位)
命令码 0x9f  - READ_ID       (读取ID)
命令码 0x0f  - GET_FEATURE   (读寄存器)
命令码 0x1f  - SET_FEATURE   (写寄存器)
命令码 0x06  - WRITE_ENABLE  (写使能)
命令码 0x04  - WRITE_DISABLE (写禁止)
命令码 0x13  - PAGE_READ     (页读取到缓存)
命令码 0x03  - READ_FROM_CACHE      (从缓存读数据)
命令码 0x0b  - READ_FROM_CACHE_FAST (快速读缓存)
命令码 0x3b  - READ_FROM_CACHE_x2   (双线读缓存)
命令码 0x6b  - READ_FROM_CACHE_x4   (四线读缓存)
命令码 0x02  - PROGRAM_LOAD         (加载数据到缓存)
命令码 0x10  - PROGRAM_EXECUTE      (执行编程)
命令码 0xd8  - BLOCK_ERASE          (块擦除)
```

### 1.6 SPI NOR 标准命令集

```c
/* 来自 include/linux/mtd/spi-nor.h */
SPINOR_OP_RDSR   0x05  - 读状态寄存器
SPINOR_OP_WREN   0x06  - 写使能
SPINOR_OP_READ   0x03  - 读数据 (低速)
SPINOR_OP_PP     0x02  - 页编程 (最多256字节)
SPINOR_OP_BE_4K  0x20  - 4KB 扇区擦除
SPINOR_OP_SE     0xd8  - 64KB 块擦除
SPINOR_OP_RDID   0x9f  - 读 JEDEC ID
SPINOR_OP_RDSFDP 0x5a  - 读 SFDP 参数
```

---

## 2. SPI Flash 硬件属性分析

### 2.1 存储组织结构

```
┌─────────────────────────────────────────────────┐
│                   SPI NAND Flash                 │
├─────────────────────────────────────────────────┤
│  Target (Die) 0                                  │
│  ├── LUN 0                                       │
│  │   ├── Plane 0                                 │
│  │   │   ├── Block 0                             │
│  │   │   │   ├── Page 0 [Data Area | OOB Area]   │
│  │   │   │   ├── Page 1 [Data Area | OOB Area]   │
│  │   │   │   └── ...                             │
│  │   │   ├── Block 1                             │
│  │   │   └── ...                                 │
│  │   └── Plane 1                                 │
│  └── LUN 1                                       │
└─────────────────────────────────────────────────┘
```

### 2.2 内核中的内存组织数据结构

```c
/* include/linux/mtd/nand.h */
struct nand_memory_organization {
    unsigned int bits_per_cell;              // 每个存储单元的位数: SLC=1, MLC=2, TLC=3
    unsigned int pagesize;                   // 页大小: 通常 2048/4096 字节
    unsigned int oobsize;                    // OOB(Out-Of-Band) 区大小: 通常 64/128/256 字节
    unsigned int pages_per_eraseblock;       // 每个擦除块的页数: 通常 64
    unsigned int eraseblocks_per_lun;        // 每个 LUN 的擦除块数
    unsigned int max_bad_eraseblocks_per_lun;// 每个 LUN 最大坏块数 (通常为总块数的2%)
    unsigned int planes_per_lun;             // 每个 LUN 的平面数: 通常 1 或 2
    unsigned int luns_per_target;            // 每个 Target 的 LUN 数
    unsigned int ntargets;                   // Target(Die) 总数
};
```

**典型 SPI NAND 配置示例** (来自 `gigadevice.c`):

```c
// GigaDevice GD5F1GQ4UExxG: 1Gbit SPI NAND
NAND_MEMORG(1,     // bits_per_cell (SLC)
            2048,  // pagesize (2KB)
            128,   // oobsize (128B)
            64,    // pages_per_eraseblock (64页/块, 即128KB/块)
            1024,  // eraseblocks_per_lun (1024块)
            20,    // max_bad_eraseblocks_per_lun
            1,     // planes_per_lun
            1,     // luns_per_target
            1)     // ntargets
```

### 2.3 Block Size（块大小）

| Flash 类型 | 典型块大小 | 说明 |
|-----------|-----------|------|
| SPI NOR | 4KB / 32KB / 64KB | 支持多种擦除粒度 |
| SPI NAND | 128KB (2KB×64页) | 最小擦除单位为一个 Block |
| Raw NAND | 128KB ~ 8MB | 大页/超大页 NAND 块更大 |

### 2.4 OTP (One-Time Programmable)

```c
/* include/linux/mtd/spinand.h */
// OTP 区域布局
struct spinand_otp_layout {
    unsigned int npages;      // OTP 区域页数
    unsigned int start_page;  // OTP 起始页
};

// 工厂 OTP 操作
struct spinand_fact_otp_ops {
    int (*info)(...);   // 获取 OTP 信息
    int (*read)(...);   // 读取 OTP 数据
};

// 用户 OTP 操作
struct spinand_user_otp_ops {
    int (*info)(...);   // 获取 OTP 信息
    int (*lock)(...);   // 锁定 OTP 区域
    int (*erase)(...);  // 擦除 OTP 区域
    int (*read)(...);   // 读取 OTP 数据
    int (*write)(...);  // 写入 OTP 数据
};
```

- **工厂 OTP**: 出厂时写入的数据（如唯一序列号），只能读取
- **用户 OTP**: 用户可一次性写入的区域，写入后可锁定防修改
- **控制寄存器**: `CFG_OTP_ENABLE` (BIT(6) of REG_CFG 0xb0) 使能 OTP 访问模式

### 2.5 ECC (Error Correction Code)

```c
/* ECC 引擎类型 - include/linux/mtd/nand.h */
enum nand_ecc_engine_type {
    NAND_ECC_ENGINE_TYPE_NONE,      // 无 ECC
    NAND_ECC_ENGINE_TYPE_SOFT,      // 软件 ECC (Hamming/BCH)
    NAND_ECC_ENGINE_TYPE_ON_HOST,   // 主控 ECC (SPI 控制器硬件 ECC)
    NAND_ECC_ENGINE_TYPE_ON_DIE,    // 片上 ECC (Flash 芯片内部 ECC)
};

/* ECC 算法 */
enum nand_ecc_algo {
    NAND_ECC_ALGO_HAMMING,  // Hamming 码: 纠1检2
    NAND_ECC_ALGO_BCH,      // BCH 码: 可纠正多个位翻转
    NAND_ECC_ALGO_RS,       // Reed-Solomon: 字节级纠错
};

/* ECC 需求描述 */
#define NAND_ECCREQ(str, stp) { .strength = (str), .step_size = (stp) }
// 例: NAND_ECCREQ(8, 512) 表示每512字节可纠正8位错误

/* ECC 状态寄存器 (SPI NAND) */
#define STATUS_ECC_NO_BITFLIPS   (0 << 4)  // 无位翻转
#define STATUS_ECC_HAS_BITFLIPS  (1 << 4)  // 有位翻转已纠正
#define STATUS_ECC_UNCOR_ERROR   (2 << 4)  // 不可纠正错误
```

**SPI NAND 片上 ECC 流程**:
```
读操作:
  1. spinand_ecc_enable(true)    → 使能片上 ECC
  2. PAGE_READ 命令              → 数据从阵列读到缓存, 芯片自动做 ECC
  3. 读取状态寄存器               → 获取 ECC 校验结果
  4. READ_FROM_CACHE             → 读出纠正后的数据

写操作:
  1. spinand_ecc_enable(true)    → 使能片上 ECC
  2. PROGRAM_LOAD                → 数据写入缓存, 芯片自动计算 ECC
  3. PROGRAM_EXECUTE             → 执行编程
```

### 2.6 Block 数据布局与坏块/有效块 Layout

#### 2.6.1 布局一：单 Plane 顺序布局 (Sequential)

```
┌───────────────────────────────────────────────────────────────────────┐
│                        NAND Flash 整体布局                            │
├───────────────────────────────────────────────────────────────────────┤
│  Block 0    [GOOD]  ┌─ Page 0:  [Data 2048B][OOB 64B/128B]          │
│                     ├─ Page 1:  [Data 2048B][OOB 64B/128B]          │
│                     ├─ ...                                           │
│                     └─ Page 63: [Data 2048B][OOB 64B/128B]          │
├───────────────────────────────────────────────────────────────────────┤
│  Block 1    [GOOD]  ┌─ Page 0 ~ Page 63                             │
├───────────────────────────────────────────────────────────────────────┤
│  Block 2    [BAD ✗] ┌─ Page 0:  OOB[0]=0x00 ← 坏块标记 (非0xFF)     │
│   (出厂坏块)        ├─ 整个块不可使用, 跳过                           │
├───────────────────────────────────────────────────────────────────────┤
│  Block 3    [GOOD]  ┌─ Page 0 ~ Page 63                             │
├───────────────────────────────────────────────────────────────────────┤
│  ...                                                                  │
├───────────────────────────────────────────────────────────────────────┤
│  Block 500  [BAD ✗] ┌─ 运行时产生的坏块 (擦除/编程失败)              │
│   (磨损坏块)        ├─ Page 0: OOB[0:1]=0x0000 ← 软件写入坏块标记    │
├───────────────────────────────────────────────────────────────────────┤
│  ...                                                                  │
├───────────────────────────────────────────────────────────────────────┤
│  Block 1022 [RSVD]  ┌─ BBT Mirror (坏块表镜像, Raw NAND 可选)        │
│  Block 1023 [RSVD]  ┌─ BBT Primary (坏块表主表, Raw NAND 可选)       │
└───────────────────────────────────────────────────────────────────────┘
```

> **注意**: SPI NAND 的 BBT 通常只存在于内存中（惰性加载），不写回 Flash；
> Raw NAND 可选将 BBT 存储在 Flash 最后几个好块中（`NAND_BBT_USE_FLASH`）。

#### 2.6.2 布局二：多 Plane 交错布局 (Multi-Plane Interleaved)

当 `planes_per_lun >= 2` 时，NAND 内部的 Block 按**奇偶交错**分布在不同 Plane 上，
两个 Plane 共享一套行地址但拥有**独立的页缓存 (Page Buffer)**，因此可以**并行操作**。

```
┌───────────────────────────────────────────────────────────────────────┐
│           Multi-Plane 交错布局 (planes_per_lun = 2)                   │
│                                                                       │
│   LUN 0                                                               │
│   ┌─────────────────────────┬─────────────────────────┐              │
│   │      Plane 0            │      Plane 1            │              │
│   │   (偶数 Block)          │   (奇数 Block)          │              │
│   ├─────────────────────────┼─────────────────────────┤              │
│   │  Block 0  [GOOD]        │  Block 1  [GOOD]        │              │
│   │  Block 2  [BAD ✗]       │  Block 3  [GOOD]        │              │
│   │  Block 4  [GOOD]        │  Block 5  [GOOD]        │              │
│   │  Block 6  [GOOD]        │  Block 7  [BAD ✗]       │              │
│   │  ...                    │  ...                    │              │
│   │  Block 2044 [GOOD]      │  Block 2045 [GOOD]      │              │
│   │  Block 2046 [RSVD BBT]  │  Block 2047 [RSVD BBT]  │              │
│   └─────────────────────────┴─────────────────────────┘              │
│       ↕ 共享行地址总线                                                │
│       ↕ 各自拥有独立 Page Buffer                                      │
│       ↕ 支持 Multi-Plane 并行读写擦除                                 │
└───────────────────────────────────────────────────────────────────────┘
```

**内核中 Plane 归属的计算** (来自 `include/linux/mtd/nand.h`):

```c
// eraseblock 号对 planes_per_lun 取模，决定该块属于哪个 Plane
pos->plane = pos->eraseblock % nand->memorg.planes_per_lun;

// 例: planes_per_lun = 2
//   Block 0 → Plane 0 (0 % 2 = 0)
//   Block 1 → Plane 1 (1 % 2 = 1)
//   Block 2 → Plane 0 (2 % 2 = 0)
//   Block 3 → Plane 1 (3 % 2 = 1)
```

**SPI NAND 中 Plane Select 的硬件实现** (来自 `drivers/mtd/nand/spi/core.c`):

```c
// 读操作: 通过列地址的高位传递 Plane 编号
if (spinand->flags & SPINAND_HAS_READ_PLANE_SELECT_BIT)
    column |= req->pos.plane << fls(nanddev_page_size(nand));
    //                           ↑ 在页大小 MSB 之上插入 plane bit

// 写操作: 同理
if (spinand->flags & SPINAND_HAS_PROG_PLANE_SELECT_BIT)
    column |= req->pos.plane << fls(nanddev_page_size(nand));

// dirmap 创建时也按 plane 偏移
info.offset = plane << fls(nand->memorg.pagesize);
```

**两种布局对比**:

| 特性 | 单 Plane 顺序布局 | 多 Plane 交错布局 |
|------|------------------|------------------|
| **planes_per_lun** | 1 | 2 (或更多) |
| **Block 分布** | 所有 Block 在同一 Plane | 偶数 Block→Plane 0, 奇数→Plane 1 |
| **Page Buffer** | 1 个共享 | 每 Plane 独立 (2个+) |
| **并行操作** | 不支持 | 两个 Plane 可同时读/写/擦除 |
| **吞吐量** | 1x | 理论 2x (Multi-Plane 命令) |
| **地址编码** | 列地址 = 页内偏移 | 列地址高位包含 Plane Select bit |
| **实际芯片举例** | GD5F1GQ4 (1Gbit, 1-plane) | Macronix MX35LF2GE4AB (2Gbit, 2-plane) |
| **内核标志** | 无 | `SPINAND_HAS_PROG_PLANE_SELECT_BIT`<br>`SPINAND_HAS_READ_PLANE_SELECT_BIT` |

**内核中 2-Plane 芯片的真实定义** (来自 `drivers/mtd/nand/spi/macronix.c`):

```c
// Macronix MX35LF2GE4AB: 2Gbit, 2-plane SPI NAND
SPINAND_INFO("MX35LF2GE4AB",
    SPINAND_ID(SPINAND_READID_METHOD_OPCODE_DUMMY, 0x22),
    NAND_MEMORG(1, 2048, 64, 64, 2048, 40, 2, 1, 1),
    //                                     ↑ planes_per_lun = 2
    NAND_ECCREQ(4, 512),
    ...,
    SPINAND_HAS_QE_BIT |
    SPINAND_HAS_PROG_PLANE_SELECT_BIT |   // ← 写时需要 Plane Select
    SPINAND_HAS_READ_PLANE_SELECT_BIT,    // ← 读时需要 Plane Select
    ...);

// 对比: 单 Plane 芯片
// GigaDevice GD5F1GQ4UExxG: 1Gbit, 1-plane
NAND_MEMORG(1, 2048, 128, 64, 1024, 20, 1, 1, 1),
//                                    ↑ planes_per_lun = 1
```

**交错布局的坏块处理要点**:
- 坏块管理**不区分 Plane**：BBT 按全局 eraseblock 编号管理
- 一个 Plane 中的坏块不影响另一个 Plane 中同行位置的块
- Multi-Plane 并行操作时，如果其中一个 Plane 的目标块是坏块，需回退到单 Plane 操作

#### 2.6.3 单个 Block 内 Page 分布

**硬件物理上**，每个 Page 在 Flash 阵列中始终是 Data + OOB 连续存储。但在**软件/控制器读写**时，
有两种不同的组织方式来处理一个 Page 内部 Data 和 OOB 的排列关系：

##### 布局 A：Data+OOB 连续排列 (标准布局，NAND_ECC_PLACEMENT_OOB)

```
每个 Page 内: Data 和 OOB 作为一个整体, Data 在前, OOB 紧随其后。
ECC 校验码集中存放在 OOB 区域。

单个 Page (2KB + 64B OOB):
┌──────────────────────────────────────────────────┐
│          Data (2048 Bytes)          │ OOB (64B)   │
│                                     │[BBM|Free|ECC]│
└──────────────────────────────────────────────────┘

一个 Block (64 Pages) 在 Flash 中的物理排列:
┌───────────────┬────────┐
│  Page 0 Data  │Page0 OOB│  ← (2048B + 64B)
├───────────────┼────────┤
│  Page 1 Data  │Page1 OOB│  ← (2048B + 64B)
├───────────────┼────────┤
│  Page 2 Data  │Page2 OOB│  ← (2048B + 64B)
├───────────────┼────────┤
│     ...       │  ...   │
├───────────────┼────────┤
│  Page 63 Data │Page63OOB│  ← (2048B + 64B)
└───────────────┴────────┘
总计: 64 × (2048 + 64) = 64 × 2112 = 135,168 Bytes
```

**特点**:
- **最常见的标准布局**，SPI NAND 和大多数 Raw NAND 使用此方式
- 软件先读/写 Data 区域，再读/写 OOB 区域
- ECC 码集中存放在 OOB 区，不占用 Data 区空间
- 内核枚举值: `NAND_ECC_PLACEMENT_OOB`

**内核标准读取函数** (`nand_read_page_raw`):
```c
// drivers/mtd/nand/raw/nand_base.c
int nand_read_page_raw(struct nand_chip *chip, uint8_t *buf,
                       int oob_required, int page)
{
    // 第一步: 读取 Data 区域 (writesize 字节)
    ret = nand_read_page_op(chip, page, 0, buf, mtd->writesize);

    // 第二步: 读取 OOB 区域 (oobsize 字节)
    if (oob_required)
        ret = nand_read_data_op(chip, chip->oob_poi, mtd->oobsize, ...);
}
```

**SPI NAND 写缓冲区组织** (`spinand_write_to_cache_op`):
```c
// drivers/mtd/nand/spi/core.c
// SPI NAND 内部缓冲区布局: [databuf (2048B)] [oobbuf (64B)]
// spinand->oobbuf = spinand->databuf + nanddev_page_size(nand);
// 写入时 PROGRAM_LOAD 一次性写入 pagesize + oobsize 字节

nbytes = nanddev_page_size(nand) + nanddev_per_page_oobsize(nand);
// databuf 在前, oobbuf 紧随其后, 连续传输
```

##### 布局 B：分离存储布局 2K+2K...+(64B+64B...)

```
一个 Block 内, 所有 Page 的 Data 区先集中连续排列,
然后所有 Page 的 OOB 区再集中连续排列。
数据区和 OOB 区在地址空间上是分离的。

一个 Block (64 Pages) 的存储排列:

         数据区 (Data Region)                    OOB 区 (Spare Region)
┌─────────────────────────────┐          ┌─────────────────────────┐
│  Page 0 Data   (2048B)      │          │  Page 0 OOB  (64B)      │
├─────────────────────────────┤          ├─────────────────────────┤
│  Page 1 Data   (2048B)      │          │  Page 1 OOB  (64B)      │
├─────────────────────────────┤          ├─────────────────────────┤
│  Page 2 Data   (2048B)      │          │  Page 2 OOB  (64B)      │
├─────────────────────────────┤          ├─────────────────────────┤
│       ...                   │          │       ...               │
├─────────────────────────────┤          ├─────────────────────────┤
│  Page 63 Data  (2048B)      │          │  Page 63 OOB (64B)      │
└─────────────────────────────┘          └─────────────────────────┘
  共 64 × 2048 = 131,072 Bytes            共 64 × 64 = 4,096 Bytes

地址排列 (线性地址视角):
┌────────────────────────────────────────────────────────────────────┐
│  [Page0 Data][Page1 Data][Page2 Data]...[Page63 Data]              │
│  ← ─ ─ ─ ─ ─ 数据区 (128KB) ─ ─ ─ ─ ─ →                          │
│  [Page0 OOB][Page1 OOB][Page2 OOB]...[Page63 OOB]                 │
│  ← ─ ─ ─ OOB 区 (4KB) ─ ─ ─ →                                    │
└────────────────────────────────────────────────────────────────────┘
总计: 131,072 + 4,096 = 135,168 Bytes (与布局 A 总量相同)
```

**特点**:
- 数据区集中存储：所有 Page 的 2KB Data 区先连续排列
- OOB 区集中存储：所有 Page 的 64B OOB 区再连续排列
- Data 和 OOB 在物理地址空间上**分离**，不再按 Page 交错
- OOB 统一存放在整个数据区之后

**地址计算**:
```
假设 Block 起始地址为 base_addr:

布局 B 中:
  Page N 的 Data 地址 = base_addr + N × page_size
  Page N 的 OOB 地址  = base_addr + pages_per_block × page_size + N × oob_size

例 (64 Pages, 2KB Page, 64B OOB):
  Page 5 Data = base_addr + 5 × 2048 = base_addr + 0x2800
  Page 5 OOB  = base_addr + 64 × 2048 + 5 × 64 = base_addr + 0x20140

对比 布局 A 中:
  Page N 的 Data 地址 = base_addr + N × (page_size + oob_size)
  Page N 的 OOB 地址  = base_addr + N × (page_size + oob_size) + page_size

例:
  Page 5 Data = base_addr + 5 × 2112 = base_addr + 0x2940
  Page 5 OOB  = base_addr + 5 × 2112 + 2048 = base_addr + 0x3140
```

**应用场景**:
- 部分 NOR Flash 或混合型 Flash 控制器采用数据/Spare 区分离编址
- 某些 SoC 的 NAND 控制器在硬件层面将 Data 和 Spare 映射到不同地址窗口
- 一些 Bootloader/固件对整块数据做批量处理时，在 RAM 缓冲区中重组为此布局
- 便于对数据区做连续 DMA 传输，OOB 单独处理

##### 两种布局对比

| 特性 | 布局 A: 连续存储 (2K+64B)×N | 布局 B: 分离存储 2K×N + 64B×N |
|------|---------------------------|-------------------------------|
| **排列方式** | 每个 Page 的 Data+OOB 连续 | 所有 Data 在前, 所有 OOB 在后 |
| **Page N Data 地址** | `base + N × (pagesize+oobsize)` | `base + N × pagesize` |
| **Page N OOB 地址** | `base + N × (pagesize+oobsize) + pagesize` | `base + pages × pagesize + N × oobsize` |
| **Data 区是否连续** | 否，被 OOB 打断 | 是，所有 Page Data 区连续 |
| **OOB 区是否连续** | 否，分散在每个 Page 后面 | 是，所有 Page OOB 区连续 |
| **随机读单 Page** | 一次寻址即可读 Data+OOB | 需两次寻址 (Data 区 + OOB 区) |
| **顺序读整块 Data** | 每隔 2048B 需跳过 64B OOB | 可直接连续读取 128KB |
| **DMA 友好度** | 一般，需 scatter-gather 跳过 OOB | 好，Data 区可做单次连续 DMA |
| **硬件实现** | **标准 NAND Flash 硬件布局** | 部分控制器/NOR Flash |
| **Linux 内核 MTD** | 标准路径，绝大多数驱动 | 需控制器驱动做地址转换 |

> **关键区分**: 绝大多数 NAND Flash 芯片的**硬件物理布局**都是布局 A —— 每个 Page 内
> Data 和 OOB 连续存储。布局 B 通常出现在特定的 Flash 控制器硬件抽象层或软件缓冲区
> 重组中，而非 NAND Flash 芯片本身的物理排列。
>
> Linux 内核中，SPI NAND (`spinand_write_to_cache_op`) 和 Raw NAND (`nand_read_page_raw`)
> 均按布局 A 组织数据。如果控制器硬件使用布局 B，则需要在控制器驱动中做地址转换。

#### 2.6.4 各厂商坏块标记检查位置

```
┌──────────────────────────────────────────────────────────────────────────┐
│  Block N (128KB = 64 Pages × 2KB)                                       │
├──────────────────────────────────────────────────────────────────────────┤
│  Page 0 (首页) ← 坏块标记检测位置之一 (NAND_BBM_FIRSTPAGE)              │
│  ┌────────────────────────────────┬──────────────────────────┐           │
│  │       Data Area (2048B)        │     OOB Area (64/128B)    │           │
│  │  用户数据 / 文件系统数据        │  BBM + Free + ECC         │           │
│  └────────────────────────────────┴──────────────────────────┘           │
│                                                                          │
│  Page 1 (第二页) ← 坏块标记检测位置之一 (NAND_BBM_SECONDPAGE)            │
│  ┌────────────────────────────────┬──────────────────────────┐           │
│  │       Data Area (2048B)        │     OOB Area (64/128B)    │           │
│  └────────────────────────────────┴──────────────────────────┘           │
│                                                                          │
│  Page 2 ~ Page 62                                                        │
│  ┌────────────────────────────────┬──────────────────────────┐           │
│  │       Data Area (2048B)        │     OOB Area (64/128B)    │           │
│  └────────────────────────────────┴──────────────────────────┘           │
│                                                                          │
│  Page 63 (末页) ← 坏块标记检测位置之一 (NAND_BBM_LASTPAGE)              │
│  ┌────────────────────────────────┬──────────────────────────┐           │
│  │       Data Area (2048B)        │     OOB Area (64/128B)    │           │
│  └────────────────────────────────┴──────────────────────────┘           │
└──────────────────────────────────────────────────────────────────────────┘
```

| 厂商 | 检查页位置 | 内核代码 |
|------|-----------|---------|
| **Samsung** (SLC) | 第1页, 第2页 | `NAND_BBM_FIRSTPAGE \| NAND_BBM_SECONDPAGE` |
| **Samsung** (MLC) | 最后一页 | `NAND_BBM_LASTPAGE` |
| **Micron** | 第1页 (部分+第2页) | `NAND_BBM_FIRSTPAGE` (+ `SECONDPAGE`) |
| **Hynix** (SLC) | 第1页, 第2页 | `NAND_BBM_FIRSTPAGE \| NAND_BBM_SECONDPAGE` |
| **Hynix** (MLC) | 最后一页 | `NAND_BBM_LASTPAGE` |
| **Toshiba/Kioxia** | 第1页, 第2页 | `NAND_BBM_FIRSTPAGE \| NAND_BBM_SECONDPAGE` |
| **Macronix** | 第1页, 第2页 | `NAND_BBM_FIRSTPAGE \| NAND_BBM_SECONDPAGE` |
| **ESMT** | 第1页, 第2页, 最后一页 | `FIRSTPAGE \| SECONDPAGE \| LASTPAGE` |
| **AMD/Spansion** | 第1页, 第2页, 最后一页 | `FIRSTPAGE \| SECONDPAGE \| LASTPAGE` |
| **SPI NAND** (通用) | **仅第1页** | `spinand_isbad()` 只读 page 0 OOB |

### 2.7 OOB 区域内容详细布局

#### 2.7.1 OOB 通用结构

```
OOB 区域 = BBM (坏块标记) + Free Area (用户/FS可用) + ECC (纠错码)
```

#### 2.7.2 SPI NAND 默认 OOB 布局 (无自定义 ECC)

来自 `spinand_noecc_ooblayout`（`drivers/mtd/nand/spi/core.c`）:

```
64字节 OOB (典型 2KB 页):
┌──────┬──────────────────────────────────────────────┐
│ 偏移  │  内容                                        │
├──────┼──────────────────────────────────────────────┤
│ 0-1  │  BBM (Bad Block Marker) — 坏块标记 (保留)     │
│ 2-63 │  Free Area (62字节) — 用户/文件系统可用        │
└──────┴──────────────────────────────────────────────┘
```

#### 2.7.3 Winbond W25M02GV OOB 布局 (4段式, 64字节)

来自 `w25m02gv_ooblayout`（`drivers/mtd/nand/spi/winbond.c`）:

```
64字节 OOB, 分为4个section (每section 16字节):

Section 0 (偏移 0-15):                    Section 1 (偏移 16-31):
┌─────┬────────────┬───────────────┐      ┌─────┬────────────┬───────────────┐
│ 0-1 │  2-7       │  8-15         │      │16-17│ 18-23      │ 24-31         │
│ BBM │  Free(6B)  │  ECC(8B)      │      │(空) │  Free(6B)  │  ECC(8B)      │
└─────┴────────────┴───────────────┘      └─────┴────────────┴───────────────┘

Section 2 (偏移 32-47):                   Section 3 (偏移 48-63):
┌─────┬────────────┬───────────────┐      ┌─────┬────────────┬───────────────┐
│32-33│ 34-39      │ 40-47         │      │48-49│ 50-55      │ 56-63         │
│(空) │  Free(6B)  │  ECC(8B)      │      │(空) │  Free(6B)  │  ECC(8B)      │
└─────┴────────────┴───────────────┘      └─────┴────────────┴───────────────┘

布局规则: 每section内 offset 0-1 保留, 2-7 Free, 8-15 ECC
总计: Free=4×6=24字节, ECC=4×8=32字节, 保留=4×2=8字节
```

#### 2.7.4 Winbond W25N01JW OOB 布局 (紧凑式, 64字节)

来自 `w25n01jw_ooblayout`（`drivers/mtd/nand/spi/winbond.c`）:

```
64字节 OOB, 分为4个section (每section 16字节):

Section 0:                                 Section 1:
┌─────┬─────────────┬──────────┐           ┌──────────────┬──────────┐
│ 0-1 │ 2-11        │ 12-15    │           │ 16-27        │ 28-31    │
│ BBM │ Free(10B)   │ ECC(4B)  │           │ Free(12B)    │ ECC(4B)  │
└─────┴─────────────┴──────────┘           └──────────────┴──────────┘

Section 2:                                 Section 3:
┌──────────────┬──────────┐                ┌──────────────┬──────────┐
│ 32-43        │ 44-47    │                │ 48-59        │ 60-63    │
│ Free(12B)    │ ECC(4B)  │                │ Free(12B)    │ ECC(4B)  │
└──────────────┴──────────┘                └──────────────┴──────────┘

注意: Section 0 的 Free 跳过前2字节 BBM (offset+=2, length-=2)
总计: Free=10+12+12+12=46字节, ECC=4×4=16字节, BBM=2字节
```

#### 2.7.5 GigaDevice GD5FXGQ4XA OOB 布局 (64字节)

来自 `gd5fxgq4xa_ooblayout`（`drivers/mtd/nand/spi/gigadevice.c`）:

```
64字节 OOB, 分为4个section (每section 16字节):

Section 0:                                 Section 1:
┌───┬─────────────┬──────────┐             ┌──────────────┬──────────┐
│ 0 │ 1-7         │ 8-15     │             │ 16-23        │ 24-31    │
│BBM│ Free(7B)    │ ECC(8B)  │             │ Free(8B)     │ ECC(8B)  │
└───┴─────────────┴──────────┘             └──────────────┴──────────┘

Section 2:                                 Section 3:
┌──────────────┬──────────┐                ┌──────────────┬──────────┐
│ 32-39        │ 40-47    │                │ 48-55        │ 56-63    │
│ Free(8B)     │ ECC(8B)  │                │ Free(8B)     │ ECC(8B)  │
└──────────────┴──────────┘                └──────────────┴──────────┘

注意: Section 0 只保留1字节 BBM (GD 特有)
总计: Free=7+8+8+8=31字节, ECC=4×8=32字节, BBM=1字节
```

#### 2.7.6 Toshiba TX58 OOB 布局 (128字节, 前后对半分)

来自 `tx58cxgxsxraix_ooblayout`（`drivers/mtd/nand/spi/toshiba.c`）:

```
128字节 OOB:
┌─────┬───────────────────────────────┬───────────────────────────────┐
│ 0-1 │  2-63                         │  64-127                       │
│ BBM │  Free Area (62字节)           │  ECC Area (64字节)            │
└─────┴───────────────────────────────┴───────────────────────────────┘

规则: oobsize/2 分界, 前半为 Free+BBM, 后半为 ECC
```

#### 2.7.7 OOB 布局总结对比

| 芯片 | OOB 大小 | BBM 字节 | Free 字节 | ECC 字节 | 分段 |
|------|---------|---------|----------|---------|------|
| 默认 (noecc) | 64 | 2 | 62 | 0 | 1段 |
| W25M02GV | 64 | 2 | 24 | 32 | 4段 |
| W25N01JW | 64 | 2 | 46 | 16 | 4段 |
| GD5FXGQ4XA | 64 | 1 | 31 | 32 | 4段 |
| Toshiba TX58 | 128 | 2 | 62 | 64 | 1段 |
| Alliance (128) | 128 | 2 | 70 | 56 | 1段 |
| Alliance (256) | 256 | 2 | 142 | 112 | 1段 |

### 2.8 坏块标记 (Bad Block Marker) 详解

#### 2.8.1 坏块标记在 OOB 中的位置

```c
/* include/linux/mtd/rawnand.h */
#define NAND_BBM_POS_SMALL    5   // 小页 NAND (512B): OOB 第5字节
#define NAND_BBM_POS_LARGE    0   // 大页 NAND (2KB+): OOB 第0字节
```

- **SPI NAND**: 固定检查 OOB 的 **byte[0] 和 byte[1]**，任一非 0xFF 即为坏块
- **Raw NAND 大页**: OOB 的 **byte[0]**（`chip->badblockpos`）
- **Raw NAND 小页**: OOB 的 **byte[5]**

#### 2.8.2 好块 vs 坏块 OOB 对比

```
好块 (Good Block) — Block 首页 OOB:
┌────────────────────────────────────────────────┐
│ FF FF FF FF FF FF FF FF FF FF FF FF FF FF ... │
│ ↑  ↑                                          │
│ BBM=0xFF,0xFF → 好块                          │
└────────────────────────────────────────────────┘

出厂坏块 (Factory Bad) — Block 首页 OOB:
┌────────────────────────────────────────────────┐
│ 00 00 FF FF FF FF FF FF FF FF FF FF FF FF ... │
│ ↑  ↑                                          │
│ BBM=0x00,0x00 → 出厂已标记为坏块               │
└────────────────────────────────────────────────┘

运行时标记坏块 (Worn Bad) — 软件写入:
┌────────────────────────────────────────────────┐
│ 00 00 FF FF FF FF FF FF FF FF FF FF FF FF ... │
│ ↑  ↑                                          │
│ BBM=0x00,0x00 → 由 spinand_markbad() 写入      │
└────────────────────────────────────────────────┘
```

### 2.9 如何区分出厂坏块、首次烧写坏块和运行时坏块

#### 2.9.1 三种坏块的产生场景

| 坏块类型 | 产生时机 | 原因 | 特征 |
|---------|---------|------|------|
| **出厂坏块 (Factory Bad)** | 芯片出厂前 | 制造工艺缺陷 | 出厂时 OOB 已标记非 0xFF |
| **首次烧写坏块** | 第一次编程/擦除 | 潜在缺陷在首次操作时暴露 | 擦除/编程命令返回失败状态 |
| **运行时坏块 (Worn Bad)** | 正常使用中 | 擦写磨损、电压/温度异常 | 擦除/编程失败或 ECC 不可纠正 |

#### 2.9.2 区分方法

**方法一：通过产生时机区分**

```
[上电] → 首次扫描所有块的 OOB byte[0:1]
         ↓
     非 0xFF ?
     ├── YES → 出厂坏块 (Factory Bad)
     │         此块从未被擦除过，标记由工厂写入
     │         内核标记: NAND_BBT_BLOCK_FACTORY_BAD
     └── NO  → 好块 (Good)
               │
               ↓ (运行中进行擦除/编程操作)
          操作失败?
          ├── 擦除失败 (STATUS_ERASE_FAILED, BIT(2)) → 运行时坏块
          ├── 编程失败 (STATUS_PROG_FAILED, BIT(3))  → 运行时坏块
          └── ECC 不可纠正 (-EBADMSG)                 → 运行时坏块
               │
               ↓ 调用 nanddev_markbad()
          写入 OOB[0:1]=0x0000 标记
          BBT 状态设为: NAND_BBT_BLOCK_WORN
```

**方法二：通过 BBT 状态码区分**

```c
/* include/linux/mtd/nand.h */
enum nand_bbt_block_status {
    NAND_BBT_BLOCK_STATUS_UNKNOWN = 0,  // 初始状态, 尚未检查
    NAND_BBT_BLOCK_GOOD = 1,           // 好块 (OOB 标记为 0xFF)
    NAND_BBT_BLOCK_WORN = 2,           // ★ 运行时坏块 (软件标记)
    NAND_BBT_BLOCK_RESERVED = 3,       // 保留块 (BBT 存储等)
    NAND_BBT_BLOCK_FACTORY_BAD = 4,    // ★ 出厂坏块 (硬件标记)
};
```

内核通过以下逻辑区分：

```c
/* drivers/mtd/nand/core.c — nanddev_isbad() */
bool nanddev_isbad(struct nand_device *nand, const struct nand_pos *pos)
{
    // 1. 查询 BBT 缓存
    status = nanddev_bbt_get_block_status(nand, entry);

    // 2. 如果状态未知，从硬件读取
    if (status == NAND_BBT_BLOCK_STATUS_UNKNOWN) {
        if (nand->ops->isbad(nand, pos))
            status = NAND_BBT_BLOCK_FACTORY_BAD;  // ← 首次检查发现的坏块视为出厂坏块
        else
            status = NAND_BBT_BLOCK_GOOD;
        nanddev_bbt_set_block_status(nand, entry, status);
    }

    // 3. WORN 或 FACTORY_BAD 都是坏块
    return (status == NAND_BBT_BLOCK_WORN ||
            status == NAND_BBT_BLOCK_FACTORY_BAD);
}

/* drivers/mtd/nand/core.c — nanddev_markbad() */
int nanddev_markbad(struct nand_device *nand, const struct nand_pos *pos)
{
    // 运行时调用 markbad 写入的坏块，标记为 WORN
    nanddev_bbt_set_block_status(nand, entry, NAND_BBT_BLOCK_WORN);
    // ...
}
```

**关键区分逻辑**:
- **`NAND_BBT_BLOCK_FACTORY_BAD`**: 惰性扫描时，通过 `isbad()` 读 OOB 发现的坏块 → 出厂坏块
- **`NAND_BBT_BLOCK_WORN`**: 通过 `markbad()` 主动标记的坏块 → 运行时坏块

#### 2.9.3 首次烧写坏块的识别

首次烧写坏块本质上是**运行时坏块的第一种表现**，但发生在量产烧写阶段:

```
首次烧写流程:
1. 擦除所有块
   └── 某些块擦除失败 → STATUS_ERASE_FAILED
       └── 标记为坏块 (WORN)
           └── 这些是"潜在缺陷块", 出厂时可能未被检测到

2. 逐块编程数据
   └── 某些块编程失败 → STATUS_PROG_FAILED
       └── 标记为坏块 (WORN)

3. 读回验证
   └── 某些块数据不一致 → ECC 不可纠正
       └── 标记为坏块 (WORN)
```

**注意**: 如果在擦除前先检查 OOB 标记, 可以区分:
- 擦除前 OOB[0] 已非 0xFF → 出厂坏块 (不应擦除)
- 擦除后操作失败 → 首次暴露的潜在坏块

#### 2.9.4 实际区分策略 (最佳实践)

```
┌─────────────────────────────────────────────────────────────────┐
│                     坏块区分决策树                               │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  上电 / 首次使用                                                 │
│   │                                                              │
│   ├─ 扫描所有块 OOB byte[0:1]                                   │
│   │   ├─ 非 0xFF → 【出厂坏块】(FACTORY_BAD)                    │
│   │   │   • 永远不要擦除这些块                                   │
│   │   │   • 记入 BBT, 标记永久跳过                               │
│   │   │                                                          │
│   │   └─ 全 0xFF → 好块, 可以使用                                │
│   │                                                              │
│   ├─ 执行擦除/编程操作                                           │
│   │   ├─ 擦除成功 + 编程成功 → 【好块】                          │
│   │   │                                                          │
│   │   ├─ 擦除失败 (status & BIT(2))                              │
│   │   │   └─ 【运行时坏块】(WORN)                                │
│   │   │       • 如果是首次操作 → 也叫"首次烧写坏块"              │
│   │   │       • 调用 markbad() 标记                               │
│   │   │                                                          │
│   │   ├─ 编程失败 (status & BIT(3))                              │
│   │   │   └─ 【运行时坏块】(WORN)                                │
│   │   │       • 调用 markbad() 标记                               │
│   │   │                                                          │
│   │   └─ ECC 不可纠正 (read 返回 -EBADMSG)                      │
│   │       └─ 【运行时坏块】(WORN)                                │
│   │           • 数据已不可恢复, 标记坏块                          │
│   │           • UBI 层会自动将数据搬移到其他块                    │
│   │                                                              │
│  正常运行中                                                      │
│   ├─ ECC 接近阈值 (bitflips >= bitflip_threshold)                │
│   │   └─ 返回 -EUCLEAN → UBI 主动搬移数据 (预防性)              │
│   │       • 此时块仍是好块, 但需要磨损均衡                       │
│   │                                                              │
│   └─ 擦写次数耗尽 → 变为坏块, 同上处理                          │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

#### 2.9.5 内核代码中的具体检查流程

**SPI NAND 坏块检测** (`drivers/mtd/nand/spi/core.c`):

```c
// SPI NAND: 只检查块的第一页, 读 OOB byte[0:1]
static bool spinand_isbad(struct nand_device *nand, const struct nand_pos *pos)
{
    u8 marker[2] = { };
    struct nand_page_io_req req = {
        .pos = *pos,              // pos->page 隐式为块的第一页
        .ooblen = sizeof(marker), // 读 2 字节
        .ooboffs = 0,             // OOB 偏移 0
        .oobbuf.in = marker,
        .mode = MTD_OPS_RAW,      // RAW 模式, 不做 ECC 校正
    };
    spinand_read_page(spinand, &req);

    // 只要 byte[0] 或 byte[1] 非 0xFF, 即为坏块
    if (marker[0] != 0xff || marker[1] != 0xff)
        return true;
    return false;
}
```

**Raw NAND 坏块检测** (`drivers/mtd/nand/raw/nand_base.c`):

```c
// Raw NAND: 根据厂商配置检查多个页 (首页/第二页/末页)
static int nand_block_bad(struct nand_chip *chip, loff_t ofs)
{
    first_page = (int)(ofs >> chip->page_shift) & chip->pagemask;
    page_offset = nand_bbm_get_next_page(chip, 0);

    while (page_offset >= 0) {
        // 逐个检查指定页的 OOB
        chip->ecc.read_oob(chip, first_page + page_offset);
        bad = chip->oob_poi[chip->badblockpos]; // 通常 byte[0]

        // 8位全检: 非 0xFF 即坏
        if (bad != 0xFF)
            return 1;  // 坏块

        // 获取下一个需要检查的页
        page_offset = nand_bbm_get_next_page(chip, page_offset + 1);
    }
    return 0;  // 好块
}
```

#### 2.9.6 总结对比表

| 维度 | 出厂坏块 | 首次烧写坏块 | 运行时坏块 |
|------|---------|------------|-----------|
| **产生阶段** | 制造测试阶段 | 量产烧写阶段 | 产品使用阶段 |
| **根本原因** | 明确的制造缺陷 | 潜在缺陷首次暴露 | 擦写磨损/环境异常 |
| **检测方式** | 读 OOB 标记 (不擦除) | 擦除/编程返回失败 | 擦除/编程返回失败或 ECC 失败 |
| **OOB 标记来源** | 工厂测试程序写入 | 烧写软件调用 markbad | 驱动调用 markbad |
| **BBT 状态** | `FACTORY_BAD` | `WORN` | `WORN` |
| **能否恢复** | 不能 | 不能 | 不能 |
| **应对策略** | 永久跳过, 不擦除 | 标记并跳过 | 标记并跳过, UBI 自动搬数据 |
| **典型比例** | ≤2% (1Gbit约20块) | 极少 (个位数) | 随使用时间增加 |

> **核心要点**: 从**硬件 OOB 标记本身**无法区分出厂坏块和运行时坏块（两者都是 OOB 非 0xFF）。
> 区分的关键在于**检测时机**：上电首次扫描发现的是出厂坏块，运行中 markbad 标记的是运行时坏块。
> 内核 BBT 通过 `FACTORY_BAD` vs `WORN` 两个不同状态码来记录这一区别。

### 2.10 SPI Flash 关键寄存器

```
┌──────────────────────────────────────────┐
│  寄存器地址    名称            功能        │
├──────────────────────────────────────────┤
│  0xA0     Block Lock     块保护锁定      │
│  0xB0     Configuration  配置寄存器      │
│           BIT(0) QUAD_EN  四线使能       │
│           BIT(4) ECC_EN   ECC使能        │
│           BIT(6) OTP_EN   OTP使能        │
│  0xC0     Status         状态寄存器      │
│           BIT(0) BUSY     忙标志         │
│           BIT(2) E_FAIL   擦除失败       │
│           BIT(3) P_FAIL   编程失败       │
│           BIT(5:4) ECC    ECC状态        │
└──────────────────────────────────────────┘
```

### 2.11 时钟频率：Datasheet 标称 vs SoC 实际提供

#### 2.11.1 三层频率约束关系

Flash Datasheet 上标的频率是芯片能支持的**最大时钟频率上限**，而实际运行的时钟频率由
**Flash 芯片、PCB 走线、SoC 控制器**三者中最低的那个决定：

```
                   频率确定流程 (取最小值)
┌─────────────────────────────────────────────────────────────┐
│                                                             │
│  ① Flash Datasheet 标称         ② SoC SPI 控制器能力       │
│     (芯片最大频率上限)              (硬件最大时钟频率)        │
│     例: W25N01GV max 104MHz        例: QSPI 最高 150MHz     │
│          ↓                              ↓                   │
│          │         ③ PCB/板级约束        │                   │
│          │         (走线长度/信号完整性)   │                   │
│          │         DTS: spi-max-frequency │                   │
│          │              ↓                │                   │
│          └──────→ min(①, ②, ③) ←────────┘                   │
│                       ↓                                      │
│               实际运行时钟频率                                 │
│          例: min(104, 150, 80) = 80 MHz                      │
└─────────────────────────────────────────────────────────────┘
```

**三层约束详解**:

| 层次 | 来源 | 含义 | 典型值 |
|------|------|------|-------|
| **① Flash 芯片** | Datasheet AC Timing 表 | 芯片硬件能承受的最高时钟频率 | 80~166 MHz |
| **② SoC 控制器** | SPI controller 硬件规格 | SoC 的 SPI/QSPI 控制器最大输出频率 | 50~200 MHz |
| **③ 板级/DTS** | 设备树 `spi-max-frequency` | 综合考虑 PCB 走线、信号完整性后的安全频率 | 通常保守取值 |

#### 2.11.2 Datasheet 频率的含义 — 不是一个值，而是一组值

Flash Datasheet 中的时钟频率**不是单一值**，而是**按操作模式和 dummy cycles 分级**的一组值。
不同的 SPI 模式（x1/x2/x4/x8, STR/DTR）和不同的 dummy cycle 数对应不同的最大频率：

**示例：Winbond W35N01JW Datasheet (内核 winbond.c 中的定义)**:
```c
/* drivers/mtd/nand/spi/winbond.c */

// Octal 模式读缓存操作 — 同一个命令, 不同 dummy cycles 对应不同最大频率:
static SPINAND_OP_VARIANTS(read_cache_octal_variants,
    //                                                        dummy  max_freq
    SPINAND_PAGE_READ_FROM_CACHE_1S_1D_8D_OP(0, 3, NULL, 0, 120MHz), // 8线DTR, 3 dummy→120MHz
    SPINAND_PAGE_READ_FROM_CACHE_1S_1D_8D_OP(0, 2, NULL, 0, 105MHz), // 8线DTR, 2 dummy→105MHz
    SPINAND_PAGE_READ_FROM_CACHE_1S_8S_8S_OP(0, 20, NULL, 0, 无额外频率上限),// 8线STR, 20 dummy→无额外频率上限
    SPINAND_PAGE_READ_FROM_CACHE_1S_8S_8S_OP(0, 16, NULL, 0, 162MHz),// 8线STR, 16 dummy→162MHz
    SPINAND_PAGE_READ_FROM_CACHE_1S_8S_8S_OP(0, 12, NULL, 0, 124MHz),// 8线STR, 12 dummy→124MHz
    SPINAND_PAGE_READ_FROM_CACHE_1S_8S_8S_OP(0, 8,  NULL, 0, 86MHz), // 8线STR, 8 dummy→86MHz
    SPINAND_PAGE_READ_FROM_CACHE_1S_1S_8S_OP(0, 2,  NULL, 0, 无额外频率上限),// 1-1-8 STR, 2 dummy→无额外频率上限
    SPINAND_PAGE_READ_FROM_CACHE_1S_1S_8S_OP(0, 1,  NULL, 0, 133MHz),// 1-1-8 STR, 1 dummy→133MHz
    SPINAND_PAGE_READ_FROM_CACHE_FAST_1S_1S_1S_OP(0, 1, NULL, 0, 0), // 单线Fast, 无额外频率上限
    SPINAND_PAGE_READ_FROM_CACHE_1S_1S_1S_OP(0, 1, NULL, 0, 0));     // 单线标准, 无额外频率上限

// Quad+DTR 模式:
static SPINAND_OP_VARIANTS(read_cache_dual_quad_dtr_variants,
    SPINAND_PAGE_READ_FROM_CACHE_1S_4D_4D_OP(0, 8, NULL, 0, 80MHz),  // 4线DTR→80MHz
    SPINAND_PAGE_READ_FROM_CACHE_1S_1D_4D_OP(0, 2, NULL, 0, 80MHz),  // 1-1-4 DTR→80MHz
    SPINAND_PAGE_READ_FROM_CACHE_1S_4S_4S_OP(0, 4, NULL, 0, 无额外频率上限), // 4线STR, 4 dummy→无额外频率上限
    SPINAND_PAGE_READ_FROM_CACHE_1S_4S_4S_OP(0, 2, NULL, 0, 104MHz), // 4线STR, 2 dummy→104MHz
    SPINAND_PAGE_READ_FROM_CACHE_1S_1S_4S_OP(0, 1, NULL, 0, 无额外频率上限), // 1-1-4 STR→无额外频率上限
    ...);
```

**这些名字到底是什么意思？**

`1S_1D_8D`、`1S_8S_8S`、`1S_1S_8S` 这类命名不是“不同功能的读函数”，而是**同一个 read-from-cache 逻辑操作的不同总线协议模板**。可以按下面的规则拆开理解：

- 第 1 段: 命令 (`cmd`) 阶段
- 第 2 段: 地址 + dummy 阶段
- 第 3 段: 数据 (`data`) 阶段

其中数字表示这一阶段使用几根 IO 线，字母表示传输方式：

- `1/2/4/8` = 1线 / 2线 / 4线 / 8线
- `S` = STR (Single Transfer Rate, 单沿传输)
- `D` = DTR (Double Transfer Rate, 双沿传输)

例如：

- `1S_1D_8D`: 命令阶段 1线 STR；地址+dummmy 阶段 1线 DTR；数据阶段 8线 DTR
- `1S_8S_8S`: 命令阶段 1线 STR；地址+dummy 阶段 8线 STR；数据阶段 8线 STR
- `1S_1S_8S`: 命令阶段 1线 STR；地址+dummy 阶段 1线 STR；数据阶段 8线 STR
- `FAST 1S_1S_1S`: 单线 Fast Read，仍然是 1-1-1，只是用了带 dummy 的 fast opcode
- `普通 1S_1S_1S`: 最保守的标准单线读

这里的“1线/8线”说的是**该阶段并行使用多少根 IO 线传输位流**，不是整个事务从头到尾都固定只用 1 根或 8 根线。很多 Flash 的命令阶段仍然要求单线发 opcode，而地址/数据阶段才切到 Quad/Octal。

**为什么同一个 read_cache 要准备这么多 variants？**

虽然芯片 datasheet 的命令集合是固定的，但“从 cache 读数据”这个逻辑动作，通常对应多种**合法的物理传输格式**：

- 芯片可能同时支持 1-1-1、1-1-8、1-8-8、1D-8D 等多种读法
- 同一个 opcode 在不同 dummy cycle 下，对应不同的最大时钟频率
- 控制器不一定支持所有总线宽度、DTR、dummy 格式
- 板级连线和设备树 `spi-max-frequency` 还会进一步约束可用模式

因此 `SPINAND_OP_VARIANTS(read_cache_octal_variants, ...)` 的作用不是“列出多个不同读函数”，而是告诉 SPI NAND core：

- 这颗芯片对 `read_cache` 支持哪些候选协议模板
- core 在 probe 时遍历这些模板
- 对每个模板调用 `spi_mem_adjust_op_freq()`、`spi_mem_supports_op()` 等检查
- 最后选出**当前控制器 + 当前板级约束下最快且可用**的一条，保存到 `spinand->op_templates.read_cache`

也就是说，variants 是“同一逻辑操作的候选协议集合”，不是“重复定义命令表”。

**这些配置的数据源头从哪里来？是不是硬编码？**

是，**源头通常来自芯片 datasheet，但在 Linux SPI NAND 驱动里大多是厂商驱动手工硬编码进去的**，而不是像 SPI NOR 的 SFDP 那样大范围自动发现。

具体分三层理解：

- **协议能力来源于 datasheet**：例如 opcode、支持的 1-4-4 / 1-8-8 / DDR 读模式、dummy cycles 与最高频率的对应关系、是否需要切换 Quad/Octal/DDR 模式寄存器
- **Linux 用统一记号表达这些能力**：datasheet 常写成 `1-4-4 Quad I/O Read`、`1-8-8 Octal I/O Read`、`Octal DDR Read`，Linux 则统一编码成 `1S_4S_4S`、`1S_8S_8S`、`1S_1D_8D` 这类 `spi-mem` 形式
- **厂商驱动把这些能力静态写进代码**：例如 `drivers/mtd/nand/spi/winbond.c` 里的 `read_cache_octal_variants`、`write_cache_octal_variants`、dummy/freq 对应关系、VCR IO mode 配置值，都是根据 datasheet 手工翻译后写进驱动的

所以更准确地说：

- **芯片支持什么模式**：通常由厂商驱动静态描述在代码里
- **当前平台最终启用哪一种模式**：由 SPI NAND core 在 probe 时结合控制器能力、DTS 频率、DTR/线宽支持情况动态选择

也就是说，Linux SPI NAND 的流程不是“芯片把所有高级模式完整自描述出来”，而是“驱动先提供一份候选协议菜单，core 再从中选出当前平台可用且最快的一项”。

**代码里到底按什么判断 `1S_4S_4S`、`1S_8S_8S`、`1S_1D_8D`？**

关键点是：**代码里没有一个独立的“mode 枚举值”去判断这些名字**。这些模式名本质上只是人类便于阅读的缩写，真正被 core 判断的是 `struct spi_mem_op` 里的字段组合：

- `op->cmd.buswidth`
- `op->addr.buswidth`
- `op->dummy.buswidth`
- `op->data.buswidth`
- `op->cmd.dtr / op->addr.dtr / op->dummy.dtr / op->data.dtr`
- `op->max_freq`

例如下面这几个宏展开后，真正差别就在这些字段上：

```c
/* include/linux/mtd/spinand.h */

#define SPINAND_PAGE_READ_FROM_CACHE_1S_8S_8S_OP(addr, ndummy, buf, len, freq) \
    SPI_MEM_OP(SPI_MEM_OP_CMD(0xcb, 1),        /* cmd.buswidth = 1 */       \
               SPI_MEM_OP_ADDR(2, addr, 8),    /* addr.buswidth = 8 */      \
               SPI_MEM_OP_DUMMY(ndummy, 8),    /* dummy.buswidth = 8 */     \
               SPI_MEM_OP_DATA_IN(len, buf, 8),/* data.buswidth = 8 */      \
               SPI_MEM_OP_MAX_FREQ(freq))      /* max_freq = freq */

#define SPINAND_PAGE_READ_FROM_CACHE_1S_1D_8D_OP(addr, ndummy, buf, len, freq) \
    SPI_MEM_OP(SPI_MEM_OP_CMD(0x9d, 1),            /* cmd.buswidth = 1 */   \
               SPI_MEM_DTR_OP_ADDR(2, addr, 1),    /* addr.buswidth = 1, dtr */ \
               SPI_MEM_DTR_OP_DUMMY(ndummy, 1),    /* dummy.buswidth = 1, dtr */\
               SPI_MEM_DTR_OP_DATA_IN(len, buf, 8),/* data.buswidth = 8, dtr */ \
               SPI_MEM_OP_MAX_FREQ(freq))
```

因此：

- `1S_8S_8S` 的实质是：`cmd=1线STR, addr=8线STR, dummy=8线STR, data=8线STR`
- `1S_1D_8D` 的实质是：`cmd=1线STR, addr=1线DTR, dummy=1线DTR, data=8线DTR`
- `1S_4S_4S` 的实质是：`cmd=1线STR, addr=4线STR, dummy=4线STR, data=4线STR`

也就是说，**mode 名字只是对 `buswidth + dtr + max_freq` 这组字段的口语化概括**。

**模式选择的完整调用链**

`SPI NAND` 在 probe 匹配到芯片后，会走下面这条调用链来决定最终采用哪种 `read_cache` 模式：

```c
/* drivers/mtd/nand/spi/core.c */

op = spinand_select_op_variant(spinand, info->op_variants.read_cache);
if (!op)
    return -ENOTSUPP;

spinand->op_templates.read_cache = op;
```

也就是说：厂商驱动先提供一组候选项，core 再从 `info->op_variants.read_cache` 里挑出一个最优的，保存到 `spinand->op_templates.read_cache`，后续真正读 page 时就用这个模板。

**第一层：SPI NAND core 遍历所有候选模式**

```c
/* drivers/mtd/nand/spi/core.c */

for (i = 0; i < variants->nops; i++) {
    struct spi_mem_op op = variants->ops[i];

    op.data.nbytes = nbytes;
    ret = spi_mem_adjust_op_size(spinand->spimem, &op);
    if (ret)
        break;

    spi_mem_adjust_op_freq(spinand->spimem, &op);

    if (!spi_mem_supports_op(spinand->spimem, &op))
        break;

    op_duration_ns += spi_mem_calc_op_duration(spinand->spimem, &op);
}
```

这一层做了三件事：

- 先看这条 op 的数据长度能不能被控制器接受
- 再看频率是否需要被降到 `DTS/控制器/芯片` 的共同上限
- 再问 `spi-mem`：控制器到底支不支持这组 `buswidth + dtr` 组合

如果支持，再估算理论传输时长，用来和其它候选模式比较。

**第二层：SPI-MEM 判断“这个模式支不支持”**

真正的支持性判断在 `spi_mem_supports_op()` 和 `spi_mem_default_supports_op()`：

```c
/* drivers/spi/spi-mem.c */

bool spi_mem_supports_op(struct spi_mem *mem, const struct spi_mem_op *op)
{
    spi_mem_adjust_op_freq(mem, (struct spi_mem_op *)op);

    if (spi_mem_check_op(op))
        return false;

    return spi_mem_internal_supports_op(mem, op);
}
```

默认判断规则里，最关键的是这几项：

```c
bool op_is_dtr =
    op->cmd.dtr || op->addr.dtr || op->dummy.dtr || op->data.dtr;

if (op_is_dtr) {
    if (!spi_mem_controller_is_capable(ctlr, dtr))
        return false;
}

if (op->max_freq && op->max_freq < mem->spi->controller->min_speed_hz)
    return false;

return spi_mem_check_buswidth(mem, op);
```

这意味着：

- 如果某个候选项是 `1S_1D_8D`，只要控制器不支持 `dtr`，这条模式就会被直接淘汰
- 如果某个候选项要求 8 线地址/数据，但控制器或板级连线不支持 8bit buswidth，这条模式也会被淘汰
- 如果某个候选项设置了更低的 `max_freq`，而控制器又不支持 per-op 限频，同样可能被淘汰

**第三层：在所有“支持的模式”中选最快的**

如果一个候选模式通过了支持性检查，还不会马上被选中；它还要和其它候选模式比较理论耗时。耗时计算在：

```c
/* drivers/spi/spi-mem.c */

ncycles += ((op->cmd.nbytes * 8) / op->cmd.buswidth) / (op->cmd.dtr ? 2 : 1);
ncycles += ((op->addr.nbytes * 8) / op->addr.buswidth) / (op->addr.dtr ? 2 : 1);

if (op->dummy.nbytes)
    ncycles += ((op->dummy.nbytes * 8) / op->dummy.buswidth) / (op->dummy.dtr ? 2 : 1);

ncycles += ((op->data.nbytes * 8) / op->data.buswidth) / (op->data.dtr ? 2 : 1);
```

也就是说，core 会把整个事务拆成四段来算：

- `cmd cycles`
- `addr cycles`
- `dummy cycles`
- `data cycles`

每一段的周期数都直接由 `buswidth` 和 `dtr` 决定：

- 线越宽，周期越少
- DTR 每拍传两次，周期进一步减少
- 但 dummy 越多，总空转周期也越多

所以：

- `1S_8S_8S` 往往比 `1S_1S_8S` 更快，因为地址和数据阶段都更宽
- `1S_1D_8D` 理论吞吐更高，但如果控制器不支持 DTR，直接淘汰
- `FAST 1S_1S_1S` 虽然慢，但兼容性好，经常作为保底回退项

最后，`spinand_select_op_variant()` 会把所有**既支持又最快**的候选项选出来：

```c
if (!nbytes && op_duration_ns < best_op_duration_ns) {
    best_op_duration_ns = op_duration_ns;
    best_variant = &variants->ops[i];
}
```

**一句话总结这段代码逻辑**

`1S_4S_4S`、`1S_8S_8S`、`1S_1D_8D` 的选择，不是靠一个模式枚举值，而是靠每个 `spi_mem_op` 里的 `cmd/addr/dummy/data` 四段 `buswidth + dtr + max_freq` 字段组合来判断：

- `spi_mem_supports_op()` 负责判断“能不能用”
- `spi_mem_calc_op_duration()` 负责估算“哪个更快”
- `spinand_select_op_variant()` 负责在候选列表里选出“当前平台可用且最快”的那一条

**关键理解**: `max_freq = 0` 表示无额外频率限制 (使用 SoC/DTS 中的全局限制)；
`max_freq = 104MHz` 表示此操作模式下芯片最高只能跑 104MHz。

**为什么 dummy cycles 越多可以跑更高频率？**
```
dummy cycles 的作用: 在发完地址之后、数据输出之前，
给 Flash 内部电路留出时间来准备数据。

更高的时钟频率 → 每个 cycle 更短 → Flash 内部准备时间不够
→ 需要更多的 dummy cycles 来补偿

权衡:
  多 dummy → 可用更高频率 → 数据传输更快
  多 dummy → dummy 本身浪费时间 → 开销增大

最优选择 = min(总传输时间), 内核自动计算
```

#### 2.11.3 内核中的频率协商机制

**第一步：DTS 设备树声明板级最大频率**
```dts
/* 设备树 (DTS) 中的 SPI Flash 节点 */
&spi0 {
    /* SoC SPI 控制器节点, 控制器本身的时钟由 SoC clock tree 提供 */
    /* 比如 SoC PLL → 分频器 → SPI 控制器时钟 */

    flash@0 {
        compatible = "spi-nand";
        reg = <0>;
        spi-max-frequency = <80000000>;  /* ← 板级最大频率: 80MHz */
        /* 这个值由硬件工程师根据 PCB 走线质量、
           Flash datasheet 和 SoC 能力综合确定 */
    };
};
```

**第二步：SPI 核心层解析 DTS 频率**
```c
/* drivers/spi/spi.c */
// 解析设备树, 设置 spi_device 的 max_speed_hz
if (!of_property_read_u32(nc, "spi-max-frequency", &value))
    spi->max_speed_hz = value;  // 例: 80000000 (80MHz)
```

**第三步：每次传输时取最小值**
```c
/* drivers/spi/spi.c - __spi_validate() */
// 每次 SPI 传输前, 确保速率不超过控制器上限
if (!xfer->speed_hz)
    xfer->speed_hz = spi->max_speed_hz;  // 默认用设备的最大速率

// 再 clamp 到控制器的硬件上限
if (ctlr->max_speed_hz && xfer->speed_hz > ctlr->max_speed_hz)
    xfer->speed_hz = ctlr->max_speed_hz;
```

**第四步：SPI MEM 层的 per-op 频率调整**
```c
/* drivers/spi/spi-mem.c */
void spi_mem_adjust_op_freq(struct spi_mem *mem, struct spi_mem_op *op)
{
    // op->max_freq: Flash 芯片驱动为此操作设置的最大频率 (来自 Datasheet)
    // mem->spi->max_speed_hz: DTS + 控制器 clamp 后的设备最大频率

    // 取两者中的较小值
    if (!op->max_freq || op->max_freq > mem->spi->max_speed_hz)
        op->max_freq = mem->spi->max_speed_hz;
}

// 最终: actual_freq = min(flash_op_max_freq, dts_max_freq, controller_max_freq)
```

**第五步：SPI NAND 自动选择最优操作模式**
```c
/* drivers/mtd/nand/spi/core.c */
static const struct spi_mem_op *
spinand_select_op_variant(struct spinand_device *spinand,
                          const struct spinand_op_variants *variants)
{
    // 遍历所有操作变体 (x1/x2/x4/x8, 不同 dummy cycles...)
    for (i = 0; i < variants->nops; i++) {
        struct spi_mem_op op = variants->ops[i];

        // 1. 调整频率: min(flash_op_limit, dts/ctlr_limit)
        spi_mem_adjust_op_freq(spinand->spimem, &op);

        // 2. 检查控制器是否支持此操作 (线宽/DTR 等)
        if (!spi_mem_supports_op(spinand->spimem, &op))
            break;

        // 3. 计算此模式下传输整页数据需要的时间 (纳秒)
        op_duration_ns += spi_mem_calc_op_duration(spinand->spimem, &op);

        // 4. 选择总时间最短的操作模式
        if (op_duration_ns < best_op_duration_ns) {
            best_op_duration_ns = op_duration_ns;
            best_variant = &variants->ops[i];
        }
    }
    return best_variant;  // 返回最快且控制器支持的操作模式
}
```

#### 2.11.4 完整频率协商流程图

```
               Flash Datasheet                    SoC 硬件
            ┌──────────────────┐           ┌──────────────────┐
            │ AC Timing Table  │           │ SPI Controller   │
            │                  │           │                  │
            │ 1-1-1 STR: 无限制│           │ max_speed_hz:    │
            │ 1-1-4 STR: 104MHz│           │   150MHz (HW)    │
            │ 1-4-4 STR: 无限制│           │                  │
            │ 1-4-4(2dummy):   │           └──────┬───────────┘
            │         104MHz   │                  │
            │ 1-1-4 DTR: 80MHz│                  │
            └────────┬─────────┘                  │
                     │                            │
     ┌───────────────┘                            │
     ▼                                            ▼
  内核驱动注册            DTS 设备树声明板级约束
  op_variants[]           spi-max-frequency = 80MHz
  每个 op 带 max_freq           │
     │                          │
     ▼                          ▼
  ┌─────────────────────────────────────────────────┐
  │         spi_mem_adjust_op_freq()                 │
  │  actual = min(op->max_freq, spi->max_speed_hz)  │
  │                                                  │
  │  例: 1-1-4 STR (104MHz limit):                   │
  │    actual = min(104MHz, 80MHz) = 80MHz           │
  │                                                  │
  │  例: 1-1-4 DTR (80MHz limit):                    │
  │    actual = min(80MHz, 80MHz) = 80MHz            │
  │                                                  │
  │  例: 1-1-1 STR (无限制):                          │
  │    actual = min(∞, 80MHz) = 80MHz                │
  └──────────────┬──────────────────────────────────┘
                 │
                 ▼
  ┌─────────────────────────────────────────────────┐
  │     spinand_select_op_variant()                  │
  │                                                  │
  │  对每种操作模式计算: 传输时间 = 总 cycles / freq  │
  │                                                  │
  │  1-1-4 STR @80MHz: (8+16+8+16384)/4/80M = 快    │
  │  1-1-1 STR @80MHz: (8+16+8+16384)/1/80M = 慢    │
  │  1-4-4 STR @80MHz: (8+8+16384)/4/80M  = 最快    │
  │                    (如果控制器支持)                 │
  │                                                  │
  │  → 选择传输时间最短的: 1-4-4 STR ← best_variant  │
  └──────────────────────────────────────────────────┘
```

#### 2.11.5 实际吞吐量计算

```
实际吞吐量 ≠ 时钟频率 × 线宽

完整的吞吐量计算:

                     page_size × 8
  吞吐量 = ─────────────────────────────────────────────
           cmd_cycles + addr_cycles + dummy_cycles + data_cycles
           ─────────────────────────────────────────────────────
                              actual_freq

  其中:
    cmd_cycles   = cmd_bytes × 8 / cmd_buswidth / (dtr ? 2 : 1)
    addr_cycles  = addr_bytes × 8 / addr_buswidth / (dtr ? 2 : 1)
    dummy_cycles = dummy_bytes × 8 / dummy_buswidth / (dtr ? 2 : 1)
    data_cycles  = data_bytes × 8 / data_buswidth / (dtr ? 2 : 1)

示例 (2KB page, 80MHz):
  ┌────────────────┬──────────┬──────────┬──────────┬─────────────┐
  │ 操作模式        │ 数据cycles│ 开销cycles│ 总cycles  │ 吞吐量(MB/s)│
  ├────────────────┼──────────┼──────────┼─────────────┼────────────┤
  │ 1-1-1 STR      │ 16384    │ 32       │ 16416    │ ~9.8       │
  │ 1-1-4 STR      │ 4096     │ 24       │ 4120     │ ~38.8      │
  │ 1-4-4 STR      │ 4096     │ 16       │ 4112     │ ~38.9      │
  │ 1-1-4 DTR      │ 2048     │ 16       │ 2064     │ ~77.5      │
  │ 1-4-4 DTR      │ 2048     │ 12       │ 2060     │ ~77.7      │
  └────────────────┴──────────┴──────────┴─────────────┴────────────┘
  (以上未计入 PAGE_READ 命令本身的执行时间 tRD ≈ 25~100μs)
```

#### 2.11.6 常见问题与调试

**Q1: DTS 中 spi-max-frequency 应该填什么值？**
```
答: 取 min(Flash Datasheet 最大频率, SoC 控制器最大频率) 再适当降档。
    通常由硬件工程师根据 PCB 信号完整性测试结果给出。
    如果不确定, 先填 Datasheet 标称值的 80% 左右, 然后通过测试验证。

    例: Flash 支持 133MHz, SoC 控制器支持 150MHz
        → DTS 可填 104000000 (104MHz) 或更保守的 80000000 (80MHz)
```

**Q2: 为什么实际测到的速率比理论值低很多？**
```
原因:
  1. tRD (page read time): Flash 内部读取到缓存需要 25~100μs
     这段时间 SPI 总线空闲, 不传数据
  2. 软件开销: 中断处理、DMA 设置、锁竞争等
  3. 控制器实际分频: SoC 时钟树不一定能精确分频到目标频率
     例: PLL=600MHz, 目标 80MHz → 实际 600/8=75MHz 或 600/7≈85.7MHz
  4. CS 切换延迟: 每次操作的 CS setup/hold/inactive 时间
```

**Q3: 如何查看当前实际运行频率？**
```bash
# 方法1: 查看 SPI 设备信息
cat /sys/bus/spi/devices/spi0.0/modalias
cat /sys/bus/spi/devices/spi0.0/statistics/spi_sync

# 方法2: 在内核 SPI 传输路径加 trace
echo 1 > /sys/kernel/debug/tracing/events/spi/spi_transfer_start/enable
cat /sys/kernel/debug/tracing/trace_pipe
# 输出中 speed_hz 字段即为实际请求频率

# 方法3: 示波器/逻辑分析仪直接测量 SCK 引脚
```

---

### 3.1 源码目录结构

```
drivers/mtd/
├── mtdcore.c           # MTD 核心层: 设备注册/管理
├── mtdchar.c           # MTD 字符设备接口 (/dev/mtdN)
├── mtdblock.c          # MTD 块设备接口 (/dev/mtdblockN)
├── mtdpart.c           # MTD 分区管理
├── mtdconcat.c         # MTD 设备拼接
├── nand/
│   ├── core.c          # NAND 通用核心层 (坏块检测/擦除/ECC引擎)
│   ├── bbt.c           # BBT (Bad Block Table) 管理
│   ├── ecc.c           # ECC 引擎框架
│   ├── ecc-sw-hamming.c # 软件 Hamming ECC
│   ├── ecc-sw-bch.c    # 软件 BCH ECC
│   ├── spi/
│   │   ├── core.c           # ★ SPI NAND 核心驱动
│   │   ├── otp.c            # SPI NAND OTP 支持
│   │   ├── gigadevice.c     # GigaDevice 厂商驱动
│   │   ├── winbond.c        # Winbond 厂商驱动
│   │   ├── micron.c         # Micron 厂商驱动
│   │   ├── macronix.c       # Macronix 厂商驱动
│   │   ├── toshiba.c        # Toshiba/Kioxia 厂商驱动
│   │   ├── esmt.c           # ESMT 厂商驱动
│   │   ├── fmsh.c           # FMSH 厂商驱动
│   │   ├── xtx.c            # XTX 厂商驱动
│   │   └── ...              # 更多厂商
│   └── raw/            # Raw NAND 控制器驱动
├── spi-nor/
│   ├── core.c          # SPI NOR 核心驱动
│   ├── sfdp.c          # SFDP 参数解析
│   └── <厂商>.c        # 各厂商 NOR Flash 驱动
├── chips/              # 并行 NOR/CFI Flash
├── tests/              # MTD 测试模块
└── ubi/                # UBI (Unsorted Block Images) 层
```

### 3.2 软件架构层次

```
┌─────────────────────────────────────────────┐
│           用户空间 (User Space)               │
│    /dev/mtdN  /dev/mtdblockN  /dev/ubiN     │
├─────────────────────────────────────────────┤
│         MTD 核心层 (mtdcore.c)               │
│    设备注册 | 分区管理 | 字符/块设备接口       │
├─────────────────────────────────────────────┤
│         NAND 通用核心 (nand/core.c)          │
│    坏块管理 | ECC引擎 | 擦除流程 | BBT        │
├─────────────────────────────────────────────┤
│      SPI NAND 核心 (nand/spi/core.c)        │
│    probe/detect | read/write/erase           │
│    OOB管理 | dirmap | 连续读                  │
├─────────────────────────────────────────────┤
│     厂商驱动 (gigadevice.c, winbond.c...)    │
│    芯片信息表 | 特定ECC处理 | 初始化           │
├─────────────────────────────────────────────┤
│        SPI MEM 层 (spi-mem.h)               │
│    spi_mem_exec_op() | spi_mem_dirmap_xxx()  │
├─────────────────────────────────────────────┤
│        SPI 控制器驱动 (SPI Master)           │
│    硬件寄存器操作 | DMA传输                    │
└─────────────────────────────────────────────┘
```

### 3.3 SPI NAND 核心操作流程

#### 3.3.1 Probe 流程

```c
spinand_probe(struct spi_mem *mem)
  ├── devm_kzalloc(spinand)              // 分配 spinand_device
  ├── spinand_init(spinand)
  │   ├── kzalloc(scratchbuf)            // DMA 安全的临时缓冲区
  │   ├── spinand_detect(spinand)
  │   │   ├── spinand_reset_op()         // 发送 0xFF RESET 命令
  │   │   └── spinand_id_detect()        // 读取 ID 并匹配芯片表
  │   │       ├── spinand_read_id_op()   // READ_ID 命令 (0x9f)
  │   │       └── spinand_manufacturer_match()  // 在各厂商表中查找
  │   ├── kzalloc(databuf + oobbuf)      // 页数据+OOB 缓冲区
  │   ├── spinand_init_cfg_cache()       // 初始化配置寄存器缓存
  │   ├── spinand_init_flash()
  │   │   ├── spinand_read_cfg()         // 读取各 die 的 CFG 寄存器
  │   │   ├── spinand_init_quad_enable() // 使能 Quad 模式 (如需)
  │   │   ├── spinand_upd_cfg(OTP_EN=0)  // 关闭 OTP 模式
  │   │   ├── spinand_manufacturer_init()// 厂商特定初始化
  │   │   └── spinand_lock_block(UNLOCKED)// 解锁所有块
  │   ├── nanddev_init()                 // 初始化通用 NAND 设备
  │   ├── nanddev_ecc_engine_init()      // 初始化 ECC 引擎
  │   ├── spinand_cont_read_init()       // 初始化连续读
  │   ├── 注册 MTD 回调函数:
  │   │   mtd->_read_oob = spinand_mtd_read
  │   │   mtd->_write_oob = spinand_mtd_write
  │   │   mtd->_erase = spinand_mtd_erase
  │   │   mtd->_block_isbad = spinand_mtd_block_isbad
  │   │   mtd->_block_markbad = spinand_mtd_block_markbad
  │   └── spinand_create_dirmaps()       // 创建 SPI 直接映射描述符
  └── mtd_device_register(mtd)           // 注册到 MTD 子系统
```

#### 3.3.2 Read 流程

```c
spinand_mtd_read(mtd, from, ops)
  ├── mutex_lock(&spinand->lock)
  ├── spinand_use_cont_read() ?          // 判断是否使用连续读
  │   ├── YES → spinand_mtd_continuous_page_read()
  │   └── NO  → spinand_mtd_regular_page_read()
  │             └── nanddev_io_for_each_page()  // 遍历每一页
  │                 ├── spinand_select_target()  // 选择目标 die
  │                 └── spinand_read_page()
  │                     ├── nand_ecc_prepare_io_req()  // ECC 准备
  │                     ├── spinand_load_page_op()     // PAGE_READ(0x13)
  │                     ├── spinand_wait()             // 等待就绪
  │                     ├── spinand_read_from_cache_op()// READ_CACHE
  │                     │   └── spi_mem_dirmap_read()  // 通过 dirmap 读
  │                     └── nand_ecc_finish_io_req()   // ECC 校验
  └── mutex_unlock(&spinand->lock)
```

#### 3.3.3 Write 流程

```c
spinand_mtd_write(mtd, to, ops)
  ├── mutex_lock(&spinand->lock)
  ├── nanddev_io_for_each_page()         // 遍历每一页
  │   ├── spinand_select_target()
  │   └── spinand_write_page()
  │       ├── nand_ecc_prepare_io_req()  // ECC 准备 (启用/禁用片上ECC)
  │       ├── spinand_write_enable_op()  // WRITE_ENABLE (0x06)
  │       ├── spinand_write_to_cache_op()// PROGRAM_LOAD (0x02)
  │       │   ├── memset(databuf, 0xff)  // 清空数据缓冲区
  │       │   ├── memcpy(data + oob)     // 复制用户数据和OOB
  │       │   └── spi_mem_dirmap_write() // 通过 dirmap 写缓存
  │       ├── spinand_program_op()       // PROGRAM_EXECUTE (0x10)
  │       ├── spinand_wait()             // 等待编程完成
  │       │   └── 检查 STATUS_PROG_FAILED
  │       └── nand_ecc_finish_io_req()
  └── mutex_unlock(&spinand->lock)
```

#### 3.3.4 Erase 流程

```c
spinand_mtd_erase(mtd, einfo)
  ├── mutex_lock(&spinand->lock)
  └── nanddev_mtd_erase(mtd, einfo)      // 通用 NAND 擦除
      └── nanddev_erase()                 // 对每个块:
          ├── nanddev_isbad() → 跳过坏块
          └── spinand_erase()             // SPI NAND 擦除实现
              ├── spinand_select_target()
              ├── spinand_write_enable_op() // WRITE_ENABLE
              ├── spinand_erase_op()        // BLOCK_ERASE (0xd8)
              └── spinand_wait()            // 等待完成
                  └── 检查 STATUS_ERASE_FAILED
```

---

## 4. NAND Flash 与 MTD 驱动框架及数据结构

### 4.1 核心数据结构关系图

```
┌──────────────────────────────────────────────────────────────┐
│                     struct spinand_device                      │
│  ┌──────────────────────────────────────────────┐             │
│  │           struct nand_device (base)           │             │
│  │  ┌────────────────────────────────┐          │             │
│  │  │    struct mtd_info (mtd)       │          │             │
│  │  │    - size, erasesize           │          │             │
│  │  │    - writesize, oobsize        │          │             │
│  │  │    - _read_oob(), _write_oob() │          │             │
│  │  │    - _erase(), _block_isbad()  │          │             │
│  │  └────────────────────────────────┘          │             │
│  │  struct nand_memory_organization (memorg)     │             │
│  │  struct nand_ecc (ecc)                        │             │
│  │  struct nand_bbt (bbt)                        │             │
│  │  const struct nand_ops *ops                   │             │
│  └──────────────────────────────────────────────┘             │
│  struct spi_mem *spimem          // SPI 内存设备引用            │
│  struct mutex lock               // 操作互斥锁                 │
│  struct spinand_id id            // 芯片 ID                   │
│  u32 flags                       // QE/CR/PLANE_SELECT 等标志  │
│  op_templates {read/write/update_cache}  // SPI 操作模板        │
│  struct spinand_dirmap *dirmaps  // 直接映射描述符              │
│  struct spinand_ecc_info eccinfo // 片上 ECC 信息              │
│  u8 *cfg_cache                   // 配置寄存器缓存 (每die一个)  │
│  u8 *databuf, *oobbuf           // 页数据/OOB 缓冲区           │
│  u8 *scratchbuf                  // DMA 安全的临时缓冲区        │
│  const struct spinand_manufacturer *manufacturer               │
│  bool cont_read_possible         // 是否支持连续读               │
└──────────────────────────────────────────────────────────────┘
```

### 4.2 MTD 核心数据结构

```c
/* include/linux/mtd/mtd.h */
struct mtd_info {
    /* === 设备基本参数 === */
    u_char type;           // 设备类型: MTD_NORFLASH/MTD_NANDFLASH/MTD_RAM
    uint32_t flags;        // 设备标志: MTD_WRITEABLE/MTD_BIT_WRITEABLE
    uint64_t size;         // 设备总大小 (字节)
    uint32_t erasesize;    // 擦除块大小 (SPI NAND 通常为 128KB)
    uint32_t writesize;    // 最小写入单位 (SPI NAND 通常为 2048B = 1页)
    uint32_t writebufsize; // 写缓冲区大小
    uint32_t oobsize;      // 每页 OOB 大小
    uint32_t oobavail;     // 可用 OOB 字节数 (去除 ECC 后)

    /* === ECC 相关 === */
    unsigned int ecc_step_size;  // ECC 步长 (如 512B)
    unsigned int ecc_strength;   // ECC 纠错能力 (如 8 bits)
    unsigned int bitflip_threshold; // 位翻转告警阈值
    const struct mtd_ooblayout_ops *ooblayout; // OOB 布局

    /* === 操作回调 === */
    int (*_erase)(struct mtd_info *, struct erase_info *);
    int (*_read)(struct mtd_info *, loff_t, size_t, size_t *, u_char *);
    int (*_write)(struct mtd_info *, loff_t, size_t, size_t *, const u_char *);
    int (*_read_oob)(struct mtd_info *, loff_t, struct mtd_oob_ops *);
    int (*_write_oob)(struct mtd_info *, loff_t, struct mtd_oob_ops *);
    int (*_block_isbad)(struct mtd_info *, loff_t);
    int (*_block_markbad)(struct mtd_info *, loff_t);
    int (*_block_isreserved)(struct mtd_info *, loff_t);

    /* === OTP 操作 === */
    int (*_read_fact_prot_reg)(...);  // 读工厂 OTP
    int (*_read_user_prot_reg)(...);  // 读用户 OTP
    int (*_write_user_prot_reg)(...); // 写用户 OTP
    int (*_lock_user_prot_reg)(...);  // 锁定用户 OTP

    /* === 统计信息 === */
    struct mtd_ecc_stats ecc_stats;  // ECC 统计: corrected/failed/badblocks

    /* === 分区支持 === */
    struct mtd_info *parent;         // 父 MTD 设备 (分区时)
    struct list_head partitions;     // 子分区列表
    struct mtd_part part;            // 分区信息 (offset/size)
};
```

### 4.3 NAND 设备数据结构

```c
/* include/linux/mtd/nand.h */
struct nand_device {
    struct mtd_info mtd;                    // 嵌入的 MTD 设备
    struct nand_memory_organization memorg; // 存储组织
    struct nand_ecc ecc;                    // ECC 配置
    struct nand_row_converter rowconv;      // 行地址转换器
    struct nand_bbt bbt;                    // 坏块表 (位图缓存)
    const struct nand_ops *ops;             // NAND 底层操作
};

/* NAND 底层操作接口 */
struct nand_ops {
    int (*erase)(struct nand_device *nand, const struct nand_pos *pos);
    int (*markbad)(struct nand_device *nand, const struct nand_pos *pos);
    bool (*isbad)(struct nand_device *nand, const struct nand_pos *pos);
};

/* NAND 位置描述 */
struct nand_pos {
    unsigned int target;      // 目标 die
    unsigned int lun;         // 逻辑单元
    unsigned int plane;       // 平面
    unsigned int eraseblock;  // 擦除块号
    unsigned int page;        // 页号
};

/* 页 I/O 请求 */
struct nand_page_io_req {
    enum nand_page_io_req_type type; // READ 或 WRITE
    struct nand_pos pos;             // 目标位置
    unsigned int dataoffs;           // 页内数据偏移
    unsigned int datalen;            // 数据长度
    union { const void *out; void *in; } databuf; // 数据缓冲区
    unsigned int ooboffs;            // OOB 偏移
    unsigned int ooblen;             // OOB 长度
    union { const void *out; void *in; } oobbuf;  // OOB 缓冲区
    int mode;                        // MTD_OPS_PLACE_OOB/AUTO_OOB/RAW
    bool continuous;                 // 是否连续读模式
};
```

### 4.4 ECC 引擎数据结构

```c
/* include/linux/mtd/nand.h */
struct nand_ecc {
    struct nand_ecc_props defaults;      // 默认 ECC 配置
    struct nand_ecc_props requirements;  // 芯片要求的 ECC 能力
    struct nand_ecc_props user_conf;     // 用户配置
    struct nand_ecc_context ctx;         // 运行时 ECC 上下文
    struct nand_ecc_engine *ondie_engine;// 片上 ECC 引擎
    struct nand_ecc_engine *engine;      // 实际使用的 ECC 引擎
};

struct nand_ecc_engine {
    struct device *dev;
    struct list_head node;
    const struct nand_ecc_engine_ops *ops;       // ECC 操作
    enum nand_ecc_engine_integration integration; // 集成方式
    void *priv;
};

struct nand_ecc_engine_ops {
    int (*init_ctx)(struct nand_device *nand);       // 初始化
    void (*cleanup_ctx)(struct nand_device *nand);   // 清理
    int (*prepare_io_req)(struct nand_device *, struct nand_page_io_req *); // IO前准备
    int (*finish_io_req)(struct nand_device *, struct nand_page_io_req *);  // IO后处理
};
```

### 4.5 SPI NAND 芯片描述结构

```c
/* include/linux/mtd/spinand.h */
struct spinand_info {
    const char *model;                   // 型号名称
    struct spinand_devid devid;          // 设备 ID
    u32 flags;                           // SPINAND_HAS_QE_BIT 等标志
    struct nand_memory_organization memorg; // 存储组织
    struct nand_ecc_props eccreq;        // ECC 需求
    struct spinand_ecc_info eccinfo;     // 片上 ECC 处理
    struct {
        const struct spinand_op_variants *read_cache;   // 读缓存操作变体
        const struct spinand_op_variants *write_cache;  // 写缓存操作变体
        const struct spinand_op_variants *update_cache; // 更新缓存操作变体
    } op_variants;
    int (*select_target)(...);           // 多 die 选择
    int (*configure_chip)(...);          // 芯片配置
    int (*set_cont_read)(...);           // 连续读配置
    struct spinand_fact_otp fact_otp;    // 工厂 OTP
    struct spinand_user_otp user_otp;    // 用户 OTP
    unsigned int read_retries;           // 读重试次数
    int (*set_read_retry)(...);          // 读重试设置
};
```

---

## 5. 坏块处理机制与流程

### 5.1 坏块类型

| 类型 | 来源 | 标识方式 |
|------|------|---------|
| **出厂坏块 (Factory Bad)** | 制造过程缺陷 | OOB 第0-1字节非 0xFF |
| **运行时坏块 (Worn Bad)** | 擦写磨损/ECC不可纠正 | 擦除/编程失败后标记 |
| **保留块 (Reserved)** | BBT 存储等特殊用途 | BBT 中特殊标记 |

### 5.2 BBT (Bad Block Table) 实现

```c
/* drivers/mtd/nand/bbt.c */

// BBT 内存结构: 使用位图表示每个块的状态
struct nand_bbt {
    unsigned long *cache;  // 位图缓存, 每块 2 bits
};

// 块状态枚举
enum nand_bbt_block_status {
    NAND_BBT_BLOCK_STATUS_UNKNOWN = 0,  // 未知 (初始状态)
    NAND_BBT_BLOCK_GOOD = 1,           // 好块
    NAND_BBT_BLOCK_WORN = 2,           // 磨损坏块
    NAND_BBT_BLOCK_RESERVED = 3,       // 保留块
    NAND_BBT_BLOCK_FACTORY_BAD = 4,    // 出厂坏块
};
```

### 5.3 坏块检测流程

```
spinand_mtd_block_isbad(mtd, offs)
  └── nanddev_isbad(nand, &pos)
      ├── [BBT 已初始化] → 查询 BBT 缓存
      │   ├── nanddev_bbt_get_block_status()
      │   ├── 如果状态 == UNKNOWN:
      │   │   ├── 调用 nand->ops->isbad()        ← spinand_isbad()
      │   │   │   ├── spinand_read_page(mode=RAW) ← 读第0页OOB
      │   │   │   └── 检查 marker[0] != 0xff || marker[1] != 0xff
      │   │   └── 更新 BBT 缓存 (GOOD 或 FACTORY_BAD)
      │   └── 返回 WORN 或 FACTORY_BAD → true (坏块)
      └── [BBT 未初始化] → 直接调用 nand->ops->isbad()
```

**关键代码** (`drivers/mtd/nand/spi/core.c`):
```c
static bool spinand_isbad(struct nand_device *nand, const struct nand_pos *pos)
{
    struct spinand_device *spinand = nand_to_spinand(nand);
    u8 marker[2] = { };
    struct nand_page_io_req req = {
        .pos = *pos,
        .ooblen = sizeof(marker),  // 读取 OOB 前2字节
        .ooboffs = 0,
        .oobbuf.in = marker,
        .mode = MTD_OPS_RAW,       // RAW 模式, 不做 ECC
    };

    spinand_select_target(spinand, pos->target);
    spinand_read_page(spinand, &req);

    // 前两个 OOB 字节任一非 0xFF 即为坏块
    if (marker[0] != 0xff || marker[1] != 0xff)
        return true;
    return false;
}
```

### 5.4 坏块标记流程

```
spinand_mtd_block_markbad(mtd, offs)
  └── nanddev_markbad(nand, &pos)
      ├── 检查是否已是坏块 → 是则直接返回
      ├── nand->ops->markbad()        ← spinand_markbad()
      │   ├── spinand_select_target()
      │   └── spinand_write_page()    ← 向 OOB 写入 0x0000
      ├── 更新 BBT: set_block_status(NAND_BBT_BLOCK_WORN)
      ├── nanddev_bbt_update()        ← 同步 BBT (当前为 NOP)
      └── mtd->ecc_stats.badblocks++ ← 更新统计
```

### 5.5 擦除时的坏块保护

```c
/* drivers/mtd/nand/core.c */
static int nanddev_erase(struct nand_device *nand, const struct nand_pos *pos)
{
    // 擦除前检查: 坏块和保留块不允许擦除
    if (nanddev_isbad(nand, pos) || nanddev_isreserved(nand, pos)) {
        pr_warn("attempt to erase a bad/reserved block @%llx\n",
                nanddev_pos_to_offs(nand, pos));
        return -EIO;
    }
    return nand->ops->erase(nand, pos);
}
```

### 5.6 BBT 惰性加载策略

SPI NAND 采用**惰性加载 (Lazy Loading)** 策略:
- 初始时所有块状态为 `UNKNOWN`
- 首次访问某块时才从硬件读取坏块标记
- 读取结果缓存到 BBT, 后续直接查表
- 这避免了启动时全扫描的延迟

### 5.7 坏块池 (Bad Block Reserve) 与 BMT (Bad Block Management Table)

#### 5.7.1 概念澄清

**坏块池 (Bad Block Reserve Pool)** 和 **BMT (Bad Block Management Table)** 是实现坏块**替换/重映射**的机制，
它们**不在 Linux 主线内核的 MTD/NAND 核心层**中，而是分布在不同的软件层次：

```
┌────────────────────────────────────────────────────────────────────┐
│                        用户空间                                     │
├───────────────┬────────────────────────────────────────────────────┤
│  UBIFS / JFFS2 / YAFFS2         │    块设备文件系统 (ext4 等)       │
├───────────────┴──────┬──────────┴────────────────────────────────┤
│         UBI 层       │           FTL 层                           │
│  (坏块池 beb_rsvd)   │    (NFTL/INFTL: ReplUnitTable)            │
│  (磨损均衡 WL)       │    (厂商BMT: block remap table)            │
├──────────────────────┴───────────────────────────────────────────┤
│                  MTD 核心层                                        │
│         (BBT: 只记录坏块, 不做替换)                                 │
├──────────────────────────────────────────────────────────────────┤
│              NAND 驱动 (SPI NAND / Raw NAND)                      │
├──────────────────────────────────────────────────────────────────┤
│              NAND Flash 硬件                                      │
└──────────────────────────────────────────────────────────────────┘
```

**关键区分**:
- **MTD/BBT 层**: 只做坏块**标记和查询**，**不做坏块替换**。发现坏块后跳过，由上层处理
- **UBI 层**: 有坏块预留池 (`beb_rsvd_pebs`)，实现 PEB 级别的坏块替换和磨损均衡
- **FTL 层**: NFTL/INFTL 有 `ReplUnitTable`，实现块替换链表
- **厂商 BMT**: 不在主线内核中，是 MTK 等 SoC 厂商私有的块重映射表实现

#### 5.7.2 UBI 的坏块预留池 (beb_rsvd_pebs) — 主线内核中最接近"坏块池"的实现

UBI (Unsorted Block Images) 是 Linux 内核中管理 NAND Flash 的卷管理层，
它在 MTD 之上实现了坏块替换、磨损均衡和逻辑到物理块映射。

```c
/* drivers/mtd/ubi/ubi.h */
struct ubi_device {
    int rsvd_pebs;          // 总预留 PEB 数量
    int avail_pebs;         // 可用的空闲 PEB
    int beb_rsvd_pebs;      // 坏块预留池: 为坏块替换预留的 PEB 数量 ★
    int beb_rsvd_level;     // 坏块预留级别 (目标预留数)
    int bad_peb_limit;      // 预期坏 PEB 上限
    int bad_peb_count;      // 当前坏 PEB 计数
    int good_peb_count;     // 好 PEB 计数

    struct rb_root used;    // 已用 PEB 红黑树
    struct rb_root free;    // 空闲 PEB 红黑树 (可供分配)
    struct rb_root scrub;   // 需要 scrub 的 PEB 红黑树
    struct rb_root erroneous; // 错误 PEB 红黑树
    // ...
};
```

**坏块预留池计算** (`drivers/mtd/ubi/misc.c`):
```c
void ubi_calculate_reserved(struct ubi_device *ubi)
{
    // 预留级别 = 坏块上限 - 当前坏块数
    // 即: 还需要为未来可能出现的坏块预留多少个 PEB
    ubi->beb_rsvd_level = ubi->bad_peb_limit - ubi->bad_peb_count;
    if (ubi->beb_rsvd_level < 0) {
        ubi->beb_rsvd_level = 0;
        ubi_warn(ubi, "number of bad PEBs (%d) is above the expected limit (%d)",
                 ubi->bad_peb_count, ubi->bad_peb_limit);
    }
}
```

**坏块替换流程** (`drivers/mtd/ubi/wl.c`):
```c
// 当擦除某个 PEB 失败 (返回 -EIO) 时触发坏块替换:
static int __erase_worker(...)
{
    // 1. 擦除失败, PEB 变坏
    /* It is -EIO, the PEB went bad */

    // 2. 检查坏块预留池是否还有余量
    spin_lock(&ubi->volumes_lock);
    if (ubi->beb_rsvd_pebs == 0) {
        if (ubi->avail_pebs == 0) {
            // 预留池和可用池都为空 → 只能进入只读模式
            goto out_ro;
        }
        // 从可用池借一个 PEB
        ubi->avail_pebs -= 1;
    }
    spin_unlock(&ubi->volumes_lock);

    // 3. 标记物理坏块
    ubi_msg(ubi, "mark PEB %d as bad", pnum);
    ubi_io_mark_bad(ubi, pnum);  // 写入 MTD 层坏块标记

    // 4. 从预留池消耗一个 PEB
    spin_lock(&ubi->volumes_lock);
    if (ubi->beb_rsvd_pebs > 0)
        ubi->beb_rsvd_pebs -= 1;
    ubi->bad_peb_count += 1;
    ubi->good_peb_count -= 1;

    // 5. 重新计算预留级别
    ubi_calculate_reserved(ubi);

    if (ubi->beb_rsvd_pebs)
        ubi_msg(ubi, "%d PEBs left in the reserve", ubi->beb_rsvd_pebs);
    else
        ubi_warn(ubi, "last PEB from the reserve was used");
    // ...
}
```

**UBI 坏块替换示意**:
```
初始状态:
  Total PEBs: 1024    Good PEBs: 1000    Bad PEBs: 24
  beb_rsvd_pebs: 16   avail_pebs: 50      used PEBs: 934
  bad_peb_limit: 40

  ┌────────────────────────────────────────────────────────┐
  │         Used PEBs (934)          │  Free (50)│Reserve(16)│
  │ [PEB0][PEB3][PEB5]...[PEB998]    │  空闲池    │  坏块池   │
  └──────────────────────────────────┴──────────┴──────────┘

擦除 PEB#500 失败后:
  1. PEB#500 被标记为坏块
  2. 从坏块预留池消耗 1 个 PEB
  3. UBI 自动从 free 树分配新 PEB 替代 PEB#500 的逻辑角色

  Total PEBs: 1024    Good PEBs: 999     Bad PEBs: 25
  beb_rsvd_pebs: 15   avail_pebs: 50      used PEBs: 934
```

**Fastmap Pool** — UBI 的另一种"池"概念:
```c
/* drivers/mtd/ubi/ubi.h */
struct ubi_fm_pool {
    int pebs[UBI_FM_MAX_POOL_SIZE];  // 池中的 PEB 编号
    int used;                         // 已使用数
    int size;                         // 池当前大小
    int max_size;                     // 池最大容量
};

// UBI 有两个 fastmap pool:
// 1. fm_pool     - 用户卷分配用
// 2. fm_wl_pool  - 磨损均衡子系统专用
```

#### 5.7.3 NFTL/INFTL 的块替换表 (ReplUnitTable) — 早期 FTL 实现

NFTL (NAND Flash Translation Layer) 和 INFTL 是较早的 Flash 转换层实现，
主要用于 DiskOnChip 等设备，在主线内核中保留但已很少使用。

```c
/* include/linux/mtd/nftl.h */
struct NFTLrecord {
    __u16 *EUNtable;       // 虚拟块→物理块映射表 (Erase Unit Number)
    __u16 *ReplUnitTable;  // 替换块链表 ← 坏块替换的核心 ★
    __u16 numvunits;       // 虚拟块数量
    __u16 numfreeEUNs;     // 空闲替换块计数
    __u16 LastFreeEUN;     // 最后一个空闲替换块
    unsigned int nb_blocks;      // 物理块总数
    unsigned int nb_boot_blocks; // 引导区保留块数
};

// 块状态常量
#define BLOCK_NIL          0xffff  // 链末尾
#define BLOCK_FREE         0xfffe  // 空闲块 (可用于替换)
#define BLOCK_NOTEXPLORED  0xfffd  // 未探索块
#define BLOCK_RESERVED     0xfffc  // 保留块 (BIOS/坏块)
```

**NFTL 块替换原理**:
```
虚拟块号 (VUN)     物理块号 (EUN)       替换链 (ReplUnitTable)
┌─────┐           ┌─────┐
│ V0  │ ────────→ │ E3  │ ──→ E15 ──→ E27 ──→ NIL
├─────┤           ├─────┤
│ V1  │ ────────→ │ E7  │ ──→ NIL
├─────┤           ├─────┤
│ V2  │ ────────→ │ E12 │ ──→ E33 ──→ NIL
└─────┘           └─────┘

EUNtable[V0] = E3        (首个物理块)
ReplUnitTable[E3] = E15   (E3 的替换块)
ReplUnitTable[E15] = E27  (E15 的替换块)
ReplUnitTable[E27] = NIL  (链末尾)

当 E3 变坏或需要更新:
  1. 从 BLOCK_FREE 池中取一个空闲块 E15
  2. 将 E3 数据拷贝到 E15
  3. ReplUnitTable[E3] = E15
  4. 将 E3 标记为 BLOCK_RESERVED
```

#### 5.7.4 厂商 BMT (Bad Block Management Table) — 非主线内核

BMT 是部分 SoC 厂商 (最典型的是 **MediaTek/MTK**) 在其 BSP 代码中实现的坏块重映射机制，
**不在 Linux 主线内核中**。

**BMT 设计原理**:
```
Flash 物理区域划分:
┌──────────────────────────────────┬──────────────────────┐
│       正常数据区                  │     BMT 预留区        │
│  Block 0 ~ Block (N-R-1)         │  Block (N-R) ~ (N-1) │
│                                  │  (Reserve Pool)       │
│  当某个 Block 变坏时:             │  提供替换块            │
│  Block X → 重映射到预留区的 Block Y│                       │
└──────────────────────────────────┴──────────────────────┘

BMT 表结构 (典型 MTK 实现):
┌──────────────┬──────────────┐
│  Bad Block # │  Remap Block # │
├──────────────┼──────────────┤
│     50       │     1020     │
│     123      │     1021     │
│     456      │     1022     │
│     ...      │     ...      │
└──────────────┴──────────────┘

BMT 表存储位置: 通常在 Flash 最后几个好块中
```

**BMT vs UBI 坏块池 vs BBT 对比**:

| 特性 | MTD BBT | UBI beb_rsvd | NFTL ReplUnit | 厂商 BMT |
|------|---------|-------------|---------------|---------|
| **是否在主线内核** | ✅ 是 | ✅ 是 | ✅ 是 (legacy) | ❌ 否 (BSP) |
| **功能** | 记录坏块 | 坏块替换+WL | 块替换链 | 块重映射 |
| **做坏块替换吗** | ❌ 不做 | ✅ PEB级替换 | ✅ 链式替换 | ✅ 直接重映射 |
| **有预留池吗** | ❌ 无 | ✅ beb_rsvd_pebs | ✅ BLOCK_FREE池 | ✅ Reserve Pool |
| **磨损均衡** | ❌ 无 | ✅ 有 | ❌ 无 | ❌ 通常无 |
| **替换粒度** | N/A | 物理擦除块(PEB) | 擦除单元(EU) | 物理块 |
| **映射表存储** | Flash/RAM | RAM (EC/VID hdr) | Flash OOB | Flash 末尾块 |
| **典型使用者** | 所有 NAND 驱动 | UBIFS | DiskOnChip | MTK/部分嵌入式 |
| **代码位置** | `nand/bbt.c`, `nand/raw/nand_bbt.c` | `ubi/wl.c`, `ubi/eba.c` | `nftlcore.c` | 厂商BSP私有 |

#### 5.7.5 为什么 MTD 层不做坏块替换

Linux MTD 子系统的设计哲学是**分层解耦**:

```
设计原则:
  MTD 层 = 原始 Flash 访问 + 坏块信息管理
  上层  = 坏块策略 (替换/跳过/均衡)

原因:
1. MTD 是通用的 Flash 抽象层, 不应耦合特定的坏块策略
2. 不同使用场景需要不同的坏块处理策略:
   - UBIFS 场景: UBI 层做坏块替换 + 磨损均衡
   - JFFS2 场景: 文件系统自己跳过坏块, 不需要替换
   - 裸分区场景: 用户自行处理坏块 (如 Bootloader 镜像)
3. 坏块替换引入的逻辑复杂度应该由需要的层去承担

典型软件栈:
  方案 A:  UBIFS → UBI (坏块替换+WL) → MTD (BBT) → NAND 驱动
  方案 B:  JFFS2 (内部跳过坏块) → MTD (BBT) → NAND 驱动
  方案 C:  mtd-utils 裸操作 (用户跳过坏块) → MTD (BBT) → NAND 驱动
```

#### 5.7.6 Raw NAND BBT 的预留块 (maxblocks)

虽然 MTD BBT 不做坏块"替换"，但 Raw NAND 的 flash-based BBT 会**预留若干块**用于存储 BBT 表本身：

```c
/* include/linux/mtd/bbm.h */
struct nand_bbt_descr {
    int maxblocks;           // 用于搜索/存储 BBT 的最大块数 (预留)
    int reserved_block_code; // 预留块在 BBT 中的标识码
    // ...
};

#define NAND_BBT_SCAN_MAXBLOCKS  4  // 默认预留 4 个块

/* drivers/mtd/nand/raw/nand_bbt.c */
// BBT 存储在 Flash 末尾的 maxblocks 个块中:
//
// Flash 布局:
// ┌──────────────────────────────────┬─────────────┐
// │         正常数据区                │ BBT 预留区   │
// │     Block 0 ~ Block (N-5)        │ Block (N-4)  │
// │                                  │ Block (N-3)  │ ← BBT 主表
// │                                  │ Block (N-2)  │
// │                                  │ Block (N-1)  │ ← BBT 镜像表
// └──────────────────────────────────┴─────────────┘
//
// 在 BBT 位图中, 这些块被标记为 BBT_BLOCK_RESERVED (0x02)
// 擦除操作会检查 nanddev_isreserved() 并拒绝擦除预留块
```

---

## 6. Flash 经典测试 Case

### 6.1 内核自带 MTD 测试模块

位于 `drivers/mtd/tests/`:

| 测试模块 | 文件 | 功能 |
|---------|------|------|
| **stresstest** | `stresstest.c` | 随机读写擦除压力测试 |
| **oobtest** | `oobtest.c` | OOB 区域读写测试 |
| **pagetest** | `pagetest.c` | 页级读写验证 |
| **readtest** | `readtest.c` | 全设备读取测试 |
| **speedtest** | `speedtest.c` | 读写速度基准测试 |
| **subpagetest** | `subpagetest.c` | 子页读写测试 |
| **nandbiterrs** | `nandbiterrs.c` | 位翻转/ECC 测试 |
| **torturetest** | `torturetest.c` | 擦写循环耐久性测试 |
| **mtd_nandecctest** | `mtd_nandecctest.c` | NAND ECC 算法正确性测试 |

### 6.2 使用方法

```bash
# 加载测试模块 (指定设备号)
insmod /lib/modules/$(uname -r)/kernel/drivers/mtd/tests/mtd_stresstest.ko dev=0 count=1000
insmod /lib/modules/$(uname -r)/kernel/drivers/mtd/tests/mtd_readtest.ko dev=0
insmod /lib/modules/$(uname -r)/kernel/drivers/mtd/tests/mtd_oobtest.ko dev=0
insmod /lib/modules/$(uname -r)/kernel/drivers/mtd/tests/mtd_speedtest.ko dev=0

# 查看测试结果
dmesg | tail -50
```

### 6.3 用户空间测试工具 (mtd-utils)

```bash
# 基本信息查看
mtdinfo /dev/mtd0

# 擦除测试
flash_erase /dev/mtd0 0 0

# 读写测试
dd if=/dev/urandom of=/tmp/test_data bs=4096 count=10
nandwrite -p /dev/mtd0 /tmp/test_data
nanddump /dev/mtd0 -l 40960 -f /tmp/readback
diff /tmp/test_data /tmp/readback

# 坏块管理
nandtest /dev/mtd0          # 综合读写擦除测试
flash_erase -j /dev/mtd0 0 0  # 带坏块跳过的擦除

# OOB 操作
nanddump -o /dev/mtd0       # 带 OOB 数据 dump

# UBI 测试
ubiformat /dev/mtd0
ubiattach -m 0
ubimkvol /dev/ubi0 -N test -m
```

### 6.4 典型测试场景

```bash
#!/bin/bash
# Flash 综合测试脚本

MTD_DEV="/dev/mtd0"
MTD_NUM=0

echo "=== 1. 基本信息 ==="
cat /proc/mtd
mtdinfo $MTD_DEV

echo "=== 2. 坏块扫描 ==="
nandtest -k $MTD_DEV 2>&1 | grep -i bad

echo "=== 3. 全擦除 ==="
flash_erase $MTD_DEV 0 0

echo "=== 4. 写入测试 ==="
dd if=/dev/urandom of=/tmp/flash_test bs=2048 count=64
nandwrite -p $MTD_DEV /tmp/flash_test

echo "=== 5. 读回验证 ==="
nanddump $MTD_DEV -l 131072 -f /tmp/flash_readback
md5sum /tmp/flash_test /tmp/flash_readback

echo "=== 6. 压力测试 ==="
nandtest -l 10 $MTD_DEV  # 10轮读写擦除

echo "=== 7. ECC 状态 ==="
cat /sys/class/mtd/mtd${MTD_NUM}/ecc_failures
cat /sys/class/mtd/mtd${MTD_NUM}/ecc_corrected
cat /sys/class/mtd/mtd${MTD_NUM}/bad_blocks
```

---

## 7. 如何适配一个新的 Flash

### 7.1 适配 SPI NAND 步骤

#### Step 1: 获取芯片 datasheet 关键参数

需要确认:
- JEDEC ID (Manufacturer ID + Device ID)
- READ_ID 方法 (OPCODE / OPCODE_ADDR / OPCODE_DUMMY)
- 页大小、OOB 大小、块大小、总容量
- ECC 能力和步长
- 支持的 SPI 模式 (1/2/4/8线, DTR)
- 特殊寄存器和功能

#### Step 2: 添加厂商驱动文件

创建 `drivers/mtd/nand/spi/<vendor>.c`:

```c
// SPDX-License-Identifier: GPL-2.0
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/mtd/spinand.h>

#define SPINAND_MFR_NEWVENDOR    0xXX  // 厂商 JEDEC ID

/* ========== 操作变体定义 ========== */
static SPINAND_OP_VARIANTS(read_cache_variants,
    SPINAND_PAGE_READ_FROM_CACHE_1S_4S_4S_OP(0, 1, NULL, 0, 0),
    SPINAND_PAGE_READ_FROM_CACHE_1S_1S_4S_OP(0, 1, NULL, 0, 0),
    SPINAND_PAGE_READ_FROM_CACHE_1S_1S_2S_OP(0, 1, NULL, 0, 0),
    SPINAND_PAGE_READ_FROM_CACHE_FAST_1S_1S_1S_OP(0, 1, NULL, 0, 0),
    SPINAND_PAGE_READ_FROM_CACHE_1S_1S_1S_OP(0, 1, NULL, 0, 0));

static SPINAND_OP_VARIANTS(write_cache_variants,
    SPINAND_PROG_LOAD_1S_1S_4S_OP(true, 0, NULL, 0),
    SPINAND_PROG_LOAD_1S_1S_1S_OP(true, 0, NULL, 0));

static SPINAND_OP_VARIANTS(update_cache_variants,
    SPINAND_PROG_LOAD_1S_1S_4S_OP(false, 0, NULL, 0),
    SPINAND_PROG_LOAD_1S_1S_1S_OP(false, 0, NULL, 0));

/* ========== ECC 状态处理 (如需自定义) ========== */
static int newvendor_ecc_get_status(struct spinand_device *spinand, u8 status)
{
    switch (status & STATUS_ECC_MASK) {
    case STATUS_ECC_NO_BITFLIPS:
        return 0;
    case STATUS_ECC_HAS_BITFLIPS:
        return nanddev_get_ecc_conf(spinand_to_nand(spinand))->strength;
    case STATUS_ECC_UNCOR_ERROR:
        return -EBADMSG;
    default:
        break;
    }
    return -EINVAL;
}

/* ========== OOB 布局 (如需自定义) ========== */
static int newvendor_ooblayout_ecc(struct mtd_info *mtd, int section,
                                    struct mtd_oob_region *region)
{
    if (section)
        return -ERANGE;
    region->offset = 64;  // ECC 起始偏移
    region->length = 64;  // ECC 字节数
    return 0;
}

static int newvendor_ooblayout_free(struct mtd_info *mtd, int section,
                                     struct mtd_oob_region *region)
{
    if (section)
        return -ERANGE;
    region->offset = 2;   // 跳过 BBM 标记
    region->length = 62;  // 可用 OOB 字节数
    return 0;
}

static const struct mtd_ooblayout_ops newvendor_ooblayout = {
    .ecc = newvendor_ooblayout_ecc,
    .free = newvendor_ooblayout_free,
};

/* ========== 芯片信息表 ========== */
static const struct spinand_info newvendor_spinand_table[] = {
    /* 型号: NV1GQ4 - 1Gbit SPI NAND */
    SPINAND_INFO("NV1GQ4",
        SPINAND_ID(SPINAND_READID_METHOD_OPCODE_ADDR, 0xAA),
        NAND_MEMORG(1, 2048, 64, 64, 1024, 20, 1, 1, 1),
        NAND_ECCREQ(1, 512),
        SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
                                 &write_cache_variants,
                                 &update_cache_variants),
        SPINAND_HAS_QE_BIT,
        SPINAND_ECCINFO(&newvendor_ooblayout,
                        newvendor_ecc_get_status)),

    /* 型号: NV2GQ4 - 2Gbit SPI NAND, 双 die */
    SPINAND_INFO("NV2GQ4",
        SPINAND_ID(SPINAND_READID_METHOD_OPCODE_ADDR, 0xBB),
        NAND_MEMORG(1, 2048, 128, 64, 2048, 40, 1, 1, 2),
        NAND_ECCREQ(8, 512),
        SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
                                 &write_cache_variants,
                                 &update_cache_variants),
        SPINAND_HAS_QE_BIT,
        SPINAND_ECCINFO(&newvendor_ooblayout,
                        newvendor_ecc_get_status),
        SPINAND_SELECT_TARGET(newvendor_select_target)),
};

/* ========== 厂商初始化 (可选) ========== */
static int newvendor_spinand_init(struct spinand_device *spinand)
{
    // 厂商特定初始化, 如设置特殊寄存器
    return 0;
}

static const struct spinand_manufacturer_ops newvendor_spinand_manuf_ops = {
    .init = newvendor_spinand_init,
};

/* ========== 厂商注册结构 ========== */
const struct spinand_manufacturer newvendor_spinand_manufacturer = {
    .id = SPINAND_MFR_NEWVENDOR,
    .name = "NewVendor",
    .chips = newvendor_spinand_table,
    .nchips = ARRAY_SIZE(newvendor_spinand_table),
    .ops = &newvendor_spinand_manuf_ops,
};
```

#### Step 3: 注册到框架

**修改 `drivers/mtd/nand/spi/core.c`**:
```c
static const struct spinand_manufacturer *spinand_manufacturers[] = {
    // ... 已有厂商 ...
    &newvendor_spinand_manufacturer,  // 添加新厂商
};
```

**修改 `include/linux/mtd/spinand.h`**:
```c
extern const struct spinand_manufacturer newvendor_spinand_manufacturer;
```

#### Step 4: 修改编译配置

**修改 `drivers/mtd/nand/spi/Makefile`**:
```makefile
spinand-objs := core.o ... newvendor.o
```

#### Step 5: 设备树配置

```dts
&spi0 {
    flash@0 {
        compatible = "spi-nand";
        reg = <0>;
        spi-max-frequency = <104000000>;
        spi-tx-bus-width = <4>;
        spi-rx-bus-width = <4>;
    };
};
```

### 7.2 适配检查清单

- [ ] READ_ID 方法正确 (OPCODE/OPCODE_ADDR/OPCODE_DUMMY)
- [ ] 内存组织参数正确 (页大小/OOB/块大小/总容量)
- [ ] ECC 需求匹配 (strength/step_size)
- [ ] SPI 操作变体配置 (根据芯片支持的读写模式)
- [ ] QE (Quad Enable) 位正确配置
- [ ] OOB 布局正确 (BBM 位置/ECC 区域/可用区域)
- [ ] 多 die 芯片实现 select_target
- [ ] 特殊 ECC 状态解析 (如 GigaDevice 的 8-bit ECC)
- [ ] 块解锁命令正确
- [ ] 坏块标记位置正确 (通常 OOB byte 0-1)

---

## 8. QEMU 上测试 Flash 特性的 Case 和驱动代码

### 8.1 QEMU Flash 模拟支持

QEMU 内置了多种 Flash 模拟:
- **pflash (CFI NOR Flash)**: `virt` 平台自带
- **SPI NOR (m25p80)**: 通过 SPI 控制器模拟
- **NAND Flash**: 通过 `nand` 设备模拟 (限 Raw NAND)

### 8.2 QEMU 启动配置

```bash
#!/bin/bash
# 启动 QEMU 并挂载模拟 Flash

# 创建 Flash 镜像文件
dd if=/dev/zero of=flash.img bs=1M count=32

qemu-system-aarch64 \
    -machine virt \
    -cpu cortex-a53 \
    -nographic \
    -kernel Image \
    -append "console=ttyAMA0 root=/dev/ram0" \
    -initrd rootfs.cpio.gz \
    -drive if=pflash,format=raw,file=flash.img \
    -m 1024M
```

### 8.3 使用 mtd_nandsim 模拟 NAND Flash

Linux 内核自带 NAND 模拟器 `nandsim`:

```bash
# 加载 nandsim 模拟 256MB NAND (2048+64, 128KB block)
# 第一个ID字节=0x20(制造商), 第二个=0xAA(设备), 第三个=0x00, 第四个=0x15
modprobe nandsim first_id_byte=0x20 second_id_byte=0xaa \
    third_id_byte=0x00 fourth_id_byte=0x15

# 验证
cat /proc/mtd
# 应显示: mtd0: 10000000 00020000 "NAND simulator partition 0"

# 运行测试
insmod mtd_stresstest.ko dev=0 count=100
insmod mtd_readtest.ko dev=0
insmod mtd_oobtest.ko dev=0
insmod mtd_speedtest.ko dev=0
insmod mtd_pagetest.ko dev=0
```

### 8.4 QEMU + nandsim 测试脚本

```bash
#!/bin/bash
# qemu_flash_test.sh - QEMU 环境下的 Flash 测试脚本

set -e

echo "===== Flash 测试开始 ====="

# 1. 加载模拟器
echo "[1] 加载 NAND 模拟器..."
modprobe nandsim first_id_byte=0x20 second_id_byte=0xaa \
    third_id_byte=0x00 fourth_id_byte=0x15
sleep 1

MTD=/dev/mtd0
echo "[INFO] MTD 设备信息:"
cat /proc/mtd

# 2. 坏块扫描
echo "[2] 扫描坏块..."
mtd_debug info $MTD

# 3. 擦除全部
echo "[3] 全擦除..."
flash_erase $MTD 0 0

# 4. 页写入测试
echo "[4] 页写入测试..."
dd if=/dev/urandom of=/tmp/page_data bs=2048 count=1
nandwrite $MTD /tmp/page_data
nanddump $MTD -l 2048 -f /tmp/page_readback
if cmp -s /tmp/page_data /tmp/page_readback; then
    echo "[PASS] 页写入验证通过"
else
    echo "[FAIL] 页写入验证失败"
fi

# 5. OOB 测试
echo "[5] OOB 读写测试..."
insmod /lib/modules/$(uname -r)/kernel/drivers/mtd/tests/mtd_oobtest.ko dev=0
dmesg | tail -5

# 6. 压力测试
echo "[6] 压力测试..."
insmod /lib/modules/$(uname -r)/kernel/drivers/mtd/tests/mtd_stresstest.ko dev=0 count=100
dmesg | tail -5

# 7. ECC 测试
echo "[7] ECC 测试..."
insmod /lib/modules/$(uname -r)/kernel/drivers/mtd/tests/mtd_nandecctest.ko
dmesg | tail -5

# 8. 速度测试
echo "[8] 速度测试..."
flash_erase $MTD 0 0
insmod /lib/modules/$(uname -r)/kernel/drivers/mtd/tests/mtd_speedtest.ko dev=0
dmesg | tail -10

# 9. UBI 测试
echo "[9] UBI 层测试..."
flash_erase $MTD 0 0
modprobe ubi
ubiattach -m 0 -d 0
ubimkvol /dev/ubi0 -N test -m
mount -t ubifs ubi0:test /mnt
dd if=/dev/urandom of=/mnt/testfile bs=4096 count=100
sync
md5sum /mnt/testfile > /tmp/md5_before
umount /mnt
mount -t ubifs ubi0:test /mnt
md5sum /mnt/testfile > /tmp/md5_after
if diff /tmp/md5_before /tmp/md5_after; then
    echo "[PASS] UBI/UBIFS 数据一致性验证通过"
else
    echo "[FAIL] UBI/UBIFS 数据一致性验证失败"
fi
umount /mnt

# 10. 清理
echo "[10] 清理..."
ubidetach -d 0 2>/dev/null || true
rmmod nandsim 2>/dev/null || true

echo "===== Flash 测试结束 ====="
```

### 8.5 模拟坏块测试

```bash
# nandsim 支持注入坏块
# weakpages 参数模拟弱页 (写入后会产生位翻转)
modprobe nandsim first_id_byte=0x20 second_id_byte=0xaa \
    third_id_byte=0x00 fourth_id_byte=0x15 \
    weakpages=0,10,20,30  # 弱页列表

# bitflips 参数模拟位翻转概率
modprobe nandsim first_id_byte=0x20 second_id_byte=0xaa \
    third_id_byte=0x00 fourth_id_byte=0x15 \
    bitflips=5  # 最大5位翻转
```

---

## 9. SoC BootROM 从 NAND Flash 启动流程

> **注意**: BootROM 是烧录在 SoC 内部 ROM 中的固件，不在 Linux 内核源码中。
> 本章内容基于各 SoC 厂商公开文档 (Allwinner, Rockchip, MediaTek, Qualcomm, Samsung 等) 的通用原理总结。

### 9.1 上电启动全景

```
 SoC 上电
   │
   ▼
┌──────────────────────────────────────────────────────────────────┐
│ 阶段 0: 硬件复位                                                 │
│   CPU 从固定地址 (如 0x0 或 0xFFFF0000) 开始执行                  │
│   该地址映射到 SoC 内部 BootROM                                   │
└──────────────────────┬───────────────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────────────┐
│ 阶段 1: BootROM 执行                                             │
│   1. 读取 BootPin/eFuse/GPIO → 判断启动介质 (NAND/eMMC/SD/SPI NOR)│
│   2. 初始化对应的存储控制器 (SPI/NAND controller)                  │
│   3. 识别 Flash 芯片 (READ_ID)                                   │
│   4. 从 Flash 加载 SPL/BL2 到 SRAM                               │
│   5. 验证签名 (Secure Boot, 可选)                                │
│   6. 跳转到 SPL 执行                                             │
└──────────────────────┬───────────────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────────────┐
│ 阶段 2: SPL (Secondary Program Loader) / BL2                    │
│   1. 初始化 DDR 内存控制器                                       │
│   2. 从 Flash 加载 U-Boot/BL31/BL33 到 DDR                      │
│   3. 跳转到 U-Boot                                              │
└──────────────────────┬───────────────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────────────┐
│ 阶段 3: U-Boot                                                   │
│   1. 完整初始化硬件 (含完整的 MTD/NAND 驱动)                      │
│   2. 从 Flash 加载 kernel + DTB + rootfs                        │
│   3. 跳转到 Linux 内核                                          │
└──────────────────────────────────────────────────────────────────┘
```

### 9.2 BootROM 如何识别外部 Flash

#### 9.2.1 启动介质选择

```
SoC 通过以下方式确定启动介质:

方式 1: Boot Pin (硬件引脚)
  ┌─────────────────────────────┐
  │  BOOT_SEL[2:0]  启动介质    │
  ├─────────────────────────────┤
  │  000            SPI NOR     │
  │  001            SPI NAND    │
  │  010            Parallel NAND│
  │  011            eMMC        │
  │  100            SD Card     │
  │  101            USB         │
  └─────────────────────────────┘
  (具体编码因 SoC 而异)

方式 2: eFuse / OTP
  SoC 内部一次性可编程区域存储启动配置,
  量产时通过烧录工具写入, 优先级高于 Boot Pin。

方式 3: 启动序列 (Boot Sequence)
  部分 SoC 支持自动探测: 依次尝试 SPI NOR → SPI NAND → eMMC → SD,
  哪个能成功读到有效 header 就从哪个启动。
```

#### 9.2.2 "鸡生蛋"问题 — 不知道参数怎么读数据

```
BootROM 面临的核心矛盾:
  要读数据 → 需要知道 page_size, oob_size, block_size
  要知道这些参数 → 需要先从 Flash 读出信息
  → 这是一个"鸡生蛋"问题

关键解答: SPI NAND 的命令协议设计天然解决了这个问题。
识别芯片的命令 (RESET/READ_ID) 完全不需要知道任何几何参数。
```

**SPI NAND 的命令分为两类**:

```
类别 1: 与 Flash 几何参数无关的命令 (BootROM 可以直接发)
┌──────────────────────────────────────────────────────────────────┐
│ 命令           │ 格式                  │ 需要的先验知识          │
├──────────────────────────────────────────────────────────────────┤
│ RESET (FFh)    │ [FFh]                 │ 无 (固定1字节命令)      │
│ READ_ID (9Fh)  │ [9Fh][00h] → [ID1][ID2][ID3]  │ 无           │
│ GET_FEATURE    │ [0Fh][reg_addr] → [value]      │ 无           │
│ SET_FEATURE    │ [1Fh][reg_addr][value]          │ 无           │
└──────────────────────────────────────────────────────────────────┘
  这些命令是 SPI NAND 标准协议规定的, 所有芯片必须支持,
  格式完全固定, 不依赖芯片的 page/block/oob 尺寸。

类别 2: 依赖几何参数的命令 (必须先知道参数才能正确使用)
┌──────────────────────────────────────────────────────────────────┐
│ 命令              │ 需要知道什么                                  │
├──────────────────────────────────────────────────────────────────┤
│ PAGE_READ (13h)   │ 需要 row address → 需知道 pages_per_block    │
│                   │ 来计算 "第N个block的第M个page" 的行地址         │
│ READ_CACHE (03h)  │ 需要 column address → 需知道 page_size       │
│                   │ 来区分 Data 区和 OOB 区的偏移                  │
│ 坏块检查          │ 需要 page_size → OOB 从 column=page_size 开始 │
│ PROGRAM_LOAD      │ 需要 page_size → 确定写入范围                 │
│ BLOCK_ERASE       │ 需要 pages_per_block → 计算块地址              │
└──────────────────────────────────────────────────────────────────┘
```

**完整的参数获取时序**:

```
  时间线 ─────────────────────────────────────────────────────────→

  阶段 0: 完全不需要参数
  ├── RESET(FFh)        ← 固定命令, 无参数
  ├── 等待 BUSY 清除     ← GET_FEATURE(0Fh, 0xC0), 固定命令
  ├── READ_ID(9Fh, 00h) ← 固定命令, 读回 2~3 字节 ID
  │
  ├── ★ 此时 BootROM 拿到了芯片 ID (如 0xEF, 0xAA, 0x21)
  │
  阶段 1: 查表获取参数 (在 BootROM 内部完成, 不访问 Flash)
  ├── 查内置 ID 表: 0xEF_AA21 → page=2048, oob=64, ppb=64
  │
  ├── ★ 此时 BootROM 知道了所有几何参数
  │
  阶段 2: 用已知参数读数据
  ├── 检查坏块: 读 page 0 的 column=2048 (即OOB起始处)
  ├── 读数据: PAGE_READ(13h, row=0) → READ_CACHE(03h, col=0, len=2048)
  └── ...

  所以: 不存在 "不知道参数就要读数据" 的情况。
       READ_ID 在 "知道参数" 之前执行, 且不需要任何参数。
```

**如果 READ_ID 返回的 ID 不在 BootROM 内置表中怎么办？**

```
不同 SoC 的处理策略:

策略 A: 使用默认参数 (最常见)
  ┌──────────────────────────────────────────────────┐
  │ ID 不认识 → 假设 page=2048, oob=64, ppb=64     │
  │ (当前市场上 90%+ 的 SPI NAND 都是这个规格)       │
  │ 尝试读取 → 验证 boot header                     │
  │ 如果 header 有效 → 继续启动                      │
  │ 如果 header 无效 → 启动失败, 进入下载模式        │
  └──────────────────────────────────────────────────┘

策略 B: 暴力探测 (部分 SoC)
  ┌──────────────────────────────────────────────────┐
  │ 依次尝试常见的 page size:                        │
  │   尝试 page=2048 → 读 page 0 → 检查 header      │
  │   尝试 page=4096 → 读 page 0 → 检查 header      │
  │   尝试 page=2048,oob=128 → ...                  │
  │ 哪个能读出有效 header 就用哪个参数                │
  └──────────────────────────────────────────────────┘

策略 C: 从 eFuse/OTP 读参数 (高端 SoC)
  ┌──────────────────────────────────────────────────┐
  │ Flash 参数提前烧录在 SoC 的 eFuse 中:            │
  │   eFuse[NAND_PAGE_SIZE] = 2048                   │
  │   eFuse[NAND_OOB_SIZE]  = 64                     │
  │   eFuse[NAND_PPB]       = 64                     │
  │ BootROM 直接读 eFuse, 不依赖 ID 查找表           │
  │ 优点: 支持任意新型号 Flash, 无需更新 BootROM      │
  └──────────────────────────────────────────────────┘

策略 D: 读 ONFI/JEDEC 参数页 (少数高端 SoC)
  ┌──────────────────────────────────────────────────┐
  │ 发送 Read Parameter Page 命令                     │
  │ Flash 返回标准化的参数结构体, 包含所有几何信息     │
  │ 但: 大多数 SPI NAND 不支持标准 ONFI 参数页        │
  │ 主要用于 Parallel NAND                            │
  └──────────────────────────────────────────────────┘
```

**SPI NAND vs Parallel NAND 的参数获取差异**:

```
┌────────────────────────┬──────────────────────┬─────────────────────┐
│                        │ SPI NAND             │ Parallel (Raw) NAND │
├────────────────────────┼──────────────────────┼─────────────────────┤
│ ID 读取命令            │ 9Fh + dummy(00h)     │ 90h + addr(00h)     │
│ ID 长度                │ 2~3 字节             │ 2~5 字节            │
│ ONFI 参数页            │ 极少支持             │ 普遍支持            │
│ 主要参数获取方式       │ ID 查表              │ ID 查表 + ONFI      │
│ ID 编码信息量          │ 较少 (需查表)        │ 较多 (部分可解码)   │
│ 默认假设可行性         │ 高 (2KB+64B 占多数)  │ 中 (2KB/4KB/8KB)    │
│ BootROM 复杂度         │ 低                   │ 高                  │
└────────────────────────┴──────────────────────┴─────────────────────┘

Parallel NAND 的 ID 字节可以部分解码:
  第 3 字节 (Samsung 编码规则):
    bit[1:0] = page size: 00→1KB, 01→2KB, 10→4KB, 11→8KB
    bit[2]   = oob per 512B: 0→8B, 1→16B
    bit[5:4] = block size: 00→64KB, 01→128KB, 10→256KB, 11→512KB
  → 部分 BootROM 可以不查表, 直接从 ID 解码参数
  → 但这种编码规则因厂商和世代而异, 不完全可靠
```

**总结: BootROM 参数获取优先级**:

```
  ┌──────────────────────────────────────────────────┐
  │ 优先级 1: eFuse / OTP 中预烧录的参数 (如果有)    │
  │     ↓ 没有                                       │
  │ 优先级 2: READ_ID → 查内置 ID 表                 │
  │     ↓ ID 不认识                                  │
  │ 优先级 3: 读 ONFI Parameter Page (如果支持)       │
  │     ↓ 不支持                                     │
  │ 优先级 4: 使用默认参数 (2KB page, 64B OOB)        │
  │     ↓ 读出的 header 无效                          │
  │ 优先级 5: 启动失败, 进入 USB/UART 下载模式        │
  └──────────────────────────────────────────────────┘
```

#### 9.2.3 SPI NAND Flash 识别流程详细

```
BootROM 确定是 SPI NAND 启动后, 执行以下序列:

1. 初始化 SPI 控制器
   ┌─────────────────────────────────────────────┐
   │ - 配置为 SPI Mode 0 (CPOL=0, CPHA=0)       │
   │ - 设置安全的低速时钟 (通常 24~50MHz)         │
   │ - 使用标准单线 (1-1-1) 模式                  │
   │   (BootROM 不能假设 Flash 支持 Quad/Octal)   │
   └─────────────────────────────────────────────┘

2. 复位 Flash 芯片
   ┌─────────────────────────────────────────────┐
   │ 发送 RESET (FFh) 命令                        │
   │ 等待 tRST (通常 ≤ 500μs)                    │
   │ 目的: 确保芯片处于已知初始状态                 │
   └─────────────────────────────────────────────┘

3. 读取芯片 ID
   ┌─────────────────────────────────────────────┐
   │ 发送 READ_ID (9Fh) 命令                      │
   │ 读取 MFR ID + Device ID (2~3 字节)           │
   │                                             │
   │ 例: 9Fh → [EF] [AA] [21]                    │
   │         MFR=0xEF (Winbond)                  │
   │         Dev=0xAA21 (W25N01GV)               │
   │                                             │
   │ BootROM 内置一张 ID 查找表:                   │
   │ ┌───────────┬──────────┬──────┬──────┐       │
   │ │ MFR+DevID │ Page Size│ OOB  │Block │       │
   │ ├───────────┼──────────┼──────┼──────┤       │
   │ │ EF AA21   │ 2048     │ 64   │ 64pg │       │
   │ │ C8 D148   │ 2048     │ 128  │ 64pg │       │
   │ │ 2C 12     │ 2048     │ 64   │ 64pg │       │
   │ │ ...       │ ...      │ ...  │ ...  │       │
   │ └───────────┴──────────┴──────┴──────┘       │
   │                                             │
   │ 如果 ID 不在表中 → 使用默认参数或尝试下一介质  │
   └─────────────────────────────────────────────┘

4. (可选) 读取参数页 / ONFI 参数
   部分高端 BootROM 支持读取 ONFI Parameter Page,
   从中获取 page size, oob size, block size 等参数,
   无需硬编码 ID 表。但大多数 SPI NAND BootROM 仍依赖 ID 表。
```

#### 9.2.4 为什么 BootROM 只用单线低速模式

```
BootROM 运行时的约束:

1. 不知道外部 Flash 具体型号 → 只能用最通用的 1-1-1 SPI 模式
2. 不知道 PCB 信号质量    → 只能用保守的低速时钟 (24~50MHz)
3. 代码空间极小 (几十KB ROM) → 不能包含复杂的协商逻辑
4. 没有 DDR 内存             → 只能用 SoC 内部 SRAM (通常 32~256KB)

所以 BootROM 的策略是:
  - 用最简单、最安全的方式读出 SPL
  - SPL 加载后再初始化 DDR、切换到高速模式
  - 完整的 Flash 能力识别和优化由 U-Boot/Linux 完成
```

### 9.3 BootROM 如何读取 Flash 数据

#### 9.3.1 SPI NAND 读取命令序列

```
BootROM 读取 SPI NAND 的一个 Page 需要两步:

步骤 1: PAGE READ to cache (13h)
  ┌──────┬───────────────────┐
  │ 13h  │ Row Address (3B)  │  → Flash 将整页从阵列读入内部缓存
  └──────┴───────────────────┘
  等待 BUSY 清除 (轮询 Status Register, 0Fh 命令)
  典型等待时间: 25~100μs (tRD)

步骤 2: READ FROM CACHE (03h / 0Bh)
  ┌──────┬───────────────────┬───────┬──────────┐
  │ 03h  │ Column Addr (2B)  │ Dummy │ Data Out │  → 从缓存读出数据
  └──────┴───────────────────┴───────┴──────────┘
  BootROM 读取 Data (2048B) + OOB (64B)

完整序列:
  RESET(FFh) → GET_FEATURE(0Fh) 等 BUSY → READ_ID(9Fh)
  → PAGE_READ(13h, page=0) → 等 BUSY → READ_CACHE(03h) → 得到数据
```

#### 9.3.2 BootROM 的 Flash 地址布局

SoC 厂商对 NAND Flash 的启动区域有特定的布局规范：

```
典型 SPI NAND 启动布局 (以常见的 128MB SPI NAND 为例):

┌─────────────────────────────────────────────────────────────┐
│ Block 0    │ SPL/BL2 主拷贝 (Primary)                       │
├────────────┤ (BootROM 首先从这里读取)                        │
│ Block 1    │ SPL/BL2 主拷贝续 (如果SPL大于1个block)          │
├────────────┼────────────────────────────────────────────────┤
│ Block 2    │ SPL/BL2 备份拷贝 1 (Backup 1)                  │
├────────────┤                                                │
│ Block 3    │ SPL/BL2 备份拷贝 1 续                           │
├────────────┼────────────────────────────────────────────────┤
│ Block 4    │ SPL/BL2 备份拷贝 2 (Backup 2)                  │
├────────────┤                                                │
│ Block 5    │ SPL/BL2 备份拷贝 2 续                           │
├────────────┼────────────────────────────────────────────────┤
│ Block 6~7  │ SPL/BL2 备份拷贝 3 (Backup 3)                  │
├────────────┼────────────────────────────────────────────────┤
│ Block 8~15 │ U-Boot / BL31 / BL33                          │
├────────────┼────────────────────────────────────────────────┤
│ Block 16~31│ U-Boot 环境变量 / DTB                          │
├────────────┼────────────────────────────────────────────────┤
│ Block 32+  │ Kernel / RootFS / 用户数据                     │
└────────────┴────────────────────────────────────────────────┘

关键设计:
  - SPL 存储多份拷贝 (通常 4~8 份), 防止某个 block 变坏后无法启动
  - 每份拷贝从 block 边界开始, 方便 BootROM 按 block 粒度跳过坏块
  - SPL 体积通常 ≤ 64~128KB, 正好 1~2 个 block
```

#### 9.3.3 Boot Header 格式

```
BootROM 读取 Flash 第一个 Page 后, 首先验证 Boot Header:

┌──────────────────────────────────────────────────────────┐
│  Boot Header (通常在 Block 0, Page 0 起始处)              │
├──────────┬──────────┬────────────────────────────────────┤
│ Offset   │ Size     │ 含义                               │
├──────────┼──────────┼────────────────────────────────────┤
│ 0x00     │ 4/8 B    │ Magic Number (魔数, 厂商定义)       │
│          │          │ Allwinner: "eGON.BT0"              │
│          │          │ Rockchip:  "RK33" / 0x534E4B52     │
│          │          │ MediaTek:  "BRLYT" / "EMMC_BOOT"   │
├──────────┼──────────┼────────────────────────────────────┤
│ 0x04/08  │ 4 B      │ SPL 大小 (字节数)                   │
├──────────┼──────────┼────────────────────────────────────┤
│ 0x08/0C  │ 4 B      │ Checksum / CRC32                   │
├──────────┼──────────┼────────────────────────────────────┤
│ 0x0C/10  │ 4 B      │ Load Address (加载到 SRAM 的地址)   │
├──────────┼──────────┼────────────────────────────────────┤
│ 0x10+    │ 变长     │ 其他元数据 (版本号、签名等)          │
├──────────┼──────────┼────────────────────────────────────┤
│ Header后 │ SPL Size │ SPL 代码/数据                       │
└──────────┴──────────┴────────────────────────────────────┘

BootROM 验证流程:
  1. 读取 Page 0 的前 N 字节
  2. 检查 Magic Number 是否匹配 → 不匹配则此 block 无效
  3. 提取 SPL 大小和 Checksum
  4. 继续读取剩余 Page, 拼成完整 SPL 镜像
  5. 计算 Checksum/CRC, 与 Header 中的值比对
  6. (Secure Boot) 验证 RSA/ECDSA 签名
  7. 全部通过 → 跳转到 Load Address 执行
```

### 9.4 BootROM 碰到坏块怎么处理

这是最关键的部分。BootROM 的坏块处理**极其简化**，与 Linux 内核的完整 BBT 机制完全不同。

#### 9.4.1 BootROM 坏块处理策略 — 跳过坏块法 (Skip Bad Block)

```
大多数 SoC BootROM 采用最简单的策略: 遇到坏块就跳过, 读下一个 block。

                    BootROM 读取 SPL 流程
                    =====================

  初始化: block_idx = 0, 已读数据 = 0, 目标大小 = SPL_SIZE

  ┌──────────────────────────────────────────────┐
  │  读取 Block[block_idx] 的第一个 Page          │
  │  检查 OOB 第一个字节 (坏块标记位置)            │
  └─────────────────────┬────────────────────────┘
                        │
            ┌───────────┴───────────┐
            │ OOB[0] == 0xFF ?      │
            │ (好块?)               │
            └───────┬───────┬───────┘
                    │       │
                  是│       │否 (坏块)
                    ▼       ▼
         ┌──────────────┐  ┌──────────────────────┐
         │ 好块:         │  │ 坏块:                 │
         │ 读取整个 block │  │ 跳过此 block           │
         │ 的所有 page   │  │ block_idx++           │
         │ 追加到 SRAM   │  │ (不读取任何数据)       │
         │ 已读 += block │  │ 检查是否超出搜索范围    │
         └──────┬───────┘  └──────────┬───────────┘
                │                     │
                ▼                     ▼
         ┌─────────────┐      ┌─────────────────┐
         │ 已读够       │      │ 继续找下一个      │
         │ SPL_SIZE ?   │      │ 好 block         │
         │ 是→验证执行  │      │ (回到开头)        │
         │ 否→读下一block│      │                  │
         └─────────────┘      └─────────────────┘

伪代码:
  block_idx = 0
  loaded = 0
  max_search_blocks = 最大搜索范围 (如 32 或 64 个 block)

  while (loaded < spl_size && block_idx < max_search_blocks):
      // 读第一个 page 的 OOB, 检查坏块标记
      page_read(block_idx * pages_per_block, &page_buf, &oob_buf)
      if (oob_buf[0] != 0xFF):  // 坏块
          block_idx++
          continue

      // 好块: 逐 page 读取整个 block
      for page in range(pages_per_block):
          page_read(block_idx * pages_per_block + page, sram_ptr, NULL)
          sram_ptr += page_size
          loaded += page_size
          if (loaded >= spl_size):
              break

      block_idx++

  // 验证 checksum, 跳转执行
```

#### 9.4.2 多份备份拷贝机制

```
大多数 SoC BootROM 不只依赖跳过坏块, 还支持多份备份:

搜索策略 (以 4 份 SPL 备份为例):

  拷贝 0: 从 Block 0 开始, 搜索范围 Block 0~7
  拷贝 1: 从 Block 8 开始, 搜索范围 Block 8~15
  拷贝 2: 从 Block 16 开始, 搜索范围 Block 16~23
  拷贝 3: 从 Block 24 开始, 搜索范围 Block 24~31

  BootROM 搜索逻辑:
  ┌──────────────────────────────────────────────┐
  │ for copy = 0 to N_COPIES-1:                  │
  │     start_block = copy * blocks_per_copy      │
  │     尝试从 start_block 开始, 跳过坏块读取 SPL  │
  │     if (读取成功 && checksum 正确):            │
  │         跳转执行 SPL ← 启动成功!              │
  │     else:                                    │
  │         继续尝试下一份拷贝                     │
  │ 所有拷贝都失败 → 进入 USB/UART 下载模式       │
  └──────────────────────────────────────────────┘

实际例子:
  Block 0: 坏块 ✗ → 跳过
  Block 1: 好块 ✓ → 读取 SPL... checksum 失败 (数据损坏)
  → 拷贝 0 失败, 尝试拷贝 1
  Block 8: 好块 ✓ → 读取 SPL... checksum 正确!
  → 从拷贝 1 启动成功
```

#### 9.4.3 各厂商 BootROM 坏块处理差异

| SoC 厂商 | 坏块检查方式 | SPL 备份数 | 搜索范围 | ECC 处理 | 特殊机制 |
|----------|------------|-----------|---------|---------|--------|
| **Allwinner** | OOB[0] != 0xFF | 最多 7 份 | 前 64 blocks | 依赖片上ECC | Boot0 特殊格式 |
| **Rockchip** | OOB[0] != 0xFF | 5 份 | 前 50 blocks | 硬件 BCH | IDB (ID Block) 格式 |
| **MediaTek** | OOB[0] != 0xFF | 2~4 份 | 前 20 blocks | 硬件 BCH/RS | BMT 坏块重映射 |
| **Qualcomm** | OOB[0] != 0xFF | 多份 | 配置依赖 | 硬件 RS/BCH | MIBIB 分区表 |
| **Samsung** | OOB[0]/[column] | 2~4 份 | 前 8~16 blocks | 硬件 BCH | BL1 固定格式 |
| **NXP/Freescale** | OOB[0] != 0xFF | 2~8 份 | FCB/DBBT 表 | BCH | FCB+DBBT 发现机制 |

#### 9.4.4 NXP 的 FCB/DBBT 机制 (最复杂的 BootROM 坏块处理)

```
NXP i.MX 系列有独特的坏块处理, 值得单独介绍:

FCB (Firmware Configuration Block):
  - 存储 NAND 参数 (page size, block size, ECC 配置等)
  - BootROM 在前 N 个 block 中搜索 FCB
  - FCB 本身使用特殊 ECC (BCH62 bit), 比正常数据更强的纠错
  - FCB 中包含 DBBT 和 Firmware 的位置信息

DBBT (Discovered Bad Block Table):
  - 存储已知坏块列表
  - BootROM 读取 DBBT 后, 在加载 Firmware 时自动跳过表中的坏块
  - 相当于 BootROM 级别的 "BBT"

启动搜索流程:
  ┌────────────────────────────────────────────────────┐
  │ Block 0: 搜索 FCB → 成功/失败                       │
  │ Block 1: 搜索 FCB → 成功/失败                       │
  │ Block 2: 搜索 FCB → 找到!                           │
  │   → 从 FCB 获取: DBBT 在 Block 4, FW 在 Block 8    │
  │ Block 4: 读取 DBBT → 坏块列表: [Block 10, Block 15] │
  │ Block 8: 开始读取 Firmware                          │
  │   → Block 10 在坏块列表中, 自动跳过                  │
  │   → Block 15 在坏块列表中, 自动跳过                  │
  │   → 加载完成, 验证, 执行                            │
  └────────────────────────────────────────────────────┘
```

#### 9.4.5 ECC 在 BootROM 阶段的处理

```
BootROM 阶段的 ECC 处理是另一个关键问题:

┌────────────────────────────────────────────────────────────────┐
│                   BootROM ECC 策略                             │
├────────────────────────────────────────────────────────────────┤
│                                                                │
│  方案 A: 依赖 SPI NAND 片上 ECC (On-Die ECC)                   │
│  ─────────────────────────────────────────                     │
│  大多数 SPI NAND BootROM 使用此方案:                            │
│  - SPI NAND 出厂默认 ECC_EN = 1 (ECC 已开启)                   │
│  - BootROM 只需正常读数据, 片上 ECC 自动纠错                    │
│  - BootROM 读 Status Register 检查 ECC 状态位                  │
│    - ECC OK → 数据可信                                        │
│    - ECC 不可纠正 → 此 page/block 视为损坏, 跳过               │
│  - 优点: BootROM 不需要实现 ECC 算法, 代码量极小                │
│  - 缺点: 依赖芯片内置 ECC 强度 (通常 1~8 bit/512B)             │
│                                                                │
│  方案 B: BootROM 内置硬件 ECC 引擎 (Raw NAND)                   │
│  ─────────────────────────────────────────                     │
│  Parallel/Raw NAND 没有片上 ECC, SoC 需要自己做:               │
│  - SoC NAND 控制器内置 BCH/RS 硬件 ECC 引擎                    │
│  - BootROM 配置 ECC 引擎参数 (通常硬编码)                      │
│  - 读取时硬件自动计算和纠正 ECC 错误                            │
│  - 要求: 烧录工具必须使用相同的 ECC 配置写入 SPL               │
│                                                                │
│  方案 C: 不使用 ECC, 依赖多份冗余                               │
│  ─────────────────────────────────────────                     │
│  极少数简单 BootROM:                                           │
│  - 不做 ECC, 用 checksum 整体校验                              │
│  - 如果 checksum 失败, 尝试下一份备份                           │
│  - 可靠性依赖于多份拷贝                                        │
└────────────────────────────────────────────────────────────────┘
```

### 9.5 BootROM vs Linux 内核 — 坏块处理对比

| 维度 | BootROM | Linux 内核 (MTD/UBI) |
|------|---------|--------------------|
| **代码量** | 几 KB~几十 KB (ROM) | 几十万行 (MTD+NAND+UBI) |
| **运行环境** | SRAM (32~256KB), 无 DDR | DDR 全部可用 |
| **BBT** | 无 (或简单坏块表如 DBBT) | 完整 BBT (RAM bitmap + Flash 存储) |
| **坏块检测** | 只检查 OOB[0] | OOB 多字节检测 + BBT 缓存 |
| **坏块处理** | 简单跳过 + 多份备份 | 完整替换 (UBI beb_rsvd) + 磨损均衡 |
| **ECC** | 依赖片上 ECC / 硬件 ECC (固定配置) | 支持多种 ECC 引擎, 可配置 |
| **Flash 识别** | ID 查找表 (有限型号) | 完整 spinand_info 表 + ONFI |
| **SPI 模式** | 只用 1-1-1 单线低速 | 自动协商最优模式 (x1/x2/x4/x8) |
| **坏块标记** | 只读检查, 不标记新坏块 | 检测+标记+记录 |
| **地址映射** | 物理 block 地址直接访问 | 逻辑→物理映射 (UBI PEB/LEB) |
| **目标** | 尽快加载 SPL, 不追求完备 | 完整的 Flash 管理和数据可靠性 |

### 9.6 量产烧录时的坏块处理

```
量产时如何把 SPL/U-Boot 正确烧录到有坏块的 Flash 上:

烧录工具的工作流程:

  1. 全片扫描坏块
     对每个 block 的 page 0 和 page (last) 检查 OOB[0]
     记录坏块列表

  2. 烧录 SPL (多份拷贝, 跳过坏块)
     ┌──────────────────────────────────────────┐
     │ 拷贝 0 目标: Block 0 开始                 │
     │   Block 0: 坏块 → 跳过                    │
     │   Block 1: 好块 → 烧录 SPL 第一部分       │
     │   Block 2: 好块 → 烧录 SPL 第二部分       │
     │ 拷贝 1 目标: Block 8 开始                 │
     │   Block 8: 好块 → 烧录 SPL               │
     │   ...                                    │
     └──────────────────────────────────────────┘

     关键: 烧录工具的跳过坏块逻辑必须与 BootROM 一致!
           否则 BootROM 读到的数据会错位。

  3. 烧录 U-Boot / Kernel / RootFS
     使用 UBI 格式: ubiformat 会自动处理坏块
     或者使用 mtd-utils: nandwrite -p 自动跳过坏块

  4. (可选) 写入坏块表
     - NXP: 写入 DBBT
     - MTK: 写入 BMT
     - 其他: 依赖运行时 BBT 扫描
```

### 9.7 如果 Block 0 本身就是坏块怎么办

```
这是最极端的场景, 各厂商的处理方式:

1. 大多数 NAND 芯片厂商保证 Block 0 出厂时是好块
   (这是 JEDEC/ONFI 标准的隐含约定, 但并非强制)

2. 如果 Block 0 确实是坏块:
   ┌──────────────────────────────────────────────────┐
   │ 方案 A: BootROM 搜索前 N 个 block (最常见)        │
   │   Block 0 坏 → 检查 Block 1 → 检查 Block 2...   │
   │   在搜索范围内找到第一个有效 boot header 的好块    │
   │                                                  │
   │ 方案 B: BootROM 放弃, 进入下载模式               │
   │   极简 BootROM 只检查 Block 0                    │
   │   Block 0 坏 → 进入 USB/UART 下载模式            │
   │   (这类 SoC 不太适合用 NAND 启动)                │
   │                                                  │
   │ 方案 C: 切换到备选启动介质                        │
   │   NAND 启动失败 → 自动尝试 eMMC/SD/SPI NOR       │
   └──────────────────────────────────────────────────┘

实际可靠性计算:
  假设: 坏块率 2% (每 100 个 block 有 2 个坏块)
         4 份 SPL 备份, 每份占 2 blocks, 搜索范围各 8 blocks

  单份 SPL 所有 block 都坏的概率: 0.02^8 ≈ 2.56×10^-14
  4 份全部失败的概率: (1 - (1-0.02)^8)^4 ≈ 极其微小
  → 多份备份策略在实践中非常可靠
```

---

## 10. 面试问题与答案

### Q1: NOR Flash 和 NAND Flash 的区别是什么？

**答**: 

| 特性 | NOR Flash | NAND Flash |
|------|-----------|------------|
| 接口 | 地址/数据总线并行, 或 SPI | 8/16位I/O总线, 或 SPI |
| 读取 | 随机读, XIP (可就地执行) | 页读取, 不支持 XIP |
| 写入 | 按字节/字写入 | 按页写入 (2KB/4KB) |
| 擦除 | 扇区擦除 (4KB~64KB) | 块擦除 (128KB~) |
| 速度 | 读快, 写慢 | 写快, 读也较快 |
| 密度 | 较小 (MB级) | 较大 (GB级) |
| 可靠性 | 高, 几乎无坏块 | 需坏块管理, 需 ECC |
| 寿命 | 10万~100万次擦写 | 1万~10万次擦写 |
| 典型用途 | Bootloader, 代码存储 | 数据存储, 文件系统 |

### Q2: SPI NAND 的读取操作分为哪几步？为什么需要两步？

**答**: SPI NAND 读取分为两步:

1. **PAGE READ (0x13)**: 将 Flash 阵列中的数据加载到芯片内部的**页缓存 (Page Buffer)**。这一步是芯片内部操作, 需要等待 tREAD (25~100μs)。
2. **READ FROM CACHE (0x03/0x0b/0x3b/0x6b)**: 从页缓存中通过 SPI 总线读出数据。

两步分离的原因:
- Flash 存储阵列和 SPI 接口的时钟域不同
- 片上 ECC 在 PAGE READ 阶段完成校验和纠错
- 页缓存作为中间缓冲, 允许用 Dual/Quad SPI 高速读出

### Q3: 什么是 ECC？SPI NAND 支持哪些 ECC 方式？

**答**: ECC (Error Correction Code) 是纠错码, 用于检测和纠正 Flash 存储位翻转。

SPI NAND 支持的 ECC 方式:
1. **片上 ECC (On-Die)**: Flash 芯片内部硬件 ECC, 最常用, 通过 CFG 寄存器 BIT(4) 使能
2. **主控 ECC (On-Host)**: SPI 控制器硬件 ECC, 性能高但需控制器支持
3. **软件 ECC**: 内核软件实现 (Hamming/BCH), 灵活但消耗 CPU
4. **无 ECC**: 不做纠错 (风险高, 极少使用)

典型 ECC 能力: 1-bit/512B (Hamming), 4-bit/512B, 8-bit/512B (BCH)

### Q4: NAND Flash 为什么会有坏块？如何管理？

**答**: 

**坏块产生原因**:
- **出厂坏块**: 制造缺陷, 1Gbit NAND 通常允许 20个
- **运行时坏块**: 擦写磨损、电压异常、温度过高导致

**管理策略**:
1. **坏块表 (BBT)**: 内存中维护位图, 记录每个块状态
2. **坏块标记**: OOB 区前2字节非 0xFF 表示坏块
3. **坏块跳过**: 文件系统 (UBI/UBIFS/JFFS2) 自动跳过坏块
4. **磨损均衡**: UBI 层实现, 均匀分布擦写次数

**内核实现**: `nanddev_isbad()` 先查 BBT 缓存, 未命中则读硬件标记并更新缓存 (惰性加载)

### Q5: MTD 子系统的层次结构是怎样的？

**答**:
```
用户空间: /dev/mtdN (字符设备) | /dev/mtdblockN (块设备)
   │
MTD 核心层 (mtdcore.c): 统一接口, 设备注册, 分区管理
   │
NAND 核心 (nand/core.c): 坏块管理, ECC 引擎, 通用操作
   │
SPI NAND/Raw NAND/SPI NOR: 协议特定实现
   │
总线层: SPI-MEM / 并行总线
   │
控制器驱动: 硬件操作
```

### Q6: `mtd_info` 结构中 `erasesize` 和 `writesize` 分别代表什么？

**答**:
- **erasesize**: 最小擦除单位 (一个 Block), SPI NAND 典型值 128KB
- **writesize**: 最小写入单位 (一个 Page), SPI NAND 典型值 2KB 或 4KB

NAND Flash 必须先擦后写, 擦除以 Block 为单位, 写入以 Page 为单位。不能对单个 Page 擦除, 也不能在 Block 中只擦除部分 Page。

### Q7: SPI Flash 支持的 Quad/Dual 模式有什么区别？命名含义是什么？

**答**: 以 `1S-1S-4S` 为例:
- 第一个数字 (1): **命令** 使用的 IO 线数
- 第二个数字 (1): **地址** 使用的 IO 线数
- 第三个数字 (4): **数据** 使用的 IO 线数
- S = SDR (单沿传输), D = DTR (双沿传输)

常见模式:
- `1S-1S-1S`: 标准 SPI, 最慢
- `1S-1S-2S`: 双线数据读
- `1S-1S-4S`: 四线数据读 (常用)
- `1S-4S-4S`: 四线地址+数据 (更快)
- `1S-1D-4D`: DTR 四线 (最快之一)

### Q8: 如何在 Linux 内核中为一款新的 SPI NAND Flash 添加支持？

**答**: 核心步骤:

1. 查看 Datasheet 获取: JEDEC ID, 读 ID 方法, 存储参数, ECC 能力
2. 在 `drivers/mtd/nand/spi/` 创建或修改厂商文件
3. 填写 `spinand_info` 结构: memorg, eccreq, op_variants, devid
4. 如果 ECC 状态解析不同于标准, 实现自定义 `ecc_get_status()`
5. 如果 OOB 布局不同, 实现自定义 `ooblayout` ops
6. 注册到 `spinand_manufacturers` 数组
7. 修改 Makefile, 编译测试
8. 设备树中配置 `compatible = "spi-nand"`

**关键宏**: `SPINAND_INFO()`, `NAND_MEMORG()`, `NAND_ECCREQ()`, `SPINAND_ID()`

### Q9: OOB (Out-Of-Band) 区域是做什么的？

**答**: OOB 是每个 NAND 页附带的额外存储区域, 用途:

1. **坏块标记 (BBM)**: 前 2 字节, 非 0xFF 表示坏块
2. **ECC 校验数据**: 存储 ECC 码, 用于纠错
3. **文件系统元数据**: JFFS2/YAFFS 用于存管理信息
4. **用户数据**: 剩余的 "free" 区域可由应用使用

典型大小: 2KB 页对应 64B OOB, 4KB 页对应 128~256B OOB

OOB 访问模式:
- `MTD_OPS_PLACE_OOB`: 指定偏移放置
- `MTD_OPS_AUTO_OOB`: 自动使用 free 区域
- `MTD_OPS_RAW`: 原始访问 (包含 ECC 区域)

### Q10: 什么是磨损均衡 (Wear Leveling)？为什么 NAND Flash 需要它？

**答**: NAND Flash 每个 Block 的擦写次数有限 (SLC: ~10万次, MLC: ~1万次)。如果总是擦写同一个 Block, 它会很快耗尽寿命, 而其他 Block 可能几乎未被使用。

磨损均衡确保擦写操作均匀分布到所有好块上, 延长 Flash 整体寿命。

Linux 中的实现:
- **UBI (Unsorted Block Images)**: MTD 之上的磨损均衡层
  - 维护逻辑块到物理块的映射表
  - 动态/静态磨损均衡
  - 坏块管理和损耗均衡整合
- **UBIFS**: 基于 UBI 的文件系统, 直接受益于磨损均衡

### Q11: SPI NAND 和 Raw NAND 的区别？

**答**:

| 特性 | SPI NAND | Raw NAND |
|------|----------|----------|
| 接口 | SPI (1/2/4/8线) | 并行 8/16 位总线 |
| 引脚数 | 4-8 个 | 30+ 个 |
| 速度 | 较低 (SPI 时钟限制) | 较高 (并行总线) |
| ECC | 通常片上 ECC | 通常主控 ECC |
| 控制器 | SPI 控制器 (通用) | 专用 NAND 控制器 |
| 成本 | 低 | 中 |
| 容量 | 小~中 (128MB~4GB) | 中~大 (1GB~64GB+) |
| 应用场景 | IoT、嵌入式、启动存储 | 手机、SSD |

### Q12: 描述一下 `spinand_probe()` 函数的完整流程。

**答**: (参见 3.3.1 节的详细流程图)

核心步骤:
1. 分配 `spinand_device` 结构
2. 发送 RESET 命令复位芯片
3. READ_ID 识别芯片, 在厂商表中匹配
4. 分配数据/OOB/scratch 缓冲区
5. 读取并缓存各 die 的配置寄存器
6. 使能 Quad 模式 (如需)
7. 执行厂商特定初始化
8. 解锁所有块
9. 初始化通用 NAND 设备 (BBT, memorg → MTD 参数转换)
10. 初始化 ECC 引擎 (默认片上 ECC)
11. 注册 MTD 回调函数
12. 创建 SPI dirmap 描述符
13. `mtd_device_register()` 注册到 MTD 子系统

---

## 附录: 关键文件索引

| 文件路径 | 功能 |
|---------|------|
| `include/linux/mtd/mtd.h` | MTD 核心数据结构定义 |
| `include/linux/mtd/nand.h` | NAND 通用数据结构和 ECC 框架 |
| `include/linux/mtd/spinand.h` | SPI NAND 协议和数据结构 |
| `include/linux/mtd/rawnand.h` | Raw NAND 命令和选项定义 |
| `include/linux/mtd/spi-nor.h` | SPI NOR 命令和协议定义 |
| `include/linux/mtd/bbm.h` | 坏块管理 (BBT) 数据结构 |
| `drivers/mtd/mtdcore.c` | MTD 核心: 注册/分区/sysfs |
| `drivers/mtd/nand/core.c` | NAND 通用: isbad/markbad/erase/ECC引擎 |
| `drivers/mtd/nand/bbt.c` | BBT: 初始化/查询/更新 |
| `drivers/mtd/nand/spi/core.c` | SPI NAND 核心: probe/read/write/erase |
| `drivers/mtd/nand/spi/<vendor>.c` | 各厂商芯片描述和特殊处理 |
| `drivers/mtd/spi-nor/core.c` | SPI NOR 核心驱动 |
| `drivers/mtd/tests/` | MTD 测试模块集合 |
