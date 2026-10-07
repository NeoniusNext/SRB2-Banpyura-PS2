// SONIC ROBO BLAST 2 - PS2 port
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps2_nodescan.h
/// \brief PS2-161: compact rectangle table of the draw node list of R_CreateDrawNodes (r_things.c), and the stable sprite sort
///
/// The sprite phase of R_CreateDrawNodes walks the whole node list for every sprite and, for every node, first runs a rejection test
/// (x range of sprite and node, y range for planes and sprites) and only then the real ordering test. The test never has side effects
/// and rejects ~95% of the nodes. This table keeps one rectangle per node so the rejection runs over arrays, 8 nodes per step with the
/// R5900 multimedia instructions: a node is rejected  <=>  x1 > rx2 || x2 < rx1 || y1 > rsz || y2 < rszt.
/// The caller keeps the linked list as the truth and runs the original test for every node that is not rejected (the test has no side
/// effects), then takes the claiming node that is first in the list, so the result is identical to the original loop.
///
/// Layout: groups of 8 nodes, one 64-byte block per group: x1[8], x2[8], y1[8], y2[8] (INT16). Lanes beyond the node count reject
/// everything. The table is in creation order (appending never moves anything).
/// Tested by tools/ps2/sw_hosttest.py (host: portable kernel against the scalar reference) and, for the MMI kernel, by the PS2_NODECHECK
/// shadow check on the EE plus the four-demo golden comparison.

#ifndef __PS2_NODESCAN__
#define __PS2_NODESCAN__

#include <string.h>
#include "../doomtype.h"

#ifndef PS2NS_ALLOC // r_things.c defines these to the zone, the host test uses libc
#include <stdlib.h>
#define PS2NS_ALLOC(n) malloc(n)
#define PS2NS_FREE(p) free(p)
#define PS2NS_OOM() abort()
#define PS2NS_MISMATCH() abort()
#endif

#define PS2NS_LIM 30000 // coordinates beyond this make the caller use the original loop
#define PS2NS_NONE_X1 32767 // a lane that rejects everything (padding, nodes of no type)
#define PS2NS_NONE_X2 (-32768)

typedef struct
{
	INT16 *blk;            // [capacity / 8][32], 64-byte aligned
	void **node;           // [capacity]
	INT32 count, capacity; // capacity is a multiple of 8, > count + 7: the group after the last node exists and rejects everything
	void *mem;
} ps2_nodescan_t;

static inline void PS2NS_FillNone(ps2_nodescan_t *ns, INT32 from, INT32 to)
{
	INT32 i;
	for (i = from; i < to; i++)
	{
		INT16 *g = ns->blk + (size_t)(i >> 3) * 32;
		const INT32 l = i & 7;
		g[l] = PS2NS_NONE_X1;
		g[8 + l] = PS2NS_NONE_X2;
		g[16 + l] = PS2NS_NONE_X1;
		g[24 + l] = PS2NS_NONE_X2;
		ns->node[i] = NULL;
	}
}

static inline void PS2NS_Reset(ps2_nodescan_t *ns)
{
	if (ns->capacity)
		PS2NS_FillNone(ns, 0, ns->count + 8 < ns->capacity ? ns->count + 8 : ns->capacity);
	ns->count = 0;
}

// room for at least need entries (plus one group of padding)
static inline void PS2NS_Reserve(ps2_nodescan_t *ns, INT32 need)
{
	if (need + 8 <= ns->capacity)
		return;
	{
		const INT32 cap = (INT32)(((size_t)(need + 8) * 3 / 2 + 7) & ~(size_t)7);
		const size_t bytes = (size_t)(cap >> 3) * 64 + (size_t)cap * sizeof(void *) + 64;
		unsigned char *mem = (unsigned char *)PS2NS_ALLOC(bytes);
		ps2_nodescan_t n;
		unsigned char *p;

		if (!mem)
			PS2NS_OOM();
		p = (unsigned char *)(((uintptr_t)mem + 63) & ~(uintptr_t)63);
		n.blk = (INT16 *)p;
		n.node = (void **)(p + (size_t)(cap >> 3) * 64);
		n.count = ns->count;
		n.capacity = cap;
		n.mem = mem;
		PS2NS_FillNone(&n, 0, cap);
		if (ns->count)
		{
			memcpy(n.blk, ns->blk, (size_t)((ns->count + 7) >> 3) * 64);
			memcpy(n.node, ns->node, (size_t)ns->count * sizeof(void *));
		}
		if (ns->mem)
			PS2NS_FREE(ns->mem);
		*ns = n;
	}
}

// append a rectangle: the table is in creation order, not in list order (the caller orders by labels), so nothing ever shifts
static inline void PS2NS_Append(ps2_nodescan_t *ns, void *node, INT32 x1, INT32 x2, INT32 y1, INT32 y2)
{
	const INT32 at = ns->count;
	INT16 *g;
	PS2NS_Reserve(ns, at + 1);
	g = ns->blk + (size_t)(at >> 3) * 32;
	g[at & 7] = (INT16)x1;
	g[8 + (at & 7)] = (INT16)x2;
	g[16 + (at & 7)] = (INT16)y1;
	g[24 + (at & 7)] = (INT16)y2;
	ns->node[at] = node;
	ns->count++;
}

