#ifndef NEXT7_RENDER_FOG_STUB_H
#define NEXT7_RENDER_FOG_STUB_H
#include <stddef.h>
#include <string.h>
#include "m_fixed.h"
#define MAXVIDWIDTH 640
#define MAXLIGHTSCALE 48
#define BASEVIDWIDTH 320
typedef UINT8 lighttable_t;
/* Match screen.h's complete Software viddef_t field layout and size. This
 * matters to actual EE addressing and scheduling, not just C value semantics. */
typedef struct {
	INT32 modenum;
	UINT8 *buffer;
	size_t rowbytes;
	INT32 width, height;
	union { INT32 numpages, windowed; } u;
	INT32 recalc;
	UINT8 *direct;
	INT32 dup, fdup, bpp, baseratio;
	void *WndParent;
	UINT8 smalldup, meddup;
} next7_vid_t;
extern next7_vid_t vid;
extern INT32 centerx, centery, ds_y, ds_x1, ds_x2, ds_bgofs;
extern INT32 dc_x, dc_yl, dc_yh;
extern UINT8 *topleft, *screens[5], *colormaps;
extern lighttable_t **planezlight;
extern UINT8 *ds_source, *ds_colormap, *ds_transmap, *dc_colormap;
extern INT32 tiltlighting[MAXVIDWIDTH];
#ifdef PS2_OPT_SLOPE
typedef struct { float x, y, z; } next7_vector_t;
extern float ds_lightscale;
#else
typedef struct { double x, y, z; } next7_vector_t;
extern double zeroheight;
extern fixed_t fovtan;
#endif
extern next7_vector_t ds_slopelight;
#define NEXT7_DECLARE(name) void reference_##name(void); void candidate_##name(void)
NEXT7_DECLARE(R_DrawTiltedFogSpan_8);
NEXT7_DECLARE(R_DrawTransSolidColorSpan_8);
NEXT7_DECLARE(R_DrawTiltedSolidColorSpan_8);
NEXT7_DECLARE(R_DrawTiltedTransSolidColorSpan_8);
NEXT7_DECLARE(R_DrawWaterSolidColorSpan_8);
NEXT7_DECLARE(R_DrawTiltedWaterSolidColorSpan_8);
NEXT7_DECLARE(R_DrawFogColumn_8);
#undef NEXT7_DECLARE
#endif
