// SRB2 PS2 port: zone arena (D4) and memory reporting. See ps2_mem.h.

#include "doomdef.h"
#include "i_system.h"
#include "command.h"
#include "m_argv.h"
#include "z_zone.h"
#include "ps2_mem.h"
#ifdef _EE
#include "ps2_boot.h"
#include "ps2_net.h"
#endif
#ifdef _EE
#include "doomstat.h"
#include "r_defs.h"
#include "r_state.h"
#include "p_mobj.h"
#include "p_tick.h"
#include "r_plane.h"
#include "r_things.h"
#endif

#ifdef _EE
#include <malloc.h>
#include <unistd.h>
#include <kernel.h>
#include <timer.h>
extern char _stack_size[]; // linker symbol: value of -Wl,--defsym,_stack_size
#endif

#define ZA_NBINS 128

typedef struct zafree_s
{
	uint32_t sf;
	struct zafree_s *next, *prev;
} zafree_t;

int za_twosided = 1;
// PS2-62: 0 = every free block that fits is considered and the one nearest to the wanted end wins (address-ordered fit: temporaries
// and growth holes are reused before the arena's contiguous middle is touched). The previous default (16: best-fit bin first, address among
// its first 16 blocks) left 1.4 MB of 16..80 KB holes inside the long-lived end of the arena on MAP11 and OOM'd on a 512 KB request.
int za_prefer = 0;
// PS2-74: a request of at most za_smallfit bytes takes the smallest free block that fits (the first non-empty size class from its own upwards;
// within the class the one nearest to the wanted end). Everything above stays address-ordered. Mobjs, sector nodes and cache descriptors are
// small, long-lived and arrive all through play: carved from the end of the largest hole they pinned every hole evicted caches left (MAP11: 12 holes
// of 16..72 KB each with a 48-byte block behind it); a best fit fills the small holes first and leaves the large ones whole.
int za_smallfit = 1024;
#ifdef ZDEBUG
int za_redzone = 1;
#else
int za_redzone = 0;
#endif

static uint8_t *za_base, *za_end;
static uint8_t *za_front; // PS2-75: see ZA_SetFrontier
static size_t za_size;
static zafree_t *za_bin[ZA_NBINS];
static uint32_t za_map[ZA_NBINS / 32];

static size_t za_used, za_peak, za_gpeak, za_freecount, za_usedcount;
static size_t za_nalloc, za_nfree, za_nfail, za_nsearch, za_nevict, za_evictbytes;

// ---------------------------------------------------------------------------------------------
// Free lists
// ---------------------------------------------------------------------------------------------

static unsigned ZA_BinOf(size_t size)
{
	size_t u = size >> 4;
	unsigned l, bin;

	if (u <= 32)
		return (unsigned)u;
#ifdef __GNUC__
	l = 31u - (unsigned)__builtin_clz((unsigned)u);
#else
	for (l = 5; u >> (l + 1); l++)
		;
#endif
	bin = 33u + (l - 5u) * 4u + (unsigned)((u >> (l - 2u)) & 3u);
	return bin < ZA_NBINS ? bin : ZA_NBINS - 1;
}

static void ZA_SetFooter(uint8_t *start, size_t size)
{
	*(uint32_t *)(start + size - 4) = (uint32_t)size;
}

static void ZA_ListAdd(zafree_t *f, size_t size)
{
	unsigned bin = ZA_BinOf(size);

	f->prev = NULL;
	f->next = za_bin[bin];
	if (f->next)
		f->next->prev = f;
	za_bin[bin] = f;
	za_map[bin >> 5] |= 1u << (bin & 31);
	ZA_SetFooter((uint8_t *)f, size);
	za_freecount++;
}

static void ZA_ListDel(zafree_t *f, size_t size)
{
	unsigned bin = ZA_BinOf(size);

	if (f->prev)
		f->prev->next = f->next;
	else
		za_bin[bin] = f->next;
	if (f->next)
		f->next->prev = f->prev;
	if (!za_bin[bin])
		za_map[bin >> 5] &= ~(1u << (bin & 31));
	za_freecount--;
}

// first non-empty bin >= from, or ZA_NBINS
static unsigned ZA_NextBin(unsigned from)
{
	while (from < ZA_NBINS)
	{
		uint32_t w = za_map[from >> 5] >> (from & 31);
		if (w)
		{
			while (!(w & 1u))
			{
				w >>= 1;
				from++;
			}
			return from;
		}
		from = (from | 31u) + 1u;
	}
	return ZA_NBINS;
}

// A free block [start, start+size); `prevfree` is set only for the arena's first block (never, by coalescing).
static void ZA_MakeFree(uint8_t *start, size_t size, uint32_t prevfree)
{
	zafree_t *f = (zafree_t *)start;

	f->sf = (uint32_t)size | ZAF_VALID | prevfree;
	ZA_ListAdd(f, size);
}

// ---------------------------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------------------------

int ZA_InitMem(void *base, size_t bytes)
{
	size_t i;

	if (!base || ((uintptr_t)base & 63) || bytes < 256 || bytes > 0x7FFFFFF0u)
		return -1;
	bytes &= ~(size_t)15;
	for (i = 0; i < ZA_NBINS; i++)
		za_bin[i] = NULL;
	for (i = 0; i < ZA_NBINS / 32; i++)
		za_map[i] = 0;
	za_base = base;
	za_front = NULL;
	za_end = za_base + bytes;
	za_size = bytes;
	za_used = za_peak = za_gpeak = za_freecount = za_usedcount = 0;
	za_nalloc = za_nfree = za_nfail = za_nsearch = za_nevict = za_evictbytes = 0;
	ZA_MakeFree(za_base, bytes, 0);
	return 0;
}

size_t ZA_InitHeap(size_t reserve, size_t cap)
{
	extern size_t PS2_HeapCapacity(void);
	size_t capacity = PS2_HeapCapacity(), size;
	unsigned tries;
	void *mem = NULL;

	if (capacity <= reserve)
		return 0;
	// Leave room for newlib metadata/alignment and its heap growth granularity as well as the reserve.
	if (capacity - reserve <= 65536)
		return 0;
	size = (capacity - reserve - 65536) & ~(size_t)4095;
	if (cap && size > cap)
		size = cap & ~(size_t)4095;
	// A loader/libc can impose a smaller limit than the kernel. Bounded geometric backoff
	// reaches a usable arena even when that mismatch is much larger than four MiB.
	for (tries = 0; tries < 64 && size > 65536; tries++)
	{
		size_t step;
		mem = memalign(64, size);
		if (mem)
			break;
		step = (size / 8) & ~(size_t)4095;
		if (step < 65536)
			step = 65536;
		size -= step;
	}
	if (!mem || ZA_InitMem(mem, size))
		return 0;
#ifdef _EE
	PS2Spill_Init();
#endif
	return size;
}

int ZA_Ready(void)
{
	return za_base != NULL;
}

// PS2-79: is this payload address inside the arena (src/ps2/ps2_spill.c routes free/realloc of a spilled C-heap block by it)
int ZA_Contains(const void *p)
{
	return za_base && (const uint8_t *)p >= za_base && (const uint8_t *)p < za_end;
}

size_t ZA_PayloadBytes(const void *payload)
{
	return ZA_BLOCK(payload)->realsize;
}

void ZA_Shutdown(void)
{
	za_base = za_end = NULL;
	za_front = NULL;
	za_size = 0;
}

// PS2-75: the frontier divides the arena in two zones for the two-sided policy: requests for the short-lived side (ZA_BOTTOM: caches, renderer
// work) are placed below it, requests for the long-lived side (ZA_TOP) above it. NULL = no zones. It only steers new placements: blocks stay where
// they are, and z_zone.c moves it (evicting caches next to it) when one zone runs out while the other has room.
void ZA_SetFrontier(void *addr)
{
	uint8_t *a = addr;

	if (!a || a <= za_base || a >= za_end)
	{
		za_front = NULL;
		return;
	}
	za_front = (uint8_t *)((uintptr_t)a & ~(uintptr_t)15);
}

void *ZA_Frontier(void)
{
	return za_front;
}

zablock_t *ZA_PrevFree(zablock_t *b)
{
	if (!(b->sf & ZAF_PREVFREE))
		return NULL;
	return (zablock_t *)((uint8_t *)b - *(uint32_t *)((uint8_t *)b - 4));
}

