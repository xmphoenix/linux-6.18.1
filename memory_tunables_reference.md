# Linux 6.18.1 ARM64 — Memory Optimization Tunables Reference

---

## 1. `/proc/sys/vm/` Sysctl Tunables (page_alloc.c)

Registered via `register_sysctl_init("vm", page_alloc_sysctl_table)` at **mm/page_alloc.c:6730**.

| Tunable | Variable | Default | Range | File:Line | Description |
|---------|----------|---------|-------|-----------|-------------|
| `min_free_kbytes` | `min_free_kbytes` | 1024 (auto-calculated: `sqrt(lowmem_kbytes * 16)`, clamped 128–262144) | ≥0 | mm/page_alloc.c:275 | Minimum free memory the kernel keeps. Controls zone watermarks. Higher = more reserved for atomic allocs |
| `watermark_boost_factor` | `watermark_boost_factor` | 15000 | ≥0 | mm/page_alloc.c:277 | Factor for temporarily boosting watermarks after fragmentation events. 15000 = 150% boost. 0 disables boosting |
| `watermark_scale_factor` | `watermark_scale_factor` | 10 | 1–3000 | mm/page_alloc.c:278 | Scale factor (in 0.01% of managed pages) for gap between min/low/high watermarks. 10 = 0.1% |
| `defrag_mode` | `defrag_mode` | 0 | 0–1 | mm/page_alloc.c:279 | When 1, enables aggressive defragmentation in the page allocator by altering fallback behavior |
| `percpu_pagelist_high_fraction` | `percpu_pagelist_high_fraction` | 0 | 0 or ≥8 | mm/page_alloc.c:5900 | Fraction controlling per-cpu pagelist high watermark. 0 = use default batch-based calculation |
| `lowmem_reserve_ratio` | `sysctl_lowmem_reserve_ratio[]` | DMA=256, DMA32=256, NORMAL=32, HIGHMEM=0, MOVABLE=0 | — | mm/page_alloc.c:231 | Per-zone ratios for lowmem reservation protecting lower zones from higher-zone allocations |
| `numa_zonelist_order` | `numa_zonelist_order` | (string) | — | mm/page_alloc.c:5521 | NUMA zone list ordering policy (NUMA only) |
| `min_unmapped_ratio` | `sysctl_min_unmapped_ratio` | 1 | 0–100 | mm/vmscan.c:7587 | % of zone pages that must be unmapped before node reclaim claims pages (NUMA) |
| `min_slab_ratio` | `sysctl_min_slab_ratio` | 5 | 0–100 | mm/vmscan.c:7593 | % threshold of slab pages beyond which slab reclaim occurs (NUMA) |

### Helper variables

| Variable | Default | File:Line | Note |
|----------|---------|-----------|------|
| `user_min_free_kbytes` | -1 | mm/page_alloc.c:276 | Tracks whether user explicitly set min_free_kbytes |

---

## 2. `/proc/sys/vm/` Sysctl Tunables (vmscan.c)

Registered via `register_sysctl_init("vm", vmscan_sysctl_table)` at **mm/vmscan.c:7561**.

| Tunable | Variable | Default | Range | File:Line | Description |
|---------|----------|---------|-------|-----------|-------------|
| `swappiness` | `vm_swappiness` | 60 | 0–200 | mm/vmscan.c:202 | Relative tendency to swap out anonymous vs reclaim file-backed pages. 0 = minimal swapping, 200 = max |
| `zone_reclaim_mode` | `node_reclaim_mode` | 0 | ≥0 | mm/vmscan.c:7574 | NUMA node reclaim mode bitmask: 1=RECLAIM_ZONE, 2=RECLAIM_WRITE, 4=RECLAIM_UNMAP (NUMA only) |

---

## 3. Compaction Tunables (`/proc/sys/vm/`)

Registered via `register_sysctl_init("vm", vm_compaction)` at **mm/compaction.c:3329**.

