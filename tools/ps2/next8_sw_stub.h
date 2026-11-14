#ifndef NEXT8_SW_STUB_H
#define NEXT8_SW_STUB_H
#include <stddef.h>
#include <string.h>
#include "m_fixed.h"
#define TRANSPARENTPIXEL 255
/* Exact Software viddef_t layout from screen.h, including fields unused here. */
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
} next8_vid_t;
extern next8_vid_t vid;
extern INT32 ds_y, ds_x1, ds_x2, ds_bgofs, ds_waterofs;
extern UINT8 *topleft, *screens[5];
extern UINT8 *ds_source, *ds_colormap, *ds_transmap, *ds_translation;
extern fixed_t ds_xfrac, ds_yfrac, ds_xstep, ds_ystep;
extern UINT16 ds_flatwidth, ds_flatheight;
#define NEXT8_DECLARE(name) void reference_##name(void); void candidate_##name(void)
NEXT8_DECLARE(R_DrawTranslucentSpan_NPO2_8);
NEXT8_DECLARE(R_DrawTranslucentSplat_NPO2_8);
NEXT8_DECLARE(R_DrawTranslucentFloorSprite_NPO2_8);
NEXT8_DECLARE(R_DrawWaterSpan_NPO2_8);
#undef NEXT8_DECLARE
#endif
