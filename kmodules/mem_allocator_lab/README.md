# mem_allocator_lab

这个目录放的是一个外部内核模块，用来把文档里的 `memblock -> buddy -> SLUB` 结论落成可观测实验。

## 目标

模块在 `init` 时做三件事：

1. `alloc_pages()` 分配一个 buddy 管理的页块，并打印 `page -> pfn -> phys -> linear VA` 往返信息。
2. `kmalloc()` 分配一个普通小对象，并打印它落在哪个页上。
3. `kmem_cache_create()` 创建一个自定义 cache，再分配多份对象，观察多个对象是否共享底层页。

这三步分别对应主文档里的这几个结论：

- buddy 管的是页，不是物理区间。
- 普通页和大多数 slab page 都已经处于 kernel linear map 中。
- SLUB 管对象，但对象最终还是落在 buddy 提供的页上。

## 构建

在仓库根目录执行：

```bash
ARCH=arm64 LLVM=1 make -C /repo/ybzhang/kernel/linux-6.18.1 \
  M=$PWD/kmodules/mem_allocator_lab modules
```

如果你当前就在仓库根目录，也可以执行：

```bash
ARCH=arm64 LLVM=1 make M=$PWD/kmodules/mem_allocator_lab modules
```

如果只是想清理由这次验证生成的构建产物：

```bash
ARCH=arm64 LLVM=1 make M=$PWD/kmodules/mem_allocator_lab clean
```

## 运行

先看基线：

```bash
cat /proc/buddyinfo
cat /proc/slabinfo | grep -E 'kmalloc-|mem_allocator_lab'
```

加载模块：

```bash
sudo insmod kmodules/mem_allocator_lab/mem_allocator_lab.ko \
  page_order=2 kmalloc_bytes=96 cache_bytes=128 cache_objects=8
```

观察日志：

```bash
dmesg | grep mem_allocator_lab
```

卸载模块：

```bash
sudo rmmod mem_allocator_lab
```

## 你应该重点看什么

### 1. `alloc_pages` 这一行

你应该看到：

- `page_to_virt(page)` 给出的地址是一个稳定的 kernel 线性映射地址。
- `virt_to_page(page_to_virt(page))` 能回到原始 `struct page`。
- `folio_order` 等于你传入的 `page_order`。

这对应的结论是：buddy 提供的页已经在线性映射里，不是“裸物理页”。

### 2. `kmalloc` 这一行

你应该看到：

- `kmalloc()` 返回对象指针。
- 它有 `ksize()`。
- 它能通过 `virt_to_page()` 找到底层页。

这对应的结论是：普通小对象通常直接落在 direct-mapped slab page 上。

### 3. 多条 `cache_object` 日志

你应该看到：

- 多个对象可能拥有不同 offset。
- 它们往往落在相同或相邻的页上。

这对应的结论是：SLUB 先拿页，再在页中切对象。

## 推荐的联合观测

为了把日志和全局状态连起来，建议同时看：

```bash
cat /proc/buddyinfo
cat /proc/zoneinfo
cat /proc/slabinfo | grep -E 'kmalloc-|mem_allocator_lab'
```

如果你要验证 memblock 视角，再结合启动参数：

```text
earlycon ignore_loglevel loglevel=8 memblock=debug
```

这样你就能把这条链路完整串起来：

`memblock` 先描述和保留物理区间，buddy 接手页，SLUB 再把 buddy 提供的页切成对象。