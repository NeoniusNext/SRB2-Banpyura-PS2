// SRB2 PS2 port: zone arena (D4) and memory reporting.
//
// One big block taken at start-up; EE blocks carry a 16-byte header (32 with ZDEBUG), free space is kept in
// size-segregated lists with boundary tags, so neighbours coalesce in O(1). z_zone.c owns the tag/user/eviction
// policy on top of this; nothing here knows about purging.

#ifndef __PS2_MEM_H__
#define __PS2_MEM_H__

#include <stddef.h>
#include <stdint.h>

#ifdef _MSC_VER // host test only (x86 MSVC): the header must be a multiple of 16 bytes there as well
#pragma warning(disable : 4324) // padded by align(16): intended
#define ZA_ALIGN16_PRE __declspec(align(16))
#define ZA_ALIGN16_POST
#else
#define ZA_ALIGN16_PRE
#define ZA_ALIGN16_POST __attribute__((aligned(16)))
#endif

// Block header. Free blocks reuse it as {sf, next, prev} and keep a copy of the size in their last 4 bytes.
typedef ZA_ALIGN16_PRE struct zablock_s
{
	uint32_t sf;        // total block bytes (header included, multiple of 16) | ZAF_* flags
	void **user;        // owner pointer cleared when the block goes away
	uint32_t realsize;  // payload bytes the caller asked for
	uint32_t tagstamp;  // purge tag (8 bits) | frame stamp (24 bits)
#ifdef ZDEBUG
	const char *ownerfile;
	int ownerline;
#endif
}
ZA_ALIGN16_POST
zablock_t;

#if UINTPTR_MAX == UINT32_MAX
#ifdef ZDEBUG
_Static_assert(sizeof (zablock_t) == 32, "PS2 debug zone header must be 32 bytes");
#else
_Static_assert(sizeof (zablock_t) == 16, "PS2 release zone header must be 16 bytes (D4)");
#endif
#endif

#define ZAF_USED     1u
#define ZAF_PREVFREE 2u  // the block before this one is free (its size is in the 4 bytes below this header)
#define ZAF_REDZONE  4u  // bytes between payload end and block end are a guard pattern
#define ZAF_VALID    8u  // always set: cheap sanity bit
#define ZAF_MASK     15u

#define ZA_HDR       ((size_t)sizeof (zablock_t))
#define ZA_MINBLK    32u
#define ZA_MAXTAG    255
#define ZA_MAXALIGN  ((size_t)1 << 31)
#define ZA_GUARD     0xA5

#define ZA_SIZE(b)   ((size_t)((b)->sf & ~ZAF_MASK))
#define ZA_ISFREE(b) (!((b)->sf & ZAF_USED))
#define ZA_TAG(b)    ((int)((b)->tagstamp & 0xFFu))
#define ZA_STAMP(b)  ((b)->tagstamp >> 8)
static inline void ZA_SetTag(zablock_t *b, int tag) { b->tagstamp = (b->tagstamp & ~0xFFu) | (uint32_t)tag; }
static inline void ZA_SetStamp(zablock_t *b, uint32_t frame) { b->tagstamp = (b->tagstamp & 0xFFu) | (frame << 8); }
#define ZA_PAYLOAD(b) ((void *)((uint8_t *)(b) + ZA_HDR))
#define ZA_BLOCK(p)   ((zablock_t *)((uint8_t *)(p) - ZA_HDR))

// Which end of a free block a request is carved from (long-lived data from the top, the rest from the bottom).
enum { ZA_BOTTOM = 0, ZA_TOP = 1 };

typedef struct
{
	size_t arena;        // bytes in the arena
	size_t used;         // bytes in used blocks (headers and padding included)
	size_t freebytes;
	size_t largestfree;  // largest free block (header included)
	size_t freeblocks;
	size_t usedblocks;
	size_t peakused;     // high-water mark of used since ZA_ResetPeak (level exit)
	size_t globalpeak;   // high-water mark since start
	size_t allocs, frees, failures;
	size_t binsearch;    // free blocks examined by allocations (cost metric)
	size_t evictions, evictedbytes; // blocks the zone evicted from the cache (ZA_NoteEvict)
} zastats_t;

// Policy knobs, settable before ZA_Init.
extern int za_twosided;  // 1: carve ZA_TOP requests from the high end of the chosen block (default), 0: always low end
extern int za_prefer;    // fitting free blocks compared by address for the requested side (default 16; 1 = first fit)
extern int za_smallfit;  // requests of at most this many bytes take the smallest fitting free block (default 1024; 0 = address order for all)
extern int za_redzone;   // reserve a guard of ZA_GUARD_MIN bytes after every payload
#define ZA_GUARD_MIN 16u

