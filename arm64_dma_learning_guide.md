# ARM64 Linux 内核 DMA 深度学习指南

> 基于 Linux 6.18.1 内核源码分析，面向 ARMv8 架构

---

## 目录

- [第一部分：DMA 硬件基础](#第一部分dma-硬件基础)
  - [1.1 DMA 概念与动机](#11-dma-概念与动机)
  - [1.2 ARMv8 DMA 硬件架构](#12-armv8-dma-硬件架构)
  - [1.3 DMA 传输模式](#13-dma-传输模式)
  - [1.4 MMU、IOMMU、SMMU：区别与联系](#14-mmuiommusmmu区别与联系)
  - [1.5 ARM SMMU (System MMU) 详解](#15-arm-smmu-system-mmu-详解)
  - [1.6 Cache 一致性与 DMA](#16-cache-一致性与-dma)
  - [1.7 DMA 地址空间与总线拓扑](#17-dma-地址空间与总线拓扑)
- [第二部分：内核 DMA 软件框架](#第二部分内核-dma-软件框架)
  - [2.1 DMA Mapping API（流式映射）](#21-dma-mapping-api流式映射)
  - [2.2 DMA Engine 框架（DMA 控制器抽象）](#22-dma-engine-框架dma-控制器抽象)
  - [2.3 地址转换流程](#23-地址转换流程)
  - [2.4 三条 DMA 映射路径](#24-三条-dma-映射路径)
  - [2.5 Bounce Buffer (swiotlb) 机制](#25-bounce-buffer-swiotlb-机制)
  - [2.6 CMA 连续内存分配器](#26-cma-连续内存分配器)
  - [2.7 Coherent DMA 内存池](#27-coherent-dma-内存池)
- [第三部分：ARM64 平台特有实现](#第三部分arm64-平台特有实现)
  - [3.1 Cache 维护操作](#31-cache-维护操作)
  - [3.2 DMA Zone 配置](#32-dma-zone-配置)
  - [3.3 ARM SMMUv3 驱动](#33-arm-smmuv3-驱动)
- [第四部分：核心数据结构](#第四部分核心数据结构)
- [第五部分：关键 API 速查](#第五部分关键-api-速查)
- [第六部分：典型使用模式](#第六部分典型使用模式)
- [第七部分：调试与问题排查](#第七部分调试与问题排查)
- [第八部分：内核经典 DMA 映射案例分析](#第八部分内核经典-dma-映射案例分析)
- [第九部分：QEMU ARM64 DMA 实验](#第九部分qemu-arm64-dma-实验)

---

## 第一部分：DMA 硬件基础

### 1.1 DMA 概念与动机

**DMA (Direct Memory Access)** 允许外设直接读写系统内存，无需 CPU 参与每个字节的传输。

```
传统 PIO 模式:                DMA 模式:
CPU ←→ 设备寄存器 ←→ 内存     CPU 发起 → DMA 控制器 → 设备⟷内存
  (每字节都要 CPU 参与)         (CPU 只需配置，传输由 DMA 硬件完成)
```

**核心优势：**
- CPU 从数据搬运中解放，可执行其他计算任务
- 总线利用率更高（突发传输 burst transfer）
- 对大块数据传输（网络、存储、多媒体）性能提升显著

#### 深入理解：DMA 和 CPU 都要访问内存，为什么 DMA 还能节省 CPU？

这是学习 DMA 时最常见的疑问。DMA 控制器和 CPU 确实**共享同一个物理内存**，但 DMA 节省的不是"内存带宽"，而是 **CPU 的指令执行周期**。

**1) PIO 模式下 CPU 做了什么（以从设备读 4KB 数据为例）：**

```
                 CPU 视角（PIO 模式）
                 ─────────────────────
循环 1024 次 (4KB / 4 bytes):
  ① LDR  r0, [设备FIFO寄存器]    // 从设备读一个字 → CPU 流水线停顿等待 I/O
  ② STR  r0, [内存目标地址]       // 写入内存
  ③ ADD  目标地址, #4             // 地址递增
  ④ SUBS 计数器, #1              // 循环控制
  ⑤ BNE  loop

每轮循环：5 条指令 × 1024 = 5120 条指令
更关键的：每次 LDR 设备寄存器要等待 I/O 总线响应
  - DRAM 访问延迟 ~100ns
  - 外设寄存器访问延迟 ~100-1000ns（取决于总线和设备）
  - CPU 在等待期间完全阻塞（或占用一个硬件线程）
```

**2) DMA 模式下 CPU 做了什么：**

```
                 CPU 视角（DMA 模式）
                 ─────────────────────
  ① 配置 DMA 控制器寄存器 (源地址、目标地址、长度、方向)
     → 大约 4~8 次 MMIO 写操作
  ② 启动 DMA 传输
     → 1 次 MMIO 写操作
  ③ CPU 去做其他事情！（运行其他进程/线程）
  ④ DMA 完成后产生中断
  ⑤ 中断处理程序处理完成事件

总共：约 10~20 条指令 + 1 次中断处理
CPU 在传输期间完全自由！
```

**3) 关键区别——总线仲裁与并行：**

```
时间轴对比:

PIO 模式:
CPU: ██████████████████████████████████████  (100% 忙于搬数据)
总线: ─读─写─读─写─读─写─读─写─读─写─读─写  (CPU 驱动的零散访问)
设备:  ↑   ↑   ↑   ↑   ↑   ↑               (被动等 CPU 来取)

DMA 模式:
CPU: ██░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░██  (配置+中断，中间空闲)
     ↑ 配置DMA                        ↑ 中断处理
DMA: ──████████████████████████████████──  (硬件突发传输)
总线: ──BURST──BURST──BURST──BURST──────  (高效突发访问)

CPU 空闲期间可以执行其他任务:
CPU: ██▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓██
     ↑                              ↑
   配置DMA    执行其他计算任务      中断处理
```

**4) 为什么共享同一个内存总线，DMA 还能更高效？**

| 维度 | PIO 模式 | DMA 模式 |
|------|---------|---------|
| **CPU 占用** | 100% 占用，每个字节都要 CPU 参与 | 只需配置 + 中断，<1% 占用 |
| **总线效率** | 每次传输 1 个字（4/8 字节），总线利用率低 | 硬件突发传输（Burst），一次传 16~256 字节 |
| **流水线** | CPU 流水线频繁停顿等 I/O 响应 | DMA 控制器专为数据搬运优化，无流水线浪费 |
| **并行度** | CPU 搬数据期间无法做其他事 | CPU 和 DMA 控制器**真正并行**工作 |
| **中断频率** | 可能每字节/每FIFO半满就中断 | 整块传输完才一次中断 |

**5) ARMv8 互联总线保证了并行访问的可能性：**

```
         ┌─────────┐    ┌──────────┐
         │  CPU    │    │ DMA 控制器│
         │(Master) │    │ (Master)  │
         └────┬────┘    └─────┬────┘
              │               │
         ┌────┴───────────────┴────┐
         │    Interconnect          │  ← CCI/CCN/CMN 支持多个 Master
         │    (总线仲裁器)          │     同时发起访问，交替使用总线
         └────────────┬────────────┘
                      │
              ┌───────┴───────┐
              │  DRAM 控制器   │  ← 现代 DDR 控制器支持多端口/Bank 交叉
              │  (多Bank并行)  │     不同 Bank 可同时服务不同 Master
              └───────────────┘
```

现代 ARM SoC 的互联总线（如 ARM CCI-550、CMN-700）支持**多个 Master 并发访问**：
- CPU 和 DMA 控制器可以交替占用总线时隙
- DDR 控制器的多 Bank 架构允许不同地址区域被并行访问
- 即使总线带宽被分享，CPU 省下的**指令周期**远比偶尔的总线竞争代价大

> **总结**：DMA 节省的核心不是内存带宽，而是 **CPU 指令执行时间** 和 **CPU 流水线效率**。CPU 是通用处理器，每搬一个字都要执行取指-译码-执行-访存-写回完整流水线；DMA 控制器是**专用硬件**，只做数据搬运这一件事，效率远高于通用 CPU。更重要的是，DMA 传输期间 CPU **完全释放**，可以执行计算密集型任务，实现真正的 I/O 与计算并行。

### 1.2 ARMv8 DMA 硬件架构

ARMv8 平台的 DMA 硬件通常包含以下层次：

```
┌─────────────────────────────────────────────────────────────┐
│                    CPU Cluster                               │
│  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐                       │
│  │Core 0│ │Core 1│ │Core 2│ │Core 3│                       │
│  │L1 D$ │ │L1 D$ │ │L1 D$ │ │L1 D$ │                       │
│  └──┬───┘ └──┬───┘ └──┬───┘ └──┬───┘                       │
│     └────┬───┘        └────┬───┘                            │
│      ┌───┴───┐         ┌───┴───┐                            │
│      │  L2   │         │  L2   │                            │
│      └───┬───┘         └───┬───┘                            │
│          └────────┬────────┘                                │
│              ┌────┴────┐                                    │
│              │   L3/   │  ← PoC (Point of Coherency)       │
│              │  System │                                    │
│              │  Cache  │                                    │
│              └────┬────┘                                    │
└───────────────────┼─────────────────────────────────────────┘
                    │
            ┌───────┴───────┐
            │  Interconnect │  (CCI/CCN/CMN)
            │  (Coherent    │
            │   Bus Fabric) │
            └─┬──────┬────┬─┘
              │      │    │
     ┌────────┴┐  ┌──┴──┐ ┌───┴────┐
     │  DRAM   │  │SMMU │ │  GIC   │
     │Controller│  │(IOMMU)│ │       │
     └─────────┘  └──┬──┘ └────────┘
                     │
              ┌──────┴──────┐
              │  I/O 总线   │
              │  (PCIe/AXI) │
              └─┬─────┬────┬┘
           ┌────┴┐ ┌──┴──┐ ┌┴────┐
           │NIC  │ │eMMC │ │GPU  │
           │(网卡)│ │控制器│ │     │
           └─────┘ └─────┘ └─────┘
```

**关键硬件组件：**

| 组件 | 功能 | 与 DMA 的关系 |
|------|------|--------------|
| **DMA 控制器** | 管理 DMA 通道，执行数据传输 | 核心硬件，驱动设备与内存间数据搬运 |
| **SMMU (IOMMU)** | I/O 地址翻译与隔离 | 将设备 DMA 地址映射到物理地址 |
| **Interconnect** | 总线互联（CCI-400/500, CMN-600） | 保证 I/O coherency |
| **Cache 层次** | L1/L2/L3 缓存 | DMA 需处理缓存一致性问题 |
| **GIC** | 中断控制器 | DMA 完成后通过中断通知 CPU |

### 1.3 DMA 传输模式

DMA 控制器支持多种传输模式，适用于不同的硬件场景。理解这些模式是正确使用 DMA Engine API 的基础。

#### 四种基本传输模式

```
┌────────────────────────────────────────────────────────────────────┐
│                    DMA 传输模式总览                                  │
├──────────┬─────────────────────────────────────────────────────────┤
│ 单次传输  │  ■ → ■                    传一次就停                     │
│ (Single) │  CPU 发起，DMA 传一个数据单元，完成后停止                  │
├──────────┼─────────────────────────────────────────────────────────┤
│ 块传输    │  ■■■■■■■■ → ■■■■■■■■     一次传一整块                   │
│ (Block)  │  CPU 发起，DMA 连续传输 N 个数据单元，全部完成后中断通知    │
├──────────┼─────────────────────────────────────────────────────────┤
│ 需求传输  │  ■..■..■■..■ → ■..■..■■..■  外设按需请求                │
│ (Demand) │  外设每准备好数据就发 DRQ 信号，DMA 传一个/一组数据单元     │
├──────────┼─────────────────────────────────────────────────────────┤
│ 循环传输  │  ■■■■■■■■ → ■■■■■■■■ → ■■■■■■■■ → ...  无限循环       │
│ (Cyclic) │  传输完一个 buffer 后自动回到起点继续，直到软件主动停止     │
└──────────┴─────────────────────────────────────────────────────────┘
```

#### 1) 单次传输 (Single Transfer)

```
CPU 配置 DMA                     DMA 执行
┌──────────┐                    ┌───────────────┐
│ 源地址    │    ──触发──▶       │ 读 1 个数据单元│
│ 目的地址  │                    │ 写 1 个数据单元│
│ 传 1 次   │                    │ 完成 → 停止   │
└──────────┘                    └───────────────┘
```

- **特点**：每次只传输一个数据单元（1/2/4/8 字节），传完即停
- **触发方式**：软件触发或硬件 DRQ 信号
- **应用场景**：极少单独使用，通常用于低速外设的单字传输
- **内核对应**：无直接 API，通常封装在 block 传输中（burst size = 1）

#### 2) 块传输 (Block / Burst Transfer)

```
CPU 配置 DMA                     DMA 执行
┌──────────┐                    ┌───────────────────────────────────┐
│ 源地址    │    ──触发──▶       │ 连续传输 N 个数据单元               │
│ 目的地址  │                    │ ■■■■■■■■ (burst)                  │
│ 长度 = N  │                    │ 全部完成后：                       │
│ burst=16  │                    │  - 产生完成中断                    │
└──────────┘                    │  - 停止 DMA 通道                   │
                                └───────────────────────────────────┘
```

- **特点**：一次性连续传输一整块数据，传输期间 DMA 独占总线（或按仲裁策略交替）
- **Burst Size**：每次总线事务传输的数据单元数（如 4/8/16），由硬件和总线宽度决定
- **应用场景**：内存到内存拷贝（memcpy 加速）、大块数据搬运
- **内核对应**：`dmaengine_prep_dma_memcpy()`

```c
// 内核 DMA Engine API — 块传输（memcpy）
struct dma_async_tx_descriptor *desc;
desc = dmaengine_prep_dma_memcpy(chan,
    dst_dma_addr,     // 目的 DMA 地址
    src_dma_addr,     // 源 DMA 地址
    len,              // 传输长度（字节）
    DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
```

#### 3) 需求传输 (Demand / Slave Transfer)

```
外设 FIFO                    DMA 控制器                    内存
┌────────┐                  ┌────────────┐               ┌────────┐
│ 数据   │──DRQ信号──▶      │            │               │        │
│ 就绪   │                  │ 检测到 DRQ  │               │        │
│        │◀──────────────── │ 读取 burst │──────────────▶│ 写入   │
│        │   读取数据        │ 个数据单元  │   写入数据     │ 数据   │
│  ...   │                  │            │               │        │
│ 数据   │──DRQ信号──▶      │ 又检测到DRQ │               │        │
│ 就绪   │                  │ 再读 burst │──────────────▶│ 写入   │
└────────┘                  └────────────┘               └────────┘
                            总计达到 len 后：完成中断
```

- **特点**：由外设硬件信号 (DRQ/DREQ) 控制传输节奏，外设 FIFO 准备好了才传
- **流控**：硬件流控（device_fc）——设备控制何时传输，避免 FIFO 溢出/欠载
- **应用场景**：UART/SPI/I2C 等慢速外设数据收发（最常见的 DMA 模式）
- **内核对应**：`dmaengine_prep_slave_sg()` / `dmaengine_prep_slave_single()`

```c
// 配置从设备参数
struct dma_slave_config cfg = {
    .direction    = DMA_DEV_TO_MEM,
    .src_addr     = uart_phys + UART_RBR,   // 外设 FIFO 寄存器物理地址
    .src_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE,
    .src_maxburst = 16,                      // 每次 DRQ 触发传输 16 字节
    .device_fc    = false,                   // DMA 控制器管理流控
};
dmaengine_slave_config(chan, &cfg);

// 准备从设备 SG 传输描述符
desc = dmaengine_prep_slave_sg(chan,
    sg_list, sg_len,          // scatter-gather 列表
    DMA_DEV_TO_MEM,           // 设备→内存
    DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
```

**DRQ 信号与 burst 的关系：**
```
FIFO 半满 → 拉高 DRQ → DMA 传输 maxburst 个数据 → DRQ 拉低
         → 设备继续接收数据 → FIFO 又半满 → 再拉高 DRQ → ...

maxburst = 16, buswidth = 1 byte:
  每次 DRQ 脉冲 → DMA 传输 16 字节
  直到总长度 len 达到 → 完成中断
```

#### 4) 循环传输 (Cyclic Transfer)

```
      缓冲区（环形）
    ┌─────────────────────────────────┐
    │ Period 0 │ Period 1 │ Period 2  │
    │  ■■■■■■  │  ■■■■■■  │  ■■■■■■  │
    └─────┬────┴────┬─────┴────┬─────┘
          │         │          │
          ▼         ▼          ▼
     中断回调    中断回调    中断回调
     (半满)     (全满)     (又半满)
          │                    │
          └───── 自动回绕 ─────┘
           DMA 地址重新指向 Period 0
```

- **特点**：传输完最后一个 period 后自动回到缓冲区起始地址继续，永不停止
- **Period**：缓冲区被分为 N 个等长段，每完成一个 period 产生一次中断
- **双缓冲思想**：CPU 处理已完成 period 的数据时，DMA 继续写下一个 period
- **应用场景**：音频流（ALSA PCM）、视频帧采集、持续数据采集
- **内核对应**：`dmaengine_prep_dma_cyclic()`

```c
// 循环 DMA — 音频播放典型用法
desc = dmaengine_prep_dma_cyclic(chan,
    buf_dma_addr,            // 环形缓冲区 DMA 地址
    buf_len,                 // 缓冲区总大小（如 64KB）
    period_len,              // 每个 period 大小（如 16KB）
    DMA_MEM_TO_DEV,          // 内存→I2S 控制器
    DMA_PREP_INTERRUPT);     // 每个 period 完成触发回调

// 缓冲区被分为 buf_len / period_len = 4 个 period
// DMA 完成 Period 0 → 中断 → 回调（应用填充 Period 0 新数据）
// DMA 继续 Period 1 → 中断 → 回调（应用填充 Period 1 新数据）
// ...
// DMA 完成 Period 3 → 自动回到 Period 0 → 无限循环
```

#### 四种模式对比总结

| 模式 | 触发方式 | 传输次数 | 中断频率 | 典型场景 | DMA Engine API |
|------|---------|---------|---------|---------|---------------|
| **单次** | 软件/DRQ | 1 次 | 每次 | 极少用 | — |
| **块传输** | 软件触发 | 1 块 | 完成时 1 次 | memcpy, 大块搬运 | `prep_dma_memcpy()` |
| **需求传输** | 外设 DRQ | 按需多次 | 完成时 1 次 | UART/SPI/I2C | `prep_slave_sg()` |
| **循环传输** | 外设 DRQ | 无限循环 | 每 period | 音频/视频流 | `prep_dma_cyclic()` |

### 1.4 MMU、IOMMU、SMMU：区别与联系

这三个名字经常混用，但它们分别工作在不同层面。一句话概括：**MMU 服务于 CPU，IOMMU 服务于设备，SMMU 是 ARM 对 IOMMU 的具体实现**。

#### 三者对比总览

| 维度 | MMU | IOMMU | SMMU |
|------|-----|-------|------|
| **全称** | Memory Management Unit | I/O Memory Management Unit | System Memory Management Unit |
| **服务对象** | CPU 核心 | I/O 设备（通用概念） | I/O 设备（ARM 实现） |
| **位置** | CPU 内部，每个核心一个 | 总线上，设备与内存之间 | ARM SoC 总线上 |
| **翻译什么** | CPU 虚拟地址 → 物理地址 | 设备 DMA 地址 → 物理地址 | 设备 DMA 地址 → 物理地址 |
| **页表由谁管理** | 内核 mm 子系统 | 内核 IOMMU 子系统 | 内核 ARM SMMU 驱动 |
| **页表格式** | ARMv8 TTBR 页表 | 平台相关 | ARMv8 兼容格式（可共享 CPU 页表） |
| **TLB** | CPU 内 TLB | IOTLB（I/O 侧 TLB） | IOTLB |
| **典型硬件** | ARM Cortex-A 系列内置 | Intel VT-d, AMD-Vi, ARM SMMU | ARM SMMUv2, SMMUv3 |
| **内核代码** | `arch/arm64/mm/` | `drivers/iommu/` (框架) | `drivers/iommu/arm/arm-smmu-v3/` |

#### 硬件位置关系

```
┌─────────────────────────────────────────────────────────────────────┐
│  CPU Core                                                           │
│  ┌──────────────────────────────────┐                               │
│  │  应用程序使用虚拟地址 (VA)        │                               │
│  │  例: 0x00007fff_12340000         │                               │
│  └──────────┬───────────────────────┘                               │
│             │                                                       │
│       ┌─────┴─────┐                                                 │
│       │  ★ MMU ★  │  ← CPU 内部硬件，翻译 CPU 发出的每一次内存访问   │
│       │  VA → PA   │     通过 TTBR0/TTBR1 寄存器指向的页表            │
│       └─────┬─────┘                                                 │
│             │ 物理地址 (PA)                                          │
└─────────────┼───────────────────────────────────────────────────────┘
              │
        ┌─────┴──────────────────────────────┐
        │        Interconnect (总线)          │
        └─────┬────────────────┬─────────────┘
              │                │
              │          ┌─────┴──────┐
              │          │ ★ SMMU ★   │  ← 总线上的独立硬件单元
              │          │ (ARM IOMMU)│     翻译设备发出的每一次 DMA 访问
              │          │ DA → PA    │     通过 Stream Table + CD 指向的 I/O 页表
              │          └─────┬──────┘
              │                │
        ┌─────┴──┐     ┌──────┴──────┐
        │  DRAM  │     │  I/O 设备    │
        │  内存  │     │  (NIC/GPU/   │
        │        │     │   eMMC...)   │
        └────────┘     └─────────────┘

★ MMU  = 翻译 CPU 的地址，在 CPU 内部
★ SMMU = 翻译设备的地址，在总线上（是 IOMMU 的 ARM 实现）
```

#### 概念层次关系

```
IOMMU（通用概念/内核框架）
  │
  ├── Intel VT-d          ← x86 平台的 IOMMU 实现
  ├── AMD-Vi (AMD IOMMU)  ← AMD 平台的 IOMMU 实现
  ├── ARM SMMU            ← ARM 平台的 IOMMU 实现
  │     ├── SMMUv1        (已淘汰)
  │     ├── SMMUv2        (ARMv7/v8 早期)
  │     └── SMMUv3        (ARMv8 主流，支持 Stage1+2, ATS, PRI, SVA)
  └── RISC-V IOMMU        ← RISC-V 平台的 IOMMU 实现
```

**Linux 内核的抽象层次：**
- `drivers/iommu/iommu.c` — **IOMMU 通用框架**，定义 `struct iommu_ops` 接口
- `drivers/iommu/dma-iommu.c` — IOMMU 与 DMA 子系统的**集成层**
- `drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c` — ARM **SMMUv3 具体驱动**，实现 `iommu_ops`

#### 为什么需要 IOMMU/SMMU？MMU 不够用吗？

MMU 只翻译 **CPU 发出的地址**，但设备做 DMA 时绕过了 CPU，直接在总线上发地址访问内存。没有 IOMMU 的情况：

```
危险场景（无 IOMMU）:
─────────────────────
                           物理内存
设备 DMA 发出地址 0x80000000 ──────▶ 直接访问！
                                     没有任何检查！

问题1: 恶意/有缺陷的设备可以读写任意物理内存 → 安全漏洞
问题2: 设备必须使用物理地址，需要物理连续内存 → 内存管理困难
问题3: 虚拟机中的设备直通(passthrough)无法安全隔离 → 虚拟化受限
```

```
安全场景（有 IOMMU/SMMU）:
─────────────────────────
                                    SMMU I/O 页表
设备 DMA 发出地址 0x1000_0000 ──▶ [翻译+权限检查] ──▶ 物理内存 0x8000_0000
                                    │
                                    ├─ 地址翻译：设备地址 → 真实物理地址
                                    ├─ 权限检查：只能访问分配给它的内存区域
                                    ├─ 地址聚合：多段不连续物理内存 → 设备看到连续地址
                                    └─ 隔离保护：不同设备/VM 互不干扰
```

#### MMU 与 SMMU 的页表对比

| 特性 | MMU 页表 | SMMU 页表 |
|------|---------|----------|
| **基地址寄存器** | `TTBR0_EL1` / `TTBR1_EL1` | Stream Table → CD → TTB |
| **查找键** | 虚拟地址 (VA) | StreamID + 设备地址 |
| **多级查找** | TTBR → L0 → L1 → L2 → L3 | STE → CD → L0 → L1 → L2 → L3 |
| **两阶段翻译** | 不涉及（EL2 有 Stage 2） | Stage 1 (VA→IPA) + Stage 2 (IPA→PA) |
| **ASID** | 区分不同进程 | SubstreamID（SVA 场景共享进程地址空间） |
| **VMID** | 区分不同虚拟机（EL2） | 区分不同虚拟机 |
| **页表格式** | ARMv8 Long Descriptor | **兼容 ARMv8 格式**（可与 CPU 共享页表！） |
| **TLB 失效** | `TLBI` 指令 | SMMU Command Queue: `CFGI`/`TLBI` 命令 |

> **重要特性**：SMMUv3 使用与 CPU MMU **相同格式**的页表，这意味着在 SVA（Shared Virtual Addressing）场景下，设备可以直接使用进程的页表，设备和 CPU 看到同一个虚拟地址空间。

#### 三者协作全景

```
用户进程调用 write(fd, buf, 4096):
  │
  ▼
CPU 执行系统调用，buf 是用户虚拟地址 0x7fff_0000
  │
  │ ★ CPU MMU 翻译 ★
  │ VA 0x7fff_0000 → PA 0x8000_0000 (通过进程页表)
  ▼
内核驱动调用 dma_map_single(dev, kernel_buf, 4096, DMA_TO_DEVICE)
  │
  │ 内核 DMA 子系统处理：
  │ ├─ 如果有 SMMU: 在 I/O 页表建立映射，返回 IOVA
  │ ├─ 如果无 SMMU: phys_to_dma() 直接转换，返回 DMA 地址
  │ └─ Cache 维护: dcache_clean_poc()
  ▼
设备收到 DMA 地址，发起 DMA 读操作
  │
  │ ★ SMMU 翻译（如果有）★
  │ IOVA 0x1000_0000 → PA 0x8000_0000 (通过 I/O 页表)
  ▼
设备读到物理内存中的数据，传输完成，触发中断
```

### 1.5 ARM SMMU (System MMU) 详解

ARM SMMU（System Memory Management Unit）是 ARM 架构的 IOMMU 实现，在内核中对应 `drivers/iommu/arm/arm-smmu-v3/` 目录。

**SMMUv3 关键寄存器（源码 `arm-smmu-v3.h`）：**

```c
/* 身份寄存器 */
ARM_SMMU_IDR0    (0x00)  // 能力寄存器：Stage1/2支持、ATS、VMID位宽等
ARM_SMMU_IDR1    (0x04)  // 队列大小、SID/SSID位宽
ARM_SMMU_IDR5    (0x14)  // 页粒度支持(4K/16K/64K)、输出地址宽度(OAS)

/* 控制寄存器 */
ARM_SMMU_CR0     (0x20)  // 使能控制：SMMUEN, CMDQEN, EVTQEN, PRIQEN
ARM_SMMU_CR1     (0x28)  // 缓存属性配置（表/队列的 Inner/Outer Cache）
ARM_SMMU_CR2     (0x2C)  // PTM, E2H 等特性控制

/* 流表基地址 */
ARM_SMMU_STRTAB_BASE      (0x80)   // 流表(Stream Table)基地址
ARM_SMMU_STRTAB_BASE_CFG  (0x88)   // 流表格式（线性/两级）

/* 中断与错误 */
ARM_SMMU_GERROR  (0x60)  // 全局错误寄存器
ARM_SMMU_IRQ_CTRL(0x50)  // 中断使能
```

**SMMUv3 工作流程：**

```
设备发起 DMA 请求 (携带 StreamID)
    │
    ▼
┌──────────────────────────────────┐
│  SMMU Stream Table 查找          │
│  StreamID → Stream Table Entry   │
│  (STE 包含 Context Descriptor)   │
└──────────┬───────────────────────┘
           │
           ▼
┌──────────────────────────────────┐
│  Stage 1 翻译 (可选)             │
│  设备虚拟地址(VA) → 中间物理地址 │
│  (IPA)，使用 CD 中的页表基地址    │
└──────────┬───────────────────────┘
           │
           ▼
┌──────────────────────────────────┐
│  Stage 2 翻译 (可选)             │
│  IPA → 物理地址(PA)              │
│  用于虚拟化场景                   │
└──────────┬───────────────────────┘
           │
           ▼
    访问 DRAM 物理内存
```

**SMMUv3 命令队列与事件队列：**
- **Command Queue (CMDQ)**：CPU 向 SMMU 发命令（TLB invalidate、配置更新等）
- **Event Queue (EVTQ)**：SMMU 向 CPU 报告事件（翻译错误、权限错误等）
- **PRI Queue (PRIQ)**：Page Request Interface，设备请求缺页处理

### 1.6 Cache 一致性与 DMA

这是 ARM64 DMA 编程中**最核心、最易出错**的知识点。

#### 问题根源

```
   CPU 写数据到内存:
   CPU → L1 Cache → L2 Cache → (可能还在Cache中，未写回DRAM)

   设备通过 DMA 读内存:
   设备 → 总线 → DRAM  (读到的可能是旧数据！)

   设备通过 DMA 写内存:
   设备 → 总线 → DRAM → (CPU Cache 中还有旧数据！)
```

#### ARMv8 的两种一致性模型

| 模型 | 说明 | Cache 维护 | 性能 |
|------|------|-----------|------|
| **Hardware Coherent** | 硬件自动保证 I/O 与 CPU Cache 一致 | 不需要 | 高 |
| **Non-Coherent** | 软件必须手动做 Cache 维护操作 | 必须 | 取决于维护开销 |

**ARM64 内核实现（`arch/arm64/mm/dma-mapping.c`）：**

```c
// 设备访问前：将 CPU Cache 数据刷到内存（PoC）
void arch_sync_dma_for_device(phys_addr_t paddr, size_t size,
                              enum dma_data_direction dir)
{
    unsigned long start = (unsigned long)phys_to_virt(paddr);
    dcache_clean_poc(start, start + size);  // Clean: Cache → Memory
}

// CPU 访问前：使 Cache 无效化，迫使从内存重读
void arch_sync_dma_for_cpu(phys_addr_t paddr, size_t size,
                           enum dma_data_direction dir)
{
    unsigned long start = (unsigned long)phys_to_virt(paddr);
    if (dir == DMA_TO_DEVICE)
        return;  // CPU→设备方向，CPU不需要读新数据
    dcache_inval_poc(start, start + size);  // Invalidate: 丢弃Cache
}

// 分配 Coherent 内存前：先清理 Cache
void arch_dma_prep_coherent(struct page *page, size_t size)
{
    unsigned long start = (unsigned long)page_address(page);
    dcache_clean_poc(start, start + size);
}
```

#### PoC（Point of Coherency）概念

```
          L1 Cache
              │
          L2 Cache
              │
     ┌────────┴────────┐
     │   PoC (L3/LLC)  │  ← 所有 observer（CPU + I/O）都能看到一致数据的点
     └────────┬────────┘
              │
           DRAM
```

- **`dcache_clean_poc()`**：将 Cache 行清洗到 PoC，确保设备能看到最新数据
- **`dcache_inval_poc()`**：使 Cache 行无效，CPU 下次访问从 PoC 重新获取
- **`dcache_clean_inval_poc()`**：先清洗再无效（双向传输场景）

#### DMA 方向与 Cache 操作对应关系

| DMA 方向 | 含义 | 传输前操作 | 传输后操作 |
|----------|------|-----------|-----------|
| `DMA_TO_DEVICE` | CPU→设备 | Clean（确保设备读到新数据） | 无需操作 |
| `DMA_FROM_DEVICE` | 设备→CPU | 无需操作（或 Invalidate） | Invalidate（丢弃旧 Cache） |
| `DMA_BIDIRECTIONAL` | 双向 | Clean + Invalidate | Invalidate |

### 1.7 DMA 地址空间与总线拓扑

ARMv8 系统中存在**多个地址空间**：

```
┌──────────────────────────────────────────────────┐
│  CPU 虚拟地址 (VA)                                │
│  例: 0xffff800000000000 (内核线性映射区)           │
│       │                                          │
│       │ MMU / 页表翻译                            │
│       ▼                                          │
│  CPU 物理地址 (PA)                                │
│  例: 0x0000000080000000                          │
│       │                                          │
│       │ phys_to_dma() / bus_dma_region 映射       │
│       ▼                                          │
│  DMA/总线地址                                     │
│  (可能等于PA，也可能有偏移)                         │
│       │                                          │
│       │ 可选: SMMU/IOMMU 翻译                     │
│       ▼                                          │
│  设备看到的最终物理地址                             │
└──────────────────────────────────────────────────┘
```

**`bus_dma_region` 结构体（描述 CPU→DMA 地址映射规则）：**

```c
struct bus_dma_region {
    phys_addr_t cpu_start;    // CPU 物理地址起始
    dma_addr_t  dma_start;    // 对应的 DMA 地址起始
    u64         size;         // 区域大小
};
```

---

## 第二部分：内核 DMA 软件框架

Linux 内核的 DMA 子系统由两个互补的框架组成：

```
┌─────────────────────────────────────────────────────┐
│                   驱动程序                            │
├───────────────────────┬─────────────────────────────┤
│  DMA Mapping API      │  DMA Engine API              │
│  (地址映射/缓存同步)  │  (DMA 控制器通道管理)          │
│  kernel/dma/          │  drivers/dma/                │
├───────────────────────┴─────────────────────────────┤
│                     平台层                            │
│  arch/arm64/mm/dma-mapping.c  +  IOMMU/SMMU         │
├─────────────────────────────────────────────────────┤
│                   DMA 硬件                            │
└─────────────────────────────────────────────────────┘
```

### 2.1 DMA Mapping API（流式映射）

这是驱动开发者最常用的 API 层，定义在 `include/linux/dma-mapping.h`。

#### 核心数据方向

```c
// include/linux/dma-direction.h
enum dma_data_direction {
    DMA_BIDIRECTIONAL = 0,   // 双向传输
    DMA_TO_DEVICE     = 1,   // 内存→设备（如发送网络包）
    DMA_FROM_DEVICE   = 2,   // 设备→内存（如接收网络包）
    DMA_NONE          = 3,   // 仅用于调试校验
};
```

#### DMA 属性标志

```c
// include/linux/dma-mapping.h
#define DMA_ATTR_WEAK_ORDERING       (1UL << 1)  // 允许弱序访问
#define DMA_ATTR_WRITE_COMBINE       (1UL << 2)  // 写合并，提升写性能
#define DMA_ATTR_NO_KERNEL_MAPPING   (1UL << 4)  // 不建立内核虚拟映射
#define DMA_ATTR_SKIP_CPU_SYNC       (1UL << 5)  // 跳过 Cache 同步
#define DMA_ATTR_FORCE_CONTIGUOUS    (1UL << 6)  // 强制物理连续
#define DMA_ATTR_ALLOC_SINGLE_PAGES  (1UL << 7)  // 提示：不必追求 TLB 效率
#define DMA_ATTR_NO_WARN             (1UL << 8)  // 抑制分配失败警告
#define DMA_ATTR_PRIVILEGED          (1UL << 9)  // 特权级访问
#define DMA_ATTR_MMIO                (1UL << 10) // MMIO 区域（P2P DMA）
```

#### `dma_map_ops` 操作表

内核通过 `dma_map_ops` 结构体抽象不同平台的 DMA 操作（`include/linux/dma-map-ops.h`）：

```c
struct dma_map_ops {
    /* 一致性内存分配/释放 */
    void *(*alloc)(struct device *dev, size_t size,
                   dma_addr_t *dma_handle, gfp_t gfp, unsigned long attrs);
    void  (*free)(struct device *dev, size_t size, void *vaddr,
                  dma_addr_t dma_handle, unsigned long attrs);

    /* 单页映射/解映射 */
    dma_addr_t (*map_page)(struct device *dev, struct page *page,
                           unsigned long offset, size_t size,
                           enum dma_data_direction dir, unsigned long attrs);
    void (*unmap_page)(struct device *dev, dma_addr_t dma_handle,
                       size_t size, enum dma_data_direction dir,
                       unsigned long attrs);

    /* scatter-gather 映射/解映射 */
    int  (*map_sg)(struct device *dev, struct scatterlist *sg, int nents,
                   enum dma_data_direction dir, unsigned long attrs);
    void (*unmap_sg)(struct device *dev, struct scatterlist *sg, int nents,
                     enum dma_data_direction dir, unsigned long attrs);

    /* MMIO 资源映射 (P2P DMA) */
    dma_addr_t (*map_resource)(struct device *dev, phys_addr_t phys_addr,
                               size_t size, enum dma_data_direction dir,
                               unsigned long attrs);

    /* Cache 同步 */
    void (*sync_single_for_cpu)(struct device *dev, dma_addr_t dma_handle,
                                size_t size, enum dma_data_direction dir);
    void (*sync_single_for_device)(struct device *dev, dma_addr_t dma_handle,
                                   size_t size, enum dma_data_direction dir);

    /* 能力查询 */
    int  (*dma_supported)(struct device *dev, u64 mask);
    u64  (*get_required_mask)(struct device *dev);
    size_t (*max_mapping_size)(struct device *dev);
};
```

**操作表选择逻辑（`kernel/dma/mapping.c`）：**

```c
// 三级路由：直接映射 > IOMMU映射 > 自定义ops
static inline bool dma_map_direct(struct device *dev, const struct dma_map_ops *ops)
{
    return dma_go_direct(dev, *dev->dma_mask, ops);
}

// 核心映射函数的路由逻辑：
dma_addr_t dma_map_phys(struct device *dev, phys_addr_t phys, ...)
{
    if (dma_map_direct(dev, ops))
        addr = dma_direct_map_phys(dev, phys, ...);      // 直接路径
    else if (use_dma_iommu(dev))
        addr = iommu_dma_map_phys(dev, phys, ...);       // IOMMU路径
    else
        addr = ops->map_page(dev, page, offset, ...);    // 自定义ops
}
```

### 2.2 DMA Engine 框架（DMA 控制器抽象）

DMA Engine 框架管理 DMA 控制器硬件，位于 `drivers/dma/dmaengine.c`。

#### 核心数据结构关系

```
┌──────────────┐
│  dma_device  │ ← DMA 控制器（如 PL330）
│  .channels   │
│  .cap_mask   │ ← 支持的操作类型
│  .device_prep_dma_memcpy()  │ ← 准备描述符的回调
│  .device_issue_pending()    │ ← 触发硬件传输
│  .device_tx_status()        │ ← 查询完成状态
└──────┬───────┘
       │ 1:N
       ▼
┌──────────────┐
│  dma_chan     │ ← DMA 通道
│  .device      │ ← 所属控制器
│  .cookie      │ ← 最新提交的 cookie
│  .completed_cookie │ ← 最新完成的 cookie
│  .client_count│ ← 引用计数
└──────┬───────┘
       │ N:M
       ▼
┌──────────────────────┐
│ dma_async_tx_descriptor │ ← DMA 传输描述符
│  .cookie              │ ← 传输标识
│  .tx_submit()         │ ← 提交到通道
│  .callback()          │ ← 完成回调函数
│  .flags               │ ← DMA_PREP_INTERRUPT 等
└───────────────────────┘
```

#### DMA 事务类型

```c
enum dma_transaction_type {
    DMA_MEMCPY,       // 内存到内存拷贝（最常用）
    DMA_XOR,          // XOR 运算（RAID5）
    DMA_PQ,           // P+Q 运算（RAID6）
    DMA_MEMSET,       // 内存填充
    DMA_SLAVE,        // 从设备传输（外设⟷内存）
    DMA_CYCLIC,       // 循环缓冲区传输（音频/视频）
    DMA_INTERLEAVE,   // 交错传输（2D DMA）
};
```

#### DMA 传输方向

```c
enum dma_transfer_direction {
    DMA_MEM_TO_MEM,   // 内存→内存（memcpy加速）
    DMA_MEM_TO_DEV,   // 内存→设备（如UART发送）
    DMA_DEV_TO_MEM,   // 设备→内存（如UART接收）
    DMA_DEV_TO_DEV,   // 设备→设备（较少使用）
};
```

#### DMA Engine 完整工作流

```
                          ┌──────────────────┐
                          │ 1. 请求 DMA 通道  │
                          │ dma_request_chan() │
                          └────────┬─────────┘
                                   │
                          ┌────────▼─────────┐
                          │ 2. 配置从设备参数  │
                          │ dmaengine_slave_  │
                          │ config()          │
                          └────────┬─────────┘
                                   │
                          ┌────────▼─────────┐
                          │ 3. 准备传输描述符  │
                          │ dmaengine_prep_   │
                          │ slave_sg()        │
                          └────────┬─────────┘
                                   │
                          ┌────────▼─────────┐
                          │ 4. 设置完成回调   │
                          │ desc->callback    │
                          └────────┬─────────┘
                                   │
                          ┌────────▼─────────┐
                          │ 5. 提交描述符     │
                          │ dmaengine_submit()│
                          │ → 返回 cookie     │
                          └────────┬─────────┘
                                   │
                          ┌────────▼─────────┐
                          │ 6. 触发传输       │
                          │ dma_async_issue_  │
                          │ pending()         │
                          └────────┬─────────┘
                                   │
                          ┌────────▼─────────┐
                          │ 7. 等待完成       │
                          │ (回调/轮询/等待)  │
                          └────────┬─────────┘
                                   │
                          ┌────────▼─────────┐
                          │ 8. 释放通道       │
                          │ dma_release_      │
                          │ channel()         │
                          └──────────────────┘
```

### 2.3 地址转换流程

Linux 内核 DMA 子系统的核心任务之一是**地址空间转换**：

```
┌───────────────────────────────────────────────────────────┐
│ 内核虚拟地址 (Kernel VA)                                   │
│ 例: 0xffff000000000000 + offset                           │
└────────────┬──────────────────────────────────────────────┘
             │ virt_to_phys() 或页表遍历
             ▼
┌───────────────────────────────────────────────────────────┐
│ CPU 物理地址 (PA)                                         │
│ 例: 0x80000000                                           │
└────────────┬──────────────────────────────────────────────┘
             │ phys_to_dma(dev, pa)
             │   ├─ 检查内存加密 (AMD SME/SEV)
             │   └─ 查询 bus_dma_region 映射表
             ▼
┌───────────────────────────────────────────────────────────┐
│ DMA/总线地址 (dma_addr_t)                                 │
│ (可能等于PA，也可能有偏移或加密位)                           │
└────────────┬──────────────────────────────────────────────┘
             │
             ├── [直接路径] 设备直接使用此地址访问内存
             │
             ├── [IOMMU路径] SMMU 将 DMA 地址通过 I/O 页表
             │   翻译为物理地址
             │
             └── [Bounce路径] 数据被拷贝到 swiotlb 区域，
                 设备访问 bounce buffer 的地址
```

**关键函数（`kernel/dma/direct.c`）：**

```c
static inline dma_addr_t phys_to_dma_direct(struct device *dev,
                                             phys_addr_t phys)
{
    if (force_dma_unencrypted(dev))
        return phys_to_dma_unencrypted(dev, phys);  // 加密平台
    return phys_to_dma(dev, phys);                   // 常规转换
}

// 检查设备 DMA 能力
bool dma_coherent_ok(struct device *dev, phys_addr_t phys, size_t size)
{
    dma_addr_t dma_addr = phys_to_dma_direct(dev, phys);
    if (dma_addr == DMA_MAPPING_ERROR)
        return false;
    return dma_addr + size - 1 <=
        min_not_zero(dev->coherent_dma_mask, dev->bus_dma_limit);
}
```

### 2.4 三条 DMA 映射路径

#### 路径一：直接映射 (Direct DMA)

```
CPU PA ──phys_to_dma()──▶ DMA Address (设备直接访问)
```

- **条件**：设备 DMA mask 覆盖所有物理内存，无需 IOMMU
- **优点**：零开销，最快路径
- **缺点**：无地址空间隔离，需物理连续内存

```c
// kernel/dma/direct.c - 直接映射的 map_sg 实现
int dma_direct_map_sg(struct device *dev, struct scatterlist *sgl,
                      int nents, enum dma_data_direction dir,
                      unsigned long attrs)
{
    for_each_sg(sgl, sg, nents, i) {
        sg->dma_address = dma_direct_map_phys(dev, sg_phys(sg),
                                               sg->length, dir, attrs);
        // ...
    }
    return nents;
}
```

#### 路径二：IOMMU 映射

```
CPU PA ──▶ IOMMU 分配 IOVA ──▶ 建立 I/O 页表 ──▶ IOVA (设备使用)
```

- **条件**：设备关联了 IOMMU（如 ARM SMMU）
- **优点**：地址空间隔离、支持非连续物理内存聚合、安全性高
- **缺点**：有 TLB miss 开销、需管理 IOVA 空间

```c
// kernel/dma/mapping.c - 路由到 IOMMU
if (use_dma_iommu(dev))
    addr = iommu_dma_map_phys(dev, phys, size, dir, attrs);
```

**IOMMU DMA 的延迟 TLB 刷新优化：**
- 使用 per-CPU flush queue 批量处理 IOTLB 失效
- 定时器触发（默认 10ms）减少频繁刷新开销
- 原子计数器同步 `fq_flush_start_cnt` / `fq_flush_finish_cnt`

#### 路径三：Bounce Buffer (swiotlb)

```
数据: 高地址内存 ──memcpy──▶ swiotlb bounce buffer (低地址) ──▶ 设备访问
```

- **条件**：设备 DMA mask 不够大，无法访问目标内存
- **优点**：兼容性强，任何设备都能工作
- **缺点**：额外内存拷贝，性能开销大

### 2.5 Bounce Buffer (swiotlb) 机制

**swiotlb（Software I/O TLB）** 是 DMA 的最后防线，当设备无法直接访问目标内存时自动启用。

```
┌──────────────────────────────────────────────────────────┐
│                    系统内存布局                             │
│                                                          │
│  0            16MB          4GB                    max    │
│  ├─ ZONE_DMA ─┤─ ZONE_DMA32 ─┤──── ZONE_NORMAL ────┤    │
│               │               │                      │    │
│   ┌───────────┴─┐             │                      │    │
│   │ swiotlb pool│             │                      │    │
│   │ (bounce buf)│             │                      │    │
│   └─────────────┘             │                      │    │
│   设备可达区域 ◄──────────────►│                      │    │
│                               │  设备不可达           │    │
└──────────────────────────────────────────────────────────┘
```

**核心数据结构（`kernel/dma/swiotlb.c`）：**

```c
struct io_tlb_slot {
    phys_addr_t orig_addr;   // 原始物理地址
    size_t alloc_size;       // 分配大小
    unsigned short list;     // 空闲列表（连续空闲 slot 数）
    unsigned short pad_slots;// 对齐填充 slot 数
};
```

**工作流程：**

```
1. dma_map_page() 发现设备无法访问目标地址
2. 从 swiotlb pool 分配 bounce buffer
3. 如果是 DMA_TO_DEVICE：
   memcpy(bounce_buf, orig_buf, size)  // CPU→bounce
4. 设备 DMA 访问 bounce buffer
5. 如果是 DMA_FROM_DEVICE：
   memcpy(orig_buf, bounce_buf, size)  // bounce→CPU
6. 释放 bounce buffer
```

**配置参数：**
- `swiotlb=<size>`：启动参数设置池大小
- 默认约 64MB
- `CONFIG_SWIOTLB_DYNAMIC`：支持动态扩展池

### 2.6 CMA 连续内存分配器

DMA 设备常需要**物理连续内存**，但系统运行后内存碎片化严重。CMA 解决此问题。

**原理：**

```
系统启动时:
┌──────────────────────────────────────────────┐
│ 内核预留 CMA 区域（如128MB）                  │
│ 标记为 MIGRATE_CMA                           │
│ 只允许可迁移页（page cache等）使用             │
└──────────────────────────────────────────────┘

正常运行时:                     设备请求时:
┌────────────────────┐         ┌────────────────────┐
│ CMA区域被page cache│         │ 迁移走可移动页       │
│ 和匿名页临时使用    │ ─────▶  │ 返回连续物理内存     │
└────────────────────┘         └────────────────────┘
```

**关键 API（`include/linux/dma-map-ops.h`）：**

```c
void dma_contiguous_reserve(phys_addr_t addr_limit);
struct page *dma_alloc_contiguous(struct device *dev, size_t size, gfp_t gfp);
void dma_free_contiguous(struct device *dev, struct page *page, size_t size);

// 每个设备可有独立 CMA 区域
static inline struct cma *dev_get_cma_area(struct device *dev)
{
    if (dev && dev->cma_area)
        return dev->cma_area;      // 设备私有 CMA
    return dma_contiguous_default_area; // 全局 CMA
}
```

**配置参数：**
```
cma=128M                  # 全局 CMA 大小
numa_cma=1:64M,2:128M     # Per-NUMA 节点 CMA
```

### 2.7 Coherent DMA 内存池

对于需要 CPU 和设备**同时持续访问**的共享内存，使用 Coherent DMA 分配：

```c
// 分配一致性 DMA 内存（映射为 uncached 或 hardware-coherent）
void *dma_alloc_coherent(struct device *dev, size_t size,
                         dma_addr_t *dma_handle, gfp_t gfp);
void dma_free_coherent(struct device *dev, size_t size,
                       void *cpu_addr, dma_addr_t dma_handle);

// 托管版本（设备移除时自动释放）
void *dmam_alloc_attrs(struct device *dev, size_t size,
                       dma_addr_t *dma_handle, gfp_t gfp, unsigned long attrs);
```

**分配流程（`kernel/dma/direct.c::dma_direct_alloc()`）：**

```c
void *dma_direct_alloc(struct device *dev, size_t size,
                       dma_addr_t *dma_handle, gfp_t gfp, unsigned long attrs)
{
    // 非一致性设备的处理策略：
    if (!dev_is_dma_coherent(dev)) {
        // 方案1: 架构提供专用分配（如有的SoC有专用SRAM）
        if (IS_ENABLED(CONFIG_ARCH_HAS_DMA_ALLOC))
            return arch_dma_alloc(dev, ...);

        // 方案2: 全局一致性池
        if (IS_ENABLED(CONFIG_DMA_GLOBAL_POOL))
            return dma_alloc_from_global_coherent(dev, ...);

        // 方案3: 将内存映射为 uncached
        set_uncached = IS_ENABLED(CONFIG_ARCH_HAS_DMA_SET_UNCACHED);
        // 方案4: 重新映射为 uncached (vmalloc)
        remap = IS_ENABLED(CONFIG_DMA_DIRECT_REMAP);
    }

    // 分配物理连续页面
    page = __dma_direct_alloc_pages(dev, size, gfp, ...);

    // 清理 Cache，确保设备看到干净数据
    arch_dma_prep_coherent(page, size);

    // 设置返回的 DMA 句柄
    *dma_handle = phys_to_dma_direct(dev, page_to_phys(page));
    return ret;
}
```

---

## 第三部分：ARM64 平台特有实现

### 3.1 Cache 维护操作

ARM64 的 DMA Cache 维护在 `arch/arm64/mm/dma-mapping.c` 中实现，使用 ARMv8 的 Cache 维护指令：

| 函数 | 底层指令 | 作用 |
|------|---------|------|
| `dcache_clean_poc()` | `DC CVAC` / `DC CIVAC` | 将 D-Cache 清洗到 PoC |
| `dcache_inval_poc()` | `DC IVAC` | 使 D-Cache 行无效化 |
| `dcache_clean_inval_poc()` | `DC CIVAC` | 清洗+无效化 |

**ARCH_DMA_MINALIGN 检查：**

```c
void arch_setup_dma_ops(struct device *dev, bool coherent)
{
    int cls = cache_line_size_of_cpu();

    // 重要安全检查：DMA 缓冲区对齐必须 >= Cache 行大小
    // 否则可能导致相邻数据被意外刷出 Cache
    WARN_TAINT(!coherent && cls > ARCH_DMA_MINALIGN,
               TAINT_CPU_OUT_OF_SPEC,
               "%s %s: ARCH_DMA_MINALIGN smaller than CTR_EL0.CWG (%d < %d)",
               dev_driver_string(dev), dev_name(dev),
               ARCH_DMA_MINALIGN, cls);

    dev->dma_coherent = coherent;  // 标记设备一致性属性
}
```

**为什么 DMA buffer 必须 Cache 行对齐？**

```
假设 Cache 行 = 64 bytes, DMA buffer 起始于偏移 32:

Cache Line N:  [其他数据 32B][DMA buffer 前32B]
Cache Line N+1:[DMA buffer 后部分 ...]

当 invalidate DMA buffer 时，Cache Line N 被整行无效化，
"其他数据 32B" 也被丢弃！→ 数据损坏
```

### 3.2 DMA Zone 配置

ARM64 使用内存区域来支持不同 DMA 能力的设备：

```c
// kernel/dma/direct.c
u64 zone_dma_limit __ro_after_init = DMA_BIT_MASK(24);  // 默认16MB

// 分配策略：根据设备能力选择合适的 zone
static gfp_t dma_direct_optimal_gfp_mask(struct device *dev, u64 *phys_limit)
{
    u64 dma_limit = min_not_zero(
        dev->coherent_dma_mask,
        dev->bus_dma_limit);

    *phys_limit = dma_to_phys(dev, dma_limit);

    if (*phys_limit <= zone_dma_limit)
        return GFP_DMA;         // 最低16MB
    if (*phys_limit <= DMA_BIT_MASK(32))
        return GFP_DMA32;       // 低4GB
    return 0;                    // 无限制
}
```

**ARM64 DMA Zone 布局：**

```
0                 16MB              4GB               最大物理地址
├── ZONE_DMA ────┤── ZONE_DMA32 ──┤── ZONE_NORMAL ──────────┤
│  GFP_DMA       │  GFP_DMA32     │  (默认)                 │
│  24-bit设备     │  32-bit设备    │  64-bit设备              │
│  (极老旧ISA设备)│  (多数PCI设备) │  (现代设备)               │
```

### 3.3 ARM SMMUv3 驱动

SMMUv3 驱动位于 `drivers/iommu/arm/arm-smmu-v3/`，是 ARM64 平台最重要的 IOMMU 实现。

**驱动文件结构：**

| 文件 | 功能 |
|------|------|
| `arm-smmu-v3.c` | 核心驱动：初始化、流表管理、命令队列 |
| `arm-smmu-v3.h` | 寄存器定义、数据结构 |
| `arm-smmu-v3-sva.c` | Shared Virtual Addressing 支持 |
| `arm-smmu-v3-iommufd.c` | IOMMUFD 用户态接口 |
| `tegra241-cmdqv.c` | NVIDIA Tegra CMDQ 虚拟化扩展 |

**SMMUv3 关键特性（IDR0 寄存器）：**

```c
#define IDR0_S1P    (1 << 1)   // Stage 1 翻译支持
#define IDR0_S2P    (1 << 0)   // Stage 2 翻译支持（虚拟化）
#define IDR0_ATS    (1 << 10)  // ATC/ATS 支持（PCIe设备侧TLB）
#define IDR0_PRI    (1 << 16)  // Page Request Interface
#define IDR0_CD2L   (1 << 19)  // 两级 Context Descriptor
#define IDR0_VMID16 (1 << 18)  // 16-bit VMID
#define IDR0_MSI    (1 << 13)  // MSI 中断支持
#define IDR0_COHACC (1 << 4)   // 一致性访问（页表遍历可缓存）
#define IDR0_HTTU   GENMASK(7, 6)  // Hardware Translation Table Update
```

**SMMUv3 页表粒度支持（IDR5 寄存器）：**

```c
#define IDR5_GRAN4K   (1 << 4)   // 4KB 页粒度
#define IDR5_GRAN16K  (1 << 5)   // 16KB 页粒度
#define IDR5_GRAN64K  (1 << 6)   // 64KB 页粒度
#define IDR5_OAS      GENMASK(2, 0)  // 输出地址大小
// OAS 编码: 32/36/40/42/44/48/52 位
```

---

## 第四部分：核心数据结构

### 设备 DMA 配置（`struct device` 相关字段）

```c
struct device {
    const struct dma_map_ops *dma_ops;    // DMA 操作表（可 NULL，用默认）
    u64    *dma_mask;                      // 流式 DMA 地址掩码
    u64     coherent_dma_mask;             // 一致性 DMA 地址掩码
    u64     bus_dma_limit;                 // 总线 DMA 地址上限
    const struct bus_dma_region *dma_range_map; // CPU→DMA 地址映射表
    struct cma *cma_area;                  // 设备私有 CMA 区域
    bool    dma_coherent;                  // 设备是否硬件一致性
    bool    dma_ops_bypass;                // 旁路 dma_ops
    // ...
};
```

### DMA Cookie 机制

```c
typedef s32 dma_cookie_t;
#define DMA_MIN_COOKIE  1      // 最小有效 cookie

// Cookie 工作流：
// 1. 描述符创建时 cookie = -EBUSY（未提交）
// 2. tx_submit() 后 cookie = 正值（提交成功）
// 3. 通过 cookie 查询传输状态

enum dma_status {
    DMA_COMPLETE,       // 传输完成
    DMA_IN_PROGRESS,    // 传输中
    DMA_PAUSED,         // 已暂停
    DMA_ERROR,          // 传输错误
    DMA_OUT_OF_ORDER,   // 乱序完成
};
```

### DMA Slave 配置

```c
struct dma_slave_config {
    enum dma_transfer_direction direction;  // 传输方向
    phys_addr_t src_addr;     // 源设备寄存器物理地址
    phys_addr_t dst_addr;     // 目的设备寄存器物理地址
    enum dma_slave_buswidth src_addr_width;  // 源总线宽度(1/2/4/8/...字节)
    enum dma_slave_buswidth dst_addr_width;  // 目的总线宽度
    u32 src_maxburst;         // 源最大突发长度
    u32 dst_maxburst;         // 目的最大突发长度
    u32 src_port_window_size; // 源端口窗口大小
    u32 dst_port_window_size; // 目的端口窗口大小
    bool device_fc;           // 设备流控
    bool peripheral_config;   // 外设特定配置
    size_t peripheral_size;   // 外设配置大小
};
```

---

## 第五部分：关键 API 速查

### DMA Mapping API（驱动开发者必知）

| API | 功能 | 使用场景 |
|-----|------|---------|
| `dma_map_single()` | 映射单个连续缓冲区 | 简单 DMA 传输 |
| `dma_unmap_single()` | 解除单缓冲区映射 | 传输完成后 |
| `dma_map_page_attrs()` | 映射页面到 DMA 地址 | 通用页映射 |
| `dma_map_sg_attrs()` | 映射 scatter-gather 列表 | 非连续缓冲区 |
| `dma_map_sgtable()` | 映射 sg_table | 新推荐的 SG 映射 |
| `dma_map_resource()` | 映射 MMIO 资源 | P2P DMA |
| `dma_alloc_coherent()` | 分配一致性 DMA 内存 | CPU/设备共享内存 |
| `dma_alloc_noncontiguous()` | 分配非连续 DMA 内存 | 大块 DMA 缓冲区 |
| `dma_sync_single_for_cpu()` | 同步给 CPU 访问 | DMA 完成后 CPU 读 |
| `dma_sync_single_for_device()` | 同步给设备访问 | CPU 写完后 DMA 读 |
| `dma_set_mask()` | 设置流式 DMA 地址掩码 | 设备初始化 |
| `dma_set_coherent_mask()` | 设置一致性 DMA 掩码 | 设备初始化 |
| `dma_mapping_error()` | 检查映射是否失败 | 每次映射后必须检查 |

### DMA Engine API（DMA 控制器使用者）

| API | 功能 |
|-----|------|
| `dma_request_chan()` | 按名称请求 DMA 通道 |
| `dma_release_channel()` | 释放 DMA 通道 |
| `dmaengine_slave_config()` | 配置从设备传输参数 |
| `dmaengine_prep_slave_sg()` | 准备 SG 从设备传输 |
| `dmaengine_prep_dma_memcpy()` | 准备内存拷贝传输 |
| `dmaengine_prep_dma_cyclic()` | 准备循环传输 |
| `dmaengine_submit()` | 提交描述符（获取 cookie） |
| `dma_async_issue_pending()` | 触发挂起的传输 |
| `dmaengine_tx_status()` | 查询传输状态 |
| `dmaengine_terminate_all()` | 终止所有传输 |
| `dmaengine_pause()` / `resume()` | 暂停/恢复传输 |

---

## 第六部分：典型使用模式

### 模式一：流式 DMA 映射（网络驱动典型）

```c
/* 发送数据（DMA_TO_DEVICE） */
dma_addr_t dma_handle;
void *buf = kmalloc(size, GFP_KERNEL);

// 填充数据...
memcpy(buf, data, size);

// 映射为 DMA 地址（会自动处理 Cache flush）
dma_handle = dma_map_single(dev, buf, size, DMA_TO_DEVICE);
if (dma_mapping_error(dev, dma_handle)) {
    // 错误处理！
    goto err;
}

// 配置设备 DMA 寄存器，启动传输
writel(dma_handle, dev->regs + TX_ADDR);
writel(size, dev->regs + TX_SIZE);
writel(START, dev->regs + TX_CTRL);

// ... 等待 DMA 完成中断 ...

// 解除映射
dma_unmap_single(dev, dma_handle, size, DMA_TO_DEVICE);
kfree(buf);
```

### 模式二：一致性 DMA 内存（设备描述符环典型）

```c
/* 分配 CPU 和设备共享的描述符环 */
struct my_desc_ring *ring;
dma_addr_t ring_dma;

// 分配：返回 CPU 虚拟地址 + DMA 地址
ring = dma_alloc_coherent(dev, sizeof(*ring) * RING_SIZE,
                          &ring_dma, GFP_KERNEL);
if (!ring)
    return -ENOMEM;

// CPU 可直接读写 ring，设备通过 ring_dma 访问
// 无需手动 Cache 同步！
ring[0].addr = some_dma_addr;
ring[0].len = transfer_size;
ring[0].flags = DESC_VALID;

// 告诉设备描述符环地址
writel(ring_dma, dev->regs + DESC_BASE);

// 使用完释放
dma_free_coherent(dev, sizeof(*ring) * RING_SIZE, ring, ring_dma);
```

### 模式三：DMA Engine 从设备传输（UART/SPI 典型）

```c
struct dma_chan *chan;
struct dma_slave_config cfg;
struct dma_async_tx_descriptor *desc;
dma_cookie_t cookie;

// 1. 请求通道（设备树中定义 dma-names = "tx"）
chan = dma_request_chan(dev, "tx");
if (IS_ERR(chan))
    return PTR_ERR(chan);

// 2. 配置从设备参数
cfg.direction = DMA_MEM_TO_DEV;
cfg.dst_addr = uart_phys_base + UART_THR;  // UART 发送寄存器
cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE;
cfg.dst_maxburst = 16;
dmaengine_slave_config(chan, &cfg);

// 3. 准备 SG 传输描述符
desc = dmaengine_prep_slave_sg(chan, sg, nents,
                                DMA_MEM_TO_DEV,
                                DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
if (!desc) {
    // 错误处理
}

// 4. 设置回调
desc->callback = tx_complete_callback;
desc->callback_param = my_private_data;

// 5. 提交并触发
cookie = dmaengine_submit(desc);
dma_async_issue_pending(chan);

// 6. 回调中或主动查询完成
// callback 中: 标记传输完成

// 7. 释放
dma_release_channel(chan);
```

### 模式四：Scatter-Gather 映射（块设备典型）

```c
struct scatterlist sg[MAX_SG];
int nents, mapped;

sg_init_table(sg, MAX_SG);

// 填充 SG 列表
for (i = 0; i < nents; i++)
    sg_set_page(&sg[i], pages[i], PAGE_SIZE, 0);

// 映射整个 SG 列表
mapped = dma_map_sg(dev, sg, nents, DMA_FROM_DEVICE);
if (!mapped) {
    // 错误处理
}

// mapped 可能 < nents（IOMMU 可能合并相邻项）
for_each_sg(sg, s, mapped, i) {
    dma_addr_t addr = sg_dma_address(s);
    unsigned int len = sg_dma_len(s);
    // 配置设备 DMA：addr + len
}

// ... 传输完成后 ...

// 解除映射
dma_unmap_sg(dev, sg, nents, DMA_FROM_DEVICE);
```

---

## 第七部分：调试与问题排查

### 常见 DMA 问题

| 问题 | 症状 | 原因 | 解决方案 |
|------|------|------|---------|
| **数据损坏** | 读到旧数据或乱数据 | Cache 一致性未正确处理 | 检查 sync_for_cpu/device 调用 |
| **DMA 超时** | 传输不完成 | 地址错误/设备未响应 | 检查 DMA 地址是否在设备可达范围 |
| **映射失败** | `DMA_MAPPING_ERROR` | 地址超出 DMA mask | 检查 `dma_set_mask()` 设置 |
| **内存泄漏** | 系统内存持续下降 | 未调用 `dma_unmap_*` | 使用 `CONFIG_DMA_API_DEBUG` |
| **SMMU fault** | 内核 oops/设备报错 | IOMMU 翻译失败 | 检查 IOMMU domain 绑定 |
| **性能低下** | 传输速率不达预期 | swiotlb bounce 拷贝 | 检查是否走了 bounce 路径 |

### 调试工具与内核配置

```kconfig
# DMA 调试选项
CONFIG_DMA_API_DEBUG=y          # 启用 DMA API 调试
CONFIG_DMA_API_DEBUG_SG=y       # SG 映射调试
CONFIG_IOMMU_DEBUGFS=y          # IOMMU debugfs 接口

# swiotlb 调试
CONFIG_SWIOTLB=y                # 启用 swiotlb
CONFIG_SWIOTLB_DYNAMIC=y        # 动态 swiotlb 池
```

**debugfs 检查点：**

```bash
# 查看 IOMMU 组和设备绑定
ls /sys/kernel/iommu_groups/

# 查看 DMA 引擎设备和通道
ls /sys/class/dma/

# 查看 swiotlb 使用情况
cat /sys/kernel/debug/swiotlb/io_tlb_nslabs
cat /sys/kernel/debug/swiotlb/io_tlb_used

# 检查设备 DMA 属性
cat /sys/bus/platform/devices/<device>/dma_coherent
```

### DMA 编程检查清单

- [ ] 设备初始化时调用 `dma_set_mask_and_coherent()` 设置 DMA 能力
- [ ] 每次 `dma_map_*()` 后检查 `dma_mapping_error()`
- [ ] `dma_map_*()` 和 `dma_unmap_*()` 成对调用，避免泄漏
- [ ] DMA 缓冲区对齐到 Cache 行大小（`ARCH_DMA_MINALIGN`）
- [ ] 映射后、设备访问前不要 CPU 写入（DMA_TO_DEVICE 场景）
- [ ] 设备 DMA 完成后、CPU 读取前调用 `dma_sync_*_for_cpu()`
- [ ] Coherent 内存只在真正需要 CPU/设备同时访问时使用
- [ ] 优先使用 `dma_map_sgtable()` 替代旧的 `dma_map_sg()`
- [ ] 使用 `dmam_alloc_attrs()` 等托管版本简化资源管理

---

## 第八部分：内核经典 DMA 映射案例分析

以下案例均取自本内核源码树中的 ARM/ARM64 平台真实驱动，展示不同 DMA 映射模式的工业级用法。

### 案例一：流式 DMA 映射 + DMA Engine（PL011 UART TX）

> **源码**: `drivers/tty/serial/amba-pl011.c`
> **模式**: `dma_map_single()` + `dmaengine_prep_slave_single()` — 需求传输

PL011 是 ARM 经典 UART 控制器，其 DMA 发送路径是学习流式映射最佳范例。

**第一步：探测阶段 — 请求 DMA 通道并配置**

```c
// drivers/tty/serial/amba-pl011.c: pl011_dma_probe()

// 配置从设备参数
struct dma_slave_config tx_conf = {
    .dst_addr = uap->port.mapbase +
                pl011_reg_to_offset(uap, REG_DR),   // UART 数据寄存器物理地址
    .dst_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE,     // UART 是字节设备
    .direction = DMA_MEM_TO_DEV,                      // 内存→UART
    .dst_maxburst = uap->fifosize >> 1,               // burst = FIFO 大小 / 2
    .device_fc = false,                               // DMA 控制器管理流控
};

// 通过设备树 dma-names = "tx" 请求通道
chan = dma_request_chan(dev, "tx");

// 将从设备配置应用到通道
dmaengine_slave_config(chan, &tx_conf);
```

**第二步：发送数据 — 流式映射 + 提交描述符**

```c
// drivers/tty/serial/amba-pl011.c: pl011_dma_tx_refill()

// 1. 从内核 FIFO 拷贝待发送数据到 DMA 缓冲区
count = kfifo_out_peek(&tport->xmit_fifo, dmatx->buf, count);

// 2. ★ 流式映射：将 CPU 缓冲区映射为 DMA 地址 ★
//    内核会自动执行 Cache Clean（ARM64: dcache_clean_poc）
dmatx->dma = dma_map_single(dma_dev->dev, dmatx->buf, count,
                             DMA_TO_DEVICE);

// 3. ★ 必须检查映射是否成功 ★
if (dma_mapping_error(dma_dev->dev, dmatx->dma)) {
    dev_dbg(uap->port.dev, "unable to map TX DMA\n");
    return -EBUSY;
}

// 4. 准备从设备传输描述符（单缓冲区 → UART DR 寄存器）
desc = dmaengine_prep_slave_single(chan,
    dmatx->dma,                              // 已映射的 DMA 地址
    dmatx->len,                              // 传输长度
    DMA_MEM_TO_DEV,                          // 方向：内存→设备
    DMA_PREP_INTERRUPT | DMA_CTRL_ACK);      // 完成时中断

// 5. 设置完成回调 + 提交 + 触发
desc->callback = pl011_dma_tx_callback;
desc->callback_param = uap;
dmaengine_submit(desc);
dma_dev->device_issue_pending(chan);

// 6. 使能 UART 的 DMA 发送模式
uap->dmacr |= UART011_TXDMAE;
pl011_write(uap->dmacr, uap, REG_DMACR);
```

**第三步：完成回调 — 解除映射**

```c
// pl011_dma_tx_callback() 中：
dma_unmap_single(dma_dev->dev, dmatx->dma, dmatx->len, DMA_TO_DEVICE);
// 映射和解映射必须成对出现！
```

**关键学习要点：**
```
                完整生命周期
┌──────────────────────────────────────────────────────────┐
│ kfifo_out_peek()  → 数据在 CPU Cache 中                   │
│       │                                                   │
│ dma_map_single()  → Cache Clean + 返回 DMA 地址           │
│       │              (CPU 不能再写这块内存！)               │
│       │                                                   │
│ dmaengine_submit() → 硬件开始传输                         │
│       │                                                   │
│ 回调触发            → DMA 传输完成                         │
│       │                                                   │
│ dma_unmap_single() → 释放映射（CPU 可再次使用此内存）       │
└──────────────────────────────────────────────────────────┘
```

---

### 案例二：Coherent DMA 内存分配（PL011 DMA 缓冲区）

> **源码**: `drivers/tty/serial/amba-pl011.c`
> **模式**: `dma_alloc_coherent()` — 一致性内存

同样是 PL011 驱动，其 DMA 缓冲区本身使用 Coherent 分配：

```c
// drivers/tty/serial/amba-pl011.c: pl011_dmabuf_init()

static int pl011_dmabuf_init(struct dma_chan *chan, struct pl011_dmabuf *db,
                             enum dma_data_direction dir)
{
    // ★ 分配一致性 DMA 内存 ★
    // 返回值: db->buf = CPU 虚拟地址（可直接读写）
    //         db->dma = DMA 地址（给设备使用）
    db->buf = dma_alloc_coherent(chan->device->dev,
                                 PL011_DMA_BUFFER_SIZE,  // PAGE_SIZE = 4KB
                                 &db->dma,               // 输出 DMA 地址
                                 GFP_KERNEL);
    if (!db->buf)
        return -ENOMEM;
    db->len = PL011_DMA_BUFFER_SIZE;
    return 0;
}

// 释放时：
static void pl011_dmabuf_free(struct dma_chan *chan, struct pl011_dmabuf *db,
                              enum dma_data_direction dir)
{
    if (db->buf)
        dma_free_coherent(chan->device->dev,
                          PL011_DMA_BUFFER_SIZE,
                          db->buf,     // CPU 虚拟地址
                          db->dma);    // DMA 地址
}
```

**Coherent vs Streaming 选择依据：**
```
┌───────────────────────────────────────────────────────────────────┐
│  问题: PL011 为什么用 Coherent 分配 DMA 缓冲区？                   │
│                                                                   │
│  答: 因为该缓冲区被 CPU (kfifo_out_peek) 和 DMA 控制器反复使用。   │
│  如果用流式映射，每次发送都要 map/unmap（Cache flush 开销大）。      │
│  Coherent 内存在 non-coherent 平台上映射为 uncached，              │
│  CPU 写入立即可见于设备，无需手动 Cache 维护。                      │
│                                                                   │
│  ★ 选择规则:                                                      │
│  - 缓冲区被多次反复使用 → dma_alloc_coherent()                    │
│  - 缓冲区一次性使用后释放 → dma_map_single()                      │
│  - 描述符环（DMA descriptor ring）→ 永远用 dma_alloc_coherent()   │
└───────────────────────────────────────────────────────────────────┘
```

---

### 案例三：Scatter-Gather DMA 映射（DW MMC 控制器）

> **源码**: `drivers/mmc/host/dw_mmc.c`
> **模式**: `dma_map_sg()` / `dma_unmap_sg()` — SG 流式映射

DesignWare MMC 控制器是 ARM SoC 常用的 eMMC/SD 控制器。

```c
// drivers/mmc/host/dw_mmc.c: dw_mci_pre_dma_transfer()

static int dw_mci_pre_dma_transfer(struct dw_mci *host,
                                    struct mmc_data *data, int cookie)
{
    // 1. ★ 对齐检查：DMA 要求字对齐 ★
    if (data->blksz & 3)       // 块大小必须 4 字节对齐
        return -EINVAL;
    for_each_sg(data->sg, sg, data->sg_len, i) {
        if (sg->offset & 3 || sg->length & 3)  // 每个 SG 段也要对齐
            return -EINVAL;
    }

    // 2. ★ 映射 scatter-gather 列表 ★
    //    将多个不连续物理页映射为 DMA 地址
    //    如果有 IOMMU，多段可能被合并为一个 IOVA 连续区域
    sg_len = dma_map_sg(host->dev,
                        data->sg,               // SG 列表
                        data->sg_len,            // SG 段数
                        mmc_get_dma_dir(data));  // 根据读/写决定方向
    if (sg_len == 0)
        return -EINVAL;

    // sg_len 可能 <= data->sg_len（IOMMU 合并了相邻段）
    data->host_cookie = cookie;
    return sg_len;
}

// 传输完成后解除映射:
// drivers/mmc/host/dw_mmc.c: dw_mci_post_req()
static void dw_mci_post_req(struct mmc_host *mmc,
                            struct mmc_request *mrq, int err)
{
    if (data->host_cookie != COOKIE_UNMAPPED)
        dma_unmap_sg(slot->host->dev,
                     data->sg,
                     data->sg_len,             // ★ 用原始 sg_len，不是映射后的 ★
                     mmc_get_dma_dir(data));
    data->host_cookie = COOKIE_UNMAPPED;
}
```

**SG 映射的核心价值：**
```
  不使用 SG (需要物理连续):           使用 SG (支持离散页):
  ┌────────────────────────┐         ┌────┐ ┌────┐ ┌────┐ ┌────┐
  │ 连续 16KB 物理内存       │         │4KB │ │4KB │ │4KB │ │4KB │
  │ (很难分配到！)          │         │Page│ │Page│ │Page│ │Page│
  └────────────────────────┘         └──┬─┘ └──┬─┘ └──┬─┘ └──┬─┘
                                        │      │      │      │
                                    dma_map_sg() 映射每一段
                                        │      │      │      │
                                        ▼      ▼      ▼      ▼
  如果有 IOMMU:          ┌──────────────────────────────────────┐
  设备看到连续 IOVA →    │ IOVA 0x1000_0000 ~ 0x1000_3FFF (16KB)│
                         └──────────────────────────────────────┘
```

---

### 案例四：SPI DMA 全双工传输 + SG（PL022 SPI 控制器）

> **源码**: `drivers/spi/spi-pl022.c`
> **模式**: `dma_map_sg()` + `dmaengine_prep_slave_sg()` — SG 需求传输

PL022 是最经典的 ARM AMBA SPI 控制器，展示了**全双工 DMA 传输**模式。

```c
// drivers/spi/spi-pl022.c: configure_dma()

// 1. 构建收发两个 SG 列表
setup_dma_scatter(pl022, pl022->rx, pl022->cur_transfer->len, &pl022->sgt_rx);
setup_dma_scatter(pl022, pl022->tx, pl022->cur_transfer->len, &pl022->sgt_tx);

// 2. ★ 分别映射 RX 和 TX 的 SG 列表 ★
//    注意方向不同：RX 是设备写内存(FROM_DEVICE)，TX 是内存写设备(TO_DEVICE)
rx_sglen = dma_map_sg(rxchan->device->dev,
                      pl022->sgt_rx.sgl, pl022->sgt_rx.nents,
                      DMA_FROM_DEVICE);       // 设备→内存

tx_sglen = dma_map_sg(txchan->device->dev,
                      pl022->sgt_tx.sgl, pl022->sgt_tx.nents,
                      DMA_TO_DEVICE);         // 内存→设备

// 3. ★ 准备两个方向的 DMA 描述符 ★
rxdesc = dmaengine_prep_slave_sg(rxchan,
    pl022->sgt_rx.sgl, rx_sglen,
    DMA_DEV_TO_MEM,                           // SPI RX FIFO → 内存
    DMA_PREP_INTERRUPT | DMA_CTRL_ACK);

txdesc = dmaengine_prep_slave_sg(txchan,
    pl022->sgt_tx.sgl, tx_sglen,
    DMA_MEM_TO_DEV,                           // 内存 → SPI TX FIFO
    DMA_PREP_INTERRUPT | DMA_CTRL_ACK);

// 4. ★ 回调只挂在 RX 上（RX 肯定最后完成） ★
rxdesc->callback = dma_callback;
rxdesc->callback_param = pl022;

// 5. ★ 先提交 RX，再提交 TX ★
//    SPI 是主从式：主机发 TX 同时收 RX，先启动 RX 确保不丢数据
dmaengine_submit(rxdesc);
dmaengine_submit(txdesc);
dma_async_issue_pending(rxchan);
dma_async_issue_pending(txchan);
```

**全双工 DMA 时序：**
```
时间 ──────────────────────────────────────────▶

TX DMA:  ═══════发送数据到SPI TX FIFO══════════▶ 完成
         │
SPI总线:  ═══MOSI═══════════════════════════════▶
         ═══MISO═══════════════════════════════▶
         │
RX DMA:  ═══════从SPI RX FIFO读数据════════════▶ 完成 → 回调触发！
```

**错误处理的正确模式（逆序释放资源）：**
```c
err_txdesc:
    dmaengine_terminate_all(txchan);            // 先终止通道
err_rxdesc:
    dmaengine_terminate_all(rxchan);
    dma_unmap_sg(txchan->device->dev,           // 再解除 TX SG 映射
                 pl022->sgt_tx.sgl, pl022->sgt_tx.nents, DMA_TO_DEVICE);
err_tx_sgmap:
    dma_unmap_sg(rxchan->device->dev,           // 再解除 RX SG 映射
                 pl022->sgt_rx.sgl, pl022->sgt_rx.nents, DMA_FROM_DEVICE);
err_rx_sgmap:
    sg_free_table(&pl022->sgt_tx);              // 最后释放 SG 表
err_alloc_tx_sg:
    sg_free_table(&pl022->sgt_rx);
```

---

### 案例五：网络驱动 Coherent 描述符环 + 流式数据映射（STMMAC）

> **源码**: `drivers/net/ethernet/stmicro/stmmac/stmmac_main.c`
> **模式**: `dma_alloc_coherent()`（描述符环）+ `dma_map_single()`（数据帧）

STMMAC 是 ARM SoC 最常用的千兆以太网驱动，展示了**混合使用两种 DMA 映射**的经典模式。

```c
// ★ 第一层：DMA 描述符环 — Coherent 分配 ★
// 描述符环被 CPU 和 DMA 引擎持续共享访问，必须用 coherent

// drivers/net/ethernet/stmicro/stmmac/stmmac_main.c: alloc_dma_rx_desc_resources()
rx_q->dma_rx = dma_alloc_coherent(priv->device,
    dma_conf->dma_rx_size * sizeof(struct dma_desc),   // 256 个描述符
    &rx_q->dma_rx_phy,                                  // DMA 地址
    GFP_KERNEL);

// CPU 直接写描述符（无需 Cache 同步）:
rx_q->dma_rx[i].des0 = cpu_to_le32(lower_32_bits(buf_dma_addr));
rx_q->dma_rx[i].des1 = cpu_to_le32(upper_32_bits(buf_dma_addr));
// 设备通过 rx_q->dma_rx_phy 直接看到描述符更新


// ★ 第二层：网络数据帧 — 流式映射 ★
// 每个 SKB 是一次性使用，用流式映射效率最高

// stmmac_tso_xmit() — 发送路径:
des = dma_map_single(priv->device,
    skb->data,                     // SKB 线性数据区
    skb_headlen(skb),              // 头部长度
    DMA_TO_DEVICE);                // CPU→网卡

if (dma_mapping_error(priv->device, des))
    goto dma_map_err;              // ★ 必须检查 ★

// 将 DMA 地址写入 Coherent 描述符环
stmmac_set_desc_addr(priv, first, des);

// 记录映射信息，供发送完成后 unmap
tx_q->tx_skbuff_dma[tx_q->cur_tx].buf = des;
tx_q->tx_skbuff_dma[tx_q->cur_tx].len = skb_headlen(skb);
tx_q->tx_skbuff_dma[tx_q->cur_tx].map_as_page = false;
```

**双层架构全景：**
```
CPU 视角:                          设备 (DMA) 视角:
                                   
┌──────────────────┐               dma_rx_phy 指向描述符环
│ rx_q->dma_rx[]   │ ◄═══════════  (Coherent: CPU/DMA 都能直接访问)
│ ┌──────────────┐ │               
│ │ desc[0].des0 │──┼──▶ buf[0] 的 DMA 地址 ──▶ ┌─────────┐
│ │ desc[0].des1 │ │                             │ SKB buf │ 流式映射
│ ├──────────────┤ │                             └─────────┘
│ │ desc[1].des0 │──┼──▶ buf[1] 的 DMA 地址 ──▶ ┌─────────┐
│ │ desc[1].des1 │ │                             │ SKB buf │ 流式映射
│ ├──────────────┤ │                             └─────────┘
│ │    ...       │ │                             
└──────────────────┘               

描述符环: Coherent DMA — CPU 写描述符，设备读描述符，无需 sync
数据帧:   Streaming DMA — 每帧 map 一次，传完 unmap，需 Cache 维护
```

---

## 第九部分：QEMU ARM64 DMA 实验

本节提供可在 QEMU `virt` 机器上实践的 DMA 实验，帮助理解 DMA Mapping API 的实际行为。

### 9.1 实验环境准备

**所需工具：**
```bash
# QEMU (支持 ARM64 virt 机器)
qemu-system-aarch64 --version

# 交叉编译工具链
aarch64-linux-gnu-gcc --version

# 或者使用本机编译（如果在 ARM64 上）
```

**QEMU 启动命令参考：**
```bash
qemu-system-aarch64 \
    -machine virt \
    -cpu cortex-a72 \
    -m 1G \
    -kernel Image \
    -initrd rootfs.cpio.gz \
    -append "console=ttyAMA0 root=/dev/ram rdinit=/init loglevel=8" \
    -nographic \
    -smp 4
```

### 9.2 实验一：DMA Coherent 内存分配内核模块

这个模块演示 `dma_alloc_coherent()` 的基本用法，可以在 QEMU ARM64 上直接运行。

**模块代码：`dma_coherent_test.c`**

```c
/*
 * DMA Coherent Memory Allocation Test Module
 * 演示 dma_alloc_coherent/dma_free_coherent 在 ARM64 上的行为
 *
 * 编译: make -C /path/to/kernel M=$PWD modules ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
 * 加载: insmod dma_coherent_test.ko
 * 查看: dmesg | grep dma_test
 * 卸载: rmmod dma_coherent_test
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>

#define TEST_BUF_SIZE  4096
#define TEST_PATTERN   0xDEADBEEF

static struct platform_device *test_pdev;

static int dma_coherent_test(struct device *dev)
{
    void *cpu_addr;
    dma_addr_t dma_handle;
    u32 *buf;
    int i, errors = 0;

    pr_info("dma_test: === Coherent DMA Allocation Test ===\n");

    /* 1. 设置 DMA 掩码 */
    if (dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64))) {
        pr_err("dma_test: Failed to set DMA mask\n");
        return -EIO;
    }
    pr_info("dma_test: DMA mask set to 64-bit\n");

    /* 2. 分配一致性 DMA 内存 */
    cpu_addr = dma_alloc_coherent(dev, TEST_BUF_SIZE, &dma_handle, GFP_KERNEL);
    if (!cpu_addr) {
        pr_err("dma_test: dma_alloc_coherent failed\n");
        return -ENOMEM;
    }

    pr_info("dma_test: Coherent alloc OK\n");
    pr_info("dma_test:   CPU virtual addr : %px\n", cpu_addr);
    pr_info("dma_test:   DMA bus addr     : %pad\n", &dma_handle);
    pr_info("dma_test:   Physical addr    : %pa\n",
            &(phys_addr_t){virt_to_phys(cpu_addr)});
    pr_info("dma_test:   dma_coherent     : %d\n", dev->dma_coherent);

    /* 3. CPU 写入测试数据 */
    buf = (u32 *)cpu_addr;
    for (i = 0; i < TEST_BUF_SIZE / sizeof(u32); i++)
        buf[i] = TEST_PATTERN + i;

    /* 4. CPU 立即读回验证（Coherent 内存无需 sync） */
    for (i = 0; i < TEST_BUF_SIZE / sizeof(u32); i++) {
        if (buf[i] != (TEST_PATTERN + i)) {
            errors++;
            if (errors <= 3)
                pr_err("dma_test: Mismatch at [%d]: 0x%08x != 0x%08x\n",
                       i, buf[i], TEST_PATTERN + i);
        }
    }

    if (errors == 0)
        pr_info("dma_test: Coherent R/W verify PASSED (%zu bytes)\n",
                (size_t)TEST_BUF_SIZE);
    else
        pr_err("dma_test: Coherent R/W verify FAILED (%d errors)\n", errors);

    /* 5. 释放 */
    dma_free_coherent(dev, TEST_BUF_SIZE, cpu_addr, dma_handle);
    pr_info("dma_test: Coherent memory freed\n");

    return errors ? -EIO : 0;
}

static int __init dma_test_init(void)
{
    int ret;

    pr_info("dma_test: Module loading...\n");

    /* 创建一个 platform_device 用于测试 */
    test_pdev = platform_device_register_simple("dma-test", -1, NULL, 0);
    if (IS_ERR(test_pdev)) {
        pr_err("dma_test: Failed to create platform device\n");
        return PTR_ERR(test_pdev);
    }

    ret = dma_coherent_test(&test_pdev->dev);

    return ret;
}

static void __exit dma_test_exit(void)
{
    if (test_pdev)
        platform_device_unregister(test_pdev);
    pr_info("dma_test: Module unloaded\n");
}

module_init(dma_test_init);
module_exit(dma_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DMA Coherent Memory Test for ARM64");
```

### 9.3 实验二：流式 DMA 映射 + Cache 同步

这个模块演示 `dma_map_single()` / `dma_sync_*()` 的完整流程，以及 Cache 同步的效果。

**模块代码：`dma_streaming_test.c`**

```c
/*
 * DMA Streaming Mapping Test Module
 * 演示 dma_map_single / dma_sync_for_device / dma_sync_for_cpu
 *
 * 在 ARM64 non-coherent 场景下，可以观察到 Cache 维护操作的效果
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/ktime.h>

#define BUF_SIZE    (64 * 1024)  /* 64KB */
#define PATTERN_A   0xAAAAAAAA
#define PATTERN_B   0xBBBBBBBB

static struct platform_device *test_pdev;

static int streaming_dma_test(struct device *dev)
{
    void *buf;
    dma_addr_t dma_handle;
    u32 *p;
    ktime_t t_start, t_end;
    int i;

    pr_info("dma_stream: === Streaming DMA Mapping Test ===\n");

    dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));

    /* 1. 分配普通内核内存（kmalloc 或页分配） */
    buf = kmalloc(BUF_SIZE, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    p = (u32 *)buf;

    /*
     * ============================================
     * 测试 A: DMA_TO_DEVICE 流程（CPU→设备）
     * ============================================
     */
    pr_info("dma_stream: --- Test A: DMA_TO_DEVICE ---\n");

    /* A1. CPU 写入数据 */
    for (i = 0; i < BUF_SIZE / sizeof(u32); i++)
        p[i] = PATTERN_A;

    /* A2. ★ 映射为 DMA 地址 ★
     * ARM64 上会调用 arch_sync_dma_for_device()
     * → dcache_clean_poc(): 将 Cache 中的数据刷到内存
     */
    t_start = ktime_get();
    dma_handle = dma_map_single(dev, buf, BUF_SIZE, DMA_TO_DEVICE);
    t_end = ktime_get();

    if (dma_mapping_error(dev, dma_handle)) {
        pr_err("dma_stream: map failed\n");
        kfree(buf);
        return -EIO;
    }

    pr_info("dma_stream: map_single(TO_DEVICE) took %lld ns\n",
            ktime_to_ns(ktime_sub(t_end, t_start)));
    pr_info("dma_stream:   buf virt=%px phys=%pa dma=%pad\n",
            buf, &(phys_addr_t){virt_to_phys(buf)}, &dma_handle);

    /* A3. 此时 CPU 不应再写入 buf（CPU 已"交出所有权"给设备） */

    /* A4. ★ 解除映射 ★ — CPU 重新获得所有权 */
    t_start = ktime_get();
    dma_unmap_single(dev, dma_handle, BUF_SIZE, DMA_TO_DEVICE);
    t_end = ktime_get();

    pr_info("dma_stream: unmap_single(TO_DEVICE) took %lld ns\n",
            ktime_to_ns(ktime_sub(t_end, t_start)));

    /*
     * ============================================
     * 测试 B: DMA_FROM_DEVICE 流程（设备→CPU）
     * ============================================
     */
    pr_info("dma_stream: --- Test B: DMA_FROM_DEVICE ---\n");

    /* B1. 映射为 DMA 地址（设备将写入此缓冲区） */
    dma_handle = dma_map_single(dev, buf, BUF_SIZE, DMA_FROM_DEVICE);
    if (dma_mapping_error(dev, dma_handle)) {
        pr_err("dma_stream: map failed\n");
        kfree(buf);
        return -EIO;
    }

    /* B2. 模拟"设备已写入数据"（在真实场景中是硬件 DMA 写入） */
    /* 注意：这里我们直接写内存来模拟设备行为 */

    /* B3. ★ 同步给 CPU ★
     * ARM64 上调用 arch_sync_dma_for_cpu()
     * → dcache_inval_poc(): 使 Cache 行无效化，CPU 将从内存重新读取
     */
    t_start = ktime_get();
    dma_sync_single_for_cpu(dev, dma_handle, BUF_SIZE, DMA_FROM_DEVICE);
    t_end = ktime_get();

    pr_info("dma_stream: sync_for_cpu(FROM_DEVICE) took %lld ns\n",
            ktime_to_ns(ktime_sub(t_end, t_start)));

    /* B4. 现在 CPU 可以安全读取 buf */

    /* B5. 如果 CPU 需要让设备再次写入 */
    t_start = ktime_get();
    dma_sync_single_for_device(dev, dma_handle, BUF_SIZE, DMA_FROM_DEVICE);
    t_end = ktime_get();

    pr_info("dma_stream: sync_for_device(FROM_DEVICE) took %lld ns\n",
            ktime_to_ns(ktime_sub(t_end, t_start)));

    dma_unmap_single(dev, dma_handle, BUF_SIZE, DMA_FROM_DEVICE);

    /*
     * ============================================
     * 测试 C: 映射开销对比
     * ============================================
     */
    pr_info("dma_stream: --- Test C: Mapping overhead comparison ---\n");
    {
        int iter = 100;
        ktime_t total = 0;
        for (i = 0; i < iter; i++) {
            t_start = ktime_get();
            dma_handle = dma_map_single(dev, buf, BUF_SIZE, DMA_TO_DEVICE);
            t_end = ktime_get();
            total = ktime_add(total, ktime_sub(t_end, t_start));
            dma_unmap_single(dev, dma_handle, BUF_SIZE, DMA_TO_DEVICE);
        }
        pr_info("dma_stream: avg map_single(%dKB, TO_DEVICE) = %lld ns "
                "(%d iterations)\n",
                BUF_SIZE / 1024,
                ktime_to_ns(total) / iter, iter);
    }

    kfree(buf);
    pr_info("dma_stream: All tests completed\n");
    return 0;
}

static int __init dma_stream_init(void)
{
    test_pdev = platform_device_register_simple("dma-stream-test", -1, NULL, 0);
    if (IS_ERR(test_pdev))
        return PTR_ERR(test_pdev);
    return streaming_dma_test(&test_pdev->dev);
}

static void __exit dma_stream_exit(void)
{
    if (test_pdev)
        platform_device_unregister(test_pdev);
    pr_info("dma_stream: Module unloaded\n");
}

module_init(dma_stream_init);
module_exit(dma_stream_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DMA Streaming Mapping Test for ARM64");
```

### 9.4 实验三：DMA Engine + PL011 UART 观察

利用 QEMU virt 机器自带的 PL011 UART，观察 DMA Engine 的工作过程。

**模块代码：`dma_engine_test.c`**

```c
/*
 * DMA Engine Channel Test Module
 * 演示 DMA Engine memcpy 操作（使用 QEMU 中的 DMA 控制器）
 *
 * 注意：QEMU virt 机器默认无独立 DMA 控制器，
 *       本模块可在有 DMA Engine 的真实硬件或添加了 PL330 的 QEMU 配置上运行。
 *       即使没有 DMA Engine 硬件，模块也会优雅地报告并退出。
 */
#include <linux/module.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/completion.h>
#include <linux/ktime.h>

#define DMA_BUF_SIZE    (16 * 1024)  /* 16KB */
#define TEST_PATTERN    0xCAFEBABE

static struct platform_device *test_pdev;
static DECLARE_COMPLETION(dma_done);

static void dma_memcpy_callback(void *param)
{
    complete(&dma_done);
}

static int dma_engine_memcpy_test(struct device *dev)
{
    struct dma_chan *chan;
    struct dma_async_tx_descriptor *desc;
    dma_cap_mask_t mask;
    dma_cookie_t cookie;
    void *src_buf, *dst_buf;
    dma_addr_t src_dma, dst_dma;
    ktime_t t_start, t_end;
    u32 *src, *dst;
    int i, errors = 0;

    pr_info("dma_engine: === DMA Engine Memcpy Test ===\n");

    /* 1. 请求一个支持 MEMCPY 的 DMA 通道 */
    dma_cap_zero(mask);
    dma_cap_set(DMA_MEMCPY, mask);
    chan = dma_request_chan_by_mask(&mask);
    if (IS_ERR(chan)) {
        pr_warn("dma_engine: No DMA MEMCPY channel available (err=%ld)\n",
                PTR_ERR(chan));
        pr_info("dma_engine: This is expected on QEMU virt without DMA controller\n");
        pr_info("dma_engine: Try on real HW or QEMU with PL330 DMA\n");
        return 0;  /* 不算失败 */
    }
    pr_info("dma_engine: Got channel: %s\n", dma_chan_name(chan));

    dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));

    /* 2. 分配源和目的 DMA 缓冲区（coherent 避免手动 sync） */
    src_buf = dma_alloc_coherent(dev, DMA_BUF_SIZE, &src_dma, GFP_KERNEL);
    dst_buf = dma_alloc_coherent(dev, DMA_BUF_SIZE, &dst_dma, GFP_KERNEL);
    if (!src_buf || !dst_buf) {
        pr_err("dma_engine: Buffer alloc failed\n");
        goto out_free;
    }

    /* 3. 填充源数据，清零目的 */
    src = (u32 *)src_buf;
    dst = (u32 *)dst_buf;
    for (i = 0; i < DMA_BUF_SIZE / sizeof(u32); i++) {
        src[i] = TEST_PATTERN + i;
        dst[i] = 0;
    }

    /* 4. 准备 DMA memcpy 描述符 */
    desc = dmaengine_prep_dma_memcpy(chan, dst_dma, src_dma, DMA_BUF_SIZE,
                                     DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
    if (!desc) {
        pr_err("dma_engine: prep_dma_memcpy failed\n");
        goto out_free;
    }

    /* 5. 设置完成回调 */
    desc->callback = dma_memcpy_callback;
    desc->callback_param = NULL;

    /* 6. 提交并触发 */
    reinit_completion(&dma_done);
    t_start = ktime_get();

    cookie = dmaengine_submit(desc);
    if (dma_submit_error(cookie)) {
        pr_err("dma_engine: submit failed\n");
        goto out_free;
    }
    dma_async_issue_pending(chan);

    /* 7. 等待完成 */
    if (!wait_for_completion_timeout(&dma_done, msecs_to_jiffies(5000))) {
        pr_err("dma_engine: DMA timeout!\n");
        dmaengine_terminate_all(chan);
        goto out_free;
    }
    t_end = ktime_get();

    /* 8. 验证数据 */
    for (i = 0; i < DMA_BUF_SIZE / sizeof(u32); i++) {
        if (dst[i] != (TEST_PATTERN + i)) {
            errors++;
            if (errors <= 3)
                pr_err("dma_engine: [%d] 0x%08x != 0x%08x\n",
                       i, dst[i], TEST_PATTERN + i);
        }
    }

    if (errors == 0)
        pr_info("dma_engine: DMA memcpy PASSED (%d KB in %lld ns)\n",
                DMA_BUF_SIZE / 1024,
                ktime_to_ns(ktime_sub(t_end, t_start)));
    else
        pr_err("dma_engine: DMA memcpy FAILED (%d errors)\n", errors);

out_free:
    if (src_buf)
        dma_free_coherent(dev, DMA_BUF_SIZE, src_buf, src_dma);
    if (dst_buf)
        dma_free_coherent(dev, DMA_BUF_SIZE, dst_buf, dst_dma);
    dma_release_channel(chan);
    return errors ? -EIO : 0;
}

static int __init dma_engine_init(void)
{
    test_pdev = platform_device_register_simple("dma-engine-test", -1, NULL, 0);
    if (IS_ERR(test_pdev))
        return PTR_ERR(test_pdev);
    return dma_engine_memcpy_test(&test_pdev->dev);
}

static void __exit dma_engine_exit(void)
{
    if (test_pdev)
        platform_device_unregister(test_pdev);
    pr_info("dma_engine: Module unloaded\n");
}

module_init(dma_engine_init);
module_exit(dma_engine_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DMA Engine Memcpy Test for ARM64");
```

### 9.5 Makefile（通用，适用于以上三个模块）

```makefile
# Makefile for DMA test modules
# Usage:
#   内核源码内编译:
#     make -C /path/to/kernel M=$(pwd) modules ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
#   或放到内核 kmodules/ 目录下:
#     make -C .. M=kmodules modules

obj-m += dma_coherent_test.o
obj-m += dma_streaming_test.o
obj-m += dma_engine_test.o

# 如果直接执行 make
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules ARCH=arm64

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
```

### 9.6 实验操作步骤

```bash
# === 步骤 1: 编译模块 ===
cd /path/to/dma_test_modules/
make -C /path/to/linux-6.18.1 M=$(pwd) modules ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

# === 步骤 2: 将 .ko 文件放入 rootfs ===
# (打包到 initramfs 或通过 9p/virtio-fs 共享)

# === 步骤 3: 在 QEMU 中加载测试 ===

# 实验1: Coherent DMA 测试
insmod dma_coherent_test.ko
dmesg | grep dma_test
rmmod dma_coherent_test

# 预期输出:
# dma_test: === Coherent DMA Allocation Test ===
# dma_test: DMA mask set to 64-bit
# dma_test: Coherent alloc OK
# dma_test:   CPU virtual addr : ffff0000xxxxxxxx
# dma_test:   DMA bus addr     : 0x00000000xxxxxxxx
# dma_test:   Physical addr    : 0x00000000xxxxxxxx
# dma_test:   dma_coherent     : 1          ← QEMU virt 默认 coherent
# dma_test: Coherent R/W verify PASSED (4096 bytes)

# 实验2: Streaming DMA 测试
insmod dma_streaming_test.ko
dmesg | grep dma_stream
rmmod dma_streaming_test

# 预期输出:
# dma_stream: === Streaming DMA Mapping Test ===
# dma_stream: map_single(TO_DEVICE) took XXX ns
# dma_stream:   buf virt=ffff... phys=0x... dma=0x...
# dma_stream: unmap_single(TO_DEVICE) took XXX ns
# dma_stream: sync_for_cpu(FROM_DEVICE) took XXX ns
# dma_stream: avg map_single(64KB, TO_DEVICE) = XXX ns (100 iterations)

# 实验3: DMA Engine 测试（需要 DMA 控制器硬件支持）
insmod dma_engine_test.ko
dmesg | grep dma_engine
rmmod dma_engine_test

# 在 QEMU virt 上预期:
# dma_engine: No DMA MEMCPY channel available
# dma_engine: This is expected on QEMU virt without DMA controller
# 在真实硬件上:
# dma_engine: Got channel: dma0chan0
# dma_engine: DMA memcpy PASSED (16 KB in XXXXX ns)
```

### 9.7 实验观察要点

```
┌──────────────────────────────────────────────────────────────────────┐
│  观察点 1: DMA 地址 vs 物理地址                                       │
│  ─────────────────────────────────                                   │
│  在 QEMU virt (无 IOMMU) 上: DMA addr == Physical addr              │
│  在真实硬件 (有 SMMU) 上:     DMA addr 是 IOVA，≠ Physical addr      │
│                                                                      │
│  观察点 2: dma_coherent 标志                                         │
│  ─────────────────────────────                                       │
│  QEMU virt: dma_coherent = 1 (模拟硬件一致性)                        │
│  真实 SoC:  取决于设备树 dma-coherent 属性                            │
│             非 coherent 设备的 map/sync 耗时会明显更长(Cache操作)      │
│                                                                      │
│  观察点 3: Streaming vs Coherent 性能                                │
│  ──────────────────────────────────                                   │
│  Streaming map/unmap 有开销 (Cache flush)，但单次传输后即释放          │
│  Coherent 分配较重（可能 vmalloc + 页表修改），但后续无 sync 开销      │
│  小缓冲区频繁使用 → Streaming 更快                                    │
│  大缓冲区长期共享 → Coherent 更合适                                   │
│                                                                      │
│  观察点 4: QEMU 加 IOMMU 对比                                       │
│  ────────────────────────────                                        │
│  # 不加 IOMMU:                                                       │
│  qemu-system-aarch64 -machine virt ...                               │
│  → DMA addr = Physical addr (直接映射)                               │
│                                                                      │
│  # 加 SMMU:                                                         │
│  qemu-system-aarch64 -machine virt,iommu=smmuv3 ...                 │
│  → DMA addr = IOVA (SMMU 翻译后的地址)                               │
│  → 内核日志会显示 SMMU 初始化信息                                     │
│                                                                      │
│  对比两种模式下 dma_map_single 返回的地址和耗时差异                    │
└──────────────────────────────────────────────────────────────────────┘
```

### 9.8 进阶实验：QEMU 开启 SMMU 对比

```bash
# 无 IOMMU 启动
qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 1G \
    -kernel Image -initrd rootfs.cpio.gz \
    -append "console=ttyAMA0" -nographic

# 有 SMMUv3 启动
qemu-system-aarch64 -machine virt,iommu=smmuv3 -cpu cortex-a72 -m 1G \
    -kernel Image -initrd rootfs.cpio.gz \
    -append "console=ttyAMA0" -nographic

# 在两种模式下分别加载 dma_streaming_test.ko 模块
# 对比:
# 1. dma_map_single 返回的 DMA 地址是否不同
# 2. map/unmap 耗时是否有差异（IOMMU 路径有额外开销）
# 3. dmesg 中是否有 SMMU 相关初始化日志
```

---

## 附录：源码文件索引

| 文件路径 | 功能说明 |
|----------|---------|
| `include/linux/dma-mapping.h` | DMA Mapping API 头文件 |
| `include/linux/dma-direction.h` | DMA 方向枚举 |
| `include/linux/dmaengine.h` | DMA Engine API 头文件 |
| `include/linux/dma-map-ops.h` | DMA 操作表结构体 |
| `kernel/dma/mapping.c` | DMA Mapping 核心实现（路由层） |
| `kernel/dma/direct.c` | 直接 DMA 映射实现 |
| `kernel/dma/swiotlb.c` | Bounce Buffer 实现 |
| `kernel/dma/coherent.c` | Coherent DMA 内存池 |
| `kernel/dma/contiguous.c` | CMA 连续内存分配 |
| `drivers/dma/dmaengine.c` | DMA Engine 框架核心 |
| `arch/arm64/mm/dma-mapping.c` | ARM64 平台 DMA Cache 维护 |
| `drivers/iommu/arm/arm-smmu-v3/` | ARM SMMUv3 驱动 |
| `drivers/iommu/dma-iommu.c` | IOMMU-DMA 集成层 |

---

> **学习建议**：建议按 "硬件基础 → Cache 一致性 → Mapping API → Engine API → IOMMU" 的顺序逐步深入，结合实际驱动代码（如网络驱动的 DMA 使用）加深理解。
