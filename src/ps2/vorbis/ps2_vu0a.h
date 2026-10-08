// PS2-315: VU0 (COP2, macro mode) kernels of the Ogg Vorbis decoder, shared by mdct.c / block.c / the PCM conversion.
//
// Everything here computes the same single-precision operations in the same order as the scalar code it replaces (add, sub and mul are
// separate instructions, never a multiply-add, never a different association), so the results are bit-identical to the scalar EE FPU
// code on the emulator; the C twin of every kernel (ps2a_*_c) is the fallback when VU0 is busy / not available and the model the host
// test (tools/ps2/ee_tests/mdct_host.c) compares with the libvorbis original.
//
// VU0 state and threads: the EE kernel does NOT save COP2 state at a thread switch (tools/ps2/ee_tests/vu0ctx.c: after the first
// switch the other thread's vf1, ACC, I and Q were visible in the main thread). The hardware renderer keeps its constants in vf1..vf13
// and uses vf14..vf18, ACC, Q and the clip flag on the game thread. A kernel section therefore (a) saves vf1..vf18 on entry and restores
// them on exit (ps2a_vu0_enter / ps2a_vu0_leave), (b) uses no ACC form (vmula/vmadd/vmsub), no Q (vdiv), no clip (vclip) and no I
// instruction, so those registers stay the renderer's, (c) sets a busy flag so that two audio users never overlap: the second one takes the C
// path (identical result). The decoder thread (higher priority than the game thread) is never preempted by the game thread, and a
// section never blocks, so the flag needs no atomic update; the mixer thread does not use VU0.
#ifndef PS2_VU0A_H
#define PS2_VU0A_H

#include <stdint.h>
#include <stddef.h>

#if defined(_EE) && defined(PS2_VORBIS_VU0) && !defined(PS2A_FORCE_C)
#define PS2A_VU0 1
#else
#define PS2A_VU0 0
#endif

typedef struct { float r[18][4]; } ps2a_vu0_saved __attribute__((aligned(16)));

#if PS2A_VU0
extern volatile int ps2a_vu0_busy;

static inline int ps2a_vu0_enter(ps2a_vu0_saved *s)
{
	if (ps2a_vu0_busy) return 0;
	ps2a_vu0_busy = 1;
	__asm__ volatile(
		"sqc2 $vf1, 0x00(%0)\n sqc2 $vf2, 0x10(%0)\n sqc2 $vf3, 0x20(%0)\n sqc2 $vf4, 0x30(%0)\n"
		"sqc2 $vf5, 0x40(%0)\n sqc2 $vf6, 0x50(%0)\n sqc2 $vf7, 0x60(%0)\n sqc2 $vf8, 0x70(%0)\n"
		"sqc2 $vf9, 0x80(%0)\n sqc2 $vf10, 0x90(%0)\n sqc2 $vf11, 0xa0(%0)\n sqc2 $vf12, 0xb0(%0)\n"
		"sqc2 $vf13, 0xc0(%0)\n sqc2 $vf14, 0xd0(%0)\n sqc2 $vf15, 0xe0(%0)\n sqc2 $vf16, 0xf0(%0)\n"
		"sqc2 $vf17, 0x100(%0)\n sqc2 $vf18, 0x110(%0)\n"
		: : "r"(s) : "memory");
	return 1;
}

static inline void ps2a_vu0_leave(const ps2a_vu0_saved *s)
{
	__asm__ volatile(
		"lqc2 $vf1, 0x00(%0)\n lqc2 $vf2, 0x10(%0)\n lqc2 $vf3, 0x20(%0)\n lqc2 $vf4, 0x30(%0)\n"
		"lqc2 $vf5, 0x40(%0)\n lqc2 $vf6, 0x50(%0)\n lqc2 $vf7, 0x60(%0)\n lqc2 $vf8, 0x70(%0)\n"
		"lqc2 $vf9, 0x80(%0)\n lqc2 $vf10, 0x90(%0)\n lqc2 $vf11, 0xa0(%0)\n lqc2 $vf12, 0xb0(%0)\n"
		"lqc2 $vf13, 0xc0(%0)\n lqc2 $vf14, 0xd0(%0)\n lqc2 $vf15, 0xe0(%0)\n lqc2 $vf16, 0xf0(%0)\n"
		"lqc2 $vf17, 0x100(%0)\n lqc2 $vf18, 0x110(%0)\n"
		: : "r"(s) : "memory");
	ps2a_vu0_busy = 0;
}
#else
static inline int ps2a_vu0_enter(ps2a_vu0_saved *s) { (void)s; return 0; }
static inline void ps2a_vu0_leave(const ps2a_vu0_saved *s) { (void)s; }
#endif

