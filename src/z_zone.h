// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1993-1996 by id Software, Inc.
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2023 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  z_zone.h
/// \brief Zone Memory Allocation, perhaps NeXT ObjectiveC inspired

#ifndef __Z_ZONE__
#define __Z_ZONE__

#include <stdio.h>
#include "doomdef.h"
#include "doomtype.h"

#ifdef __GNUC__ // __attribute__ ((X))
#if (__GNUC__ > 4) || (__GNUC__ == 4 && (__GNUC_MINOR__ >= 3 || (__GNUC_MINOR__ == 2 && __GNUC_PATCHLEVEL__ >= 5)))
#define FUNCALLOC(X) __attribute__((alloc_size(X)))
#endif // odd, it is documented in GCC 4.3.0 but it exists in 4.2.4, at least
#endif

#ifndef FUNCALLOC
#define FUNCALLOC(x)
#endif

//#define ZDEBUG

//
// Purge tags
//
// Now they are an enum! -- Monster Iestyn 15/02/18
//
enum
{
	// Tags < PU_LEVEL are not purged until freed explicitly.
	PU_STATIC                = 1, // static entire execution time
	PU_LUA                   = 2, // static entire execution time -- used by lua so it doesn't get caught in loops forever
	PU_PERFSTATS             = 3, // static between changes to ps_samplesize cvar

	PU_SOUND                 = 11, // static while playing
	PU_MUSIC                 = 12, // static while playing

	PU_PATCH                 = 14, // static entire execution time
	PU_PATCH_LOWPRIORITY     = 15, // lower priority patch, static until level exited
	PU_PATCH_ROTATED         = 16, // rotated patch, static until level exited or WAD added
	PU_PATCH_DATA            = 17, // patch data, lifetime depends on the patch that owns it
	PU_SPRITE                = 18, // sprite patch, static until WAD added
	PU_HUDGFX                = 19, // HUD patch, static until WAD added

	PU_HWRPATCHINFO          = 21, // Hardware GLPatch_t struct for OpenGL texture cache
	PU_HWRPATCHCOLMIPMAP     = 22, // Hardware GLMipmap_t struct colormap variation of patch
	PU_HWRMODELTEXTURE       = 23, // Hardware model texture
	PU_HWRLIGHTTABLEDATA     = 24, // Hardware light table data
	PU_HWRBATCH              = 25, // persistent CPU-side batching arrays; never a purgeable texture cache
#ifdef PS2_PROFILE
	PU_RENDERWORK            = 26, // pinned renderer construction scratch, allocated beside reconstructible caches
	PU_HWRCACHE_LRU          = 27, // OPT12 HWDRV (PS2-HW-442): data of the hardware renderer's textures kept between uses: evicted least recently used first (a cache block), and freed at once by an allocation that
	                               // nothing else can serve (what PU_HWRCACHE_UNLOCKED is freed by always); Z_ChangeTag turns PU_HWRCACHE_UNLOCKED into this one while Z_SetHWCacheLRU is on
#endif

	PU_HWRCACHE              = 48, // static until unlocked
	PU_CACHE                 = 49, // static until unlocked

	// Tags s.t. PU_LEVEL <= tag < PU_PURGELEVEL are purged at level start
	PU_LEVEL                 = 50, // static until level exited
	PU_LEVSPEC               = 51, // a special thinker in a level
	PU_HWRPLANE              = 52, // if ZPLANALLOC is enabled in hw_bsp.c, this is used to alloc polygons for OpenGL

	// Tags >= PU_PURGELEVEL are purgable whenever needed
	PU_PURGELEVEL            = 100, // Note: this is never actually used as a tag
	PU_CACHE_UNLOCKED        = 101, // Note: unused
	PU_HWRCACHE_UNLOCKED     = 102, // 'unlocked' PU_HWRCACHE memory:
									// 'second-level' cache for graphics
                                    // stored in hardware format and downloaded as needed
	PU_HWRMODELTEXTURE_UNLOCKED = 103, // 'unlocked' PU_HWRMODELTEXTURE memory
};

//
// Zone memory initialisation
//
void Z_Init(void);

//
// Zone memory allocation
//
// enable ZDEBUG to get the file + line the functions were called from
// for ZZ_Alloc, see doomdef.h
//

// Z_Free and alloc with alignment
#ifdef ZDEBUG
#define Z_Free(p)                 Z_Free2(p, __FILE__, __LINE__)
#define Z_MallocAlign(s,t,u,a)    Z_Malloc2(s, t, u, a, __FILE__, __LINE__)
#define Z_CallocAlign(s,t,u,a)    Z_Calloc2(s, t, u, a, __FILE__, __LINE__)
#define Z_ReallocAlign(p,s,t,u,a) Z_Realloc2(p,s, t, u, a, __FILE__, __LINE__)
void Z_Free2(void *ptr, const char *file, INT32 line);
void *Z_Malloc2(size_t size, INT32 tag, void *user, INT32 alignbits, const char *file, INT32 line) FUNCALLOC(1);
void *Z_Calloc2(size_t size, INT32 tag, void *user, INT32 alignbits, const char *file, INT32 line) FUNCALLOC(1);
void *Z_Realloc2(void *ptr, size_t size, INT32 tag, void *user, INT32 alignbits, const char *file, INT32 line) FUNCALLOC(2);
#else
void Z_Free(void *ptr);
void *Z_MallocAlign(size_t size, INT32 tag, void *user, INT32 alignbits) FUNCALLOC(1);
void *Z_CallocAlign(size_t size, INT32 tag, void *user, INT32 alignbits) FUNCALLOC(1);
void *Z_ReallocAlign(void *ptr, size_t size, INT32 tag, void *user, INT32 alignbits) FUNCALLOC(2);
#endif

