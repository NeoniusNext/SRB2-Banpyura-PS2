/* Host test (gcc, Linux) of the bit-identical helpers added by the OPT10-SW work (PS2-89 and later) against the original expressions.
 * Built and run by tools/ps2/sw_hosttest.py. Prints "SW <name> checks=<n> failures=<n>" and "SW DONE".
 * -DSW_BROKEN builds negative controls with a deliberately wrong reference/candidate (must fail). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m_fixed.h"
#include "tables.h"
#include "ps2/ps2_rdraw.h"
#include "ps2/ps2_nodescan.h"

static UINT64 st = 88172645463325252ull;
static UINT32 rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return (UINT32)(st >> 16); }
static UINT32 rnd_mixed(void)
{
	UINT32 r = rnd(), v = rnd();
	switch (r & 7)
	{
		case 0: return v;
		case 1: return v >> (r >> 8 & 31);
		case 2: return v & ((1u << (1 + (r >> 8 & 31))) - 1u);
		case 3: return (1u << (r >> 8 & 31)) + (UINT32)((int)(v % 7) - 3);
		case 4: return 0u - (v >> (r >> 8 & 31));
		case 5: return (v % 4096u) << (r >> 8 & 15);
		case 6: return (UINT32)((int)(v % 65536) - 32768) << 16;
		default: { static const UINT32 e[] = {0, 1, 2, 255, 256, 65535, 65536, 65537, 0x1FFFFFFF, 0x20000000, 0x7FFF0000u, 0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xFFFFFFFFu, 0xFFFF0000u}; return e[v % 16]; }
	}
}

/* ---- PS2-89: R_IsPointInSector edge test (src/r_main.c) ---- */
static int ref_EdgeXLess(fixed_t v1x, fixed_t v1y, fixed_t v2x, fixed_t v2y, fixed_t x, fixed_t y)
{
	/* the original expression, int arithmetic wrapping like -fwrapv */
	fixed_t vx = (fixed_t)((INT64)v1x + (INT64)(fixed_t)((UINT32)v2x - (UINT32)v1x) * (fixed_t)((UINT32)y - (UINT32)v1y) / (fixed_t)((UINT32)v2y - (UINT32)v1y));
#ifdef SW_BROKEN
	return vx <= x;
#else
	return vx < x;
#endif
}

static void test_edge(unsigned long long *checks, unsigned long long *fails)
{
	UINT32 i;
	for (i = 0; i < 120000000u; i++)
	{
		fixed_t v1x = (fixed_t)rnd_mixed(), v1y = (fixed_t)rnd_mixed(), v2x = (fixed_t)rnd_mixed(), v2y = (fixed_t)rnd_mixed();
		fixed_t x = (fixed_t)rnd_mixed(), y = (fixed_t)rnd_mixed();
		const UINT32 mode = rnd() & 3;
		fixed_t t;
		if (mode == 0) /* map-like: coordinates inside +-32768 units, the common case */
		{
			v1x = (fixed_t)((int)(rnd() % 65536) - 32768) * FRACUNIT + (fixed_t)(rnd() & 0xFFFF);
			v2x = (fixed_t)((int)(rnd() % 65536) - 32768) * FRACUNIT + (fixed_t)(rnd() & 0xFFFF);
			v1y = (fixed_t)((int)(rnd() % 65536) - 32768) * FRACUNIT + (fixed_t)(rnd() & 0xFFFF);
			v2y = (fixed_t)((int)(rnd() % 65536) - 32768) * FRACUNIT + (fixed_t)(rnd() & 0xFFFF);
			x = (fixed_t)((int)(rnd() % 65536) - 32768) * FRACUNIT + (fixed_t)(rnd() & 0xFFFF);
		}
		else if (mode == 1) /* x on or next to the intersection point (the boundary of the comparison) */
		{
			x = v1x + (fixed_t)((INT64)(fixed_t)((UINT32)v2x - (UINT32)v1x) * (fixed_t)((UINT32)(y) - (UINT32)v1y) / ((fixed_t)((UINT32)v2y - (UINT32)v1y) ? (fixed_t)((UINT32)v2y - (UINT32)v1y) : 1));
			x += (fixed_t)(rnd() % 3) - 1;
		}
		/* the caller guarantees v1y < y <= v2y: bring the sample into that domain (also try it unconstrained in mode 3) */
		if (mode != 3)
		{
			if (v1y > v2y) { t = v1y; v1y = v2y; v2y = t; }
			if (v1y == v2y) continue;
			if (!(v1y < y && y <= v2y))
			{
				/* y inside the interval, possibly at its ends */
				const UINT32 span = (UINT32)v2y - (UINT32)v1y;
				y = (fixed_t)((UINT32)v1y + 1u + (span > 1 ? rnd() % span : 0));
				if (rnd() % 8 == 0) y = v2y;
				if (!(v1y < y && y <= v2y)) continue;
			}
		}
		else if (v1y == v2y)
			continue;
		else if (v1y > v2y)
		{
			t = v1y; v1y = v2y; v2y = t;
		}
		checks[0]++;
		if ((int)PS2_EdgeXLess(v1x, v1y, v2x, v2y, x, y) != ref_EdgeXLess(v1x, v1y, v2x, v2y, x, y))
			if (fails[0]++ < 8)
				printf("SW FAIL edge v1=(%d,%d) v2=(%d,%d) p=(%d,%d)\n", v1x, v1y, v2x, v2y, x, y);
	}
}


