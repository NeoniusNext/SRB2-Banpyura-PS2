/* Bit-exactness of the PS2 FixedMul/FixedDiv/FixedDiv2/FixedInt/FixedSqrt/FixedHypot (src/m_fixed.h, m_fixed.c with PS2_PROFILE)
 * against the original implementation (HEAD sources compiled without PS2_PROFILE, symbols renamed ref_*).
 * Built and run by tools/ps2/math_hosttest.py; exit code 0 only if there is no difference.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "m_fixed.h"

/* original implementation, compiled from the HEAD tree (ref_wrap.c / ref_m_fixed.c) */
fixed_t ref_FixedMul(fixed_t a, fixed_t b);
fixed_t ref_FixedDiv(fixed_t a, fixed_t b);
fixed_t ref_FixedDiv2(fixed_t a, fixed_t b);
fixed_t ref_FixedInt(fixed_t a);
fixed_t ref_FixedSqrt(fixed_t x);
fixed_t ref_FixedHypot(fixed_t x, fixed_t y);
fixed_t ref_FV3_Magnitude(const vector3_t *v);
fixed_t ref_FV3_NormalizeEx(const vector3_t *v, vector3_t *o);
vector3_t *ref_FV3_DivideEx(const vector3_t *v, fixed_t d, vector3_t *o);
fixed_t ref_FV2_Distance(const vector2_t *a, const vector2_t *b);
fixed_t ref_FV2_NormalizeEx(const vector2_t *v, vector2_t *o);
vector2_t *ref_FV2_DivideEx(const vector2_t *v, fixed_t d, vector2_t *o);
fixed_t ref_FV3_PlaneIntersection(const vector3_t *a, const vector3_t *b, const vector3_t *c, const vector3_t *d);

static uint64_t rs[4];
static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
static uint64_t rnd(void)
{
	uint64_t r = rotl(rs[1] * 5, 7) * 9, t = rs[1] << 17;
	rs[2] ^= rs[0]; rs[3] ^= rs[1]; rs[1] ^= rs[2]; rs[0] ^= rs[3]; rs[2] ^= t; rs[3] = rotl(rs[3], 45);
	return r;
}
static void seed(uint64_t s)
{
	for (int i = 0; i < 4; i++) { s += 0x9E3779B97F4A7C15ull; uint64_t z = s; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; rs[i] = z ^ (z >> 31); }
}

/* value classes: uniform, random bit-length, near a power of two, small, extreme */
static fixed_t gen(void)
{
	uint64_t r = rnd();
	switch (r & 7)
	{
		case 0: case 1: return (fixed_t)(r >> 32);
		case 2: case 3: { int bits = 1 + (int)((r >> 8) % 32); uint32_t m = (uint32_t)(rnd() >> 16); if (bits < 32) m &= (1u << bits) - 1; return (fixed_t)((r & 0x100000000ull) ? 0u - m : m); }
		case 4: { int k = (int)((r >> 8) % 32); int d = (int)((r >> 16) % 7) - 3; uint32_t m = (1u << k) + (uint32_t)d; return (fixed_t)((r & 0x100000000ull) ? 0u - m : m); }
		case 5: return (fixed_t)((int)((r >> 8) % 4097) - 2048);
		case 6: return (fixed_t)(((int)((r >> 8) % 2049) - 1024) * FRACUNIT + (int)((r >> 24) % 5) - 2);
		default: { static const fixed_t e[] = { INT32_MIN, INT32_MIN + 1, INT32_MAX, INT32_MAX - 1, 0, 1, -1, FRACUNIT, -FRACUNIT, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x10001, 0x3FFFFFFF, 0x40000000, -0x40000000 }; return e[(r >> 8) % (sizeof e / sizeof e[0])]; }
	}
}

