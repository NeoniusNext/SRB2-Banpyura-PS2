// SONIC ROBO BLAST 2 - PS2 port
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps2_nodescan.h
/// \brief PS2-161: compact rectangle table of the draw node list of R_CreateDrawNodes (r_things.c)
///
/// The sprite phase of R_CreateDrawNodes walks the whole node list for every sprite and, for every node, first runs a rejection test
/// (x range of sprite and node, y range for planes and sprites) and only then the real ordering test. The test never has side effects
/// and rejects ~95% of the nodes. This table keeps, in list order, one rectangle per node so the rejection runs over arrays
/// (8 nodes per step with the R5900 multimedia instructions): a node is rejected  <=>  x1 > rx2 || x2 < rx1 || y1 > rsz || y2 < rszt.
/// The caller keeps the linked list as the truth and runs the original test for every node that is not rejected (the test has no side
/// effects), then takes the claiming node that is first in the list, so the result is identical to the original loop. Tested by tools/ps2/sw_hosttest.py (host, scalar scan) and by the PS2_NODECHECK shadow
/// check on the EE.

#ifndef __PS2_NODESCAN__
#define __PS2_NODESCAN__

#include <string.h>
#include "../doomtype.h"

#ifndef PS2NS_ALLOC // r_things.c defines these to the zone, the host test uses libc
#include <stdlib.h>
#define PS2NS_ALLOC(n) malloc(n)
#define PS2NS_FREE(p) free(p)
#define PS2NS_OOM() abort()
#define PS2NS_MISMATCH(from, v, r) abort()
#endif

#define PS2NS_LIM 30000 // coordinates beyond this make the caller use the original loop
#define PS2NS_NONE_X1 32767 // a lane that rejects everything (padding, nodes of no type)
#define PS2NS_NONE_X2 (-32768)

typedef struct
{
	INT16 *x1, *x2, *y1, *y2; // [capacity], 16-byte aligned, creation order
	void **node;              // [capacity]
	INT32 count, capacity;    // capacity is a multiple of 8; lanes >= count reject everything
	void *mem;
} ps2_nodescan_t;

