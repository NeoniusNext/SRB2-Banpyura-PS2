// PS2-315 host check of the vector-structured MDCT (src/ps2/vorbis/mdct.c with PS2_VORBIS_VU0 + PS2A_MODEL: the C twins of the VU0 kernels)
// against the libvorbis original (mdct_ref.c = the same file with renamed symbols).  Both run on the host's IEEE single precision with
// -ffp-contract=off, so equal bits mean the data flow, the tables and the operation order are right.  The EE build of this test
// (mdct_ee.c) runs the same comparison with the real VU0 kernels on the EE.
// Mode: round to nearest (default) or, with -DTRUNC, toward zero (what VU0 and, on the real console, the EE FPU do).
//   gcc -O2 -ffp-contract=off -DPS2_VORBIS_VU0 -DPS2A_MODEL -DHAVE_ALLOCA_H -w -Isrc/ps2/vorbis tools/ps2/ee_tests/mdct_host.c -lm
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "mdct_ref.c"
#include "../../../src/ps2/vorbis/mdct.c"

static float *alloc16(size_t n) { void *p; if (posix_memalign(&p, 16, n * sizeof(float))) abort(); return p; }

#ifdef TRUNC
#include <fenv.h>
#endif
int main(void)
{
#ifdef TRUNC
	fesetround(FE_TOWARDZERO);
#endif
	unsigned seed = 1;
	int bad = 0, total = 0, log2n, zeros = 0;
	for (log2n = 6; log2n <= 13; log2n++)
	{
		int n = 1 << log2n, it;
		mdct_lookup a, b;
		float *x = alloc16(n), *ra = alloc16(n), *rb = alloc16(n);
		ref_mdct_init(&a, n);
		mdct_init(&b, n);
		for (it = 0; it < 300; it++)
		{
			int i, mode = it % 4;
			for (i = 0; i < n; i++)
			{
				seed = seed * 1664525u + 1013904223u;
				x[i] = ((int)(seed >> 8) - 8388608) * (1.0f / 4096.0f);
				if (mode == 1) x[i] *= 1e-3f;
				if (mode == 2 && (seed & 8)) x[i] = 0;
				if (mode == 3) x[i] *= (1 << ((seed >> 4) & 15));
			}
			memcpy(ra, x, n * sizeof(float)); memcpy(rb, x, n * sizeof(float));
			ref_mdct_backward(&a, ra, ra);
			mdct_backward(&b, rb, rb);
			total++;
			{
				int differ = 0, bitdiff = 0;
				for (i = 0; i < n; i++) { differ += ra[i] != rb[i]; bitdiff += memcmp(&ra[i], &rb[i], 4) != 0; }
				if (bitdiff && !differ) zeros++;      // only the sign of a zero differs: the same number
				if (differ) { if (bad < 5) { for (i = 0; i < n && ra[i] == rb[i]; i++) ; printf("MISMATCH n=%d it=%d first at %d (%a vs %a)\n", n, it, i, ra[i], rb[i]); } bad++; }
			}
		}
		ref_mdct_clear(&a); mdct_clear(&b);
	}
	printf("mdct_host: %d transforms, %d mismatches (%d transforms differ only in the sign of a zero)\n", total, bad, zeros);
	return bad != 0;
}