// ---------------------------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------------------------

static size_t ZA_Need(size_t size)
{
	size_t n = ZA_HDR + size + (za_redzone ? ZA_GUARD_MIN : 0);

	n = (n + 15) & ~(size_t)15;
	return n < ZA_MINBLK ? ZA_MINBLK : n;
}

typedef struct
{
	uint8_t *start;
	size_t size;
} zaplace_t;

// Where a block of `need` bytes with an `align`-aligned payload goes inside the free block [f, fend). The pieces
// left over must be empty or at least ZA_MINBLK (they become free blocks of their own); a tail that is too small
// is absorbed by the block, a leading gap that is too small makes the candidate invalid.
static int ZA_Place(uint8_t *f, uint8_t *fend, size_t need, size_t align, int top, uint8_t *lo, uint8_t *hi, zaplace_t *out)
{
	uintptr_t low, high, p;
	int attempt;
	uintptr_t cand[4];
	int n = 0; // candidates are tried in order: unsigned wrap-around is rejected below
	uint8_t *wlo = f > lo ? f : lo, *whi = fend < hi ? fend : hi; // the part of the free block inside the window [lo, hi)

	if (whi <= wlo || (size_t)(whi - wlo) < need)
		return 0;
	low = ((uintptr_t)wlo + ZA_HDR + align - 1) & ~(uintptr_t)(align - 1);
	if (top)
	{
		high = ((uintptr_t)whi - need + ZA_HDR) & ~(uintptr_t)(align - 1);
		cand[n++] = high;
		cand[n++] = high - align;
	}
	cand[n++] = low;
	cand[n++] = low + align;

	for (attempt = 0; attempt < n; attempt++)
	{
		uint8_t *s, *e;
		size_t lead, tail;

		p = cand[attempt];
		if (p < (uintptr_t)wlo + ZA_HDR || p - ZA_HDR > (uintptr_t)(whi - need))
			continue; // (the second test also catches a candidate that wrapped around below zero)
		s = (uint8_t *)(p - ZA_HDR);
		e = s + need;
		lead = (size_t)(s - f);
		tail = (size_t)(fend - e);
		if (lead && lead < ZA_MINBLK)
			continue;
		if (tail && tail < ZA_MINBLK)
			e = fend; // too small to stand alone: the block keeps it
		out->start = s;
		out->size = (size_t)(e - s);
		return 1;
	}
	return 0;
}

void *ZA_Alloc(size_t size, size_t align, int side)
{
	size_t need;
	unsigned bin;
	zafree_t *best = NULL;
	zaplace_t bestplace = {0}, place;
	int fits = 0;
	int top = za_twosided && side == ZA_TOP;
	uint8_t *lo = za_base, *hi = za_end; // PS2-75: with a frontier the short-lived side stays below it, the long-lived side above
	uint8_t *f, *fend, *s, *e;
	size_t lead, tail;
	zablock_t *b;
	uint32_t pf;

	if (!za_base || size > za_size || (align & (align - 1)) || !align)
	{
		za_nfail++;
		return NULL;
	}
	if (align < 16)
		align = 16;
	need = ZA_Need(size);
	if (need > za_size)
	{
		za_nfail++;
		return NULL;
	}
	if (za_front && za_twosided)
	{
		if (top)
			lo = za_front;
		else
			hi = za_front;
	}

	// blocks in a higher bin than need's are all big enough by size; alignment is checked per block
	for (bin = ZA_BinOf(need); (bin = ZA_NextBin(bin)) < ZA_NBINS; bin++)
	{
		zafree_t *c;

		for (c = za_bin[bin]; c; c = c->next)
		{
			// address-ordered fit: a block that cannot beat the best one found by address is not worth the placement test
			if (best && za_prefer <= 0 && (top ? (uint8_t *)c <= (uint8_t *)best : (uint8_t *)c >= (uint8_t *)best))
				continue;
			za_nsearch++;
			if ((c->sf & ~ZAF_MASK) < need || !ZA_Place((uint8_t *)c, (uint8_t *)c + (c->sf & ~ZAF_MASK), need, align, top, lo, hi, &place))
				continue;
			if (!best || (top ? (uint8_t *)c > (uint8_t *)best : (uint8_t *)c < (uint8_t *)best))
			{
				best = c;
				bestplace = place;
			}
			fits++;
			if (za_prefer > 0 && fits >= (za_prefer > 1 ? za_prefer : 1))
				break;
		}
		if (best && (za_prefer > 0 || need <= (size_t)za_smallfit))
			break; // PS2-74: a small request stops at the first size class that has a fit: the smallest hole
	}
	if (!best)
	{
		za_nfail++;
		return NULL;
	}

	f = (uint8_t *)best;
	fend = f + (best->sf & ~ZAF_MASK);
	pf = best->sf & ZAF_PREVFREE;
	ZA_ListDel(best, (size_t)(fend - f));
	s = bestplace.start;
	e = s + bestplace.size;
	lead = (size_t)(s - f);
	tail = (size_t)(fend - e);

	if (lead)
		ZA_MakeFree(f, lead, pf);
	b = (zablock_t *)s;
	b->sf = (uint32_t)bestplace.size | ZAF_USED | ZAF_VALID | (lead ? ZAF_PREVFREE : pf);
	b->user = NULL;
	b->realsize = (uint32_t)size;
	b->tagstamp = 0;
	if (tail)
		ZA_MakeFree(e, tail, 0);
	else if (fend < za_end)
		((zablock_t *)fend)->sf &= ~ZAF_PREVFREE;

	if (za_redzone)
	{
		b->sf |= ZAF_REDZONE;
		memset((uint8_t *)ZA_PAYLOAD(b) + size, ZA_GUARD, (size_t)(e - ((uint8_t *)ZA_PAYLOAD(b) + size)));
	}

	za_used += bestplace.size;
	if (za_used > za_peak)
		za_peak = za_used;
	if (za_used > za_gpeak)
		za_gpeak = za_used;
	za_usedcount++;
	za_nalloc++;
	return ZA_PAYLOAD(b);
}

static int ZA_GuardOk(zablock_t *b)
{
	const uint8_t *g = (const uint8_t *)ZA_PAYLOAD(b) + b->realsize;
	const uint8_t *end = (const uint8_t *)b + ZA_SIZE(b);

	while (g < end)
		if (*g++ != ZA_GUARD)
			return 0;
	return 1;
}

int ZA_Resize(void *payload, size_t size)
{
	zablock_t *b = ZA_BLOCK(payload);
	uint8_t *end = (uint8_t *)b + ZA_SIZE(b), *tail;
	size_t old = ZA_SIZE(b), total = old, need, rest;
	uint32_t flags = b->sf & ZAF_MASK;

	if (size > za_size)
		return 0;
	if ((flags & ZAF_REDZONE) && !ZA_GuardOk(b))
		I_Error("ZA_Resize: red zone of block %p was overwritten", payload);
	// Keep the block's guard policy even if the global test knob changed.
	need = (ZA_HDR + size + ((flags & ZAF_REDZONE) ? ZA_GUARD_MIN : 0) + 15) & ~(size_t)15;
	if (need < ZA_MINBLK)
		need = ZA_MINBLK;
	if (end < za_end && ZA_ISFREE((zablock_t *)end))
		total += ZA_SIZE((zablock_t *)end);
	if (need > total)
		return 0;
	if (total > old)
		ZA_ListDel((zafree_t *)end, total - old);
	rest = total - need;
	if (rest < ZA_MINBLK)
	{
		need = total;
		rest = 0;
	}
	b->sf = (uint32_t)need | flags;
	b->realsize = (uint32_t)size;
	tail = (uint8_t *)b + need;
	if (rest)
		ZA_MakeFree(tail, rest, 0);
	if ((uint8_t *)b + total < za_end)
	{
		zablock_t *next = (zablock_t *)((uint8_t *)b + total);
		if (rest)
			next->sf |= ZAF_PREVFREE;
		else
			next->sf &= ~ZAF_PREVFREE;
	}
	za_used = za_used - old + need;
	if (za_used > za_peak)
		za_peak = za_used;
	if (za_used > za_gpeak)
		za_gpeak = za_used;
	if (flags & ZAF_REDZONE)
		memset((uint8_t *)payload + size, ZA_GUARD, need - ZA_HDR - size);
	return 1;
}

