# Linux ARM64 KSM (Kernel Same-page Merging) 深度学习指南

> 适用范围：Linux 6.18.1 / ARM64 / 当前工作区源码
>
> 文档目标：系统梳理 KSM 的原理、数据结构、核心算法、写保护与 COW 机制、ARM64 特有机制（MTE 标签、Contiguous PTE、BBM），并通过 QEMU 实验验证关键行为。

---

## 目录

<details>
<summary>展开目录</summary>

- [一句话先说清](#一句话先说清)
- [为什么需要 KSM](#为什么需要-ksm)
- [核心心智模型](#核心心智模型)
- [关键数据结构](#关键数据结构)
- [KSM 核心算法：cmp_and_merge_page](#ksm-核心算法cmp_and_merge_page)
- [写保护机制](#写保护机制)
- [页替换与合并流程](#页替换与合并流程)
- [COW 断裂与写缺页处理](#cow-断裂与写缺页处理)
- [零页优化](#零页优化)
- [稳定树与非稳定树](#稳定树与非稳定树)
- [NUMA 与 merge_across_nodes](#numa-与-merge_across_nodes)
- [rmap 反向映射集成](#rmap-反向映射集成)
- [ksmd 内核线程与扫描流程](#ksmd-内核线程与扫描流程)
- [madvise 与 prctl 入口](#madvise-与-prctl-入口)
- [ARM64 特有机制](#arm64-特有机制)
  - [MTE 标签与 memcmp_pages](#mte-标签与-memcmp_pages)
  - [PTE 写保护与 DBM 协议](#pte-写保护与-dbm-协议)
  - [Contiguous PTE 与写保护冲突](#contiguous-pte-与写保护冲突)
  - [BBM 与 COW 断裂 TLB 维护](#bbm-与-cow-断裂-tlb-维护)
  - [MTE 标签在 COW 复制中的传播](#mte-标签在-cow-复制中的传播)
  - [Errata #2645198 与权限变更](#errata-2645198-与权限变更)
- [延迟与跳过机制](#延迟与跳过机制)
- [sysfs 接口与调优参数](#sysfs-接口与调优参数)
- [vmstat 与 Tracepoint](#vmstat-与-tracepoint)
- [QEMU 实验](#qemu-实验)
  - [实验 1：构建 ARM64 内核与 KSM 启动验证](#实验-1构建-arm64-内核与-ksm-启动验证)
  - [实验 2：madvise 触发 KSM 合并观察](#实验-2madvise-触发-ksm-合并观察)
  - [实验 3：手动控制 ksmd 运行与合并统计](#实验-3手动控制-ksmd-运行与合并统计)
  - [实验 4：COW 断裂验证](#实验-4cow-断裂验证)
  - [实验 5：零页合并验证](#实验-5零页合并验证)
  - [实验 6：ftrace 追踪 KSM 合并过程](#实验-6ftrace-追踪-ksm-合并过程)
  - [实验 7：NUMA 环境下 merge_across_nodes 对比](#实验-7numa-环境下-merge_across_nodes-对比)
  - [实验 8：MTE 环境下 KSM 行为验证](#实验-8mte-环境下-ksm-行为验证)
- [源码阅读路线图](#源码阅读路线图)
- [关键源码索引](#关键源码索引)

</details>

---

## 一句话先说清

**KSM = 找出不同进程虚拟地址空间中内容完全相同的匿名页，通过写保护 + 页替换让它们共享同一个物理页，从而节省内存。** 当进程写入该共享页时触发 COW，复制出私有页恢复独立性。核心算法是"双红黑树 + checksum 过滤 + memcmp 逐字节比对"。

---

## 为什么需要 KSM

| 场景 | 说明 |
|------|------|
| KVM 虚拟机 | 多个 VM 运行相同 OS/应用，大量内存页内容重复 |
| 容器 | 同一基础镜像的多个容器共享相同库和配置 |
| 大型数据库 | 相同查询缓存页在不同进程中重复 |
| 通用服务器 | 进程 fork 后未修改的页仍可合并 |

**KSM 与其他内存优化机制的区别**：

| 机制 | 原理 | 作用域 |
|------|------|--------|
| **KSM** | 内容相同的匿名页共享同一物理页 | 跨进程 |
| **COW (fork)** | fork 时父子共享，写时复制 | 同进程父子 |
| **THP** | 合并连续小页为大页减少 TLB 压力 | 单进程 |
| **共享内存 (shm)** | 主动共享指定内存区域 | 需应用配合 |
| **页回收** | 释放不活跃页增加空闲量 | 单进程 |

---

## 核心智模型

```
┌──────────┐    ┌──────────┐    ┌──────────┐
│ 进程 A   │    │ 进程 B   │    │ 进程 C   │
│ VMA:     │    │ VMA:     │    │ VMA:     │
│  MADV_   │    │  MADV_   │    │  MADV_   │
│  MERGE-  │    │  MERGE-  │    │  MERGE-  │
│  ABLE    │    │  ABLE    │    │  ABLE    │
│          │    │          │    │          │
│ PTE:     │    │ PTE:     │    │ PTE:     │
│ RO→KSM页 │    │ RO→KSM页 │    │ RO→KSM页 │
│  ↕       │    │  ↕       │    │  ↕       │
│  └───────┼────┼──────────┼────┘          │
│          │    │          │    │          │
└──────────┘    └──────────┘    └──────────┘
                    ↕
              ┌──────────┐
              │ KSM 页   │ ← 单个物理页，内容不可变
              │ (WP)     │ ← 写保护 (PTE_RDONLY=1, PTE_WRITE=0)
              │ stable_  │
              │ node     │
              │ rmap:    │ ← hlist 指向 A,B,C 的 rmap_item
              │ A,B,C    │
              └──────────┘

进程 A 写入该页:
  → 硬件缺页 (permission fault)
  → do_wp_page() → wp_page_copy()
  → 分配新私有页, 复制内容
  → PTE 指向新私有页 (PTE_WRITE=1)
  → KSM sharing 对进程 A 断裂
```

**三阶段核心流程**：
1. **扫描发现**：ksmd 线程遍历所有 `VM_MERGEABLE` VMA 的匿名页
2. **比对合并**：checksum 过滤 → stable tree 搜索 → unstable tree 搜索 → 写保护 + 页替换
3. **断裂恢复**：进程写入 → COW → 恢复私有页 → ksmd 下次扫描可重新发现相同页

---

## 关键数据结构

### `struct ksm_stable_node` (`mm/ksm.c:159-185`)

稳定树节点，代表一个独一无二的已合并页内容：

```c
struct ksm_stable_node {
    union {
        struct rb_node node;         // 稳定树中的红黑树节点
        struct {                     // NUMA 迁移时的链表节点
            struct list_head *head;
            struct {
                struct hlist_node hlist_dup;  // chain 中的 dup 链接
                struct list_head list;
            };
        };
    };
    struct hlist_head hlist;         // 挂载所有共享此页的 rmap_item
    union {
        unsigned long kpfn;          // KSM 页的物理页帧号
        unsigned long chain_prune_time; // chain 节点的修剪时间
    };
    int rmap_hlist_len;              // rmap_item 数量（-1024 表示 chain 节点）
#ifdef CONFIG_NUMA
    int nid;                         // NUMA 节点 ID
#endif
};
```

**关键概念**：
- `STABLE_NODE_CHAIN` (`rmap_hlist_len == -1024`)：chain 节点，链接多个 "dup" stable_node，代表同一内容在不同 NUMA 节点上的副本
- `kpfn`：KSM 页的 PFN，用于从 stable_node 找到物理页

### `struct ksm_rmap_item` (`mm/ksm.c:201-221`)

反向映射项，一个扫描页对应一个 rmap_item：

```c
struct ksm_rmap_item {
    struct ksm_rmap_item *rmap_list;  // mm_slot 中单向链表
    union {
        struct anon_vma *anon_vma;    // 稳定树中：指向 anon_vma
        int nid;                      // 非稳定树中：NUMA 节点 ID
    };
    struct mm_struct *mm;             // 所属进程的 mm
    unsigned long address;            // 虚拟地址 + 低比特标志
    unsigned int oldchecksum;         // 上次扫描的 checksum
    rmap_age_t age;                   // 创建以来的扫描轮数
    rmap_age_t remaining_skips;       // 智能扫描跳过计数
    union {
        struct rb_node node;          // 非稳定树中的红黑树节点
        struct {                      // 稳定树中：
            struct ksm_stable_node *head;
            struct hlist_node hlist;
        };
    };
};
```

**地址低比特标志** (`mm/ksm.c:223-225`)：
- `SEQNR_MASK   0x0ff` — 非稳定树扫描序号
- `UNSTABLE_FLAG 0x100` — 当前在非稳定树中
- `STABLE_FLAG   0x200` — 当前在稳定树中

### `struct ksm_mm_slot` (`mm/ksm.c:126-129`)

每个注册到 KSM 的 mm（进程地址空间）对应一个 mm_slot：

```c
struct ksm_mm_slot {
    struct mm_slot slot;              // 通用 mm_slot（hash + 链表节点）
    struct ksm_rmap_item *rmap_list;  // 该 mm 的 rmap_item 单向链表头
};
```

### `struct ksm_scan` (`mm/ksm.c:140-145`)

全局扫描游标：

```c
struct ksm_scan {
    struct ksm_mm_slot *mm_slot;      // 当前扫描的 mm_slot
    unsigned long address;            // 下一个扫描地址
    struct ksm_rmap_item **rmap_list; // 下一个 rmap_item 指针
    unsigned long seqnr;              // 完成的全扫描轮数
};
```

### 全局状态 (`mm/ksm.c:228-248`)

```c
static struct rb_root one_stable_tree[1]   = { RB_ROOT };
static struct rb_root one_unstable_tree[1] = { RB_ROOT };
static struct rb_root *root_stable_tree   = one_stable_tree;
static struct rb_root *root_unstable_tree = one_unstable_tree;

static LIST_HEAD(migrate_nodes);           // NUMA 迁移挂起节点
static DEFINE_HASHTABLE(mm_slots_hash, 10); // mm → mm_slot 哈希表
static struct ksm_mm_slot ksm_mm_head;     // mm_slot 链表哨兵
```

当 `merge_across_nodes=0` 时，`root_stable_tree` 和 `root_unstable_tree` 变为 `nr_node_ids` 大小的数组，每个 NUMA 节点独立一棵树。

### KSM 页标识 (`include/linux/page-flags.h:717-752`)

```c
#define FOLIO_MAPPING_ANON     0x1
#define FOLIO_MAPPING_ANON_KSM 0x2
#define FOLIO_MAPPING_KSM      (FOLIO_MAPPING_ANON | FOLIO_MAPPING_ANON_KSM)  // 0x3

static __always_inline bool folio_test_ksm(const struct folio *folio)
{
    return ((unsigned long)folio->mapping & FOLIO_MAPPING_FLAGS) == FOLIO_MAPPING_KSM;
}
```

KSM 页通过 `folio->mapping` 的低 2 位编码标识：bit0=1 表示匿名，bit1=1 表示 KSM，两者都为 1 即 `0x3` = KSM 页。`stable_node` 指针直接存储在 `mapping` 中（OR 上 `FOLIO_MAPPING_KSM` 标志位）：

```c
// mm/ksm.c:1059-1069
static inline void folio_set_stable_node(struct folio *folio,
                                         struct ksm_stable_node *stable_node)
{
    folio->mapping = (void *)((unsigned long)stable_node | FOLIO_MAPPING_KSM);
}
```

---

## KSM 核心算法：cmp_and_merge_page

`cmp_and_merge_page()` (`mm/ksm.c:2221`) 是 KSM 的心脏。对每个扫描到的页，执行以下决策流程：

```
cmp_and_merge_page(page, rmap_item)
│
├── [A] 页已是 KSM 页 (folio_stable_node 存在)?
│   ├── NUMA 迁移需要 → 移到 migrate_nodes
│   ├── rmap_item 已指向同一 stable_node → RETURN
│   └── 超过 max_page_sharing → 设置 bypass 标志
│
├── [B] 页不是 KSM 页:
│   ├── remove_rmap_item_from_tree(rmap_item)
│   ├── checksum = calc_checksum(page)  ← xxhash 快速校验
│   ├── checksum 变化 (与 oldchecksum 不同)?
│   │   └── 更新 oldchecksum, RETURN (页太不稳定)
│   └── try_to_merge_with_zero_page()  ← 内容全零则合并到零页
│
├── [C] 稳定树搜索:
│   ├── kfolio = stable_tree_search(page) ← memcmp_pages 逐字节比较
│   ├── 找到相同页?
│   │   ├── try_to_merge_with_ksm_page(rmap_item, page, kfolio)
│   │   │   └── write_protect_page() → pages_identical() → replace_page()
│   │   └── stable_tree_append(rmap_item, stable_node)
│   │   └── RETURN
│
├── [D] 非稳定树搜索/插入:
│   ├── tree_rmap_item = unstable_tree_search_insert(rmap_item, page, &tree_page)
│   ├── 找到匹配页?
│   │   ├── try_to_merge_two_pages(rmap_item, page, tree_rmap_item, tree_page)
│   │   │   ├── write_protect_page(page)  → 设为 KSM 页 (stable_node=NULL)
│   │   │   ├── write_protect_page(tree_page) + replace_page(tree_page → page)
│   │   │   └── 页内容现在共享同一物理页
│   │   ├── stable_tree_insert(kfolio)  ← 插入稳定树
│   │   ├── stable_tree_append(tree_rmap_item)
│   │   └── stable_tree_append(rmap_item)
│   └── 无匹配 → 插入非稳定树，等下次扫描
```

**checksum 过滤的意义**：只有连续两次扫描 checksum 相同的页才进入非稳定树，避免处理频繁变化的页（"volatile" 页）。这是一个两级过滤：
1. 第一次扫描：计算 checksum，记录到 `oldchecksum`
2. 第二次扫描：如果 checksum 不变 → 插入非稳定树
3. 非稳定树中找到匹配 → 合并，进入稳定树

---

## 写保护机制

`write_protect_page()` (`mm/ksm.c:1245-1334`) 是 KSM 合并的前提——只有写保护的页才能被信任内容不变：

```
write_protect_page(vma, folio, orig_pte)
│
├── page_vma_mapped_walk() — 找到 PTE
├── mmu_notifier_invalidate_range_start()
│
├── PTE 是可写/脏/anon_exclusive/TLB flush pending?
│   ├── flush_cache_page(vma, addr, pfn)
│   ├── entry = ptep_clear_flush(vma, addr, ptep)  ← 清除 PTE + TLB flush
│   │   // 关键：先清 PTE 再检查 refcount，防止 O_DIRECT 竞争
│   ├── 检查 refcount == mapcount + 1 + swapped?
│   │   └── 不等 → 恢复 PTE, 返回失败 (有 O_DIRECT 等在进行)
│   ├── anon_exclusive?
│   │   └── folio_try_share_anon_rmap_pte() → 失败则恢复 PTE
│   ├── pte_dirty → folio_mark_dirty()  // 保留脏状态到 folio
│   ├── entry = pte_mkclean(entry)       // 清除 PTE 脏位
│   ├── entry = pte_wrprotect(entry)      // 移除写权限
│   └── set_pte_at(mm, addr, ptep, entry) // 安装只读 PTE
│
├── *orig_pte = entry ← 保存原始 PTE，供 replace_page 使用
└── mmu_notifier_invalidate_range_end()
```

**ARM64 PTE 写保护细节** (`arch/arm64/include/asm/pgtable.h:319-331`)：

```c
static inline pte_t pte_wrprotect(pte_t pte)
{
    // 硬件脏 (PTE_WRITE/DBM=1, PTE_RDONLY=0) → 转软件脏 (PTE_DIRTY)
    if (pte_hw_dirty(pte))
        pte = set_pte_bit(pte, __pgprot(PTE_DIRTY));

    pte = clear_pte_bit(pte, __pgprot(PTE_WRITE));   // 清 DBM/WRITE
    pte = set_pte_bit(pte, __pgprot(PTE_RDONLY));     // 设只读
    return pte;
}
```

ARM64 的 DBM (Dirty Bit Management) 协议：`PTE_WRITE` 和 `PTE_DBM` 共享 bit 51，写保护时必须先将硬件脏状态转移为软件 `PTE_DIRTY` (bit 55)，再清 `PTE_WRITE` 设 `PTE_RDONLY`。

---

## 页替换与合并流程

`replace_page()` (`mm/ksm.c:1345-1437`) 将进程的私有页替换为共享的 KSM 页：

```
replace_page(vma, page, kpage, orig_pte)
│
├── 找到 PMD 和 PTE
├── mmu_notifier_invalidate_range_start()
├── pte_offset_map_lock() — 获取 PTL
├── pte_same(ptep_get(ptep), orig_pte)? — 验证 PTE 未变
│
├── kpage 不是零页:
│   ├── folio_get(kfolio)                  ← 增加 KSM 页引用
│   ├── folio_add_anon_rmap_pte(kfolio)    ← 加入 KSM 页 rmap
│   └── newpte = mk_pte(kpage, vma->vm_page_prot) ← 只读 PTE
│
├── kpage 是零页:
│   ├── newpte = pte_mkdirty(pte_mkspecial(pfn_pte(...)))
│   ├── ksm_map_zero_page(mm)             ← 记录零页数
│   └── dec_mm_counter(mm, MM_ANONPAGES)  ← 减匿名页计数
│
├── flush_cache_page()
├── ptep_clear_flush()                     ← 清除旧 PTE + TLB flush
├── set_pte_at(mm, addr, ptep, newpte)     ← 安装新 PTE
├── folio_remove_rmap_pte(folio, page)     ← 移除旧页 rmap
├── folio_put(folio)                        ← 释放旧页引用
└── mmu_notifier_invalidate_range_end()
```

`try_to_merge_one_page()` (`mm/ksm.c:1448-1506`) 封装了写保护 + 比对 + 替换的完整流程：

```c
static int try_to_merge_one_page(struct vm_area_struct *vma,
                                 struct page *page, struct page *kpage)
{
    if (page == kpage)         return 0;  // KSM fork：页已共享
    if (!folio_test_anon(folio)) goto out; // 只合匿名页
    if (!folio_trylock(folio))  goto out;  // 非阻塞，锁不住就跳过
    if (folio_test_large(folio)) split_huge_page(); // THP 先拆分

    if (write_protect_page(vma, folio, &orig_pte) == 0) {
        if (!kpage) {
            // 第一次合并：将页标记为 KSM 页
            folio_set_stable_node(folio, NULL);  // mapping → KSM 标识
            folio_mark_dirty(folio);              // 确保 swap 可用
            err = 0;
        } else if (pages_identical(page, kpage)) {
            // 替换为已有的 KSM 页
            err = replace_page(vma, page, kpage, orig_pte);
        }
    }
}
```

---

## COW 断裂与写缺页处理

当进程写入 KSM 合并页时，ARM64 硬件触发 permission fault → 软件走 COW 路径：

```
ARM64 硬件:
  PTE_RDONLY=1, PTE_WRITE=0 → level 3 permission fault (FSC=0x0f)

ARM64 do_page_fault() (arch/arm64/mm/fault.c:552):
  → ESR_ELx_WNR=1 → FAULT_FLAG_WRITE
  → handle_mm_fault() → handle_pte_fault()
  → !pte_write(entry) → do_wp_page()

do_wp_page() (mm/memory.c:4049):
  → folio_test_ksm(folio) == true
  → wp_can_reuse_anon_folio() 返回 false (KSM 页不可复用)
  → count_vm_event(COW_KSM)         ← KSM COW 统计
  → wp_page_copy(vmf)               ← COW 复制

wp_page_copy() (mm/memory.c:3658):
  → 分配新私有页: vma_alloc_zeroed_movable_folio()
  → 复制内容: copy_user_highpage() [ARM64 实现]
  │   ├── copy_page()           ← ARM64 汇编优化的页复制
  │   ├── MTE 标签复制 (如果启用)
  │   └── flush_dcache_page()   ← D-cache 清理
  → ptep_clear_flush()           ← BBM: 清旧 PTE + TLB flush
  → set_pte_at() + maybe_mkwrite() ← 安装可写新 PTE
```

**关键：KSM 页永远走 `wp_page_copy()` 而非 `wp_page_reuse()`**。

`wp_can_reuse_anon_folio()` (`mm/memory.c:3986`) 对 KSM folio 显式返回 `false`：

```c
if (folio_test_ksm(folio) || folio_ref_count(folio) > 3)
    return false;
```

因为 KSM 页被多个进程共享，不能简单复用——必须 COW 分配新页。

### `break_ksm()` — 强制断裂 (`mm/ksm.c:623-678`)

`MADV_UNMERGEABLE` 或进程退出时强制断裂 KSM 共享：

```c
break_ksm(vma, addr, lock_vma)
  → folio_walk_start() 找到 folio
  → folio_test_ksm(folio)?
    → handle_mm_fault(vma, addr, FAULT_FLAG_UNSHARE | FAULT_FLAG_REMOTE)
    → 触发 COW 路径，复制出私有页
  → 循环直到该地址不再有 KSM 页
```

---

## 零页优化

KSM 可将全零的匿名页合并到系统零页，节省物理内存分配 (`mm/ksm.c:1512-1544`)：

```c
static int try_to_merge_with_zero_page(struct ksm_rmap_item *rmap_item,
                                        struct page *page)
{
    // checksum == zero_checksum && ksm_use_zero_pages?
    // → try_to_merge_one_page(vma, page, ZERO_PAGE(0))
}
```

`zero_checksum` 在 `ksm_init()` (`mm/ksm.c:3941`) 中预计算：

```c
static unsigned int zero_checksum __read_mostly;
// ksm_init() 中:
zero_checksum = calc_checksum(ZERO_PAGE(0));
```

`replace_page()` 中零页 PTE 的构造 (`mm/ksm.c:1405`):

```c
newpte = pte_mkdirty(pte_mkspecial(pfn_pte(page_to_pfn(kpage), vma->vm_page_prot)));
ksm_map_zero_page(mm);
dec_mm_counter(mm, MM_ANONPAGES);  // 零页不算匿名页
```

**用 `PTE_DIRTY` 标记零页 PTE**：通过检查零页 PTE 的 dirty bit 来区分 KSM 零页和普通零页映射。

---

## 稳定树与非稳定树

### 稳定树

- **红黑树**，按页内容排序（`memcmp_pages()` 返回值作为排序键）
- 页是**写保护的**，内容可信，不会被偷偷修改
- **永不清空**，跨扫描轮次持久存在
- 每个 `stable_node` 的 `hlist` 挂载所有共享此页的 `rmap_item`

`stable_tree_search()` (`mm/ksm.c:1798-2003`)：
- 遍历红黑树，在每个节点调用 `memcmp_pages(scan_page, node_page)` 比对
- `<0` 左子树，`>0` 右子树，`==0` 找到匹配
- 处理 stale 节点（物理页已释放）和 NUMA chain/dup

`stable_tree_insert()` (`mm/ksm.c:2012-2090`)：
- 标准红黑树插入，`memcmp_pages()` 确定位置
- 内容重复时创建 chain + dup

### 非稳定树

- **红黑树**，按页内容排序
- 页**不是写保护的**，内容可能随时变化
- **每轮全扫描清空重建**（`root_unstable_tree[nid] = RB_ROOT`）
- 只接受**连续两次 checksum 相同**的页

`unstable_tree_search_insert()` (`mm/ksm.c:2107-2171`)：
- 搜索比对 → 找到匹配返回 `tree_rmap_item`
- 无匹配 → 将当前 `rmap_item` 插入树中

**为什么需要非稳定树？**：新页首次扫描时 checksum 通过但尚未写入非稳定树；第二次扫描 checksum 不变才插入非稳定树，第三次扫描时可能找到匹配页合并。这是两轮稳定性保证。

---

## NUMA 与 merge_across_nodes

```c
// mm/ksm.c:471-476
#ifdef CONFIG_NUMA
static unsigned int ksm_merge_across_nodes = 1;  // 默认：跨 NUMA 合并
#else
#define ksm_merge_across_nodes  1U
#endif
```

| 模式 | 树结构 | 合并范围 |
|------|--------|----------|
| `merge_across_nodes=1` | 单对稳定/非稳定树 | 任何 NUMA 节点 |
| `merge_across_nodes=0` | 每 NUMA 节点各一对树 | 仅同 NUMA 节点 |

**Chain 机制**：当 KSM 页迁移到不同 NUMA 节点时，原 stable_node 变为 "chain"，链接多个 "dup" stable_node 代表同一内容在不同节点上的副本。

- `alloc_stable_node_chain()` (`mm/ksm.c:780`) 创建 chain
- `stable_node_chain_add_dup()` (`mm/ksm.c:532`) 添加 dup
- `chain_prune()` (`mm/ksm.c:1775`) 定期清理过期 dup（默认 2000ms）

**切换 `merge_across_nodes`**：需要先执行 `KSM_RUN_UNMERGE` 解除所有合并，再重建树。

---

## rmap 反向映射集成

`rmap_walk_ksm()` (`mm/ksm.c:3111-3183`) — 从 KSM 页找到所有虚拟映射，用于页迁移、hwpoison、unmap：

```
rmap_walk_ksm(folio, rmap_walk_control)
│
├── stable_node = folio_stable_node(folio)
├── hlist_for_each_entry(rmap_item, &stable_node->hlist, hlist):
│   ├── anon_vma = rmap_item->anon_vma
│   ├── anon_vma_interval_tree_foreach(vmac):
│   │   ├── addr = rmap_item->address & PAGE_MASK
│   │   ├── 第一轮：rmap_item->mm == vma->vm_mm (原始 mm)
│   │   └── 第二轮：search_new_forks (fork 的 mm)
│   └── rwc->rmap_one(folio, vma, addr, rwc->arg)
```

`folio_migrate_ksm()` (`mm/ksm.c:3226-3249`) — KSM 页迁移时将 stable_node 移到 `migrate_nodes` 列表。

`collect_procs_ksm()` (`mm/ksm.c:3185-3224`) — 内存故障处理时收集所有映射 KSM 页的进程。

---

## ksmd 内核线程与扫描流程

### ksmd 线程 (`mm/ksm.c:2768-2793`)

```c
static int ksm_scan_thread(void *nothing)
{
    set_freezable();
    set_user_nice(current, 5);  // 低优先级，nice=5

    while (!kthread_should_stop()) {
        mutex_lock(&ksm_thread_mutex);
        wait_while_offlining();
        if (ksmd_should_run())
            ksm_do_scan(ksm_thread_pages_to_scan);  // 默认 100 页/轮
        mutex_unlock(&ksm_thread_mutex);

        if (ksmd_should_run()) {
            sleep_ms = READ_ONCE(ksm_thread_sleep_millisecs);  // 默认 20ms
            wait_event_freezable_timeout(ksm_iter_wait, ...,
                                         msecs_to_jiffies(sleep_ms));
        } else {
            wait_event_freezable(ksm_thread_wait, ksmd_should_run());
        }
    }
}
```

### ksm_do_scan (`mm/ksm.c:2747-2761`)

```c
static void ksm_do_scan(unsigned int scan_npages)
{
    while (scan_npages-- && likely(!freezing(current))) {
        cond_resched();
        rmap_item = scan_get_next_rmap_item(&page);
        if (!rmap_item) return;
        cmp_and_merge_page(page, rmap_item);  // 核心算法
        put_page(page);
        ksm_pages_scanned++;
    }
}
```

### scan_get_next_rmap_item (`mm/ksm.c:2547-2740`)

页遍历器，遍历所有注册 mm 的 `VM_MERGEABLE` VMA：

1. 全扫描开始 → 清空非稳定树、drain LRU、prune migrate_nodes
2. 遍历 mm_slot 链表中的每个 mm
3. 对每个 mm 的 `VM_MERGEABLE` VMA：用 `walk_page_range_vma()` 找到匿名页
4. 对每个找到的页：`flush_anon_page()` + `flush_dcache_page()` + 获取/创建 rmap_item

---

## madvise 与 prctl 入口

### `ksm_madvise()` (`mm/ksm.c:2934-2971`)

```c
// mm/madvise.c:1404-1410
case MADV_MERGEABLE:
case MADV_UNMERGEABLE:
    error = ksm_madvise(vma, range->start, range->end, behavior, &new_flags);
```

**`MADV_MERGEABLE`**：
- 检查 `vma_ksm_compatible()` — 排除 shared/special/hugetlb/file-backed VMA
- 如果 mm 没有 `MMF_VM_MERGEABLE` → `__ksm_enter(mm)` → 创建 mm_slot，加入扫描列表
- 设置 `VM_MERGEABLE` 标志

**`MADV_UNMERGEABLE`**：
- 如果 VMA 有 anon_vma → `unmerge_ksm_pages()` → 逐页 `break_ksm()`
- 清除 `VM_MERGEABLE` 标志

### `ksm_enable_merge_any()` (`mm/ksm.c:2875-2893`)

`prctl(PR_SET_MEMORY_MERGE)` 设置 `MMF_VM_MERGE_ANY`，使得所有新建的兼容 VMA 自动标记 `VM_MERGEABLE`。

### 生命周期钩子 (`include/linux/ksm.h`)

| 钩子 | 时机 | 动作 |
|------|------|------|
| `ksm_fork()` | fork | 如果父 mm 有 `MMF_VM_MERGEABLE`，子 mm 加入扫描 |
| `__ksm_exit()` | mm 退出 | 清理 mm_slot 和所有 rmap_item |
| `ksm_execve()` | execve | 如果 `MMF_VM_MERGE_ANY`，重新注册 |

---

## ARM64 特有机制

### MTE 标签与 memcmp_pages

**这是 ARM64 与 KSM 最关键的交互点**。`arch/arm64/kernel/mte.c:72-94` 覆写了通用 `memcmp_pages()`：

```c
int memcmp_pages(struct page *page1, struct page *page2)
{
    char *addr1 = page_address(page1);
    char *addr2 = page_address(page2);
    int ret = memcmp(addr1, addr2, PAGE_SIZE);

    // 数据内容相同，但至少一个页有 MTE 标签？
    if (!system_supports_mte() || ret)
        return ret;

    if (page_mte_tagged(page1) || page_mte_tagged(page2))
        return addr1 != addr2;  // 返回非零，阻止 KSM 合并！

    return ret;  // 0 = 内容相同 + 无标签 → 可以合并
}
```

**原因**：如果 KSM 合并有 MTE 标签的页，`__set_ptes()` → `mte_sync_tags()` 可能清零或修改另一页的标签，破坏内存安全。因此 ARM64 **禁止 KSM 合并带 MTE 标签的页**。

### PTE 写保护与 DBM 协议

ARM64 的 `PTE_WRITE` 和 `PTE_DBM` 共享 bit 51。写保护时必须遵守硬件脏/写协议 (`arch/arm64/include/asm/pgtable.h:426-439`)：

```
 Dirty  Writable | PTE_RDONLY  PTE_WRITE(PTE_DBM)  PTE_DIRTY(sw)
   0      0      |   1           0                    0         ← KSM 合并后状态
   0      1      |   1           1                    0
   1      0      |   1           0                    1         ← KSM 合并前可能状态
   1      1      |   0           1                    x         ← COW 断裂后状态
```

**KSM 写保护关键步骤**：
1. `pte_wrprotect()` 先检查 `pte_hw_dirty(pte)`（`PTE_WRITE=1, PTE_RDONLY=0`）
2. 如果硬件脏 → `set_pte_bit(PTE_DIRTY)` 保存脏状态
3. `clear_pte_bit(PTE_WRITE)` + `set_pte_bit(PTE_RDONLY)`
4. 结果：KSM 页处于 `{RDONLY=1, WRITE=0, DIRTY=0/1}` 状态

**COW 断裂恢复写权限** (`pte_mkwrite_novma()`, pgtable.h:293-299):
```c
pte = set_pte_bit(pte, __pgprot(PTE_WRITE));
if (pte_sw_dirty(pte))
    pte = clear_pte_bit(pte, __pgprot(PTE_RDONLY));
```

### Contiguous PTE 与写保护冲突

ARM64 的 `PTE_CONT` (bit 52) 让连续 16 个 PTE (4K 页) 共享一个 TLB entry。**KSM 写保护单个页时可能与 contig 映射冲突**。

`wrprotect_ptes()` (`arch/arm64/include/asm/pgtable.h:1873-1894`)：

```c
static __always_inline void wrprotect_ptes(struct mm_struct *mm,
                unsigned long addr, pte_t *ptep, unsigned int nr)
{
    if (likely(nr == 1)) {
        pte_t orig_pte = __ptep_get(ptep);
        if (unlikely(pte_cont(orig_pte))) {
            __contpte_try_unfold(mm, addr, ptep, orig_pte);  // 必须先拆开!
            orig_pte = pte_mknoncont(orig_pte);
        }
        ___ptep_set_wrprotect(mm, addr, ptep, orig_pte);
    } else {
        contpte_wrprotect_ptes(mm, addr, ptep, nr);
    }
}
```

`contpte_wrprotect_ptes()` (`arch/arm64/mm/contpte.c:537-553`)：

```c
void contpte_wrprotect_ptes(struct mm_struct *mm, unsigned long addr,
                            pte_t *ptep, unsigned int nr)
{
    // 如果写保护整个 contig 范围 → 不需要 unfold
    // 只设置 wrprotect，等 mmu_gather flush 时统一 TLB invalidation
    // 如果是部分范围 → 必须 unfold（不能让 contig 内部分只读部分可写）
    contpte_try_unfold_partial(mm, addr, ptep, nr);
    __wrprotect_ptes(mm, addr, ptep, nr);
}
```

**关键约束**：ARM64 硬件要求 contig block 内所有 PTE 权限一致。如果 KSM 只写保护一个页（部分 contig 范围），必须先 unfold 整个 contig block，代价包括：
1. `__ptep_get_and_clear()` 清除所有 CONT_PTES 个 PTE
2. TLB flush（BBML2 系统可跳过中间 flush）
3. `__set_ptes()` 重写为非 contig 的 PTE

### BBM 与 COW 断裂 TLB 维护

`wp_page_copy()` 中的 BBM 序列：

```
ptep_clear_flush(vma, addr, ptep)
  → __ptep_get_and_clear()  ← 清除旧 PTE
  → flush_tlb_page()         ← ARM64: DSB ISHST → TLBI VAE1IS → DSB ISH
                              ← 确保 TLB 中旧映射失效

set_pte_at(mm, addr, ptep, newpte)
  → WRITE_ONCE(*ptep, newpte)
  → ARM64 用户映射: 无额外 barrier
```

`pgattr_change_is_safe()` (`arch/arm64/mm/mmu.c:127-164`) 判断 PTE 属性变更是否需要 BBM：

- **`PTE_RDONLY`/`PTE_WRITE` 变更** → **安全，无需 BBM**（KSM 写保护可直接修改）
- **`PTE_CONT` 变更** → **不安全，必须 BBM**（contig 映射需要先清除再重设）
- **PFN 变更** → **不安全，必须 BBM**（COW 断裂必须走 ptep_clear_flush + set_pte_at）

### MTE 标签在 COW 复制中的传播

`copy_highpage()` (`arch/arm64/mm/copypage.c:17-66`) 确保 COW 断裂时 MTE 标签正确复制：

```c
void copy_highpage(struct page *to, struct page *from)
{
    copy_page(kto, kfrom);  // ARM64 汇编优化的页内容复制

    if (!system_supports_mte()) return;

    if (page_mte_tagged(from)) {
        try_page_mte_tagging(to);
        mte_copy_page_tags(kto, kfrom);  // LDGM + STGM 指令复制标签
        set_page_mte_tagged(to);
    }
}
```

**COW 断裂完整流程**：KSM 共享页有 MTE 标签 → 写缺页 → `wp_page_copy()` → `copy_user_highpage()` → `copy_page()` 复制数据 + `mte_copy_page_tags()` 复制标签 → 新私有页数据+标签完整。

### Errata #2645198 与权限变更

`modify_prot_start_ptes()` (`arch/arm64/mm/mmu.c:2084-2101`)：受 errata #2645198 影响的 CPU，从 executable 变为 non-executable 的权限变更必须走 BBM。

KSM 页通常是数据页（不可执行），不受此 errata 影响。但如果 `mprotect(PROT_EXEC)` 应用到 KSM 区域后又取消执行权限，此 workaround 会生效。

---

## 延迟与跳过机制

### Page Volatility（checksum 变化）

`cmp_and_merge_page()` (`mm/ksm.c:2259-2262`)：

```c
checksum = calc_checksum(page);
if (rmap_item->oldchecksum != checksum) {
    rmap_item->oldchecksum = checksum;
    return;  // 页内容变化频繁，跳过
}
```

只有**连续两次 checksum 相同**的页才进入非稳定树——这是 KSM 的核心稳定性保证。

### Smart Scan (`ksm_smart_scan`)

默认启用，跳过不可能被去重的页：
- `rmap_age_t remaining_skips` — 跳过剩余扫描轮数
- `should_skip_rmap_item()` (`mm/ksm.c`) — 根据年龄和跳过计数决定是否跳过

### max_page_sharing 限制

默认 256：每个 stable_node 最多挂载 256 个 rmap_item。超过后新页无法合并到该 KSM 页，必须等待已有共享被 COW 断裂释放。

---

## sysfs 接口与调优参数

`/sys/kernel/mm/ksm/` 下的所有参数 (`mm/ksm.c:3899-3932`)：

| 参数 | R/W | 默认值 | 说明 |
|------|-----|--------|------|
| `run` | RW | 0 | 0=停止, 1=开始合并, 2=解除合并 |
| `sleep_millisecs` | RW | 20 | 扫描间隔 (ms) |
| `pages_to_scan` | RW | 100 | 每轮扫描页数 |
| `pages_shared` | RO | — | 稳定树节点数（唯一 KSM 页数） |
| `pages_sharing` | RO | — | 共享 KSM 页的额外映射数 |
| `pages_unshared` | RO | — | 非稳定树节点数 |
| `pages_volatile` | RO | — | checksum 变化频繁的页数 |
| `pages_scanned` | RO | — | 总扫描页数 |
| `full_scans` | RO | — | 完成全扫描轮数 |
| `merge_across_nodes` | RW | 1 | 是否跨 NUMA 合并（切换需先 unmerge） |
| `max_page_sharing` | RW | 256 | 每稳定节点最大 rmap_item 数 |
| `use_zero_pages` | RW | 0 | 是否合并全零页到系统零页 |
| `smart_scan` | RW | 1 | 是否启用智能扫描跳过 |
| `ksm_zero_pages` | RO | — | KSM 零页数 |
| `stable_node_chains` | RO | — | Chain 节点数 |
| `stable_node_dups` | RO | — | Dup 节点数 |
| `stable_node_chains_prune_millisecs` | RW | 2000 | Chain 修剪间隔 |
| `advisor_mode` | RW | none | "none" 或 "scan-time" |
| `advisor_max_cpu` | RW | 70 | Advisor 最大 CPU% |
| `general_profit` | RO | — | `pages_sharing - pages_shared - overhead` |

**调优建议**：

| 场景 | pages_to_scan | sleep_millisecs | advisor_mode |
|------|--------------|----------------|-------------|
| KVM 虚拟机 | 100-1000 | 20-50 | scan-time |
| 容器环境 | 100-500 | 50-100 | scan-time |
| 保守模式 | 100 | 200 | none |

---

## vmstat 与 Tracepoint

### vmstat 计数器

| 计数器 | 何时增加 |
|--------|----------|
| `COW_KSM` | 写缺页到 KSM 页触发 COW 复制 (`mm/memory.c:4139`) |
| `KSM_SWPIN_COPY` | swap-in 需要复制 KSM 页 (`mm/ksm.c:3104`) |

### Tracepoint (`include/trace/events/ksm.h`)

| Tracepoint | 触发位置 | 参数 |
|------------|----------|------|
| `ksm_start_scan` | 全扫描开始 | seqnr, rmap_entries |
| `ksm_stop_scan` | 全扫描结束 | seqnr, rmap_entries |
| `ksm_enter` | mm 注册到 KSM | mm |
| `ksm_exit` | mm 从 KSM 退出 | mm |
| `ksm_merge_one_page` | 单页合并完成 | pfn, rmap_item, mm, err |
| `ksm_merge_with_ksm_page` | 与已有 KSM 页合并 | ksm_page, pfn, rmap_item, mm, err |
| `ksm_remove_ksm_page` | KSM 页从稳定树移除 | pfn |
| `ksm_remove_rmap_item` | rmap_item 移除 | pfn, rmap_item, mm |
| `ksm_advisor` | Advisor 调整扫描参数 | scan_time, pages_to_scan, cpu_percent |

---

## QEMU 实验

### 实验 1：构建 ARM64 内核与 KSM 启动验证

**目标**：构建支持 KSM 的 ARM64 内核，验证 ksmd 线程和 sysfs 接口可用。

```bash
# 1. 配置内核（确保 KSM 启用）
cd /home/ybzhang/kernel/linux-6.18.1
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig

# 验证 KSM 配置
grep CONFIG_KSM .config
# 期望: CONFIG_KSM=y

# 2. 编译内核
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)

# 3. 创建 rootfs（含测试工具）
mkdir -p rootfs
cat > rootfs/init << 'EOF'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t debugfs debugfs /sys/kernel/debug
mount -t tmpfs tmpfs /tmp

echo "=== KSM Initialization Check ==="
echo "Kernel: $(uname -r)"
echo "Arch: $(uname -m)"
cat /proc/meminfo | grep -i ksm

# 检查 KSM sysfs
echo "=== KSM sysfs ==="
ls /sys/kernel/mm/ksm/
cat /sys/kernel/mm/ksm/run
cat /sys/kernel/mm/ksm/pages_shared
cat /sys/kernel/mm/ksm/pages_sharing

# 检查 ksmd 线程
ps -e | grep ksmd || echo "ksmd not found (may need KSM run=1)"

exec /bin/sh
EOF
chmod +x init

# 创建 cpio rootfs
find . | cpio -o -H newc > ../rootfs.cpio
cd ..

# 4. 启动 QEMU
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
- `/sys/kernel/mm/ksm/` 目录存在
- `run` 默认为 0（需手动启动）
- `pages_shared` 和 `pages_sharing` 为 0

### 实验 2：madvise 触发 KSM 合并观察

**目标**：编写测试程序，通过 `madvise(MADV_MERGEABLE)` 注册页到 KSM，观察合并过程。

```c
// ksm_test.c — KSM 合并测试程序
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

#define PAGE_SIZE 4096
#define NR_PAGES 100  // 400KB

int main() {
    // 启动 ksmd
    FILE *f = fopen("/sys/kernel/mm/ksm/run", "w");
    fprintf(f, "1");
    fclose(f);

    // 设置扫描参数（加速合并）
    f = fopen("/sys/kernel/mm/ksm/pages_to_scan", "w");
    fprintf(f, "1000");
    fclose(f);
    f = fopen("/sys/kernel/mm/ksm/sleep_millisecs", "w");
    fprintf(f, "50");
    fclose(f);

    // 分配多个内存区域，内容相同
    void *regions[10];
    char pattern[] = "KSM test data pattern for merging demonstration";

    for (int i = 0; i < 10; i++) {
        regions[i] = mmap(NULL, NR_PAGES * PAGE_SIZE,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (regions[i] == MAP_FAILED) {
            perror("mmap");
            return 1;
        }
        // 写入相同内容
        for (int j = 0; j < NR_PAGES; j++) {
            memcpy(regions[i] + j * PAGE_SIZE, pattern, sizeof(pattern));
        }
    }

    // 注册到 KSM
    for (int i = 0; i < 10; i++) {
        if (madvise(regions[i], NR_PAGES * PAGE_SIZE, MADV_MERGEABLE) < 0) {
            printf("madvise MERGEABLE failed for region %d: %s\n", i, strerror(errno));
        } else {
            printf("Region %d: MADV_MERGEABLE set\n", i);
        }
    }

    // 等待 ksmd 扫描并合并
    printf("Waiting for KSM to merge (10 seconds)...\n");
    for (int t = 0; t < 10; t++) {
        sleep(1);
        f = fopen("/sys/kernel/mm/ksm/pages_shared", "r");
        int shared;
        fscanf(f, "%d", &shared);
        fclose(f);
        f = fopen("/sys/kernel/mm/ksm/pages_sharing", "r");
        int sharing;
        fscanf(f, "%d", &sharing);
        fclose(f);
        printf("  t=%d: pages_shared=%d, pages_sharing=%d, saved=%d KB\n",
               t, shared, sharing, sharing * 4);
    }

    // 验证写入仍然正常（COW 断裂）
    printf("Writing to region 0 to trigger COW...\n");
    memset(regions[0], 0x42, PAGE_SIZE);  // 修改第一页
    printf("  Write succeeded (COW break)\n");

    sleep(2);
    f = fopen("/sys/kernel/mm/ksm/pages_shared", "r");
    fscanf(f, "%d", &shared);
    fclose(f);
    f = fopen("/sys/kernel/mm/ksm/pages_sharing", "r");
    fscanf(f, "%d", &sharing);
    fclose(f);
    printf("After COW: pages_shared=%d, pages_sharing=%d\n", shared, sharing);

    // 取消 KSM 注册
    for (int i = 0; i < 10; i++) {
        madvise(regions[i], NR_PAGES * PAGE_SIZE, MADV_UNMERGEABLE);
    }

    sleep(2);
    printf("After unmerge:\n");
    f = fopen("/sys/kernel/mm/ksm/pages_shared", "r");
    fscanf(f, "%d", &shared);
    fclose(f);
    printf("  pages_shared=%d\n", shared);

    return 0;
}
```

```bash
# 交叉编译
aarch64-linux-gnu-gcc -static ksm_test.c -o rootfs/ksm_test
# 重建 rootfs 并启动 QEMU 运行
```

**验证要点**：
- `pages_shared` 从 0 逐步增长
- `pages_sharing` 应大于 `pages_shared`（多个进程共享同一页）
- COW 写入后 `pages_sharing` 减少
- UNMERGE 后 `pages_shared` 回到 0

### 实验 3：手动控制 ksmd 运行与合并统计

**目标**：通过 sysfs 控制 ksmd，观察不同参数下的合并效率。

```bash
# 在 QEMU 系统中

# 1. 启动 ksmd
echo 1 > /sys/kernel/mm/ksm/run

# 2. 调整扫描参数
echo 100 > /sys/kernel/mm/ksm/pages_to_scan   # 慢速
echo 200 > /sys/kernel/mm/ksm/sleep_millisecs

# 3. 运行测试程序后观察
cat /sys/kernel/mm/ksm/pages_scanned
cat /sys/kernel/mm/ksm/pages_shared
cat /sys/kernel/mm/ksm/pages_sharing
cat /sys/kernel/mm/ksm/pages_unshared
cat /sys/kernel/mm/ksm/pages_volatile
cat /sys/kernel/mm/ksm/full_scans
cat /sys/kernel/mm/ksm/general_profit

# 4. 加速扫描
echo 1000 > /sys/kernel/mm/ksm/pages_to_scan
echo 20 > /sys/kernel/mm/ksm/sleep_millisecs

# 5. 使用 advisor (自动调优)
echo scan-time > /sys/kernel/mm/ksm/advisor_mode

# 6. 停止 ksmd
echo 0 > /sys/kernel/mm/ksm/run
```

### 实验 4：COW 断裂验证

**目标**：验证写入 KSM 合并页时 COW 断裂的正确性和 vmstat 统计。

```bash
# 在 QEMU 系统中

# 1. 记录初始 COW_KSM 计数
grep cow_ksm /proc/vmstat
# 期望: cow_ksm 0

# 2. 启动 KSM 并运行合并测试
echo 1 > /sys/kernel/mm/ksm/run
./ksm_test  # 实验 2 的测试程序

# 3. 观察 COW_KSM 增长
grep cow_ksm /proc/vmstat
# 期望: cow_ksm > 0 (写入合并页触发 COW)

# 4. 批量触发 COW（写所有合并页）
# 编写 stress 测试
cat > /tmp/cow_stress.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>

#define SIZE (100 * 4096)

int main() {
    void *buf = mmap(NULL, SIZE, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    memset(buf, 0xAA, SIZE);              // 初始化
    madvise(buf, SIZE, MADV_MERGEABLE);    // 注册 KSM

    sleep(5);  // 等待合并

    // 触发 COW：逐页写入
    for (int i = 0; i < 100; i++) {
        ((char*)buf)[i * 4096] = 0xBB;
    }

    sleep(1);
    return 0;
}
EOF

# 交叉编译运行后
grep cow_ksm /proc/vmstat
```

### 实验 5：零页合并验证

**目标**：验证 KSM 零页优化：全零页合并到系统零页而非分配新物理页。

```bash
# 1. 启用零页合并
echo 1 > /sys/kernel/mm/ksm/use_zero_pages
echo 1 > /sys/kernel/mm/ksm/run

# 2. 分配全零页并注册 KSM
cat > /tmp/zero_page_test.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>

#define SIZE (50 * 4096)

int main() {
    void *buf1 = mmap(NULL, SIZE, PROT_READ|PROT_WRITE,
                      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    void *buf2 = mmap(NULL, SIZE, PROT_READ|PROT_WRITE,
                      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    // 两个区域全是零（mmap 已清零）
    madvise(buf1, SIZE, MADV_MERGEABLE);
    madvise(buf2, SIZE, MADV_MERGEABLE);

    printf("Two zero regions registered, waiting for merge...\n");
    sleep(10);

    FILE *f = fopen("/sys/kernel/mm/ksm/ksm_zero_pages", "r");
    int zero_pages;
    fscanf(f, "%d", &zero_pages);
    fclose(f);
    printf("ksm_zero_pages = %d\n", zero_pages);

    f = fopen("/sys/kernel/mm/ksm/pages_sharing", "r");
    int sharing;
    fscanf(f, "%d", &sharing);
    fclose(f);
    printf("pages_sharing = %d\n", sharing);

    return 0;
}
EOF

# 3. 禁用零页合并对比
echo 0 > /sys/kernel/mm/ksm/use_zero_pages
# 重复测试，观察 pages_shared vs ksm_zero_pages 的差异
```

### 实验 6：ftrace 追踪 KSM 合并过程

**目标**：使用 tracepoint 观察合并的详细过程。

```bash
# 1. 启用 KSM tracepoint
echo 1 > /sys/kernel/debug/tracing/events/ksm/enable

# 2. 启动 KSM 和测试
echo 1 > /sys/kernel/mm/ksm/run
./ksm_test

# 3. 读取 trace
sleep 5
cat /sys/kernel/debug/tracing/trace | head -30

# 4. 过滤关键事件
cat /sys/kernel/debug/tracing/trace | grep ksm_merge_one_page
cat /sys/kernel/debug/tracing/trace | grep ksm_merge_with_ksm_page
cat /sys/kernel/debug/tracing/trace | grep ksm_start_scan
cat /sys/kernel/debug/tracing/trace | grep ksm_stop_scan

# 5. 清理
echo 0 > /sys/kernel/debug/tracing/events/ksm/enable
echo > /sys/kernel/debug/tracing/trace
```

**期望 trace 格式**：
```
ksmd-XX  [001] ... 100.0: ksm_start_scan: seqnr=1 rmap_entries=200
ksmd-XX  [001] ... 100.1: ksm_merge_one_page: pfn=0x1234 err=0
ksmd-XX  [001] ... 100.2: ksm_merge_with_ksm_page: ksm_page=0x5678 err=0
ksmd-XX  [001] ... 100.5: ksm_stop_scan: seqnr=1 rmap_entries=100
```

### 实验 7：NUMA 环境下 merge_across_nodes 对比

**目标**：在模拟 NUMA 环境下对比跨节点合并与同节点合并的差异。

```bash
# 1. 启动 2 节点 NUMA QEMU
qemu-system-aarch64 \
  -machine virt \
  -cpu cortex-a57 \
  -m 512M,slots=2,maxmem=1G \
  -kernel arch/arm64/boot/Image \
  -initrd rootfs.cpio \
  -append "console=ttyAMA0 numa=fake=2" \
  -nographic \
  -smp 2

# 2. 查看节点布局
cat /sys/devices/system/node/online
ls /sys/devices/system/node/

# 3. 对比 merge_across_nodes=1 vs 0
echo 1 > /sys/kernel/mm/ksm/merge_across_nodes
echo 1 > /sys/kernel/mm/ksm/run
# 运行测试...
cat /sys/kernel/mm/ksm/pages_shared

# 切换到不跨节点（需先 unmerge）
echo 2 > /sys/kernel/mm/ksm/run  # 先 unmerge
sleep 10
echo 0 > /sys/kernel/mm/ksm/merge_across_nodes
echo 1 > /sys/kernel/mm/ksm/run  # 重新开始
# 运行测试...
cat /sys/kernel/mm/ksm/pages_shared
# 跨节点页无法合并，pages_shared 应更少
```

### 实验 8：MTE 环境下 KSM 行为验证

**目标**：验证 MTE 标签阻止 KSM 合并的行为。

```bash
# 1. 启动支持 MTE 的 QEMU（需要 ARM64 CPU with MTE）
qemu-system-aarch64 \
  -machine virt,mte=on \
  -cpu max \
  -m 512M \
  -kernel arch/arm64/boot/Image \
  -initrd rootfs.cpio \
  -append "console=ttyAMA0 kasan=off" \
  -nographic

# 2. 检查 MTE 支持
cat /proc/cpuinfo | grep mte
# 或
sysctl kernel.mte

# 3. 测试：带标签 vs 不带标签页的合并差异
cat > /tmp/mte_ksm_test.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>
#include <sys/prctl.h>

#define SIZE (50 * 4096)

int main() {
    // 启用 MTE 标签
    if (prctl(PR_SET_TAGGED_ADDR_CTRL,
              PR_TAGGED_ADDR_ENABLE | (0xfffe << PR_MTE_TAG_SHIFT),
              0, 0, 0) < 0) {
        printf("MTE not available, skipping tagged test\n");
        return 1;
    }

    // 分配带标签的内存
    void *tagged = mmap(NULL, SIZE, PROT_READ|PROT_WRITE|PROT_MTE,
                        MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    memset(tagged, 0xAA, SIZE);  // 内容相同

    // 分配不带标签的内存（相同内容）
    void *untagged = mmap(NULL, SIZE, PROT_READ|PROT_WRITE,
                          MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    memset(untagged, 0xAA, SIZE);  // 内容相同

    madvise(tagged, SIZE, MADV_MERGEABLE);
    madvise(untagged, SIZE, MADV_MERGEABLE);

    echo 1 > /sys/kernel/mm/ksm/run;
    sleep(10);

    // MTE 标签的页不应被合并
    FILE *f = fopen("/sys/kernel/mm/ksm/pages_shared", "r");
    int shared;
    fscanf(f, "%d", &shared);
    fclose(f);
    printf("pages_shared = %d\n", shared);
    printf("Expected: only untagged pages merged, tagged pages blocked\n");

    return 0;
}
EOF

# 4. 无 MTE 的对比（关闭 MTE）
# 不带 -machine virt,mte=on 的 QEMU 启动
# 所有页无标签 → 应能全部合并
```

**验证要点**：
- 带 MTE 标签的页不会被 KSM 合并（`memcmp_pages` 返回非零）
- 不带标签但内容相同的页正常合并
- 无 MTE 系统中所有相同内容页正常合并

---

## 源码阅读路线图

建议按以下顺序阅读：

### 第 1 步：理解入口 (`include/linux/ksm.h` + `mm/madvise.c`)
- `ksm_madvise()` — madvise 入口
- `__ksm_enter()` / `__ksm_exit()` — mm 注册/注销
- `ksm_fork()` / `ksm_execve()` — 生命周期钩子

### 第 2 步：理解数据结构 (`mm/ksm.c:120-250`)
- `ksm_stable_node` — 稳定树节点
- `ksm_rmap_item` — 反向映射项
- `ksm_mm_slot` — mm 扫描状态
- `ksm_scan` — 全局扫描游标
- 稳定树和非稳定树根

### 第 3 步：理解页标识 (`include/linux/page-flags.h:717-752`)
- `FOLIO_MAPPING_KSM` 编码
- `folio_test_ksm()` / `folio_stable_node()` / `folio_set_stable_node()`

### 第 4 步：理解扫描流程 (`mm/ksm.c:2547-2793`)
- `scan_get_next_rmap_item()` — 页遍历器
- `ksm_do_scan()` — 扫描主循环
- `ksm_scan_thread()` — ksmd 线程

### 第 5 步：理解核心算法 (`mm/ksm.c:2221-2359`)
- `cmp_and_merge_page()` — **核心决策流程**
- `calc_checksum()` (line 1236) — xxhash 快速过滤
- checksum 变化跳过逻辑 (line 2259)

### 第 6 步：理解写保护 (`mm/ksm.c:1245-1334`)
- `write_protect_page()` — PTE 写保护
- O/DIRECT 竞争防护（先清 PTE 再检查 refcount）
- `anon_exclusive` 处理

### 第 7 步：理解合并与替换 (`mm/ksm.c:1345-1610`)
- `replace_page()` (line 1345) — 页替换
- `try_to_merge_one_page()` (line 1448) — 单页合并
- `try_to_merge_two_pages()` (line 1591) — 双页合并
- `try_to_merge_with_zero_page()` (line 1512) — 零页合并

### 第 8 步：理解树操作 (`mm/ksm.c:1798-2171`)
- `stable_tree_search()` (line 1798) — 稳定树搜索
- `stable_tree_insert()` (line 2012) — 稳定树插入
- `unstable_tree_search_insert()` (line 2107) — 非稳定树搜索+插入

### 第 9 步：理解 COW 断裂 (`mm/memory.c` + `mm/ksm.c:623`)
- `do_wp_page()` (memory.c:4049) — 写缺页处理
- `wp_page_copy()` (memory.c:3658) — COW 复制
- `break_ksm()` (ksm.c:623) — 强制断裂
- `wp_can_reuse_anon_folio()` — KSM 页不可复用

### 第 10 步：理解 ARM64 特有机制
- `arch/arm64/kernel/mte.c:72` — `memcmp_pages()` MTE 标签阻止合并
- `arch/arm64/mm/copypage.c:17` — `copy_highpage()` MTE 标签 COW 复制
- `arch/arm64/include/asm/pgtable.h:319` — `pte_wrprotect()` DBM 协议
- `arch/arm64/mm/contpte.c:537` — `contpte_wrprotect_ptes()` contig 冲突
- `arch/arm64/mm/mmu.c:127` — `pgattr_change_is_safe()` BBM 判断
- `arch/arm64/mm/fault.c:552` — permission fault → COW

---

## 关键源码索引

| 文件 | 行号 | 函数/结构 | 说明 |
|------|------|-----------|------|
| `mm/ksm.c` | 126 | `struct ksm_mm_slot` | mm 扫描槽 |
| `mm/ksm.c` | 140 | `struct ksm_scan` | 全局扫描游标 |
| `mm/ksm.c` | 159 | `struct ksm_stable_node` | 稳定树节点 |
| `mm/ksm.c` | 201 | `struct ksm_rmap_item` | 反向映射项 |
| `mm/ksm.c` | 623 | `break_ksm()` | 强制断裂 KSM 共享 |
| `mm/ksm.c` | 1059 | `folio_stable_node()` | 从 folio 取 stable_node |
| `mm/ksm.c` | 1064 | `folio_set_stable_node()` | 设置 KSM 页标识 |
| `mm/ksm.c` | 1236 | `calc_checksum()` | xxhash 页校验 |
| `mm/ksm.c` | 1245 | `write_protect_page()` | **写保护核心** |
| `mm/ksm.c` | 1345 | `replace_page()` | **页替换核心** |
| `mm/ksm.c` | 1448 | `try_to_merge_one_page()` | **单页合并核心** |
| `mm/ksm.c` | 1512 | `try_to_merge_with_zero_page()` | 零页合并 |
| `mm/ksm.c` | 1591 | `try_to_merge_two_pages()` | 双页合并 |
| `mm/ksm.c` | 1798 | `stable_tree_search()` | 稳定树搜索 |
| `mm/ksm.c` | 2012 | `stable_tree_insert()` | 稳定树插入 |
| `mm/ksm.c` | 2107 | `unstable_tree_search_insert()` | 非稳定树搜索+插入 |
| `mm/ksm.c` | 2221 | `cmp_and_merge_page()` | **核心决策算法** |
| `mm/ksm.c` | 2547 | `scan_get_next_rmap_item()` | 页遍历器 |
| `mm/ksm.c` | 2747 | `ksm_do_scan()` | 扫描主循环 |
| `mm/ksm.c` | 2768 | `ksm_scan_thread()` | **ksmd 线程** |
| `mm/ksm.c` | 2934 | `ksm_madvise()` | madvise 入口 |
| `mm/ksm.c` | 3111 | `rmap_walk_ksm()` | KSM 反向映射遍历 |
| `mm/ksm.c` | 3226 | `folio_migrate_ksm()` | KSM 页迁移 |
| `mm/ksm.c` | 3935 | `ksm_init()` | KSM 初始化 (subsys_initcall) |
| `include/linux/ksm.h` | — | `ksm_fork()/exit()/execve()` | 生命周期钩子 |
| `include/linux/page-flags.h` | 717 | `FOLIO_MAPPING_KSM` | KSM 页标识编码 |
| `mm/madvise.c` | 1404 | `MADV_MERGEABLE case` | madvise 分发 |
| `mm/memory.c` | 4049 | `do_wp_page()` | 写缺页 → COW |
| `mm/memory.c` | 3658 | `wp_page_copy()` | COW 复制流程 |
| `mm/memory.c` | 3986 | `wp_can_reuse_anon_folio()` | KSM 页不可复用 |
| `mm/util.c` | 1035 | `memcmp_pages()` (generic) | 通用页比较 |
| `arch/arm64/kernel/mte.c` | 72 | `memcmp_pages()` (ARM64) | **MTE 阻止合并** |
| `arch/arm64/mm/copypage.c` | 17 | `copy_highpage()` | COW 复制 + MTE 标签 |
| `arch/arm64/mm/copypage.c` | 68 | `copy_user_highpage()` | 用户页复制 + dcache flush |
| `arch/arm64/include/asm/pgtable.h` | 319 | `pte_wrprotect()` | ARM64 写保护 + DBM |
| `arch/arm64/include/asm/pgtable.h` | 293 | `pte_mkwrite_novma()` | COW 后恢复写权限 |
| `arch/arm64/include/asm/pgtable.h` | 426 | PTE Dirty/Write 协议表 | DBM 协议说明 |
| `arch/arm64/include/asm/pgtable.h` | 1457 | `__ptep_set_wrprotect()` | 原子写保护 |
| `arch/arm64/include/asm/pgtable.h` | 1873 | `wrprotect_ptes()` | 公共写保护 API |
| `arch/arm64/mm/contpte.c` | 49 | `contpte_convert()` | contig PTE unfold/fold |
| `arch/arm64/mm/contpte.c` | 537 | `contpte_wrprotect_ptes()` | **contig 写保护** |
| `arch/arm64/mm/mmu.c` | 127 | `pgattr_change_is_safe()` | BBM 判断（RDONLY/WRITE 安全） |
| `arch/arm64/mm/fault.c` | 552 | `do_page_fault()` | ARM64 缺页入口 |
| `arch/arm64/mm/fault.c` | 207 | `__ptep_set_access_flags()` | 原子 PTE 权限升级 |
| `include/trace/events/ksm.h` | — | 各 tracepoint | KSM 追踪事件 |

---

> 本文档基于 Linux 6.18.1 源码分析生成，所有行号和函数签名均对应当前工作区代码。