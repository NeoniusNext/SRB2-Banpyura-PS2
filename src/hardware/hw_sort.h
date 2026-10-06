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
// tk / ti are scratch of n entries. The sort is stable: equal keys keep their order. Three passes of 11 bits; a pass whose digit is
// the same for every key is skipped. Returns 1 when the result is in keys/idx, 0 when it is in tk/ti (an odd number of passes ran).
static inline int HWR_RadixSort32(unsigned int *keys, unsigned int *idx, unsigned int *tk, unsigned int *ti, unsigned int n)
{
	unsigned int count[3][2048];
	unsigned int i, pass;
	unsigned int *sk = keys, *si = idx, *dk = tk, *di = ti;
	int in_src = 1;

	memset(count, 0, sizeof count);
	for (i = 0; i < n; i++)
	{
		unsigned int k = keys[i];

		count[0][k & 2047]++;
		count[1][(k >> 11) & 2047]++;
		count[2][k >> 22]++;
	}
	for (pass = 0; pass < 3; pass++)
	{
		unsigned int shift = pass * 11, sum = 0, d;
		unsigned int *c = count[pass];

		if (n && c[(sk[0] >> shift) & 2047] == n)
			continue; // every key has the same digit
		for (d = 0; d < 2048; d++)
		{
			unsigned int t = c[d];

			c[d] = sum;
			sum += t;
		}
		for (i = 0; i < n; i++)
		{
			unsigned int pos = c[(sk[i] >> shift) & 2047]++;

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

#endif