void *ZA_Free(void *payload)
{
	zablock_t *b = ZA_BLOCK(payload);
	uint8_t *start = (uint8_t *)b, *next;
	size_t size = ZA_SIZE(b);
	uint32_t pf = b->sf & ZAF_PREVFREE;

	if ((uint8_t *)b < za_base || start + size > za_end || !(b->sf & ZAF_USED) || !(b->sf & ZAF_VALID))
		I_Error("ZA_Free: bad or double-freed block %p", payload);
	if ((b->sf & ZAF_REDZONE) && !ZA_GuardOk(b))
		I_Error("ZA_Free: red zone of block %p (%lu bytes) was overwritten"
#ifdef ZDEBUG
			" (owner %s:%d)"
#endif
			, payload, (unsigned long)b->realsize
#ifdef ZDEBUG
			, b->ownerfile, (int)b->ownerline
#endif
			);

	za_used -= size;
	za_usedcount--;
	za_nfree++;

	next = start + size;
	if (next < za_end && !(((zablock_t *)next)->sf & ZAF_USED))
	{
		size_t nsize = ZA_SIZE((zablock_t *)next);

		ZA_ListDel((zafree_t *)next, nsize);
		size += nsize;
	}
	if (pf)
	{
		size_t psize = *(uint32_t *)(start - 4);
		zafree_t *prev = (zafree_t *)(start - psize);

		ZA_ListDel(prev, psize);
		start = (uint8_t *)prev;
		size += psize;
		pf = prev->sf & ZAF_PREVFREE;
	}
	ZA_MakeFree(start, size, pf);
	if (start + size < za_end)
		((zablock_t *)(start + size))->sf |= ZAF_PREVFREE;
	return start + size;
}

void ZA_ResetPeak(void)
{
	za_peak = za_used;
}

void ZA_NoteEvict(size_t bytes)
{
	za_nevict++;
	za_evictbytes += bytes;
}

// ---------------------------------------------------------------------------------------------
// Walking, statistics, consistency check
// ---------------------------------------------------------------------------------------------

zablock_t *ZA_First(void)
{
	return za_base ? (zablock_t *)za_base : NULL;
}

zablock_t *ZA_Next(zablock_t *b)
{
	uint8_t *n = (uint8_t *)b + ZA_SIZE(b);

	return n < za_end ? (zablock_t *)n : NULL;
}

zablock_t *ZA_BlockAt(void *addr)
{
	return (uint8_t *)addr < za_end ? (zablock_t *)addr : NULL;
}

size_t ZA_FreeBytes(void)
{
	return za_size - za_used;
}

size_t ZA_BinCount(void)
{
	return ZA_NBINS;
}

zablock_t *ZA_LargestFreeBlock(void)
{
	zablock_t *best = NULL;
	unsigned bin;
	zafree_t *c;

	for (bin = ZA_NBINS; bin-- > 0;)
		if (za_bin[bin])
		{
			for (c = za_bin[bin]; c; c = c->next)
				if (!best || (c->sf & ~ZAF_MASK) > ZA_SIZE(best))
					best = (zablock_t *)c;
			break;
		}
	return best;
}

size_t ZA_LargestFree(void)
{
	size_t best = 0;
	unsigned bin;
	zafree_t *c;

	for (bin = ZA_NBINS; bin-- > 0;) // the highest non-empty bin holds the largest blocks
		if (za_bin[bin])
		{
			for (c = za_bin[bin]; c; c = c->next)
				if ((c->sf & ~ZAF_MASK) > best)
					best = c->sf & ~ZAF_MASK;
			break;
		}
	return best;
}

void ZA_Stats(zastats_t *st)
{
	unsigned bin;
	zafree_t *c;

	memset(st, 0, sizeof *st);
	st->arena = za_size;
	st->used = za_used;
	st->freebytes = za_size - za_used;
	st->freeblocks = za_freecount;
	st->usedblocks = za_usedcount;
	st->peakused = za_peak;
	st->globalpeak = za_gpeak;
	st->allocs = za_nalloc;
	st->frees = za_nfree;
	st->failures = za_nfail;
	st->binsearch = za_nsearch;
	st->evictions = za_nevict;
	st->evictedbytes = za_evictbytes;
	for (bin = ZA_NBINS; bin-- > 0;)
		if (za_bin[bin])
		{
			for (c = za_bin[bin]; c; c = c->next)
				if ((c->sf & ~ZAF_MASK) > st->largestfree)
					st->largestfree = c->sf & ~ZAF_MASK;
			break;
		}
}

#define ZA_BAD(...) do { snprintf(msg, msglen, __VA_ARGS__); return 1; } while (0)

int ZA_Check(char *msg, size_t msglen)
{
	uint8_t *p = za_base;
	int prevfree = 0;
	size_t used = 0, nused = 0, nfree = 0, listed = 0;
	unsigned bin;
	zablock_t *prevb = NULL; // the block before the one being looked at: named in the messages (who overran into the header)

	if (!za_base)
		ZA_BAD("arena not initialised");
	while (p < za_end)
	{
		zablock_t *b = (zablock_t *)p;
		size_t size = ZA_SIZE(b);

		if (!(b->sf & ZAF_VALID))
			ZA_BAD("block at +%lu: valid bit clear (header overwritten)", (unsigned long)(p - za_base));
		if (size < ZA_MINBLK || size > (size_t)(za_end - p))
			ZA_BAD("block at +%lu: bad size %lu", (unsigned long)(p - za_base), (unsigned long)size);
		if (((b->sf & ZAF_PREVFREE) != 0) != prevfree)
#ifdef ZDEBUG
			ZA_BAD("block at +%lu: prev-free flag %d but previous block is %s (previous: +%lu %lu B tag %d real %lu owner %s:%d; this: tag %d real %lu %s:%d)",
				(unsigned long)(p - za_base), (b->sf & ZAF_PREVFREE) != 0, prevfree ? "free" : "used", prevb ? (unsigned long)((uint8_t *)prevb - za_base) : 0ul,
				prevb ? (unsigned long)ZA_SIZE(prevb) : 0ul, prevb ? ZA_TAG(prevb) : -1, prevb ? (unsigned long)prevb->realsize : 0ul,
				prevb && prevb->ownerfile ? prevb->ownerfile : "?", prevb ? prevb->ownerline : 0, ZA_TAG(b), (unsigned long)b->realsize,
				b->ownerfile ? b->ownerfile : "?", b->ownerline);
#else
			ZA_BAD("block at +%lu: prev-free flag %d but previous block is %s (previous: +%lu %lu B tag %d real %lu; this: tag %d real %lu used %d)",
				(unsigned long)(p - za_base), (b->sf & ZAF_PREVFREE) != 0, prevfree ? "free" : "used", prevb ? (unsigned long)((uint8_t *)prevb - za_base) : 0ul,
				prevb ? (unsigned long)ZA_SIZE(prevb) : 0ul, prevb ? ZA_TAG(prevb) : -1, prevb ? (unsigned long)prevb->realsize : 0ul, ZA_TAG(b),
				(unsigned long)b->realsize, (b->sf & ZAF_USED) != 0);
#endif
		if (b->sf & ZAF_USED)
		{
			if (b->realsize > size - ZA_HDR)
				ZA_BAD("block at +%lu: payload %lu does not fit in %lu", (unsigned long)(p - za_base),
					(unsigned long)b->realsize, (unsigned long)size);
			if (((uintptr_t)ZA_PAYLOAD(b) & 15) != 0)
				ZA_BAD("block at +%lu: payload not 16-byte aligned", (unsigned long)(p - za_base));
			if ((b->sf & ZAF_REDZONE) && !ZA_GuardOk(b))
				ZA_BAD("block at +%lu (tag %d, %lu bytes): red zone overwritten", (unsigned long)(p - za_base),
					ZA_TAG(b), (unsigned long)b->realsize);
			used += size;
			nused++;
			prevfree = 0;
		}
		else
		{
			if (prevfree)
				ZA_BAD("free block at +%lu follows a free block (not coalesced)", (unsigned long)(p - za_base));
			if (*(uint32_t *)(p + size - 4) != (uint32_t)size)
				ZA_BAD("free block at +%lu: footer %lu != size %lu", (unsigned long)(p - za_base),
					(unsigned long)*(uint32_t *)(p + size - 4), (unsigned long)size);
			nfree++;
			prevfree = 1;
		}
		prevb = b;
		p += size;
	}
	if (p != za_end)
		ZA_BAD("blocks end at +%lu, arena at +%lu", (unsigned long)(p - za_base), (unsigned long)za_size);
	if (used != za_used || nused != za_usedcount)
		ZA_BAD("used accounting: walk %lu bytes/%lu blocks, counters %lu/%lu", (unsigned long)used, (unsigned long)nused,
			(unsigned long)za_used, (unsigned long)za_usedcount);
	if (nfree != za_freecount)
		ZA_BAD("free block count: walk %lu, counter %lu", (unsigned long)nfree, (unsigned long)za_freecount);

	for (bin = 0; bin < ZA_NBINS; bin++)
	{
		zafree_t *c, *prev = NULL;

		if (((za_map[bin >> 5] >> (bin & 31)) & 1u) != (unsigned)(za_bin[bin] != NULL))
			ZA_BAD("bin %u: bitmap disagrees with list", bin);
		for (c = za_bin[bin]; c; prev = c, c = c->next)
		{
			size_t size = c->sf & ~ZAF_MASK;

			if ((uint8_t *)c < za_base || (uint8_t *)c + size > za_end || (c->sf & ZAF_USED) || !(c->sf & ZAF_VALID))
				ZA_BAD("bin %u: node %p is not a free block", bin, (void *)c);
			if (ZA_BinOf(size) != bin)
				ZA_BAD("bin %u: block of %lu bytes belongs to bin %u", bin, (unsigned long)size, ZA_BinOf(size));
			if (c->prev != prev)
				ZA_BAD("bin %u: broken back link", bin);
			if (++listed > nfree)
				ZA_BAD("free lists hold more blocks than the arena has (loop?)");
		}
	}
	if (listed != nfree)
		ZA_BAD("free lists hold %lu blocks, arena has %lu", (unsigned long)listed, (unsigned long)nfree);
	return 0;
}

