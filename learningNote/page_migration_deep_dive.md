# Linux 页面迁移 (Page Migration) 深度分析

> 基于 Linux 6.18.1 / ARM64 / `CONFIG_NUMA=y` / `CONFIG_MIGRATION=y` / `CONFIG_COMPACTION=y`
> 源码路径：`mm/migrate.c` · `mm/migrate_device.c` · `mm/compaction.c` · `include/linux/migrate.h`

---

## 目录

1. [页面迁移概述](#一页面迁移概述)
2. [十大迁移触发场景](#二十大迁移触发场景)
3. [核心数据结构](#三核心数据结构)
4. [迁移四阶段全流程](#四迁移四阶段全流程)
5. [核心函数逐层解析](#五核心函数逐层解析)
6. [迁移条目机制 (Migration Entry)](#六迁移条目机制-migration-entry)
7. [设备页面迁移 (Device Migration)](#七设备页面迁移-device-migration)
8. [Compaction 与迁移的协作](#八compaction-与迁移的协作)
9. [NUMA 自动平衡迁移](#九numa-自动平衡迁移)
10. [内存热插拔迁移](#十内存热插拔迁移)
11. [HugeTLB 迁移](#十一hugetlb-迁移)
12. [完整调用链一览](#十二完整调用链一览)
13. [调试与观测](#十三调试与观测)

---

## 一、页面迁移概述

### 1.1 什么是页面迁移

页面迁移是将物理页面的内容从一个物理页框（source page）复制到另一个物理页框（destination page），然后将所有指向源页面的页表项更新为指向目标页面，最后释放源页面。

```
迁移前:                             迁移后:
┌─────────────────┐                ┌─────────────────┐
│  VMA (虚拟地址)  │                │  VMA (虚拟地址)  │
│  0x7f000000     │                │  0x7f000000     │
└───────┬─────────┘                └───────┬─────────┘
        │ PTE                              │ PTE (已更新)
        ▼                                  ▼
┌─────────────────┐                ┌─────────────────┐
│ 源物理页 (PFN A) │  ──迁移──▶    │ 目标物理页(PFN B)│
│ [Page Content]  │                │ [Page Content]  │
└─────────────────┘                └─────────────────┘
                                           │
                              ┌────────────┘
                              ▼
                        ┌─────────────────┐
                        │ 源物理页 (PFN A) │
                        │ [已释放回Buddy]  │
                        └─────────────────┘
```

### 1.2 迁移的原子性保证

迁移期间，通过**迁移条目 (migration entry)** 替换 PTE 保证了原子性：

```
迁移进行中:
  进程访问虚拟地址
       │
       ▼
  PTE = swp_entry (migration entry)
       │
       ▼
  触发缺页异常 ──▶ migration_entry_wait()
       │                │
       │          等待迁移完成
       │                │
       ▼                ▼
  迁移完成后重试 ──▶ 获得正确的物理页
```

### 1.3 迁移模式 (Migrate Mode)

```c
// include/linux/migrate_mode.h
enum migrate_mode {
    MIGRATE_ASYNC,        // 不阻塞：不等待锁、不等待 writeback
    MIGRATE_SYNC_LIGHT,   // 轻量同步：允许阻塞但不等待 writepage
    MIGRATE_SYNC,         // 完全同步：允许阻塞等待所有操作
};
```

| 模式 | 阻塞行为 | 使用场景 |
|------|---------|---------|
| `MIGRATE_ASYNC` | 不等待锁，不等待 writeback | compaction, 后台迁移 |
| `MIGRATE_SYNC_LIGHT` | 等待锁，不等待 writeback | THP 分配失败后的直接压缩 |
| `MIGRATE_SYNC` | 等待所有操作 | syscall (move_pages), 内存热插拔 |

---

## 二、十大迁移触发场景

```c
// include/linux/migrate_mode.h
enum migrate_reason {
    MR_COMPACTION,        // 内存压缩：腾出连续物理页
    MR_MEMORY_FAILURE,    // 内存故障：隔离损坏页
    MR_MEMORY_HOTPLUG,    // 内存热插拔：拔出内存前迁移数据
    MR_SYSCALL,           // 系统调用：move_pages(), cpuset
    MR_MEMPOLICY_MBIND,   // 内存策略：mbind() 绑定 NUMA 节点
    MR_NUMA_MISPLACED,    // NUMA 平衡：将页面迁到访问它的 CPU 所在节点
    MR_CONTIG_RANGE,      // 连续范围分配：alloc_contig_range()
    MR_LONGTERM_PIN,      // 长期引脚：解决 longterm pin 与迁移冲突
    MR_DEMOTION,          // 降级：将冷页面从快速内存迁到慢速内存
    MR_DAMON,             // DAMON：基于访问监控的主动迁移
    MR_TYPES
};
```

### 2.1 各场景详解

```
触发源                        调用链
─────────────────────────────────────────────────────────────
compaction (kcompactd/直接)
  └─ compact_zone() → migrate_pages(..., MR_COMPACTION)

memory_failure (hwpoison)
  └─ soft_offline_page() → migrate_pages(..., MR_MEMORY_FAILURE)

memory_hotplug (offline)
  └─ do_migrate_range() → migrate_pages(..., MR_MEMORY_HOTPLUG)

sys_move_pages (syscall)
  └─ do_pages_move() → migrate_pages(..., MR_SYSCALL)

mbind (syscall)
  └─ queue_pages_range() → migrate_pages(..., MR_MEMPOLICY_MBIND)

NUMA balancing (自动)
  └─ migrate_misplaced_folio() → migrate_pages(..., MR_NUMA_MISPLACED)

alloc_contig_range (CMA/驱动)
  └─ alloc_contig_range() → migrate_pages(..., MR_CONTIG_RANGE)

longterm pin (vfio/gup)
  └─ migrate_longterm_unpinnable_pages() → migrate_pages(..., MR_LONGTERM_PIN)

memory tiering (降级)
  └─ demote_folio_list() → migrate_pages(..., MR_DEMOTION)

DAMON (主动迁移)
  └─ damon_pa_migrate() → migrate_pages(..., MR_DAMON)
```

---

## 三、核心数据结构

### 3.1 migration_target_control

```c
struct migration_target_control {
    int nid;                // 目标 NUMA 节点
    nodemask_t *nmask;      // 允许的 NUMA 节点掩码
    gfp_t gfp_mask;         // 目标页分配标志
    enum migrate_reason reason;
};
```

### 3.2 movable_operations（驱动可迁移页面接口）

```c
// include/linux/migrate.h
struct movable_operations {
    bool (*isolate_page)(struct page *, isolate_mode_t);
    int  (*migrate_page)(struct page *dst, struct page *src, enum migrate_mode);
    void (*putback_page)(struct page *);
};
```

用于非 LRU 页面的迁移（如 balloon、zsmalloc 压缩页面）。

### 3.3 migrate_vma（设备页面迁移上下文）

```c
// include/linux/migrate.h
struct migrate_vma {
    struct vm_area_struct *vma;
    unsigned long *dst;       // 目标 PFN 数组
    unsigned long *src;       // 源 PFN 数组
    unsigned long cpages;     // 收集的页面数
    unsigned long npages;     // 总页面数
    unsigned long start;      // 虚拟地址起始
    unsigned long end;        // 虚拟地址结束
    void *pgmap_owner;        // device private 所有者
    unsigned long flags;
    struct page *fault_page;  // fault 页面
};
```

### 3.4 migrate_pages_stats（迁移统计）

```c
struct migrate_pages_stats {
    int nr_succeeded;       // 成功迁移的基页数
    int nr_failed_pages;    // 失败迁移的基页数
    int nr_thp_succeeded;   // THP 成功迁移数
    int nr_thp_failed;      // THP 失败迁移数
    int nr_thp_split;       // THP 拆分后迁移数
    int nr_split;           // 大页拆分总数
};
```

### 3.5 迁移相关标志位

```c
// 迁移条目中的 PFN 标志（编码在 swp_entry 中）
#define MIGRATE_PFN_VALID    (1UL << 0)  // PFN 有效
#define MIGRATE_PFN_MIGRATE  (1UL << 1)  // 需要迁移
#define MIGRATE_PFN_WRITE    (1UL << 3)  // 可写
#define MIGRATE_PFN_SHIFT    6

// dst folio->private 中记录的迁移状态
enum {
    PAGE_WAS_MAPPED  = BIT(0),   // 迁移前页面曾被映射
    PAGE_WAS_MLOCKED = BIT(1),   // 迁移前页面曾被 mlock
};
```

---

## 四、迁移四阶段全流程

页面迁移遵循 **Isolate → Unmap → Move → Remap** 四阶段模型：

```
阶段1: ISOLATE (隔离)
┌─────────────────────────────────────────────────────────────┐
│  从 LRU 链表或驱动中取出页面                                   │
│  ├─ folio_isolate_lru()         ← 普通 LRU 页面              │
│  ├─ folio_isolate_hugetlb()     ← HugeTLB 页面              │
│  └─ isolate_movable_ops_page()   ← balloon/zsmalloc 等      │
│                                                             │
│  增加 NR_ISOLATED_ANON / NR_ISOLATED_FILE 计数               │
│  页面加入迁移链表 (from list)                                  │
└─────────────────────────────────────────────────────────────┘
                           │
                           ▼
阶段2: UNMAP (解除映射)
┌─────────────────────────────────────────────────────────────┐
│  migrate_folio_unmap()                                       │
│  ├─ folio_trylock(src)          ← 锁定源页面                 │
│  ├─ 等待 writeback 完成（SYNC 模式）                          │
│  ├─ folio_get_anon_vma()        ← 防止 anon_vma 被释放       │
│  ├─ try_to_migrate(src, ...)    ← ★ 将 PTE 替换为 migration entry │
│  │     ├─ try_to_migrate_one()                               │
│  │     │    └─ set_pte_at(mm, addr, ptep,                    │
│  │     │              swp_entry_to_pte(swp_entry))           │
│  │     └─ 刷新 TLB                                            │
│  └─ 将迁移前状态记录到 dst->private                           │
└─────────────────────────────────────────────────────────────┘
                           │
                           ▼
阶段3: MOVE (移动内容)
┌─────────────────────────────────────────────────────────────┐
│  migrate_folio_move()                                        │
│  ├─ move_to_new_folio(dst, src, mode)  ← ★ 核心拷贝          │
│  │     ├─ [匿名页]  a_ops->migrate_folio() → 复制内容         │
│  │     ├─ [文件页]  mapping->a_ops->migrate_folio()           │
│  │     ├─ [swap]    swap's migrate_folio()                   │
│  │     └─ [fallback] fallback_migrate_folio()                │
│  ├─ folio_migrate_flags(newfolio, folio)  ← 复制页面标志      │
│  └─ folio_migrate_mapping()  ← ★ 替换 address_space 中的 folio │
│        ├─ folio_ref_freeze(src)    ← 冻结引用计数              │
│        ├─ xas_store(&xas, dst)     ← XArray 原子替换          │
│        └─ folio_ref_unfreeze(src)  ← 解冻                     │
└─────────────────────────────────────────────────────────────┘
                           │
                           ▼
阶段4: REMAP (重新映射)
┌─────────────────────────────────────────────────────────────┐
│  remove_migration_ptes(src, dst, flags)                      │
│  ├─ rmap_walk(dst, &rwc)         ← 遍历所有 VMA              │
│  │     └─ remove_migration_pte()                              │
│  │          ├─ mk_pte(new, vma->vm_page_prot)                │
│  │          ├─ 恢复 dirty/young/soft_dirty 标志               │
│  │          ├─ folio_add_anon_rmap_pte() / file_rmap         │
│  │          └─ set_pte_at(mm, addr, ptep, pte)  ← 指向新页   │
│  └─ 释放源页面                                                │
│        ├─ folio_put(src) / putback_movable_pages()           │
│        └─ 减少 NR_ISOLATED 计数                               │
└─────────────────────────────────────────────────────────────┘
```

### 4.1 时序图

```
    cpu0 (迁移线程)              cpu1 (访问进程)
    ═══════════════              ═══════════════
    folio_isolate_lru()
    folio_lock(src)
    try_to_migrate()
      set_pte(swp_entry) ────▶  PTE 变为 migration entry
      flush TLB

                               进程访问 → 缺页异常
                                 ├─ do_swap_page()
                                 ├─ pte_to_swp_entry()
                                 ├─ is_migration_entry() = true
                                 ├─ migration_entry_wait()
                                 │    ├─ folio_unlock(src)  ← 迁移线程释放锁
                                 │    ├─ folio_lock(src)    ← 等迁移线程释放
                                 │    └─ spin_unlock(ptl)
                                 └─ 返回重试缺页

    copy page content
    folio_migrate_mapping()
    remove_migration_ptes()
      set_pte(new_page) ──────▶  PTE 指向新页
    folio_unlock(src)

                               重试成功，获得新页
```

---

## 五、核心函数逐层解析

### 5.1 migrate_pages() — 顶层入口

```c
// mm/migrate.c:2064
int migrate_pages(struct list_head *from,       // 待迁移页面链表
                  new_folio_t get_new_folio,    // 分配目标页的回调
                  free_folio_t put_new_folio,   // 释放目标页的回调
                  unsigned long private,        // 传递给回调的私有数据
                  enum migrate_mode mode,       // 迁移模式
                  int reason,                   // 迁移原因
                  unsigned int *ret_succeeded); // [out] 成功迁移数
```

**执行流程**：

```
migrate_pages()
  │
  ├─ migrate_hugetlbs()           ← 先处理 HugeTLB 页面
  │
  ├─ [循环] 每次取 NR_MAX_BATCHED_MIGRATION 个页面
  │    │
  │    ├─ mode == MIGRATE_ASYNC ?
  │    │    └─ migrate_pages_batch()        ← 异步批处理
  │    │         ├─ 第一遍：try_to_migrate 所有 folio
  │    │         ├─ 从 from 移动到 unmap_folios (成功) 或重试
  │    │         ├─ migrate_folios_move()   ← 移动已 unmap 的
  │    │         └─ 最多 retry NR_MAX_MIGRATE_PAGES_RETRY (10) 次
  │    │
  │    └─ mode != MIGRATE_ASYNC ?
  │         └─ migrate_pages_sync()         ← 同步逐页迁移
  │              ├─ 一次只处理一个 folio (避免死锁)
  │              ├─ migrate_folio_unmap()   ← unmap + 分配目标
  │              └─ migrate_folio_move()    ← 移动 + remap
  │
  ├─ [处理 split_folios]            ← 拆分后的大页重试
  │    └─ migrate_pages_batch(..., MIGRATE_ASYNC, ..., retry=1)
  │
  └─ 统计: count_vm_events(PGMIGRATE_SUCCESS/FAIL, ...)
```

### 5.2 migrate_pages_batch() — 批量异步迁移

```c
// mm/migrate.c:1775
static int migrate_pages_batch(struct list_head *from, ...)
```

**核心循环逻辑**:

```
for (pass = 0; pass < nr_pass && retry; pass++):
  for each folio in from:
    │
    ├─ [预先拆分] try_split_folio() ← deferred split list 上的大页
    │
    ├─ [不支持迁移的大页] try_split_folio() → 拆成 order-0
    │
    ├─ [refcount == 1 且非 movable_ops]
    │    └─ 直接释放 (页面已无人引用)
    │
    └─ migrate_folio_unmap(get_new_folio, ..., folio, &dst, ...)
         │
         ├─ 返回 0:      unmap 成功 → 移到 unmap_folios, dst 移到 dst_folios
         ├─ 返回 -ENOMEM: 内存不足 → 拆分大页或终止
         ├─ 返回 -EAGAIN: 重试
         └─ 返回 other:  永久失败 → 移到 ret_folios

  [unmap 完成后]:
  migrate_folios_move(unmap_folios, dst_folios, ...)
    └─ for each pair (src, dst):
         migrate_folio_move(src, dst)
           ├─ move_to_new_folio()       ← 复制内容
           ├─ remove_migration_ptes()   ← 更新页表
           └─ folio_put(dst) / 释放 src
```

### 5.3 migrate_folio_unmap() — 解除映射 + 分配目标

```c
// mm/migrate.c:1200
static int migrate_folio_unmap(new_folio_t get_new_folio,
        free_folio_t put_new_folio, unsigned long private,
        struct folio *src, struct folio **dstp,
        enum migrate_mode mode, struct list_head *ret)
```

**状态机**：

```
                    ┌─────────────────┐
                    │   开始 unmap     │
                    └────────┬────────┘
                             │
                    ┌────────▼────────┐
                    │ get_new_folio() │ ← 分配目标页
                    │  (如 alloc_     │
                    │  migration_     │
                    │  target)        │
                    └────────┬────────┘
                             │ 失败 → 返回 -ENOMEM
                    ┌────────▼────────┐
                    │ folio_trylock   │
                    │ (src)           │
                    └────────┬────────┘
                  失败/ASYNC → 返回 -EAGAIN
                             │
                    ┌────────▼────────┐
                    │ Mlocked?        │── 是 → PAGE_WAS_MLOCKED
                    │ Writeback?      │── 是 → ASYNC/SYNC_LIGHT: -EBUSY
                    │                 │       SYNC: 等待完成
                    └────────┬────────┘
                             │
                    ┌────────▼────────┐
                    │ folio_get_anon  │ ← 匿名页：持有 anon_vma
                    │ _vma(src)       │
                    └────────┬────────┘
                             │
                    ┌────────▼────────┐
                    │ folio_trylock   │
                    │ (dst)           │
                    └────────┬────────┘
                  失败 → 返回 -EAGAIN
                             │
                    ┌────────▼────────┐
                    │ movable_ops?    │── 是 → 记录状态，返回 0
                    │                 │   (move 阶段由驱动完成)
                    └────────┬────────┘
                             │ 否
                    ┌────────▼────────┐
                    │ try_to_migrate  │ ← ★ 替换所有 PTE
                    │ (src, ttu)      │   为 migration entry
                    └────────┬────────┘
                             │
                    ┌────────▼────────┐
                    │ !mapped(src)?   │── 是 → 记录状态，返回 0
                    │                 │   (进入 move 阶段)
                    └────────┬────────┘
                             │ 否 (仍被映射)
                            返回 -EAGAIN
```

### 5.4 move_to_new_folio() — 页面内容迁移的多态分发

```c
// mm/migrate.c:1081
static int move_to_new_folio(struct folio *dst, struct folio *src,
                             enum migrate_mode mode)
{
    struct address_space *mapping = folio_mapping(src);

    if (!mapping)
        rc = migrate_folio(mapping, dst, src, mode);    // 匿名页：直接复制
    else if (mapping_inaccessible(mapping))
        rc = -EOPNOTSUPP;
    else if (mapping->a_ops->migrate_folio)             // 文件系统有专用回调
        rc = mapping->a_ops->migrate_folio(mapping, dst, src, mode);
    else
        rc = fallback_migrate_folio(mapping, dst, src, mode);  // 通用回退
}
```

各文件系统的 `migrate_folio` 回调：

| 文件系统/类型 | migrate_folio 实现 | 说明 |
|-------------|-------------------|------|
| 匿名页 (swap) | `migrate_folio()` | `folio_mc_copy()` + `folio_migrate_mapping()` |
| ext4 | `ext4_migrate_folio()` → `buffer_migrate_folio_norefs()` | 处理 buffer_head |
| xfs | `xfs_vm_migrate_folio()` → `buffer_migrate_folio_norefs()` | 处理 buffer_head |
| btrfs | `btrfs_migrate_folio()` | BTRFS 特定处理 |
| shmem/tmpfs | `migrate_folio()` | 通用回退 |
| NFS | `nfs_migrate_folio()` | NFS 特定处理 |
| 其他 | `fallback_migrate_folio()` | 通用回退 |

### 5.5 folio_migrate_mapping() — 原子替换 address_space

```c
// mm/migrate.c:560
int folio_migrate_mapping(struct address_space *mapping,
        struct folio *newfolio, struct folio *folio, int extra_count)
```

这是迁移中**最关键**的原子操作：

```
folio_migrate_mapping()
  │
  ├─ 检查引用计数：refcount == expected_count ?
  │    └─ 不匹配 → 返回 -EAGAIN
  │
  └─ __folio_migrate_mapping()
       │
       ├─ [swapcache] swap_cluster_get_and_lock_irq()
       │    └─ 获取 swap cluster 锁
       │
       ├─ [pagecache] xas_lock_irq(&xas)
       │    └─ 获取 XArray (i_pages) 锁
       │
       ├─ folio_ref_freeze(src, expected_count)  ← 冻结引用计数
       │
       ├─ folio_unqueue_deferred_split(src)      ← 从延迟拆分队列移除
       │
       ├─ newfolio->index = folio->index          ← 复制索引
       ├─ newfolio->mapping = folio->mapping       ← 复制映射
       │
       ├─ [swapcache]
       │    ├─ folio_set_swapcache(newfolio)
       │    ├─ newfolio->private = folio->private
       │    └─ __swap_cache_replace_folio()        ← 替换 swap cache
       │
       ├─ [pagecache] xas_store(&xas, newfolio)    ← ★ 原子替换 radix tree
       │
       ├─ folio_ref_unfreeze(src, expected_count - nr)
       │
       └─ [跨 zone 迁移] 更新 per-zone 统计
            ├─ NR_FILE_PAGES, NR_SHMEM, NR_SWAPCACHE
            └─ NR_FILE_DIRTY, NR_ZONE_WRITE_PENDING
```

### 5.6 folio_migrate_flags() — 页面标志迁移

```c
// mm/migrate.c:746
void folio_migrate_flags(struct folio *newfolio, struct folio *folio)
```

迁移的标志包括：

```
源页面标志                    目标页面标志
─────────────────────────────────────────
PG_referenced       ──▶      PG_referenced
PG_uptodate         ──▶      PG_uptodate
PG_active           ──▶      PG_active (+ PG_unevictable → cleared)
PG_unevictable      ──▶      PG_unevictable
PG_workingset       ──▶      PG_workingset
PG_checked          ──▶      PG_checked
PG_mappedtodisk     ──▶      PG_mappedtodisk  (anon_exclusive)
PG_dirty            ──▶      PG_dirty
PG_young            ──▶      PG_young
PG_idle             ──▶      PG_idle
last_cpupid         ──▶      last_cpupid (NUMA balancing 模式下可能重置)
```

---

## 六、迁移条目机制 (Migration Entry)

### 6.1 什么是迁移条目

迁移期间，PTE 被替换为一个特殊的 swap entry，其格式为：

```
ARM64 PTE (迁移条目):
┌────────────────────────────────────────────────────────────┐
│  swp_type = SWP_MIGRATION_WRITE 或 SWP_MIGRATION_READ      │
│  swp_offset = PFN (物理页帧号)                              │
└────────────────────────────────────────────────────────────┘
```

### 6.2 设置迁移条目 — try_to_migrate()

```c
// mm/rmap.c
try_to_migrate(folio, ttu_flags)
  └─ rmap_walk(folio, &rwc)
       └─ try_to_migrate_one()
            ├─ 构造 migration entry:
            │    entry = make_readable_migration_entry(
            │              folio_pfn(folio));
            │    swp_pte = swp_entry_to_pte(entry);
            │
            ├─ 如果是脏页:
            │    entry = make_writable_migration_entry(
            │              folio_pfn(folio));
            │
            └─ set_pte_at(mm, addr, ptep, swp_pte);
```

### 6.3 等待迁移完成 — migration_entry_wait()

```c
// mm/migrate.c:479
void migration_entry_wait(struct mm_struct *mm, pmd_t *pmd,
                          unsigned long address)
{
    ptep = pte_offset_map_lock(mm, pmd, address, &ptl);
    pte = ptep_get(ptep);
    entry = pte_to_swp_entry(pte);
    if (is_migration_entry(entry))
        migration_entry_wait_on_locked(entry, ptl);
    // ...
}

void migration_entry_wait_on_locked(swp_entry_t entry, spinlock_t *ptl)
{
    folio = pfn_swap_entry_folio(entry);
    spin_unlock(ptl);              // 释放页表锁
    folio_lock(folio);             // 阻塞直到迁移完成
    folio_unlock(folio);
    spin_lock(ptl);                // 重新获取页表锁
}
```

**关键设计**：迁移线程在完成 `remove_migration_ptes()` 后释放 folio lock，等待者通过 `folio_lock()` 阻塞直到此时。

### 6.4 恢复迁移条目 — remove_migration_ptes()

```c
// mm/migrate.c:453
void remove_migration_ptes(struct folio *src, struct folio *dst, int flags)
{
    // 遍历所有映射了 src 的 VMA
    rmap_walk(dst, &rwc)
      └─ remove_migration_pte()
           ├─ pte = mk_pte(new_page, vma->vm_page_prot);
           ├─ 恢复标志:
           │    ├─ is_migration_entry_young()?    → pte_mkold()
           │    ├─ is_migration_entry_dirty()?    → pte_mkdirty()
           │    ├─ pte_swp_soft_dirty()?          → pte_mksoft_dirty()
           │    └─ pte_swp_uffd_wp()?             → pte_mkuffd_wp()
           ├─ [匿名页] folio_add_anon_rmap_pte()
           ├─ [文件页] folio_add_file_rmap_pte()
           ├─ set_pte_at(mm, addr, ptep, pte);
           ├─ [mlocked] mlock_drain_local()
           └─ update_mmu_cache(vma, addr, ptep);
}
```

---

## 七、设备页面迁移 (Device Migration)

### 7.1 概述

设备页面迁移用于 GPU 等设备的内存页面迁移，典型场景：
- GPU 显存溢出到系统内存（device private memory）
- GPU 与 CPU 之间的数据迁移（device coherent memory）
- RDMA 等设备内存的管理

### 7.2 migrate_vma 三步 API

```c
// mm/migrate_device.c

// 步骤1: 收集页面并锁定
int migrate_vma_setup(struct migrate_vma *args);
//   ├─ mmap_read_lock(mm)
//   ├─ walk_page_range() → migrate_vma_collect_pmd()
//   │    └─ 填充 args->src[] 为当前 PFN 或 MIGRATE_PFN_MIGRATE
//   └─ migrate_vma_pages(args)  ← 内部调用
//        └─ 对于可迁移页: try_to_migrate() + 分配目标

// 步骤2: 驱动完成数据拷贝 (调用方实现)
//   └─ 驱动从 src 页复制数据到 dst 页

// 步骤3: 完成迁移
void migrate_vma_finalize(struct migrate_vma *args);
//   └─ 更新页表，释放源页
```

### 7.3 PFN 数组编码

```c
// src[] 数组中的条目含义:
// MIGRATE_PFN_VALID | pfn       : 有效的物理页
// MIGRATE_PFN_MIGRATE            : 需要迁移的空洞
// 0                              : 跳过

// dst[] 数组中的条目:
// MIGRATE_PFN_VALID | pfn       : 目标物理页已分配
// 0                              : 未分配目标
```

### 7.4 设备迁移流程

```
用户空间驱动程序
  │
  ├─ migrate_vma_setup(&args)
  │    ├─ 遍历 VMA 页表
  │    ├─ 填充 src[] 数组
  │    ├─ 锁定源页面
  │    └─ 设置 migration entry
  │
  ├─ [驱动] 对每个需要迁移的页面:
  │    ├─ 在 device memory 中分配目标页
  │    ├─ 填充 dst[i] = migrate_pfn(pfn) | MIGRATE_PFN_VALID
  │    └─ 从 src 页复制数据到 device
  │
  ├─ migrate_vma_pages(&args)
  │    └─ 迁移已分配目标页的页面
  │
  ├─ [驱动] device MMU 更新: 指向新页面
  │
  └─ migrate_vma_finalize(&args)
       ├─ remove_migration_ptes()
       └─ 释放源页面
```

---

## 八、Compaction 与迁移的协作

### 8.1 Compaction 如何使用迁移

```c
// mm/compaction.c:2647
// compact_zone() 中的迁移阶段:
nr_migratepages = cc->nr_migratepages;
err = migrate_pages(&cc->migratepages,
                    compaction_alloc,       // ← 分配目标页
                    compaction_free,        // ← 释放未使用的目标页
                    (unsigned long)cc,
                    cc->mode,               // MIGRATE_ASYNC 或 MIGRATE_SYNC_LIGHT
                    MR_COMPACTION,          // ← 迁移原因: 压缩
                    &nr_succeeded);
```

### 8.2 compaction_alloc() — 为目标页选择位置

```c
// mm/compaction.c
static struct folio *compaction_alloc(struct folio *src, unsigned long data)
{
    struct compact_control *cc = (struct compact_control *)data;

    // 在 free scanner 位置分配目标页
    // 目标: 将页面从 migrate scanner 位置迁移到 free scanner 位置
    // 以达到压缩效果: 可移动页集中于低地址,空闲页集中于高地址
}
```

### 8.3 Compaction 与迁移的交互

```
compact_zone()
  │
  ├─ [扫描阶段] isolate_migratepages()
  │    ├─ 从 migrate scanner 位置隔离可移动页
  │    └─ 加入 cc->migratepages 链表
  │
  ├─ [迁移阶段] migrate_pages(..., MR_COMPACTION)
  │    │
  │    ├─ migrate_folio_unmap()
  │    │    ├─ compaction_alloc() ← 从 free scanner 位置分配
  │    │    └─ try_to_migrate()   ← 设置 migration entry
  │    │
  │    └─ migrate_folio_move()
  │         ├─ move_to_new_folio()           ← 复制到空闲区域
  │         └─ remove_migration_ptes()        ← 更新页表
  │
  └─ [释放] 源页返回 buddy allocator
       └─ 空闲页集中在 free scanner 方向 → 形成连续大块
```

---

## 九、NUMA 自动平衡迁移

### 9.1 触发条件

NUMA balancing 在缺页异常中触发：

```
handle_pte_fault()
  └─ do_numa_page() / do_huge_pmd_numa_page()
       ├─ 检测到 PROT_NONE (NUMA hinting fault)
       ├─ 判断页面是否 misplaced (CPU 节点 != 页面节点)
       └─ migrate_misplaced_folio()
```

### 9.2 migrate_misplaced_folio()

```c
// mm/migrate.c:2724
int migrate_misplaced_folio(struct folio *folio, int node)
{
    LIST_HEAD(migratepages);

    // 隔离页面
    folio_isolate_lru(folio);
    list_add(&folio->lru, &migratepages);

    // 执行迁移
    nr_remaining = migrate_pages(&migratepages,
                    alloc_misplaced_dst_folio,  // 分配目标 NUMA 节点页
                    NULL, node,                  // node = 目标节点
                    MIGRATE_ASYNC,               // 异步迁移
                    MR_NUMA_MISPLACED,
                    NULL);
}
```

### 9.3 NUMA 迁移的特殊处理

```
NUMA 迁移的特点:
├─ 使用 MIGRATE_ASYNC 模式 (不影响性能)
├─ 迁移失败不阻塞进程 (跳过本次迁移)
├─ 大页迁移失败不拆分 (nosplit = true)
├─ 重置 cpupid (memory tiering 模式)
└─ folio_set_owner_migrate_reason(dst, MR_NUMA_MISPLACED)
```

---

## 十、内存热插拔迁移

### 10.1 do_migrate_range()

```c
// mm/memory_hotplug.c
static void do_migrate_range(unsigned long start_pfn, unsigned long end_pfn)
{
    // 遍历目标内存范围内的所有已分配页面
    for (pfn = start_pfn; pfn < end_pfn; pfn++) {
        folio = pfn_folio(pfn);
        // 隔离并加入迁移链表
        isolate_folio_to_list(folio, &source);
    }

    // 批量迁移 (同步模式，必须全部迁移成功)
    migrate_pages(&source, alloc_migration_target, NULL,
                  (unsigned long)&mtc, MIGRATE_SYNC,
                  MR_MEMORY_HOTPLUG, NULL);

    // 失败页面放回
    putback_movable_pages(&source);
}
```

---

## 十一、HugeTLB 迁移

### 11.1 unmap_and_move_huge_page()

```c
// mm/migrate.c:1434
static int unmap_and_move_huge_page(new_folio_t get_new_folio, ...)
```

与普通页面的区别：

```
HugeTLB 迁移的特殊性:
├─ 使用 hugetlbfs 专用的 isolate/putback
│    ├─ folio_isolate_hugetlb()   ← 从 hugetlb 池隔离
│    └─ folio_putback_hugetlb()   ← 放回 hugetlb 池
│
├─ 映射锁定:
│    ├─ [共享映射] hugetlb_folio_mapping_lock_write()
│    │    └─ 防止 huge_pmd_unshare() 的竞争
│    └─ TTU_RMAP_LOCKED 标志传递
│
├─ 映射替换:
│    └─ try_to_migrate() → 设置 huge migration entry
│         └─ PMD 级别的 migration entry (如果支持 THP migration)
│
├─ 状态转移:
│    └─ move_hugetlb_state(src, dst, reason)
│
└─ 引用计数验证:
     └─ folio_ref_count(src) == 1 → 页面已被释放，直接完成
```

---

## 十二、完整调用链一览

### 12.1 Compaction 触发的迁移 (最常见)

```
kswapd / kcompactd / 直接分配路径
  │
  └─ compact_zone(cc)
       ├─ isolate_migratepages(cc)
       │    └─ folio_isolate_lru(folio)
       │         └─ list_add(&folio->lru, &cc->migratepages)
       │
       └─ migrate_pages(&cc->migratepages,
                         compaction_alloc, compaction_free, cc,
                         mode, MR_COMPACTION, &nr_succeeded)
            │
            ├─ migrate_pages_batch()  [MIGRATE_ASYNC]
            │    │
            │    ├─ migrate_folio_unmap(get_new_folio=compaction_alloc, ...)
            │    │    ├─ compaction_alloc(src, cc)
            │    │    │    └─ __folio_alloc(GFP_..., free_scanner位置)
            │    │    ├─ folio_lock(src)
            │    │    ├─ try_to_migrate(src, TTU_BATCH_FLUSH)
            │    │    │    └─ rmap_walk()
            │    │    │         └─ try_to_migrate_one()
            │    │    │              └─ set_pte_at(swp_entry)
            │    │    └─ __migrate_folio_record(dst, PAGE_WAS_MAPPED, anon_vma)
            │    │
            │    └─ migrate_folios_move(unmap_folios, dst_folios, ...)
            │         └─ migrate_folio_move(src, dst)
            │              ├─ move_to_new_folio(dst, src, mode)
            │              │    └─ mapping->a_ops->migrate_folio()
            │              │         ├─ folio_mc_copy(dst, src)
            │              │         └─ folio_migrate_mapping(mapping, dst, src, 0)
            │              │              └─ __folio_migrate_mapping()
            │              │                   ├─ folio_ref_freeze(src)
            │              │                   ├─ xas_store(&xas, dst)  ← ★ 原子
            │              │                   └─ folio_ref_unfreeze(src)
            │              ├─ folio_migrate_flags(dst, src)
            │              ├─ folio_add_lru(dst)
            │              ├─ remove_migration_ptes(src, dst, 0)
            │              │    └─ rmap_walk()
            │              │         └─ remove_migration_pte()
            │              │              └─ set_pte_at(new_page)
            │              └─ folio_put(dst)  / list_del(&src->lru)
            │
            └─ [重试逻辑] 失败页面 retry up to 10 次
```

### 12.2 sys_move_pages 触发的迁移

```
sys_move_pages(nr_pages, pages, nodes, status, flags)
  └─ do_pages_move(mm, task_nodes, nr_pages, pages, nodes, status, flags)
       ├─ [循环] add_folio_for_migration(mm, p, node, &pagelist, migrate_all)
       │    ├─ mmap_read_lock(mm)
       │    ├─ vma_lookup(mm, addr)
       │    ├─ folio_walk_start(&fw, vma, addr)
       │    └─ __add_folio_for_migration()
       │         ├─ 检查是否已在目标节点 → 跳过
       │         ├─ [hugetlb] folio_isolate_hugetlb()
       │         └─ [普通] folio_isolate_lru() → list_add
       │
       └─ do_move_pages_to_node(&pagelist, node)
            └─ migrate_pages(&pagelist,
                             alloc_migration_target, NULL,
                             &mtc, MIGRATE_SYNC, MR_SYSCALL, NULL)
```

### 12.3 NUMA Balancing 触发的迁移

```
缺页异常
  └─ handle_pte_fault()
       └─ do_numa_page()
            └─ migrate_misplaced_folio(folio, target_node)
                 └─ migrate_misplaced_folio_prepare()
                 └─ migrate_pages(&migratepages,
                                  alloc_misplaced_dst_folio,
                                  NULL, target_node,
                                  MIGRATE_ASYNC,
                                  MR_NUMA_MISPLACED, NULL)
```

---

## 十三、调试与观测

### 13.1 tracepoint

```bash
# 迁移开始/结束
echo 1 > /sys/kernel/debug/tracing/events/migrate/mm_migrate_pages_start/enable
echo 1 > /sys/kernel/debug/tracing/events/migrate/mm_migrate_pages/enable

# 查看迁移事件
cat /sys/kernel/debug/tracing/trace_pipe

# 输出示例:
# mm_migrate_pages: mode=2 reason=1 nr_succeeded=512 nr_failed=0 ...
```

### 13.2 /proc/vmstat 计数器

```bash
grep -E "pgmigrate|compact_migrate" /proc/vmstat

# pgmigrate_success      : 成功迁移的页面数
# pgmigrate_fail         : 失败迁移的页面数
# compact_migrate_scanned: compaction 扫描的页面数
# thp_migration_success  : THP 迁移成功数
# thp_migration_fail     : THP 迁移失败数
# thp_migration_split    : THP 拆分后迁移数
```

### 13.3 常见迁移失败原因

| 返回值 | 含义 | 常见原因 |
|--------|------|---------|
| `-EAGAIN` | 临时失败，可重试 | 页面被锁定、引用计数变化、folio_trylock 失败 |
| `-ENOMEM` | 内存不足 | 无法分配目标页面 |
| `-EBUSY` | 永久失败 | writeback 中 (非 SYNC 模式)、页面状态异常 |
| `-EIO` | I/O 错误 | 文件系统迁移回调失败 |
| `-EOPNOTSUPP` | 不支持 | mapping_inaccessible (如某些设备映射) |
| `-EFAULT` | 地址错误 | 零页、设备页、已释放页 |

### 13.4 关键配置选项

```
CONFIG_MIGRATION=y            ← 页面迁移总开关
CONFIG_COMPACTION=y           ← 内存压缩（依赖 MIGRATION）
CONFIG_NUMA_BALANCING=y       ← NUMA 自动平衡（依赖 MIGRATION）
CONFIG_MEMORY_HOTPLUG=y       ← 内存热插拔（依赖 MIGRATION）
CONFIG_ARCH_ENABLE_THP_MIGRATION=y  ← THP 迁移支持
CONFIG_DEVICE_PRIVATE=y       ← 设备私有内存迁移
```

---

## 附录A：页面迁移状态机总图

```
                         ┌──────────────────┐
                         │   页面在 LRU 上    │
                         │ (正常状态)         │
                         └────────┬─────────┘
                                  │ folio_isolate_lru()
                         ┌────────▼─────────┐
                         │   已隔离 (Isolated)│
                         │ NR_ISOLATED++    │
                         └────────┬─────────┘
                                  │ migrate_folio_unmap()
                         ┌────────▼─────────┐
                         │  已解除映射       │
                         │ PTE=swp_entry    │
                         │ (迁移中)          │
                         └────────┬─────────┘
                                  │
                    ┌─────────────┼─────────────┐
                    │ migrate_folio_move()      │ 失败
                    ▼                           ▼
          ┌──────────────────┐        ┌──────────────────┐
          │   迁移成功        │        │   迁移失败        │
          │ page → new page  │        │                  │
          │ 更新 PTE         │        │ putback/retry    │
          │ 释放源页面        │        │ 恢复 PTE         │
          └──────────────────┘        └──────────────────┘
```

## 附录B：关键文件索引

| 文件 | 内容 |
|------|------|
| `mm/migrate.c` | 核心迁移逻辑 (2700+ 行) |
| `mm/migrate_device.c` | 设备页面迁移 |
| `mm/compaction.c` | 内存压缩（迁移的主要调用者） |
| `mm/memory_hotplug.c` | 热插拔迁移 (`do_migrate_range`) |
| `mm/mempolicy.c` | mbind/move_pages NUMA 策略迁移 |
| `mm/rmap.c` | `try_to_migrate()`, `try_to_migrate_one()` |
| `include/linux/migrate.h` | 迁移 API 头文件 |
| `include/linux/migrate_mode.h` | 迁移模式和原因枚举 |
| `include/trace/events/migrate.h` | 迁移 tracepoint 定义 |
| `include/linux/swapops.h` | migration entry 的构造/识别宏 |

---

## 附录C：嵌入式系统视角

### C.1 嵌入式设备为什么几乎没有页面迁移

嵌入式系统通常满足以下特征，导致页面迁移机制**编译时可能开启，但运行时几乎从不触发**：

```
典型嵌入式系统配置:
├─ UMA 架构 (单内存节点)            → 无 NUMA balancing
├─ RAM 焊死在 PCB 上                → 无 memory hotplug
├─ 无 THP (透明大页)                 → 不需要 order-9+ 分配
│   └─ 通常设置 CONFIG_TRANSPARENT_HUGEPAGE=n
├─ 无 kcompactd                     → 无后台压缩
│   └─ 通常设置 CONFIG_COMPACTION=n
├─ 使用 CMA 做 DMA 预留               → 唯一可能触发迁移的场景
│   └─ 但 CMA 区域通常仅被 MOVABLE 类型使用
│   └─ 非 MOVABLE 分配不会 fallback 到 CMA
└─ 无 GPU/FPGA 的 device private memory → 无设备页面迁移
```

### C.2 CONFIG_MIGRATION 的依赖关系

```kconfig
config MIGRATION
    bool "Page migration"
    default y
    depends on (NUMA || ARCH_ENABLE_MEMORY_HOTREMOVE || COMPACTION || CMA) && MMU
```

| 场景 | 嵌入式典型值 | MIGRATION | 实际发生迁移？ |
|------|:----------:|:---------:|:------------:|
| 极简无 CMA 系统 | CMA=n | **n** | 编译时就不存在 |
| 有 CMA (DMA/视频) | CMA=y | **y** (随 CMA 开启) | 极少，CMA 区域内 MOVABLE 页被挤出时 |
| 有 CMA + COMPACTION | CMA=y, COMPACTION=y | **y** | 可能，order-3+ 分配失败时 |

### C.3 CMA 场景下的迁移触发时机

即使 `CONFIG_MIGRATION=y` 随 CMA 打开，实际迁移也只在以下时机触发：

```
cma_alloc()
  └─ alloc_contig_range()
       └─ __alloc_contig_migrate_range()
            └─ migrate_pages(..., MR_CONTIG_RANGE)
                 ↑ 仅当 CMA 区域内存在 MOVABLE 页面
                 │ 且当前分配不是 MOVABLE 类型时
```

但嵌入式系统中，CMA 区域通常仅被**用户态页面（MOVABLE）**使用，而非 movable 的分配（内核数据结构等）不会 fallback 到 CMA 区域，因此迁移概率极低。

**验证方法**：

```bash
# 在嵌入式设备上检查:
grep pgmigrate /proc/vmstat
# 如果输出为 0，说明从未发生过页面迁移

cat /sys/kernel/debug/tracing/events/migrate/mm_migrate_pages/enable 2>/dev/null
# 或直接检查是否有 tracepoint 事件日志
```

### C.4 嵌入式 vs 服务器对比

```
                    嵌入式 (ARM Cortex-A)         服务器 (x86_64/ARM64)
                    ─────────────────────        ─────────────────────
CONFIG_MIGRATION    可能 y (随 CMA)              必 y
CONFIG_COMPACTION   通常 n                        必 y
CONFIG_NUMA         通常 n                        必 y
THP                 通常 n                        必 y
pgmigrate_success   ≈ 0                          数百 ~ 数十万/秒
主要迁移原因        无 (或极少 MR_CONTIG_RANGE)    MR_COMPACTION, MR_NUMA_MISPLACED
```

### C.5 结论

> **嵌入式设备中，页面迁移的代码路径在绝大多数情况下是死代码（dead code）**。`CONFIG_MIGRATION` 可能因 `CMA` 而被连带编译进内核，但实际的 `migrate_pages()` 调用几乎不会发生。对于学习目的，理解页面迁移机制有助于理解 CMA 的工作原理和内存管理的完整图景；但在嵌入式系统的调优和调试中，不需要关注页面迁移相关的计数器或性能指标。
