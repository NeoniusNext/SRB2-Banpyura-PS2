// SONIC ROBO BLAST 2 (PS2 port)
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  w_pack.h
/// \brief SRP2 cooked packs (docs/PACK_FORMAT.md): lumpinfo table straight from the pack, LZ4 lump reader

#ifndef __W_PACK__
#define __W_PACK__

#include "doomtype.h"
#include "w_wad.h"

#define WPACK_BLOCK 65536 // decoded block size of lumps bigger than this

// True if the file starts with the pack signature. This performs I/O: set up buffering before calling it.
boolean WPack_Detect(FILE *handle);

// Prepares a stream for the engine. A pack (the file starts with the signature) is read below stdio through a window whose size depends on the medium `path` is on
// (OPT13-IO RS-01: docs/GATES/g1/opt13-IO.md; WPack_SetMedium), its stdio stream is unbuffered; any other file gets a 64 KiB stdio buffer (64-byte aligned on PS2).
// Returns something free() accepts, to be freed after fclose(); NULL if memory is short.
// Call before ANY I/O, including WPack_Detect. Does not replace driver cache sync. `path` (may be NULL) is the name the file was opened with.
void *WPack_SetupHandleEx(FILE *handle, const char *path);
void *WPack_SetupHandle(FILE *handle);
// The size of the file of a pack stream (read once when the stream was prepared), -1 if it is not a pack stream or the size is not known.
long WPack_FileSize(FILE *handle);

// The medium the packs are on: "dvd", "usb", "sd", "hdd" fix the window policy; NULL or "auto" chooses by the device name of the path (mass:/usb -> usb, mx4sio -> sd,
// hdd/pfs -> hdd, anything else -> dvd, whose 64 KiB window is never worse than the stdio buffer it replaced). False for an unknown name.
boolean WPack_SetMedium(const char *name);
// Experiments (-pkwin): one window for every medium (bytes: buffer size <= 64 KiB, smallest read of a miss, read of a miss in a sequential run, how far in front of the last read
// a miss may be to count as sequential)
void WPack_SetWindow(UINT32 cap, UINT32 minreq, UINT32 ahead, UINT32 gap);

// Counters of the device commands of the pack reader (a command = one read() of the IOP; seeks are not counted)
typedef struct { UINT32 cmds, hits, misses, bulk, retries, failures, injected; UINT64 bytes; } wpack_iostat_t;
void WPack_GetStats(wpack_iostat_t *out);
void WPack_PrintStats(boolean on); // print the counters when the packs are closed (-iostat)
// RS-07 test: the `at`-th device read attempt (1 = the first one) and the `count` - 1 after it fail as if the device returned an error (0 = off; -pkioerr)
void WPack_InjectErrors(UINT32 at, UINT32 count);
// RS-02: the working set of a level in one sorted pass (docs/GATES/g1/opt13-IO.md). Begin; Add for every lump the level will probably use (the lumpinfo of a lump of the pack
// whose stream is `handle`); Run reads them sorted by position into one PU_CACHE block (at most `budget` stored bytes; the zone takes the block back under pressure) and
// returns the bytes kept; `pump` (may be NULL) is called between the device reads. Later reads of those lumps come from the block while it exists.
void WPack_PrefetchBegin(void);
void WPack_PrefetchAdd(FILE *handle, const lumpinfo_t *l);
UINT32 WPack_PrefetchRun(UINT32 budget, void (*pump)(void));
void WPack_PrefetchDrop(void); // frees the block (tests; the zone does it under pressure)
void WPack_PrefetchStats(UINT32 *hits, UINT32 *hitbytes, UINT32 *ranges, UINT32 *kept);
// Why the last WPack_ReadLump returned less than asked (pack name, offset, errno); empty if it did not fail
const char *WPack_LastError(void);

typedef struct wpack_s wpack_t; // an open pack of version 2 (head table); NULL for version 1

// Builds lumpinfo_t[] exactly as ResGetLumpsZip does for the pk3 the pack was cooked from (same type RET_PK3,
// same fields; name/longname/fullname point into one string pool, *pool, which is a single Z_Malloc block).
// nonmusic is the cooker's W_VerifyNMUSlumps result (true = the pack has other than music/sound lumps).
// filename is only for the messages. *pack (may be NULL) receives the context of a v2 pack (register it with WPack_Register, free it with WPack_Close).
// The index is checked (v2: checksums of the table, the string pool and the head table). Returns NULL (after a console alert naming the pack) if the pack is damaged.
lumpinfo_t *WPack_GetLumps(FILE *handle, const char *filename, UINT16 *nlmp, void **pool, boolean *nonmusic, wpack_t **pack);
void WPack_Register(wpack_t *pack);
void WPack_Close(wpack_t *pack);
// The start-up is over: frees the head tables of all open packs
void WPack_DropHeads(void);
// Like WPack_ReadLump; a request that lies in the first bytes of lump number lumpindex is answered from the head table without any file access
size_t WPack_ReadLumpN(wpack_t *pack, FILE *handle, UINT32 lumpindex, const lumpinfo_t *l, void *dest, size_t size, size_t offset);
// -verifypack: decodes every lump and compares its CRC32 with the table in the pack; returns the number of damaged lumps; report(lump, what) is called for each
UINT32 WPack_Verify(wpack_t *pack, FILE *handle, const lumpinfo_t *lumps, UINT32 numlumps, void (*report)(UINT32 lump, const char *what));

// W_VerifyNMUSlumps for a pack: 1 = only music/sound lumps, 0 = other lumps, -1 = not a pack / unreadable.
int WPack_VerifyNMUS(FILE *handle);

// Reads size bytes at offset of the (decoded) lump into dest; same contract as the CM_NOCOMPRESSION read of
// W_ReadLumpHeaderPwad: size and offset are already clamped to the lump. Handles raw and CM_LZ4 pack lumps.
// File reads use a 64-byte aligned bounce buffer on PS2 and sector-aligned, sector-multiple requests; dest need not
// be aligned. Valid packs are sector-padded. Returns bytes delivered (== size unless damaged). Not reentrant.
size_t WPack_ReadLump(FILE *handle, const lumpinfo_t *l, void *dest, size_t size, size_t offset);

// Frees the I/O and decode buffers (W_Shutdown).
void WPack_Shutdown(void);

#endif // __W_PACK__
