# Linux Kernel Memblock 子系统深度分析

> 基于 Linux 6.18.1，源码路径：`mm/memblock.c`，`include/linux/memblock.h`

---

## 目录

<details>
<summary><a href="#一设计定位与生命周期">一、设计定位与生命周期</a></summary>

</details>

<details>
<summary><a href="#二数据结构">二、数据结构</a></summary>

- [2.1 三层嵌套结构](#21-三层嵌套结构)
- [2.2 静态初始化](#22-静态初始化)
- [2.3 Flag 标志位](#23-flag-标志位)

</details>

<details>
<summary><a href="#三核心-api-一览">三、核心 API 一览</a></summary>

- [3.1 内存注册](#31-内存注册)
- [3.2 内存分配](#32-内存分配)
- [3.3 属性操作](#33-属性操作)
- [3.4 查询](#34-查询)
- [3.5 迭代器](#35-迭代器)

</details>

<details>
<summary><a href="#四核心算法详解">四、核心算法详解</a></summary>

- [4.1 有序区间插入 —— `memblock_add_range()`](#41-有序区间插入--memblock_add_range)
- [4.2 区间隔离/分裂 —— `memblock_isolate_range()`](#42-区间隔离分裂--memblock_isolate_range)
- [4.3 区间合并 —— `memblock_merge_regions()`](#43-区间合并--memblock_merge_regions)
- [4.4 双有序区间集合差遍历 —— `__next_mem_range()`](#44-双有序区间集合差遍历--__next_mem_range)
- [4.5 二分查找 —— `memblock_search()`](#45-二分查找--memblock_search)
- [4.6 动态数组倍增扩容 —— `memblock_double_array()`](#46-动态数组倍增扩容--memblock_double_array)
- [4.7 贪心累减定位 —— `__find_max_addr()`](#47-贪心累减定位--__find_max_addr)
- [4.8 数组元素删除 —— `memblock_remove_region()`](#48-数组元素删除--memblock_remove_region)
- [4.9 对齐裁剪 —— `memblock_trim_memory()`](#49-对齐裁剪--memblock_trim_memory)
- [4.10 逆序首次适配 —— `__memblock_find_range_top_down()`](#410-逆序首次适配--__memblock_find_range_top_down)
- [4.11 正序首次适配 —— `__memblock_find_range_bottom_up()`](#411-正序首次适配--__memblock_find_range_bottom_up)
- [4.12 多维条件谓词过滤 —— `should_skip_region()`](#412-多维条件谓词过滤--should_skip_region)
- [4.13 级联降级回退策略 —— `memblock_alloc_range_nid()` + `memblock_alloc_internal()`](#413-级联降级回退策略--memblock_alloc_range_nid--memblock_alloc_internal)
- [4.14 地址范围规范化 —— `memblock_find_in_range_node()`](#414-地址范围规范化--memblock_find_in_range_node)
- [4.15 区间批量删除 —— `memblock_remove_range()`](#415-区间批量删除--memblock_remove_range)
- [4.16 选择性区间保留 —— `memblock_cap_memory_range()`](#416-选择性区间保留--memblock_cap_memory_range)
- [4.17 区间属性批量设置 —— `memblock_set_node()`](#417-区间属性批量设置--memblock_set_node)

</details>

<details>
<summary><a href="#五算法汇总表">五、算法汇总表</a></summary>

- [5.1 基础数据结构算法](#51-基础数据结构算法)
- [5.2 内存分配算法](#52-内存分配算法)
- [5.3 区间操作算法](#53-区间操作算法)

</details>

<details>
<summary><a href="#六内存分配完整调用链带算法标注">六、内存分配完整调用链（带算法标注）</a></summary>

- [分配路径中算法的协作关系](#分配路径中算法的协作关系)

</details>

<details>
<summary><a href="#七关键设计模式">七、关键设计模式</a></summary>

- [7.1 isolate → 操作 → merge 三步模式](#71-isolate-→-操作-→-merge-三步模式)
- [7.2 memory − reserved = free](#72-memory-−-reserved--free)
- [7.3 NOMAP trick（ARM64）](#73-nomap-trickarm64)

</details>

<details>
<summary><a href="#八调试方法">八、调试方法</a></summary>

- [8.1 启动参数](#81-启动参数)
- [8.2 运行时查看（需要 CONFIG_ARCH_KEEP_MEMBLOCK）](#82-运行时查看需要-config_arch_keep_memblock)
- [8.3 dmesg 输出示例](#83-dmesg-输出示例)

</details>

<details>
<summary><a href="#九与后续子系统的衔接">九、与后续子系统的衔接</a></summary>

</details>

<details>
<summary><a href="#十总结">十、总结</a></summary>

</details>

---

## 一、设计定位与生命周期

Memblock 是内核**早期启动阶段**的物理内存管理器，在 buddy 页分配器就绪之前工作。

```
启动流程中 memblock 的位置：

固件/bootloader 传递内存信息 (DTB/UEFI/E820)
        │
        ▼
arch_setup → memblock_add() 注册物理内存
        │
        ▼
arm64_memblock_init() / x86 equivalent   ← memblock 主要配置阶段
        │
        ▼
paging_init() → map_mem()                ← 依据 memblock 建立页表
        │
        ▼
bootmem_init() → zone_sizes_init()       ← 建立 zone / node
        │
        ▼
mm_core_init() → mem_init()              ← memblock_free_all() 释放给 buddy
        │
        ▼
memblock_discard()                        ← 释放 memblock 自身数据（除非 CONFIG_ARCH_KEEP_MEMBLOCK）
```

**核心设计思想**：用两个有序区间数组描述整个物理内存状态——**memory** 记录"有哪些内存"，**reserved** 记录"哪些被占用了"。空闲内存 = memory − reserved。

---

## 二、数据结构

### 2.1 三层嵌套结构

```c
struct memblock {                          // 全局单例，静态初始化
    bool bottom_up;                        // 分配方向（默认 top-down）
    phys_addr_t current_limit;             // 分配上限
    struct memblock_type memory;           // 系统可用物理内存
    struct memblock_type reserved;         // 已分配/保留的内存
};

struct memblock_type {                     // 内存类型集合
    unsigned long cnt;                     // 当前 region 数量
    unsigned long max;                     // 数组最大容量
    phys_addr_t total_size;                // 所有 region 总大小
    struct memblock_region *regions;       // 有序区间数组
    char *name;                            // "memory" 或 "reserved"
};

struct memblock_region {                   // 单个内存区间
    phys_addr_t base;                      // 起始物理地址
    phys_addr_t size;                      // 区间大小
    enum memblock_flags flags;             // 属性标志
    int nid;                               // NUMA 节点 ID（CONFIG_NUMA）
};
```

### 2.2 静态初始化

```c
static struct memblock_region memblock_memory_init_regions[128];
static struct memblock_region memblock_reserved_init_regions[128];

struct memblock memblock = {
    .memory.regions   = memblock_memory_init_regions,
    .memory.max       = 128,
    .reserved.regions = memblock_reserved_init_regions,
    .reserved.max     = 128,
    .bottom_up        = false,
    .current_limit    = MEMBLOCK_ALLOC_ANYWHERE,  // PHYS_ADDR_MAX
};
```

**关键不变量**：`regions[]` 数组始终按 `base` 升序排列，相邻且属性相同的 region 会被自动合并。

### 2.3 Flag 标志位

| Flag | 值 | 用途 |
|---|---|---|
| `MEMBLOCK_NONE` | 0x0 | 无特殊属性 |
| `MEMBLOCK_HOTPLUG` | 0x1 | 可热插拔内存 |
| `MEMBLOCK_MIRROR` | 0x2 | 镜像内存（优先分配） |
| `MEMBLOCK_NOMAP` | 0x4 | 不建立内核直接映射 |
| `MEMBLOCK_DRIVER_MANAGED` | 0x8 | 驱动管理的内存 |
| `MEMBLOCK_RSRV_NOINIT` | 0x10 | 不初始化 struct page |
| `MEMBLOCK_RSRV_KERN` | 0x20 | 内核保留（所有分配都设置） |
| `MEMBLOCK_KHO_SCRATCH` | 0x40 | kexec handover 临时内存 |

---

## 三、核心 API 一览

### 3.1 内存注册

| API | 作用 | 操作目标 |
|---|---|---|
| `memblock_add(base, size)` | 注册物理内存区间 | memory |
| `memblock_add_node(base, size, nid, flags)` | 注册并指定 NUMA 节点 | memory |
| `memblock_remove(base, size)` | 移除物理内存区间 | memory |
| `memblock_reserve(base, size)` | 标记为已保留 | reserved |
| `memblock_phys_free(base, size)` | 释放已保留的区间 | reserved |

### 3.2 内存分配

| API | 返回值 | 特点 |
|---|---|---|
| `memblock_phys_alloc_range(size, align, start, end)` | 物理地址 | 指定范围 |
| `memblock_alloc(size, align)` | 虚拟地址 | 清零、可回退 |
| `memblock_alloc_try_nid(size, align, min, max, nid)` | 虚拟地址 | 优先指定 NUMA 节点 |
| `memblock_alloc_or_panic(size, align)` | 虚拟地址 | 失败则 panic |

### 3.3 属性操作

| API | 作用 |
|---|---|
| `memblock_mark_nomap(base, size)` | 标记 NOMAP（不建立直接映射）|
| `memblock_clear_nomap(base, size)` | 清除 NOMAP |
| `memblock_mark_hotplug(base, size)` | 标记可热插拔 |
| `memblock_set_node(base, size, type, nid)` | 设置 NUMA 节点 ID |

### 3.4 查询

| API | 作用 |
|---|---|
| `memblock_is_memory(addr)` | 地址是否属于 memory |
| `memblock_is_reserved(addr)` | 地址是否属于 reserved |
| `memblock_is_map_memory(addr)` | 地址是否属于可映射的 memory（排除 NOMAP）|
| `memblock_phys_mem_size()` | 系统物理内存总量 |
| `memblock_start_of_DRAM()` | 物理内存起始地址 |
| `memblock_end_of_DRAM()` | 物理内存结束地址 |

### 3.5 迭代器

| 宏 | 遍历内容 |
|---|---|
| `for_each_mem_range(i, &start, &end)` | memory 中可映射区间（排除 HOTPLUG/DRIVER_MANAGED）|
| `for_each_free_mem_range(i, nid, flags, &start, &end, &nid)` | 空闲区间（memory − reserved）|
| `for_each_reserved_mem_range(i, &start, &end)` | reserved 区间 |
| `for_each_mem_region(r)` | memory 中所有 region |

---

## 四、核心算法详解

### 4.1 有序区间插入 —— `memblock_add_range()`

**问题**：向有序区间数组中插入 `[base, end)`，允许与已有区间重叠，只插入非重叠部分。

**算法**：线性扫描 + 间隙填充 + 两遍扫描优化

```
新区间:        [==============)
已有:     [--A--]     [--B--]     [--C--]
插入:           [++++]       [++++]
合并后:   [--A--======-------=====--C--]
```

```c
static int memblock_add_range(struct memblock_type *type,
                              phys_addr_t base, phys_addr_t size, int nid,
                              enum memblock_flags flags)
{
    bool insert = false;
    // 第一遍：insert=false，只计数需要多少新 region
    // 如果数组空间不够，调用 memblock_double_array() 扩容
    // 第二遍：insert=true，实际执行插入
repeat:
    for_each_memblock_type(idx, type, rgn) {
        if (rbase > base) {
            // 间隙部分 [base, rbase) 需要插入
            nr_new++;
            if (insert)
                memblock_insert_region(type, idx, base, rbase - base, ...);
        }
        base = min(rend, end);  // 跳过重叠部分
    }
    // 尾部剩余
    if (base < end) { nr_new++; if (insert) memblock_insert_region(...); }

    if (!insert) { 确保空间足够; insert = true; goto repeat; }
    else { memblock_merge_regions(...); return 0; }
}
```

**复杂度**：O(n)，n 为当前 region 数量  
**对应经典问题**：LeetCode 57 - Insert Interval

---

### 4.2 区间隔离/分裂 —— `memblock_isolate_range()`

**问题**：确保 `[base, base+size)` 的边界与 region 边界精确对齐。

**算法**：线性扫描 + 边界分裂

```
原始:  [---------- region ----------]
目标:         [========]
结果:  [--左--][========][----右------]
         ↑                    ↑
     新 region             原 region（base/size 被修改）
```

```c
static int memblock_isolate_range(struct memblock_type *type,
                                  phys_addr_t base, phys_addr_t size,
                                  int *start_rgn, int *end_rgn)
{
    // 预留 2 个空位（左右各可能拆分一次）
    while (type->cnt + 2 > type->max)
        memblock_double_array(type, base, size);

    for_each_memblock_type(idx, type, rgn) {
        if (rbase < base) {
            // 左边界穿过 region → 拆分
            rgn->base = base;
            rgn->size -= base - rbase;
            memblock_insert_region(type, idx, rbase, base - rbase, ...);
        } else if (rend > end) {
            // 右边界穿过 region → 拆分
            rgn->base = end;
            rgn->size -= end - rbase;
            memblock_insert_region(type, idx--, rbase, end - rbase, ...);
        } else {
            // region 完全在目标范围内，记录索引
            *start_rgn = idx; *end_rgn = idx + 1;
        }
    }
}
```

**用途**：`memblock_remove`、`memblock_setclr_flag`、`memblock_set_node` 的共同底层  
**复杂度**：O(n)

---

### 4.3 区间合并 —— `memblock_merge_regions()`

**问题**：合并数组中相邻且兼容的 region。

**算法**：相邻项线性扫描

```c
static void memblock_merge_regions(struct memblock_type *type,
                                   unsigned long start_rgn, unsigned long end_rgn)
{
    while (i < end_rgn) {
        // 合并条件：地址连续 + 同一 NUMA 节点 + 相同 flags
        if (this->base + this->size == next->base &&
            this->nid == next->nid &&
            this->flags == next->flags) {
            this->size += next->size;
            memmove(next, next + 1, ...);  // 删除 next
            type->cnt--;
        }
    }
}
```

**复杂度**：O(n)  
**对应经典问题**：LeetCode 56 - Merge Intervals（此处数组已排序）

---

### 4.4 双有序区间集合差遍历 —— `__next_mem_range()`

**问题**：遍历 type_a \ type_b（memory 中有但 reserved 中没有的区间 = 空闲内存）。

**算法**：双指针归并扫描

```
type_a (memory):   [====]     [==========]     [====]
type_b (reserved):       [==]        [==]
间隙 (gaps):       [0,0) [16,32)  [48,128)  [130,MAX)
空闲区间:          [====] [===]    [==] [==]   [====]
```

```c
void __next_mem_range(u64 *idx, int nid, enum memblock_flags flags,
                      struct memblock_type *type_a,
                      struct memblock_type *type_b, ...)
{
    int idx_a = *idx & 0xffffffff;        // 低 32 位：type_a 索引
    int idx_b = *idx >> 32;               // 高 32 位：type_b 间隙索引

    for (; idx_a < type_a->cnt; idx_a++) {
        // 跳过 NOMAP/HOTPLUG 等不符合 flags 的 region
        if (should_skip_region(...)) continue;

        for (; idx_b <= type_b->cnt; idx_b++) {
            // 间隙 = [前一个 reserved 的 end, 当前 reserved 的 base)
            r_start = idx_b ? r[-1].base + r[-1].size : 0;
            r_end   = idx_b < type_b->cnt ? r->base : PHYS_ADDR_MAX;

            // 返回 type_a[idx_a] 与 gap[idx_b] 的交集
            out_start = max(m_start, r_start);
            out_end   = min(m_end, r_end);

            // 先结束者的指针前进
            if (m_end <= r_end) idx_a++;
            else                idx_b++;
            return;
        }
    }
}
```

**复杂度**：遍历完所有空闲区间 O(n+m)  
**对应经典算法**：两个有序区间列表求差集 / 归并扫描

---

### 4.5 二分查找 —— `memblock_search()`

**问题**：判断一个物理地址属于哪个 region。

```c
static int memblock_search(struct memblock_type *type, phys_addr_t addr)
{
    unsigned int left = 0, right = type->cnt;
    do {
        unsigned int mid = (right + left) / 2;
        if (addr < type->regions[mid].base)
            right = mid;
        else if (addr >= type->regions[mid].base + type->regions[mid].size)
            left = mid + 1;
        else
            return mid;  // 命中
    } while (left < right);
    return -1;
}
```

**复杂度**：O(log n)  
**用途**：`memblock_is_memory()`、`memblock_is_reserved()`、`memblock_is_map_memory()`

---

### 4.6 动态数组倍增扩容 —— `memblock_double_array()`

**问题**：region 数组已满（初始 128 项），需要扩容。

**算法**：
1. 新大小 = 旧大小 × 2
2. slab 未就绪 → 用 memblock 自身分配（`memblock_find_in_range` + `memblock_reserve`）
3. slab 已就绪 → `kmalloc`
4. `memcpy` 迁移旧数据，释放旧数组
5. 需要 `memblock_allow_resize()` 开启后才允许（防止覆盖 initrd 等区域）

**特殊处理**：扩容 reserved 数组时，新数组地址必须避开正在插入的区间 `[new_area_start, new_area_start + new_area_size)`，否则会产生循环依赖。

**复杂度**：均摊 O(1)  
**对应经典**：`std::vector` 动态扩容策略

---

### 4.7 贪心累减定位 —— `__find_max_addr()`

**问题**：`mem=` 启动参数指定内存限制大小，需要转换为物理地址上界。

```c
static phys_addr_t __find_max_addr(phys_addr_t limit)
{
    for_each_mem_region(r) {
        if (limit <= r->size)
            return r->base + limit;  // 找到了
        limit -= r->size;
    }
    return PHYS_ADDR_MAX;  // limit 超过总内存
}
```

**复杂度**：O(n)  
**本质**：前缀和上的二分定位（此处用线性扫描实现，因为 n 很小）

---

### 4.8 数组元素删除 —— `memblock_remove_region()`

```c
static void memblock_remove_region(struct memblock_type *type, unsigned long r)
{
    type->total_size -= type->regions[r].size;
    memmove(&type->regions[r], &type->regions[r + 1],
            (type->cnt - (r + 1)) * sizeof(type->regions[r]));
    type->cnt--;
}
```

**复杂度**：O(n)

---

### 4.9 对齐裁剪 —— `memblock_trim_memory()`

```c
void memblock_trim_memory(phys_addr_t align)
{
    for_each_mem_region(r) {
        start = round_up(r->base, align);
        end   = round_down(r->base + r->size, align);
        if (start < end) { r->base = start; r->size = end - start; }
        else             { 删除该 region; }
    }
}
```

---

### 4.10 逆序首次适配 —— `__memblock_find_range_top_down()`

**问题**：在空闲区间中找到一个能容纳 `size` 且满足 `align` 对齐的位置（从高地址开始）。

**算法**：Reverse First Fit —— 逆序遍历空闲区间，从每个区间的**尾部**向下对齐分配。

```c
static phys_addr_t __memblock_find_range_top_down(phys_addr_t start,
                    phys_addr_t end, phys_addr_t size, phys_addr_t align,
                    int nid, enum memblock_flags flags)
{
    phys_addr_t this_start, this_end, cand;
    u64 i;

    for_each_free_mem_range_reverse(i, nid, flags,
                                    &this_start, &this_end, NULL) {
        this_start = clamp(this_start, start, end);  // 限制在请求范围内
        this_end   = clamp(this_end,   start, end);

        if (this_end < size)
            continue;

        cand = round_down(this_end - size, align);   // 从区间尾部向下对齐
        if (cand >= this_start)
            return cand;                             // 首次适配命中
    }
    return 0;
}
```

**设计意图**：从区间尾部分配，尽量保留低地址给 DMA 设备。  
**复杂度**：O(n+m)（遍历空闲区间的开销）  
**对应经典**：内存分配中的 First Fit Decreasing Address 策略

---

### 4.11 正序首次适配 —— `__memblock_find_range_bottom_up()`

**问题**：同上，但从低地址开始搜索。

**算法**：First Fit —— 正序遍历空闲区间，从每个区间的**头部**向上对齐分配。

```c
static phys_addr_t __memblock_find_range_bottom_up(phys_addr_t start,
                    phys_addr_t end, phys_addr_t size, phys_addr_t align,
                    int nid, enum memblock_flags flags)
{
    phys_addr_t this_start, this_end, cand;
    u64 i;

    for_each_free_mem_range(i, nid, flags, &this_start, &this_end, NULL) {
        this_start = clamp(this_start, start, end);
        this_end   = clamp(this_end,   start, end);

        cand = round_up(this_start, align);          // 从区间头部向上对齐
        if (cand < this_end && this_end - cand >= size)
            return cand;                             // 首次适配命中
    }
    return 0;
}
```

**设计意图**：从低地址分配，用于 KASLR 等需要 bottom-up 布局的场景。  
**复杂度**：O(n+m)  
**对应经典**：内存分配中的 First Fit 策略

---

### 4.12 多维条件谓词过滤 —— `should_skip_region()`

**问题**：在遍历空闲区间时，根据分配请求的属性过滤不符合条件的 region。

**算法**：多条件短路求值（Short-circuit Evaluation）

```c
static bool should_skip_region(struct memblock_type *type,
                               struct memblock_region *m,
                               int nid, int flags)
{
    int m_nid = memblock_get_region_node(m);

    // 仅对 memory 类型执行过滤，reserved/physmem 不过滤
    if (type != memblock_memory)
        return false;

    // 维度 1: NUMA 节点约束
    if (numa_valid_node(nid) && nid != m_nid)
        return true;

    // 维度 2: 热插拔感知（movable_node 策略下跳过热插拔区域）
    if (movable_node_is_enabled() && memblock_is_hotpluggable(m) &&
        !(flags & MEMBLOCK_HOTPLUG))
        return true;

    // 维度 3: 镜像内存优先（请求 MIRROR 时跳过非镜像区域）
    if ((flags & MEMBLOCK_MIRROR) && !memblock_is_mirror(m))
        return true;

    // 维度 4: NOMAP 区域默认跳过
    if (!(flags & MEMBLOCK_NOMAP) && memblock_is_nomap(m))
        return true;

    // 维度 5: 驱动管理内存默认跳过
    if (!(flags & MEMBLOCK_DRIVER_MANAGED) && memblock_is_driver_managed(m))
        return true;

    // 维度 6: KHO 早期分配仅使用 scratch 内存
    if ((flags & MEMBLOCK_KHO_SCRATCH) && !memblock_is_kho_scratch(m))
        return true;

    return false;
}
```

**本质**：6 维过滤器，flag 位作为"开关"控制每个维度是否生效。  
**复杂度**：O(1) 每次调用  
**设计巧妙之处**：同一个 `flags` 参数的不同 bit 既描述了"我想要什么"（如 `MEMBLOCK_MIRROR`），也隐式描述了"我不在意什么"（如不设 `MEMBLOCK_NOMAP` 则跳过 NOMAP 区域）。

---

### 4.13 级联降级回退策略 —— `memblock_alloc_range_nid()` + `memblock_alloc_internal()`

**问题**：分配失败时，如何逐步放宽约束以提高成功率？

**算法**：四级级联降级

```
Level 1: 指定 NUMA 节点 + MIRROR 标志 + [min_addr, max_addr]
    ↓ 失败
Level 2: 任意 NUMA 节点 + MIRROR 标志 + [min_addr, max_addr]
    ↓ 失败
Level 3: 任意 NUMA 节点 + 放弃 MIRROR（降级为普通内存）+ [min_addr, max_addr]
    ↓ 失败
Level 4: 任意 NUMA 节点 + 无 MIRROR + [0, max_addr]（放弃 min_addr 下限）
    ↓ 失败
返回 NULL / panic
```

```c
// memblock_alloc_range_nid() 中：
again:
    found = memblock_find_in_range_node(size, align, start, end, nid, flags);
    if (found && !memblock_reserve_kern(found, size))
        goto done;

    // 降级 1：放弃 NUMA 节点约束
    if (numa_valid_node(nid) && !exact_nid) {
        found = memblock_find_in_range_node(size, align, start, end,
                                            NUMA_NO_NODE, flags);
        if (found && !memblock_reserve_kern(found, size))
            goto done;
    }

    // 降级 2：放弃 MIRROR 约束
    if (flags & MEMBLOCK_MIRROR) {
        flags &= ~MEMBLOCK_MIRROR;
        pr_warn_ratelimited("Could not allocate mirrored memory\n");
        goto again;
    }
    return 0;

// memblock_alloc_internal() 中：
    alloc = memblock_alloc_range_nid(size, align, min_addr, max_addr, ...);

    // 降级 3：放弃 min_addr 约束
    if (!alloc && min_addr)
        alloc = memblock_alloc_range_nid(size, align, 0, max_addr, ...);
```

**本质**：贪心回退 / 约束松弛（Constraint Relaxation）  
**优先级**：NUMA 亲和性 > 镜像内存 > 地址范围下限

---

### 4.14 地址范围规范化 —— `memblock_find_in_range_node()`

**问题**：将用户传入的各种特殊常量和边界条件转化为有效的搜索范围。

**算法**：边界 Clamp + 方向分派

```c
static phys_addr_t memblock_find_in_range_node(phys_addr_t size,
                    phys_addr_t align, phys_addr_t start,
                    phys_addr_t end, int nid, enum memblock_flags flags)
{
    // 特殊常量转换为 current_limit
    if (end == MEMBLOCK_ALLOC_ACCESSIBLE ||
        end == MEMBLOCK_ALLOC_NOLEAKTRACE)
        end = memblock.current_limit;

    // 永远不分配物理地址 0（零页保护）
    start = max_t(phys_addr_t, start, PAGE_SIZE);
    end = max(start, end);

    // 根据方向分派
    if (memblock_bottom_up())
        return __memblock_find_range_bottom_up(start, end, size, align,
                                               nid, flags);
    else
        return __memblock_find_range_top_down(start, end, size, align,
                                              nid, flags);
}
```

**安全考量**：跳过第 0 页是为了捕获空指针解引用（物理地址 0 → 虚拟地址 NULL）。

---

### 4.15 区间批量删除 —— `memblock_remove_range()`

**问题**：从 region 数组中移除 `[base, base+size)` 覆盖的所有 region。

**算法**：isolate + 逆序批量删除

```c
static int memblock_remove_range(struct memblock_type *type,
                                 phys_addr_t base, phys_addr_t size)
{
    int start_rgn, end_rgn;
    int i, ret;

    // Step 1: 在边界处拆分 region
    ret = memblock_isolate_range(type, base, size, &start_rgn, &end_rgn);
    if (ret)
        return ret;

    // Step 2: 从后往前删除（避免索引偏移问题）
    for (i = end_rgn - 1; i >= start_rgn; i--)
        memblock_remove_region(type, i);

    return 0;
}
```

**为什么逆序删除**：`memblock_remove_region()` 使用 `memmove` 左移元素，如果正序删除，删除 `regions[i]` 后原来的 `regions[i+1]` 变成了 `regions[i]`，索引关系会乱。逆序删除则不存在此问题。  
**复杂度**：O(n × k)，k 为被删除的 region 数量（每次 `memmove` 为 O(n)）  
**对应经典**：数组逆序批量删除（避免索引偏移）

---

### 4.16 选择性区间保留 —— `memblock_cap_memory_range()`

**问题**：只保留 `[base, base+size)` 范围内的可映射内存，移除范围外的所有内存（但保留 NOMAP 区域）。

**算法**：isolate + 两端反向扫描选择性删除

```c
void memblock_cap_memory_range(phys_addr_t base, phys_addr_t size)
{
    int start_rgn, end_rgn;

    // Step 1: 在 [base, base+size) 边界处拆分
    memblock_isolate_range(&memblock.memory, base, size,
                           &start_rgn, &end_rgn);

    // Step 2: 逆序删除右侧的非 NOMAP region
    for (i = memblock.memory.cnt - 1; i >= end_rgn; i--)
        if (!memblock_is_nomap(&memblock.memory.regions[i]))
            memblock_remove_region(&memblock.memory, i);

    // Step 3: 逆序删除左侧的非 NOMAP region
    for (i = start_rgn - 1; i >= 0; i--)
        if (!memblock_is_nomap(&memblock.memory.regions[i]))
            memblock_remove_region(&memblock.memory, i);

    // Step 4: 同步裁剪 reserved 数组
    memblock_remove_range(&memblock.reserved, 0, base);
    memblock_remove_range(&memblock.reserved, base + size, PHYS_ADDR_MAX);
}
```

**关键细节**：保留 NOMAP 区域是因为它们代表固件保留的特殊内存（如 ACPI 表），即使超出 `mem=` 限制也不应丢失。  
**用途**：`memblock_mem_limit_remove_map()` → 处理 `mem=` 启动参数

---

### 4.17 区间属性批量设置 —— `memblock_set_node()`

**问题**：将 `[base, base+size)` 范围内所有 region 的 NUMA 节点 ID 设置为 `nid`。

**算法**：isolate → 批量设置 → merge（三步模式的典型应用）

```c
int memblock_set_node(phys_addr_t base, phys_addr_t size,
                      struct memblock_type *type, int nid)
{
    int start_rgn, end_rgn;
    int i, ret;

    // Step 1: 在目标边界处拆分 region
    ret = memblock_isolate_range(type, base, size, &start_rgn, &end_rgn);

    // Step 2: 批量设置 nid
    for (i = start_rgn; i < end_rgn; i++)
        memblock_set_region_node(&type->regions[i], nid);

    // Step 3: 合并相邻且属性相同的 region
    memblock_merge_regions(type, start_rgn, end_rgn);
    return 0;
}
```

**复杂度**：O(n)（isolate + merge 各一趟扫描）  
**与 `memblock_setclr_flag` 对比**：结构完全相同，只是 Step 2 操作的字段不同（nid vs flags）

---

## 五、算法汇总表

### 5.1 基础数据结构算法

| # | 算法 | 函数 | 复杂度 | 经典对应 |
|---|------|------|--------|----------|
| 1 | 有序区间插入（间隙填充 + 两遍扫描） | `memblock_add_range` | O(n) | LeetCode 57 Insert Interval |
| 2 | 区间边界分裂 | `memblock_isolate_range` | O(n) | — |
| 3 | 相邻区间合并 | `memblock_merge_regions` | O(n) | LeetCode 56 Merge Intervals |
| 4 | 双有序区间列表求差集（双指针归并） | `__next_mem_range` | O(n+m) | 归并扫描 / Interval Difference |
| 5 | 二分查找 | `memblock_search` | O(log n) | 标准二分 |
| 6 | 动态数组倍增扩容 | `memblock_double_array` | 均摊 O(1) | std::vector 扩容 |
| 7 | 有序数组元素删除 | `memblock_remove_region` | O(n) | 数组删除 |

### 5.2 内存分配算法

| # | 算法 | 函数 | 复杂度 | 经典对应 |
|---|------|------|--------|----------|
| 8 | 逆序首次适配（Reverse First Fit） | `__memblock_find_range_top_down` | O(n+m) | First Fit Decreasing Address |
| 9 | 正序首次适配（First Fit） | `__memblock_find_range_bottom_up` | O(n+m) | First Fit |
| 10 | 多维条件谓词过滤 | `should_skip_region` | O(1) | 多条件短路求值 |
| 11 | 级联降级回退（Constraint Relaxation） | `memblock_alloc_range_nid` + `memblock_alloc_internal` | — | 贪心回退 |
| 12 | 地址范围规范化 + 方向分派 | `memblock_find_in_range_node` | O(1) | Clamp + Strategy Pattern |

### 5.3 区间操作算法

| # | 算法 | 函数 | 复杂度 | 经典对应 |
|---|------|------|--------|----------|
| 13 | 区间批量逆序删除 | `memblock_remove_range` | O(n×k) | 数组逆序批量删除 |
| 14 | 选择性区间保留（两端裁剪 + NOMAP 豁免） | `memblock_cap_memory_range` | O(n) | — |
| 15 | 区间属性批量设置（isolate→set→merge） | `memblock_set_node` / `memblock_setclr_flag` | O(n) | — |
| 16 | 贪心累减定位 | `__find_max_addr` | O(n) | 前缀和查找 |
| 17 | 区间对齐裁剪 | `memblock_trim_memory` | O(n) | — |

> 所有算法建立在一个核心假设上：**region 数量 n 极小**（典型系统 < 100），所以 O(n) 的线性扫描和 `memmove` 完全可接受，无需红黑树等复杂结构。

---

## 六、内存分配完整调用链（带算法标注）

```
memblock_alloc(size, align)                          // 返回虚拟地址，自动清零
 └→ memblock_alloc_try_nid(size, align, 0, MAX, NUMA_NO_NODE)
     └→ memblock_alloc_internal(...)                 ← [算法 #11: 级联降级 Level 4 - 放弃 min_addr]
         │
         ├→ memblock_alloc_range_nid(...)            ← [算法 #11: 级联降级 Level 1→2→3]
         │   │
         │   ├─ choose_memblock_flags()              ← [算法 #10: Flag 选择 - MIRROR/KHO_SCRATCH]
         │   │
         │   ├─ memblock_find_in_range_node(...)     ← [算法 #12: 范围 Clamp + 方向分派]
         │   │   │
         │   │   ├─ [top-down 默认路径]
         │   │   │   __memblock_find_range_top_down()← [算法 #8: 逆序首次适配]
         │   │   │   └─ for_each_free_mem_range_reverse(...)
         │   │   │       └─ __next_mem_range_rev()   ← [算法 #4: 双指针归并求差集（反向）]
         │   │   │           └─ should_skip_region() ← [算法 #10: 多维谓词过滤]
         │   │   │
         │   │   └─ [bottom-up 路径]
         │   │       __memblock_find_range_bottom_up()← [算法 #9: 正序首次适配]
         │   │       └─ for_each_free_mem_range(...)
         │   │           └─ __next_mem_range()       ← [算法 #4: 双指针归并求差集]
         │   │               └─ should_skip_region() ← [算法 #10: 多维谓词过滤]
         │   │
         │   ├─ memblock_reserve_kern(found, size)   ← [算法 #1: 有序区间插入]
         │   │   └─ memblock_add_range(&reserved, ...)
         │   │       ├─ memblock_insert_region()     (memmove 右移)
         │   │       ├─ memblock_double_array()      ← [算法 #6: 倍增扩容，若需要]
         │   │       └─ memblock_merge_regions()     ← [算法 #3: 相邻合并]
         │   │
         │   └─ accept_memory(found, size)           (TDX/SEV-SNP 硬件内存接受)
         │
         └→ phys_to_virt(alloc)                      // 物理地址 → 虚拟地址
```

分配方向：
- **top-down**（默认）：从高地址向低地址搜索，尽量保留低地址给 DMA
- **bottom-up**：从低地址向高地址搜索，用于 KASLR 场景避免碎片

### 分配路径中算法的协作关系

```
                    ┌─────────────────────────────────┐
                    │  级联降级回退 (#11)                │
                    │  NUMA → ANY → 无MIRROR → 无min   │
                    └─────────────┬───────────────────┘
                                  │ 每次重试
                                  ▼
                    ┌─────────────────────────────────┐
                    │  范围规范化 + 方向分派 (#12)       │
                    │  Clamp(start, end) → top/bottom  │
                    └─────────────┬───────────────────┘
                                  │
                    ┌─────────────┴───────────────────┐
                    ▼                                   ▼
          ┌────────────────┐                 ┌────────────────┐
          │ Top-Down (#8)  │                 │ Bottom-Up (#9) │
          │ Reverse 1st Fit│                 │ First Fit      │
          └───────┬────────┘                 └───────┬────────┘
                  │                                   │
                  ▼                                   ▼
          ┌─────────────────────────────────────────────────┐
          │  双指针归并求差集 (#4)                              │
          │  memory[idx_a] ∩ gap_reserved[idx_b]             │
          │                                                   │
          │  在每个 region 上执行:  多维谓词过滤 (#10)          │
          │  skip NOMAP / HOTPLUG / non-MIRROR / wrong-NUMA   │
          └─────────────────────────┬─────────────────────────┘
                                    │ 找到地址 found
                                    ▼
          ┌─────────────────────────────────────────────────┐
          │  有序区间插入 (#1)                                 │
          │  memblock_add_range(&reserved, found, size)       │
          │  → insert_region (#7 memmove)                     │
          │  → merge_regions (#3 合并相邻)                     │
          │  → double_array (#6 若空间不足)                    │
          └───────────────────────────────────────────────────┘
```

---

## 七、关键设计模式

### 7.1 isolate → 操作 → merge 三步模式

`memblock_setclr_flag()`、`memblock_set_node()`、`memblock_remove()` 都遵循同一模式：

```
1. memblock_isolate_range()  → 在目标边界处拆分 region
2. 对隔离出的 region 执行操作（设置 flag / 设置 nid / 删除）
3. memblock_merge_regions()  → 合并相邻兼容 region
```

这保证了操作的精确性（不影响目标范围外的 region）和数组的最简性（无冗余 region）。

### 7.2 memory − reserved = free

memblock 不维护"空闲列表"，而是通过迭代器实时计算：

```c
#define for_each_free_mem_range(i, nid, flags, p_start, p_end, p_nid)   \
    __for_each_mem_range(i, &memblock.memory, &memblock.reserved,       \
                         nid, flags, p_start, p_end, p_nid)
```

这避免了维护第三个数组的开销，也避免了三个数组之间一致性的问题。

### 7.3 NOMAP trick（ARM64）

在 `map_mem()` 中建立线性映射时：

```c
// 1. 临时标记内核 text/rodata 为 NOMAP
memblock_mark_nomap(kernel_start, kernel_end - kernel_start);

// 2. 映射所有内存（NOMAP 区域被 for_each_mem_range 自动跳过）
for_each_mem_range(i, &start, &end)
    __map_memblock(pgdp, start, end, PAGE_KERNEL, flags);

// 3. 单独映射内核区域（不可执行 + 禁用 contiguous）
__map_memblock(pgdp, kernel_start, kernel_end, PAGE_KERNEL, NO_CONT_MAPPINGS);

// 4. 恢复
memblock_clear_nomap(kernel_start, kernel_end - kernel_start);
```

目的：防止内核 text 在线性映射中获得可写+可执行的别名映射，确保 W^X 安全策略。

---

## 八、调试方法

### 8.1 启动参数

```bash
# 开启 memblock 调试输出
memblock=debug

# 限制可用内存
mem=512M
```

### 8.2 运行时查看（需要 CONFIG_ARCH_KEEP_MEMBLOCK）

```bash
cat /sys/kernel/debug/memblock/memory
cat /sys/kernel/debug/memblock/reserved
```

### 8.3 dmesg 输出示例

```
MEMBLOCK configuration:
 memory size = 0x0000000080000000 reserved size = 0x0000000004800000
 memory[0x0]    [0x0000000040000000-0x00000000bfffffff], 0x80000000 bytes flags: 0x0
 reserved[0x0]  [0x0000000040080000-0x00000000423affff], 0x02330000 bytes flags: 0x20
 reserved[0x1]  [0x00000000bf000000-0x00000000bfffffff], 0x01000000 bytes flags: 0x0
```

---

## 九、与后续子系统的衔接

| 阶段 | 事件 | memblock 角色 |
|------|------|---------------|
| 早期启动 | DTB 解析 | `memblock_add()` 注册物理内存 |
| 架构初始化 | `arm64_memblock_init()` | 裁剪/对齐/保留内核区域 |
| 页表建立 | `paging_init()` → `map_mem()` | `for_each_mem_range` 驱动映射 |
| Zone 初始化 | `zone_sizes_init()` | `memblock_end_of_DRAM()` 确定边界 |
| buddy 初始化 | `memblock_free_all()` | 逐页释放非 reserved 内存给 buddy |
| 启动完成 | `memblock_discard()` | 释放 memblock 自身数据结构 |
| 内存热插拔 | `arch_add_memory()` | 若保留了 memblock，继续使用 `memblock_add`/`memblock_remove` |

---

## 十、总结

1. **极简数据结构**——有序数组而非红黑树，因为 region 数量极小（< 100）
2. **两个集合描述三种状态**——memory、reserved 和隐式的 free = memory − reserved
3. **所有修改操作共享 isolate → 操作 → merge 模式**——保证精确性和最简性
4. **静态初始化，零依赖**——不需要任何动态分配器即可工作
5. **生命周期清晰**——启动完成后自动消亡（除非架构要求保留）
6. **算法选择务实**——O(n) 线性操作 + O(log n) 二分查询，足够应对早期启动场景