static inline boolean PS2NS_Passes(const ps2_nodescan_t *ns, INT32 i, INT32 rx1, INT32 rx2, INT32 rszt, INT32 rsz)
{
	const INT16 *g = ns->blk + (size_t)(i >> 3) * 32;
	const INT32 l = i & 7;
#if defined(PS2_NEGCTL) && PS2_NEGCTL == 7 // negative control of the host A/B (tools/ps2/host_ab.sh): off by one in the x test
	return !((g[l] >= rx2) | (g[8 + l] < rx1) | (g[16 + l] > rsz) | (g[24 + l] < rszt));
#else
	return !((g[l] > rx2) | (g[8 + l] < rx1) | (g[16 + l] > rsz) | (g[24 + l] < rszt));
#endif
}

// number of groups to scan (the last one may be partial: its missing lanes reject)
#define PS2NS_GROUPS(ns) (((ns)->count + 7) >> 3)

// Portable kernel and reference: the first group >= g that has a passing lane, left in *g (== groups when there is none); the result has
// 0xFF in byte k when lane k of that group passes (the layout the MMI kernel produces).
static inline UINT64 PS2NS_NextGroupScalar(const ps2_nodescan_t *ns, INT32 *g, INT32 groups, INT32 rx1, INT32 rx2, INT32 rszt, INT32 rsz)
{
	INT32 i = *g, l;
	for (; i < groups; i++)
	{
		UINT64 m = 0;
		for (l = 0; l < 8; l++)
			if (PS2NS_Passes(ns, i * 8 + l, rx1, rx2, rszt, rsz))
				m |= (UINT64)0xFF << (8 * l);
		if (m)
		{
			*g = i;
			return m;
		}
	}
	*g = groups;
	return 0;
}

#if defined(_EE) && defined(__GNUC__) && !defined(PS2NS_SCALAR)
// PCGTH compares eight signed halfwords at once; one 64-byte block per group, two groups per loop pass. Everything stays inside one asm
// block (the registers are 128 bits wide, the compiler only knows their low halves); the compiler picks the scratch registers through
// dummy 64-bit outputs, the same way ps2_audio.c uses fixed ones.
static inline UINT64 PS2NS_NextGroupVec(const ps2_nodescan_t *ns, INT32 *g, INT32 groups, INT32 rx1, INT32 rx2, INT32 rszt, INT32 rsz)
{
	const UINT32 w1 = ((UINT32)rx1 & 0xFFFFu) * 0x10001u, w2 = ((UINT32)rx2 & 0xFFFFu) * 0x10001u;
	const UINT32 w3 = ((UINT32)rsz & 0xFFFFu) * 0x10001u, w4 = ((UINT32)rszt & 0xFFFFu) * 0x10001u;
	const INT16 *p = ns->blk + (size_t)*g * 32, *const pend = ns->blk + (size_t)groups * 32;
	unsigned long long vrx1, vrx2, vrsz, vrszt, t0, t1, t2, t3, m, hit = 0;

	if (*g >= groups)
		return 0;
	__asm__ volatile(
		"pextlw %[vrx1],%[w1],%[w1]\n\t"
		"pcpyld %[vrx1],%[vrx1],%[vrx1]\n\t"
		"pextlw %[vrx2],%[w2],%[w2]\n\t"
		"pcpyld %[vrx2],%[vrx2],%[vrx2]\n\t"
		"pextlw %[vrsz],%[w3],%[w3]\n\t"
		"pcpyld %[vrsz],%[vrsz],%[vrsz]\n\t"
		"pextlw %[vrszt],%[w4],%[w4]\n\t"
		"pcpyld %[vrszt],%[vrszt],%[vrszt]\n\t"
		"1:\n\t"
		"lq %[t0],0(%[p])\n\t"
		"lq %[t1],16(%[p])\n\t"
		"lq %[t2],32(%[p])\n\t"
		"lq %[t3],48(%[p])\n\t"
		"pcgth %[t0],%[t0],%[vrx2]\n\t"   // x1 > rx2
		"pcgth %[t1],%[vrx1],%[t1]\n\t"   // rx1 > x2
		"pcgth %[t2],%[t2],%[vrsz]\n\t"   // y1 > rsz
		"pcgth %[t3],%[vrszt],%[t3]\n\t"  // rszt > y2
		"por %[t0],%[t0],%[t1]\n\t"
		"por %[t2],%[t2],%[t3]\n\t"
		"por %[t0],%[t0],%[t2]\n\t"
		"pnor %[t0],%[t0],%[t0]\n\t"      // lanes that pass are all ones
		"pcpyud %[t1],%[t0],%[t0]\n\t"
		"or %[m],%[t0],%[t1]\n\t"
		"bnez %[m],2f\n\t"
		"addiu %[p],%[p],64\n\t"
		"sltu %[m],%[p],%[pend]\n\t"
		"bnez %[m],1b\n\t"
		"b 3f\n\t"
		"2:\n\t"
		"ppacb %[t1],%[t0],%[t0]\n\t"      // one byte per lane: 0xFF where the lane passes
		"or %[hit],%[t1],$0\n\t"           // low 64 bits to a general register
		"3:\n\t"
		: [p] "+r"(p), [vrx1] "=&r"(vrx1), [vrx2] "=&r"(vrx2), [vrsz] "=&r"(vrsz), [vrszt] "=&r"(vrszt), [t0] "=&r"(t0), [t1] "=&r"(t1),
		  [t2] "=&r"(t2), [t3] "=&r"(t3), [m] "=&r"(m), [hit] "+r"(hit)
		: [w1] "r"(w1), [w2] "r"(w2), [w3] "r"(w3), [w4] "r"(w4), [pend] "r"(pend)
		: "memory");
	*g = (INT32)((p - ns->blk) >> 5);
	return hit;
}
#ifdef PS2_NODECHECK // shadow check on the EE: every vector scan is compared with the scalar reference, a difference stops the game
static inline UINT64 PS2NS_NextGroup(const ps2_nodescan_t *ns, INT32 *g, INT32 groups, INT32 rx1, INT32 rx2, INT32 rszt, INT32 rsz)
{
	INT32 gv = *g, gs = *g;
	const UINT64 v = PS2NS_NextGroupVec(ns, &gv, groups, rx1, rx2, rszt, rsz), s = PS2NS_NextGroupScalar(ns, &gs, groups, rx1, rx2, rszt, rsz);
	if (gv != gs || v != s)
		PS2NS_MISMATCH();
	*g = gv;
	return v;
}
#else
#define PS2NS_NextGroup PS2NS_NextGroupVec
#endif
#else
#define PS2NS_NextGroup PS2NS_NextGroupScalar
#endif

