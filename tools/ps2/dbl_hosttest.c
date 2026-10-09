/* PS2-LOAD-22: src/ps2/ps2_dbl.h (exact binary64 subtraction / comparison / M_RoundUp on the bit patterns, and the colour-step loop of R_GenerateLightTable)
 * against the hardware double of the host (SSE2 is IEEE binary64, the same rounding as libgcc's soft double on the EE).
 *   cc -O2 -I src/ps2 -o build/hosttest/dbl_hosttest tools/ps2/dbl_hosttest.c -lm && build/hosttest/dbl_hosttest [PLAYPAL.lmp] [iterations]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ps2_dbl.h"

static uint64_t seed = 0x9E3779B97F4A7C15ull;
static uint64_t Rnd64(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed * 0x2545F4914F6CDD1Dull; }
static double FromBits(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }

/* M_RoundUp of m_misc.c, verbatim */
static int M_RoundUp(double number)
{
	if (number > 255.0l)
		return 255;
	if (number < 0.0l)
		return 0;

	if ((int)number <= (int)(number - 0.5f))
		return (int)number + 1;

	return (int)number;
}

static long fails;

static void CheckAdd(double a, double b, const char *what)
{
	int bail = 0;
	volatile double ra = a, rb = b;
	double want = ra - rb;
	uint64_t got = Dbl_Sub(Dbl_Bits(a), Dbl_Bits(b), &bail), w = Dbl_Bits(want);

	if (bail)
	{
		/* allowed only when an operand or the result is not zero / normal */
		if (Dbl_Ok(Dbl_Bits(a)) && Dbl_Ok(Dbl_Bits(b)) && Dbl_Ok(w) && fabs(want) > 1e-290)
		{
			if (fails++ < 20)
				printf("%s: bail for %a - %a (result %a)\n", what, a, b, want);
		}
		return;
	}
	if (got != w)
	{
		if (fails++ < 20)
			printf("%s: %a - %a = %a (%016llx), got %016llx\n", what, a, b, want, (unsigned long long)w, (unsigned long long)got);
	}
}

static void CheckRound(double x)
{
	int want = M_RoundUp(x), got = Dbl_RoundUp(Dbl_Bits(x));

	if (want != got && fails++ < 20)
		printf("roundup %a: want %d got %d\n", x, want, got);
}

/* the setup and loop of R_GenerateLightTable, as in r_data.c (long double constants are double on the EE) */
static unsigned char pal[256][3];

static unsigned char Nearest(void *ctx, uint8_t r, uint8_t g, uint8_t b)
{
	int best = 0, bd = 1 << 30, i;

	(void)ctx;
	for (i = 0; i < 256; i++)
	{
		int dr = (int)r - pal[i][0], dg = (int)g - pal[i][1], db = (int)b - pal[i][2], d = dr * dr + dg * dg + db * db;

		if (d < bd)
		{
			bd = d;
			best = i;
		}
	}
	return (unsigned char)best;
}

static double deltas[256][3], mapd[256][3];

