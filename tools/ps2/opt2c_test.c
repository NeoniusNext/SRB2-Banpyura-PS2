/* Equivalence tests of the exact (bit-identical) optimisations of agent C, session 2026-10-04 (PS2-80..89).
 * One translation unit for the host (MSVC x86/x64, /W3 /WX, PS2_PROFILE) and for the EE (PCSX2, engine flags).
 * Every test compares the ORIGINAL expression (copied verbatim from the pre-change source) with the new one on random
 * and edge inputs and prints "OT <name> checks=<n> failures=<f>". Built by tools/ps2/opt2c_test.py.
 * -DOPT2C_BROKEN makes every new variant slightly wrong: the negative control, all tests must then fail.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "doomtype.h"
#include "m_fixed.h"
#include "ps2/ps2_rdraw.h"

#ifdef OPT2C_BROKEN
#define BROKEN_ADD 1
#else
#define BROKEN_ADD 0
#endif

static UINT32 rs = 0x2545F491u;
static UINT32 rnd(void)
{
	rs ^= rs << 13;
	rs ^= rs >> 17;
	rs ^= rs << 5;
	return rs;
}

static const INT32 edge[] = {0, 1, -1, 2, -2, 255, 256, 65535, 65536, 65537, -65535, -65536, -65537, 0x3FFFFFFF, 0x40000000, 0x40000001,
	-0x3FFFFFFF, -0x40000000, -0x40000001, 0x7FFFFFFF, INT32_MIN, INT32_MIN + 1, 0x20000, 0x7FFF0000, 0x00010001, 0x1234567, -0x1234567};
#define NEDGE ((int)(sizeof edge / sizeof edge[0]))

static INT32 pick(void)
{
	UINT32 r = rnd();
	switch (r & 7)
	{
		case 0: return edge[(r >> 3) % NEDGE];
		case 1: return (INT32)(rnd() & 0xFFFF) - 0x8000;
		case 2: return (INT32)(rnd() & 0xFFFFF) - 0x80000;
		case 3: return (INT32)(rnd() & 0x3FFFFFFF);
		case 4: return (INT32)(rnd() & 0x3FFFFFFF) * -1;
		default: return (INT32)rnd();
	}
}

/* ---- T1: R_FindPlane offset conversion (src/r_plane.c, PS2-82) ---- */
static INT64 ref_offset(fixed_t off, fixed_t scale, fixed_t viewcoord)
{
	float offset_xd = FixedToFloat(off) / FixedToFloat(scale ? scale : 1);
	INT64 offset_x = (INT64)(offset_xd * FRACUNIT);
	offset_x += viewcoord;
	offset_x = ((INT64)offset_x * scale) / FRACUNIT;
	return offset_x;
}

static INT64 new_offset(fixed_t off, fixed_t scale, fixed_t viewcoord)
{
	INT64 offset_x;
	if (off == 0)
		offset_x = 0;
	else
	{
		const float f = (FixedToFloat(off) / FixedToFloat(scale ? scale : 1)) * FRACUNIT;
		offset_x = (f > -2147483648.0f && f < 2147483648.0f) ? (INT64)(INT32)f : (INT64)f;
	}
	offset_x += viewcoord;
	if (scale != FRACUNIT)
		offset_x = ((INT64)offset_x * scale) / FRACUNIT;
#if BROKEN_ADD
	offset_x += (off & 0x10000) ? 1 : 0;
#endif
	return offset_x;
}

static void t_offset(UINT64 n, UINT64 *checks, UINT64 *fail)
{
	UINT64 i;
	for (i = 0; i < n; i++)
	{
		fixed_t off = pick(), sc = (i & 3) ? FRACUNIT : pick(), vc = pick();
		if (i & 8)
			off = 0;
		if (ref_offset(off, sc, vc) != new_offset(off, sc, vc))
			(*fail)++;
		(*checks)++;
	}
}

/* ---- T2: PS2_Fill16 vs the byte loop, every alignment/length, with canaries ---- */
static void t_fill(UINT64 n, UINT64 *checks, UINT64 *fail)
{
	static UINT16 buf[800 + 64];
	static UINT16 ref[800 + 64];
	UINT64 i;
	for (i = 0; i < n; i++)
	{
		size_t start = 1 + rnd() % 8, len = (i < 3000) ? (i % 700) : rnd() % 700, k;
		UINT16 v = (i & 1) ? 0xFFFF : (UINT16)rnd();
		for (k = 0; k < 800 + 64; k++)
			buf[k] = ref[k] = (UINT16)(0xA5A5 ^ k);
		for (k = 0; k < len; k++)
			ref[start + k] = v;
		PS2_Fill16(buf + start, v, len);
#if BROKEN_ADD
		if (len > 9)
			buf[start + 9] ^= 1;
#endif
		(*checks)++;
		if (memcmp(buf, ref, sizeof buf))
			(*fail)++;
	}
}

/* ---- T3: FixedDiv(x, FRACUNIT) shortcut of R_RenderSegLoop (PS2-80) ---- */
static fixed_t shortcut_div(fixed_t a, fixed_t b)
{
	if (b == FRACUNIT && a > -0x40000000 && a < 0x40000000)
#if BROKEN_ADD
		return a + 1;
#else
		return a;
#endif
	return FixedDiv(a, b);
}

static void t_unitdiv(UINT64 n, UINT64 *checks, UINT64 *fail)
{
	UINT64 i;
	for (i = 0; i < n; i++)
	{
		fixed_t a = pick(), b = (i & 1) ? FRACUNIT : pick();
		if (i < 200000)
			a = (INT32)(0x3FFFFF00u + (UINT32)(i % 512)) * ((i & 2) ? -1 : 1);
		(*checks)++;
		if (FixedDiv(a, b) != shortcut_div(a, b))
			(*fail)++;
	}
}

typedef void (*test_fn)(UINT64, UINT64 *, UINT64 *);
static const struct { const char *name; test_fn fn; UINT64 n; } tests[] = {
	{"findplane-offset", t_offset, 40000000ull},
	{"fill16", t_fill, 60000},
	{"unit-div", t_unitdiv, 40000000ull},
};

int main(int argc, char **argv)
{
	unsigned t;
	UINT64 total_fail = 0;
	double scale = 1.0;
	if (argc > 1)
		scale = atof(argv[1]);
	for (t = 0; t < sizeof tests / sizeof tests[0]; t++)
	{
		UINT64 checks = 0, fail = 0;
		UINT64 n = (UINT64)(tests[t].n * scale);
		if (n < 1000)
			n = 1000;
		tests[t].fn(n, &checks, &fail);
		printf("OT %s checks=%llu failures=%llu\n", tests[t].name, (unsigned long long)checks, (unsigned long long)fail);
		total_fail += fail;
	}
	printf("OT %s failures=%llu\n", total_fail ? "FAIL" : "PASS", (unsigned long long)total_fail);
	printf("OT DONE\n");
	return total_fail ? 1 : 0;
}
