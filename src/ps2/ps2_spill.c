// SRB2 PS2 port: C heap overflow into the zone arena (PS2-79, OPT9-S).
//
// The zone arena is one block taken from the C heap at start-up; what is left above it (the "reserve", 1.5 MiB) is all libc can ever grow into
// (sbrk stops at the main thread stack). The GS hardware renderer, Lua, lwIP, add-on loading and the codecs allocate with malloc/calloc/realloc/
// memalign: in the full build the hardware renderer alone needs 2..4 MiB (gl_textures/gl_flats tables, GLMipmap_t records, polygon pools, GIF
// rings, batching buffers), more than any fixed reserve can be without starving the zone of software-only maps. "HWR_LoadMapTextures: ran out of
// memory" at the start of the first hardware run was the symptom.
// Here every libc allocation that the C heap cannot satisfy is taken from the zone arena instead (tag PU_STATIC, so the zone evicts caches to make
// room like for any other allocation), and free/realloc of such a block find it again by address. One pool, no stranded reserve: the budget of
// both renderers is the arena, and the reserve only has to cover the allocations that must stay in libc (those of other threads).
//
// Rules: only the main (game) thread may spill (the zone is not thread-safe; other threads get NULL from the C heap exactly as before), a
// block that other thread frees is leaked and counted (never happens with the decoder/mixer threads: they allocate in their own thread from the
// C heap). The reentrant newlib entry points are wrapped (linker --wrap, tools/ps2/build.py): malloc/free/calloc/realloc/memalign all end there.
// Diagnostic builds with --leaktrace (PS2-78) wrap the same symbols and do not spill.

#include "doomdef.h"
#include "z_zone.h"
#include "m_argv.h"
#include "i_system.h"
#include "ps2_mem.h"

#if defined (_EE) && !defined (PS2_LEAKTRACE)
#include <string.h>
#include <kernel.h>

void *__real__malloc_r(void *, size_t);
void *__real__realloc_r(void *, void *, size_t);
size_t _malloc_usable_size_r(void *, void *);
void __real__free_r(void *, void *);
void *__real__memalign_r(void *, size_t, size_t);

static int spill_tid = -1;
static int spill_busy;      // Z_TryMallocAlign/Z_Free is running: a malloc made from inside it (diagnostics) must not spill again
#define SPILL_FIRST (24u << 10) // a main-thread allocation of this size and more goes to the zone first: the C heap above the arena is
                                 // all the other threads (music decoder, mixer, network) can ever allocate from, it must not be eaten by the game thread
static int spill_inhibit;   // __real__memalign_r is running: it splits chunks it got from _malloc_r and needs libc chunks
static size_t spill_now, spill_peak, spill_blocks, spill_total, spill_fail, spill_foreign;
static int spill_verbose = -1; // -zspill: one line per block of 16 KiB and more

void PS2Spill_Init(void)
{
	spill_tid = GetThreadId(); // the thread that makes the arena is the game thread
}

static int Spill_Allowed(void)
{
	return spill_tid >= 0 && !spill_busy && !spill_inhibit && ZA_Ready() && GetThreadId() == spill_tid;
}

static void *Spill_Alloc(size_t n, int alignbits)
{
	void *p;

	if (!Spill_Allowed() || !n)
		return NULL;
	spill_busy++;
	p = Z_TryMallocAlign(n, PU_STATIC, NULL, alignbits);
	spill_busy--;
	if (!p)
	{
		spill_fail++;
		return NULL;
	}
	spill_now += n;
	spill_blocks++;
	spill_total++;
	if (spill_verbose < 0)
		spill_verbose = M_CheckParm("-zspill") != 0;
	if (spill_verbose && n >= 16384)
		I_OutputMsg("[spill] %lu bytes (now %lu) ra=%08lx\n", (unsigned long)n, (unsigned long)spill_now, (unsigned long)(uintptr_t)__builtin_return_address(0));
	if (spill_now > spill_peak)
		spill_peak = spill_now;
	return p;
}

