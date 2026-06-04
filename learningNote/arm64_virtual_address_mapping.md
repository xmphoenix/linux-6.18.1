# ARM64 Linux Virtual Address Space Mapping (VA_BITS=48)

> Based on linux-6.18 kernel source: `arch/arm64/include/asm/memory.h`, `arch/arm64/include/asm/pgtable.h`, `arch/arm64/mm/mmu.c`
>
> Virtual address space layout diagram:
>
> ![ARM64 Virtual Address Space Layout](images/arm64_va_space_48bit.svg)

---

## 1. Overview

ARM64 (AArch64) architecture uses a 64-bit virtual address space, but hardware only uses the lower bits for address translation. With `VA_BITS=48` and 4KB pages, the usable virtual address range is 2 x 256TB:

- **User space (TTBR0)**: `0x0000_0000_0000_0000` ~ `0x0000_FFFF_FFFF_FFFF` (256 TB)
- **Kernel space (TTBR1)**: `0xFFFF_0000_0000_0000` ~ `0xFFFF_FFFF_FFFF_FFFF` (256 TB)
- **Canonical hole**: `0x0001_0000_0000_0000` ~ `0xFFFE_FFFF_FFFF_FFFF` (unmappable)

Hardware uses bit[55] of the virtual address to select the translation table base register:

| bit[55] | Register | Address Space |
|---------|----------|---------------|
| 0 | TTBR0_EL1 | User space (per-process page tables) |
| 1 | TTBR1_EL1 | Kernel space (`swapper_pg_dir`) |

---

## 2. Kernel Virtual Address Space Layout

The 256TB kernel address space (TTBR1) is divided into two halves by `PAGE_END`:

- **Lower half** (`PAGE_OFFSET` ~ `PAGE_END`): **Linear map** (direct map), 128 TB
- **Upper half** (`PAGE_END` ~ `0xFFFF_FFFF_FFFF_FFFF`): **Non-linear mappings** (modules, vmalloc, kernel image, vmemmap, PCI I/O, fixmap)

### 2.1 Address Region Table (high to low)

| Virtual Address | Macro | Definition | Region |
|-----------------|-------|------------|--------|
| `0xFFFF_FFFF_FFFF_FFFF` | — | — | Guard / unused |
| `0xFFFF_FFFF_FF80_0000` | `FIXADDR_TOP` | `-UL(SZ_8M)` | **fixmap** (early console, FDT) |
| `0xFFFF_FFFF_C180_0000` | `PCI_IO_END` | `PCI_IO_START + SZ_16M` | |
| `0xFFFF_FFFF_C080_0000` | `PCI_IO_START` | `VMEMMAP_END + SZ_8M` | **PCI I/O** (16MB) |
| | | | [guard: 8MB] |
| `0xFFFF_FFFF_C000_0000` | `VMEMMAP_END` | `-UL(SZ_1G)` | **vmemmap** (struct page array) |
| (dynamic) | `VMEMMAP_START` | `VMEMMAP_END - VMEMMAP_SIZE` | |
| | | | [guard: 8MB] |
| (dynamic) | `VMALLOC_END` | `VMEMMAP_START - SZ_8M` | **vmalloc** (~127 TB) |
| | | | Kernel image is inside this range |
| `0xFFFF_8000_8000_0000` | `KIMAGE_VADDR` = `VMALLOC_START` | `MODULES_END` | |
| | | | **modules** (2GB) |
| `0xFFFF_8000_0000_0000` | `PAGE_END` = `MODULES_VADDR` | `-(1UL << 47)` | Boundary: non-linear / linear |
| | | | **Linear map** (128 TB) |
| `0xFFFF_0000_0000_0000` | `PAGE_OFFSET` | `-(1UL << 48)` | Start of kernel address space |

### 2.2 Key Macro Derivation Chain

The core definitions in `arch/arm64/include/asm/memory.h`:

```c
#define VA_BITS             (CONFIG_ARM64_VA_BITS)       // 48
#define _PAGE_OFFSET(va)    (-(UL(1) << (va)))           // -(1<<48) = 0xFFFF_0000_0000_0000
#define PAGE_OFFSET         (_PAGE_OFFSET(VA_BITS))
#define _PAGE_END(va)       (-(UL(1) << ((va) - 1)))     // -(1<<47) = 0xFFFF_8000_0000_0000
#define MODULES_VADDR       (_PAGE_END(VA_BITS_MIN))     // 0xFFFF_8000_0000_0000
#define MODULES_VSIZE       (SZ_2G)                      // 0x8000_0000
#define MODULES_END         (MODULES_VADDR + MODULES_VSIZE)  // 0xFFFF_8000_8000_0000
#define KIMAGE_VADDR        (MODULES_END)                // 0xFFFF_8000_8000_0000
```

