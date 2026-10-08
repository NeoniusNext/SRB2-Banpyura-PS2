// SONIC ROBO BLAST 2 - PS2 port
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file hw_sort.h
/// \brief Stable LSD radix sort of 32-bit keys with their indices (PS2-HW-19: replaces qsort of the batch polygons).
///        Pure code, also compiled by the host test tools/ps2/hw_sort_hosttest.py.

#ifndef __HWR_SORT_H__
#define __HWR_SORT_H__


#include <string.h>

// Sorts the parallel arrays keys[] / idx[] by key (keys[i] belongs to idx[i]); both are permuted.
// tk / ti are scratch of n entries. The sort is stable: equal keys keep their order. Four passes of 8 bits (PS2-HW-109, OPT11 VU: it was three passes of 11 bits,
// whose 2048-entry histograms cost 35 000 cycles to clear and sum whatever n was: a sprite sort of 60 entries took as much as the sort of 3000); a pass whose digit is
// the same for every key is skipped. Returns 1 when the result is in keys/idx, 0 when it is in tk/ti (an odd number of passes ran).
static inline int HWR_RadixSort32(unsigned int *keys, unsigned int *idx, unsigned int *tk, unsigned int *ti, unsigned int n)
{
	unsigned int count[4][256];
	unsigned int i, pass;
	unsigned int *sk = keys, *si = idx, *dk = tk, *di = ti;
	int in_src = 1;

	memset(count, 0, sizeof count);
	for (i = 0; i < n; i++)
	{
		unsigned int k = keys[i];

		count[0][k & 255]++;
		count[1][(k >> 8) & 255]++;
		count[2][(k >> 16) & 255]++;
		count[3][k >> 24]++;
	}
	for (pass = 0; pass < 4; pass++)
	{
		unsigned int shift = pass * 8, sum = 0, d;
		unsigned int *c = count[pass];

		if (n && c[(sk[0] >> shift) & 255] == n)
			continue; // every key has the same digit
		for (d = 0; d < 256; d++)
		{
			unsigned int t = c[d];

			c[d] = sum;
			sum += t;
		}
		for (i = 0; i < n; i++)
		{
			unsigned int pos = c[(sk[i] >> shift) & 255]++;

			dk[pos] = sk[i];
			di[pos] = si[i];
		}
		{
			unsigned int *t;

			t = sk; sk = dk; dk = t;
			t = si; si = di; di = t;
		}
		in_src = !in_src;
	}
	return in_src;
}

// PS2-HW-59: the same stable order for keys with few distinct values (the batches of a frame: ~250 distinct keys among ~3000 polygons, 70 in the skybox
// view). The polygons are counted per distinct key through a small hash table, only the distinct keys are sorted (8 bit digits), and one scatter puts every
// polygon at its place: no 2048-entry histograms to clear and sum (three of them cost 75 000 cycles even for a view of 70 polygons), one pass instead of three.
// Only the order of idx is made: it ends in ti and the function returns 0; keys / tk are scratch (tk holds the group of every polygon). Returns -1 (nothing
// done) when there are more than HWR_GS_MAX distinct keys: the caller takes HWR_RadixSort32.
#define HWR_GS_SLOTS 2048u
#define HWR_GS_MAX 700u