static void Spill_Free(void *p)
{
	if (spill_tid < 0 || GetThreadId() != spill_tid || spill_busy)
	{
		spill_foreign++; // not the game thread: the zone cannot be touched from here; the block stays (bounded by the other threads' habits: none seen)
		return;
	}
	spill_now -= ZA_PayloadBytes(p);
	spill_blocks--;
	spill_busy++;
	Z_Free(p);
	spill_busy--;
}

void *__wrap__malloc_r(void *r, size_t n)
{
	void *p;

	if (n >= SPILL_FIRST && (p = Spill_Alloc(n, 4)) != NULL)
		return p;
	p = __real__malloc_r(r, n);
	if (!p && n)
		p = Spill_Alloc(n, 4);
	return p;
}

void __wrap__free_r(void *r, void *p)
{
	if (p && ZA_Contains(p))
		Spill_Free(p);
	else
		__real__free_r(r, p);
}

// newlib's _calloc_r clears (size in the chunk header before the pointer) - 4 bytes: for a block of the arena that header is the zone's, the memset
// then runs off the end of RAM (found by the first hardware run: a TLB-miss storm from memset(0x1f000000)). So calloc is made here.
void *__wrap__calloc_r(void *r, size_t a, size_t b)
{
	void *p;

	if (a && b > (size_t)-1 / a)
		return NULL;
	p = __wrap__malloc_r(r, a * b);
	if (p)
		memset(p, 0, a * b);
	return p;
}

void *__wrap__realloc_r(void *r, void *p, size_t n)
{
	void *q;
	size_t old;

	if (!p)
		return __wrap__malloc_r(r, n);
	if (!ZA_Contains(p))
	{
		// a block of the C heap: newlib's realloc (which reads chunk headers) must never see a block of the arena, so it runs with spilling
		// off; when the C heap cannot grow it the copy into the arena is made here
		spill_inhibit++;
		q = __real__realloc_r(r, p, n);
		spill_inhibit--;
		if (q || !n)
			return q;
		old = _malloc_usable_size_r(r, p);
		q = Spill_Alloc(n, 4);
		if (!q)
			return NULL;
		memcpy(q, p, old < n ? old : n);
		__real__free_r(r, p);
		return q;
	}
	if (!n)
	{
		Spill_Free(p);
		return NULL;
	}
	old = ZA_PayloadBytes(p);
	q = __real__malloc_r(r, n); // back into the C heap when it has room again
	if (!q)
		q = Spill_Alloc(n, 4);
	if (!q)
		return NULL;
	memcpy(q, p, old < n ? old : n);
	Spill_Free(p);
	return q;
}

void *__wrap__memalign_r(void *r, size_t align, size_t n)
{
	void *p;
	int bits = 4;

	while (((size_t)1 << bits) < align && bits < 20)
		bits++;
	if (n >= SPILL_FIRST && (p = Spill_Alloc(n, bits)) != NULL)
		return p;
	spill_inhibit++;
	p = __real__memalign_r(r, align, n);
	spill_inhibit--;
	if (p || !n)
		return p;
	return Spill_Alloc(n, bits);
}

// PS2-170: the game thread jumped out of an allocation (Z_GuardLanded): the "inside the zone" markers must not stay set
void PS2Spill_Reset(void)
{
	spill_busy = 0;
	spill_inhibit = 0;
}

void PS2Spill_Stats(size_t *now, size_t *peak, size_t *blocks, size_t *total, size_t *fail, size_t *foreign)
{
	*now = spill_now;
	*peak = spill_peak;
	*blocks = spill_blocks;
	*total = spill_total;
	*fail = spill_fail;
	*foreign = spill_foreign;
}
#else
void PS2Spill_Init(void) {}
void PS2Spill_Reset(void) {}
void PS2Spill_Stats(size_t *now, size_t *peak, size_t *blocks, size_t *total, size_t *fail, size_t *foreign)
{
	*now = *peak = *blocks = *total = *fail = *foreign = 0;
}
#endif
