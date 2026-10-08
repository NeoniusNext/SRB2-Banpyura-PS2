#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <kernel.h>
#include "../../../src/ps2/vorbis/mdct.c"

static float X[2048] __attribute__((aligned(64))), A[2048] __attribute__((aligned(64))), B[2048] __attribute__((aligned(64)));
int main(void)
{
	mdct_lookup l;
	int n = 128, i, bad = 0;
	unsigned seed = 5;
	ps2a_vu0_saved sv;
	mdct_init(&l, n);
	for (i = 0; i < n; i++) { seed = seed * 1664525u + 1013904223u; X[i] = ((int)(seed >> 8) - 8388608) * (1.0f / 4096.0f); }
	memcpy(A, X, sizeof X); memcpy(B, X, sizeof X);
	{
		const float *cb = ps2a_br_tab(&l);
		printf("EETEST cb=%p out=%p n=%d iters=%d\n", (const void *)cb, (void *)A, n, n >> 4);
		for (i = 0; i < 12; i++) printf("EETEST cb[%d]=%08x (%g)\n", i, ((const unsigned *)cb)[i], cb[i]);
		ps2a_vu0_enter(&sv);
		ps2a_bitrev_dispatch(2, A + n / 2, A, A + n / 2, cb, n >> 4);
		ps2a_vu0_leave(&sv);
		ps2a_bitrev_dispatch(1, B + n / 2, B, B + n / 2, cb, n >> 4);
	}
	for (i = 0; i < n / 2; i++) if (A[i] != B[i]) { if (bad < 10) printf("EETEST diff at %d: asm %g twin %g\n", i, A[i], B[i]); bad++; }
	printf("EETEST br_dbg: %d diffs of %d\n", bad, n / 2);
	printf("EETEST DONE\n");
	SleepThread();
	return 0;
}
