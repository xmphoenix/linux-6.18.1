# Linux Buddy Allocator 反碎片化机制深度分析

> 基于 Linux 6.18.1，ARM64 架构，源码路径：`mm/page_alloc.c` · `mm/compaction.c` · `arch/arm64/mm/init.c`

---

## 目录

1. [碎片化的根源与反碎片化体系总览](#一碎片化的根源与反碎片化体系总览)
2. [迁移类型分组：按移动性隔离 pageblock](#二迁移类型分组按移动性隔离-pageblock)
3. [分层 Fallback 策略：Claim vs Steal](#三分层-fallback-策略claim-vs-steal)
4. [ALLOC_NOFRAGMENT 与 defrag_mode](#四alloc_nofragment-与-defrag_mode)
5. [HIGHATOMIC 原子高阶保留](#五highatomic-原子高阶保留)
6. [CMA：可迁移连续内存](#六cma可迁移连续内存)
7. [水位线提升 Watermark Boosting](#七水位线提升-watermark-boosting)
8. [<span style="color:red">**★ 页面回收 Page Reclaim**</span>](#八页面回收-page-reclaim)
9. [内存压缩 Compaction](#九内存压缩-compaction)
10. [ARM64 架构特殊配置](#十arm64-架构特殊配置)
11. [反碎片化体系总图](#十一反碎片化体系总图)
12. [ARM64 典型场景举例](#十二arm64-典型场景举例)

---

## 一、碎片化的根源与反碎片化体系总图

### 1.1 碎片化的本质

物理内存碎片化分为两类：

**外部碎片**：空闲页被已分配页割裂，导致不存在足够大的**连续**空闲块满足高阶分配请求。

```
[已分配][空闲][已分配][空闲][空闲][已分配][空闲]   ← order=0 ok，order=3 fail
```

**内部碎片**：分配了 2^order 页但实际只用一小部分，浪费页内空间。

本
文主要关注**外部碎片**。

### 1.2 八层防御体系

```
  ┌─────────────────────────────────────────────────────────────┐
  │ 层 1: 页块迁移类型分组 (Pageblock Grouping by Mobility)       │
  │    ├─ UNMOVABLE 页组       (内核数据结构/DMA)                │
  │    ├─ MOVABLE 页组         (用户进程/文件缓存)               │
  │    └─ RECLAIMABLE 页组     (可回收页)                       │
  ├─────────────────────────────────────────────────────────────┤
  │ 层 2: 分层 Fallback (优先同类型，再跨类型)                     │
  │    NORMAL → CMA → CLAIM → STEAL                             │
  ├─────────────────────────────────────────────────────────────┤
  │ 层 3: ALLOC_NOFRAGMENT 标志                                  │
  │    阻止跨节点 fallback 时的 stealing，保护本地页块完整性       │
  ├─────────────────────────────────────────────────────────────┤
  │ 层 4: HIGHATOMIC 保留 (原子高阶分配)                         │
  │    为高优先级原子分配保留约 1% zone 管理的页块                │
  ├─────────────────────────────────────────────────────────────┤
  │ 层 5: CMA 区域 (长期可迁移的连续内存)                        │
  │    MOVABLE 共享，UNMOVABLE 可临时征用，后台迁移回 CMA         │
  ├─────────────────────────────────────────────────────────────┤
  │ 层 6: 水位线提升 (Watermark Boosting)                        │
  │    fallback 后提高 watermark，加速 kswapd 回收               │
  ├─────────────────────────────────────────────────────────────┤
  │ <span style="color:red">★ 页面回收 (Page Reclaim)</span>                                   │
  │    <span style="color:red">kswapd 后台 / Direct 直接回收，LRU 扫描 + shrink_slab，归还 buddy</span> │
  ├─────────────────────────────────────────────────────────────┤
  │ 层 7: kcompactd 后台压缩 (Proactive Compaction)              │
  │    pageblock 被不同迁移类型侵占后，kcompactd 压缩重构         │
  ├─────────────────────────────────────────────────────────────┤
  │ 层 8: 直接压缩 (Direct Compaction)                           │
  │    高 order 分配失败时直接迁移可移动页，腾出连续空间           │
  └─────────────────────────────────────────────────────────────┘
```

---

## 二、迁移类型分组：按移动性隔离 pageblock

### 2.1 MIGRATE_TYPES 枚举

```c
// include/linux/mmzone.h:60-89
enum migratetype {
    MIGRATE_UNMOVABLE,           // 不可移动：内核数据结构、DMA 缓冲区
    MIGRATE_MOVABLE,             // 可移动：用户页、文件缓存
    MIGRATE_RECLAIMABLE,         // 可回收：clean page cache
    MIGRATE_PCPTYPES,            // = 3，PCP 快速路径包含的类型数
    MIGRATE_HIGHATOMIC = MIGRATE_PCPTYPES,  // 原子高阶分配预留
#ifdef CONFIG_CMA
    MIGRATE_CMA,                 // CMA 可迁移连续区域
    __MIGRATE_TYPE_END = MIGRATE_CMA,
#else
    __MIGRATE_TYPE_END = MIGRATE_HIGHATOMIC,
#endif
#ifdef CONFIG_MEMORY_ISOLATION
    MIGRATE_ISOLATE,             // 内存隔离（热插拔、离线页）
#endif
    MIGRATE_TYPES
};
```

只有前三类（UNMOVABLE, MOVABLE, RECLAIMABLE）是 **可合并型**（mergeable），前三类也是 PCP 快速路径管理的类型（`MIGRATE_PCPTYPES = 3`）。

### 2.2 ARM64 pageblock 参数

```c
// arch/arm64/Kconfig:1638-1641
config ARCH_FORCE_MAX_ORDER
    int
    default "13" if ARM64_64K_PAGES
    default "11" if ARM64_16K_PAGES
    default "10"     // ← 4K pages: MAX_PAGE_ORDER=10, NR_PAGE_ORDERS=11

// include/linux/pageblock-flags.h:65
// ARM64 启用 THP: HPAGE_PMD_ORDER = 9 (2MB pages)
#define pageblock_order  MIN_T(unsigned int, HPAGE_PMD_ORDER, PAGE_BLOCK_MAX_ORDER)
// pageblock_order = 9
#define pageblock_nr_pages  512  // 每页块 512 页 = 2MB
```

| 参数 | 值 | 说明 |
|------|:---:|------|
| `MAX_PAGE_ORDER` | 10 | 最大分配阶数，1024 pages (4MB) |
| `NR_PAGE_ORDERS` | 11 | order 0 ~ order 10 |
| `pageblock_order` | 9 | 每个 pageblock 为 2MB |
| `pageblock_nr_pages` | 512 | 每页块 512 页 |

### 2.3 页块初始化与划分

系统启动时通过 `free_area_init_core()` 初始化各个 zone 的 free_area。页块的 migratetype 初始化为 `MIGRATE_MOVABLE`（默认最灵活的类型）。

```c
// mm/page_alloc.c --- free_area_init_core() 内
set_pageblock_migratetype(page, MIGRATE_MOVABLE);
```

**核心约束**：一个 pageblock 内所有空闲页维护在同一条 `free_area[order]->free_list[migratetype]` 链表中，保证：

1. 同一 pageblock 的空闲页**物理相邻**
2. 同一 pageblock 只有一种 migratetype
3. 同类页不会与异类页混合形成碎片

### 2.4 Buddy 跨 pageblock 合并的约束

```c
// mm/page_alloc.c:982 --- __free_one_page()
if (buddy_mt != migration_type) {
    if (!migratetype_is_mergeable(migration_type) ||
        !migratetype_is_mergeable(buddy_mt)) {
        // 不能合并！保留碎片隔离
        break;
    }
    // 可合并型之间可以合并
}
```

**只有三类 mergeable 类型可以跨 pageblock 合并**。CMA、HIGHATOMIC、ISOLATE 不会与其它类型合并。

### 2.5 移动性分组动态开关与小内存场景

当系统总空闲页过少时，内核会**完全禁用**移动性分组，所有页面退化为单一 MIGRATE_MOVABLE 池：

```c
// mm/page_alloc.c:5839 --- build_all_zonelists_init() 或 hotadd 后
if (vm_total_pages < (pageblock_nr_pages * MIGRATE_TYPES))
    page_group_by_mobility_disabled = 1;
else
    page_group_by_mobility_disabled = 0;
```

对于 ARM64 4K 页配置：`pageblock_nr_pages = 512`，`MIGRATE_TYPES` 最多 8 类，即系统空闲页少于 **4096 pages (16MB)** 时，反碎片化第一层（迁移类型分组）关闭。这是小内存嵌入式 ARM64 设备（IoT 网关等）的典型场景。

```c
// mm/page_alloc.c:531,556 --- 多处检查此标志
if (unlikely(page_group_by_mobility_disabled && ...))
    // 绕过迁移类型限制，所有页视为 MIGRATE_MOVABLE
```

同时，`PAGE_ALLOC_COSTLY_ORDER = 3` 定义了分配代价分水岭：

```c
// include/linux/mmzone.h
#define PAGE_ALLOC_COSTLY_ORDER 3
```

| order | 页数 | 大小 | 分配策略 |
|-------|------|------|---------|
| 0-3 | 1-8 | 4KB-32KB | **不容失败**：aggressive reclaim + OOM killer |
| 4-8 | 16-256 | 64KB-1MB | costly order：依赖 compaction，失败不 OOM |
| 9 | 512 | 2MB | pageblock_order = THP 关键大小 |
| 10 | 1024 | 4MB | ARM64 4K pages 的 MAX_PAGE_ORDER |

对于 ARM64 大页配置：

| 配置 | MAX_PAGE_ORDER | THP order | 最大连续块 |
|------|:---:|:---:|------|
| 4K pages (default) | 10 | 9 | 4MB |
| 16K pages | 11 | PMD_SIZE/16K | 32MB |
| 64K pages | 13 | PMD_SIZE/64K | 512MB |

---

## 三、分层 Fallback 策略：Claim vs Steal

### 3.1 回退优先级

当某迁移类型空闲列表耗尽，内核按以下优先级回退：

```
request MIGRATE_A (own type)
  ├── RMQUEUE_NORMAL:    从 A 类型查找 (__rmqueue_smallest)
  ├── RMQUEUE_CMA:       从 CMA 区域分配 (仅 ALLOC_CMA || MOVABLE)
  ├── RMQUEUE_CLAIM:     转化整个 pageblock 为 A 类型 (__rmqueue_claim)
  └── RMQUEUE_STEAL:     从异类 pageblock 偷一页 (__rmqueue_steal) ⚠ 碎片化
```

### 3.2 `__rmqueue_claim()`——转化整个 pageblock

```c
// mm/page_alloc.c:2354-2406
static struct page *__rmqueue_claim(zone, order, start_migratetype, alloc_flags)
{
    // 从 MAX_PAGE_ORDER 向下搜索（先看大块）
    for (current_order = MAX_PAGE_ORDER; current_order >= min_order; --current_order) {
        fallback_mt = find_suitable_fallback(area, current_order, start_migratetype, true);
        if (fallback_mt == -2) break;  // 太小不值得转化整个 pageblock
        page = get_page_from_free_area(area, fallback_mt);
        page = try_to_claim_block(zone, page, current_order, order,
                                   start_migratetype, fallback_mt, alloc_flags);
    }
}
```

`try_to_claim_block()` 首先检查 pageblock 中 **空闲页占比 > 50%**，若满足则转化整个 pageblock 的 migratetype，并将 block 中的剩余空闲页移到新类型的 free_list。页块的类型永久改变。

**优点**：将来该页块内的同类页不会再次产生碎片
**缺点**：需转化整个 2MB 页块（可能有消耗）

### 3.3 `__rmqueue_steal()`——偷一页

```c
// mm/page_alloc.c:2409-2434
static struct page *__rmqueue_steal(zone, order, start_migratetype)
{
    // 从请求的 order 向上搜索
    for (current_order = order; current_order < NR_PAGE_ORDERS; ++current_order) {
        fallback_mt = find_suitable_fallback(area, current_order, start_migratetype, false);
        page = get_page_from_free_area(area, fallback_mt);
        page_del_and_expand(zone, page, order, current_order, fallback_mt);
        // 注意：pageblock 的类型**不变**！
    }
}
```

Steal 直接从异类的 pageblock 中拿一页，**不改变页块类型**。该页块将来可能被任意迁移类型"偷取"，页面碎片逐渐恶化。

### 3.4 Fallback 顺序

```c
// mm/page_alloc.c:1907-1911
static int fallbacks[MIGRATE_PCPTYPES][MIGRATE_PCPTYPES - 1] = {
    [MIGRATE_UNMOVABLE]   = { MIGRATE_RECLAIMABLE, MIGRATE_MOVABLE   },
    [MIGRATE_MOVABLE]     = { MIGRATE_RECLAIMABLE, MIGRATE_UNMOVABLE },
    [MIGRATE_RECLAIMABLE] = { MIGRATE_UNMOVABLE,   MIGRATE_MOVABLE   },
};
```

| 请求类型 | 第一选择 | 第二选择 |
|---------|---------|---------|
| UNMOVABLE | RECLAIMABLE | MOVABLE |
| MOVABLE | RECLAIMABLE | UNMOVABLE |
| RECLAIMABLE | UNMOVABLE | MOVABLE |

---

## 四、ALLOC_NOFRAGMENT 与 defrag_mode

### 4.1 标志的含义

```c
// mm/internal.h:1294
#define ALLOC_NOFRAGMENT   0x100   // 防止混合 pageblock migratetype
```

当分配标志携带 `ALLOC_NOFRAGMENT` 时，内核不会执行 RMQUEUE_STEAL 偷取操作，而允许通过 reclaim/compact 来获取连续内存。

### 4.2 何时启用

```c
// mm/page_alloc.c:3673-3711 --- alloc_flags_nofragment()
if (defrag_mode)
    alloc_flags |= ALLOC_NOFRAGMENT;        // defrag_mode: 总是启用
#ifdef CONFIG_ZONE_DMA32
    if (zone_idx(zone) == ZONE_NORMAL)
        alloc_flags |= ALLOC_NOFRAGMENT;    // ARM64: ZONE_NORMAL 启用
#endif
```

**defrag_mode** 是一个 sysctl（`/proc/sys/vm/defrag_mode`），启后会保护所有的分配都尽量不碎片化。

### 4.3 对分配路径的影响

1. **阻止 STEAL**：`__rmqueue()` 中 RMQUEUE_STEAL 路径被跳过
2. **强制 claim**：在 `__rmqueue_claim()` 中，即使 order 小于 pageblock_order 也会尝试 claim（min_order 强制为 pageblock_order）
3. **zonelist 影响**：遍历到远程节点时会放弃 ALLOC_NOFRAGMENT（位置优先于碎片保护）

### 4.4 NR_FREE_PAGES_BLOCKS —— defrag_mode 下的严格空闲计数

`defrag_mode` 不仅在分配路径施加影响，还改变了 kcompactd 判断"压缩是否完成"的标准：

```c
// mm/compaction.c:2292-2300 --- compact_finished()
/*
 * When defrag_mode is enabled, make kcompactd target
 * watermarks in whole pageblocks. Because they can be stolen
 * without polluting, no further fallback checks are needed.
 */
if (defrag_mode && !cc->direct_compaction) {
    if (__zone_watermark_ok(cc->zone, cc->order,
                high_wmark_pages(cc->zone),
                cc->highest_zoneidx, cc->alloc_flags,
                zone_page_state(cc->zone, NR_FREE_PAGES_BLOCKS)))
        return COMPACT_SUCCESS;
    return COMPACT_CONTINUE;
}
```

`NR_FREE_PAGES_BLOCKS` 只统计**以整个 pageblock 为单位的空闲页面**。例如一个 pageblock 内即使有 511 个空闲页和 1 个占用页，在此计数中也视为 0。这迫使 kcompactd 以极高的标准持续压缩，直到页面以整块 pageblock 的方式空闲。

同时 kcompactd 使用更高的水位线：
```c
// mm/compaction.c:3035,3070
unsigned int alloc_flags = defrag_mode ?
    ALLOC_WMARK_HIGH : ALLOC_WMARK_MIN;
```

---

## 五、HIGHATOMIC 原子高阶保留

### 5.1 机制

内核为原子上下文（tlb、中断等）的高阶分配（order >= pageblock_order）预留约 1% zone 管理页作为 HIGHATOMIC 块。

```c
// mm/page_alloc.c:3361-3400 --- reserve_highatomic_pageblock()
max_managed = ALIGN((zone_managed_pages(zone) / 100), pageblock_nr_pages);
if (zone->nr_reserved_highatomic >= max_managed)
    return;    // 已达上限 ~1%
move_freepages_block(zone, page, mt, MIGRATE_HIGHATOMIC);
```

### 5.2 回收

```c
// mm/page_alloc.c:3413-3470 --- unreserve_highatomic_pageblock()
// 当普通分配失败压力大时，释放 HIGHATOMIC 块回到普通迁移类型
unreserve_highatomic_pageblock(ac, force);
```

---

## 六、CMA：可迁移连续内存

### 6.1 CMA 的两种角色

1. **为 DMA 设备预留**：确保大块连续物理内存满足设备 DMA 要求
2. **为 buddy 提供 MOVABLE 池**：空闲 CTRL 区域的页标记为 MIGRATE_CMA，可分配给 MOVABLE 类型使用

### 6.2 ARM64 CMA 初始化

```c
// arch/arm64/mm/init.c:327
dma_contiguous_reserve(arm64_dma_phys_limit);
```

在 ZONE_DMA/DMA32 内预留 CMA 区域。ARM64 的 ZONE_DMA/DMA32 范围由 arm64_dma_phys_limit 限定。

### 6.3 CMA 在 Buddy 中的交互

```c
// mm/page_alloc.c --- RMQUEUE_CMA 路径
if (alloc_flags & ALLOC_CMA &&
    zone_page_state(zone, NR_FREE_CMA_PAGES) >
    zone_page_state(zone, NR_FREE_PAGES) / 2) {
    page = __rmqueue_cma_fallback(zone, order);
}
// 当 CMA 空闲页 > 50% zone 空闲页时，优先从 CMA 分配以减少 CMA 占用
```

### 6.4 迁移类型保护的层次

| 迁 type | 可从中获取？ | 是否移动？ |
|-----allocateable?|movable?|
| UNMOVABLE | ✗ | ✗ |
| MOVABLE | ✓ | ✓ |
| RECLAIMABLE | ✓ | ✓ |
| CMA | ✓ | ✓ (迁移 MOVABLE 页) |
| HIGHATOMIC | ✗ (仅原子分配) | ✗ |
| ISOLATE | ✗ | ✗ |

---

## 七、水位线提升 Watermark Boosting

### 7.1 设计思想

当 fallback 偷取发生时，说明某个迁移类型有空闲页但其他类型短缺。系统临时提升 zone 的水位线，使得 kswapd 提早启动回收，尝试释放该类型的页面。

### 7.2 实现

```c
// mm/page_alloc.c:2160-2198 --- boost_watermark()
zone->watermark_boost += pageblock_nr_pages;  // 每次 fallback 加 512 页
// 上限: max(pageblock_nr_pages, WMARK_HIGH * 15000 / 10000)
```

kswapd 在每次回收循环后削减 boost：

```c
// mm/vmscan.c --- balance_pgdat()
zone->watermark_boost -= min(zone->watermark_boost, zone_boosts[i]);
wakeup_kcompactd(pgdat, pageblock_order, highest_zoneidx);  // 然后启动压缩
```

> **💡 水位线提升只是"信号"，真正执行内存释放的是页面回收子系统。详见 [第八节：页面回收](#八页面回收-page-reclaim)。**

---

## 八、页面回收 Page Reclaim

### 8.1 一句话理解

> **页面回收的任务是：在内存紧张时，找到"可以丢"或"可以换出"的页面，把它们释放回 buddy，避免系统直接 OOM。**

通俗类比：buddy 是仓库管理员，当货架（free_area）快空了，页面回收就是**盘点小组**，去各个货架上找出"可以清掉"的存货，腾出空间。

### 8.2 回收的主要工作内容：回收哪些页面

页面回收面对三类可回收内存，从易到难排列：

| <span style="color:green">**优先级**</span> | <span style="color:green">**回收对象**</span> | <span style="color:green">**难度**</span> | <span style="color:green">**说明**</span> |
|:---:|---|---|---|
| <span style="color:green">**🥇 最优先**</span> | **clean page cache**（干净文件页） | ⭐ | 直接从 page cache 丢弃，零 I/O，最快 |
| <span style="color:orange">**🥈 次优先**</span> | **dirty page cache**（脏文件页） | ⭐⭐ | 需要先回写磁盘，再由 buddy 回收 |
| <span style="color:red">**🥉 最后手段**</span> | **anonymous pages**（匿名页） | ⭐⭐⭐ | 没有文件后端，必须换出到 swap 设备 |
| <span style="color:blue">**🔧 辅助**</span> | **slab cache**（内核缓存） | ⭐⭐ | 通过 shrinker 接口回收 dentry/inode 等 |

### 8.3 页面回收的四种方式

```text
                    ┌──────────────────────────────────────┐
                    │        页面回收的四种方式               │
                    │                                      │
                    │  ① kswapd 后台回收（异步、温和）        │
                    │     水位 < LOW → 唤醒 per-node 线程    │
                    │     priority 12→1 渐进加压             │
                    │                                      │
                    │  ② Direct Reclaim 直接回收（同步、紧急） │
                    │     alloc_pages() 分配失败 → 分配者亲自回收 │
                    │     本质：free < MIN watermark，走投无路    │
                    │     可能阻塞进程，写回受限               │
                    │                                      │
                    │  ③ Proactive Reclaim 主动回收          │
                    │     用户空间触发（memory.reclaim）       │
                    │     用于 cgroup 级别的内存管理           │
                    │                                      │
                    │  ④ Slab Shrinker 内核缓存回收           │
                    │     shrink_slab() 回收 dentry/inode    │
                    │     与 LRU 回收协同工作                 │
                    └──────────────────────────────────────┘
```

### 8.4 触发条件全景

| <span style="color:red">**触发方式**</span> | <span style="color:red">**触发条件**</span> | <span style="color:red">**代码入口**</span> | <span style="color:red">**特点**</span> |
|---|---|---|---|
| **kswapd 唤醒** | ① `alloc_pages()` 快路径以 `ALLOC_WMARK_LOW` 分配失败（`__zone_watermark_ok(..., WMARK_LOW, ...)` 返回 false）<br>② 或 `watermark_boost` > 0（因 fallback steal 触发）<br>③ `wakeup_kswapd()` 内部再检查：`!pgdat_balanced()` 或 `pgdat_watermark_boosted()`<br>④ 且 `kswapd_failures < 16`、`cpuset_zone_allowed()` | `wake_all_kswapds()` → `wakeup_kswapd()` → `kswapd()` | 异步，不阻塞分配者 |
| **Direct Reclaim** | ① `alloc_pages()` 慢路径以 `ALLOC_WMARK_MIN` 重试仍失败（`free_pages ≤ min_wmark + lowmem_reserve`）<br>② `gfp_mask & __GFP_DIRECT_RECLAIM`（调用者允许阻塞）<br>③ `!(current->flags & PF_MEMALLOC)`（防止递归回收） | `__alloc_pages_slowpath()` → `__alloc_pages_direct_reclaim()` | 同步，阻塞分配者 |
| **Proactive Reclaim** | 用户写入 `echo "10M" > /sys/fs/cgroup/<group>/memory.reclaim`<br>→ `memory_reclaim()` 调用 `user_proactive_reclaim()`<br>→ `memparse()` 解析字节数 → 设置 `sc.proactive = 1` | `memory_reclaim()` → `try_to_free_mem_cgroup_pages()` | 用户态驱动，可指定回收量 |
| **Slab Shrinker** | 随 reclaim 主循环自动调用，无独立触发条件<br>→ `shrink_node()` → `shrink_slab()`<br>→ `do_shrink_slab()`: `nr_to_scan = count_objects() >> priority` | `shrink_node()` → `shrink_slab()` | 回收内核缓存对象 |

**水位线触发示意**：

```text
  free_pages
     ↑
     │  ┌─────────────────────────────  HIGH 水位 ──────────────
     │  │   kswapd 睡眠区间（内存充足）
     │  │
     │  ├─────────────────────────────  LOW 水位 ───────────────
     │  │   ★ kswapd 被唤醒，开始后台回收
     │  │   分配仍可成功（从 free_area 正常获取）
     │  │
     │  ├─────────────────────────────  MIN 水位 ───────────────
     │  │   ★ 触发 Direct Reclaim，分配者自己回收
     │  │   ALLOC_MIN_RESERVE 标志被设置
     │  │
     │  ├─────────────────────────────  0 ─────────────────────
     │  │   内存耗尽 → OOM Killer
     │  └──────────────────────────────────────────────────────
```

### 8.5 回收的总调用框架

所有回收方式最终都汇聚到同一条核心路径：

```text
无论 kswapd / direct reclaim / proactive reclaim
         │
         ▼
    shrink_node()          ← 以 node 为单位回收
         │
         ├──► shrink_node_memcgs()    ← 遍历每个 memory cgroup
         │       │
         │       └──► shrink_lruvec() ← 回收一个 lruvec 内的 LRU 链表
         │               │
         │               ├──► get_scan_count()    ← ★ 决定 anon/file 扫描比例
         │               ├──► shrink_list()       ← 扫描 inactive/active 链表
         │               │       └──► shrink_folio_list()  ← ★ 逐页决策
         │               └──► shrink_slab()       ← 回收 slab 内核缓存
         │
         └──► 后处理：reclaim_throttle（节流）、vmpressure（压力通知）
```

### 8.6 核心控制结构：`struct scan_control`

定义于 [mm/vmscan.c](mm/vmscan.c):75-172。可以理解为回收操作的<span style="color:red">**"遥控器"**</span>，所有回收决策都通过它传递参数：

| <span style="color:blue">**字段**</span> | <span style="color:blue">**含义**</span> | <span style="color:blue">**关键值**</span> |
|---|---|---|
| `nr_to_reclaim` | 本轮回收目标页数 | 通常 `SWAP_CLUSTER_MAX = 32` |
| <span style="color:red">**`priority`**</span> | 回收优先级，值越小越激进 | `12`(温和) → `0`(紧急)；kswapd 范围 12→1，direct reclaim 范围 12→0 |
| `may_unmap` | 是否允许拆除 PTE 映射 | direct reclaim 通常为 1 |
| `may_swap` | 是否允许换出匿名页 | boost reclaim 时为 0 |
| `may_writepage` | 是否允许回写脏文件页 | kswapd=1，direct reclaim 受限 |
| `cache_trim_mode` | 仅回收 page cache | 保护匿名页工作集时启用 |
| `gfp_mask` | GFP 标志 | 影响是否允许 FS/IO 操作 |

> **`priority` 是理解回收激进程度的关键**：每轮扫描 `lruvec_size >> priority` 页。`priority=12` 仅扫描 ~1/4096 的页。kswapd 止于 `priority=1`（扫描 ~1/2），direct reclaim 可达 `priority=0`（**全量扫描 LRU**），比 kswapd 更激进。

### 8.7 方式一：kswapd 后台回收 —— 主循环

kswapd 是 <span style="color:green">**per-node**</span> 的内核线程，代码在 [mm/vmscan.c](mm/vmscan.c):7326-7392。

```c
static int kswapd(void *p) {
    pg_data_t *pgdat = (pg_data_t *)p;
    tsk->flags |= PF_MEMALLOC | PF_KSWAPD;  // 防止递归回收

    for (;;) {
        // ① 睡眠，直到被 wakeup_kswapd() 唤醒
        kswapd_try_to_sleep(pgdat, alloc_order, reclaim_order, highest_zoneidx);

        // ② 读取唤醒请求的 order 和最高 zone
        alloc_order = READ_ONCE(pgdat->kswapd_order);
        highest_zoneidx = kswapd_highest_zoneidx(pgdat, highest_zoneidx);

        // ③ ★ 核心：执行回收循环
        reclaim_order = balance_pgdat(pgdat, alloc_order, highest_zoneidx);

        // ④ 如果回收的 order 仍小于请求 order，尝试以 reclaim_order 级别睡眠
        //    同时唤醒 kcompactd 处理更高阶压缩
        if (reclaim_order < alloc_order)
            goto kswapd_try_sleep;
    }
}
```

**睡眠条件**：`prepare_kswapd_sleep()` 检查所有 zone 都达到 high watermark，或 16 次回收失败后放弃。

> **<span style="color:orange">⚠️ 关键标志</span>**：`PF_MEMALLOC` 保证 kswapd 自己分配内存时 <span style="color:red">**不会递归进入 reclaim**</span>，防止死循环。

### 8.8 方式一：kswapd 的优先级循环算法 —— `balance_pgdat()`

代码在 [mm/vmscan.c](mm/vmscan.c):6997-7290。这是 kswapd <span style="color:red">**最核心的算法**</span>：

```c
static int balance_pgdat(pg_data_t *pgdat, int order, int highest_zoneidx) {
    sc.priority = DEF_PRIORITY;  // 从 12 开始

    do {
        // ① 检查是否已经平衡 → 是则退出
        if (!nr_boost_reclaim && pgdat_balanced(pgdat, sc.order, highest_zoneidx))
            goto out;

        // ② ★ 三阶段策略：控制 may_writepage / may_swap
        sc.may_writepage = !laptop_mode && !nr_boost_reclaim;
        sc.may_swap = !nr_boost_reclaim;

        // ③ 老化：将最近被访问的页从 inactive 提升到 active
        kswapd_age_node(pgdat, &sc);

        // ④ ★ 执行一次节点级 shrink
        kswapd_shrink_node(pgdat, &sc);

        // ⑤ 无进展 → 提高优先级（priority--）
    } while (sc.priority >= 1);  // kswapd 止于 priority=1，direct reclaim 可达 priority=0
}
```

**三阶段渐进加压策略**：

| <span style="color:green">**阶段**</span> | <span style="color:green">**priority**</span> | <span style="color:green">**may_writepage**</span> | <span style="color:green">**may_swap**</span> | <span style="color:green">**含义**</span> |
|---|---|---|---|---|
| <span style="color:green">**boost reclaim**</span> | 12 | ❌ | ❌ | 仅回收 clean cache，<span style="color:blue">零 I/O 开销</span> |
| <span style="color:orange">**正常 reclaim**</span> | 12→3 | ✅ | ✅ | 渐进加压，回收 page cache + 换出匿名页 |
| <span style="color:red">**紧急 reclaim**</span> | 2→1(kswapd) / 2→0(direct) | ✅(强制) | ✅ | 几乎全量扫描 inactive list，<span style="color:red">即将 OOM</span>。direct reclaim 可达 priority=0（全量扫描），比 kswapd 更激进 |

### 8.9 方式二：Direct Reclaim 直接回收 —— `alloc_pages()` 失败后的自救

> **<span style="color:red">一句话</span>**：`alloc_pages()` 实在分配不到内存了（free < MIN watermark），分配者亲自下场回收内存，回收完再试一次。

触发需要经过<span style="color:red">**两轮失败**</span>：

1. **快路径**：以 `ALLOC_WMARK_LOW` 尝试分配 → 失败（free < LOW watermark）
2. **慢路径初步**：`gfp_to_alloc_flags()` 降级为 `ALLOC_WMARK_MIN` 再次尝试 → 仍然失败（free < <span style="color:red">**MIN**</span> watermark）

只有两轮都失败，且分配者携带 `__GFP_DIRECT_RECLAIM` 标志时，才进入直接回收。完整调用链：

```text
__alloc_pages_slowpath()                     [mm/page_alloc.c:4626]
  │   alloc_flags = ALLOC_WMARK_MIN（从 LOW 降级到 MIN）
  ├─ wake_all_kswapds()                     ← ① 先唤醒后台回收（不阻塞）
  ├─ get_page_from_freelist()               ← ② 以 MIN watermark 再次尝试
  │   └─ 仍然失败（free < MIN）→ 进入 direct reclaim
  │
  ├─ __alloc_pages_direct_reclaim()         ← ③ ★ 直接回收（阻塞当前进程）
  │   └─ __perform_reclaim()
  │       └─ try_to_free_pages()
  │           └─ do_try_to_free_pages()     [mm/vmscan.c:6383]
  │               ├─ 优先级循环 (DEF_PRIORITY → 0)
  │               │   └─ shrink_zones() → shrink_node()
  │               └─ 失败 → 可能 retry（memcg_full_walk / force_deactivate）
  │
  ├─ __alloc_pages_direct_compact()         ← ④ 回收不够 → 尝试压缩
  ├─ should_reclaim_retry()                 ← ⑤ 判断是否重试
  └─ __alloc_pages_may_oom()                ← ⑥ 最终手段：OOM killer
```

**<span style="color:red">Direct reclaim 的关键限制</span>**：

| 限制 | 说明 |
|------|------|
| <span style="color:red">**写回受限**</span> | priority > 10 时不允许 `may_writepage`，脏文件页只能标记 reclaim 后放回 |
| <span style="color:red">**可能被节流**</span> | 回收效率 < 12% 时，`reclaim_throttle(NOPROGRESS)` 睡眠 1 秒 |
| <span style="color:orange">**memcg 渐进遍历**</span> | direct reclaim 做 partial walk（公平+低延迟），kswapd 做 full walk |

### 8.10 回收的核心算法：`shrink_folio_list()` 逐页决策

代码在 [mm/vmscan.c](mm/vmscan.c):1099-1673。这是<span style="color:red">**所有回收方式的公共核心**</span>——无论 kswapd 还是 direct reclaim，最终都调用这个函数对每个 folio 做命运判决。

**决策流程图**（从上到下，优先级递减）：

```text
folio 从 LRU 尾部取出（最冷页）
│
├─ ① mlocked（内存锁定）？                → <span style="color:green">放回 active（不可回收）</span>
│
├─ ② 正在 writeback？
│   ├─ kswapd + node 写压力大              → <span style="color:green">放回 active（避免死等）</span>
│   ├─ 正常 reclaim                        → <span style="color:orange">标记 reclaim，放回 active</span>
│   └─ legacy memcg                        → <span style="color:red">等待 writeback，重试</span>
│
├─ ③ 最近被访问过（PTE_AF=1）？            → <span style="color:green">二次机会：放回 active</span>
│   └─ 未被访问                            → <span style="color:red">继续回收流程</span>
│
├─ ④ 可降级到低 tier 内存？                → <span style="color:blue">demote（不丢数据，迁移）</span>
│
├─ ⑤ 匿名页 + 无 swap cache？              → <span style="color:orange">分配 swap slot</span>
│
├─ ⑥ 被页表映射？                          → <span style="color:red">try_to_unmap() 拆除所有 PTE + 延迟批量 TLB flush</span>
│
├─ ⑦ 脏页？
│   ├─ 文件页 + direct reclaim             → <span style="color:orange">标记 reclaim，放回 active</span>
│   └─ kswapd                              → <span style="color:red">pageout() 回写到磁盘</span>
│
├─ ⑧ __remove_mapping()                    → <span style="color:red">从 address_space 摘除</span>
│
└─ ⑨ free_unref_folios()                   → <span style="color:green">✅ 释放回 buddy free_area[]</span>
```

**不同回收结果的代价**：

| <span style="color:green">**回收对象**</span> | <span style="color:green">**操作**</span> | <span style="color:green">**I/O 开销**</span> | <span style="color:green">**后续 fault 代价**</span> |
|---|---|---|---|
| clean page cache | 直接丢 | 无 | minor fault（从文件重读） |
| dirty page cache | 先 pageout 回写 | 写 I/O | minor fault（从文件重读） |
| 匿名页 | swapout 换出 | 写 I/O | <span style="color:red">**major fault**（从 swap 读回）</span> |

### 8.11 扫描比例算法：`get_scan_count()` —— 决定扫什么

代码在 [mm/vmscan.c](mm/vmscan.c):2556-2670。回收 <span style="color:red">**不是盲目全扫**</span>，而是根据系统状态动态决定扫多少 anon 页、多少 file 页。

**四种扫描模式**：

| <span style="color:blue">**模式**</span> | <span style="color:blue">**触发条件**</span> | <span style="color:blue">**行为**</span> |
|---|---|---|
| `SCAN_FILE` | 无 swap / swappiness=0 / cache_trim_mode | <span style="color:green">只回收 file LRU</span> |
| `SCAN_ANON` | swappiness=200 / file 页极少 | <span style="color:red">只回收 anon LRU</span> |
| `SCAN_EQUAL` | priority=0（即将 OOM） | 按大小等比扫描 |
| `SCAN_FRACT` | **默认** | 按 swappiness + 历史效率 <span style="color:orange">自适应加权</span> |

**`SCAN_FRACT` 核心公式**（<span style="color:orange">自适应反馈算法</span>）：

```
fraction[ANON] = anon_cost × (swappiness + 1) / (total_cost + 1)
fraction[FILE] = file_cost

实际扫描量 = (lruvec_size >> priority) × fraction[type] / denominator
```

> **<span style="color:orange">💡 算法直觉</span>**：如果匿名页回收效率低（扫了很多但回收很少），`anon_cost` 自动增大，后续减少匿名页扫描比例，把 CPU 用在更容易回收的 file 页上。这是一种**带反馈的自适应控制**。

### 8.12 Slab Shrinker 算法

内核自身缓存也通过 shrinker 接口参与回收。代码在 [mm/shrinker.c](mm/shrinker.c):614-670。

<span style="color:red">**核心公式**</span>：`nr_to_scan = total_objects >> priority`

| <span style="color:blue">**Shrinker**</span> | <span style="color:blue">**回收对象**</span> | <span style="color:blue">**对 buddy 的影响**</span> |
|---|---|---|
| `super_block->s_shrink` | dentry / inode cache | 回收的 slab 页 → `free_pages()` → buddy |
| `zswap_shrinker` | zswap 压缩池 | 释放压缩内存 → buddy |
| `deferred_split_shrinker` | THP 分裂队列 | 分裂 THP → 小页回 buddy |

### 8.13 回收与反碎片化的联动闭环

将水位线 → 回收 → 压缩串成完整闭环：

```text
<span style="color:red">fallback steal 发生（迁移类型短缺）</span>
  → <span style="color:orange">boost_watermark() 提升水位线</span>
    → <span style="color:red">kswapd 被唤醒</span>（水位 < LOW+boost）
      → <span style="color:orange">balance_pgdat() 优先级循环 12→1</span>
        → <span style="color:red">shrink_node() 回收 LRU + shrink_slab()</span>
          → <span style="color:green">free_unref_folios() 归还 buddy</span>
            → <span style="color:blue">wakeup_kcompactd() 启动后台压缩</span>
              → <span style="color:green">compact_zone() 拼出连续大块</span>
                → <span style="color:green">✅ buddy 恢复健康</span>
```

> **<span style="color:red">🔑 关键洞察</span>**：反碎片化体系中，**水位线提升是触发器，页面回收是执行器，内存压缩是整理器**。三者缺一不可。

### 8.14 ARM64 架构特化

页面回收主要是通用代码，ARM64 在以下路径提供 <span style="color:green">**硬件加速与架构特化**</span>：

| <span style="color:blue">**特化点**</span> | <span style="color:blue">**代码位置**</span> | <span style="color:blue">**作用**</span> |
|---|---|---|
| **TLB 延迟批量刷新** | [arch/arm64/include/asm/tlbflush.h](arch/arm64/include/asm/tlbflush.h) | `try_to_unmap()` 拆映射时不逐页 flush，而是通过 `arch_tlbbatch` 累积 TLBI 操作（不含 DSB），最终一次 `dsb(ish)` 完成所有失效。<span style="color:green">避免逐页 TLBI+DSB 的高开销</span> |
| **硬件 Access Flag（PTE 级）** | PTE_AF 位 | 回收扫描中直接读硬件 AF 判断冷热，<span style="color:green">零软件开销</span>。`arch_has_hw_pte_young = cpu_has_hw_af` |
| **HAFT PMD 级 Access Flag** | ARMv8.9/v9.5 HAFT 特性 | `arch_has_hw_nonleaf_pmd_young = system_supports_haft`，回收 LRU gen 扫描可在 PMD 级别判断访问，AF 未设置的 PMD 直接跳过。<span style="color:red">对大地址空间回收效率提升显著</span> |
| **arch_wants_old_prefaulted_pte** | [arch/arm64/include/asm/pgtable.h:1626](arch/arm64/include/asm/pgtable.h) | 硬件 AF 可用时，新 fault-in PTE 初始 AF=0（"old"），依赖硬件首次访问时设置 AF。<span style="color:green">避免 prefault 干扰 LRU 冷热判断</span> |
| **Contiguous PTE 回收 aging** | [arch/arm64/mm/contpte.c:491-535](arch/arm64/mm/contpte.c) | 回收 aging 操作（`ptep_test_and_clear_young`/`clear_young_dirty_ptes`）<span style="color:green">**无需展开**</span> contpte block，直接对整个 CONT_PTES 范围清除 young/dirty 位。注释原文："we can get away with clearing young for the whole contig range here, so we avoid having to unfold." 只有部分修改（如 wrprotect 子集）才需 `contpte_try_unfold_partial()` |
| **Lazy MMU Mode** | [arch/arm64/include/asm/pgtable.h:83-120](arch/arm64/include/asm/pgtable.h) | `__HAVE_ARCH_ENTER_LAZY_MMU_MODE`，回收内循环（拆映射/写保护 PTE）进入 lazy MMU mode，设置 `TIF_LAZY_MMU` 延迟 `emit_pte_barriers()`。<span style="color:green">批量 PTE 修改时累积 barrier，减少指令开销</span>。vmscan.c 中 6 处调用 |
| **MTE 标签 swap 保存/恢复** | [arch/arm64/mm/mteswap.c](arch/arm64/mm/mteswap.c) | MTE 开启后每页有 128B allocation tags。swap-out 前通过 `arch_prepare_to_swap()` 保存到 xarray，swap-in 时通过 `arch_swap_restore()` 恢复。<span style="color:red">⚠️ `arch_prepare_to_swap()` 可能返回 -ENOMEM（标签存储分配失败），导致 swap-out 中止</span> |
| **MTE 标签迁移** | [arch/arm64/mm/copypage.c:17](arch/arm64/mm/copypage.c) | `copy_highpage()` 在页面迁移时必须将源页 MTE 标签复制到目标页，否则迁移后标签丢失触发 tag check fault |
| **THP swap 整页换出** | [arch/arm64/Kconfig:123](arch/arm64/Kconfig)（4K 页配置） | `ARCH_WANTS_THP_SWAP` 允许 PMD 级 swap entry，<span style="color:green">THP swap-out 无需先分裂</span>。16K/64K 页配置不支持此特性 |

#### 8.14.1 MTE 标签保存对回收的深度影响

MTE（Memory Tagging Extension）是 ARM64 独有的内存安全特性，对页面回收有**额外成本和风险**：

```c
// arch/arm64/mm/mteswap.c:94
int arch_prepare_to_swap(struct folio *folio) {
    for (i = 0; i < nr; i++) {
        err = mte_save_tags(folio_page(folio, i));  // 每页: kmalloc(128B) + xa_store
        if (err) goto out;  // 失败 → 回滚已保存的标签
    }
    return 0;
out:
    while (i--) __mte_invalidate_tags(folio_page(folio, i));  // 清理
    return err;  // -ENOMEM → swap-out 中止
}
```

| <span style="color:blue">**回收阶段**</span> | <span style="color:blue">**MTE 影响**</span> | <span style="color:blue">**后果**</span> |
|---|---|---|
| swap-out | 每页额外 128B `kmalloc` + `xa_store` | 内存紧张时 `arch_prepare_to_swap()` 失败 → 匿名页无法换出 |
| swap-in | `arch_swap_restore()` 逐页恢复标签 | 增加 swap-in 延迟 |
| swap slot 释放 | `mte_invalidate_tags()` 清理 xarray | 释放 128B 标签存储 |
| 页面迁移 | `copy_highpage()` 复制 MTE 标签 | 迁移成本增加 |

**监控**：
```bash
cat /proc/vmstat | grep mte  # MTE 相关回收统计
```

#### 8.14.2 HAFT PMD 级 AF 对回收扫描效率的影响

传统回收扫描需要逐页检查 PTE 的 AF 位来判断冷热。ARMv8.9/v9.5 的 HAFT（Hardware Access Flag for Table descriptors）将 AF 扩展到 PMD 表项，允许 **PMD 级别粗粒度扫描**：

```c
// arch/arm64/include/asm/pgtable.h:1618-1619
#define arch_has_hw_nonleaf_pmd_young  system_supports_haft

// mm/vmscan.c LRU gen 扫描路径：
// if (arch_has_hw_nonleaf_pmd_young && !pmd_young(pmd))
//     → 跳过整个 PMD 范围（512 页 / 2MB），无需逐页扫描 PTE
```

**扫描效率对比**：

| <span style="color:blue">**场景**</span> | <span style="color:blue">**无 HAFT**</span> | <span style="color:blue">**有 HAFT**</span> |
|---|---|---|
| 256GB 地址空间，10% 活跃 | 逐页扫描 ~6.7M PTE | 仅扫描 ~0.67M PMD，跳过 90% 的冷 PMD |
| PostgreSQL 大共享内存 | 全量扫描 PTE 表 | PMD 级快速筛选，<span style="color:green">扫描开销降低 ~100 倍</span> |

#### 8.14.3 TLB 延迟批量刷新机制详解

回收拆映射（`try_to_unmap()`）不是逐页执行 TLBI+DSB，而是通过 `arch_tlbbatch` 机制累积操作：

```c
// 回收路径中的批量 TLB 处理：
// 1. 判断是否延迟：arch_tlbbatch_should_defer(mm) → true（除非 REPEAT_TLBI errata）
// 2. 累积 TLBI：arch_tlbbatch_add_pending(batch, mm, start, end)
//    → __flush_tlb_range_nosync()  // 仅 TLBI，不含 DSB
// 3. 最终刷新：arch_tlbbatch_flush(batch)
//    → dsb(ish)  // 一条 DSB 完成所有累积的 TLBI
```

**对比**：

| <span style="color:blue">**方式**</span> | <span style="color:blue">**回收 1000 页的开销**</span> | <span style="color:blue">**适用场景**</span> |
|---|---|---|
| 逐页 TLBI+DSB | 1000 × (TLBI + DSB) ≈ <span style="color:red">数千周期</span> | 无批量机制时 |
| 批量 TLBI + 1 DSB | 1000 × TLBI + 1 × DSB ≈ <span style="color:green">数百周期</span> | arm64 回收路径（默认） |

同时，回收内循环进入 **Lazy MMU Mode**（`arch_enter_lazy_mmu_mode()`），延迟 PTE barrier 操作，与 TLB 批量刷新配合进一步减少开销。

---

## 九、内存压缩 Compaction

### 9.1 两种触发方式

| 类型 | 触发条件 | 流程 |
|------|---------|------|
| **直接压缩** | `__alloc_pages_slowpath()` 中高 order 分配失败 | `__alloc_pages_direct_compact()` → `try_to_compact_pages()` → `compact_zone()` |
| **后台压缩** | kswapd 水位提升后或碎片分数触及阈值 | kcompactd → `kcompactd_do_work()` → `compact_zone()` |
| **Proactive** | `sysctl_compaction_proactiveness > 0` | kcompactd 主动检测碎片并压缩 |

### 9.2 compact_zone 流程

```
compact_zone()
  ├── isolate_migratepages()    扫描并隔离可迁移页
  ├── migrate_pages()           迁移页到新位置
  ├── isolate_freepages()       扫描连续空闲页
  └── compact_finished()        检查是否完成
       ├── COMPACT_SUCCESS     达到所需连续空闲块
       ├── COMPACT_PARTIAL     部分成功
       ├── COMPACT_CONTENDED   锁冲突
       └── COMPACT_SKIPPED    无空闲或无法压缩
```

### 9.3 ARM64 defrag_mode 下的特殊行为

当 `defrag_mode=1`，kcompactd 使用 `NR_FREE_PAGES_BLOCKS` 计数（而非普通空闲页计数），目标水位变高 `ALLOC_WMARK_HIGH`，更积极地进行碎片预防性压缩。

### 9.4 fragmentation_index —— 碎片化程度的量化

`fragmentation_index()` 在 `mm/compaction.c:2414` 中调用，决定是否值得为 costly order（order > 3）执行压缩：

```c
// mm/compaction.c:2414-2418
if (order > PAGE_ALLOC_COSTLY_ORDER) {
    int fragindex = fragmentation_index(zone, order);
    if (fragindex >= 0 && fragindex <= sysctl_extfrag_threshold) {
        suitable = false;  // 碎片化不严重，跳过压缩
        compact_result = COMPACT_NOT_SUITABLE_ZONE;
    }
}
```

**计算公式**（简化）：

$$\text{fragindex} \approx 1000 - \frac{1000 + 1000 \times \frac{\text{free\_pages}}{\text{requested}}}{2^{order}}$$

**解读**：
- **fragindex → 1000**：内存充足但碎片化严重 → **应该压缩**
- **fragindex → 0**：系统整体内存不足 → 压缩无意义，应 reclaim
- **fragindex < sysctl_extfrag_threshold**（默认 500）：跳过压缩

**sysctl 接口**：
```bash
cat /proc/sys/vm/extfrag_threshold  # 默认 500
# 阈值越低越激进（更容易触发 compaction）
echo 125 > /proc/sys/vm/extfrag_threshold
```

---

## 十、ARM64 架构特殊配置

### 10.1 MAX_PAGE_ORDER

```c
// arch/arm64/Kconfig:1638
default "10"  // 4K pages: order 10 = 1024 pages = 4MB
```

| 配置 | MAX_PAGE_ORDER | 最大连续块 |
|------|:---:|------|
| 4K pages (default) | 10 | 1024 page = 4MB |
| 16K pages | 11 | 2048 pages = 32MB |
| 64K pages | 13 | 8192 pages = 512MB |

### 10.2 zone 配置

ARM64 支持 ZONE_DMA 和 ZONE_DMA32 分区。`arm64_dma_phys_limit` 限制 DMA 区域的上限：

```c
// arch/arm64/mm/init.c:67
phys_addr_t arm64_dma_phys_limit;
```

- ZONE_DMA：覆盖 32 位可地址范围，取决于平台（Raspberry Pi 4 限制为 30-bit）
- ZONE_DMA32：覆盖剩余的 32 位可地址范围
- ZONE_NORMAL：剩餘内存 ≥ 32 位地址

### 10.3 CMA 初始化流程

```c
// arch/arm64/mm/init.c:185-327
void __init arm64_memblock_init(void);  // 预留内核/initrd
void __init bootmem_init(void)
  ├── early_memtest();
  ├── arch_numa_init();           // NUMA 拓扑初始化
  ├── arm64_hugetlb_cma_reserve(); // HugeTLB 预留 CMA
  ├── sparse_init();              // Sparse Memory Model
  ├── zone_sizes_init();          // zone 大小初始化
  ├── dma_contiguous_reserve(arm64_dma_phys_limit); // CMA 预留
  └── arch_reserve_crashkernel(); // 崩溃内核预留
```

ARM64 在 bootmem_init() 中初始化 NUMA 拓扑后，预留 HugeTLB CMA，随后执行 `dma_contiguous_reserve()` 为 DMA 设备保留 CMA 区域。

### 10.4 内存模型

ARM64 使用 **Sparse Memory with VMEMMAP**（`CONFIG_SPARSEMEM_VMEMMAP=y`），通过 vmemmap 将 `struct page[]` 数组映射到虚拟地址，实现常数时间 PFN→page 转换。

### 10.5 PCP (Per-CPU Pages) 缓存与"伪碎片化"

#### 10.5.1 PCP 数据结构：按 migratetype × order 组织的链表数组

PCP 不是一个简单指针，而是一个完整的 per-CPU 结构体，内部维护了**多个链表**，按迁移类型和 order 二维分桶：

```c
// include/linux/mmzone.h:744
struct per_cpu_pages {
    spinlock_t lock;          /* 保护 lists 字段 */
    int count;                /* PCP 中缓存的页面总数（以 page 个数计） */
    int high;                 /* 高水位，超过则批量刷回 buddy */
    int high_min;             /* 高水位下限 */
    int high_max;             /* 高水位上限 */
    int batch;                /* 每次与 buddy 交换的批量大小 */
    u8 flags;                 /* PCPF_PREV_FREE_HIGH_ORDER 等标志 */
    u8 alloc_factor;          /* 分配时 batch 缩放因子 */
    short free_count;         /* 连续释放计数 */

    /* ★ 核心：链表数组，按 (migratetype × order) 索引 */
    struct list_head lists[NR_PCP_LISTS];
} ____cacheline_aligned_in_smp;
```

**`lists[]` 索引计算**（`order_to_pindex`，位于 `mm/page_alloc.c:657`）：

```c
static inline unsigned int order_to_pindex(int migratetype, int order)
{
    // 小 order (0~3): pindex = MIGRATE_PCPTYPES * order + migratetype
    return (MIGRATE_PCPTYPES * order) + migratetype;
}
```

其中 `MIGRATE_PCPTYPES = 3`（只缓存 UNMOVABLE、MOVABLE、RECLAIMABLE 三种迁移类型），
`PAGE_ALLOC_COSTLY_ORDER = 3`，所以 `NR_LOWORDER_PCP_LISTS = 3 × 4 = 12`。

`NR_PCP_LISTS` 在开启 THP 时额外 +2（THP movable/unmovable 各一个链表）。

**PCP 链表数组布局**（12 个链表 = 4 个 order × 3 种迁移类型）：

```
                 MIGRATE_UNMOVABLE(0)  MIGRATE_MOVABLE(1)  MIGRATE_RECLAIMABLE(2)
order 0 (1页):    lists[0]              lists[1]             lists[2]
order 1 (2页):    lists[3]              lists[4]             lists[5]
order 2 (4页):    lists[6]              lists[7]             lists[8]
order 3 (8页):    lists[9]              lists[10]            lists[11]
```

**页面如何挂入链表**：每个 `struct page` 内部有一个 union，复用了 `lru`/`buddy_list`/`pcp_list`：

```c
// include/linux/mm_types.h:99
union {
    struct list_head lru;           // 在 LRU 链表中时使用
    struct list_head buddy_list;    // 在 buddy free_area 中时使用
    struct list_head pcp_list;      // ★ 在 PCP 链表中时使用
    struct llist_node pcp_llist;    // RT 内核无锁路径使用
};
```

页面在 buddy 和 PCP 之间迁移时，**零额外内存分配**——只是把同一个 `list_head` 从一个链表摘下来挂到另一个链表。

#### 10.5.2 PCP 链表内存布局全景图

```
Zone (struct zone)
 │
 ├── per_cpu_pageset ──────────────────→  per-CPU 数组
 │                                         │
 │                              ┌──────────┴──────────┐
 │                              │  CPU 0                │  CPU 1  ...
 │                              │  struct per_cpu_pages
 │                              │  ┌──────────────────┐
 │                              │  │ lock              │
 │                              │  │ count = 5         │  ← 当前缓存 5 个 page
 │                              │  │ high  = 186       │  ← 高水位
 │                              │  │ high_min = 156    │
 │                              │  │ high_max = 372    │
 │                              │  │ batch = 31        │  ← 批量操作粒度
 │                              │  │                   │
 │                              │  │ lists[0] ────────→ pageA (order0, UNMOVABLE)
 │                              │  │ lists[1] ────────→ pageC → pageB (order0, MOVABLE)
 │                              │  │ lists[2] ────────→ (空)
 │                              │  │ lists[3] ────────→ pageD → pageE (order1, UNMOVABLE)
 │                              │  │ lists[4] ────────→ (空)
 │                              │  │    ...            │
 │                              │  │ lists[11]────────→ (空)
 │                              │  └──────────────────┘
 │                              │
 │                              │  pcp_list 是 page 结构体内的 list_head
 │                              │  复用 lru/buddy_list 的 union 空间，零额外开销
 │                              │
 ├── free_area[0] ────────────────────→ buddy order 0 空闲链表
 ├── free_area[1] ────────────────────→ buddy order 1 空闲链表
 ├── free_area[2] ────────────────────→ buddy order 2 空闲链表
 └── ...
```

#### 10.5.3 完整释放路径：从 `__free_pages` 到挂入 PCP 链表

以释放一个 **order=0 的 MIGRATE_MOVABLE 页面** 为例，追踪完整路径：

```
用户调用 __free_pages(page, 0)
  → ___free_pages(page, 0, FPI_NONE)
    → put_page_testzero(page) 为真后
      → __free_frozen_pages(page, 0, FPI_NONE)
```

**第1步：`__free_frozen_pages()` — 判断是否走 PCP 快速路径**

```c
// mm/page_alloc.c:2890
static void __free_frozen_pages(struct page *page, unsigned int order,
                                fpi_t fpi_flags)
{
    // order=0 <= PAGE_ALLOC_COSTLY_ORDER(3) → true，走 PCP 路径
    if (!pcp_allowed_order(order)) {
        __free_pages_ok(page, order, fpi_flags);  // 大order直接回 buddy
        return;
    }
    // 准备释放（清理 flags、kasan poison 等）
    if (!free_pages_prepare(page, order))
        return;

    zone = page_zone(page);
    migratetype = get_pfnblock_migratetype(page, pfn);
    // MIGRATE_MOVABLE(1) < MIGRATE_PCPTYPES(3)，正常处理

    // ★ 获取本 CPU 的 PCP 结构体（spin_trylock 无锁争用则成功）
    pcp = pcp_spin_trylock(zone->per_cpu_pageset);
    if (pcp) {
        // ★ 核心：把页面提交到 PCP
        free_frozen_page_commit(zone, pcp, page, migratetype, order, fpi_flags);
        pcp_spin_unlock(pcp);
    } else {
        // 拿不到 PCP 锁（IRQ 重入/并发 drain），直接放回 buddy
        free_one_page(zone, page, pfn, order, fpi_flags);
    }
}
```

**第2步：`free_frozen_page_commit()` — 挂入链表 + 水位检查**

```c
// mm/page_alloc.c:2815
static void free_frozen_page_commit(struct zone *zone,
        struct per_cpu_pages *pcp, struct page *page, int migratetype,
        unsigned int order, fpi_t fpi_flags)
{
    int high, batch, pindex;

    pcp->alloc_factor >>= 1;           // 释放时降低分配因子
    __count_vm_events(PGFREE, 1 << order);

    // ★ 计算索引: order_to_pindex(MIGRATE_MOVABLE=1, order=0)
    //           = 3*0 + 1 = 1  → 挂入 lists[1]
    pindex = order_to_pindex(migratetype, order);

    // ★★★ 关键：将 page->pcp_list 插入 pcp->lists[pindex] 链表头部
    list_add(&page->pcp_list, &pcp->lists[pindex]);

    pcp->count += 1 << order;          // order=0 → count += 1

    // ... 计算 high 水位 ...

    high = nr_pcp_high(pcp, zone, batch, free_high);
    if (pcp->count < high)
        return;                        // 没超 high，页面留在 PCP

    // 超出 high 水位，批量刷回 buddy
    free_pcppages_bulk(zone, nr_pcp_free(pcp, batch, high, free_high),
                       pcp, pindex);
}
```

**第3步：`free_pcppages_bulk()` — 超水位时批量归还 buddy**

```c
// mm/page_alloc.c:1442
static void free_pcppages_bulk(struct zone *zone, int count,
                                struct per_cpu_pages *pcp, int pindex)
{
    spin_lock_irqsave(&zone->lock, flags);

    while (count > 0) {
        // 从各个链表轮询摘取页面
        order = pindex_to_order(pindex);
        do {
            page = list_last_entry(list, struct page, pcp_list);
            list_del(&page->pcp_list);         // 从 PCP 链表摘下
            count -= (1 << order);
            pcp->count -= (1 << order);

            __free_one_page(page, pfn, zone, order, mt, FPI_NONE);
            // ↑ 归还给 buddy 分配器的 free_area[]
        } while (count > 0 && !list_empty(list));
    }

    spin_unlock_irqrestore(&zone->lock, flags);
}
```

#### 10.5.4 释放过程图解：链表变化

**释放前**：`pcp->lists[1]`（order=0, MIGRATE_MOVABLE）已有 2 个页面：

```
pcp->lists[1] (链表头)
     │
     ▼
  ┌──────────┐    ┌──────────┐
  │  page A  │───→│  page B  │───→ 回到 lists[1]
  │ pcp_list │←───│ pcp_list │
  └──────────┘    └──────────┘
```

**释放 page C 后**，`list_add` 将 page C 插入头部：

```
pcp->lists[1] (链表头)
     │
     ▼
  ┌──────────┐    ┌──────────┐    ┌──────────┐
  │  page C  │───→│  page A  │───→│  page B  │───→ 回到 lists[1]
  │ pcp_list │←───│ pcp_list │←───│ pcp_list │
  └──────────┘    └──────────┘    └──────────┘
         ↑
    list_add 插入头部，后进先出（LIFO），有利于 cache 热度
```

**分配时逆过程**：从链表尾部 `list_last_entry` 摘取页面，同样 LIFO。

#### 10.5.5 高水位触发与伪碎片化

Per-CPU Pages 缓存可能导致 buddy 视角的"伪碎片化"：大量 order=0 页面滞留在每个 CPU 的 PCP 缓存中，从 `/proc/buddyinfo` 看似乎 order=0 严重不足，实则被缓存了。

```c
// mm/page_alloc.c --- zone_batchsize()
// batch ≈ min(zone_managed >> 10, 1MB / 4KB) / 4
// 4GB zone: batch ≈ 256 pages per CPU
// 64核: 最大缓存 = 64 × 256 × 4KB ≈ 64MB
```

**对高阶分配的间接影响**：`rmqueue_bulk()` 为 PCP 补充 order=0 页面时，如果 order=0 free_list 空了，会从更高阶拆分。大批量 PCP 补充可能频繁拆散高阶块，不利于大页分配。

```bash
# 降低 PCP high watermark，让页面更快回归 buddy
echo 8 > /proc/sys/vm/percpu_pagelist_high_fraction
```

#### 10.5.6 PCP 与 SLUB 的关系：为什么分配页面不直接从 SLUB 拿？

这是一个常见的疑问：PCP 缓存 order<4 的小页面，SLUB 也管理小对象，为什么分配页面时不从 SLUB 获取？

**核心原因：架构层次不同，SLUB 是 buddy+PCP 的上层消费者。**

```
应用层: kmalloc / kmem_cache_alloc
         │
    ┌────▼────┐
    │  SLUB   │  管理页内的固定大小对象（32B~8KB）
    │ 分配器   │  将 buddy 分配的整页切分为小对象
    └────┬────┘
         │ alloc_pages()  ← SLUB 本身从这里拿页！
    ┌────▼────┐
    │ Buddy   │  管理物理页（4KB 为最小单位）
    │ + PCP   │  按 order(0~10) 分配连续物理页
    └─────────┘
```

| 维度 | Buddy + PCP | SLUB |
|------|-------------|------|
| 分配粒度 | 物理页（4KB/8KB/16KB...） | 字节级对象（32B/64B/256B...） |
| 管理单位 | `struct page` | `struct slab`（含多个对象） |
| 连续性要求 | 物理连续页（order N = 2^N 页） | 不要求——对象在页内 |
| 获取内存方式 | 从 free_area[] 或 PCP lists[] | 调用 `alloc_pages()` 向 buddy 申请 |
| 归还内存方式 | `__free_pages()` → buddy 或 PCP | 整个 slab 空后才 `__free_pages()` |

1. **SLUB 是 buddy 的消费者**：SLUB 通过 `allocate_slab()` → `alloc_slab_page()` → `alloc_frozen_pages()` 从 buddy 申请整页，然后切分成固定大小对象。
2. **粒度不匹配**：当你要一个 order-2（16KB，4个连续页），SLUB 无法提供——它持有的页已被切分为分散的小对象，无法拼出连续物理页。
3. **归属问题**：SLUB 的页属于特定 `kmem_cache`，页内对象在 freelist 中流转。无法从 SLUB "抢"一个页来满足 buddy 的分配请求。
4. **整页归还**：SLUB 只有在整个 slab 的所有对象都释放后，才通过 `__free_pages()` 把整页归还给 buddy+PCP。

**一句话总结**：SLUB 解决的是"页内小对象管理"问题，buddy+PCP 解决的是"物理页分配"问题，二者是**上下层分工**，不是二选一的关系。

### 10.6 ARM64 contpte——连续 PTE 对 buddy 的隐式依赖

ARM64 独有的 `arch/arm64/mm/contpte.c` 支持将 16 个连续 PTE 合并为一个 TLB 条目（4K 页 + 64K TLB 粒度时）：

```c
// arch/arm64/mm/contpte.c --- 将 CONT_PTES 个连续 PTE 折叠
#define CONT_PTES  16   // 16 × 4KB = 64KB → 1 TLB entry
```

这意味着：
1. 用户空间 `mmap` 需要 order=4（16 页 = 64KB）的连续物理页才能获得 TLB 折叠优化
2. buddy 碎片化直接损害 contpte 效率 —— 即使虚拟地址连续，物理页不连续也无法折叠
3. 对于大量使用 `mmap` 的 ARM64 服务器（如数据库），order=4 的连续分配能力成为性能关键

ARM64 的 contpte 在缺页异常路径中尝试折叠/展开：
```
handle_pte_fault()
  └── contpte_try_fold()     ← 尝试将 16 个相邻 PTE 折叠为 1 个 contpte
  └── contpte_try_unfold()   ← 修改部分页面时展开 contpte
```

### 10.7 ARM64 NUMA 下的碎片化不对称

ARM64 服务器通常为多 NUMA 节点架构。分配请求优先从本地节点获取内存，若本地节点碎片化严重，zonelist 遍历到远程节点时会清除 `ALLOC_NOFRAGMENT`：

```c
// mm/page_alloc.c:3698-3701 --- alloc_flags_nofragment()
// 遍历到下一个 zone（可能是远程节点）时：
// ALLOC_NOFRAGMENT 被清除，位置优先级高于碎片保护
```

这意味着远程节点的 pageblock 完整性可能被跨节点 Steal 破坏。长期运行后，不同 NUMA 节点的碎片化程度会严重不对称 —— 某些节点成为"碎片垃圾场"，某些节点保持整洁。

---

## 十一、反碎片化体系总图

```
                     ┌───────────────┐
                     │  alloc_pages() │
                     └───────┬───────┘
                             │
                     ┌───────▼──────────┐
                     │ get_page_from_   │
                     │ freelist()       │
                     └───────┬──────────┘
                             │
                    ┌────────┴────────┐
                    ▼                 ▼
             水位充足             水位不足
                    │                 │
                    ▼                 ▼
              ┌──────────┐      ┌──────────────┐
              │ rmqueue()│      │ slowpath:     │
              └────┬─────┘      │ reclaim/      │
                   │            │ compact/OOM   │
        ┌──────────┼──────────┐ └──────────────┘
        ▼          ▼          ▼
   PCP (order   buddy      slowpath
    ≤ 3)        (order      entries
                ≥ 4)
        │          │
        │    ┌─────▼──────────────┐
        │    │ __rmqueue()        │
        │    │  RMQUEUE_NORMAL    │ ← 首选同类
        │    │  RMQUEUE_CMA       │ ← CMA fallback
        │    │  RMQUEUE_CLAIM     │ ← 整块转化
        │    │  RMQUEUE_STEAL     │ ← 偷单页 ⚠
        │    └────────────────────┘
        │          │
        └──────────┼──────────────┐
                   │              │
         ┌─────────▼──────┐  ┌───▼───────────┐
         │ anti-fragment  │  │ compaction     │
         │  layers:       │  │ (direct/       │
         │  · pageblock   │  │  background)   │
         │  · claim/steal │  │                │
         │  · HIGHATOMIC  │  │ 迁移可移动页   │
         │  · CMA         │  │ 腾出连续空间   │
         │  · watermark   │  └────────────────┘
         │  · boosting    │
         └────────────────┘
```

### 总结

| 层 | 技术 | 机制 | 防护的碎片类型 |
|----|------|------|--------------|
| 1 | Pageblock Grouping | 页块按迁移类型分组 | 外部碎片（同类页分散） |
| 2 | Fallback Claim | 转化整个 pageblock | 外部碎片（迁移类型杂糅） |
| 3 | ALLOC_NOFRAGMENT | 拒绝 steal，重试 zonelist | 外部碎片（偷页破坏 pageblock 完整性） |
| 4 | HIGHATOMIC | 原子高阶保留 | 原子分配低阶碎片 |
| 5 | CMA | 永久可迁移连续区域 | DMA 连续页不足 |
| 6 | Watermark Boost | 提高水位线，提前触发回收 | 回退后的长期不平衡 |
| <span style="color:red">**★**</span> | <span style="color:red">**Page Reclaim（页面回收）**</span> | <span style="color:red">kswapd(12→1) / direct reclaim(12→0) 渐进回收 LRU + shrink_slab，归还 buddy；ARM64 特化：延迟批量 TLB flush、硬件 AF/HAPT、MTE 标签 swap 保存、contpte aging 无展开</span> | <span style="color:red">内存不足时的执行器，回收 page cache / 匿名页 / slab 缓存</span> |
| 7 | kcompactd | 后台压缩 pageblock | pageblock 迁移类型杂糅后的收敛 |
| 8 | Direct Compaction | 按需迁移可移动页 | 分配失败时的紧急连续空间释放 |

---

## 十二、ARM64 典型场景举例

### 场景 1：ARM64 服务器 THP 分配失败

**背景**：64 核 ARM64 服务器（Neoverse N2），运行 PostgreSQL 数据库，启用 `TRANSPARENT_HUGEPAGE_ALWAYS`。

**症状**：
```bash
cat /proc/buddyinfo
# Node 0, zone Normal  0  0  0  0  0  0  0  3  1  0  0
#                                         ↑ order=9 仅剩 1 块
cat /proc/vmstat | grep thp_fault_fallback
# thp_fault_fallback 持续增长
```

**根因分析**：

```
pageblock 0: [U][U][_][U][_][U][U][_]...[U]  ← UNMOVABLE 内核结构，空闲页分散
pageblock 1: [M][_][M][_][M][M][_][_]...[M]  ← MOVABLE，被文件缓存切碎
pageblock 2: [R][_][_][R][_][R][_][_]...[R]  ← RECLAIMABLE
```

THP 请求 order=9（2MB）时：
1. `__rmqueue_smallest()` → order=9 free_list 为空
2. `__rmqueue_claim()` → 扫描所有 pageblock，无一满足 >50% 空闲
3. `__rmqueue_steal()` → `ALLOC_NOFRAGMENT` 阻止（ZONE_NORMAL）
4. **slowpath**：direct reclaim 回收页缓存（order=0），无法合并为高阶块
5. **direct compact**：迁移 MOVABLE 页，腾出 2MB 连续块 → 成功，但延迟高

**修复**：
```bash
echo 1 > /proc/sys/vm/defrag_mode            # 保护 pageblock 完整性
echo 100 > /proc/sys/vm/compaction_proactiveness  # 主动压缩
```

---

### 场景 2：ARM64 嵌入式设备 CMA 碎片化

**背景**：ARM64 边缘 AI 推理设备（4GB RAM，4K pages），NPU 需 16MB 连续 DMA 缓冲区。`MAX_PAGE_ORDER=10`（最大 4MB 单次分配），必须依赖 CMA 区域。

**症状**：
```bash
cat /proc/pagetypeinfo
# Node 0, zone DMA32, type CMA  0  0  0  0  0  0  0  0  1  0  0
#                               ↑ CMA 只有 1 个 order=8 (1MB) 块
```

NPU 请求 16MB → `cma_alloc()` 扫描 CMA bitmap 找不到连续 16MB 区域 → 失败。

**根因**：ARM64 的 DMA CMA 在 zone 初始化**之后**才预留（`bootmem_init()` 中 `dma_contiguous_reserve()` 在 `zone_sizes_init()` 之后），CMA 区域从 buddy 已认领的内存中划分。如果某些 CMA 页被 UNMOVABLE 分配临时征用且无法回收，CMA 内部就变成了"瑞士奶酪"：

```
CMA 区域（64MB 预留）：
[CMA空闲][CMA空闲][UNMOVABLE占用][CMA空闲][CMA空闲][MOVABLE占用][CMA空闲]...
   1MB       1MB        1MB        1MB       1MB        1MB       1MB
                                 ↑ 虽然总空闲 6MB，但最大连续块仅 2MB
```

**另一个坑**：当 `NR_FREE_CMA_PAGES < NR_FREE_PAGES / 2` 时，MOVABLE 分配**不会**优先从 CMA 取页，导致 CMA 被"冻住"——空闲但不被使用，而 DMA 分配又需要迁移 CMA 内页。

**修复**：
```bash
# 增大 CMA 预留
cma=256M
# 或通过设备树配置
linux,cma { default-size = <0x10000000>; };  # 256MB
```

---

### 场景 3：PCP 缓存导致的伪碎片化

**背景**：ARM64 设备运行高并发网络服务，频繁分配/释放 order=0 skb。

**症状**：`/proc/buddyinfo` 显示 order=0~2 页面极少，但 `/proc/meminfo` 显示 MemAvailable 充足。系统未 OOM，但偶尔出现 order=3 分配延迟。

**根因**：

```c
// 每个 CPU 的 PCP 缓存：
// batch = min(4GB zone >> 10, 256KB / 4KB) / 4 = 256 pages/CPU
// 64核: 最大缓存 ≈ 16384 pages ≈ 64MB
```

这些页面在 `free_pcppages_bulk()` 释放回 buddy 之前，不会出现在 buddyinfo 中。从 buddy 视角看是"碎片化"，实际上是 PCP 缓存。

当 `rmqueue_bulk()` 为 PCP 补充页面时，如果 order=0 free_list 空了，会从更高阶拆分：
```
PCP 补充 order=0 页面:
  order=0 list 空 → 分拆 order=1 → order=0 + order=0
  order=1 list 空 → 分拆 order=2 → order=1 + order=1
  ...
  不断蚕食高阶块
```

**修复**：
```bash
# 降低 PCP high watermark，页面更快回流 buddy
echo 8 > /proc/sys/vm/percpu_pagelist_high_fraction
```

---

### 场景 4：ARM64 NUMA 节点间碎片化不对称

**背景**：双 socket ARM64 服务器（2 NUMA nodes），每个节点 256GB RAM。

**症状**：`numastat -p <pid>` 显示进程主要在 Node 1 上分配，Node 1 的 THP 分配频繁失败，但 Node 0 还有大量 order=9 空闲块。

**根因**：

```c
// zonelist: [Node1/Normal] → [Node0/Normal]
// Node 1 分配 order=9:
//   1. 本地 __rmqueue_smallest() → 空
//   2. 本地 __rmqueue_claim() → 无 >50% 空闲 pageblock
//   3. ALLOC_NOFRAGMENT 阻止 steal
//   4. 遍历到 Node 0:
//      alloc_flags_nofragment() → 清除 ALLOC_NOFRAGMENT
//   5. Node 0 __rmqueue_steal() → 成功但污染 Node 0 pageblock
```

长期运行后，Node 0 的 pageblock 被跨节点 Steal 逐渐污染，两个节点碎片化程度不对称加剧。

**修复**：
```bash
# 交错分配，均衡节点压力
numactl --interleave=all ./your_app

# 启用 proactive compaction 清理远程节点碎片
echo 50 > /proc/sys/vm/compaction_proactiveness
```

---

### 场景 5：HIGHATOMIC 在 ARM64 中断上下文中的救命效应

**背景**：ARM64 设备驱动在中断上下文中需分配 2MB 连续内存做 DMA 缓冲区。中断上下文不能睡眠 → 不能 reclaim/compact。

**机制**：

```c
// mm/page_alloc.c:3361 --- reserve_highatomic_pageblock()
// 为原子分配预留约 1% zone managed pages
// 4GB zone: 1% = 40MB ≈ 20 个 2MB pageblock
max_managed = ALIGN((zone_managed_pages(zone) / 100), pageblock_nr_pages);
```

当 `gfp_mask` 包含 `__GFP_ATOMIC` 且 order >= pageblock_order，内核从 HIGHATOMIC 保留池分配。如果保留池也耗尽且无法 reclaim → 分配失败。

**监控**：
```bash
cat /proc/vmstat | grep highatomic
# nr_highatomic_reserve  → 当前保留的 pageblock 数
# nr_highatomic_failed   → 从 HIGHATOMIC 分配失败的次数
```

**修复思路**：
1. 驱动层：预分配 DMA 缓冲区（在可以睡眠的上下文中提前分配）
2. 系统层：增大 `min_free_kbytes` 间接增加 HIGHATOMIC 保留
3. 监控 `nr_highatomic_failed`，持续增长说明需要加大预留

---

### 场景速查表

| 场景 | 症状 | 根因 | 主要修复手段 |
|------|------|------|-------------|
| THP 分配失败 | `thp_fault_fallback` 增长，buddyinfo order=9 少 | UNMOVABLE 页切碎 pageblock | `defrag_mode=1`, `compaction_proactiveness=100` |
| CMA DMA 失败 | NPU/显卡分配大块连续内存失败 | CMA 内页被不可移动分配征用 | 增大 `cma=` 参数 |
| PCP 伪碎片化 | buddyinfo 低 order 页极少但内存充足 | PCP 缓存滞留 order=0 页 | 降低 `percpu_pagelist_high_fraction` |
| NUMA 碎片不对称 | 某节点 THP 频繁失败，另一节点空闲充足 | 跨节点 steal 污染远程 pageblock | `numactl --interleave`, proactive compaction |
| HIGHATOMIC 耗尽 | 中断上下文高阶分配返回 NULL | 原子保留池被掏空 | 预分配 DMA 缓冲；监控 `nr_highatomic_failed` |
