// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "mem_allocator_lab: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/vmalloc.h>

static unsigned int page_order;
module_param(page_order, uint, 0644);
MODULE_PARM_DESC(page_order, "Order passed to alloc_pages() for buddy validation");

static unsigned int kmalloc_bytes = 64;
module_param(kmalloc_bytes, uint, 0644);
MODULE_PARM_DESC(kmalloc_bytes, "Size passed to kmalloc() for SLUB validation");

static unsigned int cache_bytes = 128;
module_param(cache_bytes, uint, 0644);
MODULE_PARM_DESC(cache_bytes, "Object size for the custom kmem_cache");

static unsigned int cache_objects = 8;
module_param(cache_objects, uint, 0644);
MODULE_PARM_DESC(cache_objects, "Number of objects to allocate from the custom kmem_cache");

static unsigned int vmalloc_base_kb = 64;
module_param(vmalloc_base_kb, uint, 0644);
MODULE_PARM_DESC(vmalloc_base_kb, "Base size in KiB for the vmalloc reuse and merge experiment (0 disables it)");

static struct page *lab_pages;
static void *kmalloc_object;
static struct kmem_cache *lab_cache;
static void **cache_allocations;
static void *vmalloc_a;
static void *vmalloc_b;
static void *vmalloc_c;
static void *vmalloc_d;
static void *vmalloc_e;

static void lab_free_cache_objects(void);
static void lab_free_vmalloc_objects(void);

static void lab_log_page(const char *label, struct page *page)
{
	void *linear_addr;
	struct page *round_trip;
	phys_addr_t phys;

	if (!page) {
		pr_info("%s: no page allocated\n", label);
		return;
	}

	linear_addr = page_to_virt(page);
	round_trip = virt_to_page(linear_addr);
	phys = page_to_phys(page);

	pr_info("%s: page=%px pfn=%lu phys=%pa linear=%px round_trip=%px same_page=%d folio_order=%u virt_valid=%d\n",
		label,
		page,
		page_to_pfn(page),
		&phys,
		linear_addr,
		round_trip,
		round_trip == page,
		folio_order(page_folio(page)),
		virt_addr_valid(linear_addr));
}

static void lab_log_object(const char *label, void *object)
{
	struct page *page;
	phys_addr_t page_phys;
	phys_addr_t phys;

	if (!object) {
		pr_info("%s: allocation failed\n", label);
		return;
	}

	if (!virt_addr_valid(object) || is_vmalloc_addr(object)) {
		pr_info("%s: ptr=%px ksize=%zu virt_valid=%d is_vmalloc=%d\n",
			label,
			object,
			ksize(object),
			virt_addr_valid(object),
			is_vmalloc_addr(object));
		return;
	}

	page = virt_to_page(object);
	page_phys = page_to_phys(page);
	phys = page_phys + offset_in_page(object);

	pr_info("%s: ptr=%px ksize=%zu page=%px pfn=%lu obj_phys=%pa page_phys=%pa offset=%zu virt_valid=%d is_vmalloc=%d\n",
		label,
		object,
		ksize(object),
		page,
		page_to_pfn(page),
		&phys,
		&page_phys,
		offset_in_page(object),
		virt_addr_valid(object),
		is_vmalloc_addr(object));
}

static void lab_log_vmalloc_area(const char *label, const void *addr,
				 unsigned long requested)
{
	struct page *page;
	phys_addr_t phys = 0;
	unsigned long first_pfn = 0;

	if (!addr) {
		pr_info("%s: vmalloc allocation failed\n", label);
		return;
	}

	page = vmalloc_to_page(addr);
	if (page) {
		phys = page_to_phys(page) + offset_in_page(addr);
		first_pfn = page_to_pfn(page);
	}

	pr_info("%s: addr=%px end=%px requested=%lu is_vmalloc=%d first_page=%px first_pfn=%lu obj_phys=%pa\n",
		label,
		addr,
		(void *)((unsigned long)addr + requested),
		requested,
		is_vmalloc_addr(addr),
		page,
		first_pfn,
		&phys);
}

