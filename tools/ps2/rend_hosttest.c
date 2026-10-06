/* Host test of the bit-identical renderer helpers in src/ps2/ps2_rdraw.h against the original expressions.
 * Built and run by tools/ps2/rend_hosttest.py (MSVC). Prints "RT <name> checks=<n> failures=<n>" and "RT DONE".
 * -DRT_BROKEN builds a negative control with a deliberately wrong reference (must fail). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m_fixed.h"
#include "tables.h"
#include "ps2/ps2_rdraw.h"

static UINT32 st = 88172645u;
static UINT32 rnd(void) { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; }
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
		case 6: return 511 + (v % 5);
		default: { static const UINT32 e[] = {0, 1, 2, 255, 256, 257, 511, 512, 513, 0x1FFFFFFF, 0x20000000, 0x20000001, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu}; return e[v % 15]; }
	}
}

/* original SlopeDivEx of src/tables.c */
static UINT64 ref_SlopeDivEx(unsigned int num, unsigned int den)
{
	UINT64 ans;
	if (den < 512)
		return SLOPERANGE;
	ans = ((UINT64)num << 3) / (den >> 8);
#ifdef RT_BROKEN
	ans += (num & 0x40000000u) != 0;
#endif
	return ans <= SLOPERANGE ? ans : SLOPERANGE;
}

/* original FixedDiv of src/m_fixed.h (64-bit) */
static fixed_t ref_FixedDiv(fixed_t a, fixed_t b)
{
	if (((ufixed_t)abs(a) >> (FRACBITS - 2)) >= (ufixed_t)abs(b))
		return (a ^ b) < 0 ? INT32_MIN : INT32_MAX;
	return (fixed_t)(((INT64)a * FRACUNIT) / b);
}

int main(void)
{
	unsigned long long checks, fails;
	UINT32 i;

	checks = fails = 0;
	for (i = 0; i < 200000000u; i++)
	{
		const UINT32 num = rnd_mixed(), den = rnd_mixed();
		checks++;
		if (PS2_SlopeDivEx(num, den) != ref_SlopeDivEx(num, den))
			if (fails++ < 8) printf("RT FAIL slopediv %u %u\n", num, den);
	}
	/* all small/boundary combinations */
	for (i = 0; i < 70000; i++)
	{
		UINT32 j;
		for (j = 0; j < 4096; j++)
		{
			const UINT32 num = (i < 35000) ? i : 0x1FFF0000u + (i - 35000) * 7u;
			const UINT32 den = (j < 2048) ? j * 3u : 0x7FFF000u + j;
			checks++;
			if (PS2_SlopeDivEx(num, den) != ref_SlopeDivEx(num, den))
				if (fails++ < 8) printf("RT FAIL slopediv grid %u %u\n", num, den);
		}
	}
	printf("RT SlopeDivEx checks=%llu failures=%llu\n", checks, fails);
	{
		unsigned long long c2 = 0, f2 = 0;
		for (i = 0; i < 100000000u; i++)
		{
			const fixed_t a = (fixed_t)rnd_mixed();
			fixed_t b = (i & 1) ? FRACUNIT : (fixed_t)rnd_mixed();
			c2++;
			if (PS2_FixedDivByUnit(a, b) != ref_FixedDiv(a, b) && b != 0)
				if (f2++ < 8) printf("RT FAIL divbyunit %d %d\n", a, b);
		}
		for (i = 0; i < 70000; i++)
		{
			const fixed_t a = (fixed_t)(0x3FFFFFF0u + i), a2 = (fixed_t)(0xC0000000u - 16 + i);
			c2 += 2;
			if (PS2_FixedDivByUnit(a, FRACUNIT) != ref_FixedDiv(a, FRACUNIT)) f2++;
			if (PS2_FixedDivByUnit(a2, FRACUNIT) != ref_FixedDiv(a2, FRACUNIT)) f2++;
		}
		printf("RT FixedDivByUnit checks=%llu failures=%llu\n", c2, f2);
		fails += f2;
		checks += c2;
	}
	printf("RT DONE total_checks=%llu failures=%llu\n", checks, fails);
	return fails ? 1 : 0;
}