static inline int HWR_GroupSort32(const unsigned int *keys, const unsigned int *idx, unsigned int *tk, unsigned int *ti, unsigned int n)
{
	unsigned int hk[HWR_GS_SLOTS]; // 23 KB of stack, as much as the histograms of HWR_RadixSort32: no static memory (the zone is tight)
	unsigned short hv[HWR_GS_SLOTS]; // distinct key number + 1 of the slot, 0 = free
	unsigned int dk[HWR_GS_MAX], cnt[HWR_GS_MAX], off[HWR_GS_MAX];
	unsigned short ord[2][HWR_GS_MAX];
	unsigned short *grp = (unsigned short *)(void *)tk;
	unsigned int i, m = 0, pass, run = 0;
	unsigned int c8[256];
	unsigned int andk = ~0u, ork = 0, lastk = 0, lastg = 0;
	int cur = 0;

	memset(hv, 0, sizeof hv);
	for (i = 0; i < n; i++)
	{
		const unsigned int k = keys[i];
		unsigned int h;

		if (k == lastk && i) // OPT11 round 2 (PS2-HW-225): a run of polygons with one key (consecutive walls of a texture) skips the table
		{
			grp[i] = (unsigned short)lastg;
			cnt[lastg]++;
			continue;
		}
		h = (k * 2654435761u) >> 21;
		while (hv[h] && hk[h] != k)
			h = (h + 1) & (HWR_GS_SLOTS - 1);
		if (!hv[h])
		{
			if (m == HWR_GS_MAX)
				return -1;
			hk[h] = k;
			hv[h] = (unsigned short)(m + 1);
			dk[m] = k;
			cnt[m] = 0;
			andk &= k;
			ork |= k;
			m++;
		}
		lastk = k;
		lastg = (unsigned int)hv[h] - 1u;
		grp[i] = (unsigned short)lastg;
		cnt[lastg]++;
	}
	// the distinct keys in ascending order: LSD radix, 8 bit digits, a digit that is the same for all of them is skipped
	for (i = 0; i < m; i++)
		ord[0][i] = (unsigned short)i;
	for (pass = 0; pass < 4; pass++)
	{
		const unsigned int shift = pass * 8;
		unsigned int d, sum = 0;

		if (!(((andk ^ ork) >> shift) & 255u))
			continue; // the same digit in every distinct key: nothing to count (it cost a clear of 256 words and a pass over the keys)
		memset(c8, 0, sizeof c8);
		for (i = 0; i < m; i++)
			c8[(dk[ord[cur][i]] >> shift) & 255]++;
		if (c8[(dk[ord[cur][0]] >> shift) & 255] == m)
			continue;
		for (d = 0; d < 256; d++)
		{
			const unsigned int t = c8[d];

			c8[d] = sum;
			sum += t;
		}
		for (i = 0; i < m; i++)
		{
			const unsigned short id = ord[cur][i];

			ord[cur ^ 1][c8[(dk[id] >> shift) & 255]++] = id;
		}
		cur ^= 1;
	}
	for (i = 0; i < m; i++)
	{
		const unsigned short id = ord[cur][i];

		off[id] = run;
		run += cnt[id];
	}
	for (i = 0; i < n; i++)
		ti[off[grp[i]]++] = idx[i];
	return 0;
}

// OPT11 round 2 (PS2-HW-226, 231): the grouping half of HWR_GroupSort32 for keys that sit in an array of records (key at keys + i * stride, the signed order: the sign bit is
// flipped as HWR_RenderBatches did when it made its key array), without the key and index arrays and without the final scatter, which the caller makes together with the
// copy of the record's data. Fills, for the m distinct keys (bucket numbers in order of first appearance): first[b] = the first polygon with the key, cnt[b] = how many,
// start[b] = where the bucket begins in the sorted order, border[0..m) = the buckets in the order they are drawn; grp[i] = the bucket of polygon i. Polygons with equal keys
// keep their order (the caller scatters in the order of i). Returns m, or -1 when there are more than HWR_GS_MAX distinct keys (nothing is valid then).
// The order of the buckets (PS2-HW-231): the key of a textured polygon is (texture order 15 bits << 16) | a 16 bit hash of its state, and the hash of the state contains the
// ADDRESS of the texture (patches: the address decides the texture order too): the order of the states of one texture, and of two patches, was a function of the layout of the
// heap, and two coplanar polygons (a shadow and a floor patch, two sprites at one depth) drew in an order that changed with any change of memory. Now the buckets go by (texture
// order, the first polygon of the texture in this frame, the first polygon of the bucket): the order of arrival decides, the hash only tells the states apart. `tex` = the
// texture identity of every polygon (read like the key), only its equality matters. Keys of polygons that are not textured or are horizon (a negative hash: the polygon's own
// negated index, which sorts them in reverse order of arrival) keep the order of the key itself.
#define HWR_GP_TSLOTS 1024u

