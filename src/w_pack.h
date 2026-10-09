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

// Gives the stream a 64 KiB buffer (64-byte aligned on PS2). Returns the buffer, free() it after fclose().
// Call before ANY I/O, including WPack_Detect; NULL if it could not be allocated. Does not replace driver cache sync.
void *WPack_SetupHandle(FILE *handle);

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

// OPT13 IZ (PS2-602, R2). The stored form of a lump as it lies in the file (l->disksize bytes into dest, which needs no alignment): returns the bytes read, 0 on error.
size_t WPack_ReadRaw(FILE *handle, const lumpinfo_t *l, void *dest);
// Decodes a lump from its stored form in memory (compression: CM_NOCOMPRESSION or CM_LZ4 as in lumpinfo_t; size: the decoded size), into dest (size bytes, no alignment needed). False on damage.
boolean WPack_DecodeMem(int compression, const void *src, UINT32 disksize, void *dest, UINT32 size);
// The content identity of a version 2 pack: the checksums of its table, its string pool and its CRC table (a pack cooked from other lumps has other ones). False for a version 1 pack (pack NULL).
boolean WPack_Identity(const wpack_t *pack, UINT32 id[3]);

// Frees the I/O and decode buffers (W_Shutdown).
void WPack_Shutdown(void);

#endif // __W_PACK__
