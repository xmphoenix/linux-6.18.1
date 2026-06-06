# Linux ARM64 内存规整 (Memory Compaction) 深度学习指南

> 适用范围：Linux 6.18.1 / ARM64 / 当前工作区源码
>
> 文档目标：系统梳理内存规整的原理、数据结构、核心流程、触发路径、ARM64 特有机制，并通过 QEMU 实验验证关键行为。

---

## 目录

<details>
<summary>展开目录</summary>

- [一句话先说清](#一句话先说清)
- [为什么需要内存规整](#为什么需要内存规整)
- [核心心智模型](#核心心智模型)
- [关键数据结构](#关键数据结构)
- [规整核心流程：双扫描器算法](#规整核心流程双扫描器算法)
- [规整触发路径](#规整触发路径)
  - [路径 1：直接规整 (Direct Compaction)](#路径-1直接规整-direct-compaction)
  - [路径 2：kcompactd 后台规整](#路径-2kcompactd-后台规整)
  - [路径 3：主动规整 (Proactive Compaction)](#路径-3主动规整-proactive-compaction)
  - [路径 4：用户空间手动触发](#路径-4用户空间手动触发)
- [延迟与跳过机制](#延迟与跳过机制)
- [规整与页迁移的关系](#规整与页迁移的关系)
- [ARM64 特有机制](#arm64-特有机制)
  - [CMA 预留与规整](#cma-预留与规整)
  - [Contiguous PTE 与规整效率](#contiguous-pte-与规整效率)
  - [MTE 标签在迁移中的处理](#mte-标签在迁移中的处理)
  - [大页迁移支持](#大页迁移支持)
  - [线性映射粒度与 set_direct_map](#线性映射粒度与-set_direct_map)
- [碎片评分与主动规整决策](#碎片评分与主动规整决策)
- [调试与统计接口](#调试与统计接口)
- [QEMU 实验](#qemu-实验)
  - [实验 1：构建 ARM64 内核与基础启动](#实验-1构建-arm64-内核与基础启动)
  - [实验 2：观察内存碎片与规整触发](#实验-2观察内存碎片与规整触发)
  - [实验 3：手动触发规整并观察结果](#实验-3手动触发规整并观察结果)
  - [实验 4：追踪规整过程的 Tracepoint](#实验-4追踪规整过程的-tracepoint)
  - [实验 5：主动规整与碎片评分验证](#实验-5主动规整与碎片评分验证)
  - [实验 6：CMA 与 alloc_contig_range 验证](#实验-6cma-与-alloc_contig_range-验证)
  - [实验 7：THP 分配与规整联动验证](#实验-7thp-分配与规整联动验证)
- [源码阅读路线图](#源码阅读路线图)
- [关键源码索引](#关键源码索引)

</details>

---

## 一句话先说清

**内存规整 = 把散落在 zone 低地址区的可移动页迁移到高地址区的空闲位置，从而在低地址区腾出连续空闲块，满足高阶分配请求。** 它的核心是"双扫描器 + 页迁移"：迁移扫描器从低到高找可移动页，空闲扫描器从高到低找空闲页，两者配合完成搬移。

---

## 为什么需要内存规整

| 问题 | 说明 |
|------|------|
| 外部碎片 | 系统总空闲页充足，但散布在各 pageblock 中无法拼出连续的高阶块 |
| THP 分配失败 | Transparent Huge Page 需要 order-9 (2MB) 连续块，碎片化后难以满足 |
| DMA 大缓冲区 | 设备需要物理连续的大块内存用于 DMA 传输 |
| CMA 分配失败 | 连续内存分配器 (CMA) 需要大块连续物理内存 |

**规整与回收的区别**：
- **回收 (reclaim)**：把不活跃的页释放掉 → 增加总空闲量，但不保证连续性
- **规整 (compact)**：把可移动页挪位置 → 不改变总空闲量，但让空闲页聚拢

两者互补：回收释放空间，规整让空间变得连续。

---

## 核心心智模型

```
┌─────────────────────────────────────────────────────────┐
│                    ZONE (e.g. ZONE_NORMAL)              │
│                                                         │
│  低 PFN                              高 PFN             │
│  ┌─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┐  │
│  │M│F│M│M│F│M│F│M│M│F│F│M│M│F│ │ │ │ │ │ │ │ │ │ │  │
│  └─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┘  │
│       ↑ 迁移扫描器              ↑ 空闲扫描器           │
│       (migrate_pfn)             (free_pfn)              │
│       从低→高找可移动页         从高→低找空闲页          │
│                                                         │
│  规整后:                                                 │
│  ┌─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┐  │
│  │ │ │ │ │ │ │ │ │ │ │ │ │ │ │ │ │ │ │ │M│M│M│M│ │  │
│  └─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┘  │
│       ←── 连续空闲区 ──→       ←── 可移动页聚拢 ──→    │
│                                                         │
└─────────────────────────────────────────────────────────┘

M = 已占用可移动页    F = 空闲页    空格 = 空闲页
```

**核心逻辑**：
1. 迁移扫描器从低 PFN 向上扫描，隔离 LRU 上或属于 movable_ops 的页
2. 空闲扫描器从高 PFN 向下扫描，隔离 buddy 空闲页
3. 调用 `migrate_pages()` 把迁移页搬到空闲页位置
4. 当两个扫描器相遇 (`compact_scanners_met()`) 或找到满足 order 的空闲块时停止

---

## 关键数据结构

### `struct compact_control` — 规整总控 (`mm/internal.h:875`)

```c
struct compact_control {
    struct list_head freepages[NR_PAGE_ORDERS]; // 隔离的空闲页（目标页），按 order 分组
    struct list_head migratepages;              // 隔离的待迁移页（源页）
    unsigned int nr_freepages;                  // 空闲页计数
    unsigned int nr_migratepages;               // 待迁移页计数
    unsigned long free_pfn;                     // 空闲扫描器当前位置（从高往低）
    unsigned long migrate_pfn;                  // 迁移扫描器当前位置（从低往高）
    unsigned long fast_start_pfn;               // 快速搜索起始 PFN
    struct zone *zone;                          // 当前操作的 zone
    unsigned long total_migrate_scanned;        // 迁移扫描器扫描总页数
    unsigned long total_free_scanned;           // 空闲扫描器扫描总页数
    int order;                                  // 目标分配阶数（-1 表示全 zone 规整）
    int migratetype;                            // 目标 migratetype
    enum migrate_mode mode;                     // MIGRATE_ASYNC / SYNC_LIGHT / SYNC
    bool direct_compaction;                     // true=分配路径触发, false=kcompactd
    bool proactive_compaction;                  // 主动规整模式
    bool whole_zone;                            // 全 zone 扫描
    bool contended;                             // 锁竞争导致退出
    bool alloc_contig;                          // alloc_contig_range 上下文
    const gfp_t gfp_mask;
    const unsigned int alloc_flags;
    const int highest_zoneidx;
};
```

### `struct capture_control` — 页捕获优化 (`mm/internal.h:919`)

```c
struct capture_control {
    struct compact_control *cc;
    struct page *page;     // 中断上下文释放页面时可直接捕获
};
```

当 `compact_zone_order()` 运行时，`capture_control` 挂在 `current->capture_control` 上。如果中断处理释放了一个满足 order 的页，可以直接"捕获"而不必经过 buddy 释放再分配。

### `enum compact_result` — 规整结果 (`include/linux/compaction.h:21`)

```c
COMPACT_NOT_SUITABLE_ZONE  // zone 不适合规整
COMPACT_SKIPPED            // 被跳过（zone 不满足条件）
COMPACT_DEFERRED           // 因历史失败被延迟
COMPACT_NO_SUITABLE_PAGE   // 内部状态：没找到合适页
COMPACT_CONTINUE           // 应继续扫描
COMPACT_COMPLETE           // 全 zone 扫完，未成功
COMPACT_PARTIAL_SKIPPED    // 部分扫描后停止
COMPACT_CONTENDED          // 因锁竞争退出
COMPACT_SUCCESS            // 规整成功，现在可以分配了
```

### `enum compact_priority` — 规整优先级 (`include/linux/compaction.h:9`)

```c
COMPACT_PRIO_SYNC_FULL   // 最高优先级，完全同步
COMPACT_PRIO_SYNC_LIGHT  // 默认优先级，轻量同步
COMPACT_PRIO_ASYNC       // 最低优先级，异步不阻塞
```

分配路径会逐步提升优先级：ASYNC → SYNC_LIGHT → SYNC_FULL。

### `enum migrate_mode` — 迁移模式 (`include/linux/migrate_mode.h:11`)

```c
MIGRATE_ASYNC       // 不阻塞，适用于规整首次尝试
MIGRATE_SYNC_LIGHT  // 阻塞大部分操作，但不调用 writepage
MIGRATE_SYNC        // 完全同步阻塞
```

---

## 规整核心流程：双扫描器算法

`compact_zone()` (`mm/compaction.c:2511`) 是核心函数，流程如下：

```
compact_zone(cc, capc)
│
├─ 1. 初始化扫描器位置
│     whole_zone → migrate_pfn=zone_start, free_pfn=zone_end
│     否则 → 使用缓存的 compact_cached_migrate_pfn / compact_cached_free_pfn
│
├─ 2. WHILE compact_finished() == COMPACT_CONTINUE:
│     │
│     ├─ a. isolate_migratepages(cc)        ← 迁移扫描器
│     │     ├─ fast_find_migrateblock()      ← 在 buddy free list 中快速定位
│     │     ├─ isolate_migratepages_block()  ← 隔离 LRU/movable 页
│     │     └─ 返回 ISOLATE_ABORT/NONE/SUCCESS
│     │
│     ├─ b. migrate_pages(&cc->migratepages, compaction_alloc,
│     │                   compaction_free, cc, cc->mode, MR_COMPACTION)
│     │     ├─ compaction_alloc()  ← 从隔离空闲列表提供目标页
│     │     ├─ compaction_free()   ← 返回迁移失败的目标页
│     │     └─ 内部调用 move_pages_to_new_lru() 更新 LRU
│     │
│     ├─ c. 检查 page capture (中断上下文捕获的页)
│     │
│     └─ d. drain PCP lists (迁移扫描器跨过上一个 pageblock 时)
│
├─ 3. release_free_list()  ← 释放未使用的隔离空闲页
│
├─ 4. 更新 vmstat 计数器和 trace 事件
│
└─ 5. 返回 compact_result
```

**扫描器相遇判断** (`compact_scanners_met()`)：

```c
// mm/compaction.c
static inline bool compact_scanners_met(struct compact_control *cc)
{
    return cc->free_pfn <= cc->migrate_pfn;
}
```

当 free_pfn ≤ migrate_pfn 时，两个扫描器已经"相遇"，意味着无法再找到更多可迁移/空闲页对，规整结束。

### 迁移扫描器的快速搜索优化

`fast_find_migrateblock()` (`mm/compaction.c:1923`) 不逐 pageblock 遍历，而是直接搜索 buddy free list 找到包含空闲页的 pageblock——这些 pageblock 更可能有可移动页与之交错，是迁移的好目标。

### 空闲扫描器的快速搜索优化

`fast_isolate_freepages()` (`mm/compaction.c:1504`) 直接搜索 `zone->free_area[order].free_list[MIGRATE_MOVABLE]`，从高 PFN 开始找大块空闲页，避免逐 pageblock 搜索。

---

## 规整触发路径

### 路径 1：直接规整 (Direct Compaction)

分配路径中高阶分配失败时触发：

```
__alloc_pages()
  → __alloc_pages_slowpath()                    [mm/page_alloc.c:4626]
    → __alloc_pages_direct_compact()            [mm/page_alloc.c:4081]
      → try_to_compact_pages()                  [mm/compaction.c:2814]
        → for_each_zone_zonelist:
            → compact_zone_order(zone, order, gfp, prio, ...)  [mm/compaction.c:2749]
              → compact_zone(&cc, &capc)        [mm/compaction.c:2511]
    → should_compact_retry()                    [mm/page_alloc.c:4139]
      → 逐步升级优先级: ASYNC → SYNC_LIGHT → SYNC_FULL
      → 最大重试次数 MAX_COMPACT_RETRIES = 16
```

**两次尝试时机**：
- **首次尝试** (`page_alloc.c:4722`)：costly order (> PAGE_ALLOC_COSTLY_ORDER=3) 或不可移动的高阶分配，在直接回收之前尝试
- **二次尝试** (`page_alloc.c:4817`)：直接回收之后，如果回收有进展

### 路径 2：kcompactd 后台规整

内核守护线程，在后台定期执行规整：

```
kcompactd()                                     [mm/compaction.c:3165]
  │
  ├─ wait_event_freezable_timeout(kcompactd_wait, ...)
  │
  ├─ if (!proactive_trigger):
  │     psi_memstall_enter()
  │     kcompactd_do_work(pgdat)                [mm/compaction.c:3055]
  │       → for each zone:
  │           compact_zone() with MIGRATE_SYNC_LIGHT
  │     psi_memstall_leave()
  │
  └─ if (should_proactive_compact_node()):
        compact_node(pgdat, true)               [mm/compaction.c:2893]
```

**唤醒来源**：
1. **分配路径**：直接规整成功或 `should_compact_retry()` 决定重试
2. **kswapd 回收后** (`mm/vmscan.c`)：kswapd 完成回收、降低 watermark boost、或放弃回收时调用 `wakeup_kcompactd()`
3. **sysctl 写入**：修改 `compaction_proactiveness` 时唤醒所有 kcompactd

### 路径 3：主动规整 (Proactive Compaction)

kcompactd 在无显式唤醒时，每 500ms (`HPAGE_FRAG_CHECK_INTERVAL_MSEC`) 检查碎片评分：

```
should_proactive_compact_node(pgdat)
  → fragmentation_score_node(pgdat) > wmark_high
    → Σ(zone_present_pages × extfrag_for_order(zone, COMPACTION_HPAGE_ORDER))
         / (node_present_pages + 1)
  → wmark_high = 100 - sysctl_compaction_proactiveness + leeway
```

### 路径 4：用户空间手动触发

| 接口 | 方式 | 代码位置 |
|------|------|----------|
| `echo 1 > /proc/sys/vm/compact_memory` | 全节点规整，MIGRATE_SYNC 模式 | `mm/compaction.c:2976` `sysctl_compaction_handler()` |
| `echo 1 > /sys/devices/system/node/nodeN/compact` | 单节点规整 (需 NUMA+SYSFS) | `mm/compaction.c:2996` `compact_store()` |

---

## 延迟与跳过机制

规整失败后有指数退避机制，避免反复尝试无效规整：

### 延迟 (Deferral)

```c
// mm/compaction.c:126
static void defer_compaction(struct zone *zone, int order)
{
    zone->compact_defer_shift++;    // 每次失败 +1，上限 COMPACT_MAX_DEFER_SHIFT=6
    // 即最多延迟 2^6 = 64 次
}

// mm/compaction.c:141
static bool compaction_deferred(struct zone *zone, int order)
{
    unsigned long defer_limit = 1UL << zone->compact_defer_shift;
    if (++zone->compact_considered >= defer_limit)
        return false;  // 考虑次数到达限制，不再延迟
    return true;       // 被延迟，跳过这次
}

// mm/compaction.c:164
void compaction_defer_reset(struct zone *zone, int order, bool alloc_success)
{
    if (alloc_success) {
        zone->compact_considered = 0;
        zone->compact_defer_shift = 0;  // 成功后清零
    }
}
```

### Pageblock Skip Hint

```c
// mm/compaction.c:419
static bool test_and_set_skip(struct compact_control *cc, unsigned long pfn)
{
    // 在 pageblock 的 migrateflags 中设置 PG_migrate_skip
    // 下次扫描时跳过这个 pageblock
}

// mm/compaction.c:349
static void __reset_isolation_suitable(struct zone *zone)
{
    // 遍历 zone，清除所有 pageblock 的 skip hint
    // 在规整重启 (compaction_restarting) 时调用
}
```

---

## 规整与页迁移的关系

规整本身不做搬移，它只做扫描和隔离，实际搬移委托给 `migrate_pages()`：

```c
// mm/compaction.c:2647 — compact_zone() 中的核心调用
err = migrate_pages(&cc->migratepages, compaction_alloc,
                    compaction_free, (unsigned long)cc, cc->mode,
                    MR_COMPACTION, &nr_succeeded);
```

**`compaction_alloc()`** (`mm/compaction.c:1845`)：
- 作为 `new_folio_t` 回调，从已隔离的空闲页列表中提供目标页
- 如果需要低阶页，会分裂高阶空闲页

**`compaction_free()`** (`mm/compaction.c:1855`)：
- 作为 `free_folio_t` 回调，迁移失败时把目标页放回隔离空闲列表

**`migrate_pages()`** (`mm/migrate.c:2064`) 内部流程：
```
migrate_pages()
  ├─ migrate_hugetlbs()          ← 大页单独处理
  ├─ migrate_pages_sync()        ← 先 ASYNC 批量，再 SYNC 重试
  │   └─ migrate_pages_batch()   ← 批量 unmap + move
  │     ├─ folio_mc_copy()       ← 复制页内容
  │     ├─ __folio_migrate_mapping() ← 更新页表映射
  │     └─ folio_migrate_flags() ← 迁移页标志位
  └─ 返回迁移统计 (nr_succeeded, nr_failed)
```

---

## ARM64 特有机制

### CMA 预留与规整

ARM64 在 `bootmem_init()` 中预留两类 CMA 区域：

```c
// arch/arm64/mm/init.c — bootmem_init()
bootmem_init()
  ├─ arm64_hugetlb_cma_reserve()   ← 为 gigantic hugepage 预留 CMA
  │   └─ hugetlb_cma_reserve(PUD_SHIFT - PAGE_SHIFT)  // 4K页: order=30 = 1GB
  ├─ zone_sizes_init()
  ├─ dma_contiguous_reserve(arm64_dma_phys_limit)  ← DMA CMA 区域
  └─ arch_reserve_crashkernel()
```

CMA 分配 (`alloc_contig_range()`) 本质上也是一种规整——它隔离目标范围内的所有可移动页并迁移走：

```
alloc_contig_range()
  ├─ isolate_migratepages_range()  ← 隠离范围内可移动页
  ├─ migrate_pages()               ← 迁移到其他位置
  └─ isolate_freepages_range()     ← 隔离范围内空闲页
```

### Contiguous PTE 与规整效率

ARM64 的 `PTE_CONT` (bit 52) 允许连续的 PTE 共享一个 TLB entry：

| 页粒度 | CONT_PTE 大小 | CONT_PMD 大小 |
|--------|--------------|--------------|
| 4K | 16×4K = 64KB | 16×2M = 32MB |
| 16K | 128×16K = 2MB | 32×32M = 1GB |
| 64K | 32×64K = 2MB | 32×512M = 16GB |

`contpte.c` 透明管理这个 bit：
- `contpte_try_fold()` — 满足对齐条件时设置 CONT bit
- `contpte_try_unfold()` — 修改单个页时需要先"拆开"连续映射
- `pte_batch_hint()` — 告知核心 MM 可以批量处理多少页

**对规整的影响**：规整迁移页后，新的映射可能触发 contpte fold/unfold，需要额外的 TLB 维护操作。BBML2 硬件支持可以减少 TLB flush。

### MTE 标签在迁移中的处理

ARM64 的 Memory Tagging Extension (MTE) 为每个页附加标签。`copy_highpage()` (`arch/arm64/mm/copypage.c:17`) 在迁移过程中确保标签正确复制：

```c
void copy_highpage(struct page *to, struct page *from) {
    copy_page(kto, kfrom);              // 复制页内容

    if (!system_supports_mte()) return;

    if (folio_test_hugetlb(src)) {
        // 大页：复制所有子页的 MTE 标签
        for (i = 0; i < nr_pages; i++)
            mte_copy_page_tags(kto + i*PAGE_SIZE, kfrom + i*PAGE_SIZE);
    } else if (page_mte_tagged(from)) {
        // 普通页：目标页可能复用已标签页
        try_page_mte_tagging(to);
        mte_copy_page_tags(kto, kfrom);
        set_page_mte_tagged(to);
    }
}
```

**关键注意**：迁移可能复用同一物理页（源页和目标页相同），此时必须正确管理标签状态。

### 大页迁移支持

ARM64 支持所有合法大页尺寸的迁移 (`arch/arm64/mm/hugetlbpage.c:69`)：

```c
bool arch_hugetlb_migration_supported(struct hstate *h) {
    size_t pagesize = huge_page_size(h);
    return __hugetlb_valid_size(pagesize);  // 所有 ARM64 合法大页尺寸都可迁移
}
```

Kconfig 选择 (`arch/arm64/Kconfig`):
```
select ARCH_ENABLE_HUGEPAGE_MIGRATION if HUGETLB_PAGE && MIGRATION
select ARCH_ENABLE_THP_MIGRATION if TRANSPARENT_HUGEPAGE
```

### 线性映射粒度与 set_direct_map

`can_set_direct_map()` (`arch/arm64/mm/pageattr.c`) 决定是否可以逐页控制线性映射：

```c
bool can_set_direct_map(void) {
    return rodata_full || debug_pagealloc_enabled() ||
           arm64_kfence_can_set_direct_map() || is_realm_world();
}
```

当 `can_set_direct_map()` 返回 true 时，线性映射使用 PTE 级别（而非 PMD/PUD block），支持：
- `set_direct_map_invalid_noflush()` — 页迁移时使源页映射无效
- `set_direct_map_default_noflush()` — 迁移完成后恢复映射
- `set_direct_map_valid_noflush()` — 热插拔时启用/禁用页映射

**对规整的意义**：如果线性映射使用 block mapping，迁移时无法单独使某个页的映射无效，需要依赖 TLB flush 和 BBM 机制。

---

## 碎片评分与主动规整决策

```c
// mm/compaction.c:2167
static unsigned int fragmentation_score_zone(struct zone *zone)
{
    return extfrag_for_order(zone, COMPACTION_HPAGE_ORDER);  // [0, 100]
}

// mm/compaction.c:2197
static unsigned int fragmentation_score_node(pg_data_t *pgdat)
{
    // 各 zone 加权求和：zone_present_pages × score / node_present_pages
}

// mm/compaction.c:2214
static unsigned int fragmentation_score_wmark(bool low)
{
    unsigned int wmark_low = 100U - sysctl_compaction_proactiveness;
    unsigned int leeway = min(10U, wmark_low / 2);
    return low ? wmark_low : min(wmark_low + leeway, 100U);
}

// mm/compaction.c:2223
static bool should_proactive_compact_node(pg_data_t *pgdat)
{
    if (!sysctl_compaction_proactiveness || kswapd_is_running(pgdat))
        return false;
    return fragmentation_score_node(pgdat) > fragmentation_score_wmark(false);
}
```

**阈值关系**：
- `wmark_high` = `100 - proactiveness` + `leeway(≤10)` → 开始主动规整
- `wmark_low` = `100 - proactiveness` → 主动规整可以停止
- 默认 `proactiveness=20` → `wmark_high=90`, `wmark_low=80`
- 无进展时退避：`timeout << COMPACT_MAX_DEFER_SHIFT` (64× 延长超时)

---

## 调试与统计接口

### /proc/vmstat 规整计数器

| 计数器 | 含义 |
|--------|------|
| `compact_migrate_scanned` | 迁移扫描器扫描的总页数 |
| `compact_free_scanned` | 空闲扫描器扫描的总页数 |
| `compact_isolated` | 隔离的页数 |
| `compact_stall` | 因规整导致的分配停顿次数 |
| `compact_fail` | 规整失败次数 |
| `compact_success` | 规整成功次数 |
| `compact_daemon_wake` | kcompactd 唤醒次数 |
| `compact_daemon_migrate_scanned` | kcompactd 迁移扫描器扫描页数 |
| `compact_daemon_free_scanned` | kcompactd 空闲扫描器扫描页数 |
| `pgmigrate_success` | 页迁移成功次数 |
| `pgmigrate_fail` | 页迁移失败次数 |
| `thp_migration_success` | THP 迁移成功次数 |
| `thp_migration_fail` | THP 迁移失败次数 |
| `thp_migration_split` | THP 因迁移拆分次数 |

### /sys/kernel/debug/extfrag/

| 文件 | 含义 |
|------|------|
| `unusable_index` | 每 zone 每 order 的不可用空闲空间指数 [0-1] |
| `extfrag_index` | 每 zone 每 order 的碎片指数 [-1-1]。-1=分配可成功; 0=因内存不足失败; 1=因碎片失败 |

### sysctl 接口

| 路径 | 默认值 | 用途 |
|------|--------|------|
| `/proc/sys/vm/compact_memory` | WO | 写 1 触发全节点规整 |
| `/proc/sys/vm/compaction_proactiveness` | 20 | 0-100，控制主动规整激进程度 |
| `/proc/sys/vm/extfrag_threshold` | 500 | 0-1000，碎片指数阈值 |
| `/proc/sys/vm/compact_unevictable_allowed` | 1 | 是否扫描 unevictable LRU |
| `/proc/sys/vm/defrag_mode` | 0 | 启用反碎片模式 |

### Tracepoint 事件

| Tracepoint | 含义 |
|------------|------|
| `mm_compaction_begin` | 规整开始 |
| `mm_compaction_end` | 规整结束（带结果） |
| `mm_compaction_migratepages` | 迁移页统计 |
| `mm_compaction_isolate_migratepages` | 迁移扫描器隔离页 |
| `mm_compaction_isolate_freepages` | 空闲扫描器隔离页 |
| `mm_compaction_try_to_compact_pages` | 直接规整入口 |
| `mm_compaction_suitable` | zone 适合性判断 |
| `mm_compaction_deferred` | 规整被延迟 |
| `mm_compaction_kcompactd_sleep` | kcompactd 睡眠 |
| `mm_compaction_wakeup_kcompactd` | kcompactd 唤醒请求 |
| `mm_migrate_pages` | 页迁移批量结果 |

---

## QEMU 实验

### 实验 1：构建 ARM64 内核与基础启动

**目标**：构建可在 QEMU 上启动的 ARM64 内核，验证规整代码编译和基础运行。

```bash
# 1. 安装交叉编译工具和 QEMU
sudo apt install gcc-aarch64-linux-gnu qemu-system-aarch64

# 2. 配置内核（启用规整相关选项）
cd /home/ybzhang/kernel/linux-6.18.1
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig

# 检查关键配置已启用
grep -E "COMPACTION|TRANSPARENT_HUGEPAGE|MIGRATION|CMA" .config
# 期望输出:
# CONFIG_COMPACTION=y
# CONFIG_MIGRATION=y
# CONFIG_TRANSPARENT_HUGEPAGE=y (或 =m)
# CONFIG_CMA=y

# 3. 编译内核
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)

# 4. 创建最小 rootfs
mkdir -p rootfs
cd rootfs

# 创建 init 脚本（使用 busybox 或静态 init）
cat > init << 'EOF'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t debugfs debugfs /sys/kernel/debug
mount -t tmpfs tmpfs /tmp

echo "=== Memory Compaction Test Environment ==="
echo "Kernel: $(uname -r)"
echo "Arch: $(uname -m)"
cat /proc/meminfo | head -5

# 等待 kcompactd 初始化
sleep 2
echo "=== Compaction vmstat ==="
grep compact /proc/vmstat

exec /bin/sh
EOF
chmod +x init

# 创建 cpio rootfs
find . | cpio -o -H newc > ../rootfs.cpio
cd ..

# 5. 启动 QEMU
qemu-system-aarch64 \
  -machine virt \
  -cpu cortex-a57 \
  -m 512M \
  -kernel arch/arm64/boot/Image \
  -initrd rootfs.cpio \
  -append "console=ttyAMA0 earlycon=pl011,0x9000000 init=/init" \
  -nographic \
  -smp 2
```

**验证要点**：
- 启动后 `/proc/vmstat` 中 `compact_daemon_wake` > 0（kcompactd 已唤醒）
- `compact_migrate_scanned` 和 `compact_free_scanned` 有初始值
- THP 相关配置：`always` 或 `madvise`

### 实验 2：观察内存碎片与规整触发

**目标**：人为制造碎片，观察直接规整的触发。

```bash
# 在 QEMU 启动的系统中执行

# 1. 查看初始碎片状态
cat /proc/buddyinfo
# 示例输出: Node 0, zone Normal  ... 0 1 2 3 4 5 ... (高阶块数量)

cat /sys/kernel/debug/extfrag/unusable_index
# 观察各 order 的不可用空闲空间指数

# 2. 制造碎片：大量小页分配
# 使用 stress-ng 或手动小程序
cat > /tmp/frag.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>

#define PAGE_SIZE 4096
#define NR_PAGES 4000  // 约 16MB

int main() {
    void *pages[NR_PAGES];
    int i;

    // 分配大量单页，制造碎片
    for (i = 0; i < NR_PAGES; i++) {
        pages[i] = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (pages[i] == MAP_FAILED) {
            printf("mmap failed at page %d\n", i);
            break;
        }
        // 写入数据确保物理页已分配
        memset(pages[i], 0xAA, PAGE_SIZE);
    }

    printf("Allocated %d pages (%ld KB)\n", i, (long)i * PAGE_SIZE / 1024);

    // 释放部分页制造间隙（每隔 2 页释放 1 页）
    for (i = 0; i < NR_PAGES; i += 2) {
        if (pages[i] != MAP_FAILED)
            munmap(pages[i], PAGE_SIZE);
    }

    printf("Released alternate pages to create fragmentation\n");

    // 观察 buddyinfo 变化
    sleep(1);

    // 尝试分配大块连续内存触发规整
    void *huge = mmap(NULL, 2 * 1024 * 1024, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (huge != MAP_FAILED) {
        printf("2MB allocation succeeded (THP)\n");
        memset(huge, 0xBB, 2 * 1024 * 1024);
    } else {
        printf("2MB allocation failed (fragmentation)\n");
    }

    sleep(5);
    return 0;
}
EOF

# 交叉编译
aarch64-linux-gnu-gcc -static /tmp/frag.c -o rootfs/frag
# 重建 rootfs

# 3. 运行后查看规整统计
grep compact /proc/vmstat
cat /proc/buddyinfo
```

**验证要点**：
- `compact_stall` 增长 → 分配路径触发了直接规整
- `compact_success` 或 `compact_fail` 增长
- buddyinfo 中高阶块数量变化

### 实验 3：手动触发规整并观察结果

**目标**：通过 sysctl 手动触发规整，观察前后碎片变化。

```bash
# 在 QEMU 系统中

# 1. 记录规整前状态
echo "=== Before compaction ==="
cat /proc/buddyinfo
grep compact /proc/vmstat
cat /sys/kernel/debug/extfrag/unusable_index

# 2. 触发全节点规整
echo 1 > /proc/sys/vm/compact_memory

# 3. 等待规整完成
sleep 3

# 4. 记录规整后状态
echo "=== After compaction ==="
cat /proc/buddyinfo
grep compact /proc/vmstat
cat /sys/kernel/debug/extfrag/unusable_index

# 5. 对比分析
# - buddyinfo 高阶块数量应该增加
# - unusable_index 应该下降
# - compact_success 应该增长
```

### 实验 4：追踪规整过程的 Tracepoint

**目标**：使用 ftrace 追踪规整的详细过程。

```bash
# 在 QEMU 系统中

# 1. 启用规整 tracepoint
echo 1 > /sys/kernel/debug/tracing/events/compaction/enable
echo 1 > /sys/kernel/debug/tracing/events/migrate/enable

# 2. 触发规整
echo 1 > /proc/sys/vm/compact_memory

# 3. 读取 trace
sleep 2
cat /sys/kernel/debug/tracing/trace | head -50

# 4. 过滤特定事件
cat /sys/kernel/debug/tracing/trace | grep mm_compaction_begin
cat /sys/kernel/debug/tracing/trace | grep mm_compaction_end
cat /sys/kernel/debug/tracing/trace | grep mm_compaction_migratepages

# 5. 清理
echo 0 > /sys/kernel/debug/tracing/events/compaction/enable
echo 0 > /sys/kernel/debug/tracing/events/migrate/enable
echo > /sys/kernel/debug/tracing/trace
```

**期望 trace 输出格式**：
```
<...>-123  [001] d... 100.0: mm_compaction_begin: zone=Normal move_pfn=0x100 free_pfn=0x8000 sync=1
<...>-123  [001] d... 100.1: mm_compaction_migratepages: nr_migrated=32 nr_failed=0
<...>-123  [001] d... 100.2: mm_compaction_end: status=success
```

### 实验 5：主动规整与碎片评分验证

**目标**：验证主动规整的碎片评分机制。

```bash
# 1. 设置主动规整激进度
echo 50 > /proc/sys/vm/compaction_proactiveness
# wmark_high = 100 - 50 + leeway(~5) = 55
# wmark_low = 100 - 50 = 50

# 2. 查看碎片评分（间接方式）
cat /sys/kernel/debug/extfrag/extfrag_index

# 3. 制造碎片后观察 kcompactd 主动规整
# (使用实验 2 的碎片制造方法)
./frag

# 4. 观察 kcompactd 唤醒和规整
sleep 10
grep compact /proc/vmstat
grep compact_daemon /proc/vmstat

# 5. 关闭主动规整对比
echo 0 > /proc/sys/vm/compaction_proactiveness
# 此时只有显式唤醒才会触发 kcompactd
```

### 实验 6：CMA 与 alloc_contig_range 验证

**目标**：验证 CMA 区域和 alloc_contig_range 行为。

```bash
# 1. 查看 CMA 信息
cat /proc/meminfo | grep Cma
dmesg | grep cma

# 2. 查看 CMA 区域详情
cat /sys/kernel/debug/cma/cma-reserved/stats  # 如果可用

# 3. 通过内核模块测试 alloc_contig_range
# 创建测试模块:
cat > /tmp/cma_test.c << 'EOF'
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/cma.h>
#include <linux/page-isolation.h>

static int __init cma_test_init(void)
{
    struct page *page;
    unsigned long count = 8;  // 8 页 = 32KB 连续

    // 从 CMA 分配连续页
    page = cma_alloc(&dma_contiguous_default_area, count, 0, false);
    if (page) {
        pr_info("CMA alloc success: pfn=%lu order=%lu\n",
                page_to_pfn(page), count);
        cma_release(&dma_contiguous_default_area, page, count);
        pr_info("CMA release done\n");
    } else {
        pr_info("CMA alloc failed\n");
    }
    return 0;
}

module_init(cma_test_init);
MODULE_LICENSE("GPL");
EOF

# 4. 查看规整统计变化
grep compact /proc/vmstat
```

### 实验 7：THP 分配与规整联动验证

**目标**：验证 THP 分配失败时触发规整的完整路径。

```bash
# 1. 设置 THP 为 always 模式
echo always > /sys/kernel/mm/transparent_hugepage/enabled

# 2. 记录初始规整统计
BEFORE=$(cat /proc/vmstat | grep compact_stall)

# 3. 大量碎片化后尝试 THP 分配
# 编写测试程序:
cat > /tmp/thp_test.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

int main() {
    // Phase 1: 制造碎片
    void *frag[4000];
    for (int i = 0; i < 4000; i++) {
        frag[i] = mmap(NULL, 4096, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        memset(frag[i], 1, 4096);
    }
    // 释放间隔页
    for (int i = 0; i < 4000; i += 3) {
        munmap(frag[i], 4096);
    }

    // Phase 2: 尝试 THP 分配
    void *thp = mmap(NULL, 2*1024*1024, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (thp != MAP_FAILED) {
        memset(thp, 2, 2*1024*1024);  // 触发实际物理页分配
        printf("THP alloc: %p\n", thp);

        // 检查是否真的是 THP
        FILE *f = fopen("/proc/self/smaps", "r");
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "AnonHugePages"))
                printf("  %s", line);
        }
        fclose(f);
    }
    sleep(3);
    return 0;
}
EOF

aarch64-linux-gnu-gcc -static /tmp/thp_test.c -o rootfs/thp_test

# 4. 运行后查看
AFTER=$(cat /proc/vmstat | grep compact_stall)
echo "Stall count delta:"
echo "$BEFORE"
echo "$AFTER"

grep thp /proc/vmstat
```

**验证要点**：
- THP 分配失败时 `compact_stall` 增长
- 规整成功后 THP 分配可能成功
- `thp_migration_success` 或 `thp_migration_split` 增长

---

## 源码阅读路线图

建议按以下顺序阅读，每步都有明确的阅读目标：

### 第 1 步：理解入口和结果 (`include/linux/compaction.h`)
- 阅读 `compact_priority` 和 `compact_result` 枚举
- 阅读 `compaction_suitable()`、`try_to_compact_pages()` 声明

### 第 2 步：理解数据结构 (`mm/internal.h`)
- 重点阅读 `struct compact_control` (line 875)
- 阅读 `struct capture_control` (line 919)
- 对比 `struct migration_target_control` (line 1339)

### 第 3 步：理解延迟机制 (`mm/compaction.c:120-180`)
- `defer_compaction()` → `compaction_deferred()` → `compaction_defer_reset()` → `compaction_restarting()`
- 理解指数退避逻辑和 `compact_defer_shift` 上限

### 第 4 步：理解隔离逻辑 (`mm/compaction.c`)
- `isolate_migratepages_block()` (line 837) — 迁移页隔离的详细逻辑
- `isolate_freepages_block()` (line 556) — 空闲页隔离
- `fast_find_migrateblock()` (line 1923) — buddy 搜索优化
- `fast_isolate_freepages()` (line 1504) — 空闲页快速搜索

### 第 5 步：理解核心规整循环 (`mm/compaction.c:2511-2747`)
- `compact_zone()` — 双扫描器主循环
- `__compact_finished()` (line 2234) — 终止条件判断
- `compact_scanners_met()` — 扫描器相遇判断

### 第 6 步：理解触发路径 (`mm/compaction.c:2749-2839` + `mm/page_alloc.c`)
- `compact_zone_order()` (line 2749) — 直接规整封装
- `try_to_compact_pages()` (line 2814) — zonelist 遍历
- `__alloc_pages_slowpath()` (page_alloc.c:4626) — 分配路径中规整的位置

### 第 7 步：理解 kcompactd (`mm/compaction.c:3055-3330`)
- `kcompactd_do_work()` (line 3055)
- `kcompactd()` (line 3165)
- `wakeup_kcompactd()` (line 3135)
- `kcompactd_init()` (line 3323) — subsys_initcall

### 第 8 步：理解碎片评分 (`mm/compaction.c:2167-2232`)
- `fragmentation_score_zone()` → `fragmentation_score_zone_weighted()` → `fragmentation_score_node()`
- `should_proactive_compact_node()`
- `fragmentation_score_wmark()`

### 第 9 步：理解页迁移 (`mm/migrate.c`)
- `migrate_pages()` (line 2064) — 规整调用的入口
- `migrate_pages_batch()` (line 1775)
- `alloc_migration_target()` (line 2161)

### 第 10 步：理解 ARM64 特有机制
- `arch/arm64/mm/copypage.c` — `copy_highpage()` MTE 标签处理
- `arch/arm64/mm/hugetlbpage.c` — `arch_hugetlb_migration_supported()`
- `arch/arm64/mm/pageattr.c` — `can_set_direct_map()` + `set_direct_map_*()`
- `arch/arm64/mm/contpte.c` — contpte fold/unfold
- `arch/arm64/mm/init.c` — CMA 预留 (`bootmem_init`)

---

## 关键源码索引

| 文件 | 行号 | 函数/结构 | 说明 |
|------|------|-----------|------|
| `mm/compaction.c` | 126 | `defer_compaction()` | 延迟规整 |
| `mm/compaction.c` | 141 | `compaction_deferred()` | 判断是否延迟 |
| `mm/compaction.c` | 164 | `compaction_defer_reset()` | 成功后清除延迟 |
| `mm/compaction.c` | 349 | `__reset_isolation_suitable()` | 清除 skip hint |
| `mm/compaction.c` | 419 | `test_and_set_skip()` | 设置 pageblock skip |
| `mm/compaction.c` | 556 | `isolate_freepages_block()` | 空闲页隔离 |
| `mm/compaction.c` | 837 | `isolate_migratepages_block()` | 迁移页隔离 |
| `mm/compaction.c` | 1504 | `fast_isolate_freepages()` | buddy 空闲搜索优化 |
| `mm/compaction.c` | 1845 | `compaction_alloc()` | 规整迁移目标分配 |
| `mm/compaction.c` | 1855 | `compaction_free()` | 规整迁移目标释放 |
| `mm/compaction.c` | 1923 | `fast_find_migrateblock()` | buddy 迁移搜索优化 |
| `mm/compaction.c` | 2167 | `fragmentation_score_zone()` | zone 碎片评分 |
| `mm/compaction.c` | 2197 | `fragmentation_score_node()` | node 碎片评分 |
| `mm/compaction.c` | 2223 | `should_proactive_compact_node()` | 主动规整决策 |
| `mm/compaction.c` | 2234 | `__compact_finished()` | 规整终止判断 |
| `mm/compaction.c` | 2511 | `compact_zone()` | **核心规整循环** |
| `mm/compaction.c` | 2749 | `compact_zone_order()` | 直接规整封装 |
| `mm/compaction.c` | 2814 | `try_to_compact_pages()` | **直接规整入口** |
| `mm/compaction.c` | 2893 | `compact_node()` | 全节点规整 |
| `mm/compaction.c` | 2976 | `sysctl_compaction_handler()` | compact_memory sysctl |
| `mm/compaction.c` | 3055 | `kcompactd_do_work()` | kcompactd 工作函数 |
| `mm/compaction.c` | 3135 | `wakeup_kcompactd()` | kcompactd 唤醒 |
| `mm/compaction.c` | 3165 | `kcompactd()` | **kcompactd 主循环** |
| `mm/compaction.c` | 3323 | `kcompactd_init()` | kcompactd 初始化 |
| `mm/internal.h` | 875 | `struct compact_control` | **规整总控结构** |
| `mm/internal.h` | 919 | `struct capture_control` | 页捕获控制 |
| `mm/page_alloc.c` | 4626 | `__alloc_pages_slowpath()` | 分配慢路径 |
| `mm/page_alloc.c` | 4081 | `__alloc_pages_direct_compact()` | 直接规整调用 |
| `mm/migrate.c` | 2064 | `migrate_pages()` | 页迁移主入口 |
| `mm/migrate.c` | 1775 | `migrate_pages_batch()` | 批量页迁移 |
| `mm/vmscan.c` | 7197 | `wakeup_kcompactd()` | kswapd 唤醒 kcompactd |
| `mm/vmstat.c` | 2349 | `unusable_index` | 不可用空闲空间指数 |
| `mm/vmstat.c` | 2395 | `extfrag_index` | 碎片指数 |
| `arch/arm64/mm/init.c` | — | `bootmem_init()` | CMA 预留 |
| `arch/arm64/mm/copypage.c` | 17 | `copy_highpage()` | MTE 标签迁移 |
| `arch/arm64/mm/hugetlbpage.c` | 69 | `arch_hugetlb_migration_supported()` | 大页迁移支持 |
| `arch/arm64/mm/pageattr.c` | — | `can_set_direct_map()` | 直接映射粒度 |
| `arch/arm64/mm/contpte.c` | — | `contpte_convert()` | contpte 转换 |
| `include/linux/compaction.h` | 9 | `enum compact_priority` | 规整优先级 |
| `include/linux/compaction.h` | 21 | `enum compact_result` | 规整结果 |
| `include/trace/events/compaction.h` | — | 各 tracepoint | 规整追踪点 |

---

> 本文档基于 Linux 6.18.1 源码分析生成，所有行号和函数签名均对应当前工作区代码。