static inline int HWR_GroupPlan(const unsigned char *keys, const unsigned char *tex, unsigned int stride, unsigned int n, unsigned short *grp, unsigned int *first,
	unsigned int *start, unsigned int *cnt, unsigned short *border)
{
	unsigned int hk[HWR_GS_SLOTS];
	unsigned short hv[HWR_GS_SLOTS];
	unsigned int dk[HWR_GS_MAX];
	unsigned long long sk[HWR_GS_MAX];
	unsigned int tk[HWR_GP_TSLOTS][2];
	unsigned short tv[HWR_GP_TSLOTS];
	unsigned int i, m = 0, run = 0, gap;
	unsigned int lastk = 0, lastg = 0;
	static const unsigned int gaps[] = {701, 301, 132, 57, 23, 10, 4, 1};

	memset(hv, 0, sizeof hv);
	for (i = 0; i < n; i++)
	{
		const unsigned int k = *(const unsigned int *)(const void *)(keys + (size_t)i * stride) ^ 0x80000000u;
		unsigned int h;

		if (k == lastk && i) // a run of polygons with one key skips the table
		{
			grp[i] = (unsigned short)lastg;
			cnt[lastg]++;
			continue;
		}
		h = (k * 2654435761u) >> 21;
		while (hv[h] && hk[h] != k)
			h = (h + 1) & (HWR_GS_SLOTS - 1);
		if (!hv[h])
		{
			if (m == HWR_GS_MAX)
				return -1;
			hk[h] = k;
			hv[h] = (unsigned short)(m + 1);
			dk[m] = k;
			cnt[m] = 0;
			first[m] = i;
			m++;
		}
		lastk = k;
		lastg = (unsigned int)hv[h] - 1u;
		grp[i] = (unsigned short)lastg;
		cnt[lastg]++;
	}
	// the order of the buckets: one 64 bit word each, sorted (m is a few hundred: a shell sort)
	memset(tv, 0, sizeof tv);
	for (i = 0; i < m; i++)
	{
		const unsigned int k = dk[i];
		unsigned long long w;

		if (!(k & 0x80000000u))
			w = ((unsigned long long)(k & 0x7FFFFFFFu) << 32) | i; // a negative hash: in the order of the key, before the textured ones
		else
		{
			const unsigned int hi = (k >> 16) & 0x7FFFu, id = *(const unsigned int *)(const void *)(tex + (size_t)first[i] * stride);
			unsigned int h = ((hi * 0x9E3779B1u) ^ (id * 2654435761u)) >> 22, tf;

			while (tv[h] && (tk[h][0] != hi || tk[h][1] != id))
				h = (h + 1) & (HWR_GP_TSLOTS - 1);
			if (!tv[h]) // the buckets come in the order of their first polygon: the first of a texture is its first appearance
			{
				tk[h][0] = hi;
				tk[h][1] = id;
				tv[h] = (unsigned short)(first[i] > 0xFFFEu ? 0xFFFFu : first[i] + 1u);
			}
			tf = (unsigned int)tv[h] - 1u;
			w = (1ull << 63) | ((unsigned long long)hi << 47) | ((unsigned long long)tf << 31) | ((unsigned long long)(first[i] > 0xFFFFu ? 0xFFFFu : first[i]) << 15) | i;
		}
		sk[i] = w;
	}
	for (gap = 0; gap < sizeof gaps / sizeof gaps[0]; gap++)
	{
		const unsigned int g = gaps[gap];
		unsigned int j;

		if (g >= m)
			continue;
		for (i = g; i < m; i++)
		{
			const unsigned long long v = sk[i];

			for (j = i; j >= g && sk[j - g] > v; j -= g)
				sk[j] = sk[j - g];
			sk[j] = v;
		}
	}
	for (i = 0; i < m; i++)
	{
		const unsigned int id = (unsigned int)(sk[i] & 0x7FFFu);

		border[i] = (unsigned short)id;
		start[id] = run;
		run += cnt[id];
	}
	return (int)m;
}

#endif