static uint64_t fails, checks;
#define FAILMAX 12
#define CHECK(cond, ...) do { checks++; if (!(cond)) { if (fails++ < FAILMAX) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static void pair_check(fixed_t a, fixed_t b)
{
	/* Defined saturation oracle, including INT_MIN and zero: never evaluate abs(INT_MIN). */
	const uint32_t ua = a < 0 ? 0u - (uint32_t)a : (uint32_t)a;
	const uint32_t ub = b < 0 ? 0u - (uint32_t)b : (uint32_t)b;
	const fixed_t div = (ua >> (FRACBITS - 2)) >= ub
		? ((a ^ b) < 0 ? INT32_MIN : INT32_MAX)
		: (fixed_t)(((int64_t)a * FRACUNIT) / b);
	CHECK(FixedDiv(a, b) == div, "FixedDiv defined saturation(%d,%d)", a, b);
	CHECK(FixedMul(a, b) == ref_FixedMul(a, b), "FixedMul(%d,%d) new=%d ref=%d", a, b, FixedMul(a, b), ref_FixedMul(a, b));
	CHECK(FixedDiv(a, b) == ref_FixedDiv(a, b), "FixedDiv(%d,%d) new=%d ref=%d", a, b, FixedDiv(a, b), ref_FixedDiv(a, b));
	if (b != 0)
		CHECK(FixedDiv2(a, b) == ref_FixedDiv2(a, b), "FixedDiv2(%d,%d) new=%d ref=%d", a, b, FixedDiv2(a, b), ref_FixedDiv2(a, b));
}

typedef struct { uint64_t n_mul_div, n_sqrt, n_hyp, n_vec; } counts_t;