static int lab_run_vmalloc_case(void)
{
	unsigned long base;
	unsigned long size_a;
	unsigned long size_b;
	unsigned long size_c;
	unsigned long size_e;
	unsigned long saved_a;
	unsigned long saved_b;
	unsigned long saved_d;

	if (!vmalloc_base_kb) {
		pr_info("vmalloc_case: skipped because vmalloc_base_kb=0\n");
		return 0;
	}

	base = PAGE_ALIGN((unsigned long)vmalloc_base_kb << 10);
	if (base < PAGE_SIZE)
		base = PAGE_SIZE;

	size_a = base;
	size_b = base * 2;
	size_c = base;
	size_e = size_a + size_b;

	pr_info("vmalloc_case: base=%lu A=%lu B=%lu C=%lu E=%lu\n",
		base, size_a, size_b, size_c, size_e);

	vmalloc_a = vmalloc(size_a);
	if (!vmalloc_a)
		goto fail;
	vmalloc_b = vmalloc(size_b);
	if (!vmalloc_b)
		goto fail;
	vmalloc_c = vmalloc(size_c);
	if (!vmalloc_c)
		goto fail;

	saved_a = (unsigned long)vmalloc_a;
	saved_b = (unsigned long)vmalloc_b;

	lab_log_vmalloc_area("vmalloc_A", vmalloc_a, size_a);
	lab_log_vmalloc_area("vmalloc_B", vmalloc_b, size_b);
	lab_log_vmalloc_area("vmalloc_C", vmalloc_c, size_c);

	vfree(vmalloc_b);
	vmalloc_b = NULL;
	pr_info("vmalloc_case: freed B=%px; a later same-size allocation should search the free tree and reuse it first\n",
		(void *)saved_b);

	vmalloc_d = vmalloc(size_b);
	if (!vmalloc_d)
		goto fail;
	saved_d = (unsigned long)vmalloc_d;
	lab_log_vmalloc_area("vmalloc_D", vmalloc_d, size_b);
	pr_info("vmalloc_case: reuse_check reused_old_B=%d old_B=%px new_D=%px\n",
		saved_d == saved_b, (void *)saved_b, vmalloc_d);

	vfree(vmalloc_a);
	vmalloc_a = NULL;
	pr_info("vmalloc_case: freed A=%px\n", (void *)saved_a);

	vfree(vmalloc_d);
	vmalloc_d = NULL;
	pr_info("vmalloc_case: freed D=%px; free-tree insertion can now merge A and D because they are adjacent ranges\n",
		(void *)saved_d);

	vmalloc_e = vmalloc(size_e);
	if (!vmalloc_e)
		goto fail;
	lab_log_vmalloc_area("vmalloc_E", vmalloc_e, size_e);
	pr_info("vmalloc_case: merge_check reused_old_A=%d old_A=%px new_E=%px\n",
		(unsigned long)vmalloc_e == saved_a, (void *)saved_a, vmalloc_e);

	return 0;

fail:
	pr_err("vmalloc_case: allocation sequence failed, base=%lu bytes\n", base);
	return -ENOMEM;
}

static int lab_alloc_cache_objects(void)
{
	unsigned int index;

	if (!cache_objects)
		return 0;

	cache_allocations = kcalloc(cache_objects, sizeof(*cache_allocations), GFP_KERNEL);
	if (!cache_allocations)
		return -ENOMEM;

	for (index = 0; index < cache_objects; index++) {
		cache_allocations[index] = kmem_cache_alloc(lab_cache, GFP_KERNEL);
		if (!cache_allocations[index]) {
			pr_err("kmem_cache_alloc failed at index %u\n", index);
			lab_free_cache_objects();
			return -ENOMEM;
		}

		lab_log_object("cache_object", cache_allocations[index]);
	}

	return 0;
}

static void lab_free_cache_objects(void)
{
	unsigned int index;

	if (!cache_allocations)
		return;

	for (index = 0; index < cache_objects; index++) {
		if (cache_allocations[index])
			kmem_cache_free(lab_cache, cache_allocations[index]);
	}

	kfree(cache_allocations);
	cache_allocations = NULL;
}

static void lab_free_vmalloc_objects(void)
{
	vfree(vmalloc_e);
	vmalloc_e = NULL;
	vfree(vmalloc_d);
	vmalloc_d = NULL;
	vfree(vmalloc_c);
	vmalloc_c = NULL;
	vfree(vmalloc_b);
	vmalloc_b = NULL;
	vfree(vmalloc_a);
	vmalloc_a = NULL;
}

static int __init mem_allocator_lab_init(void)
{
	int ret;

	pr_info("starting: page_order=%u kmalloc_bytes=%u cache_bytes=%u cache_objects=%u vmalloc_base_kb=%u\n",
		page_order, kmalloc_bytes, cache_bytes, cache_objects, vmalloc_base_kb);

	lab_pages = alloc_pages(GFP_KERNEL | __GFP_ZERO, page_order);
	if (!lab_pages)
		return -ENOMEM;

	lab_log_page("alloc_pages", lab_pages);

	kmalloc_object = kmalloc(kmalloc_bytes, GFP_KERNEL);
	if (!kmalloc_object) {
		ret = -ENOMEM;
		goto err_free_pages;
	}

	lab_log_object("kmalloc", kmalloc_object);

	lab_cache = kmem_cache_create("mem_allocator_lab",
					 cache_bytes, 0,
					 SLAB_HWCACHE_ALIGN, NULL);
	if (!lab_cache) {
		ret = -ENOMEM;
		goto err_free_kmalloc;
	}

	ret = lab_alloc_cache_objects();
	if (ret)
		goto err_destroy_cache;

	ret = lab_run_vmalloc_case();
	if (ret)
		goto err_destroy_cache;

	pr_info("ready: inspect dmesg, /proc/buddyinfo and /proc/slabinfo\n");
	return 0;

err_destroy_cache:
	lab_free_vmalloc_objects();
	kmem_cache_destroy(lab_cache);
	lab_cache = NULL;
err_free_kmalloc:
	kfree(kmalloc_object);
	kmalloc_object = NULL;
err_free_pages:
	__free_pages(lab_pages, page_order);
	lab_pages = NULL;
	return ret;
}

static void __exit mem_allocator_lab_exit(void)
{
	lab_free_vmalloc_objects();
	lab_free_cache_objects();

	if (lab_cache) {
		kmem_cache_destroy(lab_cache);
		lab_cache = NULL;
	}

	kfree(kmalloc_object);
	kmalloc_object = NULL;

	if (lab_pages) {
		__free_pages(lab_pages, page_order);
		lab_pages = NULL;
	}

	pr_info("done\n");
}

module_init(mem_allocator_lab_init);
module_exit(mem_allocator_lab_exit);

MODULE_DESCRIPTION("ARM64 allocator and vmalloc validation module");
MODULE_LICENSE("GPL");