And in `arch/arm64/include/asm/pgtable.h`:

```c
#define VMALLOC_START       (MODULES_END)                // = KIMAGE_VADDR
#define VMALLOC_END         (VMEMMAP_START - SZ_8M)
```

---

## 3. Linear Map (Direct Map)

### 3.1 What Is It

The linear map is a contiguous 128TB virtual address region starting at `PAGE_OFFSET` that maps **all physical RAM** with a fixed offset:

```c
// arch/arm64/include/asm/memory.h
#define __phys_to_virt(x)   ((unsigned long)((x) - PHYS_OFFSET) | PAGE_OFFSET)
#define __lm_to_phys(addr)  (((addr) - PAGE_OFFSET) + PHYS_OFFSET)
```

Given a physical address `PA`, its linear map virtual address is:

```
VA = (PA - PHYS_OFFSET) | PAGE_OFFSET
```

This is the foundation for `phys_to_virt()`, `virt_to_phys()`, `__va()`, `__pa()`.

### 3.2 How It Is Built: `map_mem()`

The linear map is established during early boot by `paging_init()` -> `map_mem(swapper_pg_dir)` in `arch/arm64/mm/mmu.c`:

```c
void __init paging_init(void)
{
    map_mem(swapper_pg_dir);   // Build the linear map
    memblock_allow_resize();
    create_idmap();            // Build identity mapping
    declare_kernel_vmas();     // Register kernel VMAs
}
```

The `map_mem()` function does the following:

**Step 1**: Temporarily mark the kernel image region `[_text, __init_begin)` as `NOMAP`:

```c
memblock_mark_nomap(kernel_start, kernel_end - kernel_start);
```

This prevents the generic loop from creating a writable alias of the kernel text/rodata.

**Step 2**: Map all other physical RAM with `PAGE_KERNEL | NX`:

```c
for_each_mem_range(i, &start, &end) {
    __map_memblock(pgdp, start, end, pgprot_tagged(PAGE_KERNEL), flags);
}
```

The `flags` include `NO_EXEC_MAPPINGS` — the linear map is **never executable**.

**Step 3**: Map the kernel image region separately with controlled permissions:

```c
__map_memblock(pgdp, kernel_start, kernel_end,
               PAGE_KERNEL, NO_CONT_MAPPINGS);
memblock_clear_nomap(kernel_start, kernel_end - kernel_start);
```

This creates the kernel image's **linear alias** (see Section 4), initially writable (for alternative patching), later tightened to read-only.

**Step 4** (later in boot): `mark_linear_text_alias_ro()` removes write permission:

```c
void __init mark_linear_text_alias_ro(void)
{
    update_mapping_prot(__pa_symbol(_text), (unsigned long)lm_alias(_text),
                        (unsigned long)__init_begin - (unsigned long)_text,
                        PAGE_KERNEL_RO);
}
```

### 3.3 Why the Linear Map Matters

- **Page allocator (buddy)**: Returns linear map addresses. `kmalloc()` and slab allocators operate here.
- **`phys_to_virt()` / `virt_to_phys()`**: Only work for linear map addresses.
- **Hibernate / kexec**: Access kernel memory pages as data through the linear alias.
- **DMA**: Drivers use linear map addresses for DMA-coherent buffers (via `virt_to_phys()`).

---

## 4. Dual Mapping of the Kernel Image

This is one of the most important concepts in the ARM64 memory layout: the kernel image's physical pages have **two separate virtual address mappings** with different permissions.

### 4.1 Mapping 1: Execution Mapping (at `KIMAGE_VADDR`)

Created during early boot by `head.S`. The kernel runs from here.

| Property | Value |
|----------|-------|
| Virtual address | `KIMAGE_VADDR + kaslr_offset` (around `0xFFFF_8000_8xxx_xxxx`) |
| Translation | `VA = PA + kimage_voffset` |
| `.text` permission | Read-Only + eXecutable (`PAGE_KERNEL_ROX`) |
| `.rodata` permission | Read-Only (`PAGE_KERNEL_RO`) |
| Purpose | **CPU fetches and executes instructions from here** |