int main(int argc, char **argv)
{
	uint64_t npairs = 100000000ull;
	uint64_t nsqrt = 100000000ull;
	int exhaustive_sqrt = 0;
	for (int i = 1; i < argc; i++)
	{
		if (!strncmp(argv[i], "--pairs=", 8)) npairs = strtoull(argv[i] + 8, NULL, 10);
		else if (!strncmp(argv[i], "--sqrt=", 7)) nsqrt = strtoull(argv[i] + 7, NULL, 10);
		else if (!strcmp(argv[i], "--exhaustive-sqrt")) exhaustive_sqrt = 1;
	}
	seed(0x5352423250533221ull);

	/* 1. edge matrix: all of {0,+-1,+-2, +-(2^k + d)} squared */
	{
		static fixed_t e[1200]; int ne = 0;
		e[ne++] = 0; e[ne++] = INT32_MIN; e[ne++] = INT32_MAX; e[ne++] = INT32_MIN + 1; e[ne++] = INT32_MAX - 1;
		for (int k = 0; k < 32; k++)
			for (int d = -3; d <= 3; d++)
			{
				uint32_t m = (1u << k) + (uint32_t)d;
				e[ne++] = (fixed_t)m; e[ne++] = (fixed_t)(0u - m);
			}
		for (int i = 0; i < ne; i++)
			for (int j = 0; j < ne; j++)
				pair_check(e[i], e[j]);
		printf("edge matrix: %d x %d pairs\n", ne, ne);
	}
	/* 2. exhaustive small: |a| < 2^11 x all |b| < 2^11, both signs */
	for (int a = -2048; a <= 2048; a++)
		for (int b = -2048; b <= 2048; b++)
			pair_check(a, b);
	printf("exhaustive |a|,|b|<=2048: %d pairs\n", 4097 * 4097);
/* 3. divisor sweep: every divisor 1..2^20 with 8 numerators, both divisor signs */
	for (int b = 1; b <= (1 << 20); b++)
		for (int k = 0; k < 8; k++)
		{
			fixed_t a = gen();
			pair_check(a, b);
			pair_check(a, -b);
		}
	/* 4. random pairs */
	for (uint64_t i = 0; i < npairs; i++)
		pair_check(gen(), gen());
	printf("random pairs: %llu (each: FixedMul, FixedDiv, FixedDiv2)\n", (unsigned long long)npairs);
	for (int i = 0; i < 1 << 20; i++)
	{
		fixed_t a = gen();
		CHECK(FixedInt(a) == ref_FixedInt(a), "FixedInt(%d)", a);
	}

	/* 5. FixedSqrt: structured + random + optional exhaustive */
	{
		uint64_t base = checks;
		for (uint32_t x = 0; x < (1u << 26); x++)
			CHECK(FixedSqrt((fixed_t)x) == ref_FixedSqrt((fixed_t)x), "FixedSqrt(%d)", (int)x);
		for (int k = 0; k < 32; k++)
			for (int d = -4096; d <= 4096; d++)
			{
				uint32_t x = (1u << k) + (uint32_t)d;
				CHECK(FixedSqrt((fixed_t)x) == ref_FixedSqrt((fixed_t)x), "FixedSqrt(%d)", (int)x);
			}
		for (uint64_t i = 0; i < nsqrt; i++)
		{
			fixed_t x = gen();
			CHECK(FixedSqrt(x) == ref_FixedSqrt(x), "FixedSqrt(%d)", x);
		}
		if (exhaustive_sqrt)
			for (uint64_t x = 0; x < (1ull << 32); x++)
				CHECK(FixedSqrt((fixed_t)(uint32_t)x) == ref_FixedSqrt((fixed_t)(uint32_t)x), "FixedSqrt(%u)", (unsigned)x);
		printf("FixedSqrt checks: %llu%s\n", (unsigned long long)(checks - base), exhaustive_sqrt ? " (incl. all 2^32 inputs)" : "");
		/* the integer fix-up must converge from any nearby estimate (R5900 FPU truncation, errors of several units) */
		for (uint64_t i = 0; i < 2000000; i++)
		{
			uint32_t x = (uint32_t)gen();
			uint32_t exact = (uint32_t)ref_FixedSqrt((fixed_t)x);
			int off = (int)(rnd() % 129) - 64;
			uint32_t est = (uint32_t)((int)exact + off);
			if ((int)exact + off < 0) est = 0;
			CHECK(PS2_FixedSqrtFix(x, est) == exact, "SqrtFix(%u, est %u) exact %u", x, est, exact);
		}
	}
	/* 6. FixedHypot and the vector functions */
	{
		uint64_t base = checks;
		for (uint64_t i = 0; i < npairs / 4; i++)
		{
			fixed_t x = gen(), y = gen();
			CHECK(FixedHypot(x, y) == ref_FixedHypot(x, y), "FixedHypot(%d,%d)", x, y);
		}
		for (uint64_t i = 0; i < npairs / 20; i++)
		{
			vector3_t a = { gen(), gen(), gen() }, b = { gen(), gen(), gen() }, o1, o2;
			vector3_t c = { gen(), gen(), gen() }, d = { gen(), gen(), gen() };
			fixed_t s = gen();
			CHECK(FV3_Magnitude(&a) == ref_FV3_Magnitude(&a), "FV3_Magnitude");
			FV3_NormalizeEx(&a, &o1); ref_FV3_NormalizeEx(&a, &o2);
			CHECK(!memcmp(&o1, &o2, sizeof o1), "FV3_NormalizeEx");
			FV3_DivideEx(&a, s, &o1); ref_FV3_DivideEx(&a, s, &o2);
			CHECK(!memcmp(&o1, &o2, sizeof o1), "FV3_DivideEx");
			CHECK(FV3_PlaneIntersection(&a, &b, &c, &d) == ref_FV3_PlaneIntersection(&a, &b, &c, &d), "FV3_PlaneIntersection");
			vector2_t p = { a.x, a.y }, q = { b.x, b.y }, r1, r2;
			CHECK(FV2_Distance(&p, &q) == ref_FV2_Distance(&p, &q), "FV2_Distance");
			FV2_NormalizeEx(&p, &r1); ref_FV2_NormalizeEx(&p, &r2);
			CHECK(!memcmp(&r1, &r2, sizeof r1), "FV2_NormalizeEx");
			FV2_DivideEx(&p, s, &r1); ref_FV2_DivideEx(&p, s, &r2);
			CHECK(!memcmp(&r1, &r2, sizeof r1), "FV2_DivideEx");
		}
		printf("FixedHypot/FV2/FV3 checks: %llu\n", (unsigned long long)(checks - base));
	}
	printf("TOTAL checks=%llu failures=%llu\n", (unsigned long long)checks, (unsigned long long)fails);
	return fails ? 1 : 0;
}