// Takes `bytes` (rounded down to 16) from `base` (must be 64-byte aligned). Returns 0 on success.
int ZA_InitMem(void *base, size_t bytes);
// Takes the arena from the C heap with memalign(64): contiguous growth minus `reserve` and 64 KiB metadata allowance (capped by `cap`
// bytes when non-zero). Returns the arena size, 0 on failure.
size_t ZA_InitHeap(size_t reserve, size_t cap);
int ZA_Ready(void);
void ZA_Shutdown(void); // host test only: forget the arena (the caller owns the memory)

// NULL when no free block fits. `align` is a power of two >= 16.
void *ZA_Alloc(size_t size, size_t align, int side);
// Resize without moving, consuming only the following free block. Failure leaves the block intact.
int ZA_Resize(void *payload, size_t size);
// Returns the address just past the free block the freed block ended up in (walkers continue from there).
void *ZA_Free(void *payload);
void ZA_SetFrontier(void *addr); // PS2-75: short-lived requests are placed below, long-lived ones above (NULL: no zones); two-sided policy only
void *ZA_Frontier(void);
zablock_t *ZA_PrevFree(zablock_t *b); // PS2-76: the free block that ends where b starts (b's header says so), NULL when the block before b is used
zablock_t *ZA_LargestFreeBlock(void); // NULL when full
void ZA_NoteEvict(size_t bytes); // statistics only
void ZA_ResetPeak(void);

zablock_t *ZA_First(void);
zablock_t *ZA_Next(zablock_t *b); // NULL after the last block
zablock_t *ZA_BlockAt(void *addr); // the block starting at addr (a value ZA_Free returned), NULL at the arena end
size_t ZA_FreeBytes(void);
size_t ZA_LargestFree(void); // largest free block, header included (0 when full)
void ZA_Stats(zastats_t *st);
// 0 if consistent, else a message in `msg` (structure, coalescing, free lists, red zones).
int ZA_Check(char *msg, size_t msglen);
size_t ZA_BinCount(void);

// Reporting (console command "ps2_mem" and the OOM path). Defined here, they only read the arena.
void PS2Mem_Init(void);                 // registers "ps2_mem"
void PS2Mem_Line(const char *label);    // one-line summary for logs
void PS2Mem_Checkpoint(const char *name); // -zck: one line of usage by group (level loader stages)
void PS2Mem_Report(int owners);         // usage by tag + arena health + C heap; owners>0 adds the top call sites (ZDEBUG)
void PS2Mem_FreeList(size_t minbytes);  // free blocks of at least minbytes with their neighbours ("[zfree]" lines)
void PS2Mem_Map(size_t maxruns);        // address-ordered runs of free/level/static/cache/work blocks ("[zmap]" lines)
size_t PS2Mem_LibcFree(void);           // bytes the C heap can still give (EE: up to the stack plus free chunks; host: probes with malloc, slow)
const char *PS2Mem_TagName(int tag);

// RAM profile: 32 (retail) or 128 (kernel-reported Dev/TOOL/PCSX2 RAM). -zram can lower policy, never raise detected RAM.
size_t PS2Mem_RamBytes(void);
// Pure budget arithmetic, also exercised by host tests. Addresses are physical EE addresses.
size_t PS2Mem_HeapAvailable(size_t ram, size_t stackbase, size_t brk, size_t freechunks);
size_t PS2Mem_HeapLimit(void); // actual main-thread stack base (no RAM address probes)
int PS2Mem_RamClass(void);
void PS2Mem_Frame(void);  // once per displayed frame (Z_NextFrame): -zquit N / -zquitall N end the run with a report
void PS2Mem_Sizes(void);  // -zsizes: sizeof of the level and renderer structures
unsigned PS2Mem_Cycles(void); // COP0 Count (0 off the EE)
unsigned PS2Mem_Ms(void);     // EE milliseconds since boot (64-bit bus clock, no wrap; 0 off the EE)
size_t PS2Mem_LibcPeak(void); // highest C heap break above the arena since start (EE; 0 elsewhere)
size_t PS2Mem_StackUsed(void); // deepest main thread stack use (EE, with -zstack; 0 otherwise)
unsigned Z_TestFlushes(void); // z_zone.c: forced cache flushes done by -zflush
void Z_ReportCosts(void);  // z_zone.c: "[zcost]" line, what the eviction machinery cost in EE cycles

#endif
