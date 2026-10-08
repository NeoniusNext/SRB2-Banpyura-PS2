// Cycles of the stages of mdct_backward (libvorbis 1.3.7 as vendored in src/ps2/vorbis/mdct.c) on the EE: pre-rotate, butterfly_first,
// generic stages, the 32-point stage, bit reverse + post rotate, final rotate/unfold.  EETEST mdct_stages lines.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <kernel.h>
#include "../../../src/ps2/vorbis/mdct.c"

static inline unsigned CopCount(void) { unsigned v; __asm__ volatile("mfc0 %0,$9" : "=r"(v)); return v; }

static unsigned long long acc[8];

static void backward_stages(mdct_lookup *init, DATA_TYPE *in, DATA_TYPE *out)
{
	int n = init->n, n2 = n >> 1, n4 = n >> 2;
	unsigned c0, c1;
	DATA_TYPE *iX = in + n2 - 7, *oX = out + n2 + n4, *T = init->trig + n4;
	c0 = CopCount();
	do {
		oX -= 4;
		oX[0] = (-iX[2] * T[3] - iX[0] * T[2]);
		oX[1] = (iX[0] * T[3] - iX[2] * T[2]);
		oX[2] = (-iX[6] * T[1] - iX[4] * T[0]);
		oX[3] = (iX[4] * T[1] - iX[6] * T[0]);
		iX -= 8; T += 4;
	} while (iX >= in);
	iX = in + n2 - 8; oX = out + n2 + n4; T = init->trig + n4;
	do {
		T -= 4;
		oX[0] = (iX[4] * T[3] + iX[6] * T[2]);
		oX[1] = (iX[4] * T[2] - iX[6] * T[3]);
		oX[2] = (iX[0] * T[1] + iX[2] * T[0]);
		oX[3] = (iX[0] * T[0] - iX[2] * T[1]);
		iX -= 8; oX += 4;
	} while (iX >= in);
	c1 = CopCount(); acc[0] += c1 - c0; c0 = c1;
	{
		DATA_TYPE *x = out + n2, *Tt = init->trig;
		int points = n2, stages = init->log2n - 5, i, j;
		if (--stages > 0) mdct_butterfly_first(Tt, x, points);
		c1 = CopCount(); acc[1] += c1 - c0; c0 = c1;
		for (i = 1; --stages > 0; i++)
			for (j = 0; j < (1 << i); j++) mdct_butterfly_generic(Tt, x + (points >> i) * j, points >> i, 4 << i);
		c1 = CopCount(); acc[2] += c1 - c0; c0 = c1;
		for (j = 0; j < points; j += 32) mdct_butterfly_32(x + j);
		c1 = CopCount(); acc[3] += c1 - c0; c0 = c1;
	}
	mdct_bitreverse(init, out);
	c1 = CopCount(); acc[4] += c1 - c0; c0 = c1;
	{
		DATA_TYPE *oX1 = out + n2 + n4, *oX2 = out + n2 + n4, *iX2 = out, *T2 = init->trig + n2;
		do {
			oX1 -= 4;
			oX1[3] = (iX2[0] * T2[1] - iX2[1] * T2[0]);
			oX2[0] = -(iX2[0] * T2[0] + iX2[1] * T2[1]);
			oX1[2] = (iX2[2] * T2[3] - iX2[3] * T2[2]);
			oX2[1] = -(iX2[2] * T2[2] + iX2[3] * T2[3]);
			oX1[1] = (iX2[4] * T2[5] - iX2[5] * T2[4]);
			oX2[2] = -(iX2[4] * T2[4] + iX2[5] * T2[5]);
			oX1[0] = (iX2[6] * T2[7] - iX2[7] * T2[6]);
			oX2[3] = -(iX2[6] * T2[6] + iX2[7] * T2[7]);
			oX2 += 4; iX2 += 8; T2 += 8;
		} while (iX2 < oX1);
		c1 = CopCount(); acc[5] += c1 - c0; c0 = c1;
		iX2 = out + n2 + n4; oX1 = out + n4; oX2 = oX1;
		do {
			oX1 -= 4; iX2 -= 4;
			oX2[0] = -(oX1[3] = iX2[3]);
			oX2[1] = -(oX1[2] = iX2[2]);
			oX2[2] = -(oX1[1] = iX2[1]);
			oX2[3] = -(oX1[0] = iX2[0]);
			oX2 += 4;
		} while (oX2 < iX2);
		iX2 = out + n2 + n4; oX1 = out + n2 + n4; oX2 = out + n2;
		do {
			oX1 -= 4;
			oX1[0] = iX2[3]; oX1[1] = iX2[2]; oX1[2] = iX2[1]; oX1[3] = iX2[0];
			iX2 += 4;
		} while (oX1 > oX2);
		c1 = CopCount(); acc[6] += c1 - c0; c0 = c1;
	}
}

int main(int argc, char **argv)
{
	static mdct_lookup lk;
	static float in[2048] __attribute__((aligned(64))), a[2048] __attribute__((aligned(64))), b[2048] __attribute__((aligned(64))), c[2048] __attribute__((aligned(64)));
	int sizes[2] = {1024, 128}, s, it, i;
	(void)argc; (void)argv;
	for (s = 0; s < 2; s++)
	{
		int n = sizes[s];
		unsigned long long tot = 0;
		unsigned seed = 12345;
		mdct_init(&lk, n);
		memset(acc, 0, sizeof acc);
		for (i = 0; i < n; i++) { seed = seed * 1664525u + 1013904223u; in[i] = ((int)(seed >> 8) - 8388608) * (1.0f / 4096.0f); }
		for (it = 0; it < 200; it++)
		{
			unsigned t0, t1;
			memcpy(a, in, sizeof in); memcpy(b, in, sizeof in);
			t0 = CopCount(); mdct_backward(&lk, a, a); t1 = CopCount(); tot += t1 - t0;
			backward_stages(&lk, b, b);
		}
		printf("EETEST mdct n=%d total=%llu cyc/call (stages sum %llu): rotate1=%llu first=%llu generic=%llu b32=%llu bitrev=%llu rotate2=%llu unfold=%llu\n", n, tot / 200,
			(acc[0] + acc[1] + acc[2] + acc[3] + acc[4] + acc[5] + acc[6]) / 200, acc[0] / 200, acc[1] / 200, acc[2] / 200, acc[3] / 200, acc[4] / 200, acc[5] / 200, acc[6] / 200);
		printf("EETEST mdct n=%d instrumented result equals mdct_backward: %s\n", n, memcmp(a, b, n * sizeof(float)) ? "NO" : "yes");
		(void)c;
	}
	printf("EETEST DONE\n");
	SleepThread();
	return 0;
}
