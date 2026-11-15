#ifndef NEXT2_RENDER_COLUMNS_STUB_H
#define NEXT2_RENDER_COLUMNS_STUB_H
#include "m_fixed.h"
typedef UINT8 lighttable_t;
typedef struct { INT32 width, height; } testvid_t;
extern testvid_t vid;
extern UINT8 *topleft, *dc_source, *dc_translation, *dc_transmap, *colormaps;
extern lighttable_t *dc_colormap;
extern INT32 dc_x, dc_yl, dc_yh, dc_texheight;
extern fixed_t dc_iscale, dc_texturemid, centeryfrac;
void I_Error(const char *message, ...);
void reference_R_DrawShadeColumn_8(void);
void reference_R_DrawTranslatedColumn_8(void);
void reference_R_DrawTranslatedTranslucentColumn_8(void);
void candidate_R_DrawShadeColumn_8(void);
void candidate_R_DrawTranslatedColumn_8(void);
void candidate_R_DrawTranslatedTranslucentColumn_8(void);
#endif