// ---- kernel FFT: one butterfly stage over `npairs` pairs of two complex numbers (one 4-float vector each) ----------------------------------
// H and L are the upper and lower half of a block (16-byte aligned), C the stage table (npairs x {CA, CB}, 8 floats per pair, 16-byte aligned):
//   for every complex position k:  r = H[k] - L[k];  H[k] += L[k];  L[k] = (r.re*T0 + r.im*T1, r.im*T0 - r.re*T1)
// with CA = (T0, -T1) and CB = (T1, T0) of the position (negating a constant is exact, x*(-T1) = -(x*T1) in every rounding mode).
// This is mdct_butterfly_first / mdct_butterfly_generic of libvorbis (x1 = H, x2 = L).
static inline void ps2a_fft_pairs_c(float *H, float *L, const float *C, int npairs)
{
	int q;
	for (q = 0; q < npairs; q++, H += 4, L += 4, C += 8)
	{
		float r0 = H[0] - L[0], r1 = H[1] - L[1], r2 = H[2] - L[2], r3 = H[3] - L[3];
		H[0] += L[0]; H[1] += L[1]; H[2] += L[2]; H[3] += L[3];
		L[0] = C[0] * r0 + C[4] * r1;
		L[1] = C[1] * r0 + C[5] * r1;
		L[2] = C[2] * r2 + C[6] * r3;
		L[3] = C[3] * r2 + C[7] * r3;
	}
}

#if PS2A_VU0
// npairs must be even and >= 2 (two pairs per iteration, interleaved to cover the 4-cycle FMAC latency)
static inline void ps2a_fft_pairs(float *H, float *L, const float *C, int npairs)
{
	int cnt = npairs >> 1;
	__asm__ volatile(
		".set push\n.set noreorder\n"
		"1:\n"
		"lqc2 $vf1, 0x00(%0)\n"        // Ha
		"lqc2 $vf2, 0x00(%1)\n"        // La
		"lqc2 $vf3, 0x10(%0)\n"        // Hb
		"lqc2 $vf4, 0x10(%1)\n"        // Lb
		"lqc2 $vf5, 0x00(%2)\n"        // CAa
		"lqc2 $vf6, 0x10(%2)\n"        // CBa
		"lqc2 $vf7, 0x20(%2)\n"        // CAb
		"lqc2 $vf8, 0x30(%2)\n"        // CBb
		"vsub $vf9, $vf1, $vf2\n"      // Ra
		"vsub $vf10, $vf3, $vf4\n"     // Rb
		"vadd $vf1, $vf1, $vf2\n"      // Ha'
		"vadd $vf3, $vf3, $vf4\n"      // Hb'
		"vmulx.xy $vf11, $vf5, $vf9\n"
		"vmulz.zw $vf11, $vf5, $vf9\n"
		"vmuly.xy $vf12, $vf6, $vf9\n"
		"vmulw.zw $vf12, $vf6, $vf9\n"
		"vmulx.xy $vf13, $vf7, $vf10\n"
		"vmulz.zw $vf13, $vf7, $vf10\n"
		"vmuly.xy $vf14, $vf8, $vf10\n"
		"vmulw.zw $vf14, $vf8, $vf10\n"
		"sqc2 $vf1, 0x00(%0)\n"
		"sqc2 $vf3, 0x10(%0)\n"
		"vadd $vf11, $vf11, $vf12\n"
		"vadd $vf13, $vf13, $vf14\n"
		"addiu %3, %3, -1\n"
		"sqc2 $vf11, 0x00(%1)\n"
		"sqc2 $vf13, 0x10(%1)\n"
		"addiu %0, %0, 0x20\n"
		"addiu %1, %1, 0x20\n"
		"bgtz %3, 1b\n"
		"addiu %2, %2, 0x40\n"
		".set pop\n"
		: "+&r"(H), "+&r"(L), "+&r"(C), "+&r"(cnt) : : "memory");
}
#endif

