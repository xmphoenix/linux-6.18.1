# Linux Kernel Core Data Structures — Deep Reference (linux-6.18.1)

> All code excerpts are from the actual source tree at `/repo/ybzhang/kernel/linux-6.18.1`.

---

## 目录

<details>
<summary><a href="#1-linked-lists">1. Linked Lists</a></summary>

- [Struct Definitions](#struct-definitions)
- [Key API Functions](#key-api-functions)
- [Iteration Macros](#iteration-macros)
- [Kernel Usage Examples](#kernel-usage-examples)
- [核心实现源码逐行注释](#核心实现源码逐行注释)
- [hlist 的设计精妙之处](#hlist-的设计精妙之处)
- [Time Complexity](#time-complexity)

</details>

<details>
<summary><a href="#2-red-black-trees">2. Red-Black Trees</a></summary>

- [Struct Definitions](#struct-definitions-1)
- [Key API Functions](#key-api-functions-1)
- [Insert Pattern (Caller-implemented)](#insert-pattern-caller-implemented)
- [核心实现源码逐行注释 — 红黑树插入修复 ([rbtree.c](lib/rbtree.c#L89))](#核心实现源码逐行注释--红黑树插入修复-rbtreeclibrbtreecl89)
- [Kernel Usage Examples (Red-Black Tree)](#kernel-usage-examples-red-black-tree)
- [Time Complexity](#time-complexity-1)

</details>

<details>
<summary><a href="#3-xarray--radix-tree">3. XArray / Radix Tree</a></summary>

- [Struct Definitions](#struct-definitions-2)
- [Key API Functions](#key-api-functions-2)
- [内部节点结构 — `xa_node` ([xarray.h](include/linux/xarray.h#L1168))](#内部节点结构--xa_node-xarrayhincludelinuxxarrayhl1168)
- [Legacy Radix Tree API (Wrappers)](#legacy-radix-tree-api-wrappers)
- [Kernel Usage Examples](#kernel-usage-examples-1)
- [Time Complexity](#time-complexity-2)

</details>

<details>
<summary><a href="#4-hash-tables">4. Hash Tables</a></summary>

- ["Struct" Definition (Macro-based)](#struct-definition-macro-based)
- [Key API Functions](#key-api-functions-3)
- [Hash Functions](#hash-functions)
- [Kernel Usage Examples](#kernel-usage-examples-2)
- [Time Complexity](#time-complexity-3)

</details>

<details>
<summary><a href="#5-bitmap">5. Bitmap</a></summary>

- ["Struct" Definition](#struct-definition)
- [Key API Functions](#key-api-functions-4)
- [Kernel Usage Examples](#kernel-usage-examples-3)
- [Time Complexity](#time-complexity-4)

</details>

<details>
<summary><a href="#6-idr--ida">6. IDR / IDA</a></summary>

- [Struct Definitions](#struct-definitions-3)
- [Key API Functions — IDR](#key-api-functions--idr)
- [Key API Functions — IDA](#key-api-functions--ida)
- [Kernel Usage Examples](#kernel-usage-examples-4)
- [Time Complexity](#time-complexity-5)

</details>

<details>
<summary><a href="#7-maple-tree">7. Maple Tree</a></summary>

- [Struct Definitions](#struct-definitions-4)
- [Key API Functions](#key-api-functions-5)
- [Kernel Usage Examples](#kernel-usage-examples-5)
- [Time Complexity](#time-complexity-6)

</details>

<details>
<summary><a href="#8-priority-lists-plist">8. Priority Lists (plist)</a></summary>

- [Struct Definitions](#struct-definitions-5)
- [Key API Functions](#key-api-functions-6)
- [Kernel Usage Examples](#kernel-usage-examples-6)
- [Time Complexity](#time-complexity-7)

</details>

<details>
<summary><a href="#9-fifo-kfifo">9. FIFO (kfifo)</a></summary>

- [Struct Definitions](#struct-definitions-6)
- [核心实现源码逐行注释 — 无锁环形缓冲区](#核心实现源码逐行注释--无锁环形缓冲区)
- [Key API Functions](#key-api-functions-7)
- [Kernel Usage Examples](#kernel-usage-examples-7)
- [Time Complexity](#time-complexity-8)

</details>

<details>
<summary><a href="#10-lru-list">10. LRU List</a></summary>

- [Struct Definitions](#struct-definitions-7)
- [Key API Functions](#key-api-functions-8)
- [Kernel Usage Examples](#kernel-usage-examples-8)
- [Time Complexity](#time-complexity-9)

</details>

<details>
<summary><a href="#summary-comparison-table">Summary Comparison Table</a></summary>

</details>

<details>
<summary><a href="#architecture-of-relationships">Architecture of Relationships</a></summary>

</details>

---

## 1. Linked Lists

**Files**: `include/linux/list.h`, `include/linux/types.h`

### Struct Definitions

```c
/* include/linux/types.h */
struct list_head {
    struct list_head *next, *prev;
};

struct hlist_head {
    struct hlist_node *first;
};

struct hlist_node {
    struct hlist_node *next, **pprev;
};
```

**Design**: `list_head` is a **circular doubly-linked list**. The sentinel node (head) points to itself when empty. Unlike typical linked lists, the struct is *embedded inside* the container struct, and `container_of()` recovers the outer object.

`hlist_head` uses a **single pointer** for the head (saves memory in hash tables with millions of buckets). `hlist_node` uses `**pprev` (pointer to the pointer that points to this node) to allow O(1) deletion without knowing the head.

### Key API Functions

| Function | Description | Complexity |
|---|---|---|
| `INIT_LIST_HEAD(list)` | Initialize empty list (points to itself) | O(1) |
| `list_add(new, head)` | Insert after head (stack) | O(1) |
| `list_add_tail(new, head)` | Insert before head (queue) | O(1) |
| `list_del(entry)` | Remove entry, poison pointers | O(1) |
| `list_del_init(entry)` | Remove and re-initialize | O(1) |
| `list_move(list, head)` | Delete from current, add to head | O(1) |
| `list_move_tail(list, head)` | Delete from current, add to tail | O(1) |
| `list_replace(old, new)` | Replace old with new in-place | O(1) |
| `list_splice(list, head)` | Join two lists | O(1) |
| `list_empty(head)` | Test if list is empty | O(1) |
| `list_is_singular(head)` | Test if exactly one entry | O(1) |
| `list_count_nodes(head)` | Count all entries | O(n) |

### Iteration Macros

```c
/* Iterate over list entries of a given container type */
#define list_for_each_entry(pos, head, member)
    for (pos = list_first_entry(head, typeof(*pos), member);
         !list_entry_is_head(pos, head, member);
         pos = list_next_entry(pos, member))

/* Safe against removal during iteration */
#define list_for_each_entry_safe(pos, n, head, member)
    for (pos = list_first_entry(head, typeof(*pos), member),
         n = list_next_entry(pos, member);
         !list_entry_is_head(pos, head, member);
         pos = n, n = list_next_entry(n, member))

/* Get the container struct from a list_head pointer */
#define list_entry(ptr, type, member) container_of(ptr, type, member)
```

### Kernel Usage Examples

- **Task list**: `struct task_struct` has `struct list_head tasks` linking all processes
- **Module list**: `struct module` links via `list` member
- **Wait queues**: `wait_queue_head` uses `struct list_head task_list`
- **Block I/O**: `struct request_queue` maintains queues of `struct request`
- **Network**: `sk_buff_head` wraps a `list_head` for socket buffer chains

### 核心实现源码逐行注释

#### `__list_add()` — 双向链表插入的核心 ([list.h](include/linux/list.h#L155))

```c
/*
 * Insert a new entry between two known consecutive entries.
 * 在两个已知的相邻节点之间插入新节点。
 * 这是内部函数，调用者必须已知 prev/next。
 */
static inline void __list_add(struct list_head *new,
                              struct list_head *prev,
                              struct list_head *next)
{
    if (!__list_add_valid(new, prev, next))  // 完整性校验（CONFIG_LIST_HARDENED）
        return;                              // 检查 prev->next==next, next->prev==prev

    next->prev = new;          // Step 1: next 的前驱指向 new
    new->next = next;          // Step 2: new 的后继指向 next
    new->prev = prev;          // Step 3: new 的前驱指向 prev
    WRITE_ONCE(prev->next, new); // Step 4: prev 的后继指向 new（WRITE_ONCE 保证原子写入）
}
```

**为什么最后用 `WRITE_ONCE`？** 因为其他CPU可能正在无锁遍历链表（通过 `list_for_each_entry_rcu`），`WRITE_ONCE` 保证 `prev->next` 的更新是原子的，不会出现部分写（torn write）。在此之前，new 节点的 `next/prev` 已经设置好，所以一旦 `prev->next` 变为 new，遍历者就能看到一个完整连接的节点。

#### `list_add()` 和 `list_add_tail()` — 栈 vs 队列

```c
// 头插法 — 实现栈语义 (LIFO)
static inline void list_add(struct list_head *new, struct list_head *head)
{
    __list_add(new, head, head->next);  // 插入 head 和 head->next 之间
}

// 尾插法 — 实现队列语义 (FIFO)
static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
    __list_add(new, head->prev, head);  // 插入 head->prev 和 head 之间
}
```

**关键理解**: 由于是**循环链表**，`head->prev` 就是尾节点。所以 `list_add_tail` 把 new 插入在"尾节点"和"哨兵head"之间，等价于追加到末尾。

#### `__list_del()` 和 `list_del()` — 删除操作 ([list.h](include/linux/list.h#L203))

```c
static inline void __list_del(struct list_head *prev, struct list_head *next)
{
    next->prev = prev;           // 跳过被删节点
    WRITE_ONCE(prev->next, next); // 原子写入
}

static inline void list_del(struct list_head *entry)
{
    __list_del_entry(entry);     // 先校验再调用 __list_del(entry->prev, entry->next)
    entry->next = LIST_POISON1;  // 0x00100100 — 故意设为非法地址
    entry->prev = LIST_POISON2;  // 0x00200200 — 再次访问会触发 page fault
}
```

**`LIST_POISON` 的作用**: 如果删除后的节点被错误地再次使用（use-after-delete），访问这些 poison 地址会触发明显的 kernel oops，便于调试。`list_del_init()` 则不用 poison，而是重新初始化为指向自身（可安全地再次 `list_empty()` 检测）。

#### `container_of()` — 从嵌入式链表节点获取外层结构体

```c
// include/linux/container_of.h
#define container_of(ptr, type, member) ({               \
    void *__mptr = (void *)(ptr);                        \
    ((type *)(__mptr - offsetof(type, member))); })

// 使用示例：从 list_head 获取 task_struct
#define list_entry(ptr, type, member) container_of(ptr, type, member)

// 遍历时的典型用法
struct task_struct *task;
list_for_each_entry(task, &init_task.tasks, tasks) {
    // task 指向每个 task_struct
    // 内部实现：从 list_head* 减去 offsetof(task_struct, tasks) 得到 task_struct*
}
```

**数学原理**: 如果 `list_head` 成员在结构体中的偏移是 `offset`，那么结构体的起始地址 = `list_head 指针 - offset`。这就是 `container_of` 的全部魔法。

### hlist 的设计精妙之处

```c
struct hlist_head {
    struct hlist_node *first;       // 只有一个指针（8字节 vs list_head 的16字节）
};
struct hlist_node {
    struct hlist_node *next;
    struct hlist_node **pprev;      // 指向"指向自己的指针"
};
```

**为什么 `pprev` 是二级指针？** 删除节点时需要修改"前一个节点的 next 指针"：
- 如果前一个是 `hlist_node`，则修改 `prev->next`
- 如果前一个是 `hlist_head`，则修改 `head->first`

`pprev` 统一了这两种情况：它指向的都是"存放指向当前节点的那个指针"的地址。删除时只需：
```c
*(node->pprev) = node->next;  // 不管前面是 head 还是 node，都能正确修改
```

这让哈希表的每个桶头只占 8 字节而非 16 字节。百万级桶的哈希表能省 ~8MB 内存。

### Time Complexity

| Operation | Complexity |
|---|---|
| Insert (head/tail) | O(1) |
| Delete | O(1) |
| Search | O(n) |
| Splice (join two lists) | O(1) |
| Count | O(n) |

---

## 2. Red-Black Trees

**Files**: `include/linux/rbtree.h`, `include/linux/rbtree_types.h`, `lib/rbtree.c`

### Struct Definitions

```c
/* include/linux/rbtree_types.h */
struct rb_node {
    unsigned long  __rb_parent_color;   /* parent pointer + color bit (LSB) */
    struct rb_node *rb_right;
    struct rb_node *rb_left;
} __attribute__((aligned(sizeof(long))));

struct rb_root {
    struct rb_node *rb_node;
};

/* Cached variant: O(1) access to leftmost (minimum) node */
struct rb_root_cached {
    struct rb_root rb_root;
    struct rb_node *rb_leftmost;
};

#define RB_ROOT      (struct rb_root) { NULL, }
#define RB_ROOT_CACHED (struct rb_root_cached) { {NULL, }, NULL }
```

**Design trick**: `__rb_parent_color` stores both the parent pointer AND the red/black color in the least significant bit. This works because `rb_node` is aligned to `sizeof(long)`, so the bottom bits of any valid pointer are always 0. The macro `rb_parent(r)` extracts the parent via `(r)->__rb_parent_color & ~3`.

### Key API Functions

```c
/* Core operations */
void rb_insert_color(struct rb_node *, struct rb_root *);
void rb_erase(struct rb_node *, struct rb_root *);

/* Traversal */
struct rb_node *rb_next(const struct rb_node *);      /* in-order successor */
struct rb_node *rb_prev(const struct rb_node *);      /* in-order predecessor */
struct rb_node *rb_first(const struct rb_root *);     /* leftmost = minimum */
struct rb_node *rb_last(const struct rb_root *);      /* rightmost = maximum */

/* Link a new node (caller finds position, then calls this + rb_insert_color) */
static inline void rb_link_node(struct rb_node *node,
                                struct rb_node *parent,
                                struct rb_node **rb_link);

/* Fast replacement without rebalance */
void rb_replace_node(struct rb_node *victim, struct rb_node *new,
                     struct rb_root *root);

/* Cached (O(1) min) variants */
static inline void rb_insert_color_cached(struct rb_node *node,
                                          struct rb_root_cached *root,
                                          bool leftmost);
struct rb_node *rb_erase_cached(struct rb_node *node,
                                struct rb_root_cached *root);
#define rb_first_cached(root) (root)->rb_leftmost  /* O(1)! */

/* Safe post-order iteration (for tree destruction) */
#define rbtree_postorder_for_each_entry_safe(pos, n, root, field)
```

### Insert Pattern (Caller-implemented)

The kernel does NOT provide a generic insert function — callers write their own comparison:

```c
/* Typical rbtree insertion pattern (from Documentation/core-api/rbtree.rst) */
void my_insert(struct rb_root *root, struct my_struct *data)
{
    struct rb_node **new = &(root->rb_node), *parent = NULL;

    while (*new) {
        struct my_struct *this = rb_entry(*new, struct my_struct, node);
        parent = *new;

        if (data->key < this->key)
            new = &((*new)->rb_left);
        else if (data->key > this->key)
            new = &((*new)->rb_right);
        else
            return; /* duplicate */
    }

    rb_link_node(&data->node, parent, new);
    rb_insert_color(&data->node, root);
}
```

### 核心实现源码逐行注释 — 红黑树插入修复 ([rbtree.c](lib/rbtree.c#L89))

红黑树的五条性质确保树的高度 ≤ 2log₂(n+1)：
1. 节点非红即黑
2. 根是黑色
3. 所有叶子（NULL）是黑色
4. 红色节点的两个子节点都是黑色（不能连续两个红节点）
5. 从根到叶的每条路径包含相同数量的黑色节点

#### `__rb_insert()` — 插入后的颜色修复

```c
static __always_inline void
__rb_insert(struct rb_node *node, struct rb_root *root,
    void (*augment_rotate)(struct rb_node *old, struct rb_node *new))
{
    struct rb_node *parent = rb_red_parent(node), *gparent, *tmp;

    while (true) {
        /*
         * 循环不变量: node 始终是红色的
         */

        // 情况 0: node 是根节点 → 直接变黑
        if (unlikely(!parent)) {
            rb_set_parent_color(node, NULL, RB_BLACK);
            break;
        }

        // 如果父节点是黑色，没有违反性质4，结束
        if (rb_is_black(parent))
            break;

        gparent = rb_red_parent(parent);  // 祖父节点（必存在，因为父为红不可能是根）
        tmp = gparent->rb_right;

        if (parent != tmp) {   /* parent == gparent->rb_left */

            if (tmp && rb_is_red(tmp)) {
                /*
                 * Case 1 — 叔叔节点是红色 → 颜色翻转
                 *       G(黑)            g(红)
                 *      / \              / \
                 *     p(红) u(红) -->  P(黑) U(黑)
                 *    /                /
                 *   n(红)            n(红)
                 *
                 * 将父和叔变黑，祖父变红，然后在祖父处继续循环
                 */
                rb_set_parent_color(tmp, gparent, RB_BLACK);
                rb_set_parent_color(parent, gparent, RB_BLACK);
                node = gparent;
                parent = rb_parent(node);
                rb_set_parent_color(node, parent, RB_RED);
                continue;  // 在更高层继续检查
            }

            tmp = parent->rb_right;
            if (node == tmp) {
                /*
                 * Case 2 — 叔叔是黑色，node是右孩子 → 左旋parent
                 *      G             G
                 *     / \           / \
                 *    p   U  -->    n   U
                 *     \           /
                 *      n         p
                 * 旋转后 parent 变成了 node 的子节点
                 * 继续进入 Case 3
                 */
                WRITE_ONCE(parent->rb_right, tmp = node->rb_left);
                WRITE_ONCE(node->rb_left, parent);
                if (tmp)
                    rb_set_parent_color(tmp, parent, RB_BLACK);
                rb_set_parent_color(parent, node, RB_RED);
                augment_rotate(parent, node);
                parent = node;
                tmp = node->rb_right;
            }

            /*
             * Case 3 — 叔叔是黑色，node是左孩子 → 右旋gparent
             *        G(黑)          P(黑)
             *       / \            / \
             *      p(红) U  -->  n(红) g(红)
             *     /                     \
             *    n(红)                   U
             */
            WRITE_ONCE(gparent->rb_left, tmp);
            WRITE_ONCE(parent->rb_right, gparent);
            if (tmp)
                rb_set_parent_color(tmp, gparent, RB_BLACK);
            __rb_rotate_set_parents(gparent, parent, root, RB_RED);
            augment_rotate(gparent, parent);
            break;  // 修复完成
        } else {
            /* 对称情况: parent == gparent->rb_right */
            /* 同上三种 Case，方向相反 */
            ...
        }
    }
}
```

**关键设计点**:

1. **`__rb_parent_color` 的位打包**: `rb_node` 对齐到 `sizeof(long)`，所以指针的最低2位始终为0。红黑树利用这两位存储颜色（0=红，1=黑），用一个 `unsigned long` 同时保存父指针和颜色：
   ```c
   // 提取父指针: 清除最低2位
   #define rb_parent(r) ((struct rb_node *)((r)->__rb_parent_color & ~3))
   // 设置颜色: 修改最低位
   #define rb_set_parent_color(rb, p, color) \
       (rb)->__rb_parent_color = (unsigned long)(p) | (color)
   ```

2. **`WRITE_ONCE` 保证无锁遍历安全**: 旋转过程中的指针修改使用 `WRITE_ONCE`，使得并发的无锁 lookup（`rcu_read_lock` 保护）虽然可能miss某些节点，但不会陷入死循环或访问非法内存。

3. **Augmented RB-tree**: `augment_rotate` 回调在每次旋转后更新增量数据。CFS 调度器用它来维护子树的 `min_vruntime`。

#### 删除后的修复 — `____rb_erase_color()` ([rbtree.c](lib/rbtree.c#L230))

删除的修复更复杂，有4种情况（+对称的4种=8种）：

```c
// 核心逻辑:
while (true) {
    sibling = parent->rb_right;       // 兄弟节点
    if (node != sibling) {
        if (rb_is_red(sibling)) {
            /* Case 1: 兄弟是红色 → 左旋parent
             *     P               S
             *    / \             / \
             *   N   s    -->   p   Sr
             *      / \        / \
             *     Sl  Sr     N   Sl
             * 转化为兄弟为黑的情况 (Case 2/3/4)
             */
        }
        if (!tmp1 || rb_is_black(tmp1)) {
            if (!tmp2 || rb_is_black(tmp2)) {
                /* Case 2: 兄弟两子都是黑 → 兄弟变红
                 * 如果parent是红 → parent变黑，完成
                 * 如果parent是黑 → 在parent处递归
                 */
                rb_set_parent_color(sibling, parent, RB_RED);
                if (rb_is_red(parent)) { rb_set_black(parent); break; }
                node = parent; parent = rb_parent(node); continue;
            }
            /* Case 3: 兄弟左子红、右子黑 → 右旋兄弟
             *   (p)           (p)
             *   / \           / \
             *  N   S    -->  N   sl
             *     / \             \
             *    sl  Sr            S → Sr
             * 转化为 Case 4
             */
        }
        /* Case 4: 兄弟右子是红色 → 左旋parent
         *   (p)            (s)
         *   / \            / \
         *  N   S    -->   P   Sr
         *     / \        / \
         *    (sl) sr    N  (sl)
         * 修复完成!
         */
        break;
    }
    // 对称情况...
}
```

### Kernel Usage Examples (Red-Black Tree)

### Time Complexity

| Operation | Complexity |
|---|---|
| Insert | O(log n) |
| Delete | O(log n) |
| Search | O(log n) |
| Next/Prev | O(log n) amortized O(1) |
| First (cached) | O(1) |
| First (uncached) | O(log n) |

---

## 3. XArray / Radix Tree

**Files**: `include/linux/xarray.h`, `include/linux/radix-tree.h`, `lib/xarray.c`

### Struct Definitions

```c
/* include/linux/xarray.h */
struct xarray {
    spinlock_t   xa_lock;
    gfp_t        xa_flags;
    void __rcu  *xa_head;
};

struct xa_limit {
    u32 max;
    u32 min;
};

/* include/linux/radix-tree.h — radix tree is now an alias for xarray */
#define radix_tree_root   xarray
#define radix_tree_node   xa_node
```

**Design**: The XArray is a resizable array-like data structure indexed by `unsigned long`. Internally it uses a **radix tree** (trie) with a configurable chunk size (typically 6 bits per level = 64 slots per node). It replaces the older `radix_tree` API.

Special entry encoding in the bottom 2 bits:
- `00`: Pointer entry
- `10`: Internal entry (node pointer, retry, zero, or error)  
- `x1`: Value entry (stores integer in upper bits) or tagged pointer

### Key API Functions

```c
/* Core operations */
void *xa_load(struct xarray *, unsigned long index);
void *xa_store(struct xarray *, unsigned long index, void *entry, gfp_t);
void *xa_erase(struct xarray *, unsigned long index);
void *xa_store_range(struct xarray *, unsigned long first,
                     unsigned long last, void *entry, gfp_t);
void xa_destroy(struct xarray *);

/* Marks (tags) — up to 3 marks per entry */
bool xa_get_mark(struct xarray *, unsigned long index, xa_mark_t);
void xa_set_mark(struct xarray *, unsigned long index, xa_mark_t);
void xa_clear_mark(struct xarray *, unsigned long index, xa_mark_t);

/* Search */
void *xa_find(struct xarray *xa, unsigned long *index,
              unsigned long max, xa_mark_t);
void *xa_find_after(struct xarray *xa, unsigned long *index,
                    unsigned long max, xa_mark_t);

/* Initialization */
static inline void xa_init(struct xarray *xa);          /* zero flags */
static inline void xa_init_flags(struct xarray *xa, gfp_t flags);

/* Value entries — store integers directly */
static inline void *xa_mk_value(unsigned long v);  /* v << 1 | 1 */
static inline unsigned long xa_to_value(const void *entry);
static inline bool xa_is_value(const void *entry);

/* Iteration */
#define xa_for_each(xa, index, entry)
#define xa_for_each_marked(xa, index, entry, filter)
#define xa_for_each_start(xa, index, entry, start)
```

### 内部节点结构 — `xa_node` ([xarray.h](include/linux/xarray.h#L1168))

```c
struct xa_node {
    unsigned char  shift;      /* 每个slot还需要左移的位数 */
    unsigned char  offset;     /* 在父节点中的slot偏移 */
    unsigned char  count;      /* 非NULL entry数量 */
    unsigned char  nr_values;  /* value entry 数量 */
    struct xa_node __rcu *parent;  /* 父节点，根节点为NULL */
    struct xarray  *array;     /* 所属的xarray */
    union {
        struct list_head private_list;  /* 供使用者自定义 */
        struct rcu_head  rcu_head;      /* RCU释放时用 */
    };
    void __rcu *slots[XA_CHUNK_SIZE];  /* 64个slot (6位索引) */
    unsigned long tags[XA_MAX_MARKS][XA_MARK_LONGS];  /* 3组标记位图 */
};
```

**Radix Tree 结构图示**:
```
                     xa_head (root)
                        |
                    xa_node (shift=12)        ← 处理 bits 12-17
                   /    |    \
              slot[0] slot[1] ... slot[63]    ← 每个slot处理6位
                 |
            xa_node (shift=6)                 ← 处理 bits 6-11
           /    |    \
      slot[0] slot[1] ... slot[63]
         |
    xa_node (shift=0)                         ← 处理 bits 0-5
   /    |    \
 data  data  data  ...                        ← 叶子节点存储实际指针
```

**索引解析**: 对于 index=0x1234:
- Level 2 (shift=12): slot[(0x1234 >> 12) & 0x3F] = slot[1]
- Level 1 (shift=6):  slot[(0x1234 >> 6) & 0x3F]  = slot[13]
- Level 0 (shift=0):  slot[0x1234 & 0x3F]          = slot[52]

**XArray 的特殊编码**:
```c
// 低2位决定entry类型:
// 00: 普通指针（用户数据）
// x1: value entry — 整数直接编码在指针中
static inline void *xa_mk_value(unsigned long v) { return (void *)((v << 1) | 1); }
static inline unsigned long xa_to_value(const void *entry) { return (unsigned long)entry >> 1; }
// 10: internal entry（节点指针、retry、error）
static inline void *xa_mk_node(const struct xa_node *node) { return (void *)((unsigned long)node | 2); }
```

### Legacy Radix Tree API (Wrappers)

```c
/* These map directly to xarray operations */
#define RADIX_TREE(name, mask)   struct radix_tree_root name = RADIX_TREE_INIT(name, mask)
#define INIT_RADIX_TREE(root, mask)  xa_init_flags(root, mask)

int radix_tree_insert(struct radix_tree_root *, unsigned long index, void *);
void *radix_tree_delete(struct radix_tree_root *, unsigned long index);
void *radix_tree_lookup(const struct radix_tree_root *, unsigned long);
```

### Kernel Usage Examples

- **Page cache**: `struct address_space` uses `struct xarray i_pages` to map `pgoff_t → struct folio *`
- **PID lookup**: `struct pid_namespace` uses radix tree for PID to task mapping
- **Slab allocator**: tracks partial slabs
- **IDA/IDR**: built on top of XArray internally

### Time Complexity

| Operation | Complexity |
|---|---|
| Load (lookup) | O(log₆₄ n) ≈ O(1) for 64-bit keys (max ~11 levels) |
| Store (insert) | O(log₆₄ n) |
| Erase (delete) | O(log₆₄ n) |
| Find with mark | O(log₆₄ n) |
| Iteration (next) | Amortized O(1) |

For practical purposes with 64-bit indexes, the tree has at most ~11 levels, so these are effectively constant time.

---

## 4. Hash Tables

**Files**: `include/linux/hashtable.h`, `include/linux/hash.h`

### "Struct" Definition (Macro-based)

```c
/* include/linux/hashtable.h */
/* A hash table is simply an array of hlist_heads */
#define DEFINE_HASHTABLE(name, bits)
    struct hlist_head name[1 << (bits)] =
        { [0 ... ((1 << (bits)) - 1)] = HLIST_HEAD_INIT }

#define DECLARE_HASHTABLE(name, bits)
    struct hlist_head name[1 << (bits)]

/* Size queries */
#define HASH_SIZE(name) (ARRAY_SIZE(name))
#define HASH_BITS(name) ilog2(HASH_SIZE(name))
```

**Design**: Statically-sized chained hash table. Each bucket is an `hlist_head` (not `list_head`) to save memory — with many buckets, the 8-byte savings per bucket from using a single pointer instead of two is significant.

### Key API Functions

```c
/* Initialization */
#define hash_init(hashtable) __hash_init(hashtable, HASH_SIZE(hashtable))

/* Insert: hashes key, adds node to correct bucket */
#define hash_add(hashtable, node, key)
    hlist_add_head(node, &hashtable[hash_min(key, HASH_BITS(hashtable))])

/* Delete */
static inline void hash_del(struct hlist_node *node);     /* hlist_del_init */

/* Test */
static inline bool hash_hashed(struct hlist_node *node);   /* is in a table? */
#define hash_empty(hashtable)                               /* all buckets empty? */

/* Iteration */
#define hash_for_each(name, bkt, obj, member)              /* all entries */
#define hash_for_each_safe(name, bkt, tmp, obj, member)    /* safe removal */
#define hash_for_each_possible(name, obj, member, key)     /* same-bucket entries */

/* RCU variants */
#define hash_add_rcu(hashtable, node, key)
#define hash_del_rcu(node)
#define hash_for_each_rcu(name, bkt, obj, member)
#define hash_for_each_possible_rcu(name, obj, member, key, cond...)
```

### Hash Functions

```c
/* include/linux/hash.h */
/* Use hash_32 when possible for fast 32-bit hashing in 64-bit kernels */
#define hash_min(val, bits)
    (sizeof(val) <= 4 ? hash_32(val, bits) : hash_long(val, bits))
```

### Kernel Usage Examples

- **PID hash table**: `kernel/pid.c` — fast PID to `struct pid` lookup
- **Inode cache**: `fs/inode.c` — `inode_hashtable` for inode lookup
- **Dentry cache**: `fs/dcache.c` — `dentry_hashtable`
- **Futex**: `kernel/futex/` — hash table of futex queues
- **Network**: connection tracking in `nf_conntrack`

### Time Complexity

| Operation | Average | Worst Case |
|---|---|---|
| Insert (`hash_add`) | O(1) | O(1) |
| Delete (`hash_del`) | O(1) | O(1) |
| Lookup (`hash_for_each_possible`) | O(1) | O(n) (all collide) |
| Full iteration (`hash_for_each`) | O(n + buckets) | O(n + buckets) |

---

## 5. Bitmap

**Files**: `include/linux/bitmap.h`, `include/linux/types.h`, `lib/bitmap.c`

### "Struct" Definition

```c
/* include/linux/types.h */
#define DECLARE_BITMAP(name, bits)  unsigned long name[BITS_TO_LONGS(bits)]
```

**Design**: A bitmap is simply an array of `unsigned long` values. `BITS_TO_LONGS()` computes the minimum number of longs needed. Individual bit operations come from `asm/bitops.h`, while bulk operations come from `lib/bitmap.c`.

### Key API Functions

```c
/* Allocation */
unsigned long *bitmap_alloc(unsigned int nbits, gfp_t flags);
unsigned long *bitmap_zalloc(unsigned int nbits, gfp_t flags);
void bitmap_free(const unsigned long *bitmap);

/* Bulk operations */
void bitmap_zero(unsigned long *dst, unsigned int nbits);     /* clear all */
void bitmap_fill(unsigned long *dst, unsigned int nbits);     /* set all */
void bitmap_copy(unsigned long *dst, const unsigned long *src, unsigned int nbits);

/* Logical operations */
bool bitmap_and(unsigned long *dst, const unsigned long *src1,
                const unsigned long *src2, unsigned int nbits);
void bitmap_or(unsigned long *dst, ...);
void bitmap_xor(unsigned long *dst, ...);
bool bitmap_andnot(unsigned long *dst, ...);
void bitmap_complement(unsigned long *dst, ...);

/* Testing */
bool bitmap_equal(const unsigned long *src1, const unsigned long *src2, unsigned int nbits);
bool bitmap_intersects(const unsigned long *src1, const unsigned long *src2, unsigned int nbits);
bool bitmap_subset(const unsigned long *src1, const unsigned long *src2, unsigned int nbits);
bool bitmap_empty(const unsigned long *src, unsigned int nbits);
bool bitmap_full(const unsigned long *src, unsigned int nbits);

/* Counting */
unsigned int bitmap_weight(const unsigned long *src, unsigned int nbits);  /* popcount */

/* Region operations */
void bitmap_set(unsigned long *map, unsigned int start, int len);
void bitmap_clear(unsigned long *map, unsigned int start, int len);
unsigned long bitmap_find_next_zero_area(unsigned long *map, unsigned long size,
                                         unsigned long start, unsigned int nr,
                                         unsigned long align_mask);

/* Atomic single-bit operations (from asm/bitops.h) */
void set_bit(int nr, volatile unsigned long *addr);
void clear_bit(int nr, volatile unsigned long *addr);
void change_bit(int nr, volatile unsigned long *addr);
int test_bit(int nr, const volatile unsigned long *addr);
int test_and_set_bit(int nr, volatile unsigned long *addr);
int test_and_clear_bit(int nr, volatile unsigned long *addr);

/* Searching */
unsigned long find_first_bit(const unsigned long *addr, unsigned long size);
unsigned long find_first_zero_bit(const unsigned long *addr, unsigned long size);
unsigned long find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset);
unsigned long find_next_zero_bit(const unsigned long *addr, unsigned long size, unsigned long offset);

/* Iteration */
#define for_each_set_bit(bit, addr, size)
#define for_each_clear_bit(bit, addr, size)
```

### Kernel Usage Examples

- **CPU masks**: `cpumask_t` is `DECLARE_BITMAP(bits, NR_CPUS)` — used everywhere for CPU affinity, online CPUs
- **Node masks**: `nodemask_t` for NUMA nodes
- **Page flags**: buddy allocator page bitmaps
- **IRQ allocation**: `allocated_irqs` bitmap in `kernel/irq/irqdesc.c`
- **Slab allocator**: free object tracking bitmaps

### Time Complexity

| Operation | Complexity |
|---|---|
| `set_bit` / `clear_bit` / `test_bit` | O(1) |
| `bitmap_zero` / `bitmap_fill` | O(n/BITS_PER_LONG) |
| `bitmap_and/or/xor` | O(n/BITS_PER_LONG) |
| `bitmap_weight` | O(n/BITS_PER_LONG) |
| `find_first_bit` | O(n/BITS_PER_LONG) worst case |
| `find_next_bit` | O(n/BITS_PER_LONG) worst case |
| `bitmap_equal` | O(n/BITS_PER_LONG) |

---

## 6. IDR / IDA

**Files**: `include/linux/idr.h`, `lib/radix-tree.c`

### Struct Definitions

```c
/* include/linux/idr.h */
struct idr {
    struct radix_tree_root  idr_rt;     /* backed by XArray */
    unsigned int            idr_base;
    unsigned int            idr_next;
};

/* IDA — ID Allocator (no pointer storage, just bit allocation) */
struct ida_bitmap {
    unsigned long  bitmap[IDA_BITMAP_LONGS];  /* 128 bytes per chunk */
};

struct ida {
    struct xarray xa;
};
```

**Design**: IDR maps integer IDs to pointers (like a sparse array). It's built on top of the radix tree / XArray. IDA is a more memory-efficient variant when you only need to allocate/free IDs without associating data.

### Key API Functions — IDR

```c
/* Static definition */
#define DEFINE_IDR(name)  struct idr name = IDR_INIT(name)

/* Dynamic init */
static inline void idr_init(struct idr *idr);

/* Allocate: assigns an unused ID, stores ptr, returns the ID */
int idr_alloc(struct idr *, void *ptr, int start, int end, gfp_t);
int idr_alloc_u32(struct idr *, void *ptr, u32 *id, unsigned long max, gfp_t);
int idr_alloc_cyclic(struct idr *, void *ptr, int start, int end, gfp_t);

/* Lookup */
void *idr_find(const struct idr *, unsigned long id);

/* Remove */
void *idr_remove(struct idr *, unsigned long id);

/* Replace entry atomically */
void *idr_replace(struct idr *, void *, unsigned long id);

/* Iteration */
#define idr_for_each_entry(idr, entry, id)
    for (id = 0; ((entry) = idr_get_next(idr, &(id))) != NULL; id += 1U)

/* Destroy all entries */
void idr_destroy(struct idr *);

/* Locking (delegates to xa_lock) */
#define idr_lock(idr)       xa_lock(&(idr)->idr_rt)
#define idr_unlock(idr)     xa_unlock(&(idr)->idr_rt)
```

### Key API Functions — IDA

```c
#define DEFINE_IDA(name)  struct ida name = IDA_INIT(name)

/* Allocate an unused ID */
static inline int ida_alloc(struct ida *ida, gfp_t gfp);
int ida_alloc_range(struct ida *, unsigned int min, unsigned int max, gfp_t);

/* Free an ID */
void ida_free(struct ida *, unsigned int id);

/* Destroy */
void ida_destroy(struct ida *ida);
```

### Kernel Usage Examples

- **File descriptors**: `struct fdtable` — mapping fd numbers to `struct file *`
- **Minor device numbers**: `idr` for character/block device minors
- **IPC IDs**: `struct ipc_ids` uses IDR for shmem/semaphore/msgqueue IDs
- **DRM**: GEM handle allocation via `idr_alloc`
- **Network namespaces**: net device ifindex allocation via IDA
- **USB**: URB IDs

### Time Complexity

| Operation | Complexity |
|---|---|
| `idr_alloc` | O(log₆₄ n) |
| `idr_find` | O(log₆₄ n) ≈ O(1) |
| `idr_remove` | O(log₆₄ n) |
| `ida_alloc` | O(log₆₄ n) |
| `ida_free` | O(log₆₄ n) |

---

## 7. Maple Tree

**Files**: `include/linux/maple_tree.h`, `lib/maple_tree.c`

### Struct Definitions

```c
/* include/linux/maple_tree.h */
struct maple_tree {
    union {
        spinlock_t          ma_lock;
        struct lockdep_map  *ma_external_lock;  /* CONFIG_LOCKDEP */
    };
    unsigned int  ma_flags;
    void __rcu   *ma_root;
};

/* Node types — B-tree inspired with different slot counts */
struct maple_range_64 {
    struct maple_pnode *parent;
    unsigned long pivot[MAPLE_RANGE64_SLOTS - 1];  /* 15 pivots */
    union {
        void __rcu *slot[MAPLE_RANGE64_SLOTS];     /* 16 slots */
        struct {
            void __rcu *pad[MAPLE_RANGE64_SLOTS - 1];
            struct maple_metadata meta;
        };
    };
};

struct maple_arange_64 {       /* Allocation-range variant (tracks gaps) */
    struct maple_pnode *parent;
    unsigned long pivot[MAPLE_ARANGE64_SLOTS - 1];  /* 9 pivots */
    void __rcu *slot[MAPLE_ARANGE64_SLOTS];          /* 10 slots */
    unsigned long gap[MAPLE_ARANGE64_SLOTS];         /* 10 gap sizes */
    struct maple_metadata meta;
};

/* Node sizes on 64-bit: */
#define MAPLE_NODE_SLOTS     31    /* 256 bytes including ->parent */
#define MAPLE_RANGE64_SLOTS  16    /* 256 bytes */
#define MAPLE_ARANGE64_SLOTS 10    /* 240 bytes */

enum maple_type {
    maple_dense,        /* Dense storage */
    maple_leaf_64,      /* Leaf with 64-bit pivots */
    maple_range_64,     /* Internal node with 64-bit pivots */
    maple_arange_64,    /* Allocation-range node */
};

/* Iterator / operation state */
struct ma_state {
    struct maple_tree *tree;
    unsigned long index;           /* range start */
    unsigned long last;            /* range end */
    struct maple_enode *node;
    unsigned long min;             /* implied pivot min */
    unsigned long max;             /* implied pivot max */
    struct slab_sheaf *sheaf;
    struct maple_node *alloc;
    unsigned long node_request;
    enum maple_status status;
    unsigned char depth;
    unsigned char offset;
    unsigned char mas_flags;
    unsigned char end;
    enum store_type store_type;
};
```

**Design**: The Maple Tree is an **RCU-safe B-tree** variant optimized for storing **ranges** (key → value where keys are `unsigned long`). Nodes are 256 bytes (cache-line aligned). It replaced the rbtree+linked-list combination previously used for VMA management. The `arange` variant tracks gaps for efficient free-range allocation (critical for `mmap`).

### Key API Functions

```c
/* Static/dynamic initialization */
#define DEFINE_MTREE(name)  struct maple_tree name = MTREE_INIT(name, 0)

/* Core operations */
void *mtree_load(struct maple_tree *mt, unsigned long index);
int mtree_insert(struct maple_tree *mt, unsigned long index, void *entry, gfp_t);
int mtree_insert_range(struct maple_tree *mt, unsigned long first,
                       unsigned long last, void *entry, gfp_t);
int mtree_store(struct maple_tree *mt, unsigned long index, void *entry, gfp_t);
int mtree_store_range(struct maple_tree *mt, unsigned long first,
                      unsigned long last, void *entry, gfp_t);
void *mtree_erase(struct maple_tree *mt, unsigned long index);

/* Allocation (find free range) */
int mtree_alloc_range(struct maple_tree *mt, unsigned long *startp,
                      void *entry, unsigned long size,
                      unsigned long min, unsigned long max, gfp_t);
int mtree_alloc_rrange(struct maple_tree *mt, unsigned long *startp,
                       void *entry, unsigned long size,
                       unsigned long min, unsigned long max, gfp_t);

/* Destruction */
void mtree_destroy(struct maple_tree *mt);

/* Advanced API — maple state based */
#define MA_STATE(name, mt, first, end)  struct ma_state name = { ... }

void *mas_walk(struct ma_state *mas);
void *mas_find(struct ma_state *mas, unsigned long max);
void *mas_find_rev(struct ma_state *mas, unsigned long min);
int mas_store_gfp(struct ma_state *mas, void *entry, gfp_t gfp);
void mas_erase(struct ma_state *mas);

/* Iteration */
#define mas_for_each(__mas, __entry, __max)
    while (((__entry) = mas_find((__mas), (__max))) != NULL)

#define mas_for_each_rev(__mas, __entry, __min)
    while (((__entry) = mas_find_rev((__mas), (__min))) != NULL)

/* Locking */
#define mas_lock(mas)    spin_lock(&((mas)->tree->ma_lock))
#define mas_unlock(mas)  spin_unlock(&((mas)->tree->ma_lock))
```

### Kernel Usage Examples

- **VMA management**: `struct mm_struct` has `struct maple_tree mm_mt` — **the** primary user. All VMA lookups, insertions, splits, and merges go through the maple tree. This replaced the previous rbtree + linked list.
- **`mmap()`**: Uses `mtree_alloc_range()` with `MT_FLAGS_ALLOC_RANGE` to efficiently find free virtual address ranges.
- **`/proc/pid/maps`**: Iterates VMAs via `mas_for_each()`

### Time Complexity

| Operation | Complexity |
|---|---|
| Load (point lookup) | O(log n) — B-tree with high fanout |
| Insert / Store | O(log n) |
| Erase | O(log n) |
| Alloc range (find gap) | O(log n) — gap tracking in `arange` nodes |
| Iteration (next) | Amortized O(1) |

The high fanout (10-16 children per node) means the tree is very shallow — typically 3-4 levels for millions of entries, making it significantly faster than rbtrees in practice due to cache locality.

---

## 8. Priority Lists (plist)

**Files**: `include/linux/plist.h`, `include/linux/plist_types.h`, `lib/plist.c`

### Struct Definitions

```c
/* include/linux/plist_types.h */
struct plist_head {
    struct list_head node_list;
};

struct plist_node {
    int              prio;
    struct list_head prio_list;    /* links one node per unique priority */
    struct list_head node_list;    /* links ALL nodes in sorted order */
};
```

**Design**: A priority-sorted list implemented as a **list of lists**:
- **Tier 1** (`prio_list`): Links one representative node per unique priority level, sorted by priority.
- **Tier 2** (`node_list`): Links ALL nodes in priority order. Nodes with the same priority are FIFO-ordered.

```
 pl:prio_list (only for plist_node)
 nl:node_list
   HEAD|             NODE(S)
       |
       ||------------------------------------|
       ||->|pl|<->|pl|<--------------->|pl|<-|
       |   |10|   |21|   |21|   |21|   |40|   (prio)
       |   |  |   |  |   |  |   |  |   |  |
 |->|nl|<->|nl|<->|nl|<->|nl|<->|nl|<->|nl|<-|
 |-------------------------------------------|
```

`INT_MIN` is the highest priority, `INT_MAX` is the lowest.

### Key API Functions

```c
/* Initialization */
#define PLIST_HEAD(head)  struct plist_head head = PLIST_HEAD_INIT(head)
static inline void plist_head_init(struct plist_head *head);
static inline void plist_node_init(struct plist_node *node, int prio);

/* Core operations */
void plist_add(struct plist_node *node, struct plist_head *head);   /* O(K) insert */
void plist_del(struct plist_node *node, struct plist_head *head);   /* O(1) remove */
void plist_requeue(struct plist_node *node, struct plist_head *head);

/* Queries */
static inline int plist_head_empty(const struct plist_head *head);
static inline int plist_node_empty(const struct plist_node *node);

/* Access first/last (highest/lowest priority) */
#define plist_first_entry(head, type, member)
#define plist_last_entry(head, type, member)
static inline struct plist_node *plist_first(const struct plist_head *head);
static inline struct plist_node *plist_last(const struct plist_head *head);

/* Iteration */
#define plist_for_each(pos, head)
#define plist_for_each_safe(pos, n, head)
#define plist_for_each_entry(pos, head, mem)
#define plist_for_each_entry_safe(pos, n, head, m)
```

### Kernel Usage Examples

- **RT mutex waiters**: `struct rt_mutex_waiter` uses `struct plist_node` for priority-inheritance ordered waiting
- **PI futexes**: `struct futex_pi_state` priority waiters
- **PM QoS**: `struct pm_qos_constraints` maintains a plist of QoS requests sorted by value

### Time Complexity

| Operation | Complexity |
|---|---|
| `plist_add` (insert) | O(K), K = number of distinct priority levels (1 ≤ K ≤ 99 for RT) |
| `plist_del` (remove) | O(1) |
| Get highest priority (`plist_first`) | O(1) |
| `plist_requeue` | O(K) |

---

## 9. FIFO (kfifo)

**Files**: `include/linux/kfifo.h`, `lib/kfifo.c`

### Struct Definitions

```c
/* include/linux/kfifo.h */
struct __kfifo {
    unsigned int  in;       /* write index */
    unsigned int  out;      /* read index */
    unsigned int  mask;     /* size - 1 (size must be power of 2) */
    unsigned int  esize;    /* element size */
    void         *data;     /* buffer pointer */
};

/* Statically-sized FIFO (buffer embedded in struct) */
#define STRUCT_KFIFO(type, size)
    struct __STRUCT_KFIFO(type, size, 0, type)

/* Dynamically-allocated FIFO (buffer allocated separately) */
struct kfifo __STRUCT_KFIFO_PTR(unsigned char, 0, void);
```

**Design**: Lock-free single-producer / single-consumer ring buffer. The `in` and `out` counters are `unsigned int` and are allowed to **wrap around** naturally. The difference `in - out` always gives the correct count thanks to unsigned arithmetic. The `mask` (size - 1) is used with bitwise AND for O(1) index wrapping. Size must be a power of 2.

**Lock-free guarantee**: When there is exactly one reader and one writer, no locking is needed (the `in` index is only written by the producer, `out` only by the consumer, and `smp_wmb()`/`smp_rmb()` ensure ordering).

### 核心实现源码逐行注释 — 无锁环形缓冲区

#### `__kfifo_in()` — 生产者写入 ([kfifo.c](lib/kfifo.c#L114))

```c
unsigned int __kfifo_in(struct __kfifo *fifo,
        const void *buf, unsigned int len)
{
    unsigned int l;

    l = kfifo_unused(fifo);     // 计算空闲空间: size - (in - out)
    if (len > l)
        len = l;                // 截断到可用空间

    kfifo_copy_in(fifo, buf, len, fifo->in);  // 复制数据
    fifo->in += len;            // 更新写索引（允许自然溢出!）
    return len;
}
```

#### `kfifo_copy_in()` — 处理环绕的数据复制

```c
static void kfifo_copy_in(struct __kfifo *fifo, const void *src,
        unsigned int len, unsigned int off)
{
    unsigned int size = fifo->mask + 1;   // 实际缓冲区大小
    unsigned int l;

    off &= fifo->mask;                    // 等价于 off % size，但用位与更快
    // ... esize 调整 ...
    l = min(len, size - off);             // 从 off 到缓冲区尾部的空间

    memcpy(fifo->data + off, src, l);     // 第一段: off → buffer_end
    memcpy(fifo->data, src + l, len - l); // 第二段: buffer_start → 剩余(环绕)

    smp_wmb();  // 写屏障: 确保数据写入对消费者可见后，in 索引才更新
}
```

**无锁原理深入**:

```
      写索引 (in)               读索引 (out)
         ↓                         ↓
  |---已读---|----待读数据----|---空闲---|
  0                                    mask+1

  已用空间 = in - out            (无符号减法，自动处理溢出)
  空闲空间 = size - (in - out)

  关键: in 和 out 是 unsigned int，允许自然溢出到 0。
  例如: in=0xFFFFFFFE, out=0xFFFFFFFC → in-out=2 (正确!)
  实际访问时用 & mask 取低位作为数组下标。
```

**内存屏障配对**:
- 生产者: 先写数据 → `smp_wmb()` → 再更新 `in`
- 消费者: 先读 `in`（获取数据范围） → `smp_rmb()` → 再读数据 → 更新 `out`

这保证了消费者看到 `in` 更新时，数据一定已经写好了。

### Key API Functions

```c
/* Static definition */
#define DEFINE_KFIFO(fifo, type, size)          /* embedded buffer, power-of-2 size */
#define DECLARE_KFIFO(fifo, type, size)
#define INIT_KFIFO(fifo)

/* Dynamic allocation */
int kfifo_alloc(fifo, size, gfp_mask);          /* allocate buffer */
void kfifo_free(fifo);

/* Queries */
#define kfifo_initialized(fifo)  ((fifo)->kfifo.mask)
#define kfifo_size(fifo)         ((fifo)->kfifo.mask + 1)
#define kfifo_len(fifo)          ((fifo)->kfifo.in - (fifo)->kfifo.out)
#define kfifo_is_empty(fifo)
#define kfifo_is_full(fifo)
#define kfifo_avail(fifo)        /* free space */

/* Put (enqueue) */
#define kfifo_put(fifo, val)                    /* single element */
#define kfifo_in(fifo, buf, n)                  /* n elements from buffer */
#define kfifo_in_spinlocked(fifo, buf, n, lock)

/* Get (dequeue) */
#define kfifo_get(fifo, val)                    /* single element */
#define kfifo_out(fifo, buf, n)                 /* n elements to buffer */
#define kfifo_out_spinlocked(fifo, buf, n, lock)
#define kfifo_out_peek(fifo, buf, n)            /* peek without consuming */

/* Reset */
#define kfifo_reset(fifo)       /* dangerous: only when exclusively locked */
#define kfifo_reset_out(fifo)   /* safe: only from reader thread */
#define kfifo_skip(fifo)        /* skip one element */

/* I/O helpers */
#define kfifo_to_user(fifo, to, len, copied)    /* copy to userspace */
#define kfifo_from_user(fifo, from, len, copied) /* copy from userspace */
```

### Kernel Usage Examples

- **Serial drivers**: `uart_port` uses kfifo for transmit/receive buffers
- **Input subsystem**: `input_dev` event queue
- **Tracing**: ring buffers in ftrace use similar concepts
- **Audio**: ALSA PCM buffer management
- **USB gadget**: endpoint FIFOs

### Time Complexity

| Operation | Complexity |
|---|---|
| `kfifo_put` / `kfifo_in` | O(n) for n elements, O(1) per element |
| `kfifo_get` / `kfifo_out` | O(n) for n elements, O(1) per element |
| `kfifo_len` / `kfifo_is_empty` | O(1) |
| `kfifo_peek` | O(1) |
| `kfifo_reset` | O(1) |

---

## 10. LRU List

**Files**: `include/linux/list_lru.h`, `mm/list_lru.c`

### Struct Definitions

```c
/* include/linux/list_lru.h */
struct list_lru_one {
    struct list_head  list;      /* the actual LRU list */
    long              nr_items;  /* may become negative during memcg reparenting */
    spinlock_t        lock;      /* protects all fields above */
};

struct list_lru_node {
    /* global list, used for root cgroup in cgroup-aware lrus */
    struct list_lru_one  lru;
    atomic_long_t        nr_items;
} ____cacheline_aligned_in_smp;

struct list_lru {
    struct list_lru_node  *node;        /* per-NUMA-node lists */
#ifdef CONFIG_MEMCG
    struct list_head       list;        /* links all list_lrus together */
    int                    shrinker_id;
    bool                   memcg_aware;
    struct xarray          xa;          /* per-memcg per-node lists */
#endif
};

/* Walk callback return values */
enum lru_status {
    LRU_REMOVED,        /* item removed from list */
    LRU_REMOVED_RETRY,  /* removed, but lock was dropped/reacquired */
    LRU_ROTATE,         /* item referenced, give another pass */
    LRU_SKIP,           /* item cannot be locked, skip */
    LRU_RETRY,          /* not freeable, retry */
    LRU_STOP,           /* stop walking */
};
```

**Design**: A **NUMA-aware, memcg-aware LRU** infrastructure for kernel caches. Each `list_lru` maintains separate lists per NUMA node (and optionally per memory cgroup). Items are added at the tail (least recently used) and the shrinker walks from the head (oldest). This enables the memory reclaim subsystem to evict the least-recently-used cached objects efficiently.

### Key API Functions

```c
/* Initialization */
#define list_lru_init(lru)           __list_lru_init((lru), false, NULL)
#define list_lru_init_memcg(lru, shrinker)  __list_lru_init((lru), true, shrinker)
void list_lru_destroy(struct list_lru *lru);

/* Add/Remove */
bool list_lru_add(struct list_lru *lru, struct list_head *item,
                  int nid, struct mem_cgroup *memcg);     /* add to tail */
bool list_lru_add_obj(struct list_lru *lru, struct list_head *item); /* auto-detect node/memcg */
bool list_lru_del(struct list_lru *lru, struct list_head *item,
                  int nid, struct mem_cgroup *memcg);
bool list_lru_del_obj(struct list_lru *lru, struct list_head *item);

/* Counting */
unsigned long list_lru_count_one(struct list_lru *lru, int nid,
                                 struct mem_cgroup *memcg);
unsigned long list_lru_count_node(struct list_lru *lru, int nid);

/* Walk (for shrinkers — iterate and potentially free items) */
unsigned long list_lru_walk_one(struct list_lru *lru, int nid,
                                struct mem_cgroup *memcg,
                                list_lru_walk_cb isolate,
                                void *cb_arg, unsigned long *nr_to_walk);
unsigned long list_lru_walk_node(struct list_lru *lru, int nid,
                                 list_lru_walk_cb isolate,
                                 void *cb_arg, unsigned long *nr_to_walk);

/* Memory cgroup support */
int memcg_list_lru_alloc(struct mem_cgroup *memcg, struct list_lru *lru, gfp_t);
void memcg_reparent_list_lrus(struct mem_cgroup *memcg, struct mem_cgroup *parent);
```

### Kernel Usage Examples

- **Dentry cache**: `struct dentry` uses `d_lru` list_head, managed by `super_block->s_dentry_lru` (a `list_lru`)
- **Inode cache**: `struct inode` uses `i_lru` list_head, managed by `super_block->s_inode_lru`
- **Shrinker integration**: The shrinker subsystem calls `list_lru_walk_one()` during memory pressure to reclaim least-recently-used dentries/inodes

### Time Complexity

| Operation | Complexity |
|---|---|
| `list_lru_add` | O(1) |
| `list_lru_del` | O(1) |
| `list_lru_count_one` | O(1) (cached counter) |
| `list_lru_walk_one` | O(n) where n = `nr_to_walk` |

---

## Summary Comparison Table

| Data Structure | Primary Use | Lookup | Insert | Delete | Memory Per Entry |
|---|---|---|---|---|---|
| **list_head** | General linked list | O(n) | O(1) | O(1) | 2 pointers (16B) |
| **hlist** | Hash bucket chains | O(n) | O(1) | O(1) | 1 ptr + 1 ptr-to-ptr (16B) |
| **rbtree** | Ordered key-value | O(log n) | O(log n) | O(log n) | 3 longs (24B) |
| **XArray** | Sparse array by index | O(1)* | O(1)* | O(1)* | ~shared nodes |
| **Hash table** | Fast key lookup | O(1) avg | O(1) | O(1) | hlist_node (16B) |
| **Bitmap** | Bit flags / sets | O(1) per bit | O(1) per bit | O(1) per bit | 1 bit per entry |
| **IDR** | Integer ID → pointer | O(1)* | O(1)* | O(1)* | XArray node slots |
| **IDA** | Integer ID allocation | N/A | O(1)* | O(1)* | ~1 bit per ID |
| **Maple Tree** | Range-indexed data | O(log n) | O(log n) | O(log n) | 256B nodes, high fanout |
| **plist** | Priority queue | O(1) first | O(K) | O(1) | 2 list_heads + int |
| **kfifo** | SPSC ring buffer | N/A | O(1) | O(1) | element size |
| **list_lru** | LRU eviction | N/A | O(1) | O(1) | list_head (16B) |

\* O(1) means O(log₆₄ n) which is bounded by ~11 for 64-bit keys — effectively constant.

---

## Architecture of Relationships

```
XArray (xarray.h)
 ├── Radix Tree (radix-tree.h) — thin compatibility wrapper
 ├── IDR (idr.h) — integer-to-pointer mapping, built on radix_tree_root = xarray
 └── IDA (idr.h) — pure ID allocation, built on xarray

list_head (types.h)
 ├── Hash Table (hashtable.h) — array of hlist_heads (uses hlist variant)
 ├── Priority List (plist.h) — two-tier list_head structure
 ├── LRU List (list_lru.h) — per-node/memcg list_head lists
 └── Used internally by nearly every kernel subsystem

Standalone trees:
 ├── Red-Black Tree (rbtree.h) — self-balancing BST
 └── Maple Tree (maple_tree.h) — B-tree for ranges, replaced rbtree for VMAs
```
