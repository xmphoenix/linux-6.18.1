---
title: ARM64 Linux 内核启动：五个页表目录及其生命周期
cover: cover.png
author: 深海的小鱼儿
source_url:
---

<style>
body { font-size: 14px; line-height: 1.7; }
h1 { font-size: 18px; }
h2 { font-size: 16px; }
table { font-size: 13px; }
.footer-note { font-size: 12px; color: #999; }
</style>

# ARM64 Linux 内核启动：五个页表目录及其生命周期

> **📖 导读：** ARM64 Linux 内核从上电到 `start_kernel()` 的启动过程中，共涉及 **5 个不同的页表目录**。它们分别在 MMU 开启前、MMU 开启瞬间、早期内核映射阶段和最终页表建立阶段发挥作用。本文逐一拆解每个页表目录的用途、生命周期、级数骨架和叶子映射模式，帮助建立完整的 ARM64 启动页表演进认知。

---

## 一、五个页表目录总览

| 页表目录名 | 用途 | 生命周期 |
|---|---|---|
| `init_idmap_pg_dir` | 恒等映射 (PA=VA)<br>MMU 开启瞬间使用 | head.S 创建 → setup_arch 卸载<br>TTBR0 使用 |
| `reserved_pg_dir` | 空页表（安全占位）<br>防止野指针访问 | 切换 TTBR1 时临时使用<br>全程保留 |
| `init_pg_dir` | 早期内核映射<br>内核镜像虚拟映射 | early_map_kernel 创建<br>根页表复制到 swapper_pg_dir 后废弃 |
| `swapper_pg_dir` | **★ 最终内核页表**<br>内核镜像 + 线性映射 | early_map_kernel 初始化（从init复制）<br>paging_init 完善，永久使用<br>TTBR1 使用 |
| `idmap_pg_dir` | **★ 最终恒等映射**<br>仅映射 .idmap.text 段 | paging_init → create_idmap() 创建<br>CPU hotplug/suspend 时 TTBR0 使用 |

## 二、关键细节补充

除了记住"谁在什么时候用"，还要记住**"它到底是几级页表、叶子条目是 block 还是 page"**。以下是逐个要点：

- **主内核地址空间是 5 级页表**：`PAGE_SIZE=4KB`、`VA_BITS=52`、`CONFIG_PGTABLE_LEVELS=5`，Linux 命名层次是 `PGD → P4D → PUD → PMD → PTE`

- **每级单个条目的典型覆盖范围**：`PGD=256TB`，`P4D=512GB`，`PUD=1GB`，`PMD=2MB`，`PTE=4KB`

- **本文口语里的"段映射"在 ARM64 里更准确叫 block mapping**：对 4KB granule，常见 block leaf 是 `PUD` 的 `1GB block` 和 `PMD` 的 `2MB block`；真正的页映射是 `PTE` 的 `4KB page`

- **`idmap` 不是 52-bit 五级，而是固定 48-bit 四级**：源码里 `IDMAP_VA_BITS=48`，`IDMAP_ROOT_LEVEL = 4 - 4 = 0`，因此 `init_idmap_pg_dir` / `idmap_pg_dir` 的根表位于 **level 0（即 P4D 层）**，遍历路径为 `P4D→PUD→PMD→PTE`，不需要顶层 `PGD（level -1）`

- **启动早期的 `create_init_idmap()` / `map_kernel()`** 走的是 `arch/arm64/kernel/pi/map_range.c` 这套 PI 建表器：`idmap` 侧从 level 0 开始（四级），内核主映射侧从 level -1 开始（五级）；PI 建表器的叶子只在 **level 2 放 PMD 2MB block** 或 **level 3 放 PTE 4KB page**（`level < 2` 时一律递归，因此 PI 建表器不会产生 PUD 1GB block）

- **最终 `paging_init()` 的 `swapper_pg_dir`** 走通用建表器 `__create_pgd_mapping()`：在 4KB 页配置下会优先尝试 `PUD 1GB block` 或 `PMD 2MB block`，只有需要更细粒度权限、边界切分或后续拆分时才下沉到 `PTE 4KB page`

- **`reserved_pg_dir` 只是空根表**，只负责占位，不承载实际叶子映射

## 三、页表目录对照表

| 页表目录 | 生命周期阶段 | 级数骨架 | 实际叶子映射模式 |
|---|---|---|---|
| `init_idmap_pg_dir` | `head.S` 早期恒等映射 | 4 级 (root 在 level 0 即 `P4D→PUD→PMD→PTE`；无 `PGD`) | 以 `PMD 2MB block` 为主，必要时降到 `PTE 4KB page` |
| `reserved_pg_dir` | MMU 刚打开时给 `TTBR1` 占位 | 只有根表占位 | 无实际映射 |
| `init_pg_dir` | `early_map_kernel()` 早期内核镜像映射 | 5 级 | 以 `PMD 2MB block` 为主，段边界/权限切分处用 `PTE 4KB page` |
| `swapper_pg_dir` | `early_map_kernel()` 初始化，`paging_init()` 完善 | 5 级 | 最终内核页表，优先 `PUD 1GB block` / `PMD 2MB block`，必要时 `PTE 4KB page` |
| `idmap_pg_dir` | `paging_init()` 后的最终 idmap | 4 级 (`IDMAP_ROOT_LEVEL=0`，即 `P4D→PUD→PMD→PTE`，无 `PGD`) | 只覆盖 `.idmap.text` 一小段，通常是 `PMD block`，不够对齐时退化为 `PTE page` |

## 四、总结

ARM64 内核启动过程中，五个页表目录按"出场顺序"形成一条清晰的演进链：

1. **`init_idmap_pg_dir`**（四级，head.S 汇编创建）→ 恒等映射，安全开启 MMU
2. **`reserved_pg_dir`**（空根表）→ TTBR1 安全占位，防止野指针
3. **`init_pg_dir`**（五级，early_map_kernel 创建）→ 内核镜像的临时虚拟映射，让内核跳入高地址运行
4. **`swapper_pg_dir`**（五级，paging_init 完善）→ ★ 最终内核页表，TTBR1 使用，伴随内核整个生命周期
5. **`idmap_pg_dir`**（四级，paging_init 创建）→ ★ 最终恒等映射，CPU hotplug/suspend 时 TTBR0 使用

理解这五个目录的**级数差异**（idmap 固定四级 48-bit vs 主内核五级 52-bit）和**叶子映射差异**（PI 建表器只用 PMD block/PTE page vs 通用建表器优先尝试 PUD 1GB block），是掌握 ARM64 启动内存管理的关键。

---

<div class="footer-note">

*本文节选自《Linux ARM64 内存管理完全学习指南》第0课*
*基于 Linux 6.18.1，ARM64 架构，VA_BITS=52，PGTABLE_LEVELS=5*

</div>