/* ---- PS2-161: R_SortVisSprites stable sort (ps2_nodescan.h) against the original selection sort ---- */
static void test_sort(unsigned long long *checks, unsigned long long *fails)
{
	enum { MAXN = 700 };
	static ps2_vsortitem_t a[MAXN], tmp[MAXN], ref[MAXN];
	static unsigned char used[MAXN];
	UINT32 iter;
	for (iter = 0; iter < 60000; iter++)
	{
		const UINT32 n = rnd() % (iter % 50 == 0 ? MAXN : 90);
		const UINT32 srange = 1u + rnd() % (iter % 3 == 0 ? 3 : 40), drange = 1u + rnd() % 4;
		UINT32 i, k;
		for (i = 0; i < n; i++)
		{
			a[i].s = (INT32)(rnd() % srange) * (iter % 7 == 0 ? 65536 : 1) - (iter % 5 == 0 ? 20 : 0);
			a[i].d = (INT32)(rnd() % drange) - 1;
			a[i].p = (void *)(uintptr_t)(i + 1);
			ref[i] = a[i];
		}
		/* original: every round picks the first minimum of (s, d) among the sprites left */
		memset(used, 0, n);
		for (k = 0; k < n; k++)
		{
			INT32 bs = INT32_MAX, bd = INT32_MAX;
			INT32 best = -1;
			for (i = 0; i < n; i++)
			{
				if (used[i])
					continue;
				if (ref[i].s < bs || (ref[i].s == bs && ref[i].d < bd))
				{
					bs = ref[i].s;
					bd = ref[i].d;
					best = (INT32)i;
				}
			}
			used[best] = 1;
			tmp[k] = ref[best];
		}
		PS2_StableSortVis(a, ref, n); /* ref is free as scratch now (tmp holds the expected order) */
		checks[0]++;
		for (i = 0; i < n; i++)
			if (a[i].p != tmp[i].p)
			{
				if (fails[0]++ < 8)
					printf("SW FAIL sort n=%u at %u\n", n, i);
				break;
			}
	}
}

/* ---- PS2-161: rectangle table of the draw nodes: first passing index and growth/reuse of the table ---- */
static void test_nodescan(unsigned long long *checks, unsigned long long *fails)
{
	static ps2_nodescan_t ns;
	INT16 rx1[2000], rx2[2000], ry1[2000], ry2[2000];
	UINT32 iter;
	for (iter = 0; iter < 20000; iter++)
	{
		const INT32 n = (INT32)(rnd() % (iter % 40 == 0 ? 1500 : 120));
		INT32 i, q;
		PS2NS_Reset(&ns);
		for (i = 0; i < n; i++)
		{
			INT32 x1 = (INT32)(rnd() % 400) - 40, x2 = x1 + (INT32)(rnd() % 120), y1 = (INT32)(rnd() % 220) - 10, y2 = y1 + (INT32)(rnd() % 100);
			if (rnd() % 9 == 0) { x1 = -32768; x2 = 32767; y1 = -32768; y2 = 32767; } /* seg nodes / splats: never rejected */
			if (rnd() % 17 == 0) { x1 = PS2NS_NONE_X1; x2 = PS2NS_NONE_X2; y1 = PS2NS_NONE_X1; y2 = PS2NS_NONE_X2; }
			rx1[i] = (INT16)x1; rx2[i] = (INT16)x2; ry1[i] = (INT16)y1; ry2[i] = (INT16)y2;
			PS2NS_Append(&ns, (void *)(uintptr_t)(i + 1), x1, x2, y1, y2);
		}
		for (q = 0; q < 12; q++)
		{
			const INT32 qx1 = (INT32)(rnd() % 360) - 20, qx2 = qx1 + (INT32)(rnd() % 40), qszt = (INT32)(rnd() % 200) - 5, qsz = qszt + (INT32)(rnd() % 60);
			INT32 from = 0, got, want;
			for (;;)
			{
				got = PS2NS_Next(&ns, from, qx1, qx2, qszt, qsz);
				for (want = from; want < n; want++)
					if (!((rx1[want] > qx2) | (rx2[want] < qx1) | (ry1[want] > qsz) | (ry2[want] < qszt)))
						break;
				checks[0]++;
				if (got != want)
				{
					if (fails[0]++ < 8)
						printf("SW FAIL nodescan n=%d from=%d got=%d want=%d\n", n, from, got, want);
					break;
				}
				if (want >= n)
					break;
				from = want + 1;
			}
		}
	}
}

int main(void)
{
	unsigned long long checks[8] = {0}, fails[8] = {0};
	int bad = 0;

	test_edge(&checks[0], &fails[0]);
	printf("SW EdgeXLess checks=%llu failures=%llu\n", checks[0], fails[0]);
	bad += fails[0] != 0;

	test_sort(&checks[1], &fails[1]);
	printf("SW StableSortVis checks=%llu failures=%llu\n", checks[1], fails[1]);
	bad += fails[1] != 0;

	test_nodescan(&checks[2], &fails[2]);
	printf("SW NodeScan checks=%llu failures=%llu\n", checks[2], fails[2]);
	bad += fails[2] != 0;

	printf("SW DONE failures=%d\n", bad);
	return bad ? 1 : 0;
}