static inline void PS2NS_FillNone(ps2_nodescan_t *ns, INT32 from, INT32 to)
{
	INT32 i;
	for (i = from; i < to; i++)
	{
		ns->x1[i] = PS2NS_NONE_X1;
		ns->x2[i] = PS2NS_NONE_X2;
		ns->y1[i] = PS2NS_NONE_X1;
		ns->y2[i] = PS2NS_NONE_X2;
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
		const size_t bytes = (size_t)cap * (4 * sizeof(INT16) + sizeof(void *)) + 32;
		unsigned char *mem = (unsigned char *)PS2NS_ALLOC(bytes);
		ps2_nodescan_t n;
		unsigned char *p;

		if (!mem)
			PS2NS_OOM();
		p = (unsigned char *)(((uintptr_t)mem + 15) & ~(uintptr_t)15);
		n.x1 = (INT16 *)p;
		n.x2 = n.x1 + cap;
		n.y1 = n.x2 + cap;
		n.y2 = n.y1 + cap;
		n.node = (void **)(n.y2 + cap);
		n.count = ns->count;
		n.capacity = cap;
		n.mem = mem;
		PS2NS_FillNone(&n, 0, cap);
		if (ns->count)
		{
			memcpy(n.x1, ns->x1, (size_t)ns->count * sizeof(INT16));
			memcpy(n.x2, ns->x2, (size_t)ns->count * sizeof(INT16));
			memcpy(n.y1, ns->y1, (size_t)ns->count * sizeof(INT16));
			memcpy(n.y2, ns->y2, (size_t)ns->count * sizeof(INT16));
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
	PS2NS_Reserve(ns, at + 1);
	ns->x1[at] = (INT16)x1;
	ns->x2[at] = (INT16)x2;
	ns->y1[at] = (INT16)y1;
	ns->y2[at] = (INT16)y2;
	ns->node[at] = node;
	ns->count++;
}

static inline boolean PS2NS_Passes(const ps2_nodescan_t *ns, INT32 i, INT32 rx1, INT32 rx2, INT32 rszt, INT32 rsz)
{
#if defined(PS2_NEGCTL) && PS2_NEGCTL == 7 // negative control of the host A/B (tools/ps2/host_ab.sh): off by one in the x test
	return !((ns->x1[i] >= rx2) | (ns->x2[i] < rx1) | (ns->y1[i] > rsz) | (ns->y2[i] < rszt));
#else
	return !((ns->x1[i] > rx2) | (ns->x2[i] < rx1) | (ns->y1[i] > rsz) | (ns->y2[i] < rszt));
#endif
}

// first index >= from whose rectangle is not rejected, or count (the reference scalar scan)
static inline INT32 PS2NS_NextScalar(const ps2_nodescan_t *ns, INT32 from, INT32 rx1, INT32 rx2, INT32 rszt, INT32 rsz)
{
	INT32 i;
	for (i = from; i < ns->count; i++)
		if (PS2NS_Passes(ns, i, rx1, rx2, rszt, rsz))
			return i;
	return ns->count;
}

#if defined(_EE) && defined(__GNUC__) && !defined(PS2NS_SCALAR)
// 8 nodes per step: PCGTH compares eight signed halfwords at once. Everything stays inside one asm block (the registers are 128 bit
// wide, the compiler only knows their low halves); $8-$15 style fixed registers as in ps2_audio.c would also do, here the compiler picks
// scratch registers through dummy 64-bit outputs.
static inline INT32 PS2NS_NextVec(const ps2_nodescan_t *ns, INT32 from, INT32 rx1, INT32 rx2, INT32 rszt, INT32 rsz)
{
	INT32 i = from;
	// scalar until the group boundary
	while ((i & 7) && i < ns->count)
	{
		if (PS2NS_Passes(ns, i, rx1, rx2, rszt, rsz))
			return i;
		i++;
	}
	if (i >= ns->count)
		return ns->count;
	{
		const UINT32 w1 = ((UINT32)rx1 & 0xFFFFu) * 0x10001u, w2 = ((UINT32)rx2 & 0xFFFFu) * 0x10001u;
		const UINT32 w3 = ((UINT32)rsz & 0xFFFFu) * 0x10001u, w4 = ((UINT32)rszt & 0xFFFFu) * 0x10001u;
		const INT16 *a = ns->x1 + i, *b = ns->x2 + i, *c = ns->y1 + i, *d = ns->y2 + i;
		const INT16 *aend = ns->x1 + ns->count; // groups starting below this are scanned (padding lanes reject)
		unsigned long long vrx1, vrx2, vrsz, vrszt, t0, t1, t2, t3, m;
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
			"lq %[t0],0(%[a])\n\t"
			"lq %[t1],0(%[b])\n\t"
			"lq %[t2],0(%[c])\n\t"
			"lq %[t3],0(%[d])\n\t"
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
			"addiu %[a],%[a],16\n\t"
			"addiu %[b],%[b],16\n\t"
			"addiu %[c],%[c],16\n\t"
			"addiu %[d],%[d],16\n\t"
			"sltu %[m],%[a],%[aend]\n\t"
			"bnez %[m],1b\n\t"
			"2:\n\t"
			: [a] "+r"(a), [b] "+r"(b), [c] "+r"(c), [d] "+r"(d), [vrx1] "=&r"(vrx1), [vrx2] "=&r"(vrx2), [vrsz] "=&r"(vrsz),
			  [vrszt] "=&r"(vrszt), [t0] "=&r"(t0), [t1] "=&r"(t1), [t2] "=&r"(t2), [t3] "=&r"(t3), [m] "=&r"(m)
			: [w1] "r"(w1), [w2] "r"(w2), [w3] "r"(w3), [w4] "r"(w4), [aend] "r"(aend)
			: "memory");
		i = (INT32)(a - ns->x1);
		// the group at i holds a passing lane unless the scan ran off the end
		for (; i < ns->count; i++)
			if (PS2NS_Passes(ns, i, rx1, rx2, rszt, rsz))
				return i;
		return ns->count;
	}
}
#ifdef PS2_NODECHECK // shadow check on the EE: every vector scan is compared with the scalar reference, a difference stops the game
static inline INT32 PS2NS_Next(const ps2_nodescan_t *ns, INT32 from, INT32 rx1, INT32 rx2, INT32 rszt, INT32 rsz)
{
	const INT32 v = PS2NS_NextVec(ns, from, rx1, rx2, rszt, rsz), r = PS2NS_NextScalar(ns, from, rx1, rx2, rszt, rsz);
	if (v != r)
		PS2NS_MISMATCH(from, v, r);
	return v;
}
#else
#define PS2NS_Next PS2NS_NextVec
#endif
#else
#define PS2NS_Next PS2NS_NextScalar
#endif

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
