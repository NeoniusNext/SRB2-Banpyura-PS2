// PS2-315 on the EE (PCSX2): src/ps2/vorbis/mdct.c with the VU0 kernels against the libvorbis original (mdct_ref.c): bit equality for
// n = 64..8192 on random data (several magnitudes, zeros), cycles per call (COP0 count: an instruction-count model in PCSX2, not real EE time).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <kernel.h>
#include "mdct_ref.c"
#include "../../../src/ps2/vorbis/mdct.c"

static inline unsigned CopCount(void) { unsigned v; __asm__ volatile("mfc0 %0,$9" : "=r"(v)); return v; }
static float bufx[8192] __attribute__((aligned(64))), bufa[8192] __attribute__((aligned(64))), bufb[8192] __attribute__((aligned(64)));

int main(void)
{
	unsigned seed = 1;
	int bad = 0, total = 0, log2n;
	float worst = 0;
	long differing = 0, elems = 0;
	for (log2n = 6; log2n <= 13; log2n++)
	{
		int n = 1 << log2n, it, reps = log2n >= 11 ? 20 : 60;
		mdct_lookup a, b;
		unsigned long long ca = 0, cb = 0;
		ref_mdct_init(&a, n);
		mdct_init(&b, n);
		for (it = 0; it < reps; it++)
		{
			int i, mode = it % 4;
			unsigned t0, t1, t2;
			for (i = 0; i < n; i++)
			{
				seed = seed * 1664525u + 1013904223u;
				bufx[i] = ((int)(seed >> 8) - 8388608) * (1.0f / 4096.0f);
				if (mode == 1) bufx[i] *= 1e-3f;
				if (mode == 2 && (seed & 8)) bufx[i] = 0;
				if (mode == 3) bufx[i] *= (1 << ((seed >> 4) & 15));
			}
			memcpy(bufa, bufx, n * sizeof(float)); memcpy(bufb, bufx, n * sizeof(float));
			t0 = CopCount(); ref_mdct_backward(&a, bufa, bufa); t1 = CopCount(); mdct_backward(&b, bufb, bufb); t2 = CopCount();
			ca += t1 - t0; cb += t2 - t1;
			total++;
			if (memcmp(bufa, bufb, n * sizeof(float)))
			{
				float mx = 0, md = 0;
				int nd = 0;
				for (i = 0; i < n; i++)
				{
					float d = bufa[i] - bufb[i];
					if (d < 0) d = -d;
					if (bufa[i] > mx) mx = bufa[i];
					if (-bufa[i] > mx) mx = -bufa[i];
					if (d > md) md = d;
					nd += bufa[i] != bufb[i];
				}
				if (mx > 0 && md / mx > worst) worst = md / mx;
				differing += nd; elems += n;
				bad++;
			}
			else elems += n;
		}
		printf("EETEST n=%d ref=%llu new=%llu cycles/call (%.1f%%)\n", n, ca / reps, cb / reps, 100.0 * cb / ca);
		ref_mdct_clear(&a); mdct_clear(&b);
	}
	printf("EETEST mdct_ee: %d transforms, %d not bit-equal, differing elements %ld of %ld (%.2f%%), worst |diff|/max|ref| = %.3g (float ulp of the peak = 1.19e-7)\n", total, bad, differing, elems, 100.0 * differing / elems, (double)worst);
	printf("EETEST DONE\n");
	SleepThread();
	return 0;
}
