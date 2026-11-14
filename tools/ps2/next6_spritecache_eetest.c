/* Paired separately compiled actual sprite-header writes and four-value reads.
 * Timing covers these gathers plus their common call/store loop, not rendering. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifndef __mips__
#include <time.h>
#endif
void reference_write(unsigned, int16_t, int16_t, int16_t, int16_t);
void candidate_write(unsigned, int16_t, int16_t, int16_t, int16_t);
void reference_integer(unsigned, int32_t *);
void candidate_integer(unsigned, int32_t *);
void reference_float(unsigned, float *);
void candidate_float(unsigned, float *);
static uint64_t digest = UINT64_C(1469598103934665603);
static unsigned checks;
static volatile uint32_t sink;
static uint32_t now(void)
{
#ifdef __mips__
	uint32_t v;
	__asm__ volatile("mfc0 %0,$9" : "=r"(v));
	return v;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec);
#endif
}
static int exact(void)
{
	uint32_t n, j;
	for (n = 0; n < 65536; ++n)
	{
		uint16_t w = (uint16_t)n, h = (uint16_t)(n * 32769u + 16383u);
		uint16_t t = (uint16_t)(n * 65521u + 32764u), l = (uint16_t)(n * 8191u + 65535u);
		const uint32_t want[] = { (uint32_t)w << 16, (uint32_t)h << 16,
			(uint32_t)l << 16, ((uint32_t)t << 16) + UINT32_C(262144) };
		int32_t a[4], b[4];
		float fa[4], fb[4];
		unsigned index = n & 8191u;
		reference_write(index, (int16_t)w, (int16_t)h, (int16_t)t, (int16_t)l);
		candidate_write(index, (int16_t)w, (int16_t)h, (int16_t)t, (int16_t)l);
		reference_integer(index, a); candidate_integer(index, b);
		reference_float(index, fa); candidate_float(index, fb);
		for (j = 0; j < 4; ++j)
		{
			checks += 3;
			if ((uint32_t)a[j] != want[j] || (uint32_t)b[j] != want[j]
				|| memcmp(fa + j, fb + j, sizeof(float)))
			{
				printf("SPRCACHE6 EE FAIL n=%lu field=%lu expected=%08lx old=%08lx new=%08lx\n",
					(unsigned long)n, (unsigned long)j, (unsigned long)want[j],
					(unsigned long)(uint32_t)a[j], (unsigned long)(uint32_t)b[j]);
				return 1;
			}
			digest = (digest ^ (uint32_t)b[j]) * UINT64_C(1099511628211);
		}
	}
	printf("SPRCACHE6 EE EXACT inputs=65536 fields=4 checks=%u hash=%016llx\n", checks, (unsigned long long)digest);
	return 0;
}
static void bench(void)
{
	static const unsigned sizes[] = { 1, 64, 8192 };
	unsigned n, stride, kind, rep, order, k;
	const unsigned calls = 8192;
	for (n = 0; n < 8192; ++n)
	{
		reference_write(n, 64 + n % 257, 48 + n % 127, n % 256, -(int16_t)(n % 96));
		candidate_write(n, 64 + n % 257, 48 + n % 127, n % 256, -(int16_t)(n % 96));
	}
	for (n = 0; n < sizeof sizes / sizeof sizes[0]; ++n)
	for (stride = 1; stride <= 17; stride += 16)
	for (kind = 0; kind < 2; ++kind)
	{
		uint32_t best[2] = { UINT32_MAX, UINT32_MAX };
		int32_t out[4];
		float fout[4];
		for (rep = 0; rep < 5; ++rep)
		for (order = 0; order < 2; ++order)
		{
			unsigned variant = (rep + order) & 1;
			uint32_t start, elapsed, sum = 0;
			void (*iread)(unsigned, int32_t *) = variant ? candidate_integer : reference_integer;
			void (*fread)(unsigned, float *) = variant ? candidate_float : reference_float;
			start = now();
			if (kind)
				for (k = 0; k < calls; ++k)
				{
					uint32_t bits;
					fread((k * stride) & (sizes[n] - 1), fout);
					memcpy(&bits, fout, sizeof bits); sum += bits;
				}
			else
				for (k = 0; k < calls; ++k)
				{
					iread((k * stride) & (sizes[n] - 1), out); sum += (uint32_t)out[0];
				}
			elapsed = now() - start;
			sink = sum;
			if (elapsed < best[variant]) best[variant] = elapsed;
		}
		printf("SPRCACHE6 BENCH kind=%s entries=%u stride=%u calls=%u base=%lu cand=%lu unit=%s\n",
			kind ? "float4" : "fixed4", sizes[n], stride, calls,
			(unsigned long)((uint64_t)best[0] * 100 / calls),
			(unsigned long)((uint64_t)best[1] * 100 / calls),
#ifdef __mips__
			"cycles_x100"
#else
			"ns_x100"
#endif
		);
	}
}
int main(void)
{
	if (exact()) return 1;
	bench();
	puts("SPRCACHE6 EE DONE");
	return 0;
}