static int LightRef(unsigned char *out, uint32_t rgba, uint32_t fadergba, int fadestart, int fadeend)
{
	double cmaskr, cmaskg, cmaskb, cdestr, cdestg, cdestb, maskamt, othermask;
	uint8_t cr = rgba & 255, cg = (rgba >> 8) & 255, cb = (rgba >> 16) & 255, ca = (rgba >> 24) & 255, cfr = fadergba & 255, cfg = (fadergba >> 8) & 255, cfb = (fadergba >> 16) & 255;
	uint8_t fs = (uint8_t)fadestart, fadedist = (uint8_t)(fadeend - fadestart);
	double r, g, b, cbrightness;
	int p, i;

	cmaskr = cr; cmaskg = cg; cmaskb = cb;
	maskamt = (double)(ca / 255.0);
	othermask = 1 - maskamt;
	maskamt /= 0xff;
	cmaskr *= maskamt; cmaskg *= maskamt; cmaskb *= maskamt;
	cdestr = cfr; cdestg = cfg; cdestb = cfb;
	for (i = 0; i < 256; i++)
	{
		r = pal[i][0]; g = pal[i][1]; b = pal[i][2];
		cbrightness = sqrt((r * r) + (g * g) + (b * b));
		mapd[i][0] = (cbrightness * cmaskr) + (r * othermask);
		if (mapd[i][0] > 255.0) mapd[i][0] = 255.0;
		deltas[i][0] = (mapd[i][0] - cdestr) / (double)fadedist;
		mapd[i][1] = (cbrightness * cmaskg) + (g * othermask);
		if (mapd[i][1] > 255.0) mapd[i][1] = 255.0;
		deltas[i][1] = (mapd[i][1] - cdestg) / (double)fadedist;
		mapd[i][2] = (cbrightness * cmaskb) + (b * othermask);
		if (mapd[i][2] > 255.0) mapd[i][2] = 255.0;
		deltas[i][2] = (mapd[i][2] - cdestb) / (double)fadedist;
	}
	{
		dblbits_t mb[256][3], db[256][3], dest[3];
		unsigned char fast[34 * 256], *colormap_p = out;
		int ok, c;

		for (i = 0; i < 256; i++)
			for (c = 0; c < 3; c++)
			{
				mb[i][c] = Dbl_Bits(mapd[i][c]);
				db[i][c] = Dbl_Bits(deltas[i][c]);
			}
		dest[0] = Dbl_Bits(cdestr); dest[1] = Dbl_Bits(cdestg); dest[2] = Dbl_Bits(cdestb);
		ok = Dbl_LightSteps(mb, db, dest, fs, fast, Nearest, NULL);

		for (p = 0; p < 34; p++)
			for (i = 0; i < 256; i++)
			{
				*colormap_p = Nearest(NULL, (uint8_t)M_RoundUp(mapd[i][0]), (uint8_t)M_RoundUp(mapd[i][1]), (uint8_t)M_RoundUp(mapd[i][2]));
				colormap_p++;
				if ((unsigned)p < fs)
					continue;
#define ABS2(x) ((x) < 0 ? -(x) : (x))
				if (ABS2(mapd[i][0] - cdestr) > ABS2(deltas[i][0])) mapd[i][0] -= deltas[i][0]; else mapd[i][0] = cdestr;
				if (ABS2(mapd[i][1] - cdestg) > ABS2(deltas[i][1])) mapd[i][1] -= deltas[i][1]; else mapd[i][1] = cdestg;
				if (ABS2(mapd[i][2] - cdestb) > ABS2(deltas[i][1])) mapd[i][2] -= deltas[i][2]; else mapd[i][2] = cdestb;
#undef ABS2
			}
		if (!ok)
			return 0;
		if (memcmp(fast, out, sizeof fast))
		{
			for (i = 0; i < 34 * 256; i++)
				if (fast[i] != out[i])
					break;
			printf("light: rgba %08x fade %08x start %d end %d: first difference at level %d entry %d (%d vs %d)\n", (unsigned)rgba, (unsigned)fadergba, fadestart, fadeend, i / 256, i % 256, fast[i], out[i]);
			fails++;
		}
		return 1;
	}
}