// ---------------------------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------------------------

const char *PS2Mem_TagName(int tag)
{
	switch (tag)
	{
		case PU_STATIC: return "PU_STATIC";
		case PU_LUA: return "PU_LUA";
		case PU_PERFSTATS: return "PU_PERFSTATS";
		case PU_SOUND: return "PU_SOUND";
		case PU_MUSIC: return "PU_MUSIC";
		case PU_PATCH: return "PU_PATCH";
		case PU_PATCH_LOWPRIORITY: return "PU_PATCH_LOWPRIORITY";
		case PU_PATCH_ROTATED: return "PU_PATCH_ROTATED";
		case PU_PATCH_DATA: return "PU_PATCH_DATA";
		case PU_SPRITE: return "PU_SPRITE";
		case PU_HUDGFX: return "PU_HUDGFX";
		case PU_HWRPATCHINFO: return "PU_HWRPATCHINFO";
		case PU_HWRPATCHCOLMIPMAP: return "PU_HWRPATCHCOLMIPMAP";
		case PU_HWRMODELTEXTURE: return "PU_HWRMODELTEXTURE";
		case PU_HWRLIGHTTABLEDATA: return "PU_HWRLIGHTTABLEDATA";
		case PU_HWRBATCH: return "PU_HWRBATCH";
		case PU_RENDERWORK: return "PU_RENDERWORK";
		case PU_HWRCACHE: return "PU_HWRCACHE";
		case PU_CACHE: return "PU_CACHE";
		case PU_LEVEL: return "PU_LEVEL";
		case PU_LEVSPEC: return "PU_LEVSPEC";
		case PU_HWRPLANE: return "PU_HWRPLANE";
		case PU_CACHE_UNLOCKED: return "PU_CACHE_UNLOCKED";
		case PU_HWRCACHE_UNLOCKED: return "PU_HWRCACHE_UNLOCKED";
		case PU_HWRMODELTEXTURE_UNLOCKED: return "PU_HWRMODELTEXTURE_UNLOCKED";
		default: return "?";
	}
}

size_t PS2Mem_LibcFree(void)
{
#ifdef _EE
	// the EE's malloc reports success for sizes far above the free RAM, so the probe below is useless there:
	// the C heap can still grow up to the stack, and holds free chunks inside
	struct mallinfo mi = mallinfo();
	return PS2Mem_HeapAvailable(PS2Mem_RamBytes(), PS2Mem_HeapLimit(), (size_t)sbrk(0),
		mi.fordblks > 0 ? (size_t)mi.fordblks : 0);
#else
	size_t lo = 0, hi = 32u << 20;

	while (hi - lo > 4096)
	{
		size_t mid = lo + (hi - lo) / 2;
		void *p = malloc(mid);

		if (p)
		{
			free(p);
			lo = mid;
		}
		else
			hi = mid;
	}
	return lo;
#endif
}

#ifdef ZDEBUG
static void ZA_PrintOwners(size_t top)
{
	enum { MAXOWN = 512 };
	typedef struct { const char *file; int line; int tag; size_t bytes, count; } owner_t;
	static owner_t own[MAXOWN];
	size_t used = 0, i, j;
	zablock_t *b;

	for (b = ZA_First(); b; b = ZA_Next(b))
	{
		if (ZA_ISFREE(b))
			continue;
		for (i = 0; i < used; i++)
			if (own[i].file == b->ownerfile && own[i].line == b->ownerline && own[i].tag == ZA_TAG(b))
				break;
		if (i == used)
		{
			if (used == MAXOWN)
				continue;
			own[used].file = b->ownerfile;
			own[used].line = b->ownerline;
				own[used].tag = ZA_TAG(b);
			own[used].bytes = own[used].count = 0;
			used++;
		}
		own[i].bytes += b->realsize;
		own[i].count++;
	}
	for (j = 0; j < top && j < used; j++)
	{
		size_t best = j;

		for (i = j + 1; i < used; i++)
			if (own[i].bytes > own[best].bytes)
				best = i;
		if (best != j)
		{
			owner_t t = own[j];
			own[j] = own[best];
			own[best] = t;
		}
		I_OutputMsg("ps2_mem: owner %s:%d tag %d  %lu B in %lu blocks\n", own[j].file ? own[j].file : "?", own[j].line, own[j].tag,
			(unsigned long)own[j].bytes, (unsigned long)own[j].count);
	}
	// the most numerous call sites (the small blocks that fragment the arena)
	for (j = 0; j < top && j < used; j++)
	{
		size_t best = j;

		for (i = j + 1; i < used; i++)
			if (own[i].count > own[best].count)
				best = i;
		if (best != j)
		{
			owner_t t = own[j];
			own[j] = own[best];
			own[best] = t;
		}
		I_OutputMsg("ps2_mem: many %s:%d tag %d  %lu blocks, %lu B\n", own[j].file ? own[j].file : "?", own[j].line, own[j].tag,
			(unsigned long)own[j].count, (unsigned long)own[j].bytes);
	}
}

// OOM report entry used by PS2_ReportOOM in i_system.c
void Z_DumpOwners(size_t top)
{
	ZA_PrintOwners(top);
}
#endif

void PS2Mem_Line(const char *label)
{
	zastats_t st;
	unsigned frag;

	ZA_Stats(&st);
	frag = st.freebytes ? (unsigned)(100 - (st.largestfree * 100) / st.freebytes) : 0;
	I_OutputMsg("[zmem] %s frame=%lu cop0=%lu arena=%lu used=%lu peak=%lu gpeak=%lu free=%lu largest=%lu frag=%u%% freeblocks=%lu usedblocks=%lu evicted=%lu/%luB failed=%lu\n",
		label, (unsigned long)Z_FrameCount(), (unsigned long)PS2Mem_Cycles(), (unsigned long)st.arena, (unsigned long)st.used, (unsigned long)st.peakused, (unsigned long)st.globalpeak,
		(unsigned long)st.freebytes, (unsigned long)st.largestfree, frag, (unsigned long)st.freeblocks,
		(unsigned long)st.usedblocks, (unsigned long)st.evictions, (unsigned long)st.evictedbytes, (unsigned long)st.failures);
}

// PS2-70: stamps and watermarks for the stability work. EE time since boot (the 64-bit bus-clock counter: no wrap), the highest break the C
// heap ever needed above the arena (what the libc reserve really has to cover) and the deepest the main thread stack ever went (-zstack).
#ifdef _EE
unsigned PS2Mem_Ms(void)
{
	return (unsigned)(GetTimerSystemTime() / 147456u); // 147.456 MHz bus clock
}

