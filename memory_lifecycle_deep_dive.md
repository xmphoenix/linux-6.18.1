# Linux ARM64 内存生命周期深度分析

> 基于 Linux 6.18.1 / ARM64 / `CONFIG_NUMA=y` / QEMU 1GB  
> 记录日期：2026-04-05

---

## 目录

<details>
<summary><a href="#1-memblock-→-buddy-过渡全流程">1. memblock → Buddy 过渡全流程</a></summary>

- [1.1 整体时序](#11-整体时序)
- [1.2 Phase 1：建立 memblock（`setup_arch`）](#12-phase-1建立-memblocksetup_arch)
- [1.3 Phase 2：过渡核心 `memblock_free_all()`（mm/memblock.c:2339）](#13-phase-2过渡核心-memblock_free_allmmmemblockc2339)
- [1.4 两种区域的不同命运](#14-两种区域的不同命运)
- [1.5 嵌入式 UMA（`!CONFIG_NUMA`）下的简化](#15-嵌入式-umaconfig_numa下的简化)
- [1.6 过渡前后状态对比](#16-过渡前后状态对比)

</details>

<details>
<summary><a href="#2-arm64-虚拟内存-vma-知识体系">2. ARM64 虚拟内存 VMA 知识体系</a></summary>

- [2.1 ARM64 虚拟地址空间布局](#21-arm64-虚拟地址空间布局)
- [2.2 核心数据结构](#22-核心数据结构)
- [2.3 VMA 组织：Maple Tree（Linux 6.1+ 关键变化）](#23-vma-组织maple-treelinux-61-关键变化)
- [2.4 重要 `vm_flags` 标志](#24-重要-vm_flags-标志)
- [2.5 缺页异常处理决策树（最核心）](#25-缺页异常处理决策树最核心)
- [2.6 COW（写时复制）机制](#26-cow写时复制机制)
- [2.7 反向映射（RMAP）](#27-反向映射rmap)
- [2.8 Per-VMA Lock（Linux 6.4+，此内核已启用）](#28-per-vma-locklinux-64此内核已启用)
- [2.9 `vm_operations_struct`：VMA 多态](#29-vm_operations_structvma-多态)

</details>

<details>
<summary><a href="#3-slub-与-buddy-的双向交互">3. SLUB 与 Buddy 的双向交互</a></summary>

- [3.1 向 Buddy 申请内存（分配路径）](#31-向-buddy-申请内存分配路径)
- [3.2 向 Buddy 归还内存（释放路径）](#32-向-buddy-归还内存释放路径)
- [3.3 `min_partial` 水位：保留多少不还](#33-min_partial-水位保留多少不还)
- [3.4 主动回收：触发 `discard_slab` 的三种时机](#34-主动回收触发-discard_slab-的三种时机)
- [3.5 最终归还调用链](#35-最终归还调用链)
- [3.6 不会归还的例外](#36-不会归还的例外)

</details>

<details>
<summary><a href="#4-三大主题的统一视角">4. 三大主题的统一视角</a></summary>

- [4.1 内存管理完整层次图](#41-内存管理完整层次图)
- [4.2 关键问题汇总](#42-关键问题汇总)
- [4.3 嵌入式系统关注点](#43-嵌入式系统关注点)

</details>

---

## 1. memblock → Buddy 过渡全流程

### 1.1 整体时序

```
start_kernel()
  └─ setup_arch()                          ← Phase 1：建立 memblock
       ├─ arm64_memblock_init()            ← 填充 memblock 数据库
       ├─ paging_init()                    ← 用 memblock 建立四级页表
       └─ bootmem_init()                   ← 初始化 zone 空骨架
            ├─ sparse_init()              ← memblock_alloc() 分配 struct page 数组
            └─ zone_sizes_init()
                 └─ free_area_init()      ← 初始化 free_area 空链表
  └─ mm_core_init()                        ← Phase 2：执行过渡
       ├─ build_all_zonelists()
       ├─ memblock_free_all()             ← ★ 过渡核心
       ├─ mem_init()
       └─ kmem_cache_init()               ← buddy 已就绪，SLUB 初始化
```

### 1.2 Phase 1：建立 memblock（`setup_arch`）

#### `arm64_memblock_init()`（arch/arm64/mm/init.c:185）

从 DTB 扫描物理内存后，对 `memblock` 数据库进行修剪：

```c
// 裁剪超出线性映射窗口的物理地址（VA_BITS=48 时线性映射窗口 = 128TB）
memblock_remove(memstart_addr + linear_region_size, ULLONG_MAX);

// 保留内核镜像自身（_text → _end）
memblock_reserve(__pa_symbol(_text), _end - _text);

// 扫描 FDT /reserved-memory 节点
early_init_fdt_scan_reserved_mem();
```

此时两张表状态：
- `memblock.memory` = 所有可用物理页区间  
- `memblock.reserved` = 内核镜像 + DTB + initrd + FDT 保留区

#### `paging_init()`

调用 `memblock_phys_alloc()` 分配页目录页（PGD/PUD/PMD/PTE），建立 ARM64 四级页表，使内核可通过线性映射访问全部物理内存。

**这是 memblock 作为"分配器"的最后一批重要分配。**

#### `bootmem_init()` → `sparse_init()` + `zone_sizes_init()`（arch/arm64/mm/init.c:292）

```c
void __init bootmem_init(void)
{
    min = PFN_UP(memblock_start_of_DRAM());
    max = PFN_DOWN(memblock_end_of_DRAM());
    
    arch_numa_init();              // 解析 NUMA 拓扑（QEMU 单节点 node_data[0]）
    sparse_init();                 // memblock_alloc() 分配 struct page 数组内存
    zone_sizes_init();             // 设置 max_zone_pfns[]，调用 free_area_init()
}
```

`free_area_init()` 里的 `zone_init_free_lists()` 初始化所有空链表（mm/mm_init.c:1436）：

```c
static void __meminit zone_init_free_lists(struct zone *zone)
{
    for_each_migratetype_order(order, t) {
        INIT_LIST_HEAD(&zone->free_area[order].free_list[t]); // 空链表
        zone->free_area[order].nr_free = 0;                   // 计数为 0
    }
}
```

> **关键**：此时 buddy 的 zone 骨架已创建，但 `free_list` 全空，buddy **不可用**。  
> `struct page` 数组已分配，每个 page 的 `_refcount = 1`（标记"已分配"）。

### 1.3 Phase 2：过渡核心 `memblock_free_all()`（mm/memblock.c:2339）

```c
void __init memblock_free_all(void)
{
    free_unused_memmap();               // 1. 释放空洞对应的 struct page 内存
    reset_all_zones_managed_pages();    // 2. 清零 zone->managed_pages 计数
    memblock_clear_kho_scratch_only();
    pages = free_low_memory_core_early(); // 3. ★ 核心：把空闲页送入 buddy
    totalram_pages_add(pages);          // 4. 更新 totalram_pages
}
```

#### 步骤 3：`free_low_memory_core_early()`

```c
static unsigned long __init free_low_memory_core_early(void)
{
    // ① 给 reserved 区域的 struct page 打 PageReserved 标记
    memmap_init_reserved_pages();   // → reserve_bootmem_region() → __SetPageReserved()

    // ② 遍历所有 free 区间（= memory - reserved）
    for_each_free_mem_range(i, NUMA_NO_NODE, MEMBLOCK_NONE, &start, &end, NULL)
        count += __free_memory_core(start, end);
    
    return count;
}
```

#### 对齐优化：`__free_pages_memory()`（mm/memblock.c:2199）

```c
static void __init __free_pages_memory(unsigned long start, unsigned long end)
{
    while (start < end) {
        // __ffs(start) = start 的最低有效位 = 最大对齐 order
        order = start ? min_t(int, MAX_PAGE_ORDER, __ffs(start)) : MAX_PAGE_ORDER;
        while (start + (1UL << order) > end)
            order--;   // 缩小到不超出边界

        memblock_free_pages(pfn_to_page(start), start, order); // ★ 送入 buddy
        start += (1UL << order);
    }
}
```

**设计目的**：保证 buddy 的对齐不变式（order-n 块的 PFN 必须是 2^n 的倍数）。

#### 最终入 buddy：`memblock_free_pages()` → `__free_pages_core()`（mm/mm_init.c:2483）

```c
void __init memblock_free_pages(struct page *page, unsigned long pfn, unsigned int order)
{
    clear_page_tag_ref(page);
    __free_pages_core(page, order, MEMINIT_EARLY);
}

void __meminit __free_pages_core(struct page *page, unsigned int order, ...)
{
    for (loop = 0; loop < nr_pages; loop++, p++) {
        __ClearPageReserved(p);      // 清除 Reserved 标志
        set_page_count(p, 0);        // refcount：1 → 0（标记为 free）
    }
    atomic_long_add(nr_pages, &page_zone(page)->managed_pages);
    
    // FPI_TO_TAIL：绕过 PCP，直接插到 buddy 链表尾部
    __free_pages_ok(page, order, FPI_TO_TAIL);
    //  └── free_one_page()
    //       └── __free_one_page()   ← 插入 free_area[order].free_list[mt]
    //            └── buddy 合并算法（检查伙伴并向上合并）
}
```

### 1.4 两种区域的不同命运

| 区域类型 | 在 `memblock.reserved` 中？ | 结果 |
|----------|---------------------------|------|
| 未被 reserved 的 memory 页 | 否 | `for_each_free_mem_range` → 送入 buddy |
| 内核镜像 `_text`~`_end` | 是 | `reserve_bootmem_region()` 打 `PageReserved`，不入 buddy |
| DTB / initrd | 是 | 同上，永久 Reserved |
| sparse mem_map（struct page 数组） | 是 | 不入 buddy，永久使用 |

### 1.5 嵌入式 UMA（`!CONFIG_NUMA`）下的简化

整个流程**结构完全相同**，差异仅在规模：

| 项目 | NUMA | UMA 嵌入式 |
|------|------|-----------|
| `NODE_DATA(nid)` | `node_data[nid]` 数组 | 直接 `&contig_page_data` |
| `free_area_init()` 初始化节点数 | `nr_node_ids` 个 | 始终 1 个 |
| `__free_one_page()` 中 nid 判断 | 同 nid buddy 才合并 | 无此约束 |
| `SLUB_TINY` 配置 | MIN_PARTIAL=5 | `MIN_PARTIAL=0`（全空立即归还）|

### 1.6 过渡前后状态对比

```
─────────── bootmem_init 结束 ───────────
memblock.memory:   [0x40000000, 0x80000000)  ← 完整记录（QEMU 1GB）
memblock.reserved: [0x40080000, 0x41000000)  ← 内核等保留区
zone.free_area[*]: 全空链表                  ← buddy 存在但无页
struct page 数组:  已分配，_refcount=1       ← 标记为"已分配"状态

─────────── memblock_free_all 结束 ──────
memblock 数据:     仍在内存，静默废弃        ← 不再使用
zone.managed_pages: ~200000（约 800MB）
free_area[10].free_list[MIGRATE_MOVABLE]: 有大块（1024页）
free_area[9..0]:   有零散边缘块
totalram_pages:    约 240000（1GB / 4KB）
```

---

## 2. ARM64 虚拟内存 VMA 知识体系

### 2.1 ARM64 虚拟地址空间布局

ARM64 采用双页表基址寄存器（TTBR0/TTBR1），VA_BITS=48 时：

```
0x0000_0000_0000_0000
│   用户态（TTBR0）        [0, TASK_SIZE_64 = 1<<vabits_actual = 256TB)
│   ├── text/data/bss      ← ELF 加载
│   ├── heap               ← start_brk → brk，向高地址增长
│   ├── mmap 区域          ← 从 mmap_base 向低地址分配（top-down）
│   └── stack              ← start_stack，VM_GROWSDOWN，向低地址增长
│
0x0000_FFFF_FFFF_FFFF  ← 用户态上边界（256TB）
─ ─ ─ ─ ─ 无效洞 ─ ─ ─ ─ ─
0xFFFF_0000_0000_0000
│   内核态（TTBR1）
│   ├── MODULES_VADDR      ← 内核模块（.ko）
│   ├── VMALLOC_START      ← vmalloc / ioremap 区域
│   ├── KIMAGE_VADDR       ← 内核镜像（.text .data）
│   └── PAGE_OFFSET        ← 线性映射（直接映射全部物理内存）
0xFFFF_FFFF_FFFF_FFFF
```

- `TASK_SIZE_64 = UL(1) << vabits_actual`（arch/arm64/include/asm/processor.h:56）
- mm_struct 中 `task_size` 字段记录此值

### 2.2 核心数据结构

#### `mm_struct` — 进程地址空间描述符（include/linux/mm_types.h:944）

```c
struct mm_struct {
    struct maple_tree mm_mt;        // ★ VMA 索引树（Linux 6.1+ 替代红黑树）
    unsigned long mmap_base;        // mmap 区域起始（受 ASLR 随机化）
    unsigned long task_size;        // 用户地址空间上限
    pgd_t *pgd;                     // 四级页表根（TTBR0 存物理地址）

    struct rw_semaphore mmap_lock;  // 全局大锁，保护 VMA 树
    seqcount_t mm_lock_seq;         // 与 PER_VMA_LOCK 配合（序列号）

    int map_count;                  // VMA 数量（/proc/PID/maps 行数）
    atomic_t mm_users;              // 线程引用（降到 0 触发 exit_mmap）
    atomic_t mm_count;              // 内核持有（降到 0 释放 mm_struct）

    // 地址空间边界
    unsigned long start_code, end_code;
    unsigned long start_data, end_data;
    unsigned long start_brk, brk;   // heap 当前上边界
    unsigned long start_stack;      // 栈起始虚拟地址
};
```

#### `vm_area_struct` — 单个虚拟内存区域（include/linux/mm_types.h:813）

```c
struct vm_area_struct {
    unsigned long vm_start;              // VMA 覆盖 [vm_start, vm_end)
    unsigned long vm_end;

    struct mm_struct *vm_mm;             // 所属进程
    pgprot_t vm_page_prot;               // → ARM64 PTE 访问权限位
    vm_flags_t vm_flags;                 // 类型和权限标志

    const struct vm_operations_struct *vm_ops; // 操作函数表（多态）

    // 文件映射
    unsigned long vm_pgoff;              // 文件偏移（页单位）
    struct file *vm_file;                // NULL = 匿名映射

    // 反向映射
    struct anon_vma *anon_vma;           // 匿名页反向映射根
    struct list_head anon_vma_chain;     // 串联到 anon_vma 的链

    // 文件共享映射：插入 i_mmap interval tree
    struct { struct rb_node rb; unsigned long rb_subtree_last; } shared;

#ifdef CONFIG_PER_VMA_LOCK              // 此内核已启用（CONFIG_PER_VMA_LOCK=y）
    unsigned int vm_lock_seq;            // 细粒度读锁序列号
    refcount_t vm_refcnt;                // VMA 引用计数
#endif
};
```

**vm_area_struct 从 SLUB 分配**（mm/vma_init.c:22）：

```c
// 初始化专用 kmem_cache
vm_area_cachep = kmem_cache_create("vm_area_struct",
    sizeof(struct vm_area_struct), &args,
    SLAB_HWCACHE_ALIGN | SLAB_TYPESAFE_BY_RCU | SLAB_ACCOUNT);

// 分配
vma = kmem_cache_alloc(vm_area_cachep, GFP_KERNEL);  // → per-CPU slab
```

> **连接点**：VMA 对象本身由 SLUB 分配，是 SLUB 服务上层虚拟内存管理的典型例子。

### 2.3 VMA 组织：Maple Tree（Linux 6.1+ 关键变化）

```c
// 以虚拟地址区间 [vm_start, vm_end) 为 key
mm->mm_mt (maple_tree)
  ├── [0x400000, 0x401000) → vma1 (.text)
  ├── [0x600000, 0x601000) → vma2 (.data)
  └── [0x7fff0000, ...) → vma3 (stack)

// 查找（O(log n)，区间查询优化）
vma = vma_lookup(mm, addr);

// 迭代
VMA_ITERATOR(vmi, mm, addr);
for_each_vma(vmi, vma) { ... }
```

**6.1 之前**：红黑树（`mm_rb`）+ 链表（`mmap`），两个结构分别维护。  
**6.1 之后**：Maple Tree 合二为一，区间查找更优，减少 cache miss。

### 2.4 重要 `vm_flags` 标志

| 标志 | 含义 | ARM64 PTE 映射 |
|------|------|----------------|
| `VM_READ` | 可读 | AP 字段设置 |
| `VM_WRITE` | 可写 | AP[2]=0（read-write）|
| `VM_EXEC` | 可执行 | UXN=0（User eXecute Never 清零）|
| `VM_SHARED` | MAP_SHARED | 写直接到 page cache |
| `VM_GROWSDOWN` | 向低地址增长 | 栈，`VM_STACK = VM_GROWSDOWN`（ARM64）|
| `VM_PFNMAP` | 纯 PFN 映射，无 struct page | 设备 MMIO 映射（嵌入式常用）|
| `VM_LOCKED` | mlock() 锁定，不可换出 | 实时系统防缺页抖动 |
| `VM_HUGETLB` | 大页映射 | ARM64 使用 PMD/PUD 级页表条目 |

### 2.5 缺页异常处理决策树（最核心）

#### ARM64 异常入口（arch/arm64/mm/fault.c:924）

```
硬件触发 Data Abort / Instruction Abort（ESR_ELx 编码故障类型）
  └── do_mem_abort()
       └── arm64_abort_table[] 按 ESR 分发
            ├── Translation Fault (level 0-3) → do_translation_fault()
            │    └── do_page_fault(far, esr, regs)
            ├── Access Flag Fault              → do_page_fault()
            └── Permission Fault               → do_page_fault()
```

#### 通用处理路径（mm/memory.c）

```
do_page_fault(far=fault_addr)
  ├── vma = vma_lookup(mm, addr)        ← Maple Tree 查找所属 VMA
  ├── 检查 VMA 权限 vs 访问类型         ← 无 VMA → SIGSEGV (SEGV_MAPERR)
  │                                        权限不符 → SIGSEGV (SEGV_ACCERR)
  ├── [PER_VMA_LOCK 快路径]
  │    └── handle_mm_fault(vma, addr, FAULT_FLAG_VMA_LOCK)
  └── [mmap_lock 慢路径]
       └── handle_mm_fault(vma, addr, mm_flags)
            └── handle_pte_fault(vmf)
                 ├── PTE 不存在 → do_pte_missing()
                 │    ├── vma_is_anonymous() == true
                 │    │    └── do_anonymous_page()    ← ★ 匿名缺页
                 │    └── vm_file != NULL
                 │         └── do_fault()             ← ★ 文件缺页
                 │              ├── 只读   → do_read_fault()
                 │              ├── 私有写 → do_cow_fault()   ← COW
                 │              └── 共享写 → do_shared_fault()
                 ├── PTE 存在但写保护   → do_wp_page()  ← ★ COW 触发
                 ├── PTE 是 swap entry → do_swap_page() ← 换入
                 └── PTE 为 PROT_NONE  → do_numa_page() ← NUMA 迁移
```

#### 匿名缺页关键路径（mm/memory.c:5134）

```c
// 只读访问 → 映射零页（zero_page），延迟物理分配，零成本
if (!(flags & FAULT_FLAG_WRITE))
    entry = pte_mkspecial(pfn_pte(my_zero_pfn(addr), vma->vm_page_prot));

// 写访问 → 从 buddy 分配真实物理页
folio = alloc_anon_folio(vmf);           // → alloc_pages(ZONE_NORMAL, 0)
__folio_mark_uptodate(folio);
entry = folio_mk_pte(folio, vma->vm_page_prot);
set_pte_at(mm, addr, vmf->pte, entry);  // 写入 PTE，建立虚实映射
```

### 2.6 COW（写时复制）机制

`fork()` 时 `copy_page_range()` 将父子进程的可写私有页 PTE 都设为只读（write-protect）：

```
Permission Fault（ARM64: AP[2]=1 → 写触发）
  └── do_wp_page()
       ├── folio_mapcount() == 1（只有自己映射）
       │    └── page_mkwrite()  / pte_mkwrite()  ← 直接提升为可写，无拷贝
       └── folio_mapcount() > 1（共享）
            └── wp_page_copy()
                 ├── alloc_folio()           ← 从 buddy 分配新页
                 ├── copy_user_highpage()    ← 复制内容（物理拷贝）
                 └── 更新 PTE 指向新页      ← 父子独立，解除共享
```

### 2.7 反向映射（RMAP）

kswapd 回收时需要知道某物理页被哪些进程的哪个 VMA 映射了：

```
struct folio / struct page
  └── folio->mapping = (struct anon_vma *)   ← 匿名页反向映射根

struct anon_vma                              ← 根节点（rwsem 保护）
  └── interval_tree: [start, end) → anon_vma_chain

struct anon_vma_chain                        ← 桥接节点
  ├── struct anon_vma *anon_vma              ← 指向父/祖先 anon_vma
  └── struct vm_area_struct *vma            ← 指向具体 VMA

// fork 后结构（父子进程共享同一 anon_vma 根）：
   父 anon_vma ← AVC ← 父 VMA
               ← AVC ← 子 VMA
```

**目的**：`rmap_walk_anon()` 找到所有映射该页的 VMA → `unmap_mapping_range()` 解除映射 → 页面可回收。

### 2.8 Per-VMA Lock（Linux 6.4+，此内核已启用）

**问题**：`mmap_lock` 是进程级读写信号量，高并发缺页时写锁排队严重。

**解决**：为每个 VMA 加细粒度锁：

```
快路径（不需要修改 VMA 结构时）：
  vma_start_read(vma)           ← 只锁单个 VMA
  handle_mm_fault(..., FAULT_FLAG_VMA_LOCK)
  vma_end_read(vma)

慢路径（VMA 结构需要变更时）：
  mmap_read_lock(mm)            ← 全局读锁，兼容旧语义
```

**不变式**：`mm->mm_lock_seq` 奇数 = mmap_lock 写锁中（VMA 可能变更），偶数 = 已释放。缺页时通过比较 `vma->vm_lock_seq` 判断是否需要退化到慢路径。

### 2.9 `vm_operations_struct`：VMA 多态

| 回调 | 触发时机 | 典型实现 |
|------|----------|----------|
| `fault()` | 缺页读取文件页 | ext4/shmem → page cache 填充 |
| `page_mkwrite()` | 只读页升级为可写 | 文件系统标记 dirty |
| `map_pages()` | 缺页时预读周边页 | 减少连续缺页次数 |
| `open/close()` | VMA 创建/销毁 | 驱动引用计数 |
| `mprotect()` | `mprotect()` 系统调用 | 驱动自定义权限检查 |

---

## 3. SLUB 与 Buddy 的双向交互

### 3.1 向 Buddy 申请内存（分配路径）

当 per-CPU slab 和 node partial list 都耗尽时，SLUB 向 buddy 申请一整页（或多页）：

```c
// ___slab_alloc() 慢路径
new_slab = allocate_slab(s, flags, node)
  └── alloc_slab_page(s, alloc_gfp, node, oo)
       └── alloc_pages(alloc_gfp, oo_order(oo))  // ← 向 buddy 申请 2^order 页
```

申请到的页整体作为一个 slab 使用，并将 `struct folio` 标记为 slab（`__folio_set_slab()`）。

### 3.2 向 Buddy 归还内存（释放路径）

SLUB **会**还内存给 buddy，**但有条件**。条件是：**slab 全空** 且 **node 的 partial slab 数量 ≥ `min_partial`**。

#### 完整的内存流转图

```
kmem_cache_free(obj)
  │
  ▼
[per-CPU slab freelist]
  │  对象还入，slab 变全空
  ▼
对象全部释放？
  ├── 否 → 留在 per-CPU slab 或进 per-CPU partial list
  └── 是 → 检查 n->nr_partial >= s->min_partial
             ├── 否（partial 不足）→ 保留在 node partial list，不还
             └── 是（partial 充足）→ discard_slab()
                                          └── __free_slab()
                                               └── free_frozen_pages()
                                                    └── __free_pages_ok()
                                                         └── __free_one_page()
                                                              └── ★ 进入 buddy free_area[]
```

#### 关键决策代码（mm/slub.c:3784）

```c
// deactivate_slab 中的决策（CPU slab 换出时）
if (!new.inuse && n->nr_partial >= s->min_partial) {
    stat(s, DEACTIVATE_EMPTY);
    discard_slab(s, slab);     // ★ 直接还给 buddy
} else if (new.freelist) {
    add_partial(n, slab, tail); // 放入 partial list 留待复用
}

// __slab_free 中的决策（正常对象释放时）
if (unlikely(!new.inuse && n->nr_partial >= s->min_partial))
    goto slab_empty;   // → discard_slab() → ★ 还给 buddy
```

### 3.3 `min_partial` 水位：保留多少不还

```c
// 计算公式（mm/slub.c:8596）
s->min_partial = min_t(unsigned long, MAX_PARTIAL, ilog2(s->size) / 2);
s->min_partial = max_t(unsigned long, MIN_PARTIAL, s->min_partial);

// 默认常量（非 SLUB_TINY 配置）
#define MIN_PARTIAL   5    // 每个 node 至少保留 5 个 partial slab
#define MAX_PARTIAL  10    // 最多保留 10 个 partial slab
```

| 对象大小 | `ilog2(size)/2` | `min_partial` 实际值 |
|----------|-----------------|---------------------|
| 8 bytes  | 1               | 5（MIN_PARTIAL 下限）|
| 64 bytes | 3               | 5（MIN_PARTIAL 下限）|
| 256 bytes| 4               | 5（MIN_PARTIAL 下限）|
| 4096 bytes | 6             | 6                    |

可通过 `/sys/kernel/slab/<name>/min_partial` 动态调整。

**保留逻辑**：partial slab 中有部分对象仍在使用，保留它们避免反复向 buddy 申请/归还（thrashing）。

### 3.4 主动回收：触发 `discard_slab` 的三种时机

| 时机 | 触发条件 | 路径 |
|------|----------|------|
| **普通 free** | slab 全空 AND `nr_partial >= min_partial` | `__slab_free` → `discard_slab` |
| **内存压力** | kswapd 触发 `shrink_slab()` | `mm/vmscan.c:4955` → `__kmem_cache_do_shrink()` |
| **`kmem_cache_shrink()`** | 手动调用或内存 hotplug offline | 扫描所有 node partial，全空 slab 全部释放 |

#### `kmem_cache_shrink` 扫描逻辑（mm/slub.c:8264）

```c
static int __kmem_cache_do_shrink(struct kmem_cache *s)
{
    for_each_kmem_cache_node(s, node, n) {
        list_for_each_entry_safe(slab, t, &n->partial, slab_list) {
            int free = slab->objects - slab->inuse;
            
            if (free == slab->objects) {       // slab 完全空
                list_move(&slab->slab_list, &discard);
                n->nr_partial--;
            } else if (free <= SHRINK_PROMOTE_MAX)
                list_move(..., promote + free - 1); // 最满的提到链表头
        }
        // 批量释放，连 min_partial 也不保留（强制回收）
        list_for_each_entry_safe(slab, t, &discard, slab_list)
            free_slab(s, slab);                // ★ 全部还给 buddy
    }
}
```

### 3.5 最终归还调用链

```c
discard_slab(s, slab)
  └── free_slab(s, slab)
       ├── SLAB_TYPESAFE_BY_RCU（如 vm_area_struct）
       │    └── call_rcu(&slab->rcu_head, rcu_free_slab)  ← 延迟归还
       └── 普通 slab
            └── __free_slab(s, slab)                      // mm/slub.c:3290
                 ├── __folio_clear_slab()   // 清除 folio 的 slab 标志位
                 ├── folio->mapping = NULL  // 解除 slab 与地址空间的关联
                 └── free_frozen_pages(&folio->page, order)  // ★ 还给 buddy
                      └── __free_pages_ok(page, order, FPI_NONE)
                           └── __free_one_page()       // 插入 free_area[order]
                                └── buddy 合并，尝试合并为更大 order 块
```

### 3.6 不会归还的例外

1. **`SLAB_TYPESAFE_BY_RCU` 标志**（如 `vm_area_struct`）：全空 slab 需等 RCU grace period 后才归还，保证读者安全。
2. **`nr_partial < min_partial`**：保留全空 slab 备用，不归还。
3. **`SLUB_TINY` 配置**（极简嵌入式）：`MIN_PARTIAL = MAX_PARTIAL = 0`，全空 slab 立即归还。

---

## 4. 三大主题的统一视角

### 4.1 内存管理完整层次图

```
                         ┌─────────────────────────────────────┐
                         │         用户态进程                    │
                         │   malloc() → glibc ptmalloc2         │
                         └─────────────┬───────────────────────┘
                                       │ brk() / mmap() 系统调用
                         ┌─────────────▼───────────────────────┐
                         │         VMA 层（虚拟内存）            │
                         │   vm_area_struct + maple_tree         │
               ┌─────────┤   per-VMA lock / mmap_lock           ├──────────┐
               │         │   缺页处理决策树                      │          │
               │         └─────────────┬───────────────────────┘          │
               │                       │ alloc_pages() / do_anonymous_page │
               │         ┌─────────────▼───────────────────────┐          │
               │         │         SLUB 层（对象分配）           │          │
               │VMAs from│   kmem_cache（对象工厂）              │← 全空slab│
               │SLUB     │   kmem_cache_cpu（per-CPU 快路径）    │  还给     │
               │         │   kmem_cache_node（partial 池）       │  buddy   │
               └─────────┤   条件：nr_partial >= min_partial    ├──────────┘
                         └─────────────┬───────────────────────┘
                                       │ alloc_pages() / free_frozen_pages()
                         ┌─────────────▼───────────────────────┐
                         │         Buddy 层（页分配）            │
                         │   zone.free_area[0..10]              │
                         │   MIGRATE_MOVABLE/UNMOVABLE/RECLAIMABLE│
                         └─────────────┬───────────────────────┘
                                       │ memblock_free_all() 一次性过渡
                         ┌─────────────▼───────────────────────┐
                         │         Memblock 层（启动期）         │
                         │   memblock.memory / memblock.reserved│
                         │   启动完成后静默废弃                   │
                         └─────────────────────────────────────┘
```

### 4.2 关键问题汇总

| 问题 | 答案 | 关键代码位置 |
|------|------|-------------|
| memblock 何时退出历史舞台？ | `memblock_free_all()` 后不再使用，数据仍在内存但无人读 | mm/memblock.c:2339 |
| buddy 的 free_list 何时第一次有数据？ | `__free_one_page()` 在 `memblock_free_all()` 中首次被调用 | mm/page_alloc.c:940 |
| VMA 在哪里分配？ | 从 SLUB 的 `vm_area_cachep` 分配 | mm/vma_init.c:32 |
| 缺页时物理页从哪来？ | `alloc_anon_folio()` → buddy `alloc_pages()` | mm/memory.c:5134 |
| SLUB 全空 slab 何时还给 buddy？ | `nr_partial >= min_partial` 时立即还，否则保留 | mm/slub.c:3784 |
| `min_partial` 默认值？ | 5~10（由对象大小决定，ilog2(size)/2 取值） | mm/slub.c:8596 |
| 强制回收 SLUB 内存的方法？ | `kmem_cache_shrink()` 或内存压力触发 `shrink_slab()` | mm/slub.c:8324 |

### 4.3 嵌入式系统关注点

1. **内存紧张场景**：适当降低 `min_partial`（`/sys/kernel/slab/*/min_partial`）可以让 SLUB 更积极地将空 slab 还给 buddy
2. **实时性要求**：`VM_LOCKED` + `mlock()` 锁定关键 VMA，防止缺页抖动
3. **设备内存映射**：`VM_PFNMAP` 是将 MMIO 物理地址映射到用户态的标准方式，无 struct page 开销
4. **SLUB_TINY 配置**：`MIN_PARTIAL = 0`，全空 slab 立即归还，以延迟换内存节省
5. **UMA 过渡影响**：`!CONFIG_NUMA` 时 `NODE_DATA()` 直接返回 `&contig_page_data`，`__free_one_page` 无 nid 约束，buddy 合并效率更高

---

*文档基于 Linux 6.18.1 内核源码，ARM64 架构，GDB 实测数据。*  
*相关文档：`slub_analysis.md`（SLUB 深度分析）、`memory_subsystem_data_structures.md`（数据结构全景）*