int main(int argc, char **argv)
{
	long n, N = argc > 2 ? atol(argv[2]) : 20000000, i, handled = 0, bailed = 0;
	unsigned char game[768];
	int havegame = 0;

	if (argc > 1)
	{
		FILE *f = fopen(argv[1], "rb");

		if (f && fread(game, 1, 768, f) == 768)
			havegame = 1;
		if (f)
			fclose(f);
	}

	/* 1. a - b over random operands of every kind */
	for (n = 0; n < N; n++)
	{
		uint64_t r1 = Rnd64(), r2 = Rnd64();
		int kind = (int)(r1 & 7);
		double a, b;

		switch (kind)
		{
			case 0: /* anything normal */
				a = FromBits((r1 & 0x800FFFFFFFFFFFFFull) | ((uint64_t)(0x3C0 + (r2 & 63)) << 52));
				b = FromBits((r2 & 0x800FFFFFFFFFFFFFull) | ((uint64_t)(0x3C0 + ((r2 >> 8) & 63)) << 52));
				break;
			case 1: /* the range of the colour steps */
				a = (double)(r1 % 256) * (double)((r2 >> 3) % 1000) / 1000.0;
				b = (double)((r2 >> 20) % 256) / 31.0 * (double)((r1 >> 11) % 40);
				break;
			case 2: /* nearly equal: cancellation */
				a = FromBits((r1 & 0x000FFFFFFFFFFFFFull) | ((uint64_t)(0x3F0 + ((r1 >> 60) & 15)) << 52));
				b = FromBits(Dbl_Bits(a) + (int64_t)(r2 % 9) - 4);
				break;
			case 3: /* ties: b has a one in the first bit below a's last place */
				a = FromBits((r1 & 0x000FFFFFFFFFFFFFull) | ((uint64_t)(0x400 + (r1 >> 60)) << 52));
				b = FromBits(((r2 & 0x000FFFFFFFFFFFFFull) & ~0x7ull) | ((uint64_t)(0x400 + (r1 >> 60) - 53 - (r2 >> 61 & 1)) << 52) | (r2 & 1 ? 0 : 0));
				break;
			case 4: /* integers and small fractions */
				a = (double)(int)(r1 % 513) - 256.0 + (double)(r2 % 8) / 8.0;
				b = (double)(int)((r2 >> 9) % 513) - 256.0;
				break;
			case 5: /* exponent far apart */
				a = FromBits((r1 & 0x800FFFFFFFFFFFFFull) | ((uint64_t)(0x3FF + (r2 & 7)) << 52));
				b = FromBits((r2 & 0x800FFFFFFFFFFFFFull) | ((uint64_t)(0x3FF - (int)((r2 >> 8) % 120)) << 52));
				break;
			case 6: /* zeros */
				a = (r1 & 1) ? 0.0 : -0.0;
				b = (r1 & 2) ? 0.0 : -0.0;
				if (r1 & 4) a = FromBits((r2 & 0x800FFFFFFFFFFFFFull) | (0x3FFull << 52));
				break;
			default: /* a value and a multiple of a fraction of it */
				a = (double)(r1 % 256);
				b = a / (double)(1 + r2 % 31) * (double)(r2 >> 8 & 31);
				break;
		}
		CheckAdd(a, b, "sub");
		CheckAdd(b, a, "sub2");
		CheckAdd(-a, b, "sub3");
	}
	printf("sub: %ld x 3 operand pairs\n", N);

	/* 2. M_RoundUp */
	for (n = 0; n < N / 4; n++)
	{
		uint64_t r1 = Rnd64();
		double x = (double)(int)(r1 % 5000) / 16.0 - 40.0 + (double)((r1 >> 20) % 1000) / 1.0e10;

		CheckRound(x);
		CheckRound(nextafter(x, 1e9));
		CheckRound(nextafter(x, -1e9));
		x = (double)(int)((r1 >> 30) % 600) / 2.0 - 20.0; /* integers and halves */
		CheckRound(x);
		CheckRound(nextafter(x, 1e9));
		CheckRound(nextafter(x, -1e9));
	}
	CheckRound(0.0); CheckRound(-0.0); CheckRound(255.0); CheckRound(nextafter(255.0, 1e9)); CheckRound(0.5); CheckRound(nextafter(0.5, 0)); CheckRound(1.0); CheckRound(nextafter(1.0, 0));
	printf("roundup: %ld x 6 numbers\n", N / 4);

	/* 3. the colour steps of the light table */
	{
		long sets = N / 2000 + 40;
		unsigned char out[34 * 256];

		for (n = 0; n < sets; n++)
		{
			uint64_t r1 = Rnd64(), r2 = Rnd64();
			uint32_t rgba, fadergba;
			int fs, fe;

			if (havegame && (n & 1))
				for (i = 0; i < 256; i++) { pal[i][0] = game[i * 3]; pal[i][1] = game[i * 3 + 1]; pal[i][2] = game[i * 3 + 2]; }
			else if (n % 5 == 2)
				for (i = 0; i < 256; i++) { pal[i][0] = (unsigned char)(i & 0xE0); pal[i][1] = (unsigned char)((i << 3) & 0xE0); pal[i][2] = (unsigned char)((i << 6) & 0xC0); }
			else
				for (i = 0; i < 256; i++) { uint64_t q = Rnd64(); pal[i][0] = (unsigned char)q; pal[i][1] = (unsigned char)(q >> 8); pal[i][2] = (unsigned char)(q >> 16); }
			switch (n % 6)
			{
				case 0: rgba = 0; fadergba = 0xFF000000u; fs = 0; fe = 31; break; /* the default colormap */
				case 1: rgba = (uint32_t)r1; fadergba = (uint32_t)r2; fs = (int)(r1 >> 40) % 32; fe = (int)(r2 >> 40) % 32; break;
				case 2: rgba = (uint32_t)r1 & 0x00FFFFFFu; fadergba = 0xFF000000u; fs = 0; fe = 31; break;
				case 3: rgba = (uint32_t)r1; fadergba = (uint32_t)r2; fs = 0; fe = 31; break;
				case 4: rgba = (uint32_t)r1; fadergba = (uint32_t)r2; fs = (int)(r1 >> 40) % 16; fe = 31 - (int)(r2 >> 40) % 8; break;
				default: rgba = (uint32_t)(r1 & 0xFF000000u) | 0x00404040u; fadergba = (uint32_t)r2 & 0xFF; fs = 5; fe = 25; break;
			}
			if (LightRef(out, rgba, fadergba, fs, fe))
				handled++;
			else
				bailed++;
		}
		printf("light: %ld colormaps compared, %ld fell back to the double code\n", handled, bailed);
	}
	printf(fails ? "FAILED: %ld\n" : "ALL EQUAL (%ld failures)\n", fails);
	return fails != 0;
}