static size_t za_brk_max;
static size_t za_stack_lo, za_stack_size;

static void PS2Mem_NoteBrk(void)
{
	size_t brk = (size_t)sbrk(0);

	if (brk > za_brk_max)
		za_brk_max = brk;
}

size_t PS2Mem_LibcPeak(void) // bytes the C heap needed above the arena since start
{
	PS2Mem_NoteBrk();
	return ZA_Ready() && za_brk_max > (size_t)za_end ? za_brk_max - (size_t)za_end : 0;
}

// -zstack: pattern-fill the unused main stack once, at Z_Init time (the stack is shallow then)
static void PS2Mem_StackFill(void)
{
	ee_thread_status_t thread;
	size_t sp, hi;

	if (za_stack_lo || ReferThreadStatus(GetThreadId(), &thread) < 0 || thread.stack_size <= 0)
		return;
	__asm__ volatile("move %0,$sp" : "=r"(sp));
	hi = (sp - 8192) & ~(size_t)15;
	if ((size_t)thread.stack >= hi)
		return;
	memset(thread.stack, 0xA5, hi - (size_t)thread.stack);
	za_stack_lo = (size_t)thread.stack;
	za_stack_size = (size_t)thread.stack_size;
}

size_t PS2Mem_StackUsed(void) // 0 unless -zstack
{
	const uint8_t *p = (const uint8_t *)za_stack_lo;
	size_t n = 0;

	if (!za_stack_lo)
		return 0;
	while (n < za_stack_size && p[n] == 0xA5)
		n++;
	return za_stack_size - n;
}
#else
unsigned PS2Mem_Ms(void) { return 0; }
size_t PS2Mem_LibcPeak(void) { return 0; }
size_t PS2Mem_StackUsed(void) { return 0; }
#endif

// PS2-73: -zsample [period]: statistical PC sampler over the level load (from "level-free-before" to "precache"), the same method as ps2_prof.c.
// The interrupt handler lives in ps2_boot.c (a -G0 unit: a handler must not address its data through $gp); the table prints as
// "SM <pc> <count>" lines, tools/ps2/sample_report.py --frames 1 turns them into a function list.
#ifdef _EE
static int zs_on;

static void ZS_Start(void)
{
	unsigned period = 8000;

	if (zs_on || !M_CheckParm("-zsample"))
		return;
	if (M_IsNextParm())
		period = (unsigned)atoi(M_GetNextParm());
	PS2Boot_SampleStart(period);
	zs_on = 1;
}

static void ZS_Stop(void)
{
	unsigned i, pc, count, total, dropped;
	int next;

	if (!zs_on)
		return;
	zs_on = 0;
	PS2Boot_SampleStop(&total, &dropped);
	I_OutputMsg("SMTOTAL %u dropped %u\n", total, dropped);
	for (i = 0; (next = PS2Boot_SampleGet(i, &pc, &count)) != 0; i = (unsigned)next)
		I_OutputMsg("SM %08x %u\n", pc, count);
	I_OutputMsg("SMEND\n");
}
#endif

// -zck: one line per checkpoint of the level loader (p_setup.c): where the load peaks and what it is made of.
#ifdef _EE
void PS2Mem_Checkpoint(const char *name)
{
	static int enabled = -1;
	size_t bytes[ZA_MAXTAG + 1];
	zastats_t st;
	zablock_t *b;

	if (!strcmp(name, "level-free-before"))
		ZS_Start();
	else if (!strcmp(name, "precache"))
		ZS_Stop();
	if (enabled < 0)
		enabled = M_CheckParm("-zck") != 0;
	if (!enabled)
		return;
	memset(bytes, 0, sizeof bytes);
	for (b = ZA_First(); b; b = ZA_Next(b))
		if (!ZA_ISFREE(b))
			bytes[ZA_TAG(b)] += ZA_SIZE(b);
	ZA_Stats(&st);
	if (M_CheckParm("-zmap") && !strcmp(name, "precache"))
		PS2Mem_Map(200);
	{
		struct mallinfo mi = mallinfo();
		I_OutputMsg("[zlibc] %-22s libc in use outside the arena %ld B, free chunks %lu B, growth left %lu B\n", name,
			(long)((long)mi.uordblks - (long)st.arena), (unsigned long)mi.fordblks, (unsigned long)PS2Mem_LibcFree());
	}
	if (M_CheckParm("-zholes"))
	{
		size_t n[5] = {0}, by[5] = {0}, big = 0, sz;
		int k;
		for (b = ZA_First(); b; b = ZA_Next(b))
			if (ZA_ISFREE(b))
			{
				sz = ZA_SIZE(b);
				k = sz >= (1u << 20) ? 4 : sz >= (64u << 10) ? 3 : sz >= (16u << 10) ? 2 : sz >= (4u << 10) ? 1 : 0;
				n[k]++;
				by[k] += sz;
				if (sz > big)
					big = sz;
			}
		I_OutputMsg("[zhole] %-22s >=1M %lu/%lu  64K..1M %lu/%lu  16..64K %lu/%lu  4..16K %lu/%lu  <4K %lu/%lu\n", name,
			(unsigned long)n[4], (unsigned long)by[4], (unsigned long)n[3], (unsigned long)by[3], (unsigned long)n[2], (unsigned long)by[2],
			(unsigned long)n[1], (unsigned long)by[1], (unsigned long)n[0], (unsigned long)by[0]);
	}
	PS2Mem_NoteBrk();
	I_OutputMsg("[zck] %-22s t_ms=%lu cop0=%lu used=%lu peak=%lu free=%lu largest=%lu static=%lu level=%lu cache=%lu patch=%lu sprite=%lu evicted=%lu\n",
		name, (unsigned long)PS2Mem_Ms(), (unsigned long)PS2Mem_Cycles(), (unsigned long)st.used, (unsigned long)st.peakused, (unsigned long)st.freebytes,
		(unsigned long)st.largestfree, (unsigned long)bytes[PU_STATIC], (unsigned long)(bytes[PU_LEVEL] + bytes[PU_LEVSPEC]),
		(unsigned long)bytes[PU_CACHE], (unsigned long)(bytes[PU_PATCH] + bytes[PU_PATCH_DATA] + bytes[PU_HUDGFX]),
		(unsigned long)bytes[PU_SPRITE], (unsigned long)st.evictions);
}
#else
void PS2Mem_Checkpoint(const char *name) { (void)name; }
#endif

// Address-ordered map of the arena: neighbouring blocks of one class are merged into one run.
// F free, L level (PU_LEVEL/LEVSPEC), S static/long-lived, C cache (PU_CACHE and above), W renderer work, P patches/sprites/HUD.
static char ZA_Class(const zablock_t *b)
{
	int t;

	if (ZA_ISFREE(b))
		return 'F';
	t = ZA_TAG(b);
	if (t == PU_LEVEL || t == PU_LEVSPEC)
		return 'L';
	if (t == PU_RENDERWORK)
		return 'W';
	if (t >= PU_CACHE)
		return 'C';
	if (t == PU_PATCH || t == PU_PATCH_DATA || t == PU_SPRITE || t == PU_HUDGFX || t == PU_PATCH_LOWPRIORITY || t == PU_PATCH_ROTATED)
		return 'P';
	return 'S';
}

void PS2Mem_Map(size_t maxruns)
{
	zablock_t *b;
	size_t runs = 0, pass, skip = 0, idx, bytes = 0, blocks = 0, start = 0, pos = 0;
	char cls = 0;

	// pass 0 counts the runs, pass 1 prints the last `maxruns` of them (the front of the arena is early-boot clutter)
	for (pass = 0; pass < 2; pass++)
	{
		idx = 0;
		bytes = blocks = start = pos = 0;
		cls = 0;
		if (pass)
			skip = runs > maxruns ? runs - maxruns : 0;
		for (b = ZA_First(); b; b = ZA_Next(b))
		{
			char c = ZA_Class(b);

			if (c != cls && blocks)
			{
				if (!pass)
					runs++;
				else if (idx++ >= skip)
					I_OutputMsg("[zmap] +%07lx %c %9lu B %5lu blocks\n", (unsigned long)start, cls, (unsigned long)bytes, (unsigned long)blocks);
				bytes = blocks = 0;
			}
			if (!blocks)
			{
				start = pos;
				cls = c;
			}
			bytes += ZA_SIZE(b);
			blocks++;
			pos += ZA_SIZE(b);
		}
		if (blocks)
		{
			if (!pass)
				runs++;
			else if (idx++ >= skip)
				I_OutputMsg("[zmap] +%07lx %c %9lu B %5lu blocks\n", (unsigned long)start, cls, (unsigned long)bytes, (unsigned long)blocks);
		}
	}
	I_OutputMsg("[zmap] %lu runs, last %lu printed\n", (unsigned long)runs, (unsigned long)(runs - skip));
}


