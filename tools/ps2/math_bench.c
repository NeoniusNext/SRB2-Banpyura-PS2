/* EE micro-benchmark (COP0 Count cycles) of the fixed-point math: original (HEAD, 64-bit libgcc paths) vs PS2_PROFILE.
 * Built by tools/ps2/build_math_bench.py, run through tools/ps2/run_pcsx2.py; prints "MB ..." lines, "MB DONE" at the end.
 * Every variant runs the same operand arrays through a non-inlined wrapper; the empty wrapper (ovh) is the call/loop overhead.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "m_fixed.h"

fixed_t ref_FixedMul(fixed_t a, fixed_t b);
fixed_t ref_FixedDiv(fixed_t a, fixed_t b);
fixed_t ref_FixedDiv2(fixed_t a, fixed_t b);
fixed_t ref_FixedSqrt(fixed_t x);
fixed_t ref_FixedHypot(fixed_t x, fixed_t y);

#define N 2048
#define REPS 32
static fixed_t A[N] __attribute__((aligned(64))), B[N] __attribute__((aligned(64)));
static fixed_t SQ[N] __attribute__((aligned(64)));

static inline unsigned count(void) { unsigned v; __asm__ volatile("mfc0 %0,$9" : "=r"(v)); return v; }

static unsigned lcg_state = 12345;
static unsigned lcg(void) { lcg_state = lcg_state * 1664525u + 1013904223u; return lcg_state; }

__attribute__((noinline)) static fixed_t new_mul(fixed_t a, fixed_t b) { return FixedMul(a, b); }
__attribute__((noinline)) static fixed_t new_div(fixed_t a, fixed_t b) { return FixedDiv(a, b); }
__attribute__((noinline)) static fixed_t new_div2(fixed_t a, fixed_t b) { return FixedDiv2(a, b); }
__attribute__((noinline)) static fixed_t ovh2(fixed_t a, fixed_t b) { return a ^ b; }
__attribute__((noinline)) static fixed_t ovh1(fixed_t a) { return a; }
__attribute__((noinline)) static fixed_t ref_hyp(fixed_t a, fixed_t b) { return ref_FixedHypot(a, b); }
__attribute__((noinline)) static fixed_t new_hyp(fixed_t a, fixed_t b) { return FixedHypot(a, b); }
__attribute__((noinline)) static fixed_t new_sqrt(fixed_t a) { return FixedSqrt(a); }

typedef fixed_t (*f2)(fixed_t, fixed_t);
typedef fixed_t (*f1)(fixed_t);

static unsigned sink;

static double time2(f2 f, int skip_zero_b)
{
	unsigned best = 0xFFFFFFFFu;
	for (int r = 0; r < REPS; r++)
	{
		unsigned s = 0, t0 = count();
		for (int i = 0; i < N; i++)
		{
			fixed_t b = B[i];
			if (skip_zero_b && b == 0)
				b = 1;
			s += (unsigned)f(A[i], b);
		}
		unsigned dt = count() - t0;
		sink += s;
		if (dt < best)
			best = dt;
	}
	return (double)best / N;
}

static double time1(f1 f)
{
	unsigned best = 0xFFFFFFFFu;
	for (int r = 0; r < REPS; r++)
	{
		unsigned s = 0, t0 = count();
		for (int i = 0; i < N; i++)
			s += (unsigned)f(SQ[i]);
		unsigned dt = count() - t0;
		sink += s;
		if (dt < best)
			best = dt;
	}
	return (double)best / N;
}

static void fill(int kind)
{
	for (int i = 0; i < N; i++)
	{
		unsigned r1 = lcg(), r2 = lcg();
		switch (kind)
		{
			case 0: /* game-like: a up to +-1024 units, b 1..1024 units (quotient within 2^30) */
				A[i] = (fixed_t)((int)(r1 % (2048 * FRACUNIT)) - 1024 * FRACUNIT);
				B[i] = (fixed_t)(FRACUNIT + (int)(r2 % (1024 * FRACUNIT)));
				if (r2 & 0x80000000u) B[i] = -B[i];
				break;
			case 1: /* sub-unit divisors, 16-bit magnitudes */
				A[i] = (fixed_t)((int)(r1 % (2 * FRACUNIT)) - FRACUNIT);
				B[i] = (fixed_t)(1 + (int)(r2 % 0xFFFE));
				break;
			default: /* uniform 32-bit */
				A[i] = (fixed_t)r1;
				B[i] = (fixed_t)r2 | 1;
				break;
		}
		SQ[i] = (fixed_t)(r1 >> (r2 & 15)) & 0x7FFFFFFF;
	}
}

