# ARM64 架构 Linux 内核同步、软中断、延迟机制与通信机制深度分析

> 基于 Linux 6.18.1 内核源码分析，聚焦 ARM64 架构实现细节。

---

## 目录

<details>
<summary><a href="#一arm64-架构下内核同步关键接口">一、ARM64 架构下内核同步关键接口</a></summary>

- [1.1 原子操作 (Atomic Operations)](#11-原子操作-atomic-operations)
- [1.2 内存屏障 (Memory Barriers)](#12-内存屏障-memory-barriers)
- [1.3 自旋锁 (Spinlock / qspinlock)](#13-自旋锁-spinlock--qspinlock)
- [1.4 读写自旋锁 (Read-Write Spinlock)](#14-读写自旋锁-read-write-spinlock)
- [1.5 互斥锁 (Mutex)](#15-互斥锁-mutex)
- [1.6 读写信号量 (Read-Write Semaphore)](#16-读写信号量-read-write-semaphore)
- [1.7 计数信号量 (Semaphore)](#17-计数信号量-semaphore)
- [1.8 完成量 (Completion)](#18-完成量-completion)
- [1.9 RCU (Read-Copy-Update)](#19-rcu-read-copy-update)
- [1.10 Per-CPU 变量与操作](#110-per-cpu-变量与操作)
- [1.11 同步机制选型对比](#111-同步机制选型对比)

</details>

<details>
<summary><a href="#二内核软中断的实现与原理">二、内核软中断的实现与原理</a></summary>

- [2.1 软中断概述](#21-软中断概述)
- [2.2 软中断向量表](#22-软中断向量表)
- [2.3 核心数据结构](#23-核心数据结构)
- [2.4 软中断触发机制](#24-软中断触发机制)
- [2.5 软中断执行流程](#25-软中断执行流程)
- [2.6 ksoftirqd 内核线程](#26-ksoftirqd-内核线程)
- [2.7 Tasklet 机制](#27-tasklet-机制)
- [2.8 硬中断与软中断的关系](#28-硬中断与软中断的关系)

</details>

<details>
<summary><a href="#三workqueuetasklet-与内核线程的区别">三、Workqueue、Tasklet 与内核线程的区别</a></summary>

- [3.1 Workqueue (工作队列)](#31-workqueue-工作队列)
- [3.2 Tasklet (小任务)](#32-tasklet-小任务)
- [3.3 Kernel Thread (内核线程)](#33-kernel-thread-内核线程)
- [3.4 三者对比总结](#34-三者对比总结)
- [3.5 选型指南](#35-选型指南)

</details>

<details>
<summary><a href="#四内核通信机制">四、内核通信机制</a></summary>

- [4.1 进程间通信 (IPC) 概览](#41-进程间通信-ipc-概览)
- [4.2 信号 (Signals)](#42-信号-signals)
- [4.3 管道与 FIFO (Pipes)](#43-管道与-fifo-pipes)
- [4.4 System V IPC](#44-system-v-ipc)
- [4.5 POSIX 消息队列](#45-posix-消息队列)
- [4.6 Socket 套接字](#46-socket-套接字)
- [4.7 Netlink](#47-netlink)
- [4.8 Futex (快速用户态互斥)](#48-futex-快速用户态互斥)
- [4.9 Eventfd / Signalfd / Timerfd](#49-eventfd--signalfd--timerfd)
- [4.10 Procfs / Sysfs](#410-procfs--sysfs)
- [4.11 通知链 (Notifier Chains)](#411-通知链-notifier-chains)
- [4.12 通信机制总结对比](#412-通信机制总结对比)

</details>

<details>
<summary><a href="#附录-arm64-硬件同步指令速查">附录: ARM64 硬件同步指令速查</a></summary>

</details>

---

## 一、ARM64 架构下内核同步关键接口

### 1.1 原子操作 (Atomic Operations)

**源码**: `arch/arm64/include/asm/atomic.h`, `arch/arm64/include/asm/atomic_ll_sc.h`, `arch/arm64/include/asm/atomic_lse.h`

ARM64 提供两套原子操作实现，通过 `alternative` 机制在运行时自动选择：

#### LL/SC (Load-Link / Store-Conditional) 路径

传统 ARMv8 原子实现，使用独占访问指令：

```asm
prfm    pstl1strm, %2      // 预取到 L1 缓存 (store 流)
1:  ldxr    %w0, %2         // Load-Exclusive: 读取并标记独占
    add     %w0, %w0, %w3   // 执行操作
    stxr    %w1, %w0, %2    // Store-Exclusive: 条件写入
    cbnz    %w1, 1b          // 如果写入失败则重试
```

带 ordering 的变体使用 `ldaxr` (acquire) / `stlxr` (release)。

#### LSE (Large System Extensions) 路径

ARMv8.1 引入的硬件原子指令，在多核系统下性能更优：

| LSE 指令 | 功能 | 说明 |
|----------|------|------|
| `LDADD` / `STADD` | 原子加法 | `LDADD` 返回旧值，`STADD` 不返回 |
| `LDCLR` / `STCLR` | 原子位清除 | AND NOT 操作 |
| `LDSET` / `STSET` | 原子位设置 | OR 操作 |
| `LDEOR` / `STEOR` | 原子异或 | XOR 操作 |
| `CAS` / `CASA` / `CASAL` | 比较并交换 | 带 acquire/release 变体 |
| `SWP` | 原子交换 | 交换内存值 |

#### 运行时选择机制

```c
// arch/arm64/include/asm/lse.h
#define __lse_ll_sc_body(op, ...)                       \
    alternative_has_cap_likely(ARM64_HAS_LSE_ATOMICS) ? \
        __lse_##op(__VA_ARGS__) : __ll_sc_##op(__VA_ARGS__)
```

#### 关键 API

| API | 描述 | Ordering |
|-----|------|----------|
| `atomic_read(v)` | 读取原子值 | 无 |
| `atomic_set(v, i)` | 设置原子值 | 无 |
| `atomic_add(i, v)` | 原子加法 | relaxed |
| `atomic_sub(i, v)` | 原子减法 | relaxed |
| `atomic_add_return(i, v)` | 加并返回新值 | full barrier |
| `atomic_fetch_add(i, v)` | 加并返回旧值 | full barrier |
| `atomic_cmpxchg(v, old, new)` | 比较并交换 | full barrier |
| `atomic_inc(v)` / `atomic_dec(v)` | 递增/递减 | relaxed |
| `atomic_dec_and_test(v)` | 递减并测试是否为零 | full barrier |

所有 API 都有 `_relaxed`、`_acquire`、`_release` 后缀变体，以及 64 位版本 `atomic64_*`。

---

### 1.2 内存屏障 (Memory Barriers)

**源码**: `arch/arm64/include/asm/barrier.h`

ARM64 使用弱内存模型，内核通过以下屏障确保内存访问顺序：

#### ARM64 屏障指令

| 内核宏 | ARM64 指令 | 作用范围 | 说明 |
|--------|-----------|---------|------|
| `mb()` | `dsb sy` | 全系统 | 完全数据同步屏障 |
| `rmb()` | `dsb ld` | 全系统 | 读屏障 |
| `wmb()` | `dsb st` | 全系统 | 写屏障 |
| `smp_mb()` | `dmb ish` | 内部共享域 | SMP 完全屏障 |
| `smp_rmb()` | `dmb ishld` | 内部共享域 | SMP 读屏障 |
| `smp_wmb()` | `dmb ishst` | 内部共享域 | SMP 写屏障 |
| `isb()` | `isb` | 本 CPU | 指令同步屏障 |

#### Acquire / Release 语义

ARM64 硬件直接支持 acquire/release 语义：

```c
// smp_store_release() → STLR (store-release)
// smp_load_acquire()  → LDAR (load-acquire)
```

`STLR` 确保之前的所有内存访问在 store 之前完成；`LDAR` 确保之后的所有内存访问在 load 之后开始。

#### 条件等待 (低功耗轮询)

```c
// smp_cond_load_relaxed(ptr, cond_expr)
// 使用 WFE (Wait For Event) 指令，在等待条件满足时降低功耗
// 比纯忙等 (busy-loop) 更省电
```

#### WFE / SEV 机制

| 指令 | 作用 |
|------|------|
| `WFE` (Wait For Event) | CPU 进入低功耗等待，直到事件到达 |
| `SEV` (Send Event) | 向所有 CPU 发送事件，唤醒 WFE |
| `WFI` (Wait For Interrupt) | 等待中断到达 |

ARM64 的 store-release (`STLR`) 操作会隐式产生 event，唤醒在 WFE 上等待的 CPU。

---

### 1.3 自旋锁 (Spinlock / qspinlock)

**源码**: `include/asm-generic/qspinlock_types.h`, `include/asm-generic/qspinlock.h`, `kernel/locking/qspinlock.c`

ARM64 使用 **queued spinlock (qspinlock)**，基于 MCS 锁算法，避免所有等待 CPU 在同一缓存行上竞争。

#### 数据结构

```c
typedef struct qspinlock {
    union {
        atomic_t val;              // 完整 32 位字
        struct {
            u8 locked;             // 锁定字节 (0=未锁, 1=已锁)
            u8 pending;            // 等待字节 (第二竞争者)
        };
        struct {
            u16 locked_pending;    // locked + pending 组合
            u16 tail;              // MCS 队列尾 (CPU + 上下文编码)
        };
    };
} arch_spinlock_t;
```

#### 加锁流程

```
Fast path:  CAS(val, 0, _Q_LOCKED_VAL)   -- 无竞争直接获取
            ↓ 失败
Pending:    设置 pending 位，等待 locked 释放
            ↓ 仍失败
MCS Queue:  入队到 per-CPU MCS 节点链表
            在本地变量上自旋 (不在共享锁上)
            轮到自己时获取锁
```

#### 关键 API

| API | 描述 |
|-----|------|
| `spin_lock(lock)` | 获取自旋锁 (禁止抢占) |
| `spin_unlock(lock)` | 释放自旋锁 |
| `spin_lock_irqsave(lock, flags)` | 获取锁并关中断 |
| `spin_lock_bh(lock)` | 获取锁并关下半部 |
| `spin_trylock(lock)` | 尝试获取，失败不等待 |
| `spin_is_locked(lock)` | 检查锁状态 |

#### ARM64 特有优化

- **等待阶段**: 使用 `smp_cond_load_acquire()` → 内部调用 `__cmpwait_relaxed()` → **WFE** 指令低功耗等待
- **释放阶段**: `smp_store_release(&lock->locked, 0)` → **STLR** 指令，隐式发送 event 唤醒 WFE
- 相比 ticket lock，qspinlock 的 NUMA 友好性更好

---

### 1.4 读写自旋锁 (Read-Write Spinlock)

**源码**: `include/linux/rwlock.h`, `include/asm-generic/qrwlock.h`

ARM64 使用 **queued rwlock (qrwlock)**，基于 qspinlock 实现。

#### 关键 API

| API | 描述 |
|-----|------|
| `read_lock(lock)` | 获取读锁 (多个读者可并发) |
| `read_unlock(lock)` | 释放读锁 |
| `write_lock(lock)` | 获取写锁 (独占) |
| `write_unlock(lock)` | 释放写锁 |
| `read_lock_irqsave()` | 读锁 + 关中断 |
| `write_lock_irqsave()` | 写锁 + 关中断 |

#### 特点

- 多个 reader 可以同时持有锁
- writer 必须等待所有 reader 释放
- writer 之间互斥
- 写者优先级较高，避免写者饿死

---

### 1.5 互斥锁 (Mutex)

**源码**: `include/linux/mutex_types.h`, `kernel/locking/mutex.c`

互斥锁是内核中最常用的同步原语之一，**仅可在进程上下文使用**。

#### 数据结构

```c
struct mutex {
    atomic_long_t           owner;    // 持有者 task_struct + 标志位
    raw_spinlock_t          wait_lock;
    struct optimistic_spin_queue osq;  // MCS 乐观自旋队列
    struct list_head        wait_list; // 等待者链表
};
```

**Owner 标志位**:
- `MUTEX_FLAG_WAITERS` — 有等待者
- `MUTEX_FLAG_HANDOFF` — 防止饿死的移交机制
- `MUTEX_FLAG_PICKUP` — 锁已被移交给等待者

#### 三级加锁路径

```
1. Fast path:   CAS(owner, 0, current)
                ARM64: CASA/CASAL (LSE) 或 LDAXR/STXR (LL/SC)
                ↓ 失败
2. Mid path:    Optimistic spinning (osq_lock)
                持有者在另一 CPU 运行时，自旋等待而非睡眠
                ↓ 自旋超时或持有者睡眠
3. Slow path:   加入 wait_list，schedule() 睡眠
                被 unlock 唤醒
```

#### 关键 API

| API | 描述 |
|-----|------|
| `mutex_init(lock)` | 初始化互斥锁 |
| `mutex_lock(lock)` | 加锁 (可睡眠) |
| `mutex_unlock(lock)` | 解锁 |
| `mutex_trylock(lock)` | 尝试加锁，不等待 |
| `mutex_lock_interruptible(lock)` | 可被信号中断的加锁 |
| `mutex_lock_killable(lock)` | 可被致命信号中断 |
| `mutex_is_locked(lock)` | 检查锁状态 |
| `mutex_destroy(lock)` | 销毁锁 (debug) |

---

### 1.6 读写信号量 (Read-Write Semaphore)

**源码**: `include/linux/rwsem.h`, `kernel/locking/rwsem.c`

#### 数据结构

```c
struct rw_semaphore {
    atomic_long_t count;               // 读者计数 + 写者锁定位
    atomic_long_t owner;               // 写者 owner (用于 spin-on-owner)
    struct optimistic_spin_queue osq;  // 乐观自旋队列
    raw_spinlock_t wait_lock;
    struct list_head wait_list;
};
```

#### 关键 API

| API | 描述 |
|-----|------|
| `down_read(sem)` | 获取读信号量 |
| `up_read(sem)` | 释放读信号量 |
| `down_write(sem)` | 获取写信号量 |
| `up_write(sem)` | 释放写信号量 |
| `down_read_trylock(sem)` | 尝试获取读锁 |
| `down_write_trylock(sem)` | 尝试获取写锁 |
| `downgrade_write(sem)` | 写锁降级为读锁 |

---

### 1.7 计数信号量 (Semaphore)

**源码**: `include/linux/semaphore.h`, `kernel/locking/semaphore.c`

#### 数据结构

```c
struct semaphore {
    raw_spinlock_t  lock;      // 保护内部状态的自旋锁
    unsigned int    count;     // 计数值
    struct list_head wait_list; // 等待队列
};
```

#### 关键 API

| API | 描述 |
|-----|------|
| `sema_init(sem, val)` | 初始化，设置计数值 |
| `down(sem)` | P 操作 (count--, 可能睡眠) |
| `down_interruptible(sem)` | 可被信号中断 |
| `down_killable(sem)` | 可被致命信号中断 |
| `down_trylock(sem)` | 尝试获取，不睡眠 |
| `down_timeout(sem, jiffies)` | 带超时的获取 |
| `up(sem)` | V 操作 (count++, 唤醒等待者) |

> **注意**: 现代内核代码优先使用 `mutex` 而非二值信号量 (`count=1`)。信号量主要用于计数场景。

---

### 1.8 完成量 (Completion)

**源码**: `include/linux/completion.h`, `kernel/sched/completion.c`

用于一个或多个线程等待某个事件完成的同步原语。

#### 数据结构

```c
struct completion {
    unsigned int done;              // 完成计数
    struct swait_queue_head wait;   // 简单等待队列 (FIFO)
};
```

#### 关键 API

| API | 描述 |
|-----|------|
| `init_completion(x)` | 初始化 (done=0) |
| `reinit_completion(x)` | 重新初始化 (done=0) |
| `complete(x)` | done++, 唤醒一个等待者 |
| `complete_all(x)` | done=UINT_MAX, 唤醒所有等待者 |
| `wait_for_completion(x)` | 等待完成 (不可中断) |
| `wait_for_completion_timeout(x, t)` | 带超时等待 |
| `wait_for_completion_interruptible(x)` | 可被信号中断 |
| `wait_for_completion_killable(x)` | 可被致命信号中断 |
| `try_wait_for_completion(x)` | 非阻塞检查 |
| `completion_done(x)` | 检查是否完成 |

#### 典型使用模式

```c
DECLARE_COMPLETION(setup_done);

// 线程 A: 等待初始化完成
wait_for_completion(&setup_done);

// 线程 B: 完成初始化后通知
do_setup();
complete(&setup_done);
```

---

### 1.9 RCU (Read-Copy-Update)

**源码**: `include/linux/rcupdate.h`, `kernel/rcu/`

RCU 是一种针对读多写少场景的高性能同步机制。读者几乎零开销。

#### 核心概念

```
Reader:                         Writer:
rcu_read_lock()                 old = rcu_dereference(gp);
  p = rcu_dereference(gp);     new = kmalloc(...);
  use(p);                      *new = *old;
rcu_read_unlock()               modify(new);
                                rcu_assign_pointer(gp, new);
                                synchronize_rcu();  // 等待宽限期
                                kfree(old);
```

#### 关键 API

| API | ARM64 实现 | 描述 |
|-----|-----------|------|
| `rcu_read_lock()` | `preempt_disable()` (非 PREEMPT_RCU) | 进入 RCU 读侧临界区 |
| `rcu_read_unlock()` | `preempt_enable()` | 离开 RCU 读侧临界区 |
| `rcu_dereference(p)` | `READ_ONCE(p)` | 读取 RCU 保护的指针 |
| `rcu_assign_pointer(p, v)` | `smp_store_release()` → **STLR** | 发布 RCU 保护的指针 |
| `synchronize_rcu()` | 等待宽限期 | 等待所有现有读者完成 |
| `call_rcu(head, func)` | 注册延迟回调 | 宽限期后异步回调 |
| `rcu_dereference_protected(p, c)` | 无屏障读取 | 更新侧使用 (持有锁) |

#### ARM64 特点

- `rcu_dereference()` 使用 `READ_ONCE()` 即可 — ARM64 天然支持地址依赖排序，不需要额外屏障（Alpha 架构需要 `smp_read_barrier_depends()`）
- `rcu_assign_pointer()` 使用 ARM64 **STLR** 指令实现 store-release 语义

---

### 1.10 Per-CPU 变量与操作

**源码**: `arch/arm64/include/asm/percpu.h`

#### ARM64 实现

CPU 偏移量存储在 **`tpidr_el1`** 寄存器（VHE 模式下为 `tpidr_el2`），通过 `MRS` 指令读取，避免内存访问。

Per-CPU 原子操作同样有 LL/SC 和 LSE 双路径：

```c
// LSE 路径示例 (this_cpu_add):
ldadd val, tmp, [ptr]     // 硬件原子加法

// LL/SC 路径示例:
1: ldxr  tmp, [ptr]
   add   tmp, tmp, val
   stxr  loop, tmp, [ptr]
   cbnz  loop, 1b
```

#### 关键 API

| API | 描述 |
|-----|------|
| `this_cpu_read(var)` | 读取当前 CPU 的变量 |
| `this_cpu_write(var, val)` | 写入当前 CPU 的变量 |
| `this_cpu_add(var, val)` | 原子加法 |
| `this_cpu_inc(var)` | 原子递增 |
| `this_cpu_dec(var)` | 原子递减 |
| `per_cpu(var, cpu)` | 访问指定 CPU 的变量 |
| `get_cpu_var(var)` | 禁止抢占 + 访问 |
| `put_cpu_var(var)` | 恢复抢占 |

---

### 1.11 同步机制选型对比

| 机制 | 上下文 | 可睡眠 | 开销 | 典型场景 |
|------|--------|--------|------|----------|
| **原子操作** | 任意 | — | 极低 | 计数器、标志位、引用计数 |
| **自旋锁** | 任意 (中断/进程) | 否 | 低 | 短临界区、中断处理 |
| **读写自旋锁** | 任意 | 否 | 低 | 读多写少的短临界区 |
| **互斥锁** | 仅进程上下文 | 是 | 中 | 长临界区、I/O 操作 |
| **信号量** | 仅进程上下文 | 是 | 中 | 计数资源、生产者消费者 |
| **读写信号量** | 仅进程上下文 | 是 | 中 | 读多写少的长临界区 |
| **完成量** | 仅进程上下文 | 是 | 低 | 等待事件完成 |
| **RCU** | 读: 任意; 写: 进程 | 读: 否; 写: 是 | 读: 极低 | 读极多写极少的数据结构 |
| **Per-CPU** | 禁止抢占 | — | 极低 | 统计计数、per-CPU 缓存 |

---

## 二、内核软中断的实现与原理

### 2.1 软中断概述

软中断 (softirq) 是 Linux 内核延迟处理机制的基础层，用于将硬中断中不紧急的工作推迟到更安全的上下文中执行。

**核心特征**:
- 运行在中断上下文（下半部），不可睡眠
- 静态注册，编译时确定数量
- 同一类型软中断可在多个 CPU 上并行运行
- 优先级高于进程上下文，低于硬中断

### 2.2 软中断向量表

**源码**: `include/linux/interrupt.h`

```c
enum {
    HI_SOFTIRQ = 0,         // 高优先级 tasklet
    TIMER_SOFTIRQ,           // 定时器
    NET_TX_SOFTIRQ,          // 网络发送
    NET_RX_SOFTIRQ,          // 网络接收
    BLOCK_SOFTIRQ,           // 块设备
    IRQ_POLL_SOFTIRQ,        // IRQ 轮询
    TASKLET_SOFTIRQ,         // 普通 tasklet
    SCHED_SOFTIRQ,           // 调度器负载均衡
    HRTIMER_SOFTIRQ,         // 高精度定时器
    RCU_SOFTIRQ,             // RCU 处理
    NR_SOFTIRQS              // 总数 (10)
};
```

优先级从 0 (最高) 到 9 (最低)，处理时按序号顺序执行。

### 2.3 核心数据结构

```c
// 软中断处理函数
struct softirq_action {
    void (*action)(void);    // 6.18.1 中简化的函数签名
};

// 全局软中断向量表
static struct softirq_action softirq_vec[NR_SOFTIRQS];

// Per-CPU 软中断 pending 位图
typedef struct {
    unsigned int __softirq_pending;   // 每位对应一种软中断
    // ...
} irq_cpustat_t;
```

### 2.4 软中断触发机制

#### raise_softirq()

```c
void raise_softirq(unsigned int nr)
{
    unsigned long flags;
    local_irq_save(flags);          // 关闭本地中断
    raise_softirq_irqoff(nr);      // 设置 pending 位
    local_irq_restore(flags);       // 恢复中断
}

inline void raise_softirq_irqoff(unsigned int nr)
{
    __raise_softirq_irqoff(nr);     // or_softirq_pending(1UL << nr)
    if (!in_interrupt() && should_wake_ksoftirqd())
        wakeup_softirqd();          // 唤醒 ksoftirqd
}
```

#### 触发时机

1. **硬中断返回时**: `irq_exit()` → 检查 `local_softirq_pending()` → `__do_softirq()`
2. **代码显式调用**: `raise_softirq()` / `raise_softirq_irqoff()`
3. **本地 BH 开启时**: `local_bh_enable()` → 检查 pending → 执行
4. **ksoftirqd 线程**: 当软中断积压时由内核线程处理

### 2.5 软中断执行流程

#### `__do_softirq()` 核心处理

```
__do_softirq():
    1. 保存并清除 pending 位图 (原子操作)
    2. 开中断 (__local_bh_enable)
    3. 循环遍历所有置位的软中断:
       for each bit set in pending:
           h->action()              // 调用注册的处理函数
    4. 检查是否有新的 pending
    5. 如果有新的 pending 且未超时/未超过重入次数:
       goto restart (最多 MAX_SOFTIRQ_RESTART = 10 次)
    6. 如果仍有 pending，唤醒 ksoftirqd 线程处理
    7. 关中断，恢复状态
```

#### irq_exit() 中的调用路径

```
硬中断处理完成
  → irq_exit_rcu()
    → preempt_count_sub(HARDIRQ_OFFSET)
    → if (!in_interrupt() && local_softirq_pending())
        → __do_softirq()
```

### 2.6 ksoftirqd 内核线程

每个 CPU 都有一个 `ksoftirqd/N` 内核线程，用于处理积压的软中断：

```c
// 运行逻辑:
static void run_ksoftirqd(unsigned int cpu)
{
    ksoftirqd_run_begin();
    if (local_softirq_pending()) {
        __do_softirq();
        ksoftirqd_run_end();
        cond_resched();      // 主动让出 CPU
        return;
    }
    ksoftirqd_run_end();
}
```

**触发条件**:
- `__do_softirq()` 重试次数超过 `MAX_SOFTIRQ_RESTART`
- `__do_softirq()` 执行时间超过 `MAX_SOFTIRQ_TIME` (2ms)
- `raise_softirq()` 在进程上下文中调用

### 2.7 Tasklet 机制

Tasklet 是基于软中断实现的更灵活的延迟处理机制。

#### 数据结构

```c
struct tasklet_struct {
    struct tasklet_struct *next;     // 单向链表
    unsigned long state;             // TASKLET_STATE_SCHED | TASKLET_STATE_RUN
    atomic_t count;                  // 禁用深度 (0=启用)
    bool use_callback;
    union {
        void (*func)(unsigned long data);        // 旧 API
        void (*callback)(struct tasklet_struct *); // 新 API
    };
    unsigned long data;
};
```

#### 状态标志

| 状态 | 含义 |
|------|------|
| `TASKLET_STATE_SCHED` | 已调度，等待执行 |
| `TASKLET_STATE_RUN` | 正在某 CPU 上执行 (SMP 序列化) |

#### 调度流程

```
tasklet_schedule(t):
    1. test_and_set_bit(TASKLET_STATE_SCHED, &t->state)
       如果已经 SCHED 状态，直接返回 (幂等)
    2. local_irq_save(flags)
    3. 将 tasklet 加入 per-CPU 的 tasklet_vec 链表
    4. raise_softirq_irqoff(TASKLET_SOFTIRQ)
    5. local_irq_restore(flags)
```

#### 执行流程

```
tasklet_action_common():
    1. 关中断，原子取走整个 per-CPU tasklet 链表
    2. 遍历每个 tasklet:
       a. tasklet_trylock(t)        // 设置 TASKLET_STATE_RUN
          → 同一 tasklet 不会在两个 CPU 上同时执行
       b. 检查 count == 0 (未被 disable)
       c. 清除 TASKLET_STATE_SCHED
       d. 调用 t->callback(t) 或 t->func(t->data)
       e. tasklet_unlock(t)
    3. 如果 trylock 失败或 count != 0:
       重新入队，再次 raise_softirq
```

#### 关键 API

| API | 描述 |
|-----|------|
| `tasklet_setup(t, callback)` | 初始化 (新 API) |
| `tasklet_schedule(t)` | 调度执行 (TASKLET_SOFTIRQ) |
| `tasklet_hi_schedule(t)` | 高优先级调度 (HI_SOFTIRQ) |
| `tasklet_disable(t)` | 禁用并等待完成 |
| `tasklet_disable_nosync(t)` | 禁用不等待 |
| `tasklet_enable(t)` | 启用 |
| `tasklet_kill(t)` | 等待完成并移除 |

#### Tasklet 关键特性

- **同一 tasklet 不会并发执行** (TASKLET_STATE_RUN 保证 SMP 序列化)
- **不同 tasklet 可以并发执行**
- 运行在软中断上下文，**不可睡眠**
- 比直接注册软中断更灵活（可动态创建）

### 2.8 硬中断与软中断的关系

```
┌─────────────────────────────────────────────────────┐
│                    进程上下文                          │
│  (可抢占, 可睡眠, 优先级最低)                           │
├─────────────────────────────────────────────────────┤
│                  软中断 (下半部)                       │
│  (不可睡眠, 可被硬中断打断, 同类可并行)                   │
│  ├── TASKLET_SOFTIRQ → tasklet_action()             │
│  ├── TIMER_SOFTIRQ   → run_timer_softirq()         │
│  ├── NET_RX_SOFTIRQ  → net_rx_action()             │
│  └── ...                                            │
├─────────────────────────────────────────────────────┤
│                  硬中断 (上半部)                       │
│  (不可抢占, 不可睡眠, 优先级最高)                        │
│  快速处理 → raise_softirq() → 退出硬中断               │
└─────────────────────────────────────────────────────┘
```

| 对比项 | 硬中断 (Hardirq) | 软中断 (Softirq) |
|--------|-----------------|-----------------|
| 触发源 | 硬件设备 | 代码 raise_softirq() |
| 优先级 | 最高 | 高于进程，低于硬中断 |
| 可嵌套 | 不同 IRQ 号可嵌套 | 不嵌套 (单 CPU) |
| 并发性 | 不同 CPU 可并行 | 同类型可在不同 CPU 并行 |
| 可睡眠 | 否 | 否 |
| 数量 | 硬件决定 | 固定 10 种 |
| 注册 | `request_irq()` | `open_softirq()` |

---

## 三、Workqueue、Tasklet 与内核线程的区别

### 3.1 Workqueue (工作队列)

**源码**: `kernel/workqueue.c`, `include/linux/workqueue.h`

#### 核心数据结构

```c
// 工作项
struct work_struct {
    atomic_long_t data;       // 编码 pool ID、flags、pwq 指针
    struct list_head entry;   // 工作链表节点
    work_func_t func;         // void (*)(struct work_struct *)
};

// 延迟工作项
struct delayed_work {
    struct work_struct work;
    struct timer_list timer;
    struct workqueue_struct *wq;
    int cpu;
};

// 工作线程池 (每 CPU 两个: 普通 + 高优先级)
struct worker_pool {
    struct list_head worklist;     // 待处理工作列表
    int nr_running;                // 运行中的 worker 数
    struct list_head idle_list;    // 空闲 worker 列表
    DECLARE_HASHTABLE(busy_hash);  // 忙碌 worker 哈希
    // ...
};

// worker 线程
struct worker {
    struct work_struct *current_work;
    work_func_t current_func;
    struct task_struct *task;
    struct worker_pool *pool;
    unsigned int flags;           // WORKER_IDLE, WORKER_CPU_INTENSIVE
};
```

#### cmwq (Concurrency Managed Workqueue) 架构

```
                    workqueue_struct
                   /        |        \
              pwq(cpu0)  pwq(cpu1)  pwq(cpu2)   ← pool_workqueue
                |           |          |
           worker_pool  worker_pool  worker_pool
           [kworker/0]  [kworker/1]  [kworker/2]  ← 共享线程池
```

**关键设计**:
- 每 CPU 两个 `worker_pool` (普通优先级 + 高优先级)
- 多个 workqueue 共享同一 worker_pool
- 并发管理：当 worker 阻塞时 (`nr_running` 减少)，自动唤醒新 worker
- 通过调度器钩子 `wq_worker_running()` / `wq_worker_sleeping()` 实现

#### 系统预定义工作队列

| 工作队列 | 标志 | 用途 |
|----------|------|------|
| `system_wq` | `WQ_PERCPU` | 默认，`schedule_work()` 使用 |
| `system_highpri_wq` | `WQ_HIGHPRI` | 高优先级工作 |
| `system_long_wq` | `WQ_PERCPU` | 长时间运行的工作 |
| `system_unbound_wq` | `WQ_UNBOUND` | 不绑定 CPU |
| `system_freezable_wq` | `WQ_FREEZABLE` | 挂起时冻结 |
| `system_bh_wq` | `WQ_BH` | 在软中断上下文执行 |
| `system_bh_highpri_wq` | `WQ_BH \| WQ_HIGHPRI` | 高优先级软中断 |

#### 创建 API

```c
// 推荐 API:
alloc_workqueue(fmt, flags, max_active, ...)
alloc_ordered_workqueue(fmt, flags, ...)     // 串行化执行

// 旧 API (legacy):
create_workqueue(name)
create_singlethread_workqueue(name)
```

#### 工作队列标志

| 标志 | 含义 |
|------|------|
| `WQ_BH` | 在软中断上下文执行 (不是进程上下文) |
| `WQ_UNBOUND` | 不绑定特定 CPU |
| `WQ_FREEZABLE` | 系统挂起时冻结 |
| `WQ_MEM_RECLAIM` | 内存压力下有 rescuer 线程保证前进 |
| `WQ_HIGHPRI` | 高优先级 (nice -20) |
| `WQ_CPU_INTENSIVE` | 排除出并发管理 |

#### 关键操作 API

| API | 描述 |
|-----|------|
| `INIT_WORK(work, func)` | 初始化工作项 |
| `schedule_work(work)` | 提交到 `system_percpu_wq` |
| `queue_work(wq, work)` | 提交到指定工作队列 |
| `queue_delayed_work(wq, dwork, delay)` | 延迟提交 |
| `flush_work(work)` | 等待工作完成 |
| `cancel_work_sync(work)` | 取消并等待完成 |
| `flush_workqueue(wq)` | 等待队列中所有工作完成 |
| `destroy_workqueue(wq)` | 销毁工作队列 |

---

### 3.2 Tasklet (小任务)

(详见 [2.7 Tasklet 机制](#27-tasklet-机制))

关键特点总结：
- 基于 `TASKLET_SOFTIRQ` / `HI_SOFTIRQ` 实现
- 运行在软中断上下文
- 同一 tasklet 序列化执行，不同 tasklet 可并发
- 不可睡眠
- **已被视为 legacy API**，新代码推荐使用 `WQ_BH` workqueue 或 threaded IRQ

---

### 3.3 Kernel Thread (内核线程)

**源码**: `kernel/kthread.c`, `include/linux/kthread.h`

#### 核心数据结构

```c
// 内部结构
struct kthread {
    unsigned long flags;          // KTHREAD_SHOULD_STOP, SHOULD_PARK
    unsigned int cpu, node;
    int (*threadfn)(void *);
    void *data;
    struct completion parked, exited;
    struct task_struct *task;
};

// kthread_worker: 类似 workqueue 的单线程工作者
struct kthread_worker {
    raw_spinlock_t lock;
    struct list_head work_list;
    struct list_head delayed_work_list;
    struct task_struct *task;
    struct kthread_work *current_work;
};
```

#### 创建流程

```
kthread_create() / kthread_run():
    1. 分配 kthread_create_info
    2. 加入 kthread_create_list
    3. 唤醒 kthreadd (PID 2)
    4. kthreadd 调用 kernel_thread() 创建新进程
    5. 新线程运行 kthread() 函数:
       - 设置 SCHED_NORMAL
       - 等待 wake_up_process() 调用
       - 运行 threadfn(data)
       - kthread_exit(ret)
```

#### 关键 API

| API | 描述 |
|-----|------|
| `kthread_create(fn, data, fmt, ...)` | 创建线程 (不自动运行) |
| `kthread_run(fn, data, fmt, ...)` | 创建并启动线程 |
| `kthread_stop(k)` | 请求停止并等待退出 |
| `kthread_should_stop()` | 线程内检查是否应停止 |
| `kthread_park(k)` | 暂停线程 |
| `kthread_unpark(k)` | 恢复线程 |
| `kthread_bind(k, cpu)` | 绑定到特定 CPU |
| `kthread_create_on_cpu(fn, data, cpu, fmt)` | 创建 CPU 绑定线程 |

#### kthread_worker API

```c
kthread_create_worker(flags, fmt, ...)    // 创建 worker + 线程
kthread_queue_work(worker, work)          // 提交工作
kthread_flush_work(work)                  // 等待完成
kthread_cancel_work_sync(work)            // 取消
kthread_destroy_worker(worker)            // 销毁
```

---

### 3.4 三者对比总结

| 特性 | Workqueue | Tasklet | 内核线程 |
|------|-----------|---------|---------|
| **执行上下文** | 进程上下文 (kworker) | 软中断上下文 | 进程上下文 (独立线程) |
| **可以睡眠** | **是** | **否** | **是** |
| **并发性** | 多 worker 并发 (cmwq) | 同 tasklet 串行 | 单线程 |
| **CPU 绑定** | 默认 per-CPU 或 unbound | per-CPU 调度 | 可配置绑定 |
| **内存分配** | `GFP_KERNEL` | 仅 `GFP_ATOMIC` | `GFP_KERNEL` |
| **延迟** | 较高 (CFS 调度) | 较低 (软中断优先级) | 可控 (可设 RT 优先级) |
| **优先级** | 普通或 nice -20 | 普通/高 (HI_SOFTIRQ) | 完全可控 |
| **共享资源** | 共享 kworker 线程池 | 共享 per-CPU 链表 | 独立 task_struct |
| **前进保证** | `WQ_MEM_RECLAIM` rescuer | 无 | 调用者负责 |
| **API 复杂度** | 简单 | 简单 | 中等 |
| **适用场景** | 通用延迟工作 | 快速 BH 处理 (legacy) | 长期运行守护任务 |

### 3.5 选型指南

```
需要延迟执行一些工作？
│
├─ 需要睡眠 (mutex/IO/kmalloc GFP_KERNEL)?
│   ├─ 是 → 需要专用线程 (RT优先级/CPU亲和)?
│   │   ├─ 是 → 内核线程 (kthread_create/kthread_worker)
│   │   └─ 否 → Workqueue (schedule_work / alloc_workqueue)
│   │
│   └─ 否 → 时延要求极高 (软中断优先级)?
│       ├─ 是 → WQ_BH workqueue (推荐) 或 tasklet (legacy)
│       └─ 否 → Workqueue
│
├─ 需要长期运行的守护进程？
│   └─ 内核线程
│
└─ 简单的一次性延迟工作？
    └─ schedule_work() (最简单)
```

---

## 四、内核通信机制

### 4.1 进程间通信 (IPC) 概览

```
┌────────────────────────────────────────────────────────────┐
│                     Linux 通信机制分类                        │
├──────────────┬─────────────────┬───────────────────────────┤
│  用户 ↔ 用户  │  用户 ↔ 内核     │    内核 ↔ 内核             │
├──────────────┼─────────────────┼───────────────────────────┤
│ 信号          │ Netlink          │ 通知链 (Notifier Chain)    │
│ 管道/FIFO     │ Procfs / Sysfs   │ 完成量 (Completion)        │
│ SysV 消息队列  │ ioctl            │ 等待队列 (Waitqueue)       │
│ SysV 共享内存  │ Eventfd          │ RCU                       │
│ SysV 信号量   │ 系统调用          │                            │
│ POSIX 消息队列 │                  │                            │
│ Socket        │                  │                            │
│ Futex         │                  │                            │
│ Signalfd      │                  │                            │
│ Timerfd       │                  │                            │
│ Eventfd       │                  │                            │
└──────────────┴─────────────────┴───────────────────────────┘
```

### 4.2 信号 (Signals)

**源码**: `kernel/signal.c`, `include/linux/signal.h`

#### 数据结构

```c
struct sigpending {
    struct list_head list;    // sigqueue 链表
    sigset_t signal;          // 信号位图
};

struct sigqueue {
    struct list_head list;
    int flags;
    kernel_siginfo_t info;    // 信号详细信息
    struct ucounts *ucounts;
};

struct k_sigaction {
    struct sigaction sa;       // handler, flags, mask
};
```

#### 信号发送流程

```
kill() 系统调用
  → kill_something_info()
    → group_send_sig_info()
      → do_send_sig_info()
        → send_signal_locked()
          → __send_signal_locked()
            → 分配 sigqueue
            → 加入 pending->list
            → sigaddset(&pending->signal, sig)
            → complete_signal()      // 选择目标线程并唤醒
```

#### 关键 API

| API | 描述 |
|-----|------|
| `send_sig(sig, task, priv)` | 内核向进程发送信号 |
| `send_sig_info(sig, info, task)` | 带信息的信号发送 |
| `force_sig(sig)` | 强制发送 (不可忽略/阻塞) |
| `signal_pending(task)` | 检查是否有待处理信号 |
| `flush_signals(task)` | 清除所有挂起信号 |

---

### 4.3 管道与 FIFO (Pipes)

**源码**: `fs/pipe.c`, `include/linux/pipe_fs_i.h`

#### 数据结构

```c
struct pipe_inode_info {
    struct mutex mutex;
    wait_queue_head_t rd_wait, wr_wait;  // 读/写等待队列
    pipe_index_t head, tail;              // 环形缓冲区头/尾
    unsigned int max_usage;               // 最大可用槽位
    unsigned int ring_size;               // 环大小 (2 的幂)
    struct pipe_buffer *bufs;             // 缓冲区数组
};

struct pipe_buffer {
    struct page *page;          // 数据所在页
    unsigned int offset, len;   // 页内偏移和长度
};
```

#### 特点

- 默认 16 个 buffer slot (`PIPE_DEF_BUFFERS`)，每个一页 (4KB)
- 环形缓冲区设计，`head` / `tail` 无锁读取 (`READ_ONCE`)
- 匿名管道通过 `pipe()` 系统调用创建
- FIFO 通过 `mkfifo()` 创建，有文件系统路径名
- 单向通信，半双工

---

### 4.4 System V IPC

#### 4.4.1 消息队列 (Message Queue)

**源码**: `ipc/msg.c`

```c
struct msg_queue {
    struct kern_ipc_perm q_perm;     // 权限
    unsigned long q_cbytes;          // 当前字节数
    unsigned long q_qnum;            // 消息数量
    unsigned long q_qbytes;          // 最大字节数
    struct list_head q_messages;     // 消息链表
    struct list_head q_receivers;    // 等待接收者
    struct list_head q_senders;      // 等待发送者
};
```

**系统调用**: `msgget()`, `msgsnd()`, `msgrcv()`, `msgctl()`

**特点**: 支持消息类型过滤，发送者/接收者可以阻塞等待。

#### 4.4.2 共享内存 (Shared Memory)

**源码**: `ipc/shm.c`

```c
struct shmid_kernel {
    struct kern_ipc_perm shm_perm;
    struct file *shm_file;          // 底层 shmem 文件
    unsigned long shm_nattch;       // 挂载次数
    unsigned long shm_segsz;        // 段大小
};
```

**系统调用**: `shmget()`, `shmat()`, `shmdt()`, `shmctl()`

**特点**:
- 底层使用 `tmpfs` (shmem_fs) 实现
- **最快的 IPC 方式** — 零拷贝共享内存
- 需要配合信号量或其他同步机制使用
- 支持大页 (hugetlb)

#### 4.4.3 信号量 (Semaphore)

**源码**: `ipc/sem.c`

```c
struct sem_array {
    struct kern_ipc_perm sem_perm;
    struct list_head pending_alter;
    struct list_head pending_const;
    int sem_nsems;                  // 信号量个数
    struct sem sems[];              // 信号量数组
};

struct sem {
    int semval;                     // 当前值
    spinlock_t lock;                // 细粒度锁
    struct list_head pending_alter;
    struct list_head pending_const;
};
```

**系统调用**: `semget()`, `semop()`, `semtimedop()`, `semctl()`

**特点**: 支持原子的多信号量操作，简单操作使用 per-sem 锁实现高可扩展性。

---

### 4.5 POSIX 消息队列

**源码**: `ipc/mqueue.c`

```c
struct mqueue_inode_info {
    spinlock_t lock;
    struct rb_root msg_tree;       // 红黑树 (按优先级排序)
    struct mq_attr attr;
    struct sigevent notify;        // 异步通知配置
    struct pid *notify_owner;
};
```

**系统调用**: `mq_open()`, `mq_unlink()`, `mq_timedsend()`, `mq_timedreceive()`, `mq_notify()`, `mq_getsetattr()`

#### 与 SysV 消息队列的区别

| 特性 | POSIX MQ | SysV MQ |
|------|----------|---------|
| 实现 | VFS 文件系统 | 内核 IPC 子系统 |
| 标识 | 文件路径名 | 整数 key/id |
| 优先级 | 红黑树排序 | 类型过滤 |
| 通知 | 支持异步通知 (信号/netlink) | 不支持 |
| 文件描述符 | 是 (可 epoll/select) | 否 |

---

### 4.6 Socket 套接字

**源码**: `net/socket.c`, `include/linux/net.h`

#### 数据结构

```c
struct socket {
    socket_state state;          // SS_CONNECTED, SS_UNCONNECTED
    short type;                  // SOCK_STREAM, SOCK_DGRAM
    struct file *file;
    struct sock *sk;             // 内部协议 socket
    const struct proto_ops *ops; // 协议操作表
    struct socket_wq wq;
};
```

#### Unix Domain Socket

**源码**: `net/unix/af_unix.c`

```c
struct unix_sock {
    struct sock sk;
    struct unix_address *addr;
    struct sock *peer;           // 连接对端
    struct mutex iolock;
};
```

**三种模式**:
- `SOCK_STREAM` — 面向连接的字节流
- `SOCK_DGRAM` — 无连接数据报
- `SOCK_SEQPACKET` — 有序数据包

**特性**:
- **FD 传递**: 通过 `SCM_RIGHTS` 在进程间传递文件描述符
- **凭据传递**: 通过 `SCM_CREDENTIALS` 传递 pid/uid/gid
- **抽象命名空间**: 以 `\0` 开头的名称，不在文件系统中

---

### 4.7 Netlink

**源码**: `net/netlink/af_netlink.c`, `include/linux/netlink.h`

Netlink 是 **用户空间与内核通信的主要机制**。

#### 数据结构

```c
struct netlink_sock {
    struct sock sk;
    u32 portid;                    // 本地端口 ID
    u32 dst_portid;                // 目标端口 ID
    unsigned long *groups;         // 组播组位图
    void (*netlink_rcv)(struct sk_buff *); // 内核接收回调
};

struct netlink_kernel_cfg {
    unsigned int groups;
    void (*input)(struct sk_buff *skb); // 消息处理函数
};
```

#### 关键 API

| API | 描述 |
|-----|------|
| `netlink_kernel_create(net, unit, cfg)` | 创建内核端 netlink socket |
| `netlink_unicast(sk, skb, portid, flags)` | 单播发送 |
| `netlink_broadcast(sk, skb, portid, group, flags)` | 组播发送 |
| `nlmsg_new(payload, flags)` | 创建 netlink 消息 |
| `nlmsg_put(skb, portid, seq, type, payload, flags)` | 添加消息头 |

#### 协议族

| 协议 | 用途 |
|------|------|
| `NETLINK_ROUTE` | 路由、网络设备配置 (iproute2) |
| `NETLINK_KOBJECT_UEVENT` | udev 设备事件 |
| `NETLINK_NETFILTER` | 防火墙 |
| `NETLINK_AUDIT` | 审计 |
| `NETLINK_GENERIC` | 通用 Netlink (多子系统复用) |
| `NETLINK_SOCK_DIAG` | Socket 诊断 |

#### Generic Netlink

**源码**: `net/netlink/genetlink.c`

多个内核子系统通过 `NETLINK_GENERIC` 复用单一协议号，避免 netlink 协议号耗尽。

---

### 4.8 Futex (快速用户态互斥)

**源码**: `kernel/futex/` (core.c, syscalls.c, waitwake.c, pi.c, requeue.c)

#### 数据结构

```c
union futex_key {
    struct {  /* 文件映射 (共享) */
        u64 i_seq;
        unsigned long pgoff;
        unsigned int offset;
    } shared;
    struct {  /* 匿名映射 (私有) */
        struct mm_struct *mm;
        unsigned long address;
        unsigned int offset;
    } private;
};

struct futex_q {
    struct plist_node list;      // 优先级排序
    struct task_struct *task;
    union futex_key key;
    struct futex_pi_state *pi_state;  // 优先级继承
    u32 bitset;                  // 选择性唤醒
};
```

#### 工作原理

```
用户态快速路径 (无竞争):
    CAS 操作直接获取锁，不进入内核

用户态慢速路径 (有竞争):
    futex_wait():
        1. 计算 futex_key (标识内存位置)
        2. 哈希到 futex_hash_bucket
        3. 验证 *uaddr == expected (避免丢失唤醒)
        4. 加入等待队列, schedule()

    futex_wake():
        1. 计算 futex_key
        2. 查找哈希桶中匹配的 futex_q
        3. wake_up_process(q->task)
```

#### 系统调用

| 系统调用 | 描述 |
|---------|------|
| `futex()` | 传统多功能系统调用 |
| `futex_wait()` | 等待单个 futex |
| `futex_wake()` | 唤醒等待者 |
| `futex_waitv()` | 同时等待多个 futex |
| `futex_requeue()` | 从一个 futex 转移等待者到另一个 |

**特性**: 支持优先级继承 (PI)、NUMA 感知哈希、per-process 私有哈希表。

---

### 4.9 Eventfd / Signalfd / Timerfd

#### Eventfd

**源码**: `fs/eventfd.c`

```c
struct eventfd_ctx {
    struct kref kref;
    wait_queue_head_t wqh;
    __u64 count;          // 64 位计数器
    unsigned int flags;   // EFD_SEMAPHORE, EFD_NONBLOCK
    int id;
};
```

| 操作 | 行为 |
|------|------|
| `write()` | 加值到 counter |
| `read()` | 返回 counter 并重置为 0 (`EFD_SEMAPHORE` 模式减 1) |
| `poll()` | counter > 0 时可读 |
| `eventfd_signal()` | 内核侧原子递增 (IRQ 安全) |

**用途**: KVM guest→host 通知、io_uring 完成通知、epoll 事件循环。

#### Signalfd

**源码**: `fs/signalfd.c`

将信号转换为文件描述符可读事件，可集成到 `epoll`/`poll` 事件循环中。

```c
struct signalfd_ctx {
    sigset_t sigmask;     // 拦截的信号集
};
```

`read()` 返回 `struct signalfd_siginfo` (128 字节)，包含 signo、errno、code、pid、uid 等。

#### Timerfd

**源码**: `fs/timerfd.c`

将定时器过期转换为文件描述符可读事件。

```c
struct timerfd_ctx {
    union { struct hrtimer tmr; struct alarm alarm; } t;
    ktime_t tintv;              // 间隔 (0=一次性)
    wait_queue_head_t wqh;
    u64 ticks;                  // 到期次数
    int clockid;
};
```

支持 `CLOCK_REALTIME`、`CLOCK_MONOTONIC`、`CLOCK_BOOTTIME` 等时钟源。

---

### 4.10 Procfs / Sysfs

#### Procfs (`/proc`)

虚拟文件系统，暴露内核和进程状态：

| 路径 | 内容 |
|------|------|
| `/proc/<pid>/status` | 进程状态 |
| `/proc/<pid>/maps` | 内存映射 |
| `/proc/<pid>/fd/` | 打开的文件描述符 |
| `/proc/meminfo` | 内存信息 |
| `/proc/sys/` | 内核可调参数 |
| `/proc/interrupts` | 中断统计 |

**创建接口**: `proc_create()`, `proc_create_data()`, `seq_file` 接口。

#### Sysfs (`/sys`)

暴露内核设备模型 (`kobject` 层次结构)：

| 路径 | 内容 |
|------|------|
| `/sys/devices/` | 设备拓扑 |
| `/sys/class/` | 设备类 |
| `/sys/bus/` | 总线 |
| `/sys/module/` | 已加载模块 |
| `/sys/kernel/` | 内核子系统 |

**创建接口**: `sysfs_create_file()`, `sysfs_create_group()`, `struct kobj_attribute` 的 `show()`/`store()` 回调。

---

### 4.11 通知链 (Notifier Chains)

**源码**: `kernel/notifier.c`, `include/linux/notifier.h`

内核子系统间的 **发布-订阅** 机制。

#### 数据结构

```c
struct notifier_block {
    notifier_fn_t notifier_call;      // 回调: int (*)(nb, action, data)
    struct notifier_block __rcu *next;
    int priority;                      // 值越大优先级越高
};
```

#### 四种通知链类型

| 类型 | 保护机制 | 回调可睡眠 | 适用场景 |
|------|---------|-----------|---------|
| **Atomic** | RCU (调用) + spinlock (注册) | 否 | 中断/原子上下文 |
| **Blocking** | rwsem | 是 | 进程上下文 |
| **Raw** | 无锁 (调用者负责) | 取决于使用 | 自定义同步 |
| **SRCU** | Sleepable RCU | 是 | 高频调用/低频注册 |

#### API 模式

```c
// 注册
xxx_notifier_chain_register(head, nb);

// 注销
xxx_notifier_chain_unregister(head, nb);

// 触发
xxx_notifier_call_chain(head, val, data);
```

#### 回调返回值

| 返回值 | 含义 |
|--------|------|
| `NOTIFY_DONE` | 不关心此事件 |
| `NOTIFY_OK` | 已处理 |
| `NOTIFY_BAD` | 否决 (veto) |
| `NOTIFY_STOP` | 停止继续通知 |

#### 常见用途

- CPU 热插拔通知
- 内存热插拔通知
- 网络设备事件 (netdev_chain)
- 系统重启/关机通知
- PM 休眠/唤醒通知
- OOM 通知

---

### 4.12 通信机制总结对比

| 机制 | 方向 | 数据模型 | 开销 | 关键用途 |
|------|------|---------|------|---------|
| **信号** | U→U, K→U | 异步通知 + siginfo | 低 | 进程控制、错误通知 |
| **管道/FIFO** | U→U | 字节流 (页环形缓冲) | 低 | Shell 管道、父子进程 |
| **SysV 消息队列** | U→U | 带类型的消息 | 中 | 消息传递 |
| **SysV 共享内存** | U→U | 共享页 (零拷贝) | 极低 | 高吞吐数据共享 |
| **SysV 信号量** | U→U | 计数器数组 | 中 | 进程同步 |
| **POSIX 消息队列** | U→U | 优先级排序消息 | 中 | 实时消息传递 |
| **Unix Socket** | U→U | 流/数据报 + FD 传递 | 低-中 | 通用本地 IPC |
| **Netlink** | U↔K | sk_buff + nlattr | 中 | 网络配置 (iproute2)、udev |
| **Futex** | U→U | 用户态整数 + 内核等待队列 | 极低 (快速路径) | pthread 互斥/条件变量 |
| **Eventfd** | U↔U, K→U | 64 位计数器 | 极低 | KVM、io_uring、事件循环 |
| **Signalfd** | U→U | 信号转 fd 事件 | 低 | epoll 集成信号处理 |
| **Timerfd** | K→U | 定时器转 fd 事件 | 低 | epoll 集成定时器 |
| **Procfs/Sysfs** | U↔K | 文件读写 | 低 | 配置、监控、调优 |
| **通知链** | K→K | 回调链 + 优先级 | 极低 | CPU/内存热插拔、重启 |

---

## 附录: ARM64 硬件同步指令速查

| 指令 | 类别 | 说明 |
|------|------|------|
| `LDXR` / `STXR` | LL/SC | 独占加载/存储 |
| `LDAXR` / `STLXR` | LL/SC + ordering | 带 acquire/release 的独占操作 |
| `LDADD` / `STADD` | LSE | 原子加法 |
| `LDCLR` / `STCLR` | LSE | 原子位清除 |
| `LDSET` / `STSET` | LSE | 原子位设置 |
| `LDEOR` / `STEOR` | LSE | 原子异或 |
| `CAS` / `CASA` / `CASAL` | LSE | 比较并交换 |
| `SWP` / `SWPA` / `SWPAL` | LSE | 原子交换 |
| `LDAR` / `STLR` | Load-Acquire / Store-Release | 单向屏障内存访问 |
| `DMB` | 数据屏障 | `ish`/`ishld`/`ishst`/`sy` 变体 |
| `DSB` | 数据同步屏障 | 比 DMB 更强，等待完成 |
| `ISB` | 指令同步屏障 | 刷新流水线 |
| `WFE` | 事件等待 | 低功耗等待 (用于自旋锁) |
| `SEV` | 发送事件 | 唤醒 WFE (STLR 隐式触发) |
| `WFI` | 中断等待 | 低功耗等待中断 |
| `PRFM` | 预取 | `pstl1strm` 用于原子操作预取 |

---

> **参考源码路径** (基于 linux-6.18.1):
> - 原子操作: `arch/arm64/include/asm/atomic*.h`
> - 内存屏障: `arch/arm64/include/asm/barrier.h`
> - 自旋锁: `kernel/locking/qspinlock.c`, `include/asm-generic/qspinlock.h`
> - 互斥锁: `kernel/locking/mutex.c`, `include/linux/mutex_types.h`
> - 信号量: `kernel/locking/semaphore.c`
> - 读写信号量: `kernel/locking/rwsem.c`
> - 完成量: `kernel/sched/completion.c`
> - RCU: `kernel/rcu/`, `include/linux/rcupdate.h`
> - 软中断: `kernel/softirq.c`, `include/linux/interrupt.h`
> - 工作队列: `kernel/workqueue.c`, `include/linux/workqueue.h`
> - 内核线程: `kernel/kthread.c`, `include/linux/kthread.h`
> - 信号: `kernel/signal.c`
> - 管道: `fs/pipe.c`
> - SysV IPC: `ipc/msg.c`, `ipc/shm.c`, `ipc/sem.c`
> - Netlink: `net/netlink/af_netlink.c`
> - Futex: `kernel/futex/`
> - Eventfd: `fs/eventfd.c`
> - 通知链: `kernel/notifier.c`