// vu: 2 = VU0, 1 = C twin (model / VU0 busy)
static inline void ps2a_fft_pairs_dispatch(int vu, float *H, float *L, const float *C, int npairs)
{
#if PS2A_VU0
	if (vu == 2) { ps2a_fft_pairs(H, L, C, npairs); return; }
#endif
	(void)vu;
	ps2a_fft_pairs_c(H, L, C, npairs);
}

// ---- kernel ROT1: the pre-rotation of mdct_backward (the two loops that read the spectrum `in` and write the complex array U) ----------------
// Lower half (complex m < n/8): a = in[4m+1], b = in[4m+3]; upper half (m >= n/8, s = m - n/8): a = in[n/2-4-4s], b = in[n/2-2-4s].
// Per complex number:  re = a*CA.re + b*CB.re,  im = a*CA.im + b*CB.im  with the per-position constants of ps2a_build_rot1 (mdct.c): the negations of
// the scalar form  -b*t1 - a*t0  /  a*t1 - b*t0  are folded into the constants (exact).  Two complex numbers (one vector of U) per pair.
// G points at the first group (4 floats) of the first pair; the lower kernel walks up, the upper one down.
static inline void ps2a_rot1_lo_c(const float *G, float *O, const float *C, int npairs)
{
	int q, c;
	for (q = 0; q < npairs; q++, G += 8, C += 8, O += 4)
		for (c = 0; c < 2; c++)
		{
			float a = G[4 * c + 1], b = G[4 * c + 3];
			O[2 * c] = a * C[2 * c] + b * C[4 + 2 * c];
			O[2 * c + 1] = a * C[2 * c + 1] + b * C[4 + 2 * c + 1];
		}
}
static inline void ps2a_rot1_hi_c(const float *G, float *O, const float *C, int npairs)
{
	int q, c;
	for (q = 0; q < npairs; q++, G -= 8, C += 8, O += 4)
		for (c = 0; c < 2; c++)
		{
			float a = G[-4 * c], b = G[-4 * c + 2];
			O[2 * c] = a * C[2 * c] + b * C[4 + 2 * c];
			O[2 * c + 1] = a * C[2 * c + 1] + b * C[4 + 2 * c + 1];
		}
}
#if PS2A_VU0
// npairs even, >= 2
static inline void ps2a_rot1_lo(const float *G, float *O, const float *C, int npairs)
{
	int cnt = npairs >> 1;
	__asm__ volatile(
		".set push\n.set noreorder\n"
		"1:\n"
		"lqc2 $vf1, 0x00(%0)\n"
		"lqc2 $vf2, 0x10(%0)\n"
		"lqc2 $vf3, 0x20(%0)\n"
		"lqc2 $vf4, 0x30(%0)\n"
		"lqc2 $vf5, 0x00(%2)\n"
		"lqc2 $vf6, 0x10(%2)\n"
		"lqc2 $vf7, 0x20(%2)\n"
		"lqc2 $vf8, 0x30(%2)\n"
		"vmuly.xy $vf9, $vf5, $vf1\n"
		"vmuly.zw $vf9, $vf5, $vf2\n"
		"vmulw.xy $vf10, $vf6, $vf1\n"
		"vmulw.zw $vf10, $vf6, $vf2\n"
		"vmuly.xy $vf11, $vf7, $vf3\n"
		"vmuly.zw $vf11, $vf7, $vf4\n"
		"vmulw.xy $vf12, $vf8, $vf3\n"
		"vmulw.zw $vf12, $vf8, $vf4\n"
		"vadd $vf9, $vf9, $vf10\n"
		"vadd $vf11, $vf11, $vf12\n"
		"addiu %3, %3, -1\n"
		"sqc2 $vf9, 0x00(%1)\n"
		"sqc2 $vf11, 0x10(%1)\n"
		"addiu %0, %0, 0x40\n"
		"addiu %1, %1, 0x20\n"
		"bgtz %3, 1b\n"
		"addiu %2, %2, 0x40\n"
		".set pop\n"
		: "+&r"(G), "+&r"(O), "+&r"(C), "+&r"(cnt) : : "memory");
}
static inline void ps2a_rot1_hi(const float *G, float *O, const float *C, int npairs)
{
	int cnt = npairs >> 1;
	__asm__ volatile(
		".set push\n.set noreorder\n"
		"1:\n"
		"lqc2 $vf1, 0x00(%0)\n"
		"lqc2 $vf2, -0x10(%0)\n"
		"lqc2 $vf3, -0x20(%0)\n"
		"lqc2 $vf4, -0x30(%0)\n"
		"lqc2 $vf5, 0x00(%2)\n"
		"lqc2 $vf6, 0x10(%2)\n"
		"lqc2 $vf7, 0x20(%2)\n"
		"lqc2 $vf8, 0x30(%2)\n"
		"vmulx.xy $vf9, $vf5, $vf1\n"
		"vmulx.zw $vf9, $vf5, $vf2\n"
		"vmulz.xy $vf10, $vf6, $vf1\n"
		"vmulz.zw $vf10, $vf6, $vf2\n"
		"vmulx.xy $vf11, $vf7, $vf3\n"
		"vmulx.zw $vf11, $vf7, $vf4\n"
		"vmulz.xy $vf12, $vf8, $vf3\n"
		"vmulz.zw $vf12, $vf8, $vf4\n"
		"vadd $vf9, $vf9, $vf10\n"
		"vadd $vf11, $vf11, $vf12\n"
		"addiu %3, %3, -1\n"
		"sqc2 $vf9, 0x00(%1)\n"
		"sqc2 $vf11, 0x10(%1)\n"
		"addiu %0, %0, -0x40\n"
		"addiu %1, %1, 0x20\n"
		"bgtz %3, 1b\n"
		"addiu %2, %2, 0x40\n"
		".set pop\n"
		: "+&r"(G), "+&r"(O), "+&r"(C), "+&r"(cnt) : : "memory");
}
#endif
static inline void ps2a_rot1_lo_dispatch(int vu, const float *G, float *O, const float *C, int np)
{
#if PS2A_VU0
	if (vu == 2) { ps2a_rot1_lo(G, O, C, np); return; }
#endif
	(void)vu;
	ps2a_rot1_lo_c(G, O, C, np);
}
static inline void ps2a_rot1_hi_dispatch(int vu, const float *G, float *O, const float *C, int np)
{
#if PS2A_VU0
	if (vu == 2) { ps2a_rot1_hi(G, O, C, np); return; }
#endif
	(void)vu;
	ps2a_rot1_hi_c(G, O, C, np);
}

