/*
 * cache_info.c - ARM64 Cache Information Kernel Module
 *
 * Reads cache geometry directly from ARMv8-A hardware registers
 * (CCSIDR_EL1, CLIDR_EL1) that are only accessible at EL1.
 *
 * Build:
 *   cd /path/to/kernel-source
 *   make M=kmodules/cache_info modules
 *
 * Usage:
 *   insmod kmodules/cache_info/cache_info.ko && dmesg | tail -50
 *
 * ARMv8-A registers used:
 *   CLIDR_EL1  - Cache Level ID (discovers cache hierarchy)
 *   CSSELR_EL1 - Cache Size Selection (selects which cache to query)
 *   CCSIDR_EL1 - Current Cache Size ID (line/way/set geometry)
 *   CTR_EL0    - Cache Type Register (minimum line sizes)
 */

#include <linux/module.h>
#include <linux/kernel.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ARM64 Cache Info Dumper (reads CCSIDR_EL1)");

/* CSSELR_EL1: Cache Size Selection Register
 *   [3:1] = Level (0=L1, 1=L2, ...)
 *   [0]   = InD (0=Data/unified, 1=Instruction)
 */
static inline void select_cache(int level, int ind)
{
	u64 csselr = ((u64)level << 1) | (ind ? 1 : 0);
	asm volatile("msr csselr_el1, %0" :: "r"(csselr) : "memory");
}

static inline u64 read_ccsidr(void)
{
	u64 v;
	asm volatile("mrs %0, ccsidr_el1" : "=r"(v));
	return v;
}

struct cache_params {
	int line_size;     /* bytes */
	int num_ways;       /* associativity */
	int num_sets;       /* number of sets */
	int cache_size;     /* bytes */
};

/*
 * Decode CCSIDR_EL1 per ARMv8-A ARM (DDI 0487).
 *
 * Format:
 *   [27:13]  NumSets      (number of sets - 1)
 *   [12:3]   Associativity (number of ways - 1)
 *   [2:0]    LineSize     (log2(words_in_line) - 2, word=4 bytes)
 *
 * Derivation:
 *   line_bytes = 2^(LineSize_field + 4)    // 2^(n+2) words × 4 bytes/word
 *   ways       = Associativity + 1
 *   sets       = NumSets + 1
 *   size       = ways × sets × line_bytes
 */
static void decode_ccsidr(u64 ccsidr, struct cache_params *p)
{
	int ls = ccsidr & 0x7;                 /* CCSIDR[2:0]   */
	int assoc = (ccsidr >> 3) & 0x3FF;     /* CCSIDR[12:3]  */
	int nsets = (ccsidr >> 13) & 0x7FFF;   /* CCSIDR[27:13] */

	p->line_size  = 1 << (ls + 4);
	p->num_ways   = assoc + 1;
	p->num_sets   = nsets + 1;
	p->cache_size = p->num_ways * p->num_sets * p->line_size;
}

static const char *ctype_name(int ctype)
{
	static const char *t[] = {
		[0] = "None", [1] = "Instruction",
		[2] = "Data", [3] = "Separate I+D",
		[4] = "Unified"
	};
	return (ctype <= 4) ? t[ctype] : "Reserved";
}

static void dump_one_cache(int level, int ind, const char *label)
{
	struct cache_params p;

	select_cache(level, ind);
	decode_ccsidr(read_ccsidr(), &p);

	pr_info("  %-10s  %5d KB  %2d-way  %4d sets  %3d B/line  CCSIDR=0x%llx\n",
		label,
		p.cache_size / 1024, p.num_ways, p.num_sets, p.line_size,
		read_ccsidr());
}

static int __init cache_info_init(void)
{
	u64 clidr, ctr;
	int level;

	asm volatile("mrs %0, clidr_el1" : "=r"(clidr));
	asm volatile("mrs %0, ctr_el0"   : "=r"(ctr));

	pr_info("==============================================================\n");
	pr_info("  ARM64 Cache Info (from CCSIDR_EL1 / CLIDR_EL1)\n");
	pr_info("==============================================================\n");
	pr_info("CLIDR_EL1  = 0x%016llx\n", clidr);
	pr_info("  LoUU (Level of Unification) = %llu\n", (clidr >> 21) & 0x7);
	pr_info("  LoC  (Level of Coherency)   = %llu\n", (clidr >> 18) & 0x7);
	/*
	 * CTR_EL0 IminLine/DminLine field encoding (ARMv8 ARM DDI 0487):
	 *   field = Log2(num_words_in_line) - 2
	 *   line_bytes = 4 * 2^(field + 2)  (word = 4 bytes in AArch64)
	 */
	pr_info("CTR_EL0    = 0x%016llx\n", ctr);
	pr_info("  IminLine   = %llu B   DminLine   = %llu B\n",
		(4ULL << (((ctr & 0xF) + 2) & 0x1F)),
		(4ULL << ((((ctr >> 16) & 0xF) + 2) & 0x1F)));

	pr_info("\n%-4s  %-10s  %7s  %6s  %8s  %6s  %s\n",
		"Lvl", "Type", "Size", "Ways", "Sets", "Line", "CCSIDR");
	pr_info("----  ----------  -------  ------  --------  -----  -------\n");

	for (level = 0; level < 7; level++) {
		int ctype = (clidr >> (3 * level)) & 0x7;
		if (ctype == 0)
			break;

		pr_info("L%d    %s\n", level + 1, ctype_name(ctype));

		switch (ctype) {
		case 1: /* Instruction only */
			dump_one_cache(level, 1, "  I$");
			break;
		case 2: /* Data only */
			dump_one_cache(level, 0, "  D$");
			break;
		case 3: /* Separate I+D */
			dump_one_cache(level, 0, "  D$");
			dump_one_cache(level, 1, "  I$");
			break;
		case 4: /* Unified */
			dump_one_cache(level, 0, "  Unified");
			break;
		}
	}

	pr_info("\n==============================================================\n");
	return 0;
}

static void __exit cache_info_exit(void)
{
	pr_info("cache_info: removed\n");
}

module_init(cache_info_init);
module_exit(cache_info_exit);
