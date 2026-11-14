#ifndef NEXT3_RENDER_TILTED_STUB_H
#define NEXT3_RENDER_TILTED_STUB_H
#include <stddef.h>
#include <string.h>
#include "m_fixed.h"
#define MAXVIDWIDTH 640
#define MAXLIGHTSCALE 48
#define BASEVIDWIDTH 320
#define TRANSPARENTPIXEL 255
#define SPANSIZE 16
#define INVSPAN 0.0625f
#define PS2SUB_ADD(a,b) ((void)0)
#define PS2SUB_N(a) ((void)0)
typedef UINT8 lighttable_t;
typedef struct { INT32 width, height, rowbytes; } next3_vid_t;
extern next3_vid_t vid;
extern INT32 centerx, centery, ds_y, ds_x1, ds_x2, ds_bgofs;
extern UINT8 *topleft, *screens[2], *colormaps;
extern lighttable_t **planezlight;
extern UINT8 *ds_source, *ds_colormap, *ds_translation, *ds_transmap;
extern UINT32 nflatxshift, nflatyshift, nflatmask;
extern INT32 tiltlighting[MAXVIDWIDTH];
#ifdef PS2_OPT_SLOPE
typedef struct { float x, y, z; } next3_vector_t;
typedef float slopereal_t;
extern float ds_lightscale;
#define SLOPE_U32(x) R_SlopeToU32(x)
#else
typedef struct { double x, y, z; } next3_vector_t;
typedef double slopereal_t;
extern double zeroheight;
extern fixed_t fovtan;
#define SLOPE_U32(x) ((UINT32)(INT64)(x))
#endif
extern next3_vector_t ds_su, ds_sv, ds_sz, ds_slopelight;
#define NEXT3_DECLARE(name) void reference_##name(void); void candidate_##name(void)
NEXT3_DECLARE(R_DrawTiltedTranslucentSpan_8);
NEXT3_DECLARE(R_DrawTiltedWaterSpan_8);
NEXT3_DECLARE(R_DrawTiltedSplat_8);
NEXT3_DECLARE(R_DrawTiltedTranslucentSplat_8);
NEXT3_DECLARE(R_DrawTiltedFloorSprite_8);
NEXT3_DECLARE(R_DrawTiltedTranslucentFloorSprite_8);
#undef NEXT3_DECLARE
#endif
