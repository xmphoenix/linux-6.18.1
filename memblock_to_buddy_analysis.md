# Linux ARM64 内存初始化：memblock → Buddy Allocator 过渡分析

> 环境：Linux 6.18.1 / ARM64 / QEMU 1GB 内存

---

## 1. 核心问题：free_area 是在哪里填充的？

`free_area_init()` → `zone_init_free_lists()` 只是建立了**空骨架**（`INIT_LIST_HEAD` + `nr_free = 0`），真正把页面挂进去是在 `memblock_free_all()` 调用链中完成的。

---

## 2. 完整调用链

```
mm_core_init()                              [mm/mm_init.c]
  └─ memblock_free_all()                    [mm/memblock.c]
       ├─ free_unused_memmap()              清理多余的 memmap
       ├─ reset_all_zones_managed_pages()   清零 managed_pages 计数
       └─ free_low_memory_core_early()
            └─ __free_memory_core(start, end)
                 └─ __free_pages_memory(start_pfn, end_pfn)   ← 按对齐计算 order
                      └─ memblock_free_pages(page, pfn, order) [mm/mm_init.c:2483]
                           └─ __free_pages_core(page, order, MEMINIT_EARLY)
                                └─ __free_pages_ok(page, order, FPI_TO_TAIL)
                                     └─ free_one_page(zone, page, pfn, order, flags)
                                          └─ split_large_buddy(zone, page, pfn, order, fpi)
                                               └─ __free_one_page(page, pfn, zone, order, mt, fpi)
                                                    ├─ [buddy merge 循环] 向上合并找伙伴
                                                    └─ __add_to_free_list(page, zone, order, mt, to_tail)
                                                         └─ list_add[_tail](&page->buddy_list,
                                                                    &zone->free_area[order].free_list[mt])
                                                            area->nr_free++
                                                            ★ 真正插入 free_area
```

---

## 3. 关键函数说明

### 3.1 `__free_pages_memory()` — 按对齐分配 order（memblock.c）

```c
static void __init __free_pages_memory(unsigned long start, unsigned long end)
{
    int order;
    while (start < end) {
        if (start)
            order = min_t(int, MAX_PAGE_ORDER, __ffs(start)); // 按 PFN 对齐找最大 order
        else
            order = MAX_PAGE_ORDER;

        while (start + (1UL << order) > end)
            order--;  // 范围不够则降 order

        memblock_free_pages(pfn_to_page(start), start, order);
        start += (1UL << order);
    }
}
```

**关键规律**：order-N 块要求 `PFN % (1 << N) == 0`，即自然对齐约束决定了每个块的 order，**不是全部都用最大 order**。

### 3.2 `free_one_page()` — 中间层（page_alloc.c:1528）

注意：`free_one_page()` **不直接**调用 `__free_one_page()`，中间经过 `split_large_buddy()`：

```c
free_one_page(zone, page, pfn, order, fpi_flags)
  └─ spin_lock_irqsave(&zone->lock, flags)     获取 zone 锁
  └─ split_large_buddy(zone, page, pfn, order, fpi_flags)
       // 若 order > pageblock_order，拆成多个 pageblock_order 大小的块
       └─ __free_one_page(page, pfn, zone, order, mt, fpi)
```

### 3.3 `__free_one_page()` — buddy 合并核心（page_alloc.c:940）

```c
while (order < MAX_PAGE_ORDER) {
    buddy = find_buddy_page_pfn(page, pfn, order, &buddy_pfn);
    if (!buddy) goto done_merging;

    __del_page_from_free_list(buddy, zone, order, buddy_mt); // 从链表摘出 buddy
    combined_pfn = buddy_pfn & pfn;  // 合并后的起始 PFN
    pfn = combined_pfn;
    order++;  // 升阶
}
done_merging:
    set_buddy_order(page, order);       // 记录 order 到 page->private
    __add_to_free_list(page, zone, order, migratetype, to_tail); // ★ 插入 free_area
```

**buddy 伙伴公式**：`buddy_pfn = pfn ^ (1 << order)`

### 3.4 `__add_to_free_list()` — 最终插入（page_alloc.c:803）

```c
static inline void __add_to_free_list(struct page *page, struct zone *zone,
                                      unsigned int order, int migratetype, bool tail)
{
    struct free_area *area = &zone->free_area[order];  // 选对应 order 的槽

    if (tail)
        list_add_tail(&page->buddy_list, &area->free_list[migratetype]);
    else
        list_add(&page->buddy_list, &area->free_list[migratetype]);
    area->nr_free++;
}
```

---

## 4. memblock_free_all() 完成后 Buddy 分配器是否可用？

**是的，buddy 分配器在 `memblock_free_all()` 返回后即可使用。**

```c
void __init mm_core_init(void)
{
    build_all_zonelists(NULL);    // ① zonelist 建立（分配 fallback 链）
    ...
    memblock_free_all();          // ② buddy 可用：free_area 已填充
    mem_init();                   // ③ 体系结构收尾（ARM64 无实质内容）
    kmem_cache_init();            // ④ slab 建立（依赖 buddy）
    ...
}
```

| 分配器 | 可用时机 |
|--------|----------|
| memblock | 早期 boot，`memblock_free_all()` 之前 |
| buddy (`alloc_pages`) | `memblock_free_all()` 之后 |
| slab (`kmalloc`) | `kmem_cache_init()` 之后 |

---

## 5. 为什么不是全部都放进最大 order？

以 QEMU 1GB 内存为例，内核占用 PFN `[0x40000, 0x40800)`，空闲从 `0x40800` 开始：

```
PFN 0x40800 = 0b 0100 0000 1000 0000 0000 0000
__ffs(0x40800) = 11  → order = min(10, 11) = 10 ？

实际：0x40800 % (1<<10) = 0x40800 % 0x400 = 0
      0x40800 % (1<<11) = 0x40800 % 0x800 = 0  ← 满足 order-10
```

所以从 0x40800 开始确实可以分配 order-10 的块，这也解释了 `/proc/buddyinfo` 中看到 223 个 order-10 块的原因。边界处（内核末尾附近）的零散 PFN 因对齐不满足大 order 而产生小 order 块。

---

## 6. 数据结构回顾

```
struct zone {
    struct free_area free_area[NR_PAGE_ORDERS];  // [11] 个 order 槽
};

struct free_area {
    struct list_head free_list[MIGRATE_TYPES];  // [6] 个迁移类型链表
    unsigned long    nr_free;                    // 本 order 空闲块总数
};
```

`zone->free_area[order].free_list[migratetype]` 是最终存放空闲页的位置，由 `__add_to_free_list()` 写入，由 `__rmqueue_smallest()` 读取（分配时）。

---

## 7. 今日讨论总结

| 问题 | 结论 |
|------|------|
| free_area 在哪填充 | `memblock_free_all()` 调用链最底层的 `__add_to_free_list()` |
| `free_one_page` 是否直接调用 `__free_one_page` | 否，中间有 `split_large_buddy()` |
| memblock_free_all 后 buddy 是否可用 | 是，buddy 可用；slab 要等 `kmem_cache_init()` |
| 从 memblock 转 free_area 是否全用最大 order | 否，由 PFN 自然对齐约束决定每块的 order |
