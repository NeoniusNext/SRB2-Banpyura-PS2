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
		/* OPT11 round 2 (PS2-HW-225): HWR_GroupSort32 (few distinct keys; -1 = too many, nothing done) against the same reference */
		for (i = 0; i < n; i++)
		{
			keys[i] = orig[i];
			idx[i] = i;
		}
		{
			int gr = HWR_GroupSort32(keys, idx, tk, ti, n);

			if (gr == 0)
			{
				if (neg && n > 3)
					ti[n / 2] ^= 1;
				for (i = 0; i < n; i++)
					if (ti[i] != ref[i])
					{
						bad++;
						break;
					}
			}
			else if (mode != 0 && mode != 3)
			{
				/* few distinct keys: must not give up (modes 0 and 3 have up to 6000 / 65536 distinct keys) */
				if (mode == 1 || mode == 4)
					bad++;
			}
			cases++;
		}
		/* HWR_GroupPlan (PS2-HW-231): keys and texture identities in records of 64 bytes (offsets 8 and 12), the sign bit of the key flipped by the function. The order of the
		 * buckets: a negative hash (flipped: top bit clear) by the key; a textured polygon by (texture order, first polygon of its texture, first polygon of its key), every
		 * bucket in the order of arrival. Reference: the same by plain loops, n up to 700 */
		if (n <= 700)
		{
			static unsigned char rec[700 * 64];
			static unsigned short grp[700], border[HWR_GS_MAX];
			static unsigned int first[HWR_GS_MAX], start[HWR_GS_MAX], cnt[HWR_GS_MAX], flip[700], out[700], tid[700], kf[700], tf[700];
			static unsigned long long rk[700];
			int m;

			for (i = 0; i < n; i++)
			{
				tid[i] = rnd() % (mode == 1 ? 3u : 40u);
				*(unsigned int *)(void *)(rec + 64 * i + 8) = orig[i];
				*(unsigned int *)(void *)(rec + 64 * i + 12) = tid[i];
				flip[i] = orig[i] ^ 0x80000000u;
			}
			for (i = 0; i < n; i++)
			{
				unsigned int j;

				for (j = 0; j < n && flip[j] != flip[i]; j++)
					;
				kf[i] = j; /* first polygon of the key */
			}
			for (i = 0; i < n; i++)
			{
				unsigned int j, best = kf[i];

				for (j = 0; j < n; j++)
					if (((flip[j] >> 16) & 0x7FFF) == ((flip[i] >> 16) & 0x7FFF) && tid[kf[j]] == tid[kf[i]] && kf[j] < best)
						best = kf[j];
				tf[i] = best;
			}
			for (i = 0; i < n; i++)
			{
				/* a sort word per polygon: class, then the order of the buckets, then the order of arrival */
				if (!(flip[i] & 0x80000000u))
					rk[i] = ((unsigned long long)flip[i] << 32) | i;
				else
					rk[i] = (1ull << 63) | ((unsigned long long)((flip[i] >> 16) & 0x7FFF) << 47) | ((unsigned long long)tf[i] << 31) | ((unsigned long long)kf[i] << 15);
			}
			for (i = 0; i < n; i++)
				ref[i] = i;
			{
				unsigned int a2, b2;

				for (a2 = 1; a2 < n; a2++) /* insertion sort on (word, index): stable */
				{
					unsigned int v = ref[a2];

					for (b2 = a2; b2 > 0 && (rk[ref[b2 - 1]] > rk[v] || (rk[ref[b2 - 1]] == rk[v] && ref[b2 - 1] > v)); b2--)
						ref[b2] = ref[b2 - 1];
					ref[b2] = v;
				}
			}
			m = n ? HWR_GroupPlan(rec + 8, rec + 12, 64, n, grp, first, start, cnt, border) : 0;
			if (m >= 0)
			{
				int b, bad_here = 0;

				for (i = 0; i < n; i++)
					out[start[grp[i]]++] = i;
				for (i = 0; i < n; i++)
					if (out[i] != ref[i])
					{
						bad_here = 1;
						break;
					}
				for (b = 0; b < m && !bad_here; b++) /* first polygon and count of every bucket */
				{
					const unsigned int g = border[b];

					if (first[g] != out[start[g] - cnt[g]])
						bad_here = 1;
				}
				if (neg && n > 3)
					bad_here = 1;
				bad += bad_here;
			}
			else if (mode == 1 || mode == 4)
				bad++;
			cases++;
		}
	}
	printf("HT sort cases=%d bad=%d\n", cases, bad);
	return bad ? 1 : 0;
}
