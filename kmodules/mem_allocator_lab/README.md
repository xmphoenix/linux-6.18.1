# mem_allocator_lab

这个目录放的是一个外部内核模块，用来把文档里的 `memblock -> buddy -> SLUB` 结论落成可观测实验。

## 目标

模块在 `init` 时做三件事：

1. `alloc_pages()` 分配一个 buddy 管理的页块，并打印 `page -> pfn -> phys -> linear VA` 往返信息。
2. `kmalloc()` 分配一个普通小对象，并打印它落在哪个页上。
3. `kmem_cache_create()` 创建一个自定义 cache，再分配多份对象，观察多个对象是否共享底层页。
4. `vmalloc()` 做一组固定的 `A/B/C -> free(B) -> D -> free(A/D) -> E` 实验，观察空闲区复用和相邻区合并。

这三步分别对应主文档里的这几个结论：

- buddy 管的是页，不是物理区间。
- 普通页和大多数 slab page 都已经处于 kernel linear map 中。
- SLUB 管对象，但对象最终还是落在 buddy 提供的页上。
- vmalloc 先在 `free_vmap_area_root` 里查找空闲虚拟区，释放时再把区间插回 free tree，并尽量和前后相邻空闲区合并。

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
  page_order=2 kmalloc_bytes=96 cache_bytes=128 cache_objects=8 vmalloc_base_kb=64
```

观察日志：

```bash
dmesg | grep mem_allocator_lab
```

卸载模块：

```bash
sudo rmmod mem_allocator_lab
```

## QEMU 实验

主机侧先构建模块：

```bash
ARCH=arm64 LLVM=/repo/ybzhang/kernel/rootfs/bin/ \
make -C /repo/ybzhang/kernel/linux-6.18.1 \
  M=$PWD/kmodules/mem_allocator_lab modules
```

然后从仓库根目录启动 QEMU：

```bash
./launch.sh arm64 run
```

当前 rootfs 的启动脚本已经把宿主机的 `kmodules` 目录通过 9p 挂到来宾的 `/mnt`，所以来宾机里可以直接这样做：

```bash
mount -t tracefs nodev /sys/kernel/tracing
echo 1 > /sys/kernel/tracing/events/vmalloc/alloc_vmap_area/enable
echo 1 > /sys/kernel/tracing/events/vmalloc/free_vmap_area_noflush/enable
echo 1 > /sys/kernel/tracing/events/vmalloc/purge_vmap_area_lazy/enable
cat /sys/kernel/tracing/trace_pipe
```

另开一个终端加载模块：

```bash
insmod /mnt/mem_allocator_lab/mem_allocator_lab.ko \
  page_order=2 kmalloc_bytes=96 cache_bytes=128 cache_objects=8 vmalloc_base_kb=64
dmesg | grep mem_allocator_lab
```

卸载：

```bash
rmmod mem_allocator_lab
```

这个实验里最值得看的是两条日志：

- `reuse_check reused_old_B=1`：说明 `free(B)` 之后，同尺寸的 `D` 优先复用了旧的 `B` 区间，体现的是 `find_vmap_lowest_match()` 查找 + `insert_vmap_area()` 插入 busy tree。
- `merge_check reused_old_A=1`：说明 `free(A)` 和 `free(D)` 后，相邻空闲区经 `merge_or_add_vmap_area_augment()` 合并，新的 `E` 能直接落回 `A` 起点。

配合 tracefs 里的 vmalloc 事件，你会看到：

- `alloc_vmap_area`：每次找到了哪段虚拟区。
- `free_vmap_area_noflush`：区间释放并进入 lazy free 路径。
- `purge_vmap_area_lazy`：延迟回收的区间什么时候真正 purge。

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

### 4. `vmalloc_case` 这几条日志

你应该看到：

- `vmalloc_A/B/C` 地址单调上升，说明它们被依次插入了 busy tree。
- `reuse_check reused_old_B=1` 时，`D` 和旧 `B` 地址相同，说明分配器在 free tree 中找到了最低可用且正好满足请求的旧区间。
- `merge_check reused_old_A=1` 时，`E` 和旧 `A` 地址相同，说明 `A` 与 `D` 释放后回插 free tree 时发生了相邻区合并。

这几行日志分别对应源码里的：

- 查找：`find_vmap_lowest_match()`
- 插入：`insert_vmap_area()`
- 合并：`merge_or_add_vmap_area_augment()`

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