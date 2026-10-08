// PS2-315: kernel-level check of the VU0 kernels (src/ps2/vorbis/ps2_vu0a.h).  The same source runs on the EE (PCSX2: the asm kernels) and on the
// host (the C twins with the FPU rounding set to "toward zero", which is what VU0 does: tools/ps2/ee_tests/vu0arith.c shows that PCSX2's
// vadd/vsub/vmul equal the exactly truncated result).  Both print one FNV-1a hash per kernel over deterministic random inputs; the two
// lists must be identical.  (The EE FPU of PCSX2 rounds add/sub to nearest, so the C twins are not run on the EE for this purpose.)
//   host: gcc -O2 -ffp-contract=off -mno-fma -frounding-math -I src/ps2/vorbis tools/ps2/ee_tests/vu0kern.c -lm
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifndef _EE
#include <fenv.h>
#else
#include <kernel.h>
#endif
#ifdef _EE
#define PS2_VORBIS_VU0 1
#endif
#include "ps2_vu0a.h"
volatile int ps2a_vu0_busy;

static uint32_t seed = 12345;
static uint32_t rnd(void) { seed = seed * 1664525u + 1013904223u; return seed >> 8; }
// a random float with a wide range of exponents, exact integer arithmetic only
static float rf(void)
{
	int e = (int)(rnd() % 24) - 12;
	float m = ((int)(rnd() & 0xffffff) - 8388608) * (1.0f / 8388608.0f);
	return e >= 0 ? m * (float)(1 << e) : m / (float)(1 << -e);
}
static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
	const uint8_t *b = p;
	while (n--) h = (h ^ *b++) * 1099511628211ull;
	return h;
}
#define NEWHASH 14695981039346656037ull

static float H[256] __attribute__((aligned(16))), L[256] __attribute__((aligned(16))), C[512] __attribute__((aligned(16)));

int main(void)
{
	int it, i;
	uint64_t h;
#ifndef _EE
	fesetround(FE_TOWARDZERO);
#endif
#if PS2A_VU0
	ps2a_vu0_saved sv;
	ps2a_vu0_enter(&sv);
#endif
	// K1: fft stage
	h = NEWHASH;
	for (it = 0; it < 200; it++)
	{
		int np = 2 + 2 * (it % 16);
		for (i = 0; i < 4 * np; i++) { H[i] = rf(); L[i] = rf(); }
		for (i = 0; i < 8 * np; i++) C[i] = rf();
#if PS2A_VU0
		ps2a_fft_pairs(H, L, C, np);
#else
		ps2a_fft_pairs_c(H, L, C, np);
#endif
		h = fnv(h, H, 16 * np); h = fnv(h, L, 16 * np);
	}
	printf("EETEST KERN fft_pairs %016llx\n", (unsigned long long)h);
	// K2: rot1 (lower: ascending groups; upper: descending groups)
	h = NEWHASH;
	for (it = 0; it < 200; it++)
	{
		int np = 2 + 2 * (it % 16);
		static float G[512] __attribute__((aligned(16))), O[256] __attribute__((aligned(16)));
		for (i = 0; i < 16 * np; i++) G[i] = rf();
		for (i = 0; i < 8 * np; i++) C[i] = rf();
		memset(O, 0, sizeof O);
#if PS2A_VU0
		ps2a_rot1_lo(G, O, C, np);
#else
		ps2a_rot1_lo_c(G, O, C, np);
#endif
		h = fnv(h, O, 8 * np);
		memset(O, 0, sizeof O);
#if PS2A_VU0
		ps2a_rot1_hi(G + 8 * np - 4, O, C, np);
#else
		ps2a_rot1_hi_c(G + 8 * np - 4, O, C, np);
#endif
		h = fnv(h, O, 8 * np);
	}
	printf("EETEST KERN rot1 %016llx\n", (unsigned long long)h);
	// K3: rot2 passes
	h = NEWHASH;
	for (it = 0; it < 200; it++)
	{
		int ng = 2 + 2 * (it % 8);
		static float Y[512] __attribute__((aligned(16))), Ut[256] __attribute__((aligned(16))), Vn[256] __attribute__((aligned(16))), R1[256] __attribute__((aligned(16))), R2[256] __attribute__((aligned(16))), R3[256] __attribute__((aligned(16)));
		for (i = 0; i < 8 * ng; i++) Y[i] = rf();
		for (i = 0; i < 16 * ng; i++) C[i] = rf();
		memset(Ut, 0, sizeof Ut); memset(Vn, 0, sizeof Vn); memset(R1, 0, sizeof R1); memset(R2, 0, sizeof R2); memset(R3, 0, sizeof R3);
#if PS2A_VU0
		ps2a_rot2_a(Y, Ut, Vn, C, ng);
		ps2a_rev_neg(Ut, R1 + 4 * ng - 4, R2, ng);
		ps2a_rev(Vn, R3 + 4 * ng - 4, ng);
#else
		ps2a_rot2_a_c(Y, Ut, Vn, C, ng);
		ps2a_rev_neg_c(Ut, R1 + 4 * ng - 4, R2, ng);
		ps2a_rev_c(Vn, R3 + 4 * ng - 4, ng);
#endif
		h = fnv(h, Ut, 16 * ng); h = fnv(h, Vn, 16 * ng); h = fnv(h, R1, 16 * ng); h = fnv(h, R2, 16 * ng); h = fnv(h, R3, 16 * ng);
	}
	printf("EETEST KERN rot2 %016llx\n", (unsigned long long)h);
	// K4: bit reverse
	h = NEWHASH;
	for (it = 0; it < 200; it++)
	{
		int iters = 2 + (it % 12);
		static float X[512] __attribute__((aligned(16))), W[512] __attribute__((aligned(16))), CB[12 * 16] __attribute__((aligned(16)));
		int k;
		for (i = 0; i < 256; i++) X[i] = rf();
		for (k = 0; k < iters; k++)
		{
			for (i = 0; i < 8; i++) CB[12 * k + i] = rf();
			for (i = 0; i < 4; i++) ((int *)CB)[12 * k + 8 + i] = 8 * (rnd() % 32);
		}
		memset(W, 0, sizeof W);
#if PS2A_VU0
		ps2a_bitrev(X, W, W + 8 * iters, CB, iters);
#else
		ps2a_bitrev_c(X, W, W + 8 * iters, CB, iters);
#endif
		h = fnv(h, W, 32 * iters);
	}
	printf("EETEST KERN bitrev %016llx\n", (unsigned long long)h);
#if PS2A_VU0
	ps2a_vu0_leave(&sv);
#endif
	printf("EETEST DONE\n");
#ifdef _EE
	SleepThread();
#endif
	return 0;
}
