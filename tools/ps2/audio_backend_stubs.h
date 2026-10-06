// Host-only engine/audsrv boundary for compiling the production i_sound.c.
#ifndef AUDIO_BACKEND_STUBS_H
#define AUDIO_BACKEND_STUBS_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "ps2_audio.h"
#include "ps2_music.h"
#include <audsrv.h>
#define __attribute__(x)
#define UINT8 uint8_t
#define UINT16 uint16_t
#define UINT32 uint32_t
#define UINT64 uint64_t
#define INT16 int16_t
#define INT32 int32_t
#define INT64 int64_t
#define min(a,b) ((a)<(b)?(a):(b))
#define strncasecmp _strnicmp
#define true 1
#define false 0
#define PU_SOUND 11
#define PU_MUSIC 12
#define LUMPERROR UINT32_MAX
#define WADFILENUM(l) ((uint16_t)((l)>>16))
#define LUMPNUM(l) ((uint16_t)(l))
typedef int boolean;
typedef uint64_t precise_t;
typedef uint32_t lumpnum_t;
typedef int sfxenum_t;
enum { sfx_None, NUMSFX = 8 };
typedef struct { const char *name; void *data; size_t length; lumpnum_t lumpnum; } sfxinfo_t;
typedef enum { MU_NONE, MU_WAV, MU_MOD, MU_MID, MU_OGG, MU_MP3 } musictype_t;
enum { CM_NOCOMPRESSION, CM_LZ4 };
typedef struct { int compression; } lumpinfo_t;
typedef struct { unsigned numlumps; lumpinfo_t *lumpinfo; } wadfile_t;
extern sfxinfo_t S_sfx[NUMSFX];
extern uint16_t numwadfiles;
extern wadfile_t **wadfiles;
void CONS_Printf(const char *fmt, ...);
void I_OutputMsg(const char *fmt, ...);
boolean PS2Boot_LoadAudio(void);
precise_t I_GetPreciseTime(void);
precise_t I_GetPrecisePrecision(void);
size_t W_LumpLengthPwad(uint16_t wad, uint16_t lump);
size_t W_ReadLumpHeader(lumpnum_t lump, void *dst, size_t n, size_t off);
void W_ReadLump(lumpnum_t lump, void *dst);
const char *W_CheckNameForNum(lumpnum_t lump);
lumpnum_t S_GetSfxLumpNum(sfxinfo_t *sfx);
void *Z_TryMallocAlign(size_t n, int tag, void *user, int alignment);
void *Z_ReallocAlign(void *p, size_t n, int tag, void *user, int alignment);
typedef size_t (*z_reclaim_fn)(size_t want);
void Z_SetReclaimHook(z_reclaim_fn fn);
void Z_Free(void *p);
void I_UnloadSong(void);
void I_StopSong(void);
boolean I_SongPaused(void);
UINT32 I_GetSongPosition(void);
boolean I_FadeSong(UINT8 target, UINT32 ms, void (*callback)(void));
#endif