// Alloc with standard alignment
#define Z_Malloc(s,t,u)    Z_MallocAlign(s, t, u, sizeof(void *))
#define Z_Calloc(s,t,u)    Z_CallocAlign(s, t, u, sizeof(void *))
#define Z_Realloc(p,s,t,u) Z_ReallocAlign(p, s, t, u, sizeof(void *))

// Free all memory by tag
// these don't give line numbers for ZDEBUG currently though
// (perhaps this should be changed in future?)
#define Z_FreeTag(tagnum) Z_FreeTags(tagnum, tagnum)
void Z_FreeTags(INT32 lowtag, INT32 hightag);
#ifdef PS2
// NULL on physical exhaustion after normal cache retries; invalid requests still report programming errors.
// A failed request leaves the supplied owner unchanged. Other eligible caches may have been evicted.
void *Z_TryMallocAlign(size_t size, INT32 tag, void *user, INT32 alignbits) FUNCALLOC(1);
void *Z_TryReallocAlign(void *ptr, size_t size, INT32 tag, void *user, INT32 alignbits); // NULL (old block untouched) when there is no room
void Z_PinCachePatch(void *ptr); // PS2-140: a PU_CACHE patch that got a hardware texture stops being evictable
void Z_PurgeLock(boolean lock); // nestable: current-frame roots protected; earlier-frame caches may be evicted
void Z_NextFrame(void); // frame boundary (once per displayed frame): blocks used since the last call become evictable
void Z_SetHWCacheLRU(boolean on, size_t freemin, size_t cap); // OPT12 HWDRV (PS2-HW-442): tag changes to PU_HWRCACHE_UNLOCKED (with an owner) make an LRU cache block (PU_CACHE) instead of a block that goes at the next allocation that does not fit: at most `cap` bytes a frame, while freemin bytes of the arena are free
INT32 Z_HWCacheTag(size_t bytes); // the tag a hardware texture cache block of this size gets now: PU_CACHE or PU_HWRCACHE_UNLOCKED
void Z_Touch(void *ptr); // allocation root, never an interior pointer: used this frame (Z_ChangeTag/Z_SetUser do it too)
void Z_ReleaseCache(void *ptr); // root only, after all aliases consumed: enables pressure eviction in this frame
typedef struct { void *chunk; unsigned used; unsigned epoch; } zlevelpool_t;
void *Z_LevelPoolAlloc(zlevelpool_t *pool, size_t size, unsigned perchunk); // OPT12-CORE (PS2-511): zeroed, 16-byte aligned, PU_LEVEL, never freed alone
void Z_AgeCache(void *ptr, UINT32 frames); // OPT12-CORE (PS2-510): like Z_ReleaseCache, but the block counts as `frames` frames old: it goes before what the last frame used
void Z_FlushCache(void); // P_LoadLevel, nothing held: every owner-backed cache block (PU_CACHE, evictable sprites) goes
void Z_LevelPhase(boolean playing); // P_SetupLevel: false while the level loads, true from its end (PU_LEVEL blocks then come from the long-lived end)
UINT32 Z_FrameCount(void);
size_t Z_ArenaFree(void); // free bytes of the arena (the sum of all free blocks, not one contiguous block)
size_t Z_ArenaCapacity(void); // PS2-600 (OPT13 IZ): bytes of the arena: what the behaviour of a subsystem should follow, not the free bytes of the moment
size_t Z_ReclaimableBytes(void); // PS2-600: the free bytes and the caches that could go now (PU_CACHE, evictable sprites, PU_HWRCACHE_LRU): the room a big request can get
void Z_ModeProf(unsigned int frames); // PS2-600: the "ZMODE" line of an HWPROF window (what the zone policy did, EE cycles), counters back to 0
size_t Z_RenderHeadroom(void); // configured contiguous workspace target, also reserved from optional precaching
// PS2-71: a subsystem that keeps rebuildable memory outside the zone caches (the audio effects cache, tag PU_SOUND) registers a hook. The zone calls
// it, game thread only, when an allocation does not fit and no cache block is left to evict: the hook frees what it can (returns the bytes,
// 0 = nothing) by calling Z_Free, and may free any number of its own blocks; it must not allocate.
typedef size_t (*z_reclaim_fn)(size_t want);
void Z_SetReclaimHook(z_reclaim_fn fn);
void Z_AddReclaimHook(z_reclaim_fn fn); // PS2-170: further subsystems (the GS renderer's data cache), up to 3, asked after the audio cache gave nothing
void Z_RemoveReclaimHook(z_reclaim_fn fn);