// ---- kernels ROT2: the final rotation + unfold of mdct_backward -------------------------------------------------------------------------------
// y = out[0..n/2) holds n/4 complex numbers (a_j, b_j) = (y[2j], y[2j+1]); with the constants (t0_j, t1_j) of trig[n/2+2j]:
//   u_j = a*t1 - b*t0,  v_j = -(a*t0 + b*t1)       and the output  [ rev(u) | -u | rev(v) | v ]  (n/4 floats each, rev = reversed order).
// Pass A (rot2_a): u_j -> Ut[j] (temporary, the upper half of the output buffer) and v_j -> Vn[j] (its final place), four complex numbers per group;
// C = per group {T1, T0, -T0, -T1} as four vectors of the four lanes' constants (structure of arrays).  Pass B (rev_neg / rev): R1 = reversed Ut,
// R2 = -Ut (a multiplication by -1: -0 stays exact), R3 = reversed Vn.  The reversal is a multiplication by 1.0 of a single lane (an exact move).
static inline void ps2a_rot2_a_c(const float *Y, float *Ut, float *Vn, const float *C, int ngroups)
{
	int g, l;
	for (g = 0; g < ngroups; g++, Y += 8, Ut += 4, Vn += 4, C += 16)
		for (l = 0; l < 4; l++)
		{
			float a = Y[2 * l], b = Y[2 * l + 1];
			Ut[l] = a * C[l] - b * C[4 + l];
			Vn[l] = a * C[8 + l] + b * C[12 + l];
		}
}
// R1 + 4*(n/4 - 4 - 4g)... R1 and R2 are passed as the address of the first (g = 0) reversed block (the highest) and the plain block
static inline void ps2a_rev_neg_c(const float *U, float *R1, float *R2, int ngroups)
{
	int g;
	for (g = 0; g < ngroups; g++, U += 4, R1 -= 4, R2 += 4)
	{
		R1[0] = U[3]; R1[1] = U[2]; R1[2] = U[1]; R1[3] = U[0];
		R2[0] = -U[0]; R2[1] = -U[1]; R2[2] = -U[2]; R2[3] = -U[3];
	}
}
static inline void ps2a_rev_c(const float *V, float *R3, int ngroups)
{
	int g;
	for (g = 0; g < ngroups; g++, V += 4, R3 -= 4)
	{ R3[0] = V[3]; R3[1] = V[2]; R3[2] = V[1]; R3[3] = V[0]; }
}
#if PS2A_VU0
static const float ps2a_k_one[4] __attribute__((aligned(16))) = {1.0f, 1.0f, 1.0f, 1.0f};
static const float ps2a_k_minus1[4] __attribute__((aligned(16))) = {-1.0f, -1.0f, -1.0f, -1.0f};
// one group per iteration, pipelined by hand in pairs of two groups (ngroups even, >= 2)
static inline void ps2a_rot2_a(const float *Y, float *Ut, float *Vn, const float *C, int ngroups)
{
	int cnt = ngroups >> 1;
	__asm__ volatile(
		".set push\n.set noreorder\n"
		"1:\n"
		"lqc2 $vf1, 0x00(%0)\n"
		"lqc2 $vf2, 0x10(%0)\n"
		"lqc2 $vf3, 0x00(%3)\n"
		"lqc2 $vf4, 0x10(%3)\n"
		"lqc2 $vf5, 0x20(%3)\n"
		"lqc2 $vf6, 0x30(%3)\n"
		"lqc2 $vf11, 0x20(%0)\n"
		"lqc2 $vf12, 0x30(%0)\n"
		"lqc2 $vf13, 0x40(%3)\n"
		"lqc2 $vf14, 0x50(%3)\n"
		"lqc2 $vf15, 0x60(%3)\n"
		"lqc2 $vf16, 0x70(%3)\n"
		"vmulx.x $vf7, $vf3, $vf1\n"
		"vmulz.y $vf7, $vf3, $vf1\n"
		"vmulx.z $vf7, $vf3, $vf2\n"
		"vmulz.w $vf7, $vf3, $vf2\n"
		"vmuly.x $vf8, $vf4, $vf1\n"
		"vmulw.y $vf8, $vf4, $vf1\n"
		"vmuly.z $vf8, $vf4, $vf2\n"
		"vmulw.w $vf8, $vf4, $vf2\n"
		"vmulx.x $vf9, $vf5, $vf1\n"
		"vmulz.y $vf9, $vf5, $vf1\n"
		"vmulx.z $vf9, $vf5, $vf2\n"
		"vmulz.w $vf9, $vf5, $vf2\n"
		"vmuly.x $vf10, $vf6, $vf1\n"
		"vmulw.y $vf10, $vf6, $vf1\n"
		"vmuly.z $vf10, $vf6, $vf2\n"
		"vmulw.w $vf10, $vf6, $vf2\n"
		"vmulx.x $vf17, $vf13, $vf11\n"
		"vmulz.y $vf17, $vf13, $vf11\n"
		"vmulx.z $vf17, $vf13, $vf12\n"
		"vmulz.w $vf17, $vf13, $vf12\n"
		"vsub $vf7, $vf7, $vf8\n"
		"vadd $vf9, $vf9, $vf10\n"
		"vmuly.x $vf18, $vf14, $vf11\n"
		"vmulw.y $vf18, $vf14, $vf11\n"
		"vmuly.z $vf18, $vf14, $vf12\n"
		"vmulw.w $vf18, $vf14, $vf12\n"
		"sqc2 $vf7, 0x00(%1)\n"
		"sqc2 $vf9, 0x00(%2)\n"
		"vmulx.x $vf19, $vf15, $vf11\n"
		"vmulz.y $vf19, $vf15, $vf11\n"
		"vmulx.z $vf19, $vf15, $vf12\n"
		"vmulz.w $vf19, $vf15, $vf12\n"
		"vmuly.x $vf20, $vf16, $vf11\n"
		"vmulw.y $vf20, $vf16, $vf11\n"
		"vmuly.z $vf20, $vf16, $vf12\n"
		"vmulw.w $vf20, $vf16, $vf12\n"
		"vsub $vf17, $vf17, $vf18\n"
		"vadd $vf19, $vf19, $vf20\n"
		"addiu %4, %4, -1\n"
		"sqc2 $vf17, 0x10(%1)\n"
		"sqc2 $vf19, 0x10(%2)\n"
		"addiu %0, %0, 0x40\n"
		"addiu %1, %1, 0x20\n"
		"addiu %2, %2, 0x20\n"
		"bgtz %4, 1b\n"
		"addiu %3, %3, 0x80\n"
		".set pop\n"
		: "+&r"(Y), "+&r"(Ut), "+&r"(Vn), "+&r"(C), "+&r"(cnt) : : "memory");
}
static inline void ps2a_rev_neg(const float *U, float *R1, float *R2, int ngroups)
{
	int cnt = ngroups;
	__asm__ volatile(
		"lqc2 $vf30, 0(%4)\n"
		"lqc2 $vf31, 0(%5)\n"
		".set push\n.set noreorder\n"
		"1:\n"
		"lqc2 $vf1, 0x00(%0)\n"
		"addiu %3, %3, -1\n"
		"vmulw.x $vf2, $vf30, $vf1\n"
		"vmulz.y $vf2, $vf30, $vf1\n"
		"vmuly.z $vf2, $vf30, $vf1\n"
		"vmulx.w $vf2, $vf30, $vf1\n"
		"vmul $vf3, $vf1, $vf31\n"
		"addiu %0, %0, 0x10\n"
		"sqc2 $vf2, 0x00(%1)\n"
		"sqc2 $vf3, 0x00(%2)\n"
		"addiu %2, %2, 0x10\n"
		"bgtz %3, 1b\n"
		"addiu %1, %1, -0x10\n"
		".set pop\n"
		: "+&r"(U), "+&r"(R1), "+&r"(R2), "+&r"(cnt) : "r"(ps2a_k_one), "r"(ps2a_k_minus1) : "memory");
}
static inline void ps2a_rev(const float *V, float *R3, int ngroups)
{
	int cnt = ngroups;
	__asm__ volatile(
		"lqc2 $vf30, 0(%3)\n"
		".set push\n.set noreorder\n"
		"1:\n"
		"lqc2 $vf1, 0x00(%0)\n"
		"addiu %2, %2, -1\n"
		"vmulw.x $vf2, $vf30, $vf1\n"
		"vmulz.y $vf2, $vf30, $vf1\n"
		"vmuly.z $vf2, $vf30, $vf1\n"
		"vmulx.w $vf2, $vf30, $vf1\n"
		"addiu %0, %0, 0x10\n"
		"sqc2 $vf2, 0x00(%1)\n"
		"bgtz %2, 1b\n"
		"addiu %1, %1, -0x10\n"
		".set pop\n"
		: "+&r"(V), "+&r"(R3), "+&r"(cnt) : "r"(ps2a_k_one) : "memory");
}
#endif
static inline void ps2a_rot2_a_dispatch(int vu, const float *Y, float *Ut, float *Vn, const float *C, int ng)
{
#if PS2A_VU0
	if (vu == 2) { ps2a_rot2_a(Y, Ut, Vn, C, ng); return; }
#endif
	(void)vu;
	ps2a_rot2_a_c(Y, Ut, Vn, C, ng);
}
static inline void ps2a_rev_neg_dispatch(int vu, const float *U, float *R1, float *R2, int ng)
{
#if PS2A_VU0
	if (vu == 2) { ps2a_rev_neg(U, R1, R2, ng); return; }
#endif
	(void)vu;
	ps2a_rev_neg_c(U, R1, R2, ng);
}
static inline void ps2a_rev_dispatch(int vu, const float *V, float *R3, int ng)
{
#if PS2A_VU0
	if (vu == 2) { ps2a_rev(V, R3, ng); return; }
#endif
	(void)vu;
	ps2a_rev_c(V, R3, ng);
}

