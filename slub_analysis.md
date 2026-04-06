# Linux 6.18.1 SLUB 分配器深度分析

> **平台**：ARM64, QEMU 1GB, 单 NUMA 节点 (`nr_node_ids=1`)
> **内核版本**：linux-6.18.1

---

## 目录

1. [核心数据结构](#1-核心数据结构)
2. [数据结构关系图](#2-数据结构关系图)
3. [SLUB 自举流程 (kmem_cache_init)](#3-slub-自举流程)
4. [calculate_sizes() — 对象布局决策](#4-calculate_sizes--对象布局决策)
5. [Object Layout 详解](#5-object-layout-详解)
6. [SLUB Debug 机制](#6-slub-debug-机制)
7. [kmalloc 缓存体系创建](#7-kmalloc-缓存体系创建)
8. [分配与释放路径](#8-分配与释放路径)
9. [自定义 slab cache（fork.c）](#9-自定义-slab-cache)
10. [关键常量速查表](#10-关键常量速查表)
11. [用户态内存分配全链路](#11-用户态内存分配全链路)
12. [brk / mmap 系统调用](#12-brk--mmap-系统调用)

---

## 1. 核心数据结构

### 1.1 struct kmem_cache（328 字节）

全局工厂描述符，每种对象类型一个实例。

```
偏移    字段                          大小    说明
──────────────────────────────────────────────────────────────
+0      cpu_slab                      8      per-CPU 结构指针
+8      cpu_sheaves                   8      per-CPU sheaf 指针（可选）
+16     flags                         4      SLAB_HWCACHE_ALIGN 等标志
+24     min_partial                   8      node partial 链表最小保留数
+32     size                          4      slot 大小（含 metadata + 对齐）
+36     object_size                   4      用户请求的对象大小
+40     reciprocal_size               8      快速除法（size→index）
+48     offset                        4      freelist ptr 在对象中的偏移
+52     cpu_partial                   4      per-CPU partial 最大对象数
+56     cpu_partial_slabs             4      per-CPU partial 最大 slab 数
+60     sheaf_capacity                4      sheaf 容量
+64     oo                            4      (order << 16) | objects_per_slab
+68     min                           4      fallback oo
+72     allocflags                    4      向 buddy 申请时的 GFP flags
+76     refcount                      4      引用计数
+80     ctor                          8      构造函数指针
+88     inuse                         4      有效数据区大小（含 redzone）
+92     align                         4      对齐要求
+96     red_left_pad                  4      左 redzone 大小
+104    name                          8      "/proc/slabinfo" 显示名
+112    list                          16     slab_caches 全局链表
+128    kobj                          64     sysfs kobject
+192    remote_node_defrag_ratio      4      NUMA 远端碎片整理比率
+200    node[0..15]                   128    每 NUMA 节点的 kmem_cache_node 指针
                                      ───
                               总计   328 字节
```

### 1.2 struct kmem_cache_cpu（48 字节，per-CPU）

每 CPU 的热路径分配器，无锁。

```
偏移    字段          大小    说明
─────────────────────────────────────────
+0      freelist      8      当前 slab 中第一个空闲对象
+8      tid           8      transaction ID（ABA 防护）
+16     slab          8      当前"热" slab 页（frozen=1）
+24     partial       8      per-CPU partial slab 单链表
+32     lock          1      local_trylock
                      ───
               总计    48 字节
```

### 1.3 struct kmem_cache_node（72 字节，per-NUMA-node）

NUMA 节点级共享仓库，有锁保护。

```
偏移    字段            大小    说明
──────────────────────────────────────────────
+0      list_lock       4      自旋锁
+8      nr_partial      8      partial 链表中 slab 数量
+16     partial         16     partial slab 双向链表
+32     nr_slabs        8      总 slab 数（DEBUG）
+40     total_objects   8      总对象数（DEBUG）
+48     full            16     full slab 双向链表（DEBUG，SLAB_STORE_USER）
+64     barn            8      node_barn 指针（可选）
                        ───
                 总计    72 字节
```

---

## 2. 数据结构关系图

```
╔═══════════════════════════════════════════════════════════════════════╗
║                    struct kmem_cache (全局描述符)                      ║
║  cpu_slab ──────────────────────────────────────────────────────┐    ║
║  node[0] ──────────────────────────────────────────────────┐    │    ║
║  flags / size=128 / object_size=72 / offset=32 / oo / ...  │    │    ║
╚════════════════════════════════════════════════════════════╪════╪════╝
                                                             │    │
    ┌────────────────────────────────────────────────────────┘    │
    │  per NUMA node                                              │
    ▼                                                              │
╔══════════════════════════════╗                                   │
║  kmem_cache_node (node 0)   ║                                   │
║  list_lock                  ║                                   │
║  nr_partial = N             ║                                   │
║  partial ──────► slab ◄──► slab ◄──► slab                     │
║  full ─────────► slab (DEBUG)                                  │
╚══════════════════════════════╝                                   │
                                                                    │
    ┌───────────────────────────────────────────────────────────────┘
    │  per CPU（__percpu 变量）
    ▼
╔══════════════════════════════════════════════════════════════════╗
║  kmem_cache_cpu (CPU 0)                                         ║
║  freelist ─────────────────────────────────────────────────┐   ║
║  tid                                                        │   ║
║  slab ───────────────────────────────────────┐              │   ║
║  partial ──► slab_p1 ──► slab_p2 ──► NULL    │              │   ║
╚══════════════════════════════════════════════╪══════════════╪═══╝
                                               │              │
                                               ▼              ▼
                                    ┌─────────────────────────────┐
                                    │  slab 页（frozen=1, 热页）    │
                                    │  slab_cache → kmem_cache     │
                                    │  inuse / objects             │
                                    │  [obj0][obj1][obj2]...[objN] │
                                    │     │    │                   │
                                    │     └────┴── freelist 链 ───┘│
                                    └─────────────────────────────┘
```

**角色分工：**

| 结构 | 角色 | 锁 |
|------|------|----|
| `kmem_cache` | 工厂描述符（配置 + 指向 CPU/Node） | 无（只读居多） |
| `kmem_cache_cpu` | 前台收银台（极快，无锁） | per-CPU local_lock |
| `kmem_cache_node` | 后台仓库（共享，需锁） | spinlock |

---

## 3. SLUB 自举流程

### 3.1 鸡生蛋问题

`kmem_cache_create()` 需要从 `kmem_cache` 这个 cache 分配一个 `struct kmem_cache` 对象，但 `kmem_cache` 本身还没有创建。同理，创建任何 cache 都需要 `kmem_cache_node` 对象来管理 per-node 数据。

### 3.2 三阶段自举

```c
void __init kmem_cache_init(void)
{
    static __initdata struct kmem_cache boot_kmem_cache,
                                        boot_kmem_cache_node;
```

#### 阶段一：在 .init.data 段创建两个静态 cache

```c
    // 1. 先创建 kmem_cache_node 的工厂（产品=72字节的 struct kmem_cache_node）
    //    此时 slab_state=DOWN，early_kmem_cache_node_alloc() 直接向 buddy 要页
    kmem_cache_node = &boot_kmem_cache_node;
    kmem_cache = &boot_kmem_cache;

    create_boot_cache(kmem_cache_node, "kmem_cache_node",
                      sizeof(struct kmem_cache_node),           // 72
                      SLAB_HWCACHE_ALIGN | SLAB_NO_OBJ_EXT, 0, 0);

    slab_state = PARTIAL;  // 现在可以正常分配 kmem_cache_node 了

    // 2. 再创建 kmem_cache 的工厂（产品=208字节的 struct kmem_cache）
    //    此时已能正常分配 per-node 结构
    create_boot_cache(kmem_cache, "kmem_cache",
                      offsetof(struct kmem_cache, node) +
                          nr_node_ids * sizeof(struct kmem_cache_node *),  // 200+1*8=208
                      SLAB_HWCACHE_ALIGN | SLAB_NO_OBJ_EXT, 0, 0);
```

**为什么先创建 kmem_cache_node？** 硬性依赖：第二个 `create_boot_cache` 内部要分配 `struct kmem_cache_node`，必须有能生产它的工厂。

**为什么 size=208 而非 sizeof(struct kmem_cache)=328？** `nr_node_ids=1`（QEMU 单节点），只需 `node[0]` 一项，省掉 `node[1..15]` 的 120 字节，每个对象节省 37%。

#### 阶段二：bootstrap — 从 .init.data 迁移到堆

```c
    kmem_cache = bootstrap(&boot_kmem_cache);       // 先做 kmem_cache
    kmem_cache_node = bootstrap(&boot_kmem_cache_node);  // 再做 kmem_cache_node
```

`bootstrap()` 做了什么：
1. `kmem_cache_zalloc(kmem_cache, ...)` — 从刚建好的 kmem_cache 工厂分配一个堆对象
2. `memcpy(s, static_cache, ...)` — 复制静态 cache 的全部内容到堆
3. 遍历所有 slab 页，修正 `slab->slab_cache` back-pointer 从旧地址指向新堆地址
4. 挂入 `slab_caches` 全局链表

**为什么需要 bootstrap？** `.init.data` 段会被 `free_initmem()` 释放回 buddy，不迁移就变成悬空指针。

**为什么 bootstrap 顺序与 create_boot_cache 相反？** `kmem_cache` 先 bootstrap，确保后续分配的新对象所在 slab 页的 `slab_cache` 指针已经是永久地址（软性保证，减少一个窗口期）。

#### 阶段三：创建 kmalloc 缓存体系

```c
    setup_kmalloc_cache_index_table();  // ARM64 上是 no-op
    create_kmalloc_caches();            // 创建 kmalloc-8 到 kmalloc-8k
    // → slab_state = UP
```

---

## 4. calculate_sizes() — 对象布局决策

这是决定每个 slot 字节布局的核心函数，分 6 个阶段：

### 阶段一：word 对齐

```c
size = ALIGN(size, sizeof(void *));  // ARM64: ALIGN(size, 8)
```

### 阶段二：`__OBJECT_POISON` + 右 redzone（DEBUG）

```c
// 能毒化对象本体的条件：有 POISON 且无 RCU 且无 ctor
if ((flags & SLAB_POISON) && !(flags & SLAB_TYPESAFE_BY_RCU) && !s->ctor)
    s->flags |= __OBJECT_POISON;

// 如果 size == object_size，redzone 没空间，追加 8 字节
if ((flags & SLAB_RED_ZONE) && size == s->object_size)
    size += sizeof(void *);

s->inuse = size;  // ← inuse 在此固定
```

### 阶段三：free pointer 位置决策

```c
if (POISON || RCU(非自定义) || ctor || (REDZONE && 小对象)) {
    // 分支A：free ptr 放在对象外部（不能踩对象内容）
    s->offset = size;
    size += sizeof(void *);
} else if (RCU + use_freeptr_offset) {
    // 分支B：自定义偏移
    s->offset = args->freeptr_offset;
} else {
    // 分支C（普通情况）：放在对象中间
    s->offset = ALIGN_DOWN(s->object_size / 2, sizeof(void *));
}
```

分支C 将 free ptr 放在对象中间的原因：远离头尾，减少小范围 overflow 误踩。

### 阶段四：追加 track 信息（DEBUG）

```c
if (flags & SLAB_STORE_USER) {
    size += 2 * sizeof(struct track);  // +64 字节
    if (flags & SLAB_KMALLOC)
        size += sizeof(unsigned int);  // +4 字节 orig_size
}
```

### 阶段五：追加左 redzone（DEBUG）

```c
if (flags & SLAB_RED_ZONE) {
    size += sizeof(void *);            // +8 字节右补充
    s->red_left_pad = ALIGN(8, s->align);  // ARM64: ALIGN(8,64)=64
    size += s->red_left_pad;           // +64 字节
}
```

### 阶段六：最终对齐 + order 计算

```c
size = ALIGN(size, s->align);
s->size = size;
s->reciprocal_size = reciprocal_value(size);
order = calculate_order(size);
s->oo = oo_make(order, size);    // (order<<16) | objects_per_slab
s->min = oo_make(get_order(size), size);  // fallback（单对象）
```

---

## 5. Object Layout 详解

### 5.1 无 Debug 时（生产内核）

```
┌─────────────────────────────────────┐  ← slot 起始 = object 地址
│  object data (object_size)          │
│  ┌─────────────────────────────┐    │
│  │ free ptr at offset ≈ size/2 │    │  空闲时存下一空闲对象地址
│  └─────────────────────────────┘    │
├─────────────────────────────────────┤
│  alignment padding                  │
└─────────────────────────────────────┘  ← s->size
```

### 5.2 全 Debug 时（RED_ZONE + POISON + STORE_USER）

```
┌──────────────────────────────────────────────────────────┐
│  left redzone (red_left_pad=64B)    填 0xbb/0xcc         │
├──────────────────────────────────────────────────────────┤ ← object 指针
│  object data (object_size)                                │
│  空闲时: 0x6b 填满, 末字节 0xa5                           │
├──────────────────────────────────────────────────────────┤ ← object + object_size
│  right redzone (inuse - object_size) 填 0xbb/0xcc        │
├──────────────────────────────────────────────────────────┤ ← object + inuse
│  free pointer (8B)                                        │
├──────────────────────────────────────────────────────────┤
│  alloc track (32B): addr, handle, cpu, pid, when          │
├──────────────────────────────────────────────────────────┤
│  free track (32B): addr, handle, cpu, pid, when           │
├──────────────────────────────────────────────────────────┤
│  orig_size (4B, 仅 SLAB_KMALLOC)                         │
├──────────────────────────────────────────────────────────┤
│  padding (0x5a) 对齐到 s->size                           │
└──────────────────────────────────────────────────────────┘ ← s->size
```

### 5.3 魔法字节表

| 字节值 | 常量名 | 含义 |
|--------|--------|------|
| `0xbb` | `SLUB_RED_INACTIVE` | 空闲对象的 redzone |
| `0xcc` | `SLUB_RED_ACTIVE` | 已分配对象的 redzone |
| `0x6b` | `POISON_FREE` | 空闲对象 body（检测 UAF） |
| `0xa5` | `POISON_END` | 空闲对象 body 末字节（边界标记） |
| `0x5a` | `POISON_INUSE` | padding 区域（检测未初始化读） |

---

## 6. SLUB Debug 机制

### 6.1 Debug 标志

| 标志 | 命令行 | 功能 |
|------|--------|------|
| `SLAB_CONSISTENCY_CHECKS` | `f` | 一致性检查（slab元数据 + freelist完整性） |
| `SLAB_RED_ZONE` | `z` | 红区检测越界写 |
| `SLAB_POISON` | `p` | 投毒检测 UAF |
| `SLAB_STORE_USER` | `u` | 记录 alloc/free 调用栈 |
| `SLAB_TRACE` | `t` | 每次操作打印 trace |
| `SLAB_FAILSLAB` | `a` | 随机故障注入 |

### 6.2 初始化

```
bootloader cmdline: "slab_debug=..." / "slub_debug=..."
        │
        ▼ __setup 解析
setup_slub_debug()                     // mm/slub.c:1866
    ├── parse_slub_debug_flags()       // 解析 f/z/p/u/t/a 字母
    ├── slub_debug = 全局 flags
    ├── slub_debug_string = "cache1,cache2..."  // 仅对指定 cache
    └── static_branch_enable(&slub_debug_enabled)
        │
        ▼ 每个 cache 创建时
do_kmem_cache_create()
    └── s->flags = kmem_cache_flags(flags, name)  // 合并全局 debug flags
```

### 6.3 检测能力

| Bug 类型 | 检测机制 | 触发时机 |
|----------|---------|---------|
| use-after-free | body 不再是 0x6b | 下次分配 |
| 堆溢出 | redzone 不是 0xbb/0xcc | 下次分配/释放 |
| double-free | `on_freelist()` 发现已在链表 | 立即 |
| 错误 cache 释放 | `s != slab->slab_cache` | 立即 |
| freelist 损坏 | `check_valid_pointer()` | alloc/free 时 |
| kmalloc 越界 | orig_size + 右 redzone | 下次操作 |

### 6.4 对象生命周期字节变化

```
新建 slab 页   → 整页填 0x5a (POISON_INUSE)
    │
每个对象初始化  → left/right redzone = 0xbb
                  body = 0x6b...0xa5
    │
分配             → 检查 body=0x6b? redzone=0xbb? → 改 redzone 为 0xcc
    │
使用中           → 用户自由读写 object_size 范围
    │
释放             → 检查 redzone=0xcc?(越界?) → 检查 double-free?
                  → 重填 body=0x6b, redzone=0xbb
```

### 6.5 使用示例

```bash
slub_debug              # 全 debug: f+z+p+u
slub_debug=p            # 仅 poison（检测 UAF）
slub_debug=zpu,kmalloc-64          # 仅 kmalloc-64 开 redzone+poison+user
slub_debug=-            # 关闭所有（覆盖 CONFIG_SLUB_DEBUG_ON 默认值）
```

---

## 7. kmalloc 缓存体系创建

### 7.1 setup_kmalloc_cache_index_table()

ARM64 上 `KMALLOC_MIN_SIZE=8`，三段逻辑全部跳过（for 循环 `8<8` 不执行，两个 if `8>=64` 和 `8>=128` 均为 false）。此函数是给 MIPS 等大对齐架构使用的，ARM64 上是 no-op。

### 7.2 kmalloc_size_index[] 查表

```c
u8 kmalloc_size_index[24] = {
    3,  // 8B   → kmalloc-8 (index 3)
    4,  // 16B  → kmalloc-16
    5,  // 24B  → kmalloc-32
    5,  // 32B  → kmalloc-32
    6,  // 40B  → kmalloc-64
    ...
    1,  // 72-96B   → kmalloc-96 (index 1, 非 2 次幂)
    ...
    2,  // 136-192B → kmalloc-192 (index 2, 非 2 次幂)
};
```

### 7.3 create_kmalloc_caches()

ARM64 上 `KMALLOC_MIN_SIZE=8 ≤ 32` 且 `≤ 64`，因此 kmalloc-96 和 kmalloc-192 都会创建。

```c
for (type = KMALLOC_NORMAL; type < NR_KMALLOC_TYPES; type++) {
    new_kmalloc_cache(1, type);   // kmalloc-96
    new_kmalloc_cache(2, type);   // kmalloc-192
    for (i = 3; i <= KMALLOC_SHIFT_HIGH; i++)
        new_kmalloc_cache(i, type);  // kmalloc-8,16,...,8192
}
slab_state = UP;
```

### 7.4 new_kmalloc_cache() 在 ARM64 上的简化行为

`__kmalloc_minalign()` 返回 8 = `ARCH_KMALLOC_MINALIGN`，所以：
- `minalign > ARCH_KMALLOC_MINALIGN` 为 false
- 不做任何 size/idx 对齐调整
- 直接调用 `create_kmalloc_cache(kmalloc_info[idx].name[type], size, flags)`

### 7.5 最终创建的 kmalloc cache（NORMAL 系列）

| index | name | object_size | size（对齐后） | 每 slab 对象数 |
|-------|------|-------------|----------------|--------------|
| 1 | kmalloc-96 | 96 | 96 | 42 |
| 2 | kmalloc-192 | 192 | 192 | 21 |
| 3 | kmalloc-8 | 8 | 8 | 512 |
| 4 | kmalloc-16 | 16 | 16 | 256 |
| 5 | kmalloc-32 | 32 | 32 | 128 |
| 6 | kmalloc-64 | 64 | 64 | 64 |
| 7 | kmalloc-128 | 128 | 128 | 32 |
| 8 | kmalloc-256 | 256 | 256 | 16 |
| 9 | kmalloc-512 | 512 | 512 | 8 |
| 10 | kmalloc-1k | 1024 | 1024 | 4 |
| 11 | kmalloc-2k | 2048 | 2048 | 2 |
| 12 | kmalloc-4k | 4096 | 4096 | 1 |
| 13 | kmalloc-8k | 8192 | 8192 | 1 (order=1) |

同样创建 `KMALLOC_RECLAIM` 系列（带 `SLAB_RECLAIM_ACCOUNT`）。

---

## 8. 分配与释放路径

### 8.1 分配路径

```
kmalloc(N)
    │
    ├── 快路径: cpu_slab->freelist != NULL
    │     └─ cmpxchg 弹出 freelist[0]，纯 per-CPU，无锁
    │
    ├── 中路径: cpu_slab->partial != NULL
    │     └─ 从 cpu partial 取一个 slab 补充 freelist，仍 per-CPU
    │
    ├── 慢路径: kmem_cache_node->partial 非空
    │     └─ 需要 node->list_lock，从 node partial 取 slab
    │
    └── 最慢: new_slab() → 向 buddy allocator 申请新页
```

### 8.2 释放路径

```
kfree(ptr)
    │
    ├── 属于当前 cpu slab → 直接还入 freelist（无锁）
    │
    └── 不属于当前 cpu slab
          ├── 原 slab 是 full → 变成 partial，挂入 cpu partial
          │     └── cpu partial 超限? → 批量转移到 node partial
          │
          └── 原 slab 不是 full
                ├── 释放后变 empty → nr_partial > min_partial? → 释放页回 buddy
                │                   └── 否则留在 node partial
                └── 释放后仍 partial → 原地修改 freelist
```

---

## 9. 自定义 slab cache

### 为什么不直接用 kmalloc？

`kernel/fork.c` 中的 `mm_cache_init()` 和 `proc_caches_init()` 在 SLUB 就绪后创建专用 cache：

```c
mm_cachep = kmem_cache_create_usercopy("mm_struct", mm_size, ...);
sighand_cachep = kmem_cache_create("sighand_cache", ..., SLAB_TYPESAFE_BY_RCU, sighand_ctor);
signal_cachep = kmem_cache_create("signal_cache", ...);
files_cachep  = kmem_cache_create("files_cache", ...);
fs_cachep     = kmem_cache_create("fs_cache", ...);
```

| 原因 | 说明 |
|------|------|
| 精确 size | `mm_struct` 大小含 `cpumask_size()` 运行时决定，kmalloc bucket 会浪费空间 |
| `SLAB_ACCOUNT` | 按 memory cgroup 计费，kmalloc 做不到细粒度 |
| ctor | `sighand_ctor` 自动初始化 spinlock/waitqueue |
| `SLAB_TYPESAFE_BY_RCU` | slab 页在 RCU grace period 前不释放 |
| 可观测性 | `/proc/slabinfo` 独立显示，方便监控 |

### 调用时序

```
start_kernel()
    ├── kmem_cache_init()          ← SLUB 自举，slab_state = UP
    ├── mm_init()
    │     └── mm_cache_init()      ← 创建 mm_struct 专属 cache
    └── rest_init() → kernel_init()
          └── proc_caches_init()   ← 创建 sighand/signal/files/fs cache
```

---

## 10. 关键常量速查表

| 常量 | 值（ARM64） | 来源 |
|------|------------|------|
| `ARCH_KMALLOC_MINALIGN` | 8 | `arch/arm64/include/asm/cache.h` |
| `KMALLOC_MIN_SIZE` | 8 | `= ARCH_KMALLOC_MINALIGN` |
| `KMALLOC_SHIFT_LOW` | 3 | `= ilog2(KMALLOC_MIN_SIZE)` |
| `KMALLOC_SHIFT_HIGH` | 13 | 8KB |
| `nr_node_ids` | 1 | QEMU 单节点 |
| `MAX_NUMNODES` | 16 | `node[]` 数组大小 |
| `cache_line_size()` | 64 | |
| `PAGE_SIZE` | 4096 | |
| `slub_max_order` | 3 | `PAGE_ALLOC_COSTLY_ORDER` |
| `MIN_PARTIAL` | 5 | node partial 最小保留 |
| `MAX_PARTIAL` | 10 | node partial 最大 |
| `sizeof(struct kmem_cache)` | 328 | |
| `sizeof(struct kmem_cache_node)` | 72 | |
| `sizeof(struct kmem_cache_cpu)` | 48 | |
| `sizeof(struct track)` | 32 | debug 用 |
| `offsetof(kmem_cache, node)` | 200 (0xC8) | |

### kmem_cache_init() 后两个核心 cache 的具体值

**kmem_cache_node（生产 72 字节的 struct kmem_cache_node）：**

| 字段 | 值 |
|------|----|
| object_size | 72 |
| size | 128（ALIGN(72, 64)） |
| inuse | 72 |
| offset | 32（对象中间） |
| align | 64 |
| oo | (0<<16)\|32 = 32（order=0, 32对象/页） |
| min_partial | 5 |
| cpu_partial | 120 |

**kmem_cache（生产 208 字节的 struct kmem_cache）：**

| 字段 | 值 |
|------|----|
| object_size | 208 |
| size | 256（ALIGN(208, 64)） |
| inuse | 208 |
| offset | 104（对象中间） |
| align | 64 |
| oo | (0<<16)\|16 = 16（order=0, 16对象/页） |
| min_partial | 5 |
| cpu_partial | 52 |

---

### slab_state 状态机

```
DOWN ─────► PARTIAL ─────► UP ─────► FULL
  │              │            │          │
  │  kmem_cache_ │  create_   │  sysfs   │
  │  node 可用   │  kmalloc_  │  完成    │
  │              │  caches()  │          │
```

| 状态 | 含义 |
|------|------|
| `DOWN` | 无 slab 功能 |
| `PARTIAL` | `kmem_cache_node` 可用，能分配 per-node 结构 |
| `UP` | kmalloc 缓存就绪，`slab_is_available()` 返回 true |
| `FULL` | sysfs 等全部就绪 |

---

## 11. 用户态内存分配全链路

### 11.1 malloc → 物理内存 6 层路径

```
malloc(size)                              [glibc / ptmalloc2 用户空间]
    │
    ├─ size < 128KB → 从 brk heap pool 分配
    │       └─ pool 不足 → brk() 系统调用扩展堆
    │
    └─ size ≥ 128KB → mmap(MAP_ANONYMOUS) 系统调用
          │
          ▼
     内核：分配虚拟地址区间（创建 VMA）                [mm/mmap.c]
          │        ◄── 此时无物理页！
          ▼
     首次访问 → 缺页异常（Page Fault）               [mm/fault.c]
          │
          ▼
     handle_mm_fault()                              [mm/memory.c]
          │
          ▼
     do_anonymous_page()                            [mm/memory.c]
          │
          ├─ alloc_page()                           申请 4KB 物理页
          │       └─ buddy allocator               [mm/page_alloc.c]
          │
          └─ 填写页表 PTE，建立虚拟→物理映射
```

### 11.2 关键原则

| 原则 | 说明 |
|------|------|
| **Demand Paging** | VMA 创建时无物理页，访问时才分配 |
| **buddy 最小单位** | 永远是 4KB 页，从不分配更小粒度 |
| **glibc 批量申请** | `brk()` 每次约扩展 132KB，减少 syscall 次数 |
| **大对象直接 mmap** | ≥ 128KB 用 `mmap`，`free()` 时 `munmap()` 立即归还物理页 |

### 11.3 glibc ptmalloc2 内存池机制

```
主线程 heap（brk 管理）：
  ┌─────────────────────────┐
  │  top chunk              │  ← 可通过 malloc_trim() 收缩
  │  large bins             │
  │  small bins             │
  │  unsorted bin           │
  │  fastbins (≤176B)       │  ← 不立即合并，提高分配速度
  └─────────────────────────┘

线程 arena（每线程独立，mmap 64MB）：
  ┌─────────────────────────┐
  │  HEAP_MAX_SIZE = 64MB   │  ← MAP_NORESERVE，实际按需使用
  │  独立 bin 体系          │
  └─────────────────────────┘
```

### 11.4 glibc 碎片化处理策略

| 策略 | 触发时机 | 效果 |
|------|----------|------|
| **多级 bin 精确匹配** | 每次 free() | fastbin/small/large bin 分级复用，减少碎片 |
| **相邻 chunk 合并** | free() 时检查 prev/next | 消除堆内部小碎片 |
| **malloc_consolidate()** | fastbin 满 / malloc 大块失败 | 批量合并 fastbin，释放潜在大块 |
| **malloc_trim()** | 堆顶大片空闲时 | 调用 `brk()` 缩小堆，归还虚拟地址 |
| **madvise(MADV_DONTNEED)** | 线程 arena 空闲时 | 归还物理页但保留虚拟地址（解决 brk 被 pin 住问题）|
| **per-thread arena** | 多线程竞争时 | 线程 arena 整体释放时完整 munmap，彻底消除碎片 |

### 11.5 ARM64 虚拟地址空间布局

```
0xFFFF_FFFF_FFFF_FFFF ─── 内核空间
        ...
0x0000_FFFF_FFFF_FFFF ─── 用户空间上限（ARM64 48位 VA）
                           栈（向下增长）↓
                           ...
                           mmap 区域（do_mmap 在此分配，向下增长）↑
                           ...
                           brk heap（向上增长）↓
               mm->brk ── heap 当前末尾
          mm->start_brk ── heap 起始
                           .bss / .data / .text
0x0000_0000_0000_0000 ─── NULL
```

**64 位下 brk 增长到接近 mmap 区域**：寻址空间 256TB，实践中几乎不会碰到上限，主要受 `RLIMIT_DATA`（`ulimit -d`）限制，这是 32 位时代的遗留问题。

---

## 12. brk / mmap 系统调用

### 12.1 brk 系统调用（`mm/mmap.c` line 115）

```c
SYSCALL_DEFINE1(brk, unsigned long, brk)
```

**功能**：移动进程数据段末尾指针，扩大或缩小堆。

```
brk(addr)
   │
   ├─ mmap_write_lock(mm)
   ├─ 检查 min_brk（start_brk 或 end_data）
   ├─ 检查 RLIMIT_DATA
   │
   ├─【缩小 brk】brk <= mm->brk
   │    └─ do_vmi_align_munmap()  → 解除 VMA 映射
   │
   └─【扩大 brk】brk > mm->brk
        ├─ check_brk_limits()     → 地址空间检查
        ├─ 检查 next VMA（stack_guard_gap）
        └─ do_brk_flags()         → 创建/扩展匿名 VMA（无物理页）
```

### 12.2 mmap 系统调用（`mm/mmap.c` line 611）

```c
SYSCALL_DEFINE6(mmap_pgoff, addr, len, prot, flags, fd, pgoff)
  └─ ksys_mmap_pgoff()
       └─ vm_mmap_pgoff()
            └─ do_mmap()
```

**两条路径**：

| flags | 路径 | 典型用途 |
|-------|------|----------|
| `MAP_ANONYMOUS` | 匿名映射 | glibc 大块 malloc、栈扩展 |
| 有 fd（无 `MAP_ANONYMOUS`）| 文件映射 → `file->f_op->mmap()` | 加载 .so、文件 IO |

**`do_mmap()` 核心步骤**：
1. `get_unmapped_area()` — 在 mmap 区域找空闲虚拟地址（从高地址向下）
2. `mmap_region()` — 创建 `struct vm_area_struct (VMA)`
3. 匿名映射：`vm_ops = NULL`，等缺页异常处理；文件映射：调用 `file->f_op->mmap()` 设置 `vm_ops`

### 12.3 应用场景对比

```
malloc(size)
    │
    ├─ size < MMAP_THRESHOLD (128KB)
    │      └─ brk heap pool 分配
    │           └─ 不足 → brk() 扩展（约 132KB/次）
    │
    └─ size ≥ MMAP_THRESHOLD (128KB)
           └─ mmap(MAP_ANONYMOUS)
                └─ free() → munmap() 立即归还
```

| 维度 | `brk` | `mmap` |
|------|-------|--------|
| 适用大小 | 小对象 pool | 大块 / 文件 / 共享内存 |
| 释放灵活性 | 只能收缩末尾 | 任意区间 `munmap()` |
| 物理页归还 | 需 `malloc_trim()` + `madvise` | `munmap()` 直接归还 |
| 地址连续性 | 连续线性增长 | 分散在 mmap 区域 |
| 多线程 | 主线程专用 | 多线程 arena 各自独立 |

### 12.4 mmap 典型使用场景

```c
// 1. glibc 大块 malloc
malloc(200*1024) → mmap(MAP_ANONYMOUS|MAP_PRIVATE)
free()           → munmap()  // 立即归还，无碎片

// 2. 加载动态库
mmap(fd, PROT_READ|PROT_EXEC)  // .text 段
mmap(fd, PROT_READ|PROT_WRITE) // .data 段

// 3. 文件映射 I/O（page cache 直接映射到用户空间）
void *p = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);

// 4. 进程间共享内存
mmap(MAP_SHARED | MAP_ANONYMOUS)   // 父子进程
shm_open() + mmap()                // 无关进程

// 5. 多线程 arena（glibc 内部）
mmap(HEAP_MAX_SIZE, MAP_NORESERVE|MAP_ANONYMOUS|MAP_PRIVATE)
```