// index of the lowest passing lane of a mask (bytes 0xFF/0x00) and removal of that lane; m != 0
static inline INT32 PS2NS_PopLane(UINT64 *m)
{
	const UINT64 low = *m & (0 - *m); // lowest set bit: bit 8k of byte k
	const UINT32 lo = (UINT32)low, hi = (UINT32)(low >> 32);
	INT32 bit;
#if defined(_EE) && defined(__GNUC__)
	UINT32 r;
	if (lo)
	{
		const UINT32 x = lo >> 1;
		__asm__("plzcw %0,%1" : "=r"(r) : "r"(x));
		bit = 31 - (INT32)(r & 0xFF);
	}
	else
	{
		const UINT32 x = hi >> 1;
		__asm__("plzcw %0,%1" : "=r"(r) : "r"(x));
		bit = 63 - (INT32)(r & 0xFF);
	}
#else
	bit = lo ? 31 - __builtin_clz(lo) : 63 - __builtin_clz(hi);
#endif
	*m &= ~((low << 8) - low); // clear the byte (0xFF << 8k; for k = 7 the shift wraps to 0 and 0 - low is 0xFF00..00)
	return bit >> 3;
}

// ---- R_SortVisSprites (r_things.c): stable ascending sort by (s, d) ----
typedef struct { INT32 s; INT32 d; void *p; } ps2_vsortitem_t;

// bottom-up merge sort, insertion sort for the first runs of 8; equal keys keep their order (the original selection sort takes the first minimum)
static inline void PS2_StableSortVis(ps2_vsortitem_t *a, ps2_vsortitem_t *tmp, size_t n)
{
	size_t i, j, width;
	ps2_vsortitem_t *src = a, *dst = tmp;

#if defined(PS2_NEGCTL) && PS2_NEGCTL == 8 // negative control of the host A/B: equal keys are reordered (unstable)
#define VS_LESS(x, y) ((x).s < (y).s || ((x).s == (y).s && (x).d <= (y).d))
#else
#define VS_LESS(x, y) ((x).s < (y).s || ((x).s == (y).s && (x).d < (y).d))
#endif
	for (i = 0; i < n; i += 8)
	{
		const size_t end = i + 8 < n ? i + 8 : n;
		for (j = i + 1; j < end; j++)
		{
			const ps2_vsortitem_t v = a[j];
			size_t k = j;
			while (k > i && VS_LESS(v, a[k - 1]))
			{
				a[k] = a[k - 1];
				k--;
			}
			a[k] = v;
		}
	}
	for (width = 8; width < n; width <<= 1)
	{
		for (i = 0; i < n; i += 2 * width)
		{
			const size_t mid = i + width < n ? i + width : n, hi = i + 2 * width < n ? i + 2 * width : n;
			size_t l = i, r = mid, o = i;
			while (l < mid && r < hi)
			{
				if (VS_LESS(src[r], src[l])) // strictly less: equal keys keep the left (earlier) element first
					dst[o++] = src[r++];
				else
					dst[o++] = src[l++];
			}
			while (l < mid)
				dst[o++] = src[l++];
			while (r < hi)
				dst[o++] = src[r++];
		}
		{
			ps2_vsortitem_t *t = src;
			src = dst;
			dst = t;
		}
	}
#undef VS_LESS
	if (src != a)
		memcpy(a, src, n * sizeof (*a));
}

#endif