| Tunable | Variable | Default | Range | File:Line | Description |
|---------|----------|---------|-------|-----------|-------------|
| `compact_memory` | `sysctl_compact_memory` | 0 | write-only | mm/compaction.c:1892 | Write 1 to trigger system-wide compaction. Resets after completion |
| `compaction_proactiveness` | `sysctl_compaction_proactiveness` | 20 | 0–100 | mm/compaction.c:1890 | Aggressiveness of background proactive compaction. 0 disables it. Higher = more aggressive |
| `extfrag_threshold` | `sysctl_extfrag_threshold` | 500 | 0–1000 | mm/compaction.c:1891 | External fragmentation threshold. Below this, compaction is deferred as fragmentation is considered acceptable |
| `compact_unevictable_allowed` | `sysctl_compact_unevictable_allowed` | CONFIG_COMPACT_UNEVICTABLE_DEFAULT | 0–1 | mm/compaction.c:1884 | Whether compaction can scan the unevictable LRU for movable pages |

---

## 4. SLUB Allocator Debug/Tuning (Boot Command Line)

All set via `__setup()` boot parameters in **mm/slub.c**.

| Boot Param | Alias | Variable | Default | File:Line | Description |
|------------|-------|----------|---------|-----------|-------------|
| `slab_debug` | `slub_debug` | `slub_debug` flags | 0 (or `DEBUG_DEFAULT_FLAGS` if `CONFIG_SLUB_DEBUG_ON`) | mm/slub.c:976–978 | Enable slab debug flags (F=sanity, Z=redzoning, P=poisoning, U=track user, T=trace, A=failslab, O=orig_size). Format: `slub_debug=<flags>[,<slab_name>]` |
| `slab_min_order=` | `slub_min_order=` | `slub_min_order` | 0 | mm/slub.c:7525 | Minimum page order for slab allocations |
| `slab_max_order=` | `slub_max_order=` | `slub_max_order` | PAGE_ALLOC_COSTLY_ORDER (3), or 1 if `CONFIG_SLUB_TINY` | mm/slub.c:7526 | Maximum page order for slab allocations. Capped at MAX_PAGE_ORDER |
| `slab_min_objects=` | `slub_min_objects=` | `slub_min_objects` | 0 (auto-calculated) | mm/slub.c:7528 | Minimum number of objects per slab. 0 = auto-size based on nr_cpu_ids |
| `slab_strict_numa` | — | static key `strict_numa` | disabled | mm/slub.c:8186–8193 | Enable strict per-NUMA-node slab allocation (NUMA only) |

### SLUB sysfs

SLUB exposes per-cache attributes under `/sys/kernel/slab/<cache>/` when `CONFIG_SYSFS` is enabled (registered at mm/slub.c:351). Per-cache files include: `object_size`, `objs_per_slab`, `order`, `slab_size`, `partial`, `cpu_slabs`, `shrink`, `aliases`, `align`, etc.

---

## 5. THP (Transparent Huge Pages) Tunables

Exposed via sysfs under `/sys/kernel/mm/transparent_hugepage/`.

### Global THP attributes (mm/huge_memory.c)

| sysfs File | Variable/Flag | Default | Values | File:Line | Description |
|------------|---------------|---------|--------|-----------|-------------|
| `enabled` | `TRANSPARENT_HUGEPAGE_FLAG` / `REQ_MADV_FLAG` | `madvise` (or `always` if `CONFIG_TRANSPARENT_HUGEPAGE_ALWAYS`) | always, madvise, never | mm/huge_memory.c:299–342 | Global THP policy for PMD-sized hugepages |
| `defrag` | `DEFRAG_DIRECT`, `_KSWAPD`, `_KSWAPD_OR_MADV`, `_REQ_MADV` flags | madvise | always, defer, defer+madvise, madvise, never | mm/huge_memory.c:375–432 | When to trigger direct reclaim/compaction for THP allocation |
| `use_zero_page` | `TRANSPARENT_HUGEPAGE_USE_ZERO_PAGE_FLAG` | 1 (set in default flags) | 0, 1 | mm/huge_memory.c:435–448 | Use the huge zero page for read faults on zero-filled areas |
| `hpage_pmd_size` | — | arch-dependent | read-only | mm/huge_memory.c:451–454 | Size in bytes of a PMD-level THP |
| `shrink_underused` | `split_underused_thp` | true | 0, 1 | mm/huge_memory.c:75, 456–475 | Automatically split underused THPs to reclaim memory |

### Per-THP-size attributes (`/sys/kernel/mm/transparent_hugepage/hugepages-*kB/`)