### 4.2 Mapping 2: Linear Alias (at `PAGE_OFFSET` region)

Created by `map_mem()`. The same physical pages appear again in the linear map.

| Property | Value |
|----------|-------|
| Virtual address | `PAGE_OFFSET + offset` (around `0xFFFF_0000_0xxx_xxxx`) |
| Translation | `VA = (PA - PHYS_OFFSET) \| PAGE_OFFSET` |
| Permission | Read-Only + No-eXecute (`PAGE_KERNEL_RO` after `mark_linear_text_alias_ro()`) |
| Purpose | **Data access only** — hibernate, kexec, alternative patching |

### 4.3 Why Two Mappings?

When `map_mem()` maps all physical RAM into the linear map, the kernel image's physical pages are inevitably included. If mapped with the same `PAGE_KERNEL` (read-write) permission as normal RAM, this would create a **writable alias** of the read-only kernel text — a security violation (breaks W^X policy).

The solution is a three-phase approach:

1. **Skip** the kernel image during the generic RAM mapping loop (`memblock_mark_nomap`)
2. **Map** the kernel image separately with `PAGE_KERNEL` + `NO_CONT_MAPPINGS` (temporarily writable for alternative patching)
3. **Tighten** to `PAGE_KERNEL_RO` after patching is complete (`mark_linear_text_alias_ro()`)

### 4.4 Address Conversion Between the Two Mappings

```c
// include/linux/mm.h
#define lm_alias(x)    __va(__pa_symbol(x))
```

Given a kernel symbol address (Mapping 1), `lm_alias()` converts it to the linear map address (Mapping 2) by:
1. `__pa_symbol(x)`: Convert kernel image VA to physical address (using `kimage_voffset`)
2. `__va(pa)`: Convert physical address to linear map VA (using `PAGE_OFFSET`)

Example (assuming `PHYS_OFFSET=0x4000_0000`, no KASLR):

```
_text physical address:    0x4100_0000

Mapping 1 (execution):    0xFFFF_8000_8100_0000  (= PA + kimage_voffset)
Mapping 2 (linear alias): 0xFFFF_0000_0100_0000  (= (PA - PHYS_OFFSET) | PAGE_OFFSET)

lm_alias(_text) = 0xFFFF_0000_0100_0000
```

### 4.5 How `__virt_to_phys()` Handles Both Mappings

```c
// arch/arm64/include/asm/memory.h
#define __virt_to_phys_nodebug(x) ({
    phys_addr_t __x = (phys_addr_t)(__tag_reset(x));
    __is_lm_address(__x) ? __lm_to_phys(__x) : __kimg_to_phys(__x);
})
```

The kernel detects which mapping a virtual address belongs to:
- If in the linear map range (`PAGE_OFFSET` ~ `PAGE_END`): use `__lm_to_phys()` (subtract `PAGE_OFFSET`, add `PHYS_OFFSET`)
- Otherwise (kernel image range): use `__kimg_to_phys()` (subtract `kimage_voffset`)

---

## 5. vmalloc vs kmalloc: Different Address Regions

### 5.1 vmalloc — Non-Linear Region

```c
// arch/arm64/include/asm/pgtable.h
#define VMALLOC_START   (MODULES_END)           // = 0xFFFF_8000_8000_0000
#define VMALLOC_END     (VMEMMAP_START - SZ_8M) // near 0xFFFF_FFFF_xxxx_xxxx
```

| Property | Description |
|----------|-------------|
| VA range | `0xFFFF_8000_8000_0000` ~ `VMEMMAP_START - 8MB` (~127 TB) |
| Physical contiguity | **Not required** |
| Page tables | Created on demand, per allocation |
| Typical use | `vmalloc()`, `ioremap()`, `vmap()`, large buffers |

### 5.2 kmalloc — Linear Map Region

| Property | Description |
|----------|-------------|
| VA range | `PAGE_OFFSET` ~ `PAGE_END` (`0xFFFF_0000_xxxx` ~ `0xFFFF_8000_0000_0000`, 128 TB) |
| Physical contiguity | **Required** (guaranteed by buddy allocator) |
| Page tables | Pre-built by `map_mem()` at boot time, no per-allocation overhead |
| Typical use | `kmalloc()`, slab objects, small kernel data structures, DMA buffers |

### 5.3 Comparison

