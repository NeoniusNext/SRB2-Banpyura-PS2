/* Stub environment for the software drawers (src/r_draw8.c, src/r_draw8_npo2.c) outside the engine.
 * Used by rdraw_bench.c (EE bench / MSVC equivalence): the drawer files are #included into one translation unit that
 * owns every global they touch. The same stub serves the HEAD (reference) and the working-tree (candidate) drawers.
 */
#ifndef RDRAW_STUB_H
#define RDRAW_STUB_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void I_Error(const char *message, ...);
#include "m_fixed.h"
#include "libdivide.h"

#define MAXVIDWIDTH 320
#define MAXLIGHTSCALE 48
#define BASEVIDWIDTH 320
#define TRANSPARENTPIXEL 255
#define LIGHTSCALESHIFT 12
#define FOF_CUTSOLIDS 0x80
#define BASEDRAWFUNC 0

typedef UINT8 lighttable_t;
typedef int ffloortype_e;
struct r_lightlist_s
{
	fixed_t height, heightstep, botheight, botheightstep, startheight;
	INT16 lightlevel;
	void *extra_colormap;
	lighttable_t *rcolormap;
	ffloortype_e flags;
	INT32 lightnum;
};

static void I_Error(const char *message, ...) { fprintf(stderr, "I_Error: %s\n", message); exit(2); }

#ifdef PS2_OPT_SLOPE
typedef struct { float x, y, z; } fvector3_t;
#else
typedef struct { double x, y, z; } dvector3_t;
#endif

/* engine globals the drawers read */
static struct { INT32 width, height, rowbytes, bpp; } vid = {320, 200, 320, 1};
static INT32 centerx = 160, centery = 100;
static fixed_t centeryfrac = 100 << FRACBITS, fovtan = FRACUNIT;
static UINT8 *topleft, *screens[5], *colormaps;
static UINT8 *planezlight[MAXLIGHTSCALE];

static lighttable_t *dc_colormap;
static INT32 dc_x, dc_yl, dc_yh, dc_texheight, dc_postlength, dc_numlights;
static fixed_t dc_iscale, dc_texturemid;
static UINT8 *dc_source, *dc_transmap, *dc_translation;
static struct r_lightlist_s *dc_lightlist;
static void (*colfuncs[1])(void);

static INT32 ds_y, ds_x1, ds_x2, ds_waterofs, ds_bgofs;
static lighttable_t *ds_colormap, *ds_translation;
static fixed_t ds_xfrac, ds_yfrac, ds_xstep, ds_ystep;
static UINT16 ds_flatwidth, ds_flatheight;
static UINT8 *ds_source, *ds_transmap;
static UINT32 nflatxshift, nflatyshift, nflatshiftup, nflatmask;
#ifdef PS2_OPT_SLOPE
static fvector3_t ds_su, ds_sv, ds_sz, ds_slopelight;
static float ds_lightscale;
#else
static dvector3_t ds_su, ds_sv, ds_sz, ds_slopelight;
static double zeroheight = 16.0;
#endif
#ifdef __GNUC__
static float focallengthf __attribute__((unused));
#else
static float focallengthf;
#endif

#endif
