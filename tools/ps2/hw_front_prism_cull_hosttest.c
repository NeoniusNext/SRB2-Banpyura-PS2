// Source-extracted baseline/current cull: exact return parity, float boundary
// cases and a separate EE Count microbenchmark (not a whole-frame/FPS claim).
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef FRONT_PRISM_NATIVE
#include <kernel.h>
#endif
typedef struct { float x, y, z; } polyvertex_t;
typedef struct { float a, b, c, d; } fr_plane_t;
static struct { fr_plane_t p[5]; } frv;
#define FR_MARGIN 1.0f
#include "hw_front_prism_under_test.h"

static uint32_t random_state = 0x198ab32eu;
static uint32_t random32(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}
static float randomf(float scale)
{
	return ((float)(random32() & 0xffffffu) / 8388608.0f - 1.0f) * scale;
}
static unsigned long queries;
static int compare(polyvertex_t *pts, int n, float lo, float hi)
{
	const int a = ReferencePrismHidden(pts, n, lo, hi);
	const int b = HWR_FrPrismHidden(pts, n, lo, hi);
	const int c = PeeledPrismHidden(pts, n, lo, hi);
	queries++;
	if (a != b || a != c)
	{
		printf("FAIL query=%lu n=%d lo=%a hi=%a returns=%d/%d/%d\n", queries, n, (double)lo, (double)hi, a, b, c);
		return 0;
	}
	return 1;
}
static int correctness(void)
{
	polyvertex_t pts[64];
	const float special[] = {0, -0.0f, 1, -1, FLT_MIN, -FLT_MIN, FLT_MAX, -FLT_MAX, INFINITY, -INFINITY, NAN, 0x1p-149f, -0x1p-149f};
	unsigned int context, i, j, k;
	memset(pts, 0, sizeof pts);
	if (!compare(NULL, 0, 0, 0) || !compare(NULL, -1, 0, 0)) return 0;
	// The first point is outside. A later point needs the finite lower
	// endpoint even when the upper endpoint is NaN (selection negative).
	for (k = 0; k < 5; k++) frv.p[k] = (fr_plane_t){0, 1, 0, -100};
	frv.p[0] = (fr_plane_t){1, 1, 0, 0};
	pts[0].x = 100;
	if (!compare(pts, 4, 0, NAN)) return 0;
	memset(pts, 0, sizeof pts);
	for (i = 0; i < sizeof special / sizeof special[0]; i++)
		for (j = 0; j < sizeof special / sizeof special[0]; j++)
			for (k = 0; k < sizeof special / sizeof special[0]; k++)
			{
				unsigned int p;
				for (p = 0; p < 5; p++) frv.p[p] = (fr_plane_t){0, 1, 0, 0};
				for (p = 0; p < 4; p++) pts[p].x = special[k];
				frv.p[0].a = 1;
				if (!compare(pts, 4, special[i], special[j])) return 0;
			}
	memset(pts, 0, sizeof pts);
	for (i = 0; i < 64; i++)
	{
		const float lo = i % 3 == 0 ? nextafterf(1.0f, -INFINITY) : i % 3 == 1 ? 1 : nextafterf(1.0f, INFINITY);
		for (k = 0; k < 5; k++) frv.p[k] = (fr_plane_t){0, 1, 0, 0};
		if (!compare(pts, 64, lo, -lo) || !compare(pts, 64, lo, lo)) return 0;
	}
	for (context = 0; context <
#ifdef FRONT_PRISM_NATIVE
		32768
#else
		262144
#endif
		; context++)
	{
		const int n = 3 + (int)(random32() % 62);
		for (i = 0; i < 5; i++)
		{
			const float a = randomf(1), b = randomf(1), c = randomf(1);
			const float inv = 1.0f / sqrtf(a*a + b*b + c*c);
			frv.p[i] = (fr_plane_t){a*inv, b*inv, c*inv, randomf(32767)};
		}
		for (i = 0; i < (unsigned int)n; i++)
		{
			pts[i].x = randomf(32767);
			pts[i].y = randomf(32767);
		}
		if (!compare(pts, n, randomf(32767), randomf(32767))) return 0;
	}
	printf("PASS queries=%lu\n", queries);
	return 1;
}
#ifdef FRONT_PRISM_NATIVE
static volatile unsigned int sink;
static unsigned int count_now(void)
{
	unsigned int n;
	__asm__ volatile("sync\n\tmfc0 %0,$9" : "=r"(n) : : "memory");
	return n;
}
#define BENCH(name, fn) \
static __attribute__((noinline)) unsigned int name(const polyvertex_t *p, int n, float lo, float hi) \
{ \
	unsigned int i, sum = 0; \
	for (i = 0; i < 8192; i++) { __asm__ volatile("" : : : "memory"); sum += (unsigned int)fn(p,n,lo,hi); } \
	sink = sum; return sum; \
}
BENCH(bench_reference, ReferencePrismHidden)
BENCH(bench_current, HWR_FrPrismHidden)
BENCH(bench_peeled, PeeledPrismHidden)
static int benchmark(void)
{
	const int sizes[] = {4, 16, 64};
	polyvertex_t pts[64];
	int pattern, size, i, j, sample;
	for (pattern = 0; pattern < 6; pattern++)
		for (size = 0; size < 3; size++)
		{
			const int n = sizes[size];
			unsigned int best[3] = {UINT32_MAX,UINT32_MAX,UINT32_MAX};
			float lo = -20, hi = 20;
			for (j = 0; j < 5; j++)
				frv.p[j] = (fr_plane_t){0.5f, j & 1 ? -0.75f : 0.75f, 0.25f, -100};
			if (pattern == 1) for (j = 0; j < 5; j++) frv.p[j].d = -5;
			if (pattern == 2) frv.p[0].d = 100;
			if (pattern == 3) frv.p[4].d = 100;
			if (pattern == 4) { lo = -200; hi = -100; }
			if (pattern == 5) { lo = 100; hi = 200; }
			for (i = 0; i < n; i++)
			{
				pts[i].x = pattern == 1 ? (float)(n-i) * 5.0f : 0;
				pts[i].y = 0;
				pts[i].z = 0;
			}
			for (sample = 0; sample < 5; sample++)
			{
				unsigned int start, values[3], counts[3];
				start=count_now(); values[0]=bench_reference(pts,n,lo,hi); counts[0]=count_now()-start;
				start=count_now(); values[1]=bench_current(pts,n,lo,hi); counts[1]=count_now()-start;
				start=count_now(); values[2]=bench_peeled(pts,n,lo,hi); counts[2]=count_now()-start;
				if (values[0] != values[1] || values[0] != values[2]) { puts("EETEST FAIL benchmark returns"); return 0; }
				for (i=0;i<3;i++) if(counts[i]<best[i]) best[i]=counts[i];
			}
			printf("EETEST PRISM pattern=%d vertices=%d queries=8192 reference=%u current=%u peeled=%u\n", pattern,n,best[0],best[1],best[2]);
		}
	return 1;
}
#endif
int main(int argc, char **argv)
{
	(void)argc; (void)argv;
	if (!correctness()) return 1;
#ifdef FRONT_PRISM_NATIVE
	if (!benchmark()) return 1;
	puts("EETEST DONE");
	SleepThread();
#endif
	return 0;
}