// Free blocks of at least `minbytes`, address order, with the class of the block before and after each ("[zfree]" lines).
void PS2Mem_FreeList(size_t minbytes)
{
	zablock_t *b, *prev = NULL;
	size_t n = 0, small = 0, smallbytes = 0, pos = 0;

	for (b = ZA_First(); b; prev = b, pos += ZA_SIZE(b), b = ZA_Next(b))
	{
		zablock_t *next;
		if (!ZA_ISFREE(b))
			continue;
		if (ZA_SIZE(b) < minbytes)
		{
			small++;
			smallbytes += ZA_SIZE(b);
			continue;
		}
		next = ZA_Next(b);
		I_OutputMsg("[zfree] +%07lx %9lu B  before %c(tag %d) after %c(tag %d)\n", (unsigned long)pos, (unsigned long)ZA_SIZE(b),
			prev ? ZA_Class(prev) : '-', prev ? ZA_TAG(prev) : -1, next ? ZA_Class(next) : '-', next ? ZA_TAG(next) : -1);
		n++;
	}
	I_OutputMsg("[zfree] %lu blocks >= %lu B listed, %lu smaller blocks hold %lu B\n", (unsigned long)n, (unsigned long)minbytes,
		(unsigned long)small, (unsigned long)smallbytes);
}

void PS2Mem_Report(int owners)
{
	struct { size_t blocks, bytes, payload; } tag[ZA_MAXTAG + 1];
	zastats_t st;
	zablock_t *b;
	int t;
	unsigned frag;
#ifdef _EE
	struct mallinfo mi = mallinfo();
#endif

	memset(tag, 0, sizeof tag);
	for (b = ZA_First(); b; b = ZA_Next(b))
	{
		if (ZA_ISFREE(b))
			continue;
		t = ZA_TAG(b);
		tag[t].blocks++;
		tag[t].bytes += ZA_SIZE(b);
		tag[t].payload += b->realsize;
	}
	ZA_Stats(&st);
	frag = st.freebytes ? (unsigned)(100 - (st.largestfree * 100) / st.freebytes) : 0;

	I_OutputMsg("ps2_mem: frame %lu\n", (unsigned long)Z_FrameCount());
	I_OutputMsg("ps2_mem: arena %lu B  used %lu B (peak %lu, since start %lu)  free %lu B in %lu blocks, largest %lu B, fragmentation %u%%\n",
		(unsigned long)st.arena, (unsigned long)st.used, (unsigned long)st.peakused, (unsigned long)st.globalpeak, (unsigned long)st.freebytes,
		(unsigned long)st.freeblocks, (unsigned long)st.largestfree, frag);
	for (t = 0; t <= ZA_MAXTAG; t++)
		if (tag[t].blocks)
			I_OutputMsg("ps2_mem: tag %3d %-22s %6lu blocks %9lu B (payload %9lu B)\n", t, PS2Mem_TagName(t),
				(unsigned long)tag[t].blocks, (unsigned long)tag[t].bytes, (unsigned long)tag[t].payload);
	{
		// what the cache holds by age in frames (touched this frame / 1 / 2-3 / 4-15 / older): the working set a frame really needs
		size_t age[5] = {0};
		for (b = ZA_First(); b; b = ZA_Next(b))
			if (!ZA_ISFREE(b) && (ZA_TAG(b) == PU_CACHE || ZA_TAG(b) == PU_SPRITE) && b->user)
			{
				unsigned a = (Z_FrameCount() - ZA_STAMP(b)) & 0xFFFFFFu;
				age[a == 0 ? 0 : a == 1 ? 1 : a < 4 ? 2 : a < 16 ? 3 : 4] += ZA_SIZE(b);
			}
		I_OutputMsg("ps2_mem: cache+sprite bytes by age: now %lu, 1 frame %lu, 2-3 %lu, 4-15 %lu, older %lu\n", (unsigned long)age[0],
			(unsigned long)age[1], (unsigned long)age[2], (unsigned long)age[3], (unsigned long)age[4]);
	}
	Z_ReportCosts();
	I_OutputMsg("ps2_mem: allocs %lu frees %lu failed %lu evicted %lu blocks/%lu B, free blocks scanned %lu\n",
		(unsigned long)st.allocs, (unsigned long)st.frees, (unsigned long)st.failures, (unsigned long)st.evictions,
		(unsigned long)st.evictedbytes, (unsigned long)st.binsearch);
#ifdef _EE
	I_OutputMsg("ps2_mem: libc heap %lu B, in use %lu B, free chunks %lu B, libc can still give %lu B\n",
		(unsigned long)mi.arena, (unsigned long)mi.uordblks, (unsigned long)mi.fordblks, (unsigned long)PS2Mem_LibcFree());
	I_OutputMsg("ps2_mem: brk 0x%lx, heap start 0x%lx, RAM %lu B, main stack %lu B\n", (unsigned long)sbrk(0),
		(unsigned long)((size_t)sbrk(0) - mi.arena), (unsigned long)GetMemorySize(), (unsigned long)(size_t)_stack_size);
	I_OutputMsg("ps2_mem: safe heap ceiling 0x%lx, contiguous libc growth %lu B (free chunks above are not contiguous guarantees)\n",
		(unsigned long)PS2Mem_HeapLimit(),
		(unsigned long)PS2Mem_HeapAvailable(PS2Mem_RamBytes(), PS2Mem_HeapLimit(), (size_t)sbrk(0), 0));
	I_OutputMsg("ps2_mem: C heap peak above the arena %lu B; main stack used %lu B of %lu B (0 = -zstack off)\n",
		(unsigned long)PS2Mem_LibcPeak(), (unsigned long)PS2Mem_StackUsed(), (unsigned long)za_stack_size);
	{
		size_t sn, sp, sb, st2, sf, sx;

		PS2Spill_Stats(&sn, &sp, &sb, &st2, &sf, &sx);
		I_OutputMsg("ps2_mem: C heap blocks taken from the arena (PS2-79): now %lu B in %lu blocks, peak %lu B, %lu in all, refused %lu, left by other threads %lu\n",
			(unsigned long)sn, (unsigned long)sb, (unsigned long)sp, (unsigned long)st2, (unsigned long)sf, (unsigned long)sx);
	}
#endif
#ifdef ZDEBUG
	if (owners > 0)
		ZA_PrintOwners((size_t)owners);
#else
	(void)owners;
#endif
}

// ---------------------------------------------------------------------------------------------
// RAM profile and test hooks (-zram, -zsizes, -zquit, -zquitall)
// ---------------------------------------------------------------------------------------------

unsigned PS2Mem_Cycles(void)
{
#ifdef _EE
	unsigned v;
	__asm__ volatile("mfc0 %0,$9" : "=r"(v));
	return v;
#else
	return 0;
#endif
}

size_t PS2Mem_RamBytes(void)
{
#ifdef _EE
	// Kernel/loader supplied size only; never test whether an address above retail RAM is writable.
	size_t ram = (size_t)GetMemorySize();
	return ram == (128u << 20) ? ram : (32u << 20);
#else
	return (size_t)32 << 20;
#endif
}

int PS2Mem_RamClass(void)
{
	if (M_CheckParm("-zram") && M_IsNextParm())
		if (atoi(M_GetNextParm()) < 64)
			return 32; // tests can select a smaller policy, never manufacture physical RAM
	return PS2Mem_RamBytes() > ((size_t)64 << 20) ? 128 : 32;
}

size_t PS2Mem_HeapAvailable(size_t ram, size_t stackbase, size_t brk, size_t freechunks)
{
	size_t growth;
	if (!stackbase || stackbase > ram || brk > stackbase)
		return 0;
	growth = stackbase - brk;
	// Corrupt/unsupported mallinfo values must not turn into an unbounded unsigned budget.
	if (freechunks > brk)
		freechunks = 0;
	return growth + freechunks;
}