| sysfs File | Variable | Default | Values | File:Line | Description |
|------------|----------|---------|--------|-----------|-------------|
| `enabled` | `huge_anon_orders_always/inherit/madvise` bitmasks | PMD_ORDER=inherit, others=never | always, inherit, madvise, never | mm/huge_memory.c:497–548 | Per-order THP enablement policy for anonymous mappings |

### khugepaged tunables (`/sys/kernel/mm/transparent_hugepage/khugepaged/`)

| sysfs File | Variable | Default | File:Line | Description |
|------------|----------|---------|-----------|-------------|
| `scan_sleep_millisecs` | `khugepaged_scan_sleep_millisecs` | 10000 (10s) | mm/khugepaged.c:74 | Sleep between khugepaged scan cycles |
| `alloc_sleep_millisecs` | `khugepaged_alloc_sleep_millisecs` | 60000 (60s) | mm/khugepaged.c:76 | Sleep After allocation failure before retry |
| `pages_to_scan` | `khugepaged_pages_to_scan` | (uninitialized, set at runtime) | mm/khugepaged.c:71 | Number of pages khugepaged scans per pass |
| `max_ptes_none` | `khugepaged_max_ptes_none` | (set at init, up to HPAGE_PMD_NR-1) | mm/khugepaged.c:87 | Max unmapped PTEs allowed when collapsing. Higher = more aggressive collapse, may increase memory use |
| `max_ptes_swap` | `khugepaged_max_ptes_swap` | (set at init) | mm/khugepaged.c:88 | Max swap entries allowed when collapsing a hugepage |
| `max_ptes_shared` | `khugepaged_max_ptes_shared` | (set at init) | mm/khugepaged.c:89 | Max shared PTEs allowed when collapsing |
| `defrag` | `TRANSPARENT_HUGEPAGE_DEFRAG_KHUGEPAGED_FLAG` | 1 (enabled in default flags) | 0, 1 | mm/khugepaged.c:233–251 | Whether khugepaged uses defrag for hugepage allocation |
| `pages_collapsed` | `khugepaged_pages_collapsed` | — | read-only | mm/khugepaged.c:72 | Counter of pages collapsed |
| `full_scans` | `khugepaged_full_scans` | — | read-only | mm/khugepaged.c:73 | Counter of full scan cycles completed |

---

## 6. CMA (Contiguous Memory Allocator) Params

### Boot command line (kernel/dma/contiguous.c)

| Boot Param | Variable(s) | Default | File:Line | Description |
|------------|-------------|---------|-----------|-------------|
| `cma=<size>[@<base>[-<limit>]]` | `size_cmdline`, `base_cmdline`, `limit_cmdline` | CONFIG_CMA_SIZE_MBYTES (Kconfig, default 0 if unset) | kernel/dma/contiguous.c:72–91 | Set global CMA area size, optional base address and limit |
| `numa_cma=<node>:<size>[,...]` | `numa_cma_size[]` | 0 | kernel/dma/contiguous.c:93+ | Per-NUMA-node CMA area sizes (CONFIG_DMA_NUMA_CMA) |

### Kconfig defaults

| Config | Definition | File:Line | Description |
|--------|-----------|-----------|-------------|
| `CONFIG_CMA_SIZE_MBYTES` | `CMA_SIZE_MBYTES` | kernel/dma/contiguous.c:49–52 | Default CMA size in MB if cma= not specified on cmdline |

### CMA sysfs

Each CMA area has sysfs stats under `/sys/kernel/mm/cma/<name>/` (registered in mm/cma_sysfs.c) exposing `alloc_pages_success`, `alloc_pages_fail`, `release_pages_success`.

---

## 7. Memory Cgroup Controls

Exposed via cgroupv2 under `memory.*` files. Defined in **mm/memcontrol.c:4587–4656**.