// PS2-170 (OPT11-STAB): recoverable out-of-memory. Code that can be abandoned half way (the drawing of a frame, the hardware part of a level load) arms a guard:
//   zguard_t g;
//   if (Z_GUARD_TRY(&g)) { ...the work...; Z_GuardPop(&g); }
//   else { Z_GuardLanded(&g); ...g.size/g.tag/g.reason say what happened: free what can be freed, switch renderer, give up the level...; }
// A Z_Malloc that finds no room (after all evictions) then jumps back instead of ending the program (I_Error). The work that was running is gone half way:
// whatever it held (locks, flags, partly built tables) must be reset by the landing code; locals that are changed between the arm and the jump and read
// after it must be volatile. No jump happens while a Lua hook runs (its state cannot be abandoned): the fatal report is as before.
#include <setjmp.h>
typedef struct zguard_s
{
	jmp_buf jb;
	struct zguard_s *prev;
	INT32 lock;       // the render lock count when armed
	size_t size;      // the failing request (0: a thrown reason)
	INT32 tag;
	char reason[96];
} zguard_t;
void Z_GuardPush(zguard_t *g);
#define Z_GUARD_TRY(g) (Z_GuardPush(g), setjmp((g)->jb) == 0)
void Z_GuardPop(zguard_t *g);
void Z_GuardLanded(zguard_t *g);
boolean Z_GuardArmed(void);
boolean Z_GuardThrow(const char *reason); // false when no guard is armed (or a Lua hook is running): the caller reports the error as before
UINT32 Z_GuardAllocs(void); // allocations made while a guard was armed (the size of the fault injection test, -zoomnth)
UINT32 Z_GuardRecovered(void); // how many times the allocator went back to a guard
void Z_OutOfMemoryFatal(size_t size, INT32 tag, size_t align);
size_t Z_EmergencyFree(void); // after a frame was abandoned: every cache block, the reclaim hooks; returns the free bytes
boolean PS2Lua_InCall(void); // a Lua call is running (lua_script.c; false without Lua)
void PS2Spill_Reset(void); // ps2_spill.c: the libc-to-arena spill bookkeeping after a jump out of an allocation
#elif defined(PS2_PROFILE)
// The host profile uses the unchanged host allocator; render lock and frame calls have no effect there.
static inline void Z_PurgeLock(boolean lock) { (void)lock; }
static inline void Z_NextFrame(void) {}
static inline void Z_Touch(void *ptr) { (void)ptr; }
static inline void Z_SetHWCacheLRU(boolean on, size_t freemin, size_t cap) { (void)on; (void)freemin; (void)cap; }
static inline INT32 Z_HWCacheTag(size_t bytes) { (void)bytes; return PU_HWRCACHE_UNLOCKED; }
static inline void Z_ReleaseCache(void *ptr) { (void)ptr; }
static inline void Z_AgeCache(void *ptr, UINT32 frames) { (void)ptr; (void)frames; }
static inline void Z_LevelPhase(boolean playing) { (void)playing; }
static inline void Z_FlushCache(void) {}
static inline size_t Z_ArenaFree(void) { return (size_t)-1; }
static inline size_t Z_ArenaCapacity(void) { return (size_t)-1; }
static inline size_t Z_ReclaimableBytes(void) { return (size_t)-1; }
static inline void Z_ModeProf(unsigned int frames) { (void)frames; }
#endif

// Iterate memory by tag
#define Z_IterateTag(tagnum, func) Z_IterateTags(tagnum, tagnum, func)
void Z_IterateTags(INT32 lowtag, INT32 hightag, boolean (*iterfunc)(void *));

//
// Utility functions
//
void Z_CheckMemCleanup(void);
void Z_CheckHeap(INT32 i);

//
// Zone memory modification
//
// enable PARANOIA to get the file + line the functions were called from
//
#ifdef PARANOIA
#define Z_ChangeTag(p,t) Z_ChangeTag2(p, t, __FILE__, __LINE__)
#define Z_SetUser(p,u)   Z_SetUser2(p, u, __FILE__, __LINE__)
void Z_ChangeTag2(void *ptr, INT32 tag, const char *file, INT32 line);
void Z_SetUser2(void *ptr, void **newuser, const char *file, INT32 line);
#else
void Z_ChangeTag(void *ptr, INT32 tag);
void Z_SetUser(void *ptr, void **newuser);
#endif

//
// Zone memory usage
//
// Note: These give the memory used in bytes,
// shift down by 10 to convert to KB
//
#define Z_TagUsage(tagnum) Z_TagsUsage(tagnum, tagnum)
size_t Z_TagsUsage(INT32 lowtag, INT32 hightag);
#define Z_TotalUsage() Z_TagsUsage(0, INT32_MAX)

//
// Miscellaneous functions
//
char *Z_StrDup(const char *in);
#define Z_Unlock(p) (void)p // TODO: remove this now that NDS code has been removed

#endif
