#ifndef NEXT2_LOOKUP_COMMON_H
#define NEXT2_LOOKUP_COMMON_H
#include "doomdef.h"
#include "w_wad.h"
#include "z_zone.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <assert.h>
#include <time.h>

typedef struct
{
	UINT32 cursor, hash[64], number[64];
	char name[64][32];
} next2_cache_state;

#define DECLARE_LOOKUP(tag) \
void tag##_index(wadfile_t *wf); \
UINT16 tag##_long(const char *name, UINT16 wad, UINT16 start); \
UINT16 tag##_patch(const char *name, UINT16 wad, boolean longname); \
void tag##_cache_reset(void); \
void tag##_cache_add(lumpnum_t lump, const char *name, boolean longname); \
lumpnum_t tag##_cache_check(const char *name, boolean longname); \
void tag##_cache_state(next2_cache_state *state); \
size_t tag##_cache_bytes(void)
DECLARE_LOOKUP(reference);
DECLARE_LOOKUP(candidate);
#undef DECLARE_LOOKUP
#endif
