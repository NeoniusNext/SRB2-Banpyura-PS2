#ifndef NEXT4_RENDER_SPRITES_STUB_H
#define NEXT4_RENDER_SPRITES_STUB_H
#include <stddef.h>
#include <string.h>
#include "m_fixed.h"
#define MAXVIDWIDTH 640
#define TRANSPARENTPIXEL 255
#define PS2SUB_ADD(a,b) ((void)0)
#define PS2SUB_N(a) ((void)0)
typedef struct { INT32 width, height, rowbytes; } next4_vid_t;
extern next4_vid_t vid;
extern INT32 ds_y, ds_x1, ds_x2;
extern UINT8 *topleft, *screens[2];
extern UINT8 *ds_source, *ds_colormap, *ds_translation, *ds_transmap;
extern fixed_t ds_xfrac, ds_yfrac, ds_xstep, ds_ystep;
extern UINT32 nflatxshift, nflatyshift, nflatmask, nflatshiftup;
#define NEXT4_DECLARE(name) void reference_##name(void); void candidate_##name(void); \
    void hoists_##name(void); void advance_##name(void)
NEXT4_DECLARE(R_DrawSplat_8);
NEXT4_DECLARE(R_DrawTranslucentSplat_8);
NEXT4_DECLARE(R_DrawFloorSprite_8);
NEXT4_DECLARE(R_DrawTranslucentFloorSprite_8);
#undef NEXT4_DECLARE
#endif
