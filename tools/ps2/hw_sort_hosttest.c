/* Host test of src/hardware/hw_sort.h: the stable radix sort of the batch polygon keys against a reference stable sort
 * (qsort with the index as tie breaker). neg=1: one result element is corrupted, the test must fail. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw_sort.h"

static unsigned int *g_keys;

static int cmp(const void *a, const void *b)
{
	unsigned int ia = *(const unsigned int *)a, ib = *(const unsigned int *)b;

	if (g_keys[ia] != g_keys[ib])
		return g_keys[ia] < g_keys[ib] ? -1 : 1;
	return ia < ib ? -1 : ia > ib;
}

static unsigned long long rs = 88172645463325252ull;
static unsigned int rnd(void)
{
	rs ^= rs << 13;
	rs ^= rs >> 7;
	rs ^= rs << 17;
	return (unsigned int)(rs >> 11);
}

int main(int argc, char **argv)
{
	int neg = argc > 1 && !strcmp(argv[1], "neg=1");
	int bad = 0, cases = 0, round;
	unsigned int n;

	for (round = 0; round < 400; round++)
	{
		static unsigned int keys[6000], idx[6000], tk[6000], ti[6000], ref[6000], orig[6000];
		unsigned int i, mode = (unsigned int)round % 5;
		int in_src;

		n = round < 20 ? (unsigned int)round : rnd() % 6000;
		for (i = 0; i < n; i++)
		{
			unsigned int k = rnd() * 2654435761u;

			if (mode == 1) k &= 0xFF; /* few distinct keys: many ties */
			else if (mode == 2) k = i / 7; /* sorted with ties */
			else if (mode == 3) k = 0x80000000u ^ (rnd() & 0xFFFF); /* the sign bit pattern of the signed hash order */
			else if (mode == 4) k = 0xFFFFFFFFu - (i % 13);
			keys[i] = k;
			idx[i] = i;
			orig[i] = k;
			ref[i] = i;
		}
		g_keys = orig;
		qsort(ref, n, sizeof ref[0], cmp);
		in_src = HWR_RadixSort32(keys, idx, tk, ti, n);
		{
			const unsigned int *ri = in_src ? idx : ti, *rk = in_src ? keys : tk;

			if (neg && n > 3)
				((unsigned int *)ri)[n / 2] ^= 1;
			for (i = 0; i < n; i++)
				if (ri[i] != ref[i] || rk[i] != orig[ref[i]])
				{
					bad++;
					break;
				}
		}
		cases++;
	}
	printf("HT sort cases=%d bad=%d\n", cases, bad);
	return bad ? 1 : 0;
}