#ifdef _EE
size_t PS2Mem_HeapLimit(void)
{
	static size_t limit;
	if (!limit)
	{
		ee_thread_status_t thread;
		size_t ram = PS2Mem_RamBytes(), sp;
		__asm__ volatile("move %0,$sp" : "=r"(sp));
		// The kernel may place the main stack below reported RAM, particularly on development loaders.
		if (ReferThreadStatus(GetThreadId(), &thread) >= 0 && thread.stack_size > 0
			&& (size_t)thread.stack < sp && sp - (size_t)thread.stack <= (size_t)thread.stack_size)
			limit = (size_t)thread.stack;
		else
		{
			size_t stack = (size_t)_stack_size;
			if (!stack || stack >= ram)
				stack = 512u << 10;
			limit = sp > stack ? sp - stack : 0; // conservative fallback below the live stack
		}
		if (limit > ram)
			limit = ram;
	}
	return limit;
}
#endif

#if defined (_EE) && defined (PS2_LEAKTRACE)
// PS2-78: build.py --leaktrace wraps the C allocator (linker --wrap, which also sees newlib's own calls: fopen, strdup, ...) and records every
// live block with the return address of its allocation; every "ZCHAIN" line is followed by LEAK lines: the live blocks grouped by that address,
// with the change since the previous report. Resolve the addresses with addr2line against the ELF. Diagnostic only: never in the release.
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
void *__real_memalign(size_t, size_t);
// newlib's own code (stdio, locale, ...) calls the reentrant forms directly: they are wrapped too; a call made through malloc()/free() is
// registered once, with the caller's address (lt_depth)
void *__real__malloc_r(void *, size_t);
void *__real__calloc_r(void *, size_t, size_t);
void *__real__realloc_r(void *, void *, size_t);
void __real__free_r(void *, void *);
void *__real__memalign_r(void *, size_t, size_t);
static int lt_depth;
#define LT_SLOTS 16384
static struct { void *p; UINT32 ra, size; } lt_tab[LT_SLOTS];
static UINT32 lt_dropped;

static void LT_Add(void *p, size_t n, UINT32 ra)
{
	UINT32 h = ((UINT32)(uintptr_t)p >> 4) & (LT_SLOTS - 1);
	UINT32 i;

	for (i = 0; i < LT_SLOTS; i++, h = (h + 1) & (LT_SLOTS - 1))
		if (!lt_tab[h].p)
		{
			lt_tab[h].p = p;
			lt_tab[h].ra = ra;
			lt_tab[h].size = (UINT32)n;
			return;
		}
	lt_dropped++;
}

static void LT_Del(void *p)
{
	UINT32 h = ((UINT32)(uintptr_t)p >> 4) & (LT_SLOTS - 1);
	UINT32 i;

	if (!p)
		return;
	for (i = 0; i < LT_SLOTS; i++, h = (h + 1) & (LT_SLOTS - 1))
	{
		if (lt_tab[h].p == p)
		{
			lt_tab[h].p = NULL;
			// keep the probe chains intact: re-insert the entries that follow
			for (h = (h + 1) & (LT_SLOTS - 1); lt_tab[h].p; h = (h + 1) & (LT_SLOTS - 1))
			{
				void *q = lt_tab[h].p;
				UINT32 qra = lt_tab[h].ra, qsz = lt_tab[h].size;

				lt_tab[h].p = NULL;
				LT_Add(q, qsz, qra);
			}
			return;
		}
		if (!lt_tab[h].p)
			return;
	}
}

void *__wrap_malloc(size_t n)
{
	void *p;

	lt_depth++;
	p = __real_malloc(n);
	lt_depth--;

	if (p)
		LT_Add(p, n, (UINT32)__builtin_return_address(0));
	return p;
}

void *__wrap_calloc(size_t a, size_t b)
{
	void *p;

	lt_depth++;
	p = __real_calloc(a, b);
	lt_depth--;

	if (p)
		LT_Add(p, a * b, (UINT32)__builtin_return_address(0));
	return p;
}

void *__wrap_memalign(size_t al, size_t n)
{
	void *p;

	lt_depth++;
	p = __real_memalign(al, n);
	lt_depth--;

	if (p)
		LT_Add(p, n, (UINT32)__builtin_return_address(0));
	return p;
}

void *__wrap_realloc(void *old, size_t n)
{
	void *p;

	lt_depth++;
	p = __real_realloc(old, n);
	lt_depth--;

	if (p)
	{
		if (old)
			LT_Del(old);
		LT_Add(p, n, (UINT32)__builtin_return_address(0));
	}
	return p;
}

void __wrap_free(void *p)
{
	LT_Del(p);
	lt_depth++;
	__real_free(p);
	lt_depth--;
}

void *__wrap__malloc_r(void *r, size_t n)
{
	void *p = __real__malloc_r(r, n);

	if (p && !lt_depth)
		LT_Add(p, n, (UINT32)__builtin_return_address(0));
	return p;
}

void *__wrap__calloc_r(void *r, size_t a, size_t b)
{
	void *p = __real__calloc_r(r, a, b);

	if (p && !lt_depth)
		LT_Add(p, a * b, (UINT32)__builtin_return_address(0));
	return p;
}

void *__wrap__memalign_r(void *r, size_t al, size_t n)
{
	void *p = __real__memalign_r(r, al, n);

	if (p && !lt_depth)
		LT_Add(p, n, (UINT32)__builtin_return_address(0));
	return p;
}

void *__wrap__realloc_r(void *r, void *old, size_t n)
{
	void *p = __real__realloc_r(r, old, n);

	if (p && !lt_depth)
	{
		if (old)
			LT_Del(old);
		LT_Add(p, n, (UINT32)__builtin_return_address(0));
	}
	return p;
}

void __wrap__free_r(void *r, void *p)
{
	LT_Del(p);
	__real__free_r(r, p);
}

#define LT_GROUPS 96
static struct { UINT32 ra, count, bytes; INT32 pcount; INT32 pbytes; } lt_grp[LT_GROUPS];

static void PS2Mem_LeakReport(unsigned n)
{
	UINT32 i, g, ng = 0, live = 0, livebytes = 0;
	INT32 pass;

	for (i = 0; i < LT_GROUPS; i++)
	{
		lt_grp[i].pcount = (INT32)lt_grp[i].count;
		lt_grp[i].pbytes = (INT32)lt_grp[i].bytes;
		lt_grp[i].count = lt_grp[i].bytes = 0;
	}
	for (i = 0; i < LT_SLOTS; i++)
	{
		if (!lt_tab[i].p)
			continue;
		live++;
		livebytes += lt_tab[i].size;
		for (g = 0; g < LT_GROUPS && lt_grp[g].ra && lt_grp[g].ra != lt_tab[i].ra; g++)
			;
		if (g == LT_GROUPS)
			continue;
		lt_grp[g].ra = lt_tab[i].ra;
		lt_grp[g].count++;
		lt_grp[g].bytes += lt_tab[i].size;
		if (g >= ng)
			ng = g + 1;
	}
	{
		struct mallinfo mi = mallinfo();

		I_OutputMsg("LEAK n=%u live=%u bytes=%u dropped=%u brk=%lu mi.arena=%lu uord=%lu ford=%lu keepcost=%lu hblks=%lu\n", n, live, livebytes, lt_dropped,
			(unsigned long)(uintptr_t)sbrk(0), (unsigned long)mi.arena, (unsigned long)mi.uordblks, (unsigned long)mi.fordblks,
			(unsigned long)mi.keepcost, (unsigned long)mi.hblkhd);
	}
	for (pass = 0; pass < 12; pass++)
	{
		// the groups whose live block count grew since the previous report first (that is the leak)
		INT32 best = -1;
		INT32 bestd = 0;

		for (g = 0; g < ng; g++)
		{
			const INT32 d = (INT32)lt_grp[g].count - lt_grp[g].pcount;

			if (lt_grp[g].ra && lt_grp[g].count && d > bestd)
			{
				best = (INT32)g;
				bestd = d;
			}
		}
		if (best < 0)
			break;
		I_OutputMsg("LEAKG n=%u ra=%08x count=%u bytes=%u dcount=%d dbytes=%d\n", n, (unsigned)lt_grp[best].ra, (unsigned)lt_grp[best].count,
			(unsigned)lt_grp[best].bytes, (int)bestd, (int)((INT32)lt_grp[best].bytes - lt_grp[best].pbytes));
		lt_grp[best].pcount = (INT32)lt_grp[best].count; // listed: not again
	}
}
#endif