| | vmalloc | kmalloc |
|---|---------|---------|
| Address region | Non-linear (upper TTBR1) | Linear map (lower TTBR1) |
| Physical pages | Scattered, non-contiguous OK | Must be contiguous |
| Page table cost | Per-allocation page table setup | Zero (uses pre-built linear map) |
| `virt_to_phys()` | **Not directly usable** | Works directly |
| TLB efficiency | May be lower (scattered pages) | Higher (block/contiguous mappings possible) |
| Max allocation | Very large (TB-scale VA space) | Limited by contiguous free pages |

---

## 6. Other Key Regions

### 6.1 fixmap

- **Address**: Near `0xFFFF_FFFF_FF80_0000` (`FIXADDR_TOP`)
- **Purpose**: Compile-time-fixed virtual addresses for early boot mappings (FDT, early console, temporary I/O). Available before the full page allocator is online.

### 6.2 vmemmap

- **Address**: Up to `0xFFFF_FFFF_C000_0000` (`VMEMMAP_END`)
- **Purpose**: Virtual array of `struct page` for the sparse memory model. Each physical page frame has a corresponding `struct page` accessible via `vmemmap[pfn]`.

### 6.3 Modules

- **Address**: `0xFFFF_8000_0000_0000` ~ `0xFFFF_8000_8000_0000` (2GB)
- **Purpose**: Loadable kernel modules (`.ko`). The 2GB range ensures modules can use direct branch instructions to reach kernel symbols.

### 6.4 Identity Map (idmap)

Not part of the virtual address layout, but important for context:

- Created by `create_idmap()` in `paging_init()`
- Maps `VA == PA` for a small region of code (`__idmap_text_start` ~ `__idmap_text_end`)
- Used when enabling/disabling the MMU, CPU suspend/resume, and KPTI page table switching
- Stored in a separate page table `idmap_pg_dir`, loaded into TTBR0 temporarily

---

## 7. Boot Sequence: How the Address Space Is Established

```
head.S (early boot)
  |
  |-- Create initial kernel image mapping in swapper_pg_dir
  |   (maps _text ~ _end at KIMAGE_VADDR, executable)
  |
  |-- Enable MMU, jump to start_kernel()
  |
  v
paging_init()
  |
  |-- map_mem(swapper_pg_dir)
  |     |-- Map all RAM into linear map (PAGE_OFFSET region)
  |     |-- Special handling for kernel image linear alias
  |     '-- Map KFENCE pool if enabled
  |
  |-- memblock_allow_resize()
  |
  |-- create_idmap()
  |     '-- Build identity map in idmap_pg_dir
  |
  '-- declare_kernel_vmas()
        '-- Register kernel segments as early VMAs
              (_text, _rodata, _init, _data)

  ... later in boot ...

mark_linear_text_alias_ro()
  '-- Tighten linear alias of kernel text to PAGE_KERNEL_RO

mark_rodata_ro()
  '-- Mark .rodata as read-only in kernel image mapping
```

---

## 8. Summary Diagram

See the full diagram above, or open the SVG file directly: `arm64_va_space_48bit.svg`

```
0xFFFF_FFFF_FFFF_FFFF  +---------------------+
                       | guard               |
0xFFFF_FFFF_FF80_0000  | fixmap              |  FIXADDR_TOP
                       | PCI I/O (16MB)      |
0xFFFF_FFFF_C000_0000  | vmemmap             |  VMEMMAP_END
                       | [guard 8MB]         |
          (dynamic)    | vmalloc (~127TB)    |  VMALLOC_END
                       |   +---------------+ |
                       |   | Kernel Image  | |  (Mapping 1: Execution)
                       |   | .text (ROX)   | |
                       |   | .rodata (RO)  | |
                       |   | .init         | |
                       |   | .data (RW)    | |
                       |   +---------------+ |
0xFFFF_8000_8000_0000  | modules (2GB)       |  KIMAGE_VADDR = VMALLOC_START
0xFFFF_8000_0000_0000  |=====================|  PAGE_END = MODULES_VADDR
                       | Linear Map (128TB)  |
                       |   Normal RAM (RW,NX)|
                       |   Kernel linear     |  (Mapping 2: Data Access)
                       |     alias (RO,NX)   |
                       |   kmalloc/slab here  |
0xFFFF_0000_0000_0000  +---------------------+  PAGE_OFFSET
                       |                     |
                       | Canonical Addr Hole |
                       |                     |
0x0000_FFFF_FFFF_FFFF  +---------------------+
                       | User Space (256TB)  |  TTBR0
0x0000_0000_0000_0000  +---------------------+
```
