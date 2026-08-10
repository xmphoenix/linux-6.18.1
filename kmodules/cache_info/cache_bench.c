/*
 * cache_bench.c - ARM64 Cache Behavior Observation Toolkit
 *
 * Cross-compile on host:
 *   aarch64-linux-gnu-gcc -O2 -static cache_bench.c -o cache_bench
 *
 * Run in QEMU (mount 9p first):  /mnt/cache_info/cache_bench
 *
 * Tests:
 *   1. Cache line size effect (sequential vs strided)
 *   2. L1 / L2 / DRAM latency hierarchy (pointer chase)
 *   3. False sharing demonstration
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define ARRAY_SIZE (4 * 1024 * 1024)
#define ITERATIONS 100

/* Read ARM64 system counter (cntvct_el0) - runs at ~62.5 MHz on QEMU virt */
static uint64_t rdtsc(void)
{
	uint64_t val;
	asm volatile("isb; mrs %0, cntvct_el0" : "=r"(val));
	return val;
}

/* Test 1: Stride through a big array. Stride 1 = sequential = many cache hits.
   Stride 16 = 128 bytes apart = 2 cache lines = essentially random access. */
static void test_stride(void)
{
	printf("========== Test 1: Stride Access (cache line size effect) ==========\n");
	printf("Stride   Time(ticks)   PerAccess   Note\n");

	for (int s = 1; s <= 32; s = (s < 4) ? s + 1 : s * 2) {
		char *buf = malloc(ARRAY_SIZE);
		memset(buf, 0, ARRAY_SIZE);

		uint64_t t0 = rdtsc();
		for (int r = 0; r < ITERATIONS; r++)
			for (int i = 0; i < ARRAY_SIZE; i += s)
				(void)buf[i];
		uint64_t t1 = rdtsc();

		int n = ITERATIONS * (ARRAY_SIZE / s);
		const char *note = "";
		if (s == 1)  note = "sequential, every byte in same line";
		if (s == 8)  note = "8B = word size";
		if (s == 16) note = "128B stride = 2 cache lines apart";
		if (s == 32) note = "256B stride = 4 cache lines apart";

		printf("%-8d %-12.1f %-10.1f  %s\n", s, (double)(t1 - t0) / n,
		       (double)(t1 - t0) / n, note);
		free(buf);
	}
}

/* Test 2: Pointer chase through linked lists of varying sizes.
   Working set fits in L1 → fast. Fits in L2 → medium. Exceeds L2 → slow. */
static void test_latency(void)
{
	printf("\n========== Test 2: Memory Hierarchy Latency ==========\n");
	printf("WorkSet    Latency(ticks)   Note (Cortex-A57: L1D=32KB, L2=2048KB)\n");

	for (int kb = 8; kb <= 8192; kb *= 4) {
		int count = (kb * 1024) / sizeof(void *);
		void **buf = malloc(count * sizeof(void *));
		int *idx = malloc(count * sizeof(int));
		for (int i = 0; i < count; i++) idx[i] = i;
		srand(42);
		for (int i = count - 1; i > 0; i--) {
			int j = rand() % (i + 1);
			int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
		}
		for (int i = 0; i < count - 1; i++) buf[idx[i]] = &buf[idx[i + 1]];
		buf[idx[count - 1]] = &buf[idx[0]];

		int trials = (kb <= 64) ? count * 200 : count * 20;
		if (trials < 50000) trials = 50000;
		void *p = (void *)buf;
		for (int i = 0; i < count; i++) p = *(void **)p; /* warm */

		uint64_t t0 = rdtsc();
		p = (void *)buf;
		for (int i = 0; i < trials; i++) p = *(void **)p;
		uint64_t t1 = rdtsc();
		(void)p;

		const char *note = "";
		if (kb == 8)    note = "L1 D$ (32KB)";
		if (kb == 32)   note = "L1 D$ edge";
		if (kb == 128)  note = "L2 (2048KB)";
		if (kb == 2048) note = "L2 edge";
		if (kb == 8192) note = "DRAM territory";

		printf("%6d KB  %-16.1f  %s\n", kb, (double)(t1 - t0) / trials, note);
		free(buf);
		free(idx);
	}
}

/* Test 3: False sharing.
   Two variables on the SAME line → cache line ping-pong between cores.
   Two variables on DIFFERENT lines → no interference. */
static void test_false_sharing(void)
{
	printf("\n========== Test 3: False Sharing ==========\n");
	const long N = 50000000;

	struct { long a; char pad[120]; long b; } s1 = {0, {0}, 0};
	struct { long a; long b; } s2 = {0, 0};

	uint64_t t0 = rdtsc();
	for (long i = 0; i < N; i++) { s1.a++; s1.b++; }
	uint64_t t1 = rdtsc();
	printf("Different lines (120B pad):  %.1f ticks/op\n", (double)(t1 - t0) / N);

	t0 = rdtsc();
	for (long i = 0; i < N; i++) { s2.a++; s2.b++; }
	t1 = rdtsc();
	printf("Same line (adjacent):       %.1f ticks/op\n", (double)(t1 - t0) / N);
}

int main(void)
{
	printf("ARM64 Cache Observation Toolkit\n");
	printf("System counter: ~62.5 MHz on QEMU virt (absolute ticks, not ns)\n\n");
	test_stride();
	test_latency();
	test_false_sharing();
	printf("\nDone. Compare ratios to see cache effects.\n");
	return 0;
}