| cgroup File | Handler | File:Line | Description |
|-------------|---------|-----------|-------------|
| `memory.current` | `memory_current_read` | mm/memcontrol.c:4587 | Current memory usage (read-only) |
| `memory.peak` | `memory_peak_show/write` | mm/memcontrol.c:4592 | Peak memory usage; write to reset |
| `memory.min` | `memory_min_write` | mm/memcontrol.c:4303 | Hard memory protection floor — pages below this are never reclaimed. Default: 0 (no protection) |
| `memory.low` | `memory_low_write` | mm/memcontrol.c:4326 | Soft memory protection threshold — best-effort, reclaim avoids pages below this. Default: 0 |
| `memory.high` | `memory_high_write` | mm/memcontrol.c:4349 | High usage throttle point — kernel aggressively reclaims when exceeded. Default: max (unlimited) |
| `memory.max` | `memory_max_write` | mm/memcontrol.c:4401 | Hard memory limit — triggers OOM killer when truly exceeded. Default: max (unlimited) |
| `memory.events` | `memory_events_show` | mm/memcontrol.c:4623 | Event counters: low, high, max, oom, oom_kill, oom_group_kill (read-only) |
| `memory.oom.group` | `memory_oom_group_write` | mm/memcontrol.c:4546 | 0 or 1 — when 1, OOM kills entire cgroup instead of single task. Default: 0 |
| `memory.reclaim` | `memory_reclaim` | mm/memcontrol.c:4573 | Write bytes to trigger proactive reclaim from this cgroup |
| `memory.stat` | `memory_stat_show` | mm/memcontrol.c:4634 | Detailed memory statistics (read-only) |
| `memory.numa_stat` | `memory_numa_stat_show` | mm/memcontrol.c:4638 | Per-NUMA-node statistics (read-only, NUMA only) |

---

## 8. Watermark Tuning (Summary)

All in **mm/page_alloc.c**:

| Param | Value | Line | How It Works |
|-------|-------|------|-------------|
| `min_free_kbytes` | 1024 (auto: `sqrt(lowmem_kbytes * 16)`) | 275 | Sets WMARK_MIN. Auto-calculated at boot, clamped 128–262144 |
| `watermark_boost_factor` | 15000 | 277 | Temporary multiplier (in basis points / 10000) to boost watermarks when fragmentation detected. 0 = disabled |
| `watermark_scale_factor` | 10 | 278 | Gap between watermarks (WMARK_LOW, WMARK_HIGH) in 0.01% of managed pages. Range: 1–3000 |

Watermark calculation (mm/page_alloc.c:6340–6389):
- `WMARK_MIN = min_free_kbytes >> (PAGE_SHIFT - 10)` (proportionally distributed across zones)
- `WMARK_LOW = WMARK_MIN + max(WMARK_MIN/4, managed_pages * watermark_scale_factor / 10000)`
- `WMARK_HIGH = WMARK_MIN + max(WMARK_MIN/2, managed_pages * watermark_scale_factor / 10000)`

---

## 9. KSM Tunables

Exposed via sysfs under `/sys/kernel/mm/ksm/`. All in **mm/ksm.c**.

| sysfs File | Variable | Default | File:Line | Description |
|------------|----------|---------|-----------|-------------|
| `sleep_millisecs` | `ksm_thread_sleep_millisecs` | 20 | mm/ksm.c:285 | Sleep between KSM scan iterations (ms) |
| `pages_to_scan` | `ksm_thread_pages_to_scan` | 100 (`DEFAULT_PAGES_TO_SCAN`) | mm/ksm.c:252, 282 | Pages scanned per iteration |
| `run` | `ksm_run` | 0 (`KSM_RUN_STOP`) | mm/ksm.c:482 | 0=stop, 1=run (merge), 2=run+unmerge |
| `merge_across_nodes` | `ksm_merge_across_nodes` | 1 | mm/ksm.c:471 | Whether to merge pages across NUMA nodes (NUMA only) |
| `use_zero_pages` | `ksm_use_zero_pages` | false | mm/ksm.c:291 | Map zero-filled pages to the kernel zero page |
| `max_page_sharing` | `ksm_max_page_sharing` | 256 | mm/ksm.c:279 | Max number of mappings sharing a single KSM page |
| `stable_node_chains_prune_millisecs` | `ksm_stable_node_chains_prune_millisecs` | 2000 | mm/ksm.c:276 | Interval for pruning stable node chains |
| `smart_scan` | `ksm_smart_scan` | true | mm/ksm.c:295 | Enable smart scan to skip pages unlikely to merge |
| `advisor_mode` | `ksm_advisor` | NONE (0) | mm/ksm.c:338 | Auto-tuning mode: none or scan-time |
| `advisor_max_cpu` | `ksm_advisor_max_cpu` | 70 | mm/ksm.c:310 | Max CPU% for scan-time advisor |
| `advisor_min_pages_to_scan` | `ksm_advisor_min_pages_to_scan` | 500 | mm/ksm.c:346 | Advisor floor for pages_to_scan |
| `advisor_max_pages_to_scan` | `ksm_advisor_max_pages_to_scan` | 30000 | mm/ksm.c:304 | Advisor ceiling for pages_to_scan |
| `advisor_target_scan_time` | `ksm_advisor_target_scan_time` | 200 (seconds) | mm/ksm.c:313 | Target full-scan cycle time for advisor |

