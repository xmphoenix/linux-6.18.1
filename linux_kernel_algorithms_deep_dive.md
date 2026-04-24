# Linux 6.18.1 内核核心算法深度解析

> Based on source code analysis of `/repo/ybzhang/kernel/linux-6.18.1`

---

## 1. CFS 完全公平调度器 (Completely Fair Scheduler)

**Source**: `kernel/sched/fair.c`, `kernel/sched/core.c`

### 1.1 核心思想

CFS 的目标是给每个任务分配"理想的"CPU 时间份额。它通过 **虚拟运行时间 (vruntime)** 来追踪每个任务"欠"了多少CPU时间。权重越高（nice值越低）的任务，vruntime 增长越慢，从而获得更多实际CPU时间。

### 1.2 Virtual Runtime (vruntime) 计算

**关键函数**: `update_curr()` ([fair.c](kernel/sched/fair.c#L1207))

```
update_curr(cfs_rq):
  1. curr = cfs_rq->curr                    // 当前运行的调度实体
  2. delta_exec = update_se(rq, curr)        // 计算自上次更新以来的实际运行时间 (ns)
  3. curr->vruntime += calc_delta_fair(delta_exec, curr)  // 加权后的虚拟时间
  4. update_deadline(cfs_rq, curr)           // 更新deadline（EEVDF）
  5. update_min_vruntime(cfs_rq)             // 更新cfs_rq的最小vruntime
```

**`calc_delta_fair()`** ([fair.c](kernel/sched/fair.c#L290)):
```c
// delta_vruntime = delta_exec * NICE_0_LOAD / weight
// 如果 weight == NICE_0_LOAD (nice=0), 则 delta_vruntime == delta_exec
static inline u64 calc_delta_fair(u64 delta, struct sched_entity *se)
{
    if (unlikely(se->load.weight != NICE_0_LOAD))
        delta = __calc_delta(delta, NICE_0_LOAD, &se->load);
    return delta;
}
```

**`__calc_delta()`** ([fair.c](kernel/sched/fair.c#L260)):
```c
// 计算 delta_exec * weight / lw->weight
// 使用预计算的逆值 (inv_weight = 2^32 / weight) 来避免除法
// result = delta_exec * (NICE_0_LOAD * inv_weight) >> 32
static u64 __calc_delta(u64 delta_exec, unsigned long weight, struct load_weight *lw)
{
    u64 fact = scale_load_down(weight);
    __update_inv_weight(lw);          // 计算 lw->inv_weight = 2^32 / lw->weight
    fact = mul_u32_u32(fact, lw->inv_weight);
    return mul_u64_u32_shr(delta_exec, fact, shift);
}
```

**数学公式**:
$$vruntime_{new} = vruntime_{old} + \Delta_{exec} \times \frac{weight_{nice_0}}{weight_{task}}$$

### 1.3 Nice-to-Weight 映射

**`sched_prio_to_weight[]`** ([core.c](kernel/sched/core.c#L10342)):
```c
const int sched_prio_to_weight[40] = {
 /* -20 */     88761,     71755,     56483,     46273,     36291,
 /* -15 */     29154,     23254,     18705,     14949,     11916,
 /* -10 */      9548,      7620,      6100,      4904,      3906,
 /*  -5 */      3121,      2501,      1991,      1586,      1277,
 /*   0 */      1024,       820,       655,       526,       423,
 /*   5 */       335,       272,       215,       172,       137,
 /*  10 */       110,        87,        70,        56,        45,
 /*  15 */        36,        29,        23,        18,        15,
};
```

**设计原则**: 相邻 nice 值之间的权重比大约为 **~1.25** (25%)，保证每降低1个nice值，获得约10%更多的CPU时间。`NICE_0_LOAD = 1024`。

预计算的逆值 `sched_prio_to_wmult[]` 用于将除法转换为乘法+右移，避免运行时除法的开销。

### 1.4 EEVDF: Earliest Eligible Virtual Deadline First

Linux 6.6+ 将 CFS 从纯 vruntime 选择升级为 **EEVDF** 算法。EEVDF 增加了两个关键概念：**eligibility** (资格) 和 **deadline** (虚拟截止时间)。

#### 1.4.1 Lag (滞后量) 和 Eligibility (资格)

**`entity_eligible()`** ([fair.c](kernel/sched/fair.c#L738)):

```c
/*
 * lag_i = S - s_i = w_i * (V - v_i)
 *
 * lag >= 0 意味着实体欠服务（应该运行），具有资格
 * lag < 0 意味着实体已获得超额服务，暂时没资格
 *
 * 直觉: V 是加权平均 vruntime，如果 v_i < V 说明实体运行时间
 * 少于公平份额，应该被优先调度
 */
int entity_eligible(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    return vruntime_eligible(cfs_rq, se->vruntime);
}

static int vruntime_eligible(struct cfs_rq *cfs_rq, u64 vruntime)
{
    struct sched_entity *curr = cfs_rq->curr;
    s64 avg = cfs_rq->avg_vruntime;    // Σ(v_i - min_vruntime) * w_i
    long load = cfs_rq->avg_load;       // Σw_i

    if (curr && curr->on_rq) {
        unsigned long weight = scale_load_down(curr->load.weight);
        avg += entity_key(cfs_rq, curr) * weight;
        load += weight;
    }

    // 不做除法! 而是交叉相乘比较:
    // avg/load >= vruntime - min_vruntime
    // 等价于: avg >= (vruntime - min_vruntime) * load
    return avg >= (s64)(vruntime - cfs_rq->min_vruntime) * load;
}
```

**为什么不直接用 `avg_vruntime() >= se->vruntime`？** 因为 `avg_vruntime()` 需要做一次 64 位除法（`div_s64`），而 `vruntime_eligible()` 通过交叉相乘避免了除法，在热路径上更快。

#### 1.4.2 `avg_vruntime` — 增量维护加权平均

```c
// 加权平均的增量维护:
// avg_vruntime = Σ (v_i - min_vruntime) * w_i
// avg_load = Σ w_i
// V = avg_vruntime / avg_load + min_vruntime

static void avg_vruntime_add(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    unsigned long weight = scale_load_down(se->load.weight);
    s64 key = entity_key(cfs_rq, se);  // = se->vruntime - cfs_rq->min_vruntime
    cfs_rq->avg_vruntime += key * weight;
    cfs_rq->avg_load += weight;
}

// 当 min_vruntime 前进 delta 时，需要调整:
// avg_vruntime' = Σ (v_i - (min_vruntime + delta)) * w_i
//              = avg_vruntime - delta * avg_load
static inline void avg_vruntime_update(struct cfs_rq *cfs_rq, s64 delta)
{
    cfs_rq->avg_vruntime -= cfs_rq->avg_load * delta;
}
```

#### 1.4.3 `__pick_eevdf()` — EEVDF 核心选择算法 ([fair.c](kernel/sched/fair.c#L944))

```c
static struct sched_entity *__pick_eevdf(struct cfs_rq *cfs_rq, bool protect)
{
    struct rb_node *node = cfs_rq->tasks_timeline.rb_root.rb_node;
    struct sched_entity *se = __pick_first_entity(cfs_rq);  // leftmost = 最早deadline
    struct sched_entity *curr = cfs_rq->curr;
    struct sched_entity *best = NULL;

    // 优化: 只有一个实体时直接返回
    if (cfs_rq->nr_queued == 1)
        return curr && curr->on_rq ? curr : se;

    // 当前运行的实体如果不 eligible，就不考虑
    if (curr && (!curr->on_rq || !entity_eligible(cfs_rq, curr)))
        curr = NULL;

    // 如果当前实体还在保护期(slice未用完)，继续运行
    if (curr && protect && protect_slice(curr))
        return curr;

    // 快速路径: 最左节点(最早deadline)如果 eligible，就是最优选择
    if (se && entity_eligible(cfs_rq, se)) {
        best = se;
        goto found;
    }

    /*
     * 堆搜索: 利用 augmented 数据 min_vruntime 剪枝
     * 树按 deadline 排序，但每个节点维护子树中的 min_vruntime
     *
     * 算法: 找 eligible 且 deadline 最早的实体
     */
    while (node) {
        struct rb_node *left = node->rb_left;

        // 如果左子树中有 eligible 实体(min_vruntime够小)
        // 左子树的 deadline 更早，所以优先搜索左边
        if (left && vruntime_eligible(cfs_rq,
                    __node_2_se(left)->min_vruntime)) {
            node = left;
            continue;
        }

        se = __node_2_se(node);

        // 左子树没有 eligible 的，检查当前节点
        if (entity_eligible(cfs_rq, se)) {
            best = se;
            break;
        }

        // 当前不 eligible，试右子树
        node = node->rb_right;
    }
found:
    // 如果 curr 比 best 的 deadline 更早，选 curr
    if (!best || (curr && entity_before(curr, best)))
        best = curr;

    return best;
}
```

**算法复杂度分析**: 虽然是树搜索，但由于 augmented `min_vruntime` 的剪枝，通常只需访问 O(log n) 个节点。最坏情况下遍历整棵树，但这在实际工作负载中极为罕见。

**EEVDF vs 纯 CFS 的区别**:
- 纯 CFS: 始终选 vruntime 最小的实体（`rb_first_cached`，O(1)）
- EEVDF: 选 **eligible 且 deadline 最早** 的实体，提供了更好的延迟保证
- `deadline = vruntime + calc_delta_fair(slice, se)` — slice 越小的实体 deadline 越紧迫

### 1.5 Red-Black Tree 调度

**关键数据结构**: `cfs_rq->tasks_timeline` (augmented rb-tree, cached)

**`__enqueue_entity()`** ([fair.c](kernel/sched/fair.c#L848)):
```c
static void __enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    avg_vruntime_add(cfs_rq, se);
    se->min_vruntime = se->vruntime;
    se->min_slice = se->slice;
    rb_add_augmented_cached(&se->run_node, &cfs_rq->tasks_timeline,
                            __entity_less, &min_vruntime_cb);
}
```

- 使用 **augmented RB-tree** — 每个节点维护子树中的 `min_vruntime` 和 `min_slice`
- `rb_leftmost` 缓存指向 vruntime 最小的实体，实现 O(1) 的 `pick_next` 访问
- 入队/出队操作为 **O(log n)**

### 1.5 `enqueue_entity()` — 完整入队流程

([fair.c](kernel/sched/fair.c#L5239)):
```
enqueue_entity(cfs_rq, se, flags):
  1. place_entity(cfs_rq, se, flags)   // 设置合理的 vruntime（防止睡眠任务饥饿/抢占）
  2. update_curr(cfs_rq)               // 更新当前运行实体的 vruntime
  3. update_load_avg(cfs_rq, se, ...)  // 更新 PELT 负载
  4. se_update_runnable(se)            // 更新可运行统计
  5. update_cfs_group(se)              // 更新组调度权重
  6. place_entity(cfs_rq, se, flags)   // 非当前实体放置
  7. account_entity_enqueue(cfs_rq, se)// 更新 cfs_rq 的负载/计数
  8. __enqueue_entity(cfs_rq, se)      // 插入 rb-tree
  9. se->on_rq = 1
```

### 1.6 `pick_next_task_fair()` — 选择下一个任务

([fair.c](kernel/sched/fair.c#L8869)):
```
pick_next_task_fair(rq, prev, rf):
  1. p = pick_task_fair(rq)          // 从rb-tree选出最合适的任务（EEVDF: eligible + earliest deadline）
  2. if (!p) goto idle               // 无任务，尝试 newidle balance
  3. 优化路径: 如果 prev 和 p 在同一个 cgroup 层级，
     只切换差异部分（减少 put_prev/set_next 的层次遍历）
  4. idle: sched_balance_newidle()   // 从其他CPU偷任务
```

### 1.7 Load Balancing

- **域层次**: `sched_domain` 层次结构（SMT → MC → NUMA）
- `sched_balance_newidle()`: 当CPU即将空闲时从繁忙CPU迁移任务
- `migration_cost = 500000 ns (0.5ms)`: 防止过于频繁的迁移
- PELT (Per-Entity Load Tracking): 每个实体的负载使用几何衰减序列追踪

### 1.8 复杂度

| 操作 | 时间复杂度 |
|------|-----------|
| pick_next_task | O(log n) (EEVDF) / O(1) via leftmost cache |
| enqueue/dequeue | O(log n) |
| update_curr | O(1) |
| load balance | O(n) per domain |

**空间**: O(n) 用于rb-tree节点，每个调度实体 `struct sched_entity` 约 200+ bytes。

---

## 2. SLUB Allocator (小对象分配器)

**Source**: `mm/slub.c`

### 2.1 核心思想

SLUB 是内核的 slab 分配器，专门高效分配固定大小的小对象（如 `task_struct`, `inode`, `dentry` 等）。它比传统 SLAB 更简单，减少了缓存行的使用和元数据开销。

### 2.2 关键数据结构

```
struct kmem_cache {
    struct kmem_cache_cpu *cpu_slab;  // Per-CPU slab（快速路径）
    unsigned int size;                // 对象大小（含对齐）
    unsigned int object_size;         // 对象实际大小
    struct kmem_cache_node *node[];   // Per-node partial lists
}

struct kmem_cache_cpu {
    void **freelist;    // 指向下一个空闲对象的指针
    struct slab *slab;  // 当前 CPU 使用的 slab page
    // ...per-cpu partial list
}

struct slab (嵌入 struct page) {
    void *freelist;     // 第一个空闲对象
    unsigned inuse;     // 正在使用的对象数
    unsigned objects;   // slab 中总对象数
    unsigned frozen;    // 是否被 CPU "冻结"
}
```

### 2.3 Slab 状态分类

来自源码注释 ([slub.c](mm/slub.c#L100)):
| 状态 | SL_partial | frozen | 说明 |
|------|-----------|--------|------|
| **CPU slab** | 0 | 1 | 正在被CPU使用，从中分配对象 |
| **CPU partial** | 0 | 0 | 缓存在CPU partial list上，加速分配 |
| **Node partial** | 1 | 0 | 在节点的partial链表上 |
| **Full slab** | 0 | 0 | 所有对象都被分配，不在任何链表上 |

### 2.4 分配快速路径 (Fast Path)

**`slab_alloc_node()`** ([slub.c](mm/slub.c#L5258)):
```
slab_alloc_node(s, gfpflags, node):
  1. s = slab_pre_alloc_hook(s, gfpflags)  // memcg 记账等
  2. 尝试 kfence 分配（调试用）
  3. 如果 s->cpu_sheaves 存在，尝试 alloc_from_pcs() // per-cpu sheaves
  4. 否则: __slab_alloc_node(s, ...)
     → 快速路径:
        c = this_cpu_ptr(s->cpu_slab)
        object = c->freelist              // 从per-CPU freelist取
        c->freelist = get_freepointer(s, object)  // 前进到下一个空闲对象
        // 使用 cmpxchg 或 local lock 保证原子性
  5. slab_post_alloc_hook()               // 初始化、memcg等
```

**实际的快速路径是无锁的**：使用 `this_cpu_cmpxchg_double()` 或 `local_lock` 来原子地取出 freelist 头部。

### 2.5 分配慢速路径 (Slow Path)

**`___slab_alloc()`** ([slub.c](mm/slub.c#L4460)):

```c
static void *___slab_alloc(struct kmem_cache *s, gfp_t gfpflags, int node,
                           unsigned long addr, struct kmem_cache_cpu *c,
                           unsigned int orig_size)
{
    void *freelist;
    struct slab *slab;

    stat(s, ALLOC_SLOWPATH);  // 统计: 慢路径次数+1

reread_slab:
    slab = READ_ONCE(c->slab);        // 读取当前CPU的slab
    if (!slab)
        goto new_slab;                 // 没有slab → 需要获取新的

    /* 检查 NUMA 节点是否匹配 */
    if (unlikely(!node_match(slab, node))) {
        if (!allow_spin) {
            node = NUMA_NO_NODE;       // 不能自旋锁 → 放宽节点要求
        } else {
            goto deactivate_slab;      // 停用当前slab，获取正确节点的
        }
    }

    /* 加 per-cpu local_lock，重新检查（防止抢占竞争） */
    local_lock_cpu_slab(s, flags);
    if (unlikely(slab != c->slab)) {
        local_unlock_cpu_slab(s, flags);
        goto reread_slab;              // 被抢占了，slab已变，重试
    }

    freelist = c->freelist;
    if (freelist)
        goto load_freelist;            // 快速路径重试: 有人归还了对象

    /* 尝试从 slab 本身的 freelist 获取
     * (其他CPU释放的对象会放到slab->freelist) */
    freelist = get_freelist(s, slab);
    if (!freelist) {
        c->slab = NULL;               // slab 已满，断开关联
        goto new_slab;
    }

load_freelist:
    /* 从 freelist 取出第一个对象 */
    c->freelist = get_freepointer(s, freelist);  // freelist = freelist->next
    c->tid = next_tid(c->tid);
    local_unlock_cpu_slab(s, flags);
    return freelist;                   // 返回分配的对象

new_slab:
    /* 层次化搜索: CPU partial → Node partial → Buddy allocator */

    // 1. 尝试 CPU partial list
    while (slub_percpu_partial(c)) {
        slab = slub_percpu_partial(c);
        slub_set_percpu_partial(c, slab);  // pop from per-cpu partial
        if (likely(node_match(slab, node))) {
            c->slab = slab;
            freelist = get_freelist(s, slab);
            goto load_freelist;        // 找到了!
        }
    }

    // 2. 尝试 Node partial list (需要 node->list_lock)
    freelist = get_partial_node(s, ...);
    if (freelist)
        goto check_new_slab;

    // 3. 最终手段: 从 buddy allocator 分配新页
    slab = new_slab(s, gfpflags, node);
    // new_slab → allocate_slab → alloc_slab_page → alloc_pages
    // 然后初始化 freelist: 用指针串联所有空闲对象
}
```

**SLUB 分配的层次化结构总结**:
```
┌─────────────────────────┐
│     快速路径 (无锁)      │ ← c->freelist 直接取
│   this_cpu_cmpxchg 或   │    ~10ns
│   local_lock             │
├─────────────────────────┤
│   slab->freelist 取      │ ← 其他CPU释放的对象
│   (local_lock held)      │    ~20ns
├─────────────────────────┤
│   CPU partial list       │ ← 本CPU缓存的半满slab
│   (local_lock held)      │    ~30ns
├─────────────────────────┤
│   Node partial list      │ ← node->list_lock (自旋锁)
│                          │    ~100ns
├─────────────────────────┤
│   Buddy allocator        │ ← zone->lock, 分配新页
│   (alloc_pages)          │    ~1000ns
└─────────────────────────┘
```

### 2.6 释放快速路径

快速路径通过 `freelist` 链表：
```
slab_free():
  1. 如果对象属于当前CPU的slab:
     → set_freepointer(s, object, c->freelist)  // object->next = old_head
     → c->freelist = object                      // 头插法
     → 使用 cmpxchg 保证原子性
  2. 否则 → __slab_free() (慢路径)
```

### 2.7 `__slab_free()` 慢路径

([slub.c](mm/slub.c#L5861)):
```
__slab_free(s, slab, head, tail, cnt):
  使用 CAS 循环:
  do {
    prior = slab->freelist
    set_freepointer(s, tail, prior)    // 把新对象链到旧freelist前
    new.inuse -= cnt
    if (slab变为非满 && 之前是满的 && 非frozen):
      → 需要把slab加入partial list
      → 获取 node->list_lock
  } while (!slab_update_freelist(cmpxchg))

  成功后:
  - 如果was_frozen: 什么都不做（CPU自己管理）
  - 如果slab从满变为partial: 放入 cpu partial list
  - 如果slab完全空: 可能释放回buddy allocator
```

### 2.8 复杂度

| 操作 | 时间复杂度 |
|------|-----------|
| 快速路径分配/释放 | **O(1)** amortized，无锁 |
| 慢路径（从partial取） | O(1) + 锁开销 |
| 新slab分配 | O(1) buddy + O(n) 初始化对象 |

**空间**: 每个slab page的元数据嵌入 `struct page`，每个对象的 freelist pointer 嵌入在对象空间内（对象空闲时），极低开销。

---

## 3. Buddy System (伙伴系统)

**Source**: `mm/page_alloc.c`

### 3.1 核心思想

Buddy system 管理物理页帧。将空闲内存组织为 2^order 大小的块（order 0..MAX_PAGE_ORDER）。分配时找最小合适块，必要时分裂；释放时检查"伙伴"是否空闲，若是则合并。

### 3.2 关键数据结构

```c
struct zone {
    struct free_area free_area[NR_PAGE_ORDERS];  // order 0 to MAX_PAGE_ORDER
    // ...
};

struct free_area {
    struct list_head free_list[MIGRATE_TYPES];  // 按迁移类型分组
    unsigned long nr_free;                       // 该order空闲块数
};
```

**迁移类型** (Migration Types):
```c
enum migratetype {
    MIGRATE_UNMOVABLE,     // 不可移动（内核分配）
    MIGRATE_MOVABLE,       // 可移动（用户页面）
    MIGRATE_RECLAIMABLE,   // 可回收（page cache）
    MIGRATE_PCPTYPES,      // per-cpu页类型数量
    MIGRATE_HIGHATOMIC,    // 紧急原子分配保留
    MIGRATE_CMA,           // CMA区域
    MIGRATE_ISOLATE,       // 隔离（热插拔/compaction）
};
```

### 3.3 分配算法: `__rmqueue_smallest()` ([page_alloc.c](mm/page_alloc.c#L1880))

```c
static __always_inline
struct page *__rmqueue_smallest(struct zone *zone, unsigned int order,
                                int migratetype)
{
    unsigned int current_order;
    struct free_area *area;
    struct page *page;

    /* 从请求的 order 开始向上搜索，找到第一个有空闲块的级别 */
    for (current_order = order; current_order < NR_PAGE_ORDERS; ++current_order) {
        area = &(zone->free_area[current_order]);
        page = get_page_from_free_area(area, migratetype);
        // 从 area->free_list[migratetype] 链表头取一个 page
        if (!page)
            continue;  // 该 order 无空闲块，尝试更大的

        /* 找到了! 如果 current_order > order，需要分裂 */
        page_del_and_expand(zone, page, order, current_order, migratetype);
        return page;
    }
    return NULL;  // 该迁移类型完全没有合适的块
}
```

**`page_del_and_expand()`** — 块分裂过程:
```
请求 order=0 (1页), 找到 order=3 (8页) 的块:

  Step 1: 从 free_area[3] 取出 8页块 [A B C D E F G H]
  Step 2: 右半 [E F G H] 放入 free_area[2] (4页)
  Step 3: 右半 [C D]     放入 free_area[1] (2页)
  Step 4: 右半 [B]       放入 free_area[0] (1页)
  Step 5: 返回 [A] 给调用者

  每次分裂都把"伙伴"(右半部分)放回更小一级的 freelist
```

### 3.4 释放与合并: `__free_one_page()` ([page_alloc.c](mm/page_alloc.c#L940))

```c
static inline void __free_one_page(struct page *page,
        unsigned long pfn, struct zone *zone,
        unsigned int order, int migratetype, fpi_t fpi_flags)
{
    unsigned long buddy_pfn = 0, combined_pfn;
    struct page *buddy;

    account_freepages(zone, 1 << order, migratetype);  // 更新zone统计

    /* 循环尝试合并伙伴，每次合并 order 加 1 */
    while (order < MAX_PAGE_ORDER) {
        int buddy_mt = migratetype;

        // 通过 XOR 计算伙伴的 PFN
        // buddy_pfn = pfn ^ (1 << order)
        buddy = find_buddy_page_pfn(page, pfn, order, &buddy_pfn);
        if (!buddy)
            goto done_merging;  // 伙伴不空闲，停止

        /* 大块(≥pageblock_order)需要检查迁移类型兼容性 */
        if (unlikely(order >= pageblock_order)) {
            buddy_mt = get_pfnblock_migratetype(buddy, buddy_pfn);
            if (migratetype != buddy_mt &&
                (!migratetype_is_mergeable(migratetype) ||
                 !migratetype_is_mergeable(buddy_mt)))
                goto done_merging;  // 不可合并（如CMA/ISOLATE）
        }

        /* 伙伴空闲! 从 freelist 取出它 */
        __del_page_from_free_list(buddy, zone, order, buddy_mt);

        /* 计算合并后的起始 PFN */
        combined_pfn = buddy_pfn & pfn;  // 取两者中较小的
        page = page + (combined_pfn - pfn);
        pfn = combined_pfn;
        order++;  // 升级到更大的 order
    }

done_merging:
    set_buddy_order(page, order);
    /* 选择放入链表头还是尾:
     * - 头部: 下次会被优先分配 (hot)
     * - 尾部: 留给更大的合并机会 (cold)
     */
    __add_to_free_list(page, zone, order, migratetype, to_tail);
}
```

**伙伴查找的数学原理**:
$$buddy\_pfn = pfn \oplus (1 << order)$$

例如，order=2 (4页块):
- PFN=0b**1000** 的伙伴是 0b**1100** (XOR 0b0100)
- PFN=0b**1100** 的伙伴是 0b**1000** (XOR 0b0100)
- 合并后: `combined_pfn = 0b1000 & 0b1100 = 0b1000`，order=3 (8页块)

```
释放 PFN=12 (order=0) 的合并过程:

  order=0: PFN=12 的伙伴是 PFN=13 (12^1)
           → PFN=13 空闲! 合并为 order=1, PFN=12

  order=1: PFN=12 的伙伴是 PFN=14 (12^2)
           → PFN=14 空闲! 合并为 order=2, PFN=12

  order=2: PFN=12 的伙伴是 PFN=8 (12^4)
           → PFN=8 不空闲. 停止合并.

  最终: 将 order=2 (4页) 的块放入 free_area[2]
```

### 3.5 Fallback 机制

**`__rmqueue()`** ([page_alloc.c](mm/page_alloc.c#L2445)):
```
__rmqueue(zone, order, migratetype):
  switch (*mode):
    RMQUEUE_NORMAL: __rmqueue_smallest(zone, order, migratetype)
    RMQUEUE_CMA:    __rmqueue_cma_fallback()
    RMQUEUE_CLAIM:  __rmqueue_claim()  // 尝试从其他migratetype"偷取"整个pageblock
    RMQUEUE_STEAL:  __rmqueue_steal()  // 偷取部分页
```

Fallback 顺序:
```c
static int fallbacks[MIGRATE_PCPTYPES][MIGRATE_PCPTYPES - 1] = {
    [MIGRATE_UNMOVABLE]   = { MIGRATE_RECLAIMABLE, MIGRATE_MOVABLE   },
    [MIGRATE_MOVABLE]     = { MIGRATE_RECLAIMABLE, MIGRATE_UNMOVABLE },
    [MIGRATE_RECLAIMABLE] = { MIGRATE_UNMOVABLE,   MIGRATE_MOVABLE   },
};
```

### 3.6 复杂度

| 操作 | 时间复杂度 |
|------|-----------|
| 分配 | O(MAX_ORDER) 最坏，通常 O(1) |
| 释放+合并 | O(MAX_ORDER) 最坏 |
| 伙伴查找 | O(1) — 简单XOR |

**空间**: 每个 `struct page` ≈ 64 bytes 元数据，free_area 数组 ~固定大小。

---

## 4. Page Reclaim / LRU (页面回收)

**Source**: `mm/vmscan.c`

### 4.1 核心思想

当系统内存不足时，需要回收页面。Linux 使用 LRU (Least Recently Used) 近似算法，维护 active/inactive 链表，通过"第二次机会"算法决定哪些页面可以回收。

### 4.2 LRU 链表

```
每个 lruvec 维护 5 条 LRU 链表:
  LRU_INACTIVE_ANON    // 不活跃匿名页（需要swap才能回收）
  LRU_ACTIVE_ANON      // 活跃匿名页
  LRU_INACTIVE_FILE    // 不活跃文件页（可直接丢弃/写回）
  LRU_ACTIVE_FILE      // 活跃文件页
  LRU_UNEVICTABLE      // 不可驱逐页（mlock等）
```

### 4.3 `scan_control` 结构

([vmscan.c](mm/vmscan.c#L73)):
```c
struct scan_control {
    unsigned long nr_to_reclaim;      // 需要回收的页数
    nodemask_t *nodemask;             // 允许的节点
    struct mem_cgroup *target_mem_cgroup;
    unsigned long anon_cost;          // 匿名页扫描代价
    unsigned long file_cost;          // 文件页扫描代价
    unsigned int may_writepage:1;     // 是否允许写回
    unsigned int may_unmap:1;         // 是否允许取消映射
    unsigned int may_swap:1;          // 是否允许swap
};
```

### 4.4 Second Chance / Reference Checking

**`folio_check_references()`** ([vmscan.c](mm/vmscan.c#L906)):
```
folio_check_references(folio, sc):
  referenced_ptes = folio_referenced(folio, ...)  // 检查PTE accessed bit
  referenced_folio = folio_test_clear_referenced(folio)  // 检查并清除folio的referenced标记

  if VM_LOCKED → FOLIOREF_ACTIVATE (不可回收)

  if referenced_ptes == -1 → FOLIOREF_KEEP (rmap锁竞争)

  if lru_gen_enabled():
    → 使用 MGLRU 多代 LRU 算法

  // 经典第二次机会算法:
  if referenced_ptes:
    folio_set_referenced(folio)        // 标记 referenced
    if (referenced_folio || referenced_ptes > 1):
      return FOLIOREF_ACTIVATE         // 提升到 active list
    return FOLIOREF_KEEP               // 给第二次机会

  if referenced_folio:
    return FOLIOREF_RECLAIM_CLEAN      // 可回收（如果干净）

  return FOLIOREF_RECLAIM              // 可回收
```

### 4.5 `shrink_folio_list()` — 回收决策核心 ([vmscan.c](mm/vmscan.c#L1099))

```c
static unsigned int shrink_folio_list(struct list_head *folio_list,
        struct pglist_data *pgdat, struct scan_control *sc,
        struct reclaim_stat *stat, bool ignore_references,
        struct mem_cgroup *memcg)
{
    unsigned int nr_reclaimed = 0;

    while (!list_empty(folio_list)) {
        struct folio *folio = lru_to_folio(folio_list);
        list_del(&folio->lru);

        /* Step 1: 尝试获取 folio 锁 — 失败则跳过 */
        if (!folio_trylock(folio))
            goto keep;       // 放回 LRU，下次再试

        /* Step 2: 检查是否为 hwpoisoned 页 */
        if (folio_contain_hwpoisoned_page(folio)) {
            unmap_poisoned_folio(folio, ...);
            continue;
        }

        /* Step 3: 不可驱逐的页(mlock等) → 提升到 active */
        if (unlikely(!folio_evictable(folio)))
            goto activate_locked;

        /* Step 4: 如果不允许 unmap，跳过已映射的页 */
        if (!sc->may_unmap && folio_mapped(folio))
            goto keep_locked;

        /* Step 5: 统计脏页和写回中的页 */
        folio_check_dirty_writeback(folio, &dirty, &writeback);

        /* Step 6: 引用检查 — "第二次机会"算法 */
        if (!ignore_references) {
            references = folio_check_references(folio, sc);
            switch (references) {
            case FOLIOREF_ACTIVATE:
                goto activate_locked;    // 活跃! 提升到 active list
            case FOLIOREF_KEEP:
                goto keep_locked;        // 给第二次机会，保留
            case FOLIOREF_RECLAIM:
            case FOLIOREF_RECLAIM_CLEAN:
                ;  // 可以回收，继续流程
            }
        }

        /* Step 7: 匿名页需要先加入 swap */
        if (folio_test_anon(folio) && !folio_test_swapcache(folio)) {
            if (!add_to_swap(folio))
                goto activate_locked;
        }

        /* Step 8: 取消所有进程的页表映射 */
        if (folio_mapped(folio)) {
            try_to_unmap(folio, ...);
            if (folio_mapped(folio))
                goto keep_locked;        // 还有映射未解除
        }

        /* Step 9: 脏页需要写回 */
        if (dirty) {
            pageout(folio, mapping, &plug);  // 触发 I/O
            goto keep;                       // 等写回完成后下一轮再回收
        }

        /* Step 10: 干净且未映射 → 释放! */
        nr_reclaimed += folio_nr_pages(folio);
        folio_batch_add(&free_folios, folio);
        continue;

    activate_locked:
        folio_set_active(folio);     // 设置 active 标志
        pgactivate++;
    keep_locked:
        folio_unlock(folio);
    keep:
        list_add(&folio->lru, &ret_folios);  // 放回待归还列表
    }
    // 将 ret_folios 放回 LRU
    // 释放 free_folios 中的页面
    return nr_reclaimed;
}
```

**回收决策流程图**:
```
folio 从 inactive LRU 尾部取出
        │
  ┌─trylock 成功?──┐
  │NO              │YES
  │→ keep          ↓
  │         evictable?
  │        YES ↓    NO → activate
  │         ↓
  │   check_references()
  │   ┌──────┬──────────┐
  │  ACTIVATE KEEP    RECLAIM
  │   ↓       ↓         ↓
  │  active  keep   anon? → add_to_swap
  │  list    list        ↓
  │                 mapped? → try_to_unmap
  │                      ↓
  │                 dirty? → pageout (I/O)
  │                      ↓
  │                 释放页面! ✓
  └─────────────────────┘
```

### 4.6 `shrink_inactive_list()`

([vmscan.c](mm/vmscan.c#L2008)):
```
shrink_inactive_list(nr_to_scan, lruvec, sc, lru):
  1. lru_add_drain()                    // 把per-cpu的pagevec刷到LRU
  2. spin_lock(&lruvec->lru_lock)
  3. isolate_lru_folios(nr_to_scan, ...)  // 从LRU尾部隔离一批folio
  4. spin_unlock(&lruvec->lru_lock)
  5. shrink_folio_list(&folio_list, ...)   // 对隔离的folio逐一回收
  6. 将未回收的folio放回LRU
```

### 4.7 kswapd vs Direct Reclaim

- **kswapd**: 后台内核线程，当空闲页低于 `pages_low` 水位时被唤醒，回收到 `pages_high`
- **Direct reclaim**: 当分配路径中空闲页低于 `pages_min` 时，分配者自己同步回收
- `balance_pgdat()` 是 kswapd 的主循环
- 扫描压力在 anon 和 file 之间平衡：基于 `anon_cost` / `file_cost`

### 4.8 复杂度

| 操作 | 时间复杂度 |
|------|-----------|
| 从 LRU 隔离 | O(n) n=nr_to_scan |
| folio_check_references | O(映射数) per folio |
| shrink_folio_list | O(n) n=隔离数 |
| kswapd 一轮 | O(扫描页数) |

---

## 5. VMA Management (Maple Tree)

**Source**: `mm/mmap.c`

### 5.1 核心思想

Linux 6.x 用 **Maple Tree** 替代了传统的 RB-tree + 链表来管理进程的虚拟内存区域 (VMA)。Maple Tree 是一个 RCU-safe 的 B-tree 变体，针对范围查询优化。

### 5.2 关键数据结构

```c
struct mm_struct {
    struct maple_tree mm_mt;    // VMA 的 maple tree
    // ...
};

struct vm_area_struct {
    unsigned long vm_start;     // VMA 起始地址
    unsigned long vm_end;       // VMA 结束地址
    struct file *vm_file;       // 映射的文件（如果有）
    vm_flags_t vm_flags;        // 权限和属性标志
    // ...
};
```

### 5.3 `find_vma()` — O(log n) 查找

([mmap.c](mm/mmap.c#L904)):
```c
struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr)
{
    unsigned long index = addr;
    mmap_assert_locked(mm);
    return mt_find(&mm->mm_mt, &index, ULONG_MAX);
}
```

`mt_find()` 在 maple tree 中执行范围搜索，找到包含 `addr` 的 VMA 或下一个 VMA。

### 5.4 `find_vma_intersection()`

([mmap.c](mm/mmap.c#L885)):
```c
struct vm_area_struct *find_vma_intersection(struct mm_struct *mm,
                         unsigned long start_addr, unsigned long end_addr)
{
    unsigned long index = start_addr;
    mmap_assert_locked(mm);
    return mt_find(&mm->mm_mt, &index, end_addr - 1);
}
```

### 5.5 `do_mmap()` — 创建映射

([mmap.c](mm/mmap.c#L334)):
```
do_mmap(file, addr, len, prot, flags, pgoff):
  1. 参数验证（长度、溢出、map_count限制）
  2. calc_vm_prot_bits() + calc_vm_flag_bits() → 计算 vm_flags
  3. get_unmapped_area() → 在maple tree中找到空闲地址空间
  4. mmap_region() → 实际创建VMA:
     a) 尝试与相邻VMA合并 (vma_merge())
     b) 如果不能合并，创建新VMA
     c) 插入maple tree: vma_iter_store()
     d) 如果是文件映射: 调用 file->f_op->mmap()
```

### 5.6 Maple Tree vs RB-tree

| 特性 | Maple Tree | RB-tree |
|------|-----------|---------|
| 结构 | B-tree variant (fan-out 最大16) | Binary tree |
| 缓存友好 | 更好（节点内连续数据） | 较差（每个节点2个子节点） |
| RCU-safe | 原生支持 | 需要额外处理 |
| 范围查询 | 优化的 | 需要遍历 |
| 查找复杂度 | O(log_B n) | O(log_2 n) |

### 5.7 复杂度

| 操作 | 时间复杂度 |
|------|-----------|
| find_vma | O(log n) |
| 插入/删除 VMA | O(log n) |
| VMA 合并 | O(log n) |

---

## 6. Writeback Algorithm (写回算法)

**Source**: `mm/page-writeback.c`, `fs/fs-writeback.c`

### 6.1 核心思想

Writeback 系统控制脏页何时、如何写回到磁盘。通过设定脏页比例阈值，平衡 I/O 性能和数据安全性。

### 6.2 关键参数

([page-writeback.c](mm/page-writeback.c#L74)):
```c
static int dirty_background_ratio = 10;   // 脏页占可脏化内存 10% 时触发后台写回
static unsigned long dirty_background_bytes; // 或绝对字节数
static int vm_dirty_ratio = 20;            // 脏页占 20% 时开始限流写入者
static unsigned long vm_dirty_bytes;
static long ratelimit_pages = 32;          // 每个CPU脏了32页后检查是否需要限流
```

### 6.3 Dirty Limits 计算

**`domain_dirty_limits()`** ([page-writeback.c](mm/page-writeback.c#L356)):
```
domain_dirty_limits(dtc):
  available_memory = dtc->avail   // 可脏化内存总量

  if vm_dirty_bytes:
    thresh = vm_dirty_bytes / PAGE_SIZE
  else:
    thresh = vm_dirty_ratio * available_memory / 100

  if dirty_background_bytes:
    bg_thresh = dirty_background_bytes / PAGE_SIZE
  else:
    bg_thresh = dirty_background_ratio * available_memory / 100

  // 实时任务获得 25% 额外配额
  if rt_or_dl_task(current):
    thresh += thresh / 4
    bg_thresh += bg_thresh / 4
```

### 6.4 两阶段写回机制

```
                    0%          bg_thresh (10%)      thresh (20%)
  脏页比例:  |--------|----------------|----------------|----->
             正常写入   后台写回开始      限流写入者（throttle）

  1. 脏页 < bg_thresh: 不做写回
  2. bg_thresh ≤ 脏页 < thresh: 唤醒 writeback 线程后台写回
  3. 脏页 ≥ thresh: balance_dirty_pages() 限制写入者速度
```

### 6.5 `balance_dirty_pages()`

限流的核心是让写入者在 `balance_dirty_pages()` 中 sleep:
- 根据当前脏页量和写带宽估计，计算需要暂停的时间
- `MAX_PAUSE = max(HZ/5, 1)` — 单次最大 sleep 200ms
- 使用带宽估计 (`BANDWIDTH_INTERVAL = 200ms`) 来平滑限流

### 6.6 BDI (Backing Device Info)

每个块设备有自己的写回带宽跟踪和脏页配额，防止一个慢设备拖慢整个系统。

---

## 7. I/O Schedulers

### 7.1 mq-deadline

**Source**: `block/mq-deadline.c`

#### 核心算法

**`__dd_dispatch_request()`** ([mq-deadline.c](block/mq-deadline.c#L313)):

```
__dd_dispatch_request(dd, per_prio, latest_start):
  1. 检查 dispatch 队列是否有待处理请求

  2. 批处理续接:
     if 有下一个请求 && batching < fifo_batch:
       → 继续当前方向的请求（保持连续性）

  3. 方向选择:
     if 有读请求:
       if 有写请求 && starved >= writes_starved:
         → 切换到写（防止写饥饿，默认 writes_starved=2）
       else:
         → 选择读

     else if 有写请求:
       → 选择写

  4. 请求选择:
     if deadline 过期 || 没有下一个连续请求:
       → 从 FIFO 链表取最早过期的请求（按到达时间排序）
     else:
       → 取扇区排序的下一个请求（电梯式）

  5. deadline_move_request() → 从调度队列移到 dispatch 队列
```

**数据结构**:
- `sort_list[DD_READ/DD_WRITE]`: 按扇区号排序的 RB-tree（电梯合并）
- `fifo_list[DD_READ/DD_WRITE]`: 按到达时间排序的 FIFO（deadline 保证）
- 优先级分为 3 级 (RT, BE, IDLE)
- 默认: read_expire=500ms, write_expire=5000ms, fifo_batch=16

### 7.2 BFQ (Budget Fair Queueing)

**Source**: `block/bfq-iosched.c`

#### 核心算法

**`bfq_select_queue()`** ([bfq-iosched.c](block/bfq-iosched.c#L4798)): 选择下一个要服务的 bfq_queue
**`bfq_dispatch_rq_from_bfqq()`** ([bfq-iosched.c](block/bfq-iosched.c#L5100)): 从选中的队列取出请求

BFQ 基于 **B-WF²Q+** (Budget Weighted Fair Queuing) 算法:
- 每个进程/cgroup 有自己的 `bfq_queue`
- 每个队列有一个 **budget**（可以发送的扇区数）
- 使用时间戳实现公平性
- 特别优化了交互式/延迟敏感的工作负载

### 7.3 复杂度

| 调度器 | dispatch | insert | 适用场景 |
|--------|----------|--------|---------|
| mq-deadline | O(1)/O(log n) | O(log n) | 通用，数据库 |
| BFQ | O(log n) | O(log n) | 桌面，交互式 |

---

## 8. RCU Grace Period Detection

**Source**: `kernel/rcu/tree.c`

### 8.1 核心思想

RCU (Read-Copy Update) 允许读者无锁访问共享数据。写者更新数据后，必须等到所有可能持有旧引用的读者完成后（grace period），才能安全释放旧数据。

### 8.2 rcu_node 树结构

```
rcu_state
  └── rcu_node[0]               (root)
       ├── rcu_node[1]          (intermediate)
       │    ├── rcu_data[CPU0]
       │    └── rcu_data[CPU1]
       └── rcu_node[2]          (intermediate)
            ├── rcu_data[CPU2]
            └── rcu_data[CPU3]
```

- `rcu_state`: 全局状态，包含 `gp_seq`（grace period 序号）
- `rcu_node`: 层次结构节点，每个有 `qsmask` 位图追踪哪些子节点/CPU已报告静默状态
- `rcu_data`: Per-CPU 数据，包含回调链表

### 8.3 Grace Period 初始化 — `rcu_gp_init()` ([tree.c](kernel/rcu/tree.c#L1803))

```c
static noinline_for_stack bool rcu_gp_init(void)
{
    struct rcu_node *rnp = rcu_get_root();

    raw_spin_lock_irq_rcu_node(rnp);

    if (!rcu_state.gp_flags) {
        raw_spin_unlock_irq_rcu_node(rnp);
        return false;  // 虚假唤醒，没有GP请求
    }
    WRITE_ONCE(rcu_state.gp_flags, 0);  // 清除标志: 新GP开始

    /* 推进 gp_seq — 标记新GP开始
     * rcu_seq_start() 将 gp_seq 的低2位从 0b00 变为 0b01
     * 表示"GP进行中" */
    rcu_seq_start(&rcu_state.gp_seq);
    raw_spin_unlock_irq_rcu_node(rnp);

    /* 遍历所有叶子 rcu_node，设置 qsmask
     * qsmask 中每一位代表一个在线CPU
     * 所有位都被清除(所有CPU报告QS)后，GP完成 */
    rcu_for_each_leaf_node(rnp) {
        raw_spin_lock_rcu_node(rnp);
        oldmask = rnp->qsmaskinit;
        rnp->qsmaskinit = rnp->qsmaskinitnext;  // 应用CPU上下线变化
        rnp->qsmask = rnp->qsmaskinit;          // 设置本GP的qsmask
        raw_spin_unlock_rcu_node(rnp);
    }
}
```

### 8.4 Grace Period Kthread 主循环

**`rcu_gp_kthread()`** ([tree.c](kernel/rcu/tree.c#L2259)):
```
rcu_gp_kthread():
  for (;;):
    // Phase 1: 等待GP请求
    swait_event(rcu_state.gp_wq, gp_flags & RCU_GP_FLAG_INIT)
    
    // Phase 2: 初始化GP — rcu_gp_init()
    //   → 推进 gp_seq，设置所有 rcu_node 的 qsmask

    // Phase 3: 等待所有CPU报告 Quiescent State
    //   → rcu_gp_fqs_loop() 循环:
    //     每 jiffies_till_first_fqs 后开始强制QS检测
    //     对每个未报告QS的CPU发送 IPI 或检查其状态

    // Phase 4: 清理 — rcu_gp_cleanup()
    //   → 推进 gp_seq 完成 (低2位从 0b01 → 0b00, 高位+1)
    //   → 唤醒等待此GP的回调
```

### 8.5 Quiescent State 报告 — 自底向上传播

```c
// 每个CPU通过以下事件报告静默状态:
// - 上下文切换 → rcu_note_context_switch()
// - 进入idle   → rcu_idle_enter()
// - 返回用户态 → rcu_user_enter()
// - 显式标记   → cond_resched_rcu_qs()

rcu_report_qs_rnp(mask, rnp):
  for (;;):
    rnp->qsmask &= ~mask        // 清除已报告QS的CPU/子节点位
    if (rnp->qsmask != 0)
      break                      // 还有未报告的，等待
    rnp = rnp->parent            // 所有子节点完成 → 向上传播
    mask = rnp_child_mask

  if 到达 root && root->qsmask == 0:
    rcu_report_qs_rsp()          // GP 完成!
```

**QS 传播示例 (4 CPU 系统)**:
```
         rcu_node[0] (root)
        qsmask = 0b11
          /        \
   rcu_node[1]   rcu_node[2]
   qsmask=0b11   qsmask=0b11
    /    \         /    \
  CPU0  CPU1    CPU2  CPU3

Step 1: CPU0 上下文切换 → rcu_node[1].qsmask = 0b10
Step 2: CPU1 进入idle   → rcu_node[1].qsmask = 0b00
        → 向上: rcu_node[0].qsmask = 0b10
Step 3: CPU3 返回用户态 → rcu_node[2].qsmask = 0b01
Step 4: CPU2 cond_resched → rcu_node[2].qsmask = 0b00
        → 向上: rcu_node[0].qsmask = 0b00 → GP 完成!
```

### 8.6 复杂度

| 操作 | 时间复杂度 |
|------|-----------|
| `rcu_read_lock/unlock` | **O(1)**, 无原子操作 (preemptible RCU有开销) |
| QS 报告传播 | O(tree depth) = O(log(NR_CPUS)) |
| GP 完成 | O(NR_CPUS) 总扫描 |
| 回调执行 | O(callback数量) |

---

## 9. Timer Wheel (定时器轮)

**Source**: `kernel/time/timer.c`

### 9.1 核心思想

Linux 使用 **分层定时器轮** (Hierarchical Timing Wheel) 管理内核定时器。不同于经典的需要级联的实现，当前版本使用隐式批处理：越远的定时器精度越低，避免了级联开销。

### 9.2 层次结构 (HZ=1000)

([timer.c](kernel/time/timer.c#L85)):
```
Level  Offset  Granularity       Range
  0      0         1 ms          0 ms -     63 ms
  1     64         8 ms         64 ms -    511 ms
  2    128        64 ms        512 ms -   ~4s
  3    192       512 ms         ~4s -     ~32s
  4    256      4096 ms (~4s)   ~32s -    ~4m
  5    320     32768 ms (~32s)  ~4m -     ~34m
  6    384    262144 ms (~4m)   ~34m -    ~4h
  7    448   2097152 ms (~34m)  ~4h -     ~1d
  8    512  16777216 ms (~4h)   ~1d -     ~12d
```

**常量**:
```c
#define LVL_BITS        6                    // 每级 64 个桶
#define LVL_SIZE        (1UL << LVL_BITS)    // = 64
#define LVL_DEPTH       9                    // 9个层级 (HZ > 100)
#define WHEEL_SIZE      (LVL_SIZE * LVL_DEPTH)  // = 576 个桶
#define LVL_CLK_SHIFT   3                    // 每级时钟除以 8
```

### 9.3 `timer_base` 结构

([timer.c](kernel/time/timer.c#L250)):
```c
struct timer_base {
    raw_spinlock_t  lock;
    struct timer_list *running_timer;
    unsigned long   clk;                    // 当前时钟
    unsigned long   next_expiry;            // 最近到期时间
    bool            timers_pending;
    DECLARE_BITMAP(pending_map, WHEEL_SIZE); // 576位bitmap，标记哪些桶有定时器
    struct hlist_head vectors[WHEEL_SIZE];   // 576个桶（哈希链表）
};
```

每CPU有多个 `timer_base`: LOCAL, GLOBAL, DEF(deferrable)。

### 9.4 定时器添加

**`calc_wheel_index()`** ([timer.c](kernel/time/timer.c#L542)):
```c
static int calc_wheel_index(unsigned long expires, unsigned long clk,
                            unsigned long *bucket_expiry)
{
    unsigned long delta = expires - clk;     // 距当前时钟的差值
    unsigned int idx;

    // 根据 delta 大小选择层级 — 越远的定时器放越高层(粒度越粗)
    if (delta < LVL_START(1)) {              // 0 - 63 ticks
        idx = calc_index(expires, 0, bucket_expiry);
    } else if (delta < LVL_START(2)) {       // 64 - 511 ticks
        idx = calc_index(expires, 1, bucket_expiry);
    } else if (delta < LVL_START(3)) {       // 512 - 4095 ticks
        idx = calc_index(expires, 2, bucket_expiry);
    } else if (delta < LVL_START(4)) {
        idx = calc_index(expires, 3, bucket_expiry);
    } /* ... 最多 9 级 ... */
    else if ((long) delta < 0) {
        idx = clk & LVL_MASK;               // 已过期! 放到当前桶
        *bucket_expiry = clk;
    } else {
        if (delta >= WHEEL_TIMEOUT_CUTOFF)
            expires = clk + WHEEL_TIMEOUT_MAX; // 超大timeout截断
        idx = calc_index(expires, LVL_DEPTH - 1, bucket_expiry);
    }
    return idx;
}

static inline unsigned calc_index(unsigned long expires, unsigned lvl,
                                  unsigned long *bucket_expiry)
{
    // 对齐到该层级的粒度（向上取整，防止过早触发）
    expires = (expires >> LVL_SHIFT(lvl)) + 1;
    *bucket_expiry = expires << LVL_SHIFT(lvl);
    return LVL_OFFS(lvl) + (expires & LVL_MASK);
    // LVL_OFFS(lvl) = lvl * 64    — 每级64个桶
    // LVL_MASK = 63               — 桶内偏移
}
```

**`enqueue_timer()`** ([timer.c](kernel/time/timer.c#L612)):
```c
static void enqueue_timer(struct timer_base *base, struct timer_list *timer,
                          unsigned int idx, unsigned long bucket_expiry)
{
    hlist_add_head(&timer->entry, base->vectors + idx);  // O(1) 插入对应桶
    __set_bit(idx, base->pending_map);                    // 标记 bitmap 中该桶有定时器
    timer_set_idx(timer, idx);

    // 如果这是最早到期的定时器，更新 base->next_expiry
    if (time_before(bucket_expiry, base->next_expiry)) {
        base->next_expiry = bucket_expiry;
        base->timers_pending = true;
        trigger_dyntick_cpu(base, timer);  // 唤醒处于idle的CPU
    }
}
```

### 9.5 到期定时器的收集与处理

**`collect_expired_timers()`** ([timer.c](kernel/time/timer.c#L1807)):
```c
static int collect_expired_timers(struct timer_base *base,
                                  struct hlist_head *heads)
{
    unsigned long clk = base->clk = base->next_expiry;
    struct hlist_head *vec;
    int i, levels = 0;
    unsigned int idx;

    for (i = 0; i < LVL_DEPTH; i++) {
        // 计算当前时钟在该层级对应的桶索引
        idx = (clk & LVL_MASK) + i * LVL_SIZE;

        // 检查 bitmap 中该桶是否有定时器
        if (__test_and_clear_bit(idx, base->pending_map)) {
            vec = base->vectors + idx;
            hlist_move_list(vec, heads++);  // 整个链表移到 heads 数组
            levels++;
        }
        // 检查是否需要看更高层级:
        // 只有当低位全为0时才需要(时钟对齐到该层粒度)
        if (clk & LVL_CLK_MASK)
            break;
        clk >>= LVL_CLK_SHIFT;  // 右移3位，检查下一层
    }
    return levels;
}
```

**`__run_timers()`** ([timer.c](kernel/time/timer.c#L2343)):
```c
static inline void __run_timers(struct timer_base *base)
{
    struct hlist_head heads[LVL_DEPTH];
    int levels;

    while (time_after_eq(jiffies, base->clk) &&
           time_after_eq(jiffies, base->next_expiry)) {
        levels = collect_expired_timers(base, heads);
        base->clk++;                        // 推进时钟
        timer_recalc_next_expiry(base);     // 重新计算最近到期时间

        while (levels--)
            expire_timers(base, heads + levels);  // 逐个执行回调
    }
}
```

### 9.5 优势 vs 经典实现

- **无级联**: 不需要在轮转动时把定时器从高层移到低层
- **隐式批处理**: 高层定时器精度较低，但大多数定时器是超时用的（取消居多），精度要求不高
- **O(1) 添加**: `calc_wheel_index` + `hlist_add_head`
- **高效到期查找**: 使用 `pending_map` bitmap 的 `find_next_bit()`

### 9.6 复杂度

| 操作 | 时间复杂度 |
|------|-----------|
| 添加定时器 | **O(1)** |
| 删除定时器 | **O(1)** |
| 查找下一个到期 | O(WHEEL_SIZE/BITS_PER_LONG) ≈ O(1) |
| 处理到期定时器 | O(桶中定时器数) |

**空间**: `WHEEL_SIZE * sizeof(hlist_head)` + bitmap = ~5KB per CPU per base.

---

## 10. Kernel Sort (内核排序)

**Source**: `lib/sort.c`

### 10.1 核心思想

Linux 内核的 `sort()` 实现是一个 **底向上的堆排序 (Bottom-up Heapsort)** 变体，非递归，O(1) 额外空间，且可以在 SOFTIRQ 中调用（不使用栈递归）。

### 10.2 `__sort_r()` 完整实现源码注释 ([sort.c](lib/sort.c#L196))

```c
static void __sort_r(void *base, size_t num, size_t size,
                     cmp_r_func_t cmp_func, swap_r_func_t swap_func,
                     const void *priv, bool may_schedule)
{
    /* 预缩放计数器: 所有偏移都以字节为单位而非元素索引 */
    size_t n = num * size;              // 总字节数
    size_t a = (num/2) * size;          // 建堆起始位置(最后一个非叶节点)
    const unsigned int lsbit = size & -size;  // size的最低有效位
    size_t shift = 0;                   // 用于一次提取两个元素的优化

    if (!a)     /* num < 2 || size == 0 → 不需要排序 */
        return;

    /* 自动选择最优 swap 策略 */
    if (!swap_func) {
        if (is_aligned(base, size, 8))
            swap_func = SWAP_WORDS_64;      // 64位块交换(最快)
        else if (is_aligned(base, size, 4))
            swap_func = SWAP_WORDS_32;      // 32位块交换
        else
            swap_func = SWAP_BYTES;         // 逐字节(fallback)
    }

    /*
     * 主循环: 两个阶段
     * Phase 1 (a > 0): 建堆 — 从中间向前扫描，每个元素 sift down
     * Phase 2 (a == 0): 排序 — 反复提取最大元素放到数组尾部
     */
    for (;;) {
        size_t b, c, d;

        if (a)                          /* Phase 1: 建堆 */
            a -= size << shift;         // 下一个需要 sift down 的位置
        else if (n > 3 * size) {        /* Phase 2: 排序(每次提取两个!) */
            n -= size;
            do_swap(base, base + n, size, swap_func, priv);
            // ↑ 堆顶(最大值)放到 base[n]

            // 巧妙优化: 比较堆顶的两个子节点，
            // 更大的那个也直接提取出来!
            shift = do_cmp(base + size, base + 2*size, cmp_func, priv) <= 0;
            a = size << shift;          // a = size 或 2*size
            n -= size;
            do_swap(base + a, base + n, size, swap_func, priv);
            // ↑ 第二大值也放到数组尾部
        } else {
            break;  /* 排序完成 (剩余 ≤ 3 个元素) */
        }

        /*
         * Bottom-up sift down — 核心创新!
         *
         * 传统 sift down: 每层2次比较 (元素 vs 左子, 元素 vs 最大子)
         * Bottom-up: 分三步
         */

        /* Step 1: 从位置 a 出发，沿最大子节点一路走到叶子
         * 每层只比较两个子节点(不与目标元素比较)
         * → 每层只需 1 次比较 (vs 传统的 2 次)
         */
        for (b = a; c = 2*b + size, (d = c + size) < n;)
            b = do_cmp(base + c, base + d, cmp_func, priv) > 0 ? c : d;
        if (d == n)                     // 特殊情况: 最后一个节点只有左子
            b = c;

        /* Step 2: 从叶子回溯，找到 a 元素的正确插入位置
         * 大多数元素会 sift 到接近叶子的位置，
         * 所以这个回溯通常很短
         */
        while (b != a && do_cmp(base + a, base + b, cmp_func, priv) >= 0)
            b = parent(b, lsbit, size);

        /* Step 3: 通过旋转把 a 元素移到找到的位置
         * 从 b 开始向上 swap，直到 a
         */
        c = b;
        while (b != a) {
            b = parent(b, lsbit, size);
            do_swap(base + b, base + c, size, swap_func, priv);
        }

        if (may_schedule)
            cond_resched();             // 长排序时让出CPU
    }

    /* 处理最后 2-3 个元素 */
    n -= size;
    do_swap(base, base + n, size, swap_func, priv);
    if (n == size * 2 && do_cmp(base, base + size, cmp_func, priv) > 0)
        do_swap(base, base + size, size, swap_func, priv);
}
```

**`parent()` 函数 — 无除法的父节点计算**:
```c
static size_t parent(size_t i, unsigned int lsbit, size_t size)
{
    i -= size;
    i -= size & -(i & lsbit);  // 无分支的 "if (i & lsbit) i -= size"
    return i / 2;
}
// 数组索引中: parent(j) = (j-1)/2
// 但字节偏移中不能直接除，需要先对齐
```

### 10.3 Bottom-up 优化

传统堆排序在 sift-down 时每层做2次比较（与两个子节点）。Bottom-up 变体：
1. **下行**: 每层只做1次比较（两个子节点之间比较），一路走到叶子
2. **上行**: 从叶子回溯，做1次比较找到插入位置

由于元素通常 sift 到接近叶子的位置，平均比较次数约为标准版的 **一半多一点**。

### 10.4 Swap 优化

```c
// 根据对齐选择最优 swap 策略
if (is_aligned(base, size, 8))  → swap_words_64 (64位块交换)
elif (is_aligned(base, size, 4)) → swap_words_32 (32位块交换)
else → swap_bytes (逐字节)
```

### 10.5 复杂度

| 指标 | 值 |
|------|---|
| 平均比较次数 | $n \log_2 n + 0.37n + o(n)$ |
| 最坏比较次数 | $1.5n \log_2 n + O(n)$ |
| 时间复杂度 | **O(n log n)** |
| 空间复杂度 | **O(1)** — 完全原地 |
| 稳定性 | **不稳定** |
| 递归 | **无** — 纯迭代 |

**实际意义**: 相比 quicksort 的 $n\log_2 n - 1.26n$ 平均比较次数，堆排序多约 $1.63n$ 次比较，但保证 O(n log n) 最坏情况、O(1) 空间且无栈溢出风险——这在内核中至关重要。

---

## 11. CRC/Hash Algorithms

### 11.1 Multiplicative Hash (`hash_long`)

**Source**: `include/linux/hash.h`

([hash.h](include/linux/hash.h#L1)):

#### 算法

```c
#define GOLDEN_RATIO_32 0x61C88647
#define GOLDEN_RATIO_64 0x61C8864680B583EBull

static inline u32 hash_32(u32 val, unsigned int bits)
{
    return (val * GOLDEN_RATIO_32) >> (32 - bits);
}

static __always_inline u32 hash_64(u64 val, unsigned int bits)
{
    return val * GOLDEN_RATIO_64 >> (64 - bits);
}

static inline u32 hash_ptr(const void *ptr, unsigned int bits)
{
    return hash_long((unsigned long)ptr, bits);
}
```

#### 原理

使用 **Knuth 乘法散列**:
$$h(k) = \lfloor (k \cdot A \cdot 2^w) \gg (w - p) \rfloor$$

其中 $A$ 是黄金比例的负数 $(1 - \phi) = \phi^2 = (3 - \sqrt{5})/2$。

**黄金比例的数学特性**使得输入的任何位变化都能很好地扩散到输出的高位。取高位是因为乘法的传播方向是向高位的。

#### 复杂度

- **O(1)** — 单次乘法 + 移位
- 分布质量优秀，特别是对于连续整数和指针值

### 11.2 Jenkins Hash (jhash)

**用途**: 网络子系统（连接跟踪hash表、路由缓存）

```
jhash(key, length, initval):
  混合3个32位变量 a, b, c
  每次消费12字节，通过位运算（加法、减法、XOR、移位）充分混合
  最终返回 c
```

- 设计目标: 每个输入位影响每个输出位（雪崩效应）
- O(length) 时间
- 在内核网络子系统中广泛使用

### 11.3 CRC32

- 使用查表法 (table-driven CRC)
- 内核支持多种变体: CRC32, CRC32c (Castagnoli)
- CRC32c 在某些架构上有硬件加速 (如 x86 SSE4.2)
- 主要用于: 文件系统校验、网络校验和

---

## 总结对比

| 算法 | 核心数据结构 | 时间复杂度(关键操作) | 空间复杂度 | 主要优化 |
|------|-------------|---------------------|-----------|---------|
| CFS | Augmented RB-tree | O(log n) | O(n) | EEVDF, leftmost cache, 预计算逆权重 |
| SLUB | Per-CPU freelist + partial lists | O(1) fast path | O(n slabs) | 无锁快速路径, cmpxchg |
| Buddy | Free area arrays | O(MAX_ORDER) | O(pages) | Migration types, PCP cache |
| LRU/Reclaim | Active/Inactive lists | O(scan count) | O(1) per page | 第二次机会, MGLRU |
| VMA/Maple Tree | B-tree variant | O(log n) | O(n VMAs) | 高扇出, RCU-safe |
| Writeback | Per-BDI bandwidth | O(1) throttle | O(1) | 自适应带宽估计 |
| mq-deadline | RB-tree + FIFO | O(log n) | O(requests) | 读写分离, 批处理 |
| RCU | rcu_node tree | O(1) read, O(CPUs) GP | O(CPUs) | 树形QS传播, 无读端锁 |
| Timer Wheel | Hierarchical hash | O(1) add/del | O(WHEEL_SIZE) per CPU | 无级联, bitmap快速查找 |
| Kernel Sort | In-place array | O(n log n) | O(1) | Bottom-up heapsort, 无递归 |
| Hash | — | O(1) | O(1) | Golden ratio 乘法散列 |