// ---- kernel BR: mdct_bitreverse (gather of complex pairs by the bit-reversed table, rotation by trig[n..], unfold into two descending/ascending runs) --
// Per iteration two sub-steps (A in lanes xy, B in lanes zw): C = {CA = (T0A, T1A, T0B, T1B), CB = (T1A, -T0A, T1B, -T0B)} (8 floats, 16-byte aligned),
// boff = four BYTE offsets from x of the complex numbers x0A, x1A, x0B, x1B (the bit-reverse table of the scalar code times 4).
//   E = (x0A, x0B), F = (x1A, x1B); S = E + F; D = E - F; Z = CA*S.x/z + CB*D.y/w; W = 0.5*(S.y, D.x | S.w, D.z)
//   w0[0..3] = W + Z;  (w1[2], w1[3], w1[0], w1[1]) = (W.x - Z.x, Z.y - W.y, W.z - Z.z, Z.w - W.w);  w0 += 4, w1 -= 4 per iteration.
// Each line is the same single operation as in the scalar code (mul, mul, add; the halving is a multiplication by 0.5 as there).
static inline void ps2a_bitrev_c(const float *x, float *w0, float *w1, const float *cb, int iters)
{
	int it;
	for (it = 0; it < iters; it++, cb += 12, w0 += 4)
	{
		const float *C = cb;
		const int *boff = (const int *)(cb + 8);
		const float *xa0 = (const float *)((const char *)x + boff[0]), *xa1 = (const float *)((const char *)x + boff[1]);
		const float *xb0 = (const float *)((const char *)x + boff[2]), *xb1 = (const float *)((const char *)x + boff[3]);
		float E[4] = {xa0[0], xa0[1], xb0[0], xb0[1]}, F[4] = {xa1[0], xa1[1], xb1[0], xb1[1]};
		float S[4], D[4], P[4], Q[4], Z[4], W[4], Y1[4];
		int l;
		for (l = 0; l < 4; l++) { S[l] = E[l] + F[l]; D[l] = E[l] - F[l]; }
		P[0] = C[0] * S[0]; P[1] = C[1] * S[0]; P[2] = C[2] * S[2]; P[3] = C[3] * S[2];
		Q[0] = C[4] * D[1]; Q[1] = C[5] * D[1]; Q[2] = C[6] * D[3]; Q[3] = C[7] * D[3];
		for (l = 0; l < 4; l++) Z[l] = P[l] + Q[l];
		W[0] = 0.5f * S[1]; W[1] = 0.5f * D[0]; W[2] = 0.5f * S[3]; W[3] = 0.5f * D[2];
		for (l = 0; l < 4; l++) w0[l] = W[l] + Z[l];
		Y1[0] = W[0] - Z[0]; Y1[1] = Z[1] - W[1]; Y1[2] = W[2] - Z[2]; Y1[3] = Z[3] - W[3];
		w1 -= 4;
		w1[0] = Y1[2]; w1[1] = Y1[3]; w1[2] = Y1[0]; w1[3] = Y1[1];
	}
}
#if PS2A_VU0
static const float ps2a_k_half[4] __attribute__((aligned(16))) = {0.5f, 0.5f, 0.5f, 0.5f};
// The table stride is 12 words (C: 8 floats, boff: 4 ints) per iteration: C and boff are one array (cb), iterations are 48 bytes apart.
static inline void ps2a_bitrev(const float *x, float *w0, float *w1, const float *cb, int iters)
{
	int t0, t1, t2, t3, e0, e1, f0, f1;
	__asm__ volatile(
		"lqc2 $vf30, 0(%[half])\n"
		".set push\n.set noreorder\n"
		"1:\n"
		"lw %[t0], 0x20(%[cb])\n"
		"lw %[t1], 0x24(%[cb])\n"
		"lw %[t2], 0x28(%[cb])\n"
		"lw %[t3], 0x2c(%[cb])\n"
		"lqc2 $vf3, 0x00(%[cb])\n"
		"lqc2 $vf4, 0x10(%[cb])\n"
		"addu %[t0], %[t0], %[x]\n"
		"addu %[t1], %[t1], %[x]\n"
		"addu %[t2], %[t2], %[x]\n"
		"addu %[t3], %[t3], %[x]\n"
		"ld %[e0], 0(%[t0])\n"
		"ld %[e1], 0(%[t2])\n"
		"ld %[f0], 0(%[t1])\n"
		"ld %[f1], 0(%[t3])\n"
		"pcpyld %[e0], %[e1], %[e0]\n"
		"pcpyld %[f0], %[f1], %[f0]\n"
		"qmtc2 %[e0], $vf1\n"
		"qmtc2 %[f0], $vf2\n"
		"vadd $vf5, $vf1, $vf2\n"
		"vsub $vf6, $vf1, $vf2\n"
		"vmulx.xy $vf7, $vf3, $vf5\n"
		"vmulz.zw $vf7, $vf3, $vf5\n"
		"vmuly.xy $vf8, $vf4, $vf6\n"
		"vmulw.zw $vf8, $vf4, $vf6\n"
		"vmuly.x $vf9, $vf30, $vf5\n"
		"vmulx.y $vf9, $vf30, $vf6\n"
		"vmulw.z $vf9, $vf30, $vf5\n"
		"vmulz.w $vf9, $vf30, $vf6\n"
		"vadd $vf7, $vf7, $vf8\n"
		"vadd $vf10, $vf9, $vf7\n"
		"vsub.xz $vf11, $vf9, $vf7\n"
		"vsub.yw $vf11, $vf7, $vf9\n"
		"sqc2 $vf10, 0(%[w0])\n"
		"vmr32 $vf11, $vf11\n"
		"vmr32 $vf11, $vf11\n"
		"addiu %[w1], %[w1], -0x10\n"
		"sqc2 $vf11, 0(%[w1])\n"
		"addiu %[it], %[it], -1\n"
		"addiu %[w0], %[w0], 0x10\n"
		"bgtz %[it], 1b\n"
		"addiu %[cb], %[cb], 0x30\n"
		".set pop\n"
		: [w0] "+&r"(w0), [w1] "+&r"(w1), [cb] "+&r"(cb), [it] "+&r"(iters),
		  [t0] "=&r"(t0), [t1] "=&r"(t1), [t2] "=&r"(t2), [t3] "=&r"(t3), [e0] "=&r"(e0), [e1] "=&r"(e1), [f0] "=&r"(f0), [f1] "=&r"(f1)
		: [x] "r"(x), [half] "r"(ps2a_k_half) : "memory");
}
#endif
static inline void ps2a_bitrev_dispatch(int vu, const float *x, float *w0, float *w1, const float *cb, int iters)
{
#if PS2A_VU0
	if (vu == 2) { ps2a_bitrev(x, w0, w1, cb, iters); return; }
#endif
	(void)vu;
	ps2a_bitrev_c(x, w0, w1, cb, iters);
}

#endif
