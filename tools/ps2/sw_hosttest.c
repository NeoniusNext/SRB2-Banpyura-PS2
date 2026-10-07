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
#include "r_slopeq.h"

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

/* ---- PS2-161: rectangle table of the draw nodes: group scan + lane popping against the reference, growth/reuse of the table ---- */
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
			INT32 grp = 0, want = 0;
			const INT32 groups = PS2NS_GROUPS(&ns);
			for (;;)
			{
				UINT64 lanes = PS2NS_NextGroup(&ns, &grp, groups, qx1, qx2, qszt, qsz);
				if (grp >= groups)
					break;
				/* every passing lane of the group, lowest first, equals the reference scan of the nodes in creation order */
				do
				{
					const INT32 idx = grp * 8 + PS2NS_PopLane(&lanes);
					for (; want < n; want++)
						if (!((rx1[want] > qx2) | (rx2[want] < qx1) | (ry1[want] > qsz) | (ry2[want] < qszt)))
							break;
					checks[0]++;
					if (idx != want)
					{
						if (fails[0]++ < 8)
							printf("SW FAIL nodescan n=%d got=%d want=%d\n", n, idx, want);
						goto next_query;
					}
					want++;
				} while (lanes);
				grp++;
			}
			/* nothing passes after the last one */
			for (; want < n; want++)
				if (!((rx1[want] > qx2) | (rx2[want] < qx1) | (ry1[want] > qsz) | (ry2[want] < qszt)))
				{
					if (fails[0]++ < 8)
						printf("SW FAIL nodescan n=%d missed %d\n", n, want);
					break;
				}
next_query:;
		}
	}
}


/* ---- PS2-172: RQ_Div128 (src/r_slopeq.h) against the plain 64-iteration restoring division ---- */
static UINT64 ref_Div128(UINT64 hi, UINT64 lo, UINT64 d)
{
	UINT64 q = 0, rem;
	int i;
	if (hi >= d)
		return ~(UINT64)0;
	if (hi == 0)
		return lo / d;
	rem = hi;
	for (i = 63; i >= 0; i--)
	{
		const UINT64 top = rem >> 63;
		rem = (rem << 1) | ((lo >> i) & 1);
		if (top || rem >= d)
		{
			rem -= d;
			q |= (UINT64)1 << i;
		}
	}
	return q;
}

static UINT64 rnd64(void) { return ((UINT64)rnd() << 32) ^ rnd() ^ ((UINT64)rnd() << 11); }
static UINT64 rnd_bits(void) /* random value of a random bit length 0..64, sometimes all ones / a power of two / one off */
{
	const UINT32 r = rnd(), n = (r >> 4) % 65;
	UINT64 v = n ? (rnd64() >> (64 - n)) : 0;
	switch (r & 7)
	{
		case 0: v = n ? ((UINT64)1 << (n - 1)) : 0; break;
		case 1: v = n ? (((UINT64)1 << (n - 1)) - 1) : 0; break;
		case 2: v = n ? (((UINT64)1 << (n - 1)) + 1) : 1; break;
		default: break;
	}
	return v;
}

static void test_div128(unsigned long long *checks, unsigned long long *fails)
{
	UINT32 i;
	for (i = 0; i < 40000000u; i++)
	{
		UINT64 d = rnd_bits() & 0x8000000000000000ull ? ((UINT64)1 << 63) : (rnd_bits() >> 1), hi, lo;
		if (d == 0)
			d = 1;
		switch (rnd() & 3)
		{
			case 0: hi = rnd_bits() % d; break; /* any hi below d */
			case 1: hi = (d - 1) - (rnd_bits() % (d < 1024 ? d : 1024)); break; /* just below d */
			case 2: hi = rnd_bits() >> (rnd() & 63); if (hi >= d) hi %= d; break; /* small hi: the mapping case (quotient of about 33 bits) */
			default: hi = (rnd() & 1) ? 0 : (d >> (1 + (rnd() & 31))); break;
		}
		lo = (rnd() & 3) ? rnd_bits() : ((rnd() & 1) ? ~(UINT64)0 : 0);
		if ((rnd() & 15) == 0)
			hi = d + (rnd_bits() & 3); /* does not fit: saturates */
		(*checks)++;
		if (RQ_Div128(hi, lo, d) != ref_Div128(hi, lo, d))
		{
			if (++*fails <= 5)
				printf("SW Div128 mismatch hi=%llx lo=%llx d=%llx: %llx vs %llx\n", (unsigned long long)hi, (unsigned long long)lo, (unsigned long long)d,
					(unsigned long long)RQ_Div128(hi, lo, d), (unsigned long long)ref_Div128(hi, lo, d));
		}
	}
}

/* ---- PS2-174: PS2_FixedToDoubleBits (src/ps2/ps2_rdraw.h) against x / (double)FRACUNIT ---- */
static void test_ftd(unsigned long long *checks, unsigned long long *fails)
{
	UINT32 i;
	static const INT32 edge[] = {0, 1, -1, 2, -2, 3, 65535, 65536, 65537, -65535, -65536, -65537, 0x7FFFFFFF, (INT32)0x80000000, (INT32)0x80000001, 0x40000000, -0x40000000, 0x7FFF0000, (INT32)0x8000FFFF};
	for (i = 0; i < 120000000u + sizeof edge / sizeof edge[0]; i++)
	{
		INT32 x = i < sizeof edge / sizeof edge[0] ? edge[i] : (INT32)rnd_mixed();
		double ref = x / (double)FRACUNIT, got;
		UINT64 bits = PS2_FixedToDoubleBits(x), rb;
#ifdef SW_BROKEN
		if (bits) bits++;
#endif
		memcpy(&got, &bits, sizeof got);
		memcpy(&rb, &ref, sizeof rb);
		(*checks)++;
		if (rb != bits && ++*fails <= 5)
			printf("SW FixedToDouble mismatch x=%d: %llx vs %llx\n", (int)x, (unsigned long long)bits, (unsigned long long)rb);
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

	test_div128(&checks[3], &fails[3]);
	printf("SW Div128 checks=%llu failures=%llu\n", checks[3], fails[3]);
	bad += fails[3] != 0;

	test_ftd(&checks[4], &fails[4]);
	printf("SW FixedToDouble checks=%llu failures=%llu\n", checks[4], fails[4]);
	bad += fails[4] != 0;

	printf("SW DONE failures=%d\n", bad);
	return bad ? 1 : 0;
}