#ifdef _EE
#define SZ(t) I_OutputMsg("ps2_mem: sizeof %-14s %5lu\n", #t, (unsigned long)sizeof (t))
void PS2Mem_Sizes(void)
{
	SZ(vertex_t); SZ(line_t); SZ(side_t); SZ(sector_t); SZ(seg_t); SZ(subsector_t); SZ(node_t); SZ(mapthing_t);
	SZ(mobj_t); SZ(precipmobj_t); SZ(msecnode_t); SZ(ffloor_t); SZ(pslope_t); SZ(polyobj_t);
	SZ(visplane_t); SZ(drawseg_t); SZ(vissprite_t); SZ(patch_t); SZ(column_t); SZ(post_t);
}

// -zchain A,B,C: map names (the part after "MAP") loaded one after the other on one boot, with -zquit frames in each, after the
// level of -warp. A "ZCHAIN" line (heap check, usage, free space) follows every level; the run ends after the last one.
static int ChainNext(const char **list, char *out, size_t outsize)
{
	const char *p = *list;
	size_t n = 0;

	while (*p == ',')
		p++;
	while (p[n] && p[n] != ',' && n < outsize - 1)
	{
		out[n] = p[n];
		n++;
	}
	out[n] = 0;
	p += n;
	while (*p && *p != ',')
		p++;
	*list = p;
	return n != 0;
}

void PS2Mem_Frame(void)
{
	static int init;
	static long quitlevel = -1, quitall = -1;
	static unsigned levelframes, allframes, startcycles;
	static const char *chain_list;
	static int chain_active, chain_wait;
	static unsigned chain_base, chain_count, chain_issued, chain_cycles;
	static tic_t chain_prev_time;

	if (!init)
	{
		init = 1;
		startcycles = PS2Mem_Cycles();
		if (M_CheckParm("-zquit") && M_IsNextParm())
			quitlevel = atol(M_GetNextParm());
		if (M_CheckParm("-zquitall") && M_IsNextParm())
			quitall = atol(M_GetNextParm());
		if (M_CheckParm("-zchain") && M_IsNextParm())
		{
			chain_list = M_GetNextParm();
			chain_active = 1;
		}
		if (M_CheckParm("-zsizes"))
			PS2Mem_Sizes();
		if (M_CheckParm("-zsingle"))
			singletics = true; // PS2-141: one game tic per displayed frame, no waiting for the clock (soak runs at the speed of the emulator, scripted pads)
	}
	allframes++;
	PS2Mem_NoteBrk();
	PS2Net_Frame(); // PS2-132: -netcmd
	if (gamestate == GS_LEVEL)
		levelframes++;
	if (chain_active)
	{
		if (chain_wait && gamestate == GS_LEVEL && leveltime < chain_prev_time)
		{
			chain_wait = 0; // the next level has started
			chain_base = levelframes;
			chain_cycles = PS2Mem_Cycles();
			ZA_ResetPeak();
		}
		chain_prev_time = leveltime;
		if (chain_wait)
		{
			if (allframes - chain_issued > 20000)
			{
				I_OutputMsg("ZCHAIN timeout: the map command did not start a level\n");
				chain_wait = 0;
				chain_active = 0;
				chain_list = NULL;
				quitall = 1; // ends the run below with the report
			}
			else
				return;
		}
		else if (gamestate == GS_LEVEL && quitlevel >= 0 && levelframes - chain_base >= (unsigned)quitlevel)
		{
			zastats_t st;
			char msg[160], next[16];

			ZA_Stats(&st);
			chain_count++;
			I_OutputMsg("ZCHAIN n=%u map=%d frames=%u check=%s used=%lu peak=%lu gpeak=%lu free=%lu largest=%lu evictedbytes=%lu failed=%lu libcfree=%lu mcycles=%u libcpeak=%lu t_ms=%lu\n",
				chain_count, (int)gamemap, levelframes - chain_base, ZA_Check(msg, sizeof msg) ? "FAILED" : "ok", (unsigned long)st.used,
				(unsigned long)st.peakused, (unsigned long)st.globalpeak, (unsigned long)st.freebytes, (unsigned long)st.largestfree,
				(unsigned long)st.evictedbytes, (unsigned long)st.failures, (unsigned long)PS2Mem_LibcFree(), (PS2Mem_Cycles() - chain_cycles) >> 20,
					(unsigned long)PS2Mem_LibcPeak(), (unsigned long)PS2Mem_Ms());
			if (ZA_Check(msg, sizeof msg))
				I_OutputMsg("ps2_mem: HEAP CHECK FAILED: %s\n", msg);
#ifdef PS2_LEAKTRACE
			PS2Mem_LeakReport(chain_count);
#endif
			if (ChainNext(&chain_list, next, sizeof next))
			{
				char cmd[48];

				snprintf(cmd, sizeof cmd, "map MAP%s -force\n", next);
				COM_BufAddText(cmd);
				chain_wait = 1;
				chain_issued = allframes;
				return;
			}
			chain_active = 0; // that was the last one: the report below ends the run
		}
	}
	if ((quitlevel >= 0 && levelframes - chain_base >= (unsigned)quitlevel && gamestate == GS_LEVEL)
		|| (quitall >= 0 && allframes >= (unsigned)quitall))
	{
		zastats_t st;
		char msg[160];
		size_t sp_now, sp_peak, sp_b, sp_t, sp_f, sp_x;

		ZA_Stats(&st);
		PS2Spill_Stats(&sp_now, &sp_peak, &sp_b, &sp_t, &sp_f, &sp_x);
		if (ZA_Check(msg, sizeof msg))
			I_OutputMsg("ps2_mem: HEAP CHECK FAILED: %s\n", msg);
		PS2Mem_Line("final");
		PS2Mem_Report(M_CheckParm("-zowners") && M_IsNextParm() ? atoi(M_GetNextParm()) : 0);
		I_OutputMsg("ZSTAT ram=%lu class=%d map=%d levelframes=%u frames=%u cycles=%u arena=%lu used=%lu peak=%lu gpeak=%lu free=%lu largest=%lu evicted=%lu evictedbytes=%lu failed=%lu libcfree=%lu flushes=%lu libcpeak=%lu stackused=%lu spillpeak=%lu spillnow=%lu t_ms=%lu\n",
			(unsigned long)PS2Mem_RamBytes(), PS2Mem_RamClass(), (int)gamemap, levelframes, allframes, PS2Mem_Cycles() - startcycles,
			(unsigned long)st.arena, (unsigned long)st.used, (unsigned long)st.peakused, (unsigned long)st.globalpeak, (unsigned long)st.freebytes,
			(unsigned long)st.largestfree, (unsigned long)st.evictions, (unsigned long)st.evictedbytes, (unsigned long)st.failures,
			(unsigned long)PS2Mem_LibcFree(), (unsigned long)Z_TestFlushes(), (unsigned long)PS2Mem_LibcPeak(), (unsigned long)PS2Mem_StackUsed(), (unsigned long)sp_peak, (unsigned long)sp_now,
			(unsigned long)PS2Mem_Ms());
		I_OutputMsg("ZQUIT DONE\n");
		I_Quit();
	}
}
#else
void PS2Mem_Sizes(void) {}
void PS2Mem_Frame(void) {}
#endif

static void Command_Ps2Mem_f(void)
{
	char msg[160];
	size_t i;
	int owners = 0;

	for (i = 1; i < COM_Argc(); i++)
		if (!strcmp(COM_Argv(i), "-owners"))
			owners = i + 1 < COM_Argc() ? atoi(COM_Argv(i + 1)) : 24;
	if (ZA_Check(msg, sizeof msg))
		I_OutputMsg("ps2_mem: HEAP CHECK FAILED: %s\n", msg);
	PS2Mem_Report(owners);
}

void PS2Mem_Init(void)
{
#ifdef _EE
	if (M_CheckParm("-zstack"))
		PS2Mem_StackFill();
#endif
	COM_AddCommand("ps2_mem", Command_Ps2Mem_f, 0);
}