### KSM read-only stats (also in `/sys/kernel/mm/ksm/`)

| File | Variable | Description |
|------|----------|-------------|
| `pages_scanned` | `ksm_pages_scanned` | Total pages scanned |
| `pages_shared` | `ksm_pages_shared` | Pages actually shared (deduplicated) |
| `pages_sharing` | `ksm_pages_sharing` | Extra mappings sharing shared pages |
| `pages_unshared` | `ksm_pages_unshared` | Pages scanned but unique |
| `pages_volatile` | computed | Rapidly changing pages (not merged) |
| `pages_skipped` | `ksm_pages_skipped` | Pages skipped by smart_scan |
| `ksm_zero_pages` | `ksm_zero_pages` | Pages mapped to zero page |
| `full_scans` | `ksm_scan.seqnr` | Full scan cycles completed |
| `stable_node_dups` | `ksm_stable_node_dups` | Stable nodes with duplicates |
| `stable_node_chains` | `ksm_stable_node_chains` | Number of stable node chains |
| `general_profit` | computed | Memory saved (bytes) = (sharing + zero_pages) * PAGE_SIZE - rmap_items * sizeof(rmap_item) |

---

## 10. zswap Module Parameters

Module params under `/sys/module/zswap/parameters/`. All in **mm/zswap.c**.

| Parameter | Variable | Default | Permissions | File:Line | Description |
|-----------|----------|---------|-------------|-----------|-------------|
| `enabled` | `zswap_enabled` | `CONFIG_ZSWAP_DEFAULT_ON` (typically false) | 0644 | mm/zswap.c:90, 96 | Enable/disable zswap compressed swap cache |
| `compressor` | `zswap_compressor` | `CONFIG_ZSWAP_COMPRESSOR_DEFAULT` (e.g. "lzo-rle") | 0644 | mm/zswap.c:100, 107 | Compression algorithm name |
| `max_pool_percent` | `zswap_max_pool_percent` | 20 | 0644 | mm/zswap.c:112 | Max % of total memory for compressed pool |
| `accept_threshold_percent` | `zswap_accept_thr_percent` | 90 | 0644 | mm/zswap.c:116 | % of max pool at which to start rejecting new pages |
| `shrinker_enabled` | `zswap_shrinker_enabled` | `CONFIG_ZSWAP_SHRINKER_DEFAULT_ON` | 0644 | mm/zswap.c:122 | Enable memory pressure-based shrinker for zswap pool |

---

## 11. Per-VMA Lock Configuration

Compile-time only (no runtime tunables). Defined in **mm/Kconfig:1325–1340**.

| Config | Default | Depends | File:Line | Description |
|--------|---------|---------|-----------|-------------|
| `CONFIG_ARCH_SUPPORTS_PER_VMA_LOCK` | n (per-arch) | — | mm/Kconfig:1325 | Arch declares support for per-VMA locking |
| `CONFIG_PER_VMA_LOCK` | y (auto) | `ARCH_SUPPORTS_PER_VMA_LOCK && MMU && SMP` | mm/Kconfig:1328 | Lock each VMA separately during page fault handling instead of taking mmap_lock |
| `CONFIG_PER_VMA_LOCK_STATS` | n | `PER_VMA_LOCK` | mm/Kconfig.debug:300 | Debug statistics for per-VMA lock usage |

ARM64 enables `ARCH_SUPPORTS_PER_VMA_LOCK`, so PER_VMA_LOCK is active by default on SMP ARM64 kernels. Fault path at mm/memory.c:6449.

---

## Quick Reference: All Tunable Interfaces by Type