/* EE-side equivalence of the code the engine actually runs (inline asm mult/plzcw/sqrt.s) with the original compiled for the EE */
static unsigned gen_value(void)
{
	unsigned r = lcg();
	switch (r & 7)
	{
		case 0: case 1: return lcg();
		case 2: case 3: { int bits = 1 + (int)((r >> 8) % 32); unsigned m = lcg(); if (bits < 32) m &= (1u << bits) - 1; return (r & 0x100) ? 0u - m : m; }
		case 4: { unsigned m = (1u << ((r >> 8) % 32)) + (unsigned)((int)((r >> 16) % 7) - 3); return (r & 0x100) ? 0u - m : m; }
		case 5: return (unsigned)((int)((r >> 8) % 4097) - 2048);
		case 6: return (unsigned)(((int)((r >> 8) % 2049) - 1024) * FRACUNIT + (int)((r >> 24) % 5) - 2);
		default: { static const unsigned e[] = { 0x80000000u, 0x80000001u, 0x7FFFFFFFu, 0x7FFFFFFEu, 0, 1, 0xFFFFFFFFu, 0x10000u, 0xFFFF0000u, 0x7FFFu, 0x8000u, 0xFFFFu, 0x10001u, 0x3FFFFFFFu, 0x40000000u, 0xC0000000u }; return e[(r >> 8) % 16]; }
	}
}

static int selftest(unsigned npairs)
{
	unsigned fails = 0, checks = 0;
	static const fixed_t edges[] = { INT32_MIN, INT32_MIN + 1, INT32_MAX, INT32_MAX - 1,
		0, 1, -1, 2, -2, FRACUNIT, -FRACUNIT, 0xFFFF, 0x10001, 0x3FFFFFFF, 0x40000000 };
	for (unsigned i = 0; i < sizeof edges / sizeof edges[0]; i++)
		for (unsigned j = 0; j < sizeof edges / sizeof edges[0]; j++)
		{
			const fixed_t a = edges[i], b = edges[j];
			const UINT32 ua = a < 0 ? 0u - (UINT32)a : (UINT32)a;
			const UINT32 ub = b < 0 ? 0u - (UINT32)b : (UINT32)b;
			const fixed_t d = (ua >> (FRACBITS - 2)) >= ub
				? ((a ^ b) < 0 ? INT32_MIN : INT32_MAX) : ref_FixedDiv2(a, b);
			checks += 2;
			if (new_mul(a, b) != ref_FixedMul(a, b)) { if (fails++ < 8) printf("MB FAIL edge mul %d %d\n", a, b); }
			if (new_div(a, b) != d) { if (fails++ < 8) printf("MB FAIL edge div %d %d\n", a, b); }
		}
	for (unsigned i = 0; i < npairs; i++)
	{
		fixed_t a = (fixed_t)gen_value(), b = (fixed_t)gen_value();
		checks += 2 + (b != 0);
		if (new_mul(a, b) != ref_FixedMul(a, b)) { if (fails++ < 8) printf("MB FAIL mul %d %d\n", a, b); }
		if (new_div(a, b) != ref_FixedDiv(a, b)) { if (fails++ < 8) printf("MB FAIL div %d %d\n", a, b); }
		if (b != 0 && new_div2(a, b) != ref_FixedDiv2(a, b)) { if (fails++ < 8) printf("MB FAIL div2 %d %d\n", a, b); }
		if (i < npairs / 4)
		{
			checks += 2;
			if (new_sqrt(a) != ref_FixedSqrt(a)) { if (fails++ < 8) printf("MB FAIL sqrt %d\n", a); }
			if (new_hyp(a, b) != ref_hyp(a, b)) { if (fails++ < 8) printf("MB FAIL hypot %d %d\n", a, b); }
		}
	}
	printf("MB selftest pairs=%u checks=%u failures=%u\n", npairs, checks, fails);
	return fails != 0;
}

int main(void)
{
	static const char *kinds[] = { "game", "subunit", "uniform" };
	printf("MB start N=%d reps=%d\n", N, REPS);
	if (selftest(1000000))
		printf("MB SELFTEST FAILED\n");
	for (int k = 0; k < 3; k++)
	{
		fill(k);
		double o = time2(ovh2, 0);
		double m0 = time2(ref_FixedMul, 0) - o, m1 = time2(new_mul, 0) - o;
		double d0 = time2(ref_FixedDiv, 0) - o, d1 = time2(new_div, 0) - o;
		double e0 = time2(ref_FixedDiv2, 1) - o, e1 = time2(new_div2, 1) - o;
		double h0 = time2(ref_hyp, 0) - o, h1 = time2(new_hyp, 0) - o;
		printf("MB %-8s ovh=%.1f  FixedMul %.1f -> %.1f  FixedDiv %.1f -> %.1f  FixedDiv2 %.1f -> %.1f  FixedHypot %.1f -> %.1f (cycles/call, net of overhead)\n",
			kinds[k], o, m0, m1, d0, d1, e0, e1, h0, h1);
	}
	{
		double o = time1(ovh1);
		double s0 = time1(ref_FixedSqrt) - o, s1 = time1(new_sqrt) - o;
		printf("MB sqrt     FixedSqrt %.1f -> %.1f\n", s0, s1);
	}
	printf("MB sink=%u\n", sink);
	printf("MB DONE\n");
	for (;;) {}
	return 0;
}