### Boot Command Line Parameters
| Param | Source File |
|-------|------------|
| `cma=<size>[@<base>[-<limit>]]` | kernel/dma/contiguous.c:91 |
| `numa_cma=...` | kernel/dma/contiguous.c:93+ |
| `slub_debug=<flags>[,<slab>]` / `slab_debug` | mm/slub.c:1923–1924 |
| `slub_min_order=N` / `slab_min_order=N` | mm/slub.c:8152–8153 |
| `slub_max_order=N` / `slab_max_order=N` | mm/slub.c:8167–8168 |
| `slub_min_objects=N` / `slab_min_objects=N` | mm/slub.c:8177–8178 |
| `slab_strict_numa` | mm/slub.c:8193 |

### `/proc/sys/vm/` Sysctls
| Tunable | Source |
|---------|--------|
| `min_free_kbytes` | mm/page_alloc.c |
| `watermark_boost_factor` | mm/page_alloc.c |
| `watermark_scale_factor` | mm/page_alloc.c |
| `defrag_mode` | mm/page_alloc.c |
| `percpu_pagelist_high_fraction` | mm/page_alloc.c |
| `lowmem_reserve_ratio` | mm/page_alloc.c |
| `numa_zonelist_order` | mm/page_alloc.c |
| `min_unmapped_ratio` | mm/page_alloc.c (NUMA) |
| `min_slab_ratio` | mm/page_alloc.c (NUMA) |
| `swappiness` | mm/vmscan.c |
| `zone_reclaim_mode` | mm/vmscan.c (NUMA) |
| `compact_memory` | mm/compaction.c |
| `compaction_proactiveness` | mm/compaction.c |
| `extfrag_threshold` | mm/compaction.c |
| `compact_unevictable_allowed` | mm/compaction.c |

### `/sys/kernel/mm/transparent_hugepage/`
| File | Source |
|------|--------|
| `enabled` | mm/huge_memory.c |
| `defrag` | mm/huge_memory.c |
| `use_zero_page` | mm/huge_memory.c |
| `hpage_pmd_size` | mm/huge_memory.c |
| `shrink_underused` | mm/huge_memory.c |
| `hugepages-*kB/enabled` | mm/huge_memory.c |
| `khugepaged/scan_sleep_millisecs` | mm/khugepaged.c |
| `khugepaged/alloc_sleep_millisecs` | mm/khugepaged.c |
| `khugepaged/pages_to_scan` | mm/khugepaged.c |
| `khugepaged/max_ptes_none` | mm/khugepaged.c |
| `khugepaged/max_ptes_swap` | mm/khugepaged.c |
| `khugepaged/max_ptes_shared` | mm/khugepaged.c |
| `khugepaged/defrag` | mm/khugepaged.c |

### `/sys/kernel/mm/ksm/`
| File | Source |
|------|--------|
| `sleep_millisecs` | mm/ksm.c |
| `pages_to_scan` | mm/ksm.c |
| `run` | mm/ksm.c |
| `merge_across_nodes` | mm/ksm.c |
| `use_zero_pages` | mm/ksm.c |
| `max_page_sharing` | mm/ksm.c |
| `smart_scan` | mm/ksm.c |
| `advisor_mode` | mm/ksm.c |
| `advisor_max_cpu` | mm/ksm.c |
| `advisor_min_pages_to_scan` | mm/ksm.c |
| `advisor_max_pages_to_scan` | mm/ksm.c |
| `advisor_target_scan_time` | mm/ksm.c |
| `stable_node_chains_prune_millisecs` | mm/ksm.c |

### `/sys/module/zswap/parameters/`
| File | Source |
|------|--------|
| `enabled` | mm/zswap.c |
| `compressor` | mm/zswap.c |
| `max_pool_percent` | mm/zswap.c |
| `accept_threshold_percent` | mm/zswap.c |
| `shrinker_enabled` | mm/zswap.c |

### Memory Cgroup (cgroupv2 `memory.*`)
| File | Source |
|------|--------|
| `memory.min` | mm/memcontrol.c |
| `memory.low` | mm/memcontrol.c |
| `memory.high` | mm/memcontrol.c |
| `memory.max` | mm/memcontrol.c |
| `memory.oom.group` | mm/memcontrol.c |
| `memory.reclaim` | mm/memcontrol.c |

### Compile-Time Only
| Config | Source |
|--------|--------|
| `CONFIG_PER_VMA_LOCK` | mm/Kconfig |
| `CONFIG_CMA_SIZE_MBYTES` | Kconfig |
