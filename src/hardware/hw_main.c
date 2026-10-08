// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2024 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file hw_main.c
/// \brief hardware renderer, using the standard HardWareRender driver DLL for SRB2

#include <math.h>

#include "../doomstat.h"

#ifdef HWRENDER
#include "hw_glob.h"
#include "hw_light.h"
#include "hw_drv.h"
#include "hw_batching.h"
#ifdef PS2_PROFILE
#include "hw_sort.h" // HWR_RadixSort32 (the sprite sort)
#include "../m_argv.h" // -hwnocull
#endif
#include "hw_md2.h"
#include "hw_clip.h"

#include "../i_video.h"
#include "../v_video.h"
#include "../p_local.h"
#include "../p_setup.h"
#include "../r_fps.h"
#include "../r_local.h"
#include "../r_patch.h"
#include "../r_picformats.h"
#include "../r_bsp.h"
#include "../netcode/d_clisrv.h"
#include "../w_wad.h"
#include "../z_zone.h"
#include "../r_splats.h"
#include "../g_game.h"
#include "../st_stuff.h"
#include "../i_system.h"
#include "../m_cheat.h"
#include "../f_finale.h"
#include "../r_things.h" // R_GetShadowZ
#include "../r_translation.h"
#include "../d_main.h"
#include "../p_slopes.h"
#include "../lua_banpyura.h"
#ifdef PS2_PROFILE
#include "../ps2/hw/ps2_hw_prof.h" // PS2-HW-15: COP0 phase accumulators of the hardware renderer (HWPROF lines)
#include "../ps2/hw/ps2_hw_fx2.h" // OPT11 round 2 (FX2): -hwfx bits, the sphere test data of the driver
#include "../ps2/hw/ps2_hwd.h" // PS2HWD_TouchTexture (the sprite stream, PS2-HW-256)
unsigned long long ps2hwp_cyc[HWP_NUM];
unsigned int ps2hwp_cnt[HWC_NUM];
int ps2hwp_skyview;
extern int ps2hwd_dbg_flags; // the driver's -hwdbg bits (ps2/hw/ps2_hwd.c)
extern boolean Cubeapply; // v_video.c (the colour cube): HWR_Lighting
extern float Cubepal[2][2][2][3];
static boolean HWR_PS2_NoCull(void);
extern int PS2HWD_QuadHidden(const void *quad); // PS2-HW-72: can this quad (4 FOutVector) put a pixel on the screen? (ps2_hw_plan.inc)
#else
#define HWP_LOCAL ((void)0)
#define HWP_LAP(idx) ((void)0)
#define HWP_SPAN_BEGIN(name) ((void)0)
#define HWP_SPAN_END(name, idx) ((void)0)
#define HWC_ADD(i) ((void)0)
#define HWD_LOCAL ((void)0)
#define HWD_LAP(idx) ((void)0)
#define HWD_ADD(i) ((void)0)
#endif

// ==========================================================================
// the hardware driver object
// ==========================================================================
struct hwdriver_s hwdriver;

// ==========================================================================
//                                                                     PROTOS
// ==========================================================================

static void HWR_AddSprites(sector_t *sec);
static void HWR_ProjectSprite(mobj_t *thing);
static void HWR_ProjectPrecipitationSprite(precipmobj_t *thing);
static void HWR_ProjectBoundingBox(mobj_t *thing);

void HWR_AddTransparentFloor(levelflat_t *levelflat, extrasubsector_t *xsub, boolean isceiling, fixed_t fixedheight, INT32 lightlevel, INT32 alpha, sector_t *FOFSector, FBITFIELD blend, boolean fogplane, boolean chromakeyed, extracolormap_t *planecolormap);
void HWR_AddTransparentPolyobjectFloor(levelflat_t *levelflat, polyobj_t *polysector, boolean isceiling, fixed_t fixedheight,
                             INT32 lightlevel, INT32 alpha, sector_t *FOFSector, FBITFIELD blend, extracolormap_t *planecolormap);

static boolean drawsky = true;

// ==========================================================================
//                                                                    GLOBALS
// ==========================================================================

static seg_t *gl_curline;
static side_t *gl_sidedef;
static line_t *gl_linedef;
static sector_t *gl_frontsector;
static sector_t *gl_backsector;

// Render stats
ps_metric_t ps_hw_skyboxtime = {0};
ps_metric_t ps_hw_nodesorttime = {0};
ps_metric_t ps_hw_nodedrawtime = {0};
ps_metric_t ps_hw_spritesorttime = {0};
ps_metric_t ps_hw_spritedrawtime = {0};

// Render stats for batching
ps_metric_t ps_hw_numpolys = {0};
ps_metric_t ps_hw_numverts = {0};
ps_metric_t ps_hw_numcalls = {0};
ps_metric_t ps_hw_numshaders = {0};
ps_metric_t ps_hw_numtextures = {0};
ps_metric_t ps_hw_numpolyflags = {0};
ps_metric_t ps_hw_numcolors = {0};
ps_metric_t ps_hw_batchsorttime = {0};
ps_metric_t ps_hw_batchdrawtime = {0};

boolean gl_init = false;
boolean gl_maploaded = false;
boolean gl_sessioncommandsadded = false;
// false if shaders have not been initialized yet, or if shaders are not available
boolean gl_shadersavailable = false;

// Whether the internal state is set to palette rendering or not.
static boolean gl_palette_rendering_state = false;

// --------------------------------------------------------------------------
//                                              STUFF FOR THE PROJECTION CODE
// --------------------------------------------------------------------------

FTransform atransform;

static float gl_viewx, gl_viewy, gl_viewz;
float gl_viewsin, gl_viewcos;

// For HWR_RotateSpritePolyToAim
static float gl_viewludsin, gl_viewludcos;
static float gl_fovlud;

static angle_t gl_aimingangle;
static void HWR_SetTransformAiming(FTransform *trans, player_t *player, boolean skybox);

// ==========================================================================
// Lighting
// ==========================================================================

// Returns true if shaders can be used.
boolean HWR_UseShader(void)
{
	return (cv_glshaders.value && gl_shadersavailable);
}

static boolean HWR_IsWireframeMode(void)
{
	return (cv_glwireframe.value && cv_debug);
}

void HWR_Lighting(FSurfaceInfo *Surface, INT32 light_level, extracolormap_t *colormap)
{
	RGBA_t poly_color, tint_color, fade_color;
	HWP_SPAN_BEGIN(hwp_tlight);

#ifdef PS2_PROFILE
	// OPT11 PS2-HW-83: the common surface (no colormap of the sector, shaders on, no colour cube): what the code below makes of it without the two V_CubeApply calls, the
	// colour arithmetic of the other branch and the generic look-ups (-hwgo 8: the long way). The light table of the default colormap is asked as before.
	if (!colormap && !Cubeapply && cv_glshaders.value && gl_shadersavailable && !(hwr_geo_off & 8))
	{
		light_level = min(max(light_level, cv_secbright.value), 255);
		Surface->PolyColor.rgba = 0xFFFFFFFF;
		Surface->TintColor.rgba = 0x00000000;
		Surface->FadeColor.rgba = 0xFF000000;
		Surface->LightInfo.light_level = light_level;
		Surface->LightInfo.fade_start = 0;
		Surface->LightInfo.fade_end = 31;
		Surface->LightTableId = HWR_ShouldUsePaletteRendering() ? HWR_GetLightTableID(NULL) : 0;
		HWP_SPAN_END(hwp_tlight, HWP_LIGHT);
		return;
	}
#endif

	poly_color.rgba = 0xFFFFFFFF;
	tint_color.rgba = (colormap != NULL) ? (UINT32)colormap->rgba : 0x00000000;
	fade_color.rgba = (colormap != NULL) ? (UINT32)colormap->fadergba : 0xFF000000;

	// Clamp the light level, since it can sometimes go out of the 0-255 range from animations
	light_level = min(max(light_level, cv_secbright.value), 255);

	// Crappy backup coloring if you can't do shaders
	if (!HWR_UseShader())
	{
		// be careful, this may get negative for high lightlevel values.
		float tint_alpha, fade_alpha;
		float red, green, blue;

		red = (float)poly_color.s.red;
		green = (float)poly_color.s.green;
		blue = (float)poly_color.s.blue;

#ifdef PS2_PROFILE // PS2-HW-18: sqrt() of a double is a library call of hundreds of cycles per polygon; both arguments take 256 values: the original expressions, tabulated
		static float tint_alpha_tab[256], fade_alpha_tab[256];
		static boolean alpha_tabs;

		if (!alpha_tabs)
		{
			INT32 i;

			for (i = 0; i < 256; i++)
			{
				tint_alpha_tab[i] = (float)(sqrt((float)i / 10.2) * 48) / 255.0f;
				fade_alpha_tab[i] = (float)(sqrt(255-i) * 12) / 255.0f;
			}
			alpha_tabs = true;
		}
#endif
		// 48 is just an arbritrary value that looked relatively okay.
#ifdef PS2_PROFILE
		tint_alpha = tint_alpha_tab[tint_color.s.alpha];
#else
		tint_alpha = (float)(sqrt((float)tint_color.s.alpha / 10.2) * 48) / 255.0f;
#endif

		// 8 is roughly the brightness of the "close" color in Software, and 16 the brightness of the "far" color.
		// 8 is too bright for dark levels, and 16 is too dark for bright levels.
		// 12 is the compromise value. It doesn't look especially good anywhere, but it's the most balanced.
		// (Also, as far as I can tell, fade_color's alpha is actually not used in Software, so we only use light level.)
#ifdef PS2_PROFILE
		fade_alpha = fade_alpha_tab[light_level];
#else
		fade_alpha = (float)(sqrt(255-light_level) * 12) / 255.0f;
#endif

		// Clamp the alpha values
		tint_alpha = min(max(tint_alpha, 0.0f), 1.0f);
		fade_alpha = min(max(fade_alpha, 0.0f), 1.0f);

		red = (tint_color.s.red * tint_alpha) + (red * (1.0f - tint_alpha));
		green = (tint_color.s.green * tint_alpha) + (green * (1.0f - tint_alpha));
		blue = (tint_color.s.blue * tint_alpha) + (blue * (1.0f - tint_alpha));

		red = (fade_color.s.red * fade_alpha) + (red * (1.0f - fade_alpha));
		green = (fade_color.s.green * fade_alpha) + (green * (1.0f - fade_alpha));
		blue = (fade_color.s.blue * fade_alpha) + (blue * (1.0f - fade_alpha));

		poly_color.s.red = (UINT8)red;
		poly_color.s.green = (UINT8)green;
		poly_color.s.blue = (UINT8)blue;
	}

	V_CubeApply(&tint_color.s.red, &tint_color.s.green, &tint_color.s.blue);
	V_CubeApply(&fade_color.s.red, &fade_color.s.green, &fade_color.s.blue);
	Surface->PolyColor.rgba = poly_color.rgba;
	Surface->TintColor.rgba = tint_color.rgba;
	Surface->FadeColor.rgba = fade_color.rgba;
	Surface->LightInfo.light_level = light_level;
	Surface->LightInfo.fade_start = (colormap != NULL) ? colormap->fadestart : 0;
	Surface->LightInfo.fade_end = (colormap != NULL) ? colormap->fadeend : 31;

	if (HWR_ShouldUsePaletteRendering())
		Surface->LightTableId = HWR_GetLightTableID(colormap);
	else
		Surface->LightTableId = 0;
	HWP_SPAN_END(hwp_tlight, HWP_LIGHT);
}

UINT8 HWR_FogBlockAlpha(INT32 light, extracolormap_t *colormap) // Let's see if this can work
{
	RGBA_t realcolor, surfcolor;
	INT32 alpha;

	realcolor.rgba = (colormap != NULL) ? colormap->rgba : 0x00000000;

	if (cv_glshaders.value && gl_shadersavailable)
	{
		surfcolor.s.alpha = (255 - light);
	}
	else
	{
		light = light - (255 - light);

		// Don't go out of bounds
		if (light < 0)
			light = 0;
		else if (light > 255)
			light = 255;

		alpha = (realcolor.s.alpha*255)/25;

		// at 255 brightness, alpha is between 0 and 127, at 0 brightness alpha will always be 255
		surfcolor.s.alpha = (alpha*light) / (2*256) + 255-light;
	}

	return surfcolor.s.alpha;
}

static FUINT HWR_CalcWallLight(FUINT lightnum, fixed_t v1x, fixed_t v1y, fixed_t v2x, fixed_t v2y)
{
	INT16 finallight = lightnum;

	if (cv_glfakecontrast.value != 0)
	{
		const UINT8 contrast = 8;
		fixed_t extralight = 0;

		if (cv_glfakecontrast.value == 2) // Smooth setting
		{
#ifdef PS2_PROFILE
			// OPT11 PS2-HW-84: the extra light is a function of the two differences only (an arc tangent table look-up, a division and a multiplication): a wall that is split
			// by 3D floors or lights asks for the same one again and again; the last answer is kept (-hwgo 16: always calculated)
			static fixed_t cw_dx = -1, cw_dy = -1, cw_extra;
			const fixed_t dx = abs(v1x - v2x), dy = abs(v1y - v2y);

			if (dx == cw_dx && dy == cw_dy && !(hwr_geo_off & 16))
				extralight = cw_extra;
			else
			{
				extralight = (-(contrast<<FRACBITS) +
				FixedDiv(AngleFixed(R_PointToAngle2(0, 0, dx, dy)), 90<<FRACBITS)
				* (contrast * 2)) >> FRACBITS;
				cw_dx = dx;
				cw_dy = dy;
				cw_extra = extralight;
			}
#else
			extralight = (-(contrast<<FRACBITS) +
			FixedDiv(AngleFixed(R_PointToAngle2(0, 0,
				abs(v1x - v2x),
				abs(v1y - v2y))), 90<<FRACBITS)
			* (contrast * 2)) >> FRACBITS;
#endif
		}
		else
		{
			if (v1y == v2y)
				extralight = -contrast;
			else if (v1x == v2x)
				extralight = contrast;
		}

		if (extralight != 0)
		{
			finallight += extralight;

			if (finallight < 0)
				finallight = 0;
			if (finallight > 255)
				finallight = 255;
		}
	}

	return (FUINT)finallight;
}

#ifdef PS2_PROFILE
// PS2-HW-51: the "fake contrast" of a wall depends on the direction of its seg only (a division, an angle and a fixed point divide: 210 cycles
// for each of the 1000 segs of a frame). The extra light of a seg is kept (one INT16 per seg, 0x7F7F = not made yet) and the cache is dropped
// when the console variable changes. Polyobject segs move: they are always calculated.
static INT16 *wallxl_cache;
static INT32 wallxl_mode = -1;

static FUINT HWR_CalcSegLight(FUINT lightnum, const seg_t *seg, fixed_t v1x, fixed_t v1y, fixed_t v2x, fixed_t v2y)
{
	INT16 *e;
	INT32 xl;
	INT16 finallight = lightnum;

	if (cv_glfakecontrast.value == 0)
		return lightnum;
	if (seg->polyseg || (ps2hwd_dbg_flags & 0x40000))
		return HWR_CalcWallLight(lightnum, v1x, v1y, v2x, v2y);
	if (!wallxl_cache || wallxl_mode != cv_glfakecontrast.value)
	{
		if (!wallxl_cache)
			Z_Malloc(numsegs * sizeof(INT16), PU_LEVEL, &wallxl_cache);
		memset(wallxl_cache, 0x7F, numsegs * sizeof(INT16));
		wallxl_mode = cv_glfakecontrast.value;
	}
	e = &wallxl_cache[seg - segs];
	if (*e == 0x7F7F)
	{
		// the extra light is what HWR_CalcWallLight adds to a light level that cannot clamp: 128 (clamp only after the addition)
		*e = (INT16)((INT32)HWR_CalcWallLight(128, v1x, v1y, v2x, v2y) - 128);
	}
	xl = *e;
	if (xl != 0)
	{
		finallight += xl;
		if (finallight < 0)
			finallight = 0;
		if (finallight > 255)
			finallight = 255;
	}
	return (FUINT)finallight;
}
#endif

static FUINT HWR_CalcSlopeLight(FUINT lightnum, angle_t dir, fixed_t delta)
{
	INT16 finallight = lightnum;

	if (cv_glfakecontrast.value != 0 && cv_glslopecontrast.value != 0)
	{
		const UINT8 contrast = 8;
		fixed_t extralight = 0;

		if (cv_glfakecontrast.value == 2) // Smooth setting
		{
			fixed_t dirmul = abs(FixedDiv(AngleFixed(dir) - (180<<FRACBITS), 180<<FRACBITS));

			extralight = -(contrast<<FRACBITS) + (dirmul * (contrast * 2));

			extralight = FixedMul(extralight, delta*4) >> FRACBITS;
		}
		else
		{
			dir = ((dir + ANGLE_45) / ANGLE_90) * ANGLE_90;

			if (dir == ANGLE_180)
				extralight = -contrast;
			else if (dir == 0)
				extralight = contrast;

			if (delta >= FRACUNIT/2)
				extralight *= 2;
		}

		if (extralight != 0)
		{
			finallight += extralight;

			if (finallight < 0)
				finallight = 0;
			if (finallight > 255)
				finallight = 255;
		}
	}

	return (FUINT)finallight;
}

#ifdef PS2_PROFILE
// PS2-HW-58: the texture scale of a side is almost always FRACUNIT; FixedDiv(a, FRACUNIT) is a (the overflow test passes for every texture height),
// and a texture offset inside [0, height) needs no modulo: both cost a long division of 40..70 cycles on the EE
static inline fixed_t HWR_DivScale(fixed_t a, fixed_t scale)
{
	return scale == FRACUNIT ? a : FixedDiv(a, scale);
}

static inline fixed_t HWR_ModHeight(fixed_t x, fixed_t m)
{
	return (UINT32)x < (UINT32)m ? x : x % m;
}
#else
#define HWR_DivScale(a, scale) FixedDiv((a), (scale))
#define HWR_ModHeight(x, m) ((x) % (m))
#endif

static UINT8 HWR_SideLightLevel(side_t *side, INT16 base_lightlevel)
{
	return (max(max(0, cv_secbright.value), min(255, SIDE_LIGHT(side) +
		((SIDE_LIGHTABSOLUTE(side)) ? 0 : base_lightlevel))));
}

/* TODO: implement per-texture lighting
static UINT8 HWR_TopLightLevel(side_t *side, INT16 base_lightlevel)
{
	return max(0, min(255, SIDE_LIGHT_TOP(side) +
		((SIDE_LIGHTABSOLUTE_TOP(side)) ? 0 : HWR_SideLightLevel(side, base_lightlevel))));
}

static UINT8 HWR_MidLightLevel(side_t *side, INT16 base_lightlevel)
{
	return max(0, min(255, SIDE_LIGHT_MID(side) +
		((SIDE_LIGHTABSOLUTE_MID(side)) ? 0 : HWR_SideLightLevel(side, base_lightlevel))));
}

static UINT8 HWR_BottomLightLevel(side_t *side, INT16 base_lightlevel)
{
	return max(0, min(255, SIDE_LIGHT_BOTTOM(side) +
		((SIDE_LIGHTABSOLUTE_BOTTOM(side)) ? 0 : HWR_SideLightLevel(side, base_lightlevel))));
}
*/

static UINT8 HWR_FloorLightLevel(sector_t *sector, INT16 base_lightlevel)
{
	return max(0, min(255, sector->floorlightlevel +
		((sector->floorlightabsolute) ? 0 : base_lightlevel)));
}

static UINT8 HWR_CeilingLightLevel(sector_t *sector, INT16 base_lightlevel)
{
	return max(0, min(255, sector->ceilinglightlevel +
		((sector->ceilinglightabsolute) ? 0 : base_lightlevel)));
}
// ==========================================================================
//                                   FLOOR/CEILING GENERATION FROM SUBSECTORS
// ==========================================================================

#ifdef PS2_PROFILE
// OPT11 (GEOM, PS2-HW-80): the geometry cache of the BSP walk replaces the plane cache of PS2-HW-55 (hw_gcache.inc)
#include "hw_gcache.inc"

static UINT8 *subhoriz; // per subsector: 0 = not looked at, 1 = no horizon line, 2 = a horizon line (camera dependent geometry: never cached)
static size_t subhoriz_n;

// a new level: nothing of the cache and of the horizon flags is valid
static void HWR_PlaneCacheReset(void)
{
	free(subhoriz);
	subhoriz = NULL;
	subhoriz_n = 0;
	HWR_GCacheLevel();
}

static boolean HWR_PlaneHasHorizon(const subsector_t *sub)
{
	const size_t n = (size_t)(sub - subsectors);
	const seg_t *line;
	INT32 i;

	if (n >= numsubsectors)
		return true;
	if (!subhoriz)
	{
		subhoriz = calloc(numsubsectors, 1);
		if (!subhoriz)
			return true; // (no room: not cached)
		subhoriz_n = numsubsectors;
	}
	if (!subhoriz[n])
	{
		subhoriz[n] = 1;
		line = &segs[sub->firstline];
		for (i = 0; i < sub->numlines; i++, line++)
			if (!line->glseg && line->linedef->special == SPECIAL_HORIZON_LINE)
				subhoriz[n] = 2;
	}
	return subhoriz[n] == 2;
}
#endif

// -----------------+
// HWR_RenderPlane  : Render a floor or ceiling convex polygon
// -----------------+
static void HWR_RenderPlane(subsector_t *subsector, extrasubsector_t *xsub, boolean isceiling, fixed_t fixedheight, FBITFIELD PolyFlags, INT32 lightlevel, levelflat_t *levelflat, sector_t *FOFsector, UINT8 alpha, extracolormap_t *planecolormap)
{
	FSurfaceInfo Surf;
	FOutVector *v3d;
	polyvertex_t *pv;
	pslope_t *slope = NULL;
	INT32 shader = SHADER_NONE;

	size_t nrPlaneVerts;
	INT32 i;
#ifdef PS2_PROFILE
	gcent_t *gce = NULL; // OPT11 PS2-HW-80: the geometry cache entry this plane is recorded into
	gcfp_t gck = {0, 0};
	boolean gccheck = false;
	UINT32 gcid = 0, gcid2 = 0;
#endif

	float height; // constant y for all points on the convex flat polygon
	float anglef = 0.0f;
	float fflatwidth = 64.0f, fflatheight = 64.0f;
	float xscale = 1.0f, yscale = 1.0f;

	float tempxsow, tempytow;
	float scrollx = 0.0f, scrolly = 0.0f;
	angle_t angle = 0;

	static FOutVector *planeVerts = NULL;
	static UINT16 numAllocedPlaneVerts = 0;

	HWD_LOCAL; // OPT11: the parts of this function (HWPROF37, --hwdetail)

	if (!r_renderfloors)
		return;

	// no convex poly were generated for this subsector
	if (!xsub->planepoly)
		return;

	pv = xsub->planepoly->pts;
	nrPlaneVerts = xsub->planepoly->numpts;

	if (nrPlaneVerts < 3) // not even a triangle?
		return;

	// Get the slope pointer to simplify future code
	if (FOFsector)
	{
		if (FOFsector->f_slope && !isceiling)
			slope = FOFsector->f_slope;
		else if (FOFsector->c_slope && isceiling)
			slope = FOFsector->c_slope;
	}
	else
	{
		if (gl_frontsector->f_slope && !isceiling)
			slope = gl_frontsector->f_slope;
		else if (gl_frontsector->c_slope && isceiling)
			slope = gl_frontsector->c_slope;
	}

#ifdef PS2_PROFILE
	// OPT11 PS2-HW-80: the polygon of this plane is served from the geometry cache when everything it is made of is as it was (hw_gcache.inc)
	if (gc.on && currently_batching && nrPlaneVerts < 250 && !(gc.mode & 4) && (!subsector || !HWR_PlaneHasHorizon(subsector)))
	{
		const UINT32 t0 = ps2hwp_now();
		const sector_t *src = FOFsector ? FOFsector : gl_frontsector;
		gcfp_t t;
		boolean hit;

		gcid = ((UINT32)(xsub - extrasubsectors) * 2u + (isceiling ? 1u : 0u)) * 2u + 1u;
		gcid2 = (UINT32)(uintptr_t)FOFsector;
		gc_init_fp(&gck);
		gc_get_fp(src, false, &t);
		gcw_fp(&gck, &t);
		gcw(&gck, (UINT32)fixedheight);
		gcw(&gck, (UINT32)PolyFlags);
		gcw(&gck, (UINT32)lightlevel);
		gcw(&gck, (UINT32)alpha);
		gcw(&gck, levelflat ? (UINT32)R_GetTextureNumForFlat(levelflat) : 0xFFFFFFFFu);
		gcw(&gck, (UINT32)(uintptr_t)current_texture); // (the texture record the caller chose: the sort key of the polygon is made with it)
		gc_cmap(&gck, planecolormap);
		gce = gc_find(gcid, gcid2, &hit);
		gc.c_key += ps2hwp_now() - t0;
		if (hit && gce->k0 == gck.a && gce->k1 == gck.b)
		{
			if (!(gccheck = gc_check_this()))
			{
				const UINT32 t1 = ps2hwp_now();

				gc_replay(gc.ar + gce->off, gce->len);
				gc.s_pl_hit++;
				gc.c_hit += ps2hwp_now() - t1;
				return;
			}
		}
		gc_rec_begin(gccheck);
	}
	else
	{
		gce = NULL;
		gc.s_pl_skip++;
	}
#endif

	HWD_LAP(HWP_PL_A);
	height = FixedToFloat(fixedheight);

	// Allocate plane-vertex buffer if we need to
	if (!planeVerts || nrPlaneVerts > numAllocedPlaneVerts)
	{
		numAllocedPlaneVerts = (UINT16)nrPlaneVerts;
		Z_Free(planeVerts);
		Z_Malloc(numAllocedPlaneVerts * sizeof (FOutVector), PU_LEVEL, &planeVerts);
	}

	// set texture for polygon
	if (levelflat != NULL)
	{
		texture_t *texture = textures[R_GetTextureNumForFlat(levelflat)];
		fflatwidth = texture->width;
		fflatheight = texture->height;
	}
	else // set no texture
		HWR_SetCurrentTexture(NULL);

	// transform
	if (FOFsector != NULL)
	{
		if (!isceiling) // it's a floor
		{
			xscale = FixedToFloat(FOFsector->floorxscale);
			yscale = FixedToFloat(FOFsector->flooryscale);
			scrollx = FixedToFloat(FOFsector->floorxoffset) / fflatwidth;
			scrolly = FixedToFloat(FOFsector->flooryoffset) / fflatheight;
			angle = FOFsector->floorangle;
		}
		else // it's a ceiling
		{
			xscale = FixedToFloat(FOFsector->ceilingxscale);
			yscale = FixedToFloat(FOFsector->ceilingyscale);
			scrollx = FixedToFloat(FOFsector->ceilingxoffset) / fflatwidth;
			scrolly = FixedToFloat(FOFsector->ceilingyoffset) / fflatheight;
			angle = FOFsector->ceilingangle;
		}
	}
	else if (gl_frontsector)
	{
		if (!isceiling) // it's a floor
		{
			xscale = FixedToFloat(gl_frontsector->floorxscale);
			yscale = FixedToFloat(gl_frontsector->flooryscale);
			scrollx = FixedToFloat(gl_frontsector->floorxoffset) / fflatwidth;
			scrolly = FixedToFloat(gl_frontsector->flooryoffset) / fflatheight;
			angle = gl_frontsector->floorangle;
		}
		else // it's a ceiling
		{
			xscale = FixedToFloat(gl_frontsector->ceilingxscale);
			yscale = FixedToFloat(gl_frontsector->ceilingyscale);
			scrollx = FixedToFloat(gl_frontsector->ceilingxoffset) / fflatwidth;
			scrolly = FixedToFloat(gl_frontsector->ceilingyoffset) / fflatheight;
			angle = gl_frontsector->ceilingangle;
		}
	}

#ifdef PS2_PROFILE // PS2-HW-49: ANG2RAD is a soft-double multiply, conversion and truncation; angle 0 (almost every flat) gives exactly 0
	anglef = angle ? ANG2RAD(InvAngle(angle)) : 0.0f;
#else
	anglef = ANG2RAD(InvAngle(angle));
#endif

#define SETUP3DVERT(vert, vx, vy) {\
		/* Hurdler: add scrolling texture on floor/ceiling */\
		vert->s = ((vx) / fflatwidth) + (scrollx / xscale);\
		vert->t = -((vy) / fflatheight) + (scrolly / yscale);\
\
		/* Need to rotate before translate */\
		if (angle) /* Only needs to be done if there's an altered angle */\
		{\
			tempxsow = vert->s;\
			tempytow = vert->t;\
			vert->s = (tempxsow * cos(anglef)) - (tempytow * sin(anglef));\
			vert->t = (tempxsow * sin(anglef)) + (tempytow * cos(anglef));\
		}\
\
		vert->s *= xscale;\
		vert->t *= yscale;\
\
		if (slope)\
		{\
			fixedheight = P_GetSlopeZAt(slope, FloatToFixed((vx)), FloatToFixed((vy)));\
			height = FixedToFloat(fixedheight);\
		}\
\
		vert->x = (vx);\
		vert->y = height;\
		vert->z = (vy);\
}

	HWD_LAP(HWP_PL_B);
#ifdef PS2_PROFILE
	if (!slope)
	{
		// PS2-HW-58b: the flat's texture mapping without what does not change per vertex: scrollx / xscale and scrolly / yscale once, a multiplication
		// where the flat size is a power of two (exact), no multiplication by a scale of 1, and for a turned flat sin / cos of the angle ONCE as floats:
		// the macro called the double precision cos() and sin() four times per vertex (soft float: 600..1000 cycles each).
		// Deviation (registered): the turned flat's coordinates are products of floats, not of a float and a double (1 ulp of a float).
		const float sadd = scrollx / xscale, tadd = scrolly / yscale;
		const boolean pw = ((int)fflatwidth & ((int)fflatwidth - 1)) == 0 && (float)(int)fflatwidth == fflatwidth, ph = ((int)fflatheight & ((int)fflatheight - 1)) == 0 && (float)(int)fflatheight == fflatheight;
		const float invw = 1.0f / fflatwidth, invh = 1.0f / fflatheight;
		const boolean unit = xscale == 1.0f && yscale == 1.0f;
		const float ca = angle ? (float)cos(anglef) : 1.0f, sa = angle ? (float)sin(anglef) : 0.0f;

		for (i = 0, v3d = planeVerts; i < (INT32)nrPlaneVerts; i++, v3d++, pv++)
		{
			const float vx = pv->x, vy = pv->y;
			float s = (pw ? vx * invw : vx / fflatwidth) + sadd;
			float t = -(ph ? vy * invh : vy / fflatheight) + tadd;

			if (angle)
			{
				const float s0 = s, t0 = t;

				s = s0 * ca - t0 * sa;
				t = s0 * sa + t0 * ca;
			}
			if (!unit)
			{
				s *= xscale;
				t *= yscale;
			}
			v3d->s = s;
			v3d->t = t;
			v3d->x = vx;
			v3d->y = height;
			v3d->z = vy;
		}
	}
	else
#endif
	for (i = 0, v3d = planeVerts; i < (INT32)nrPlaneVerts; i++,v3d++,pv++)
		SETUP3DVERT(v3d, pv->x, pv->y);

	HWD_LAP(HWP_PL_C);
	if (slope)
		lightlevel = HWR_CalcSlopeLight(lightlevel, R_PointToAngle2(0, 0, slope->normal.x, slope->normal.y), abs(slope->zdelta));

	HWR_Lighting(&Surf, lightlevel, planecolormap);
	HWD_LAP(HWP_PL_D);

	if (PolyFlags & (PF_Translucent|PF_Fog|PF_Additive|PF_Subtractive|PF_ReverseSubtract|PF_Multiplicative|PF_Environment))
	{
		Surf.PolyColor.s.alpha = (UINT8)alpha;
		PolyFlags |= PF_Modulated;
	}
	else
		PolyFlags |= PF_Masked|PF_Modulated;

	if (HWR_UseShader())
	{
		if (PolyFlags & PF_Fog)
			shader = SHADER_FOG;
		else if (PolyFlags & PF_Ripple)
			shader = SHADER_WATER;
		else
			shader = SHADER_FLOOR;

		PolyFlags |= PF_ColorMapped;
	}

	HWD_LAP(HWP_PL_E);
	HWR_ProcessPolygon(&Surf, planeVerts, nrPlaneVerts, PolyFlags, shader, false);
	HWD_LAP(HWP_PL_F);

#ifdef PS2_PROFILE
	if (gce)
	{
		const UINT32 t1 = ps2hwp_now();

		gc_rec_end(gce, gcid, gcid2, &gck, gccheck, "plane", gcid >> 1);
		gce = NULL;
		if (gccheck)
			gc.s_pl_hit++;
		else
			gc.s_pl_miss++;
		gc.c_miss += ps2hwp_now() - t1;
	}
	if (subsector && HWR_PlaneHasHorizon(subsector)) // PS2-HW-58b: the flag is made once per subsector; the loop below looked at every line of the subsector (a seg and its linedef) for every plane
#else
	if (subsector)
#endif
	{
		// Horizon lines
		FOutVector horizonpts[6];
		float dist, vx, vy;
		float x1, y1, xd, yd;
		UINT8 numplanes, j;
		vertex_t v; // For determining the closest distance from the line to the camera, to split render planes for minimum distortion;

		const float renderdist = 27000.0f; // How far out to properly render the plane
		const float farrenderdist = 32768.0f; // From here, raise plane to horizon level to fill in the line with some texture distortion

		seg_t *line = &segs[subsector->firstline];

		for (i = 0; i < subsector->numlines; i++, line++)
		{
			if (!line->glseg && line->linedef->special == SPECIAL_HORIZON_LINE && R_PointOnSegSide(viewx, viewy, line) == 0)
			{
				P_ClosestPointOnLine(viewx, viewy, line->linedef, &v);
				dist = FIXED_TO_FLOAT(R_PointToDist(v.x, v.y));

				if (line->pv1)
				{
					x1 = ((polyvertex_t *)line->pv1)->x;
					y1 = ((polyvertex_t *)line->pv1)->y;
				}
				else
				{
					x1 = FIXED_TO_FLOAT(line->v1->x);
					y1 = FIXED_TO_FLOAT(line->v1->x);
				}
				if (line->pv2)
				{
					xd = ((polyvertex_t *)line->pv2)->x - x1;
					yd = ((polyvertex_t *)line->pv2)->y - y1;
				}
				else
				{
					xd = FIXED_TO_FLOAT(line->v2->x) - x1;
					yd = FIXED_TO_FLOAT(line->v2->y) - y1;
				}

				// Based on the seg length and the distance from the line, split horizon into multiple poly sets to reduce distortion
				dist = sqrtf((xd*xd) + (yd*yd)) / dist / 16.0f;
				if (dist > 100.0f)
					numplanes = 100;
				else
					numplanes = (UINT8)dist + 1;

				for (j = 0; j < numplanes; j++)
				{
					// Left side
					vx = x1 + xd * j / numplanes;
					vy = y1 + yd * j / numplanes;
					SETUP3DVERT((&horizonpts[1]), vx, vy);

					dist = sqrtf(powf(vx - gl_viewx, 2) + powf(vy - gl_viewy, 2));
					vx = (vx - gl_viewx) * renderdist / dist + gl_viewx;
					vy = (vy - gl_viewy) * renderdist / dist + gl_viewy;
					SETUP3DVERT((&horizonpts[0]), vx, vy);

					// Right side
					vx = x1 + xd * (j+1) / numplanes;
					vy = y1 + yd * (j+1) / numplanes;
					SETUP3DVERT((&horizonpts[2]), vx, vy);

					dist = sqrtf(powf(vx - gl_viewx, 2) + powf(vy - gl_viewy, 2));
					vx = (vx - gl_viewx) * renderdist / dist + gl_viewx;
					vy = (vy - gl_viewy) * renderdist / dist + gl_viewy;
					SETUP3DVERT((&horizonpts[3]), vx, vy);

					// Horizon fills
					vx = (horizonpts[0].x - gl_viewx) * farrenderdist / renderdist + gl_viewx;
					vy = (horizonpts[0].z - gl_viewy) * farrenderdist / renderdist + gl_viewy;
					SETUP3DVERT((&horizonpts[5]), vx, vy);
					horizonpts[5].y = gl_viewz;

					vx = (horizonpts[3].x - gl_viewx) * farrenderdist / renderdist + gl_viewx;
					vy = (horizonpts[3].z - gl_viewy) * farrenderdist / renderdist + gl_viewy;
					SETUP3DVERT((&horizonpts[4]), vx, vy);
					horizonpts[4].y = gl_viewz;

					// Draw
					HWR_ProcessPolygon(&Surf, horizonpts, 6, PolyFlags, shader, true);
				}
			}
		}
	}

#ifdef ALAM_LIGHTING
	// add here code for dynamic lighting on planes
	HWR_PlaneLighting(planeVerts, nrPlaneVerts);
#endif
}

#ifdef PS2_PROFILE // PS2-HW-40: inclusive timer of the plane builder (HWPROF2 "plane")
static void HWR_RenderPlaneTimed(subsector_t *subsector, extrasubsector_t *xsub, boolean isceiling, fixed_t fixedheight, FBITFIELD PolyFlags, INT32 lightlevel, levelflat_t *levelflat, sector_t *FOFsector, UINT8 alpha, extracolormap_t *planecolormap)
{
	HWP_SPAN_BEGIN(t);
	HWC_ADD(HWC_PLANES);
	HWR_RenderPlane(subsector, xsub, isceiling, fixedheight, PolyFlags, lightlevel, levelflat, FOFsector, alpha, planecolormap);
	HWP_SPAN_END(t, HWP_PLANE);
}
#define HWR_RenderPlane HWR_RenderPlaneTimed
#endif

FBITFIELD HWR_GetBlendModeFlag(INT32 style)
{
	switch (style)
	{
		case AST_TRANSLUCENT:
			return PF_Translucent;
		case AST_ADD:
			return PF_Additive;
		case AST_SUBTRACT:
			return PF_Subtractive;
		case AST_REVERSESUBTRACT:
			return PF_ReverseSubtract;
		case AST_MODULATE:
			return PF_Multiplicative;
		default:
			return PF_Masked;
	}
}

UINT8 HWR_GetTranstableAlpha(INT32 transtablenum)
{
	transtablenum = max(min(transtablenum, tr_trans90), 0);

	switch (transtablenum)
	{
		case 0          : return 0xff;
		case tr_trans10 : return 0xe6;
		case tr_trans20 : return 0xcc;
		case tr_trans30 : return 0xb3;
		case tr_trans40 : return 0x99;
		case tr_trans50 : return 0x80;
		case tr_trans60 : return 0x66;
		case tr_trans70 : return 0x4c;
		case tr_trans80 : return 0x33;
		case tr_trans90 : return 0x19;
	}

	return 0xff;
}

FBITFIELD HWR_SurfaceBlend(INT32 style, INT32 transtablenum, FSurfaceInfo *pSurf)
{
	if (!transtablenum || style <= AST_COPY || style >= AST_OVERLAY)
	{
		pSurf->PolyColor.s.alpha = 0xff;
		return PF_Masked;
	}

	pSurf->PolyColor.s.alpha = HWR_GetTranstableAlpha(transtablenum);
	return HWR_GetBlendModeFlag(style);
}

FBITFIELD HWR_TranstableToAlpha(INT32 transtablenum, FSurfaceInfo *pSurf)
{
	if (!transtablenum)
	{
		pSurf->PolyColor.s.alpha = 0x00;
		return PF_Masked;
	}

	pSurf->PolyColor.s.alpha = HWR_GetTranstableAlpha(transtablenum);
	return PF_Translucent;
}

static void HWR_AddTransparentWall(FOutVector *wallVerts, FSurfaceInfo *pSurf, INT32 texnum, FBITFIELD blend, boolean fogwall, INT32 lightlevel, extracolormap_t *wallcolormap);

// ==========================================================================
// Wall generation from subsector segs
// ==========================================================================

/*
   wallVerts order is :
		3--2
		| /|
		|/ |
		0--1
*/

//
// HWR_ProjectWall
//
static void HWR_ProjectWall(FOutVector *wallVerts, FSurfaceInfo *pSurf, FBITFIELD blendmode, INT32 lightlevel, extracolormap_t *wallcolormap)
{
	INT32 shader = SHADER_NONE;

	if (!r_renderwalls)
		return;

	HWR_Lighting(pSurf, lightlevel, wallcolormap);

	if (HWR_UseShader())
	{
		shader = SHADER_WALL;
		blendmode |= PF_ColorMapped;
	}

	HWR_ProcessPolygon(pSurf, wallVerts, 4, blendmode|PF_Modulated|PF_Occlude, shader, false);
}

// ==========================================================================
//                                                          BSP, CULL, ETC..
// ==========================================================================

// SoM: split up and light walls according to the lightlist.
// This may also include leaving out parts of the wall that can't be seen
static void HWR_SplitWall(sector_t *sector, FOutVector *wallVerts, INT32 texnum, FSurfaceInfo* Surf, INT32 cutflag, ffloor_t *pfloor, FBITFIELD polyflags)
{
	float realtop, realbot, top, bot;
	float pegt, pegb, pegmul;
	float height = 0.0f, bheight = 0.0f;

	float endrealtop, endrealbot, endtop, endbot;
	float endpegt, endpegb, endpegmul;
	float endheight = 0.0f, endbheight = 0.0f;

	float diff;

	fixed_t v1x = FloatToFixed(wallVerts[0].x);
	fixed_t v1y = FloatToFixed(wallVerts[0].z);
	fixed_t v2x = FloatToFixed(wallVerts[1].x);
	fixed_t v2y = FloatToFixed(wallVerts[1].z);

	FUINT lightnum = HWR_SideLightLevel(gl_sidedef, sector->lightlevel);
	const UINT8 alpha = Surf->PolyColor.s.alpha;
	lightnum = HWR_CalcWallLight(lightnum, v1x, v1y, v2x, v2y);
	extracolormap_t *colormap = NULL;

	if (!r_renderwalls)
		return;

	realtop = top = wallVerts[3].y;
	realbot = bot = wallVerts[0].y;
	diff = top - bot;

	pegt = wallVerts[3].t;
	pegb = wallVerts[0].t;

	// Lactozilla: If both heights of a side lay on the same position, then this wall is a triangle.
	// To avoid division by zero, which would result in a NaN, we check if the vertical difference
	// between the two vertices is not zero.
	if (fpclassify(diff) == FP_ZERO)
		pegmul = 0.0;
	else
		pegmul = (pegb - pegt) / diff;

	endrealtop = endtop = wallVerts[2].y;
	endrealbot = endbot = wallVerts[1].y;
	diff = endtop - endbot;

	endpegt = wallVerts[2].t;
	endpegb = wallVerts[1].t;

	if (fpclassify(diff) == FP_ZERO)
		endpegmul = 0.0;
	else
		endpegmul = (endpegb - endpegt) / diff;

	for (INT32 i = 0; i < sector->numlights; i++)
	{
		if (endtop < endrealbot && top < realbot)
			return;

		lightlist_t *list = sector->lightlist;

		if (!(list[i].flags & FOF_NOSHADE))
		{
			if (pfloor && (pfloor->fofflags & FOF_FOG))
			{
				lightnum = HWR_SideLightLevel(gl_sidedef, pfloor->master->frontsector->lightlevel);
				colormap = pfloor->master->frontsector->extra_colormap;
				lightnum = colormap ? lightnum : HWR_CalcWallLight(lightnum, v1x, v1y, v2x, v2y);
			}
			else
			{
				lightnum = HWR_SideLightLevel(gl_sidedef, *list[i].lightlevel);
				colormap = *list[i].extra_colormap;
				lightnum = colormap ? lightnum : HWR_CalcWallLight(lightnum, v1x, v1y, v2x, v2y);
			}
		}

		boolean solid = false;

		if ((sector->lightlist[i].flags & FOF_CUTSOLIDS) && !(cutflag & FOF_EXTRA))
			solid = true;
		else if ((sector->lightlist[i].flags & FOF_CUTEXTRA) && (cutflag & FOF_EXTRA))
		{
			if (sector->lightlist[i].flags & FOF_EXTRA)
			{
				if ((sector->lightlist[i].flags & (FOF_FOG|FOF_SWIMMABLE)) == (cutflag & (FOF_FOG|FOF_SWIMMABLE))) // Only merge with your own types
					solid = true;
			}
			else
				solid = true;
		}
		else
			solid = false;

		height = FixedToFloat(P_GetLightZAt(&list[i], v1x, v1y));
		endheight = FixedToFloat(P_GetLightZAt(&list[i], v2x, v2y));
		if (solid)
		{
			bheight = FixedToFloat(P_GetFFloorBottomZAt(list[i].caster, v1x, v1y));
			endbheight = FixedToFloat(P_GetFFloorBottomZAt(list[i].caster, v2x, v2y));
		}

		if (endheight >= endtop && height >= top)
		{
			if (solid && top > bheight)
				top = bheight;
			if (solid && endtop > endbheight)
				endtop = endbheight;
		}

		if (i + 1 < sector->numlights)
		{
			bheight = FixedToFloat(P_GetLightZAt(&list[i+1], v1x, v1y));
			endbheight = FixedToFloat(P_GetLightZAt(&list[i+1], v2x, v2y));
		}
		else
		{
			bheight = realbot;
			endbheight = endrealbot;
		}

		// Found a break
		// The heights are clamped to ensure the polygon doesn't cross itself.
		bot = min(max(bheight, realbot), top);
		endbot = min(max(endbheight, endrealbot), endtop);

		Surf->PolyColor.s.alpha = alpha;

		wallVerts[3].t = pegt + ((realtop - top) * pegmul);
		wallVerts[2].t = endpegt + ((endrealtop - endtop) * endpegmul);
		wallVerts[0].t = pegt + ((realtop - bot) * pegmul);
		wallVerts[1].t = endpegt + ((endrealtop - endbot) * endpegmul);

		// set top/bottom coords
		wallVerts[3].y = top;
		wallVerts[2].y = endtop;
		wallVerts[0].y = bot;
		wallVerts[1].y = endbot;

		if (cutflag & FOF_FOG)
			HWR_AddTransparentWall(wallVerts, Surf, texnum, PF_Fog|PF_NoTexture|polyflags, true, lightnum, colormap);
		else if (polyflags & (PF_Translucent|PF_Additive|PF_Subtractive|PF_ReverseSubtract|PF_Multiplicative|PF_Environment))
			HWR_AddTransparentWall(wallVerts, Surf, texnum, polyflags, false, lightnum, colormap);
		else
			HWR_ProjectWall(wallVerts, Surf, PF_Masked|polyflags, lightnum, colormap);

		top = bot;
		endtop = endbot;
	}

	bot = realbot;
	endbot = endrealbot;
	if (endtop <= endrealbot && top <= realbot)
		return;

	Surf->PolyColor.s.alpha = alpha;

	wallVerts[3].t = pegt + ((realtop - top) * pegmul);
	wallVerts[2].t = endpegt + ((endrealtop - endtop) * endpegmul);
	wallVerts[0].t = pegt + ((realtop - bot) * pegmul);
	wallVerts[1].t = endpegt + ((endrealtop - endbot) * endpegmul);

	// set top/bottom coords
	wallVerts[3].y = top;
	wallVerts[2].y = endtop;
	wallVerts[0].y = bot;
	wallVerts[1].y = endbot;

	if (cutflag & FOF_FOG)
		HWR_AddTransparentWall(wallVerts, Surf, texnum, PF_Fog|PF_NoTexture|polyflags, true, lightnum, colormap);
	else if (polyflags & (PF_Translucent|PF_Additive|PF_Subtractive|PF_ReverseSubtract|PF_Multiplicative|PF_Environment))
		HWR_AddTransparentWall(wallVerts, Surf, texnum, polyflags, false, lightnum, colormap);
	else
		HWR_ProjectWall(wallVerts, Surf, PF_Masked|polyflags, lightnum, colormap);
}

// HWR_DrawSkyWall
// Draw walls into the depth buffer so that anything behind is culled properly
static void HWR_DrawSkyWall(FOutVector *wallVerts, FSurfaceInfo *Surf)
{
	HWR_SetCurrentTexture(NULL);
	// no texture
	wallVerts[3].t = wallVerts[2].t = 0;
	wallVerts[0].t = wallVerts[1].t = 0;
	wallVerts[0].s = wallVerts[3].s = 0;
	wallVerts[2].s = wallVerts[1].s = 0;
	// this no longer sets top/bottom coords, this should be done before caling the function
	HWR_ProjectWall(wallVerts, Surf, PF_Invisible|PF_NoTexture, 255, NULL);
	// PF_Invisible so it's not drawn into the colour buffer
	// PF_NoTexture for no texture
	// PF_Occlude is set in HWR_ProjectWall to draw into the depth buffer
}

// Returns true if the midtexture is visible, false if not
static boolean HWR_BlendMidtextureSurface(FSurfaceInfo *pSurf)
{
	FUINT blendmode = PF_Masked;

	pSurf->PolyColor.s.alpha = 0xFF;

	if (!gl_curline->polyseg)
	{
		if (gl_linedef->blendmode && gl_linedef->blendmode != AST_FOG)
		{
			if (gl_linedef->alpha >= 0 && gl_linedef->alpha < FRACUNIT)
				blendmode = HWR_SurfaceBlend(gl_linedef->blendmode, R_GetLinedefTransTable(gl_linedef->alpha), pSurf);
			else
				blendmode = HWR_GetBlendModeFlag(gl_linedef->blendmode);
		}
		else if (gl_linedef->alpha >= 0 && gl_linedef->alpha < FRACUNIT)
			blendmode = HWR_TranstableToAlpha(R_GetLinedefTransTable(gl_linedef->alpha), pSurf);
	}
	else if (gl_curline->polyseg->translucency > 0)
	{
		// Polyobject translucency is done differently
		if (gl_curline->polyseg->translucency >= NUMTRANSMAPS) // wall not drawn
			return false;
		else
			blendmode = HWR_TranstableToAlpha(gl_curline->polyseg->translucency, pSurf);
	}

	if (blendmode != PF_Masked && pSurf->PolyColor.s.alpha == 0x00)
		return false;

	pSurf->PolyFlags = blendmode;

	return true;
}

static void HWR_RenderMidtexture(INT32 gl_midtexture, float cliplow, float cliphigh, fixed_t worldtop, fixed_t worldbottom, fixed_t worldhigh, fixed_t worldlow, fixed_t worldtopslope, fixed_t worldbottomslope, fixed_t worldhighslope, fixed_t worldlowslope, UINT32 lightnum, FOutVector *inWallVerts)
{
	sector_t *front, *back;
	FOutVector wallVerts[4];

	FSurfaceInfo Surf;
	Surf.PolyColor.s.alpha = 255;

	// Determine if it's visible
	if (!HWR_BlendMidtextureSurface(&Surf))
		return;

	if (gl_linedef->frontsector->heightsec != -1)
		front = &sectors[gl_linedef->frontsector->heightsec];
	else
		front = gl_linedef->frontsector;

	if (gl_linedef->backsector->heightsec != -1)
		back = &sectors[gl_linedef->backsector->heightsec];
	else
		back = gl_linedef->backsector;

	fixed_t texheight = HWR_DivScale(textureheight[gl_midtexture], abs(SIDE_SCALEY_MID(gl_sidedef)));
	INT32 repeats;

	if (gl_sidedef->repeatcnt)
		repeats = 1 + gl_sidedef->repeatcnt;
	else if (gl_linedef->flags & ML_WRAPMIDTEX)
	{
		fixed_t high, low;

		if (front->ceilingheight > back->ceilingheight)
			high = back->ceilingheight;
		else
			high = front->ceilingheight;

		if (front->floorheight > back->floorheight)
			low = front->floorheight;
		else
			low = back->floorheight;

		repeats = (high - low) / texheight;
		if ((high - low) % texheight)
			repeats++; // tile an extra time to fill the gap -- Monster Iestyn
	}
	else
		repeats = 1;

	GLMapTexture_t *grTex = HWR_GetTexture(gl_midtexture, true);
	float xscale = FixedToFloat(SIDE_SCALEX_MID(gl_sidedef));
	float yscale = FixedToFloat(SIDE_SCALEY_MID(gl_sidedef));

	// SoM: a little note: popentop and popenbottom
	// record the limits the texture can be displayed in.
	// polytop and polybottom, are the ideal (i.e. unclipped)
	// heights of the polygon, and h & l, are the final (clipped)
	// poly coords.
	fixed_t popentop, popenbottom, polytop, polybottom, lowcut, highcut;
	fixed_t popentopslope, popenbottomslope, polytopslope, polybottomslope, lowcutslope, highcutslope;

	// NOTE: With polyobjects, whenever you need to check the properties of the polyobject sector it belongs to,
	// you must use the linedef's backsector to be correct
	// From CB
	if (gl_curline->polyseg)
	{
		// Change this when polyobjects support slopes
		popentop = popentopslope = back->ceilingheight;
		popenbottom = popenbottomslope = back->floorheight;
	}
	else
	{
		popentop = min(worldtop, worldhigh);
		popenbottom = max(worldbottom, worldlow);
		popentopslope = min(worldtopslope, worldhighslope);
		popenbottomslope = max(worldbottomslope, worldlowslope);
	}

	// Find the wall's coordinates
	fixed_t midtexheight = texheight * repeats;

	fixed_t rowoffset = HWR_DivScale(gl_sidedef->rowoffset + SIDE_OFFSETY_MID(gl_sidedef), abs(SIDE_SCALEY_MID(gl_sidedef)));

	// Texture is not skewed
	if (gl_linedef->flags & ML_NOSKEW)
	{
		// Peg it to the floor
		if (gl_linedef->flags & ML_MIDPEG)
		{
			polybottom = max(gl_frontsector->floorheight, gl_backsector->floorheight) + rowoffset;
			polytop = polybottom + midtexheight;
		}
		// Peg it to the ceiling
		else
		{
			polytop = min(gl_frontsector->ceilingheight, gl_backsector->ceilingheight) + rowoffset;
			polybottom = polytop - midtexheight;
		}

		// The right side's coordinates are the the same as the left side
		polytopslope = polytop;
		polybottomslope = polybottom;
	}
	// Skew the texture, but peg it to the floor
	else if (gl_linedef->flags & ML_MIDPEG)
	{
		polybottom = popenbottom + rowoffset;
		polytop = polybottom + midtexheight;
		polybottomslope = popenbottomslope + rowoffset;
		polytopslope = polybottomslope + midtexheight;
	}
	// Skew it according to the ceiling's slope
	else
	{
		polytop = popentop + rowoffset;
		polybottom = polytop - midtexheight;
		polytopslope = popentopslope + rowoffset;
		polybottomslope = polytopslope - midtexheight;
	}

	// The cut-off values of a linedef can always be constant, since every line has an absoulute front and or back sector
	if (gl_curline->polyseg)
	{
		lowcut = polybottom;
		highcut = polytop;
		lowcutslope = polybottomslope;
		highcutslope = polytopslope;
	}
	else
	{
		lowcut = popenbottom;
		highcut = popentop;
		lowcutslope = popenbottomslope;
		highcutslope = popentopslope;
	}

	fixed_t h = min(highcut, polytop);
	fixed_t l = max(polybottom, lowcut);
	fixed_t hS = min(highcutslope, polytopslope);
	fixed_t lS = max(polybottomslope, lowcutslope);

	// PEGGING
	fixed_t texturevpeg, texturevpegslope;

	if (gl_linedef->flags & ML_MIDPEG)
	{
		texturevpeg = midtexheight - h + polybottom;
		texturevpegslope = midtexheight - hS + polybottomslope;
	}
	else
	{
		texturevpeg = polytop - h;
		texturevpegslope = polytopslope - hS;
	}

	memcpy(wallVerts, inWallVerts, sizeof(wallVerts));

	// Left side
	wallVerts[3].t = texturevpeg * yscale * grTex->scaleY;
	wallVerts[0].t = (h - l + texturevpeg) * yscale * grTex->scaleY;
	wallVerts[0].s = wallVerts[3].s = ((cliplow * xscale) + gl_sidedef->textureoffset + SIDE_OFFSETX_MID(gl_sidedef)) * grTex->scaleX;

	// Right side
	wallVerts[2].t = texturevpegslope * yscale * grTex->scaleY;
	wallVerts[1].t = (hS - lS + texturevpegslope) * yscale * grTex->scaleY;
	wallVerts[2].s = wallVerts[1].s = ((cliphigh * xscale) + gl_sidedef->textureoffset + SIDE_OFFSETX_MID(gl_sidedef)) * grTex->scaleX;

	// set top/bottom coords
	// Take the texture peg into account, rather than changing the offsets past
	// where the polygon might not be.
	wallVerts[3].y = FIXED_TO_FLOAT(h);
	wallVerts[0].y = FIXED_TO_FLOAT(l);
	wallVerts[2].y = FIXED_TO_FLOAT(hS);
	wallVerts[1].y = FIXED_TO_FLOAT(lS);

	// TODO: Actually use the surface's flags so that I don't have to do this
	FUINT blendmode = Surf.PolyFlags;

	// Render midtextures on two-sided lines with a z-buffer offset.
	// This will cause the midtexture appear on top, if a FOF overlaps with it.
	blendmode |= PF_Decal;

	extracolormap_t *colormap = gl_frontsector->extra_colormap;

	if (gl_frontsector->numlights)
	{
		if (!(blendmode & PF_Masked))
			HWR_SplitWall(gl_frontsector, wallVerts, gl_midtexture, &Surf, FOF_TRANSLUCENT, NULL, blendmode);
		else
			HWR_SplitWall(gl_frontsector, wallVerts, gl_midtexture, &Surf, FOF_CUTLEVEL, NULL, blendmode);
	}
	else if (!(blendmode & PF_Masked))
		HWR_AddTransparentWall(wallVerts, &Surf, gl_midtexture, blendmode, false, lightnum, colormap);
	else
		HWR_ProjectWall(wallVerts, &Surf, blendmode, lightnum, colormap);
}

// Sort of like GLWall::Process in GZDoom
static void HWR_ProcessSeg(void)
{
	HWD_LOCAL; // OPT11: the parts of this function (HWPROF34, --hwdetail)
	FOutVector wallVerts[4];
	v2d_t vs, ve; // start, end vertices of 2d line (view from above)

	fixed_t worldtop, worldbottom;
	fixed_t worldhigh = 0, worldlow = 0;
	fixed_t worldtopslope, worldbottomslope;
	fixed_t worldhighslope = 0, worldlowslope = 0;
	fixed_t v1x, v1y, v2x, v2y;

	fixed_t h, l; // 3D sides and 2s middle textures
	fixed_t hS, lS;
	float xscale, yscale;

	gl_sidedef = gl_curline->sidedef;
	gl_linedef = gl_curline->linedef;

	if (gl_curline->pv1)
	{
		vs.x = ((polyvertex_t *)gl_curline->pv1)->x;
		vs.y = ((polyvertex_t *)gl_curline->pv1)->y;
	}
	else
	{
		vs.x = FIXED_TO_FLOAT(gl_curline->v1->x);
		vs.y = FIXED_TO_FLOAT(gl_curline->v1->y);
	}
	if (gl_curline->pv2)
	{
		ve.x = ((polyvertex_t *)gl_curline->pv2)->x;
		ve.y = ((polyvertex_t *)gl_curline->pv2)->y;
	}
	else
	{
		ve.x = FIXED_TO_FLOAT(gl_curline->v2->x);
		ve.y = FIXED_TO_FLOAT(gl_curline->v2->y);
	}

	v1x = FLOAT_TO_FIXED(vs.x);
	v1y = FLOAT_TO_FIXED(vs.y);
	v2x = FLOAT_TO_FIXED(ve.x);
	v2y = FLOAT_TO_FIXED(ve.y);

#define SLOPEPARAMS(slope, end1, end2, normalheight) \
	end1 = P_GetZAt(slope, v1x, v1y, normalheight); \
	end2 = P_GetZAt(slope, v2x, v2y, normalheight);

	SLOPEPARAMS(gl_frontsector->c_slope, worldtop,    worldtopslope,    gl_frontsector->ceilingheight)
	SLOPEPARAMS(gl_frontsector->f_slope, worldbottom, worldbottomslope, gl_frontsector->floorheight)

	// remember vertices ordering
	//  3--2
	//  | /|
	//  |/ |
	//  0--1
	// make a wall polygon (with 2 triangles), using the floor/ceiling heights,
	// and the 2d map coords of start/end vertices
	wallVerts[0].x = wallVerts[3].x = vs.x;
	wallVerts[0].z = wallVerts[3].z = vs.y;
	wallVerts[2].x = wallVerts[1].x = ve.x;
	wallVerts[2].z = wallVerts[1].z = ve.y;

	// x offset the texture
	float cliplow = (float)gl_curline->offset;
	float cliphigh = cliplow + (gl_curline->flength * FRACUNIT);

	FUINT lightnum = HWR_SideLightLevel(gl_sidedef, gl_frontsector->lightlevel);
	extracolormap_t *colormap = gl_frontsector->extra_colormap;
#ifdef PS2_PROFILE
	lightnum = colormap ? lightnum : HWR_CalcSegLight(lightnum, gl_curline, vs.x, vs.y, ve.x, ve.y);
#else
	lightnum = colormap ? lightnum : HWR_CalcWallLight(lightnum, vs.x, vs.y, ve.x, ve.y);
#endif

	FSurfaceInfo Surf;
	Surf.PolyColor.s.alpha = 255;

	INT32 gl_midtexture = R_GetTextureNum(gl_sidedef->midtexture);
	GLMapTexture_t *grTex = NULL;

	HWD_LAP(HWP_SG_A);
	// two sided line
	if (gl_backsector)
	{
		INT32 gl_toptexture = 0, gl_bottomtexture = 0;

		fixed_t texturevpeg;

		SLOPEPARAMS(gl_backsector->c_slope, worldhigh, worldhighslope, gl_backsector->ceilingheight)
		SLOPEPARAMS(gl_backsector->f_slope, worldlow,  worldlowslope,  gl_backsector->floorheight)

		// hack to allow height changes in outdoor areas
		// This is what gets rid of the upper textures if there should be sky
		if (gl_frontsector->ceilingpic == skyflatnum
			&& gl_backsector->ceilingpic  == skyflatnum)
		{
			bothceilingssky = true;
		}

		// likewise, but for floors and upper textures
		if (gl_frontsector->floorpic == skyflatnum
			&& gl_backsector->floorpic == skyflatnum)
		{
			bothfloorssky = true;
		}

		if (!bothceilingssky)
			gl_toptexture = R_GetTextureNum(gl_sidedef->toptexture);
		if (!bothfloorssky)
			gl_bottomtexture = R_GetTextureNum(gl_sidedef->bottomtexture);

		HWD_LAP(HWP_SG_B);
		// check TOP TEXTURE
		if ((worldhighslope < worldtopslope || worldhigh < worldtop) && gl_toptexture)
		{
			grTex = HWR_GetTexture(gl_toptexture, false);
			xscale = FixedToFloat(abs(SIDE_SCALEX_TOP(gl_sidedef)));
			yscale = FixedToFloat(abs(SIDE_SCALEY_TOP(gl_sidedef)));

			fixed_t offsetx_top = gl_sidedef->textureoffset + SIDE_OFFSETX_TOP(gl_sidedef);

			float left = cliplow * xscale;
			float right = cliphigh * xscale;
			if (SIDE_SCALEX_TOP(gl_sidedef) < 0)
			{
				left = -left;
				right = -right;
				offsetx_top = -offsetx_top;
			}

			fixed_t texheight = textureheight[gl_toptexture];
			fixed_t texheightscaled = HWR_DivScale(texheight, abs(SIDE_SCALEY_TOP(gl_sidedef)));

			// PEGGING
			// FIXME: This is probably not correct?
			if (gl_linedef->flags & ML_DONTPEGTOP)
				texturevpeg = 0;
			else if (gl_linedef->flags & ML_SKEWTD)
				texturevpeg = worldhigh + texheight - worldtop;
			else
				texturevpeg = gl_backsector->ceilingheight + texheightscaled - gl_frontsector->ceilingheight;

			texturevpeg *= yscale;

			if (SIDE_SCALEY_TOP(gl_sidedef) < 0)
				texturevpeg -= gl_sidedef->rowoffset + SIDE_OFFSETY_TOP(gl_sidedef);
			else
				texturevpeg += gl_sidedef->rowoffset + SIDE_OFFSETY_TOP(gl_sidedef);

			// This is so that it doesn't overflow and screw up the wall, it doesn't need to go higher than the texture's height anyway
			texturevpeg = HWR_ModHeight(texturevpeg, texheightscaled);

			wallVerts[3].t = wallVerts[2].t = texturevpeg * grTex->scaleY;
			wallVerts[0].t = wallVerts[1].t = (texturevpeg + (gl_frontsector->ceilingheight - gl_backsector->ceilingheight) * yscale) * grTex->scaleY;
			wallVerts[0].s = wallVerts[3].s = (left + offsetx_top) * grTex->scaleX;
			wallVerts[2].s = wallVerts[1].s = (right + offsetx_top) * grTex->scaleX;

			// Adjust t value for sloped walls
			if (!(gl_linedef->flags & ML_SKEWTD))
			{
				// Unskewed
				wallVerts[3].t -= (worldtop - gl_frontsector->ceilingheight) * yscale * grTex->scaleY;
				wallVerts[2].t -= (worldtopslope - gl_frontsector->ceilingheight) * yscale * grTex->scaleY;
				wallVerts[0].t -= (worldhigh - gl_backsector->ceilingheight) * yscale * grTex->scaleY;
				wallVerts[1].t -= (worldhighslope - gl_backsector->ceilingheight) * yscale * grTex->scaleY;
			}
			else if (gl_linedef->flags & ML_DONTPEGTOP)
			{
				// Skewed by top
				wallVerts[0].t = (texturevpeg + (worldtop - worldhigh) * yscale) * grTex->scaleY;
				wallVerts[1].t = (texturevpeg + (worldtopslope - worldhighslope) * yscale) * grTex->scaleY;
			}
			else
			{
				// Skewed by bottom
				wallVerts[0].t = wallVerts[1].t = (texturevpeg + (worldtop - worldhigh) * yscale) * grTex->scaleY;
				wallVerts[3].t = wallVerts[0].t - (worldtop - worldhigh) * yscale * grTex->scaleY;
				wallVerts[2].t = wallVerts[1].t - (worldtopslope - worldhighslope) * yscale * grTex->scaleY;
			}

			if (SIDE_SCALEY_TOP(gl_sidedef) < 0)
			{
				wallVerts[0].t = -wallVerts[0].t;
				wallVerts[1].t = -wallVerts[1].t;
				wallVerts[2].t = -wallVerts[2].t;
				wallVerts[3].t = -wallVerts[3].t;
			}

			// set top/bottom coords
			wallVerts[3].y = FIXED_TO_FLOAT(worldtop);
			wallVerts[0].y = FIXED_TO_FLOAT(worldhigh);
			wallVerts[2].y = FIXED_TO_FLOAT(worldtopslope);
			wallVerts[1].y = FIXED_TO_FLOAT(worldhighslope);

			if (gl_frontsector->numlights)
				HWR_SplitWall(gl_frontsector, wallVerts, gl_toptexture, &Surf, FOF_CUTLEVEL, NULL, 0);
			else if (grTex->mipmap.flags & TF_TRANSPARENT)
				HWR_AddTransparentWall(wallVerts, &Surf, gl_toptexture, PF_Environment, false, lightnum, colormap);
			else
				HWR_ProjectWall(wallVerts, &Surf, PF_Masked, lightnum, colormap);
		}

		HWD_LAP(HWP_SG_C);
		// check BOTTOM TEXTURE
		if ((worldlowslope > worldbottomslope || worldlow > worldbottom) && gl_bottomtexture)
		{
			grTex = HWR_GetTexture(gl_bottomtexture, false);
			xscale = FixedToFloat(abs(SIDE_SCALEX_BOTTOM(gl_sidedef)));
			yscale = FixedToFloat(abs(SIDE_SCALEY_BOTTOM(gl_sidedef)));

			fixed_t offsetx_bottom = gl_sidedef->textureoffset + SIDE_OFFSETX_BOTTOM(gl_sidedef);

			float left = cliplow * xscale;
			float right = cliphigh * xscale;
			if (SIDE_SCALEX_BOTTOM(gl_sidedef) < 0)
			{
				left = -left;
				right = -right;
				offsetx_bottom = -offsetx_bottom;
			}

			// PEGGING
			if (!(gl_linedef->flags & ML_DONTPEGBOTTOM))
				texturevpeg = 0;
			else if (gl_linedef->flags & ML_SKEWTD)
				texturevpeg = worldbottom - worldlow;
			else
				texturevpeg = gl_frontsector->floorheight - gl_backsector->floorheight;

			texturevpeg *= yscale;

			if (SIDE_SCALEY_BOTTOM(gl_sidedef) < 0)
				texturevpeg -= gl_sidedef->rowoffset + SIDE_OFFSETY_BOTTOM(gl_sidedef);
			else
				texturevpeg += gl_sidedef->rowoffset + SIDE_OFFSETY_BOTTOM(gl_sidedef);

			// This is so that it doesn't overflow and screw up the wall, it doesn't need to go higher than the texture's height anyway
			texturevpeg = HWR_ModHeight(texturevpeg, HWR_DivScale(textureheight[gl_bottomtexture], abs(SIDE_SCALEY_BOTTOM(gl_sidedef))));

			wallVerts[3].t = wallVerts[2].t = texturevpeg * grTex->scaleY;
			wallVerts[0].t = wallVerts[1].t = (texturevpeg + (gl_backsector->floorheight - gl_frontsector->floorheight) * yscale) * grTex->scaleY;
			wallVerts[0].s = wallVerts[3].s = (left + offsetx_bottom) * grTex->scaleX;
			wallVerts[2].s = wallVerts[1].s = (right + offsetx_bottom) * grTex->scaleX;

			// Adjust t value for sloped walls
			if (!(gl_linedef->flags & ML_SKEWTD))
			{
				// Unskewed
				wallVerts[0].t -= (worldbottom - gl_frontsector->floorheight) * yscale * grTex->scaleY;
				wallVerts[1].t -= (worldbottomslope - gl_frontsector->floorheight) * yscale * grTex->scaleY;
				wallVerts[3].t -= (worldlow - gl_backsector->floorheight) * yscale * grTex->scaleY;
				wallVerts[2].t -= (worldlowslope - gl_backsector->floorheight) * yscale * grTex->scaleY;
			}
			else if (gl_linedef->flags & ML_DONTPEGBOTTOM)
			{
				// Skewed by bottom
				wallVerts[0].t = wallVerts[1].t = (texturevpeg + (worldlow - worldbottom) * yscale) * grTex->scaleY;
				wallVerts[2].t = wallVerts[1].t - (worldlowslope - worldbottomslope) * yscale * grTex->scaleY;
			}
			else
			{
				// Skewed by top
				wallVerts[0].t = (texturevpeg + (worldlow - worldbottom) * yscale) * grTex->scaleY;
				wallVerts[1].t = (texturevpeg + (worldlowslope - worldbottomslope) * yscale) * grTex->scaleY;
			}

			if (SIDE_SCALEY_BOTTOM(gl_sidedef) < 0)
			{
				wallVerts[0].t = -wallVerts[0].t;
				wallVerts[1].t = -wallVerts[1].t;
				wallVerts[2].t = -wallVerts[2].t;
				wallVerts[3].t = -wallVerts[3].t;
			}

			// set top/bottom coords
			wallVerts[3].y = FIXED_TO_FLOAT(worldlow);
			wallVerts[0].y = FIXED_TO_FLOAT(worldbottom);
			wallVerts[2].y = FIXED_TO_FLOAT(worldlowslope);
			wallVerts[1].y = FIXED_TO_FLOAT(worldbottomslope);

			if (gl_frontsector->numlights)
				HWR_SplitWall(gl_frontsector, wallVerts, gl_bottomtexture, &Surf, FOF_CUTLEVEL, NULL, 0);
			else if (grTex->mipmap.flags & TF_TRANSPARENT)
				HWR_AddTransparentWall(wallVerts, &Surf, gl_bottomtexture, PF_Environment, false, lightnum, colormap);
			else
				HWR_ProjectWall(wallVerts, &Surf, PF_Masked, lightnum, colormap);
		}

		HWD_LAP(HWP_SG_D);
		// Render midtexture if there's one
		if (gl_midtexture)
			HWR_RenderMidtexture(gl_midtexture, cliplow, cliphigh, worldtop, worldbottom, worldhigh, worldlow, worldtopslope, worldbottomslope, worldhighslope, worldlowslope, lightnum, wallVerts);

		if (!gl_curline->polyseg) // Don't do it for polyobjects
		{
			HWD_LAP(HWP_SG_E);
			// Sky culling
			// No longer so much a mess as before!
			if (gl_frontsector->ceilingpic == skyflatnum
				&& gl_backsector->ceilingpic != skyflatnum) // don't cull if back sector is also sky
			{
				wallVerts[2].y = wallVerts[3].y = FIXED_TO_FLOAT(INT32_MAX); // draw to top of map space
				wallVerts[0].y = FIXED_TO_FLOAT(worldtop);
				wallVerts[1].y = FIXED_TO_FLOAT(worldtopslope);
				HWR_DrawSkyWall(wallVerts, &Surf);
			}

			if (gl_frontsector->floorpic == skyflatnum
				&& gl_backsector->floorpic != skyflatnum) // same thing here
			{
				wallVerts[3].y = FIXED_TO_FLOAT(worldbottom);
				wallVerts[2].y = FIXED_TO_FLOAT(worldbottomslope);
				wallVerts[0].y = wallVerts[1].y = FIXED_TO_FLOAT(INT32_MIN); // draw to bottom of map space
				HWR_DrawSkyWall(wallVerts, &Surf);
			}
		}
	}
	else
	{
		HWD_LAP(HWP_SG_B);
		// Single sided line... Deal only with the middletexture (if one exists)
		if (gl_midtexture && gl_linedef->special != SPECIAL_HORIZON_LINE) // (Ignore horizon line for OGL)
		{
			grTex = HWR_GetTexture(gl_midtexture, false);
			xscale = FixedToFloat(SIDE_SCALEX_MID(gl_sidedef));
			yscale = FixedToFloat(SIDE_SCALEY_MID(gl_sidedef));

			fixed_t texturevpeg;

			// PEGGING
			if ((gl_linedef->flags & (ML_DONTPEGBOTTOM|ML_NOSKEW)) == (ML_DONTPEGBOTTOM|ML_NOSKEW))
				texturevpeg = (gl_frontsector->floorheight + textureheight[gl_sidedef->midtexture] - gl_frontsector->ceilingheight) * yscale;
			else if (gl_linedef->flags & ML_DONTPEGBOTTOM)
				texturevpeg = (worldbottom + textureheight[gl_sidedef->midtexture] - worldtop) * yscale;
			else
				// top of texture at top
				texturevpeg = 0;

			texturevpeg += gl_sidedef->rowoffset + SIDE_OFFSETY_MID(gl_sidedef);

			wallVerts[3].t = wallVerts[2].t = texturevpeg * grTex->scaleY;
			wallVerts[0].t = wallVerts[1].t = (texturevpeg + gl_frontsector->ceilingheight - gl_frontsector->floorheight) * grTex->scaleY;
			wallVerts[0].s = wallVerts[3].s = ((cliplow * xscale) + gl_sidedef->textureoffset + SIDE_OFFSETX_MID(gl_sidedef)) * grTex->scaleX;
			wallVerts[2].s = wallVerts[1].s = ((cliphigh * xscale) + gl_sidedef->textureoffset + SIDE_OFFSETX_MID(gl_sidedef)) * grTex->scaleX;

			// Texture correction for slopes
			if (gl_linedef->flags & ML_NOSKEW) {
				wallVerts[3].t += (gl_frontsector->ceilingheight - worldtop) * yscale * grTex->scaleY;
				wallVerts[2].t += (gl_frontsector->ceilingheight - worldtopslope) * yscale * grTex->scaleY;
				wallVerts[0].t += (gl_frontsector->floorheight - worldbottom) * yscale * grTex->scaleY;
				wallVerts[1].t += (gl_frontsector->floorheight - worldbottomslope) * yscale * grTex->scaleY;
			} else if (gl_linedef->flags & ML_DONTPEGBOTTOM) {
				wallVerts[3].t = wallVerts[0].t + ((worldbottom - worldtop) * yscale) * grTex->scaleY;
				wallVerts[2].t = wallVerts[1].t + ((worldbottomslope - worldtopslope) * yscale) * grTex->scaleY;
			} else {
				wallVerts[0].t = wallVerts[3].t - ((worldbottom - worldtop) * yscale) * grTex->scaleY;
				wallVerts[1].t = wallVerts[2].t - ((worldbottomslope - worldtopslope) * yscale) * grTex->scaleY;
			}

			//Set textures properly on single sided walls that are sloped
			wallVerts[3].y = FIXED_TO_FLOAT(worldtop);
			wallVerts[0].y = FIXED_TO_FLOAT(worldbottom);
			wallVerts[2].y = FIXED_TO_FLOAT(worldtopslope);
			wallVerts[1].y = FIXED_TO_FLOAT(worldbottomslope);

			// I don't think that solid walls can use translucent linedef types...
			if (gl_frontsector->numlights)
				HWR_SplitWall(gl_frontsector, wallVerts, gl_midtexture, &Surf, FOF_CUTLEVEL, NULL, 0);
			else
			{
				if (grTex->mipmap.flags & TF_TRANSPARENT)
					HWR_AddTransparentWall(wallVerts, &Surf, gl_midtexture, PF_Environment, false, lightnum, colormap);
				else
					HWR_ProjectWall(wallVerts, &Surf, PF_Masked, lightnum, colormap);
			}
		}

		if (!gl_curline->polyseg)
		{
			if (gl_frontsector->ceilingpic == skyflatnum) // It's a single-sided line with sky for its sector
			{
				wallVerts[2].y = wallVerts[3].y = FIXED_TO_FLOAT(INT32_MAX); // draw to top of map space
				wallVerts[0].y = FIXED_TO_FLOAT(worldtop);
				wallVerts[1].y = FIXED_TO_FLOAT(worldtopslope);
				HWR_DrawSkyWall(wallVerts, &Surf);
			}
			if (gl_frontsector->floorpic == skyflatnum)
			{
				wallVerts[3].y = FIXED_TO_FLOAT(worldbottom);
				wallVerts[2].y = FIXED_TO_FLOAT(worldbottomslope);
				wallVerts[0].y = wallVerts[1].y = FIXED_TO_FLOAT(INT32_MIN); // draw to bottom of map space
				HWR_DrawSkyWall(wallVerts, &Surf);
			}
		}
	}

	HWD_LAP(HWP_SG_F);
	//Hurdler: 3d-floors test
	if (gl_backsector && !Tag_Compare(&gl_frontsector->tags, &gl_backsector->tags) && (gl_backsector->ffloors || gl_frontsector->ffloors))
	{
		ffloor_t * rover;
		fixed_t    highcut = 0, lowcut = 0;
		fixed_t lowcutslope, highcutslope;

		// Used for height comparisons and etc across FOFs and slopes
		fixed_t high1, highslope1, low1, lowslope1;

		fixed_t texturehpeg = gl_sidedef->textureoffset + SIDE_OFFSETX_MID(gl_sidedef);

		INT32 texnum;

		lowcut = max(worldbottom, worldlow);
		highcut = min(worldtop, worldhigh);
		lowcutslope = max(worldbottomslope, worldlowslope);
		highcutslope = min(worldtopslope, worldhighslope);

		if (gl_backsector->ffloors)
		{
			for (rover = gl_backsector->ffloors; rover; rover = rover->next)
			{
				boolean bothsides = false;
				// Skip if it exists on both sectors.
				ffloor_t * r2;
				for (r2 = gl_frontsector->ffloors; r2; r2 = r2->next)
					if (rover->master == r2->master)
					{
						bothsides = true;
						break;
					}

				if (bothsides) continue;

				if (!(rover->fofflags & FOF_EXISTS) || !(rover->fofflags & FOF_RENDERSIDES))
					continue;
				if (!(rover->fofflags & FOF_ALLSIDES) && rover->fofflags & FOF_INVERTSIDES)
					continue;

				SLOPEPARAMS(*rover->t_slope, high1, highslope1, *rover->topheight)
				SLOPEPARAMS(*rover->b_slope, low1,  lowslope1,  *rover->bottomheight)

				if ((high1 < lowcut && highslope1 < lowcutslope) || (low1 > highcut && lowslope1 > highcutslope))
					continue;

				side_t *side = R_GetFFloorSide(gl_curline->linedef, rover, gl_backsector);

				boolean do_texture_skew;
				boolean dont_peg_bottom;

				if (rover->master->flags & ML_TFERLINE)
				{
					line_t *newline = R_GetFFloorLine(gl_curline->linedef, rover, gl_backsector);
					do_texture_skew = newline->flags & ML_SKEWTD;
					dont_peg_bottom = newline->flags & ML_DONTPEGBOTTOM;
				}
				else
				{
					do_texture_skew = rover->master->flags & ML_SKEWTD;
					dont_peg_bottom = gl_curline->linedef->flags & ML_DONTPEGBOTTOM;
				}

				texnum = R_GetTextureNum(side->midtexture);

				h  = P_GetFFloorTopZAt   (rover, v1x, v1y);
				hS = P_GetFFloorTopZAt   (rover, v2x, v2y);
				l  = P_GetFFloorBottomZAt(rover, v1x, v1y);
				lS = P_GetFFloorBottomZAt(rover, v2x, v2y);
				// Adjust the heights so the FOF does not overlap with top and bottom textures.
				if (h >= highcut && hS >= highcutslope)
				{
					h = highcut;
					hS = highcutslope;
				}
				if (l <= lowcut && lS <= lowcutslope)
				{
					l = lowcut;
					lS = lowcutslope;
				}

				// set top/bottom coords
				wallVerts[3].y = FIXED_TO_FLOAT(h);
				wallVerts[2].y = FIXED_TO_FLOAT(hS);
				wallVerts[0].y = FIXED_TO_FLOAT(l);
				wallVerts[1].y = FIXED_TO_FLOAT(lS);

				if (rover->fofflags & FOF_FOG)
				{
					wallVerts[3].t = wallVerts[2].t = 0;
					wallVerts[0].t = wallVerts[1].t = 0;
					wallVerts[0].s = wallVerts[3].s = 0;
					wallVerts[2].s = wallVerts[1].s = 0;
				}
				else
				{
					// Wow, how was this missing from OpenGL for so long?
					// ...Oh well, anyway, Lower Unpegged now changes pegging of FOFs like in software
					// -- Monster Iestyn 26/06/18
					fixed_t texturevpeg = side->rowoffset + SIDE_OFFSETY_MID(side);

					grTex = HWR_GetTexture(texnum, true);
					xscale = FixedToFloat(SIDE_SCALEX_MID(side));
					yscale = FixedToFloat(SIDE_SCALEY_MID(side));

					if (!do_texture_skew) // no skewing
					{
						if (dont_peg_bottom)
							texturevpeg -= (*rover->topheight - *rover->bottomheight) * yscale;

						wallVerts[3].t = (((*rover->topheight - h) * yscale) + texturevpeg) * grTex->scaleY;
						wallVerts[2].t = (((*rover->topheight - hS) * yscale) + texturevpeg) * grTex->scaleY;
						wallVerts[0].t = (((*rover->topheight - l) * yscale) + texturevpeg) * grTex->scaleY;
						wallVerts[1].t = (((*rover->topheight - lS) * yscale) + texturevpeg) * grTex->scaleY;
					}
					else
					{
						if (!dont_peg_bottom) // skew by top
						{
							wallVerts[3].t = wallVerts[2].t = texturevpeg * grTex->scaleY;
							wallVerts[0].t = (((h - l) * yscale) + texturevpeg) * grTex->scaleY;
							wallVerts[1].t = (((hS - lS) * yscale) + texturevpeg) * grTex->scaleY;
						}
						else // skew by bottom
						{
							wallVerts[0].t = wallVerts[1].t = texturevpeg * grTex->scaleY;
							wallVerts[3].t = wallVerts[0].t - ((h - l) * yscale) * grTex->scaleY;
							wallVerts[2].t = wallVerts[1].t - ((hS - lS) * yscale) * grTex->scaleY;
						}
					}

					wallVerts[0].s = wallVerts[3].s = ((cliplow * xscale) + texturehpeg + SIDE_OFFSETX_MID(side)) * grTex->scaleX;
					wallVerts[2].s = wallVerts[1].s = ((cliphigh * xscale) + texturehpeg + SIDE_OFFSETX_MID(side)) * grTex->scaleX;
				}

				FBITFIELD blendmode;

				if (rover->fofflags & FOF_FOG)
				{
					blendmode = PF_Fog|PF_NoTexture;

					lightnum = HWR_SideLightLevel(gl_sidedef, rover->master->frontsector->lightlevel);
					colormap = rover->master->frontsector->extra_colormap;
					lightnum = colormap ? lightnum : HWR_CalcWallLight(lightnum, vs.x, vs.y, ve.x, ve.y);

					Surf.PolyColor.s.alpha = HWR_FogBlockAlpha(HWR_SideLightLevel(gl_sidedef, rover->master->frontsector->lightlevel), rover->master->frontsector->extra_colormap);

					if (gl_frontsector->numlights)
						HWR_SplitWall(gl_frontsector, wallVerts, 0, &Surf, rover->fofflags, rover, blendmode);
					else
						HWR_AddTransparentWall(wallVerts, &Surf, 0, blendmode, true, lightnum, colormap);
				}
				else
				{
					blendmode = PF_Masked;

					if ((rover->fofflags & FOF_TRANSLUCENT && !((rover->fofflags & FOF_SPLAT) && rover->alpha >= 255)) || rover->blend)
					{
						blendmode = rover->blend ? HWR_GetBlendModeFlag(rover->blend) : PF_Translucent;
						Surf.PolyColor.s.alpha = max(0, min(rover->alpha, 255));
					}

					if (gl_frontsector->numlights)
						HWR_SplitWall(gl_frontsector, wallVerts, texnum, &Surf, rover->fofflags, rover, blendmode);
					else
					{
						if (blendmode != PF_Masked)
							HWR_AddTransparentWall(wallVerts, &Surf, texnum, blendmode, false, lightnum, colormap);
						else
							HWR_ProjectWall(wallVerts, &Surf, PF_Masked, lightnum, colormap);
					}
				}
			}
		}

		if (gl_frontsector->ffloors) // Putting this seperate should allow 2 FOF sectors to be connected without too many errors? I think?
		{
			for (rover = gl_frontsector->ffloors; rover; rover = rover->next)
			{
				boolean bothsides = false;
				// Skip if it exists on both sectors.
				ffloor_t * r2;
				for (r2 = gl_backsector->ffloors; r2; r2 = r2->next)
					if (rover->master == r2->master)
					{
						bothsides = true;
						break;
					}

				if (bothsides) continue;

				if (!(rover->fofflags & FOF_EXISTS) || !(rover->fofflags & FOF_RENDERSIDES))
					continue;
				if (!(rover->fofflags & FOF_ALLSIDES || rover->fofflags & FOF_INVERTSIDES))
					continue;

				SLOPEPARAMS(*rover->t_slope, high1, highslope1, *rover->topheight)
				SLOPEPARAMS(*rover->b_slope, low1,  lowslope1,  *rover->bottomheight)

				if ((high1 < lowcut && highslope1 < lowcutslope) || (low1 > highcut && lowslope1 > highcutslope))
					continue;

				side_t *side = R_GetFFloorSide(gl_curline->linedef, rover, gl_backsector);

				boolean do_texture_skew;
				boolean dont_peg_bottom;

				if (rover->master->flags & ML_TFERLINE)
				{
					line_t *newline = R_GetFFloorLine(gl_curline->linedef, rover, gl_backsector);
					do_texture_skew = newline->flags & ML_SKEWTD;
					dont_peg_bottom = newline->flags & ML_DONTPEGBOTTOM;
				}
				else
				{
					do_texture_skew = rover->master->flags & ML_SKEWTD;
					dont_peg_bottom = gl_curline->linedef->flags & ML_DONTPEGBOTTOM;
				}

				texnum = R_GetTextureNum(side->midtexture);

				h  = P_GetFFloorTopZAt   (rover, v1x, v1y);
				hS = P_GetFFloorTopZAt   (rover, v2x, v2y);
				l  = P_GetFFloorBottomZAt(rover, v1x, v1y);
				lS = P_GetFFloorBottomZAt(rover, v2x, v2y);
				// Adjust the heights so the FOF does not overlap with top and bottom textures.
				if (h >= highcut && hS >= highcutslope)
				{
					h = highcut;
					hS = highcutslope;
				}
				if (l <= lowcut && lS <= lowcutslope)
				{
					l = lowcut;
					lS = lowcutslope;
				}
				//Hurdler: HW code starts here
				//FIXME: check if peging is correct
				// set top/bottom coords

				wallVerts[3].y = FIXED_TO_FLOAT(h);
				wallVerts[2].y = FIXED_TO_FLOAT(hS);
				wallVerts[0].y = FIXED_TO_FLOAT(l);
				wallVerts[1].y = FIXED_TO_FLOAT(lS);
				if (rover->fofflags & FOF_FOG)
				{
					wallVerts[3].t = wallVerts[2].t = 0;
					wallVerts[0].t = wallVerts[1].t = 0;
					wallVerts[0].s = wallVerts[3].s = 0;
					wallVerts[2].s = wallVerts[1].s = 0;
				}
				else
				{
					// Wow, how was this missing from OpenGL for so long?
					// ...Oh well, anyway, Lower Unpegged now changes pegging of FOFs like in software
					// -- Monster Iestyn 26/06/18
					fixed_t texturevpeg = side->rowoffset + SIDE_OFFSETY_MID(side);

					grTex = HWR_GetTexture(texnum, true);
					xscale = FixedToFloat(SIDE_SCALEX_MID(side));
					yscale = FixedToFloat(SIDE_SCALEY_MID(side));

					if (!do_texture_skew) // no skewing
					{
						if (dont_peg_bottom)
							texturevpeg -= (*rover->topheight - *rover->bottomheight) * yscale;

						wallVerts[3].t = (((*rover->topheight - h) * yscale) + texturevpeg) * grTex->scaleY;
						wallVerts[2].t = (((*rover->topheight - hS) * yscale) + texturevpeg) * grTex->scaleY;
						wallVerts[0].t = (((*rover->topheight - l) * yscale) + texturevpeg) * grTex->scaleY;
						wallVerts[1].t = (((*rover->topheight - lS) * yscale) + texturevpeg) * grTex->scaleY;
					}
					else
					{
						if (!dont_peg_bottom) // skew by top
						{
							wallVerts[3].t = wallVerts[2].t = texturevpeg * grTex->scaleY;
							wallVerts[0].t = (((h - l) * yscale) + texturevpeg) * grTex->scaleY;
							wallVerts[1].t = (((hS - lS) * yscale) + texturevpeg) * grTex->scaleY;
						}
						else // skew by bottom
						{
							wallVerts[0].t = wallVerts[1].t = texturevpeg * grTex->scaleY;
							wallVerts[3].t = wallVerts[0].t - ((h - l) * yscale) * grTex->scaleY;
							wallVerts[2].t = wallVerts[1].t - ((hS - lS) * yscale) * grTex->scaleY;
						}
					}

					wallVerts[0].s = wallVerts[3].s = ((cliplow * xscale) + texturehpeg + SIDE_OFFSETX_MID(side)) * grTex->scaleX;
					wallVerts[2].s = wallVerts[1].s = ((cliphigh * xscale) + texturehpeg + SIDE_OFFSETX_MID(side)) * grTex->scaleX;
				}

				FBITFIELD blendmode;

				if (rover->fofflags & FOF_FOG)
				{
					blendmode = PF_Fog|PF_NoTexture;

					lightnum = HWR_SideLightLevel(gl_sidedef, rover->master->frontsector->lightlevel);
					colormap = rover->master->frontsector->extra_colormap;
					lightnum = colormap ? lightnum : HWR_CalcWallLight(lightnum, vs.x, vs.y, ve.x, ve.y);

					Surf.PolyColor.s.alpha = HWR_FogBlockAlpha(rover->master->frontsector->lightlevel, rover->master->frontsector->extra_colormap);

					if (gl_backsector->numlights)
						HWR_SplitWall(gl_backsector, wallVerts, 0, &Surf, rover->fofflags, rover, blendmode);
					else
						HWR_AddTransparentWall(wallVerts, &Surf, 0, blendmode, true, lightnum, colormap);
				}
				else
				{
					blendmode = PF_Masked;

					if ((rover->fofflags & FOF_TRANSLUCENT && !((rover->fofflags & FOF_SPLAT) && rover->alpha >= 255)) || rover->blend)
					{
						blendmode = rover->blend ? HWR_GetBlendModeFlag(rover->blend) : PF_Translucent;
						Surf.PolyColor.s.alpha = max(0, min(rover->alpha, 255));
					}

					if (gl_backsector->numlights)
						HWR_SplitWall(gl_backsector, wallVerts, texnum, &Surf, rover->fofflags, rover, blendmode);
					else
					{
						if (blendmode != PF_Masked)
							HWR_AddTransparentWall(wallVerts, &Surf, texnum, blendmode, false, lightnum, colormap);
						else
							HWR_ProjectWall(wallVerts, &Surf, PF_Masked, lightnum, colormap);
					}
				}
			}
		}
	}
#undef SLOPEPARAMS
	HWD_LAP(HWP_SG_G);
//Hurdler: end of 3d-floors test
}

#ifdef PS2_PROFILE // PS2-HW-40: inclusive timer of the wall builder (HWPROF2 "seg"); OPT11 PS2-HW-80: it is served from the geometry cache when it can be
static void HWR_ProcessSegC(void)
{
	HWP_SPAN_BEGIN(t);
	seg_t *sg = gl_curline;

	HWC_ADD(HWC_SEGS);
	if (!gc.on || sg->polyseg || !currently_batching || (gc.mode & 8))
	{
		gc.s_seg_skip++;
		HWR_ProcessSeg();
	}
	else
	{
		gcfp_t key;
		gcent_t *e;
		boolean hit, check = false;
		const UINT32 id = (UINT32)(sg - segs) * 2u;
		const UINT32 t0 = ps2hwp_now();

		gc_seg_key(sg, gl_frontsector, gl_backsector, &key);
		e = gc_find(id, 0, &hit);
		gc.c_key += ps2hwp_now() - t0;
		if (hit && e->k0 == key.a && e->k1 == key.b)
		{
			if (!(check = gc_check_this()))
			{
				const UINT32 t1 = ps2hwp_now();

				gl_sidedef = sg->sidedef;
				gl_linedef = sg->linedef;
				gc_replay(gc.ar + e->off, e->len);
				gc.s_seg_hit++;
				gc.c_hit += ps2hwp_now() - t1;
				HWP_SPAN_END(t, HWP_SEG);
				return;
			}
		}
		{
			const UINT32 t1 = ps2hwp_now();
			const UINT32 bad0 = gc.s_bad;

			gc_rec_begin(check);
			HWR_ProcessSeg();
			gc_rec_end(e, id, 0, &key, check, "seg", (UINT32)(sg - segs));
			if (gc.s_bad != bad0 && gc.reports <= 24)
				I_OutputMsg("HWGC seg %u: line %d special %d front sector %d (ff %d lights %d fslope %d/%d hs %d) back %d (ff %d lights %d slopes %d/%d hs %d) tex %d/%d/%d sd off %d/%d polyseg %d\n", (unsigned)(sg - segs), (int)(sg->linedef - lines), (int)sg->linedef->special,
					(int)(sg->frontsector - sectors), sg->frontsector->ffloors ? 1 : 0, (int)sg->frontsector->numlights, sg->frontsector->f_slope ? 1 : 0, sg->frontsector->c_slope ? 1 : 0, (int)sg->frontsector->heightsec,
					sg->backsector ? (int)(sg->backsector - sectors) : -1, sg->backsector && sg->backsector->ffloors ? 1 : 0, sg->backsector ? (int)sg->backsector->numlights : 0,
					sg->backsector && sg->backsector->f_slope ? 1 : 0, sg->backsector && sg->backsector->c_slope ? 1 : 0, sg->backsector ? (int)sg->backsector->heightsec : 0,
					(int)sg->sidedef->toptexture, (int)sg->sidedef->midtexture, (int)sg->sidedef->bottomtexture, (int)sg->sidedef->textureoffset, (int)sg->sidedef->rowoffset, sg->polyseg ? 1 : 0);
			if (check)
				gc.s_seg_hit++;
			else
				gc.s_seg_miss++;
			gc.c_miss += ps2hwp_now() - t1;
		}
	}
	HWP_SPAN_END(t, HWP_SEG);
}
#define HWR_ProcessSeg HWR_ProcessSegC
#endif

// From PrBoom:
//
// e6y: Check whether the player can look beyond this line
//
static boolean checkforemptylines = true;

// Don't modify anything here, just check
// Kalaron: Modified for sloped linedefs
static boolean CheckClip(seg_t * seg, sector_t * afrontsector, sector_t * abacksector)
{
	fixed_t frontf1,frontf2, frontc1, frontc2; // front floor/ceiling ends
	fixed_t backf1, backf2, backc1, backc2; // back floor ceiling ends

	// GZDoom method of sloped line clipping

	if (afrontsector->f_slope || afrontsector->c_slope || abacksector->f_slope || abacksector->c_slope)
	{
		fixed_t v1x, v1y, v2x, v2y; // the seg's vertexes as fixed_t
		if (gl_curline->pv1)
		{
			v1x = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv1)->x);
			v1y = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv1)->y);
		}
		else
		{
			v1x = gl_curline->v1->x;
			v1y = gl_curline->v1->y;
		}
		if (gl_curline->pv2)
		{
			v2x = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv2)->x);
			v2y = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv2)->y);
		}
		else
		{
			v2x = gl_curline->v2->x;
			v2y = gl_curline->v2->y;
		}
#define SLOPEPARAMS(slope, end1, end2, normalheight) \
		end1 = P_GetZAt(slope, v1x, v1y, normalheight); \
		end2 = P_GetZAt(slope, v2x, v2y, normalheight);

		SLOPEPARAMS(afrontsector->f_slope, frontf1, frontf2, afrontsector->  floorheight)
		SLOPEPARAMS(afrontsector->c_slope, frontc1, frontc2, afrontsector->ceilingheight)
		SLOPEPARAMS( abacksector->f_slope,  backf1,  backf2,  abacksector->  floorheight)
		SLOPEPARAMS( abacksector->c_slope,  backc1,  backc2,  abacksector->ceilingheight)
#undef SLOPEPARAMS
	}
	else
	{
		frontf1 = frontf2 = afrontsector->  floorheight;
		frontc1 = frontc2 = afrontsector->ceilingheight;
		backf1  =  backf2 =  abacksector->  floorheight;
		backc1  =  backc2 =  abacksector->ceilingheight;
	}
	// properly render skies (consider door "open" if both ceilings are sky)
	// same for floors
	if (!bothceilingssky && !bothfloorssky)
	{
		// now check for closed sectors!
		if ((backc1 <= frontf1 && backc2 <= frontf2)
			|| (backf1 >= frontc1 && backf2 >= frontc2))
		{
			checkforemptylines = false;
			return true;
		}

		if (backc1 <= backf1 && backc2 <= backf2)
		{
			// preserve a kind of transparent door/lift special effect:
			if (((backc1 >= frontc1 && backc2 >= frontc2) || seg->sidedef->toptexture)
			&& ((backf1 <= frontf1 && backf2 <= frontf2) || seg->sidedef->bottomtexture))
			{
				checkforemptylines = false;
				return true;
			}
		}
	}

	if (!bothceilingssky) {
		if (backc1 != frontc1 || backc2 != frontc2)
		{
			checkforemptylines = false;
			return false;
		}
	}

	if (!bothfloorssky) {
		if (backf1 != frontf1 || backf2 != frontf2)
		{
			checkforemptylines = false;
			return false;
		}
	}

	return false;
}

// -----------------+
// HWR_AddLine      : Clips the given segment and adds any visible pieces to the line list.
// Notes            : gl_cursectorlight is set to the current subsector -> sector -> light value
//                  : (it may be mixed with the wall's own flat colour in the future ...)
// -----------------+
static void HWR_AddLine(seg_t * line)
{
	angle_t angle1, angle2;

	// SoM: Backsector needs to be run through R_FakeFlat
	static sector_t tempsec;

	fixed_t v1x, v1y, v2x, v2y; // the seg's vertexes as fixed_t
	if (line->polyseg && !(line->polyseg->flags & POF_RENDERSIDES))
		return;

	gl_curline = line;

	if (gl_curline->pv1)
	{
		v1x = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv1)->x);
		v1y = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv1)->y);
	}
	else
	{
		v1x = gl_curline->v1->x;
		v1y = gl_curline->v1->y;
	}
	if (gl_curline->pv2)
	{
		v2x = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv2)->x);
		v2y = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv2)->y);
	}
	else
	{
		v2x = gl_curline->v2->x;
		v2y = gl_curline->v2->y;
	}

	// OPTIMIZE: quickly reject orthogonal back sides.
#ifdef PS2_PROFILE
	{
		// OPT11 PS2-HW-81: the segs of a subsector follow one another: the second vertex of the one is mostly the first of the next. R_PointToAngle64 is a function of the
		// point and of the eye only, so the angle of the last second vertex is the angle of this first one (-hwgo 1: both angles every time)
		static fixed_t al_x, al_y, al_vx, al_vy;
		static angle_t al_angle;
		static boolean al_valid;

		if (al_valid && !(hwr_geo_off & 1) && v1x == al_x && v1y == al_y && viewx == al_vx && viewy == al_vy)
			angle1 = al_angle;
		else
			angle1 = R_PointToAngle64(v1x, v1y);
		angle2 = R_PointToAngle64(v2x, v2y);
		al_x = v2x;
		al_y = v2y;
		al_vx = viewx;
		al_vy = viewy;
		al_angle = angle2;
		al_valid = true;
	}
#else
	angle1 = R_PointToAngle64(v1x, v1y);
	angle2 = R_PointToAngle64(v2x, v2y);
#endif
	HWD_ADD(HWC_AL_CALLS);

	// PrBoom: Back side, i.e. backface culling - read: endAngle >= startAngle!
	if (angle2 - angle1 < ANGLE_180)
	{
		HWD_ADD(HWC_AL_BACK);
		return;
	}

	// PrBoom: use REAL clipping math YAYYYYYYY!!!

	if (!gld_clipper_SafeCheckRange(angle2, angle1))
    {
		HWD_ADD(HWC_AL_CLIP);
		return;
    }

	checkforemptylines = true;

	gl_backsector = line->backsector;
	bothceilingssky = bothfloorssky = false;

	if (!line->backsector)
    {
		gld_clipper_SafeAddClipRange(angle2, angle1);
    }
    else
    {
		gl_backsector = R_FakeFlat(gl_backsector, &tempsec, NULL, NULL, true);

		if (gl_backsector->ceilingpic == skyflatnum && gl_frontsector->ceilingpic == skyflatnum
		&& !(P_SectorHasCeilingPortal(gl_backsector) || P_SectorHasCeilingPortal(gl_frontsector)))
			bothceilingssky = true;

		if (gl_backsector->floorpic == skyflatnum && gl_frontsector->floorpic == skyflatnum
		&& !(P_SectorHasFloorPortal(gl_backsector) || P_SectorHasFloorPortal(gl_frontsector)))
			bothfloorssky = true;

		if (bothceilingssky && bothfloorssky) // everything's sky? let's save us a bit of time then
		{
			if (!line->polyseg &&
				!line->sidedef->midtexture
				&& ((!gl_frontsector->ffloors && !gl_backsector->ffloors)
					|| Tag_Compare(&gl_frontsector->tags, &gl_backsector->tags)))
				return; // line is empty, don't even bother
			// treat like wide open window instead
			HWR_ProcessSeg(); // Doesn't need arguments because they're defined globally :D
			return;
		}

		if (CheckClip(line, gl_frontsector, gl_backsector))
		{
			gld_clipper_SafeAddClipRange(angle2, angle1);
			checkforemptylines = false;
		}
		// Reject empty lines used for triggers and special events.
		// Identical floor and ceiling on both sides,
		//  identical light levels on both sides,
		//  and no middle texture.
		if (checkforemptylines && R_IsEmptyLine(line, gl_frontsector, gl_backsector))
		{
			HWD_ADD(HWC_AL_EMPTY);
			return;
		}
    }

	HWR_ProcessSeg(); // Doesn't need arguments because they're defined globally :D
}

// HWR_CheckBBox
// Checks BSP node/subtree bounding box.
// Returns true
//  if some part of the bbox might be visible.
//
// modified to use local variables

static boolean HWR_CheckBBox(fixed_t *bspcoord)
{
	INT32 boxpos;
	fixed_t px1, py1, px2, py2;
	angle_t angle1, angle2;

	// Find the corners of the box
	// that define the edges from current viewpoint.
	if (viewx <= bspcoord[BOXLEFT])
		boxpos = 0;
	else if (viewx < bspcoord[BOXRIGHT])
		boxpos = 1;
	else
		boxpos = 2;

	if (viewy >= bspcoord[BOXTOP])
		boxpos |= 0;
	else if (viewy > bspcoord[BOXBOTTOM])
		boxpos |= 1<<2;
	else
		boxpos |= 2<<2;

	HWD_ADD(HWC_AL_BOX);
	if (boxpos == 5)
		return true;

	px1 = bspcoord[checkcoord[boxpos][0]];
	py1 = bspcoord[checkcoord[boxpos][1]];
	px2 = bspcoord[checkcoord[boxpos][2]];
	py2 = bspcoord[checkcoord[boxpos][3]];

	angle1 = R_PointToAngle64(px1, py1);
	angle2 = R_PointToAngle64(px2, py2);
	if (!gld_clipper_SafeCheckRange(angle2, angle1))
	{
		HWD_ADD(HWC_AL_BOXREJ);
		return false;
	}
	return true;
}

//
// HWR_AddPolyObjectSegs
//
// haleyjd 02/19/06
// Adds all segs in all polyobjects in the given subsector.
// Modified for hardware rendering.
//
static inline void HWR_AddPolyObjectSegs(void)
{
	size_t i, j;

	// Sort through all the polyobjects
	for (i = 0; i < numpolys; ++i)
	{
		// Render the polyobject's lines
		for (j = 0; j < po_ptrs[i]->segCount; ++j)
			HWR_AddLine(po_ptrs[i]->segs[j]);
	}
}

static void HWR_RenderPolyObjectPlane(polyobj_t *polysector, boolean isceiling, fixed_t fixedheight,
									FBITFIELD blendmode, UINT8 lightlevel, levelflat_t *levelflat, sector_t *FOFsector,
									UINT8 alpha, extracolormap_t *planecolormap)
{
	FSurfaceInfo Surf;
	FOutVector *v3d;
	INT32 shader = SHADER_NONE;

	size_t nrPlaneVerts = polysector->numVertices;
	INT32 i;

	float height = FIXED_TO_FLOAT(fixedheight); // constant y for all points on the convex flat polygon
	float fflatwidth = 64.0f, fflatheight = 64.0f;
	float xscale = 1.0f, yscale = 1.0f;

	float scrollx = 0.0f, scrolly = 0.0f;
	float tempxsow, tempytow, anglef = 0.0f;
	angle_t angle = 0;

	static FOutVector *planeVerts = NULL;
	static UINT16 numAllocedPlaneVerts = 0;

	if (!r_renderfloors || nrPlaneVerts < 3)   // Not even a triangle?
		return;

	// Allocate plane-vertex buffer if we need to
	if (!planeVerts || nrPlaneVerts > numAllocedPlaneVerts)
	{
		numAllocedPlaneVerts = (UINT16)nrPlaneVerts;
		Z_Free(planeVerts);
		Z_Malloc(numAllocedPlaneVerts * sizeof (FOutVector), PU_LEVEL, &planeVerts);
	}

	// set texture for polygon
	if (levelflat != NULL)
	{
		texture_t *texture = textures[R_GetTextureNumForFlat(levelflat)];
		fflatwidth = texture->width;
		fflatheight = texture->height;
	}
	else // set no texture
		HWR_SetCurrentTexture(NULL);

	// transform
	v3d = planeVerts;

	if (FOFsector != NULL)
	{
		if (!isceiling) // it's a floor
		{
			xscale = FixedToFloat(FOFsector->floorxscale);
			yscale = FixedToFloat(FOFsector->flooryscale);
			scrollx = FixedToFloat(FOFsector->floorxoffset) / fflatwidth;
			scrolly = FixedToFloat(FOFsector->flooryoffset) / fflatheight;
			angle = FOFsector->floorangle;
		}
		else // it's a ceiling
		{
			xscale = FixedToFloat(FOFsector->ceilingxscale);
			yscale = FixedToFloat(FOFsector->ceilingyscale);
			scrollx = FixedToFloat(FOFsector->ceilingxoffset) / fflatwidth;
			scrolly = FixedToFloat(FOFsector->ceilingyoffset) / fflatheight;
			angle = FOFsector->ceilingangle;
		}
	}
	else if (gl_frontsector)
	{
		if (!isceiling) // it's a floor
		{
			xscale = FixedToFloat(gl_frontsector->floorxscale);
			yscale = FixedToFloat(gl_frontsector->flooryscale);
			scrollx = FixedToFloat(gl_frontsector->floorxoffset) / fflatwidth;
			scrolly = FixedToFloat(gl_frontsector->flooryoffset) / fflatheight;
			angle = gl_frontsector->floorangle;
		}
		else // it's a ceiling
		{
			xscale = FixedToFloat(gl_frontsector->ceilingxscale);
			yscale = FixedToFloat(gl_frontsector->ceilingyscale);
			scrollx = FixedToFloat(gl_frontsector->ceilingxoffset) / fflatwidth;
			scrolly = FixedToFloat(gl_frontsector->ceilingyoffset) / fflatheight;
			angle = gl_frontsector->ceilingangle;
		}
	}

	for (i = 0; i < (INT32)nrPlaneVerts; i++,v3d++)
	{
		// Go from the polysector's original vertex locations
		// Means the flat is offset based on the original vertex locations
		v3d->s = (FixedToFloat(polysector->origVerts[i].x) / fflatwidth) + (scrollx / xscale);
		v3d->t = -(FixedToFloat(polysector->origVerts[i].y) / fflatheight) + (scrolly / yscale);

		// Need to rotate before translate
		if (angle) // Only needs to be done if there's an altered angle
		{
			tempxsow = v3d->s;
			tempytow = v3d->t;

			anglef = ANG2RAD(InvAngle(angle));

			v3d->s = (tempxsow * cos(anglef)) - (tempytow * sin(anglef));
			v3d->t = (tempxsow * sin(anglef)) + (tempytow * cos(anglef));
		}

		v3d->s *= xscale;
		v3d->t *= yscale;

		v3d->x = FIXED_TO_FLOAT(polysector->vertices[i]->x);
		v3d->y = height;
		v3d->z = FIXED_TO_FLOAT(polysector->vertices[i]->y);
	}

	HWR_Lighting(&Surf, lightlevel, planecolormap);

	if (blendmode & PF_Translucent)
	{
		Surf.PolyColor.s.alpha = (UINT8)alpha;
		blendmode |= PF_Modulated|PF_Occlude;
	}
	else
		blendmode |= PF_Masked|PF_Modulated;

	if (HWR_UseShader())
	{
		shader = SHADER_FLOOR;
		blendmode |= PF_ColorMapped;
	}

	HWR_ProcessPolygon(&Surf, planeVerts, nrPlaneVerts, blendmode, shader, false);
}

static void HWR_AddPolyObjectPlanes(void)
{
	size_t i;
	sector_t *polyobjsector;
	INT32 light = 0;

	// Polyobject Planes need their own function for drawing because they don't have extrasubsectors by themselves
	// It should be okay because polyobjects should always be convex anyway

	for (i  = 0; i < numpolys; i++)
	{
		polyobjsector = po_ptrs[i]->lines[0]->backsector; // the in-level polyobject sector

		if (!(po_ptrs[i]->flags & POF_RENDERPLANES)) // Only render planes when you should
			continue;

		if (po_ptrs[i]->translucency >= NUMTRANSMAPS)
			continue;

		if (polyobjsector->floorheight <= gl_frontsector->ceilingheight
			&& polyobjsector->floorheight >= gl_frontsector->floorheight
			&& (viewz < polyobjsector->floorheight))
		{
			light = R_GetPlaneLight(gl_frontsector, polyobjsector->floorheight, true);
			if (po_ptrs[i]->translucency > 0)
			{
				FSurfaceInfo Surf;
				FBITFIELD blendmode;
				memset(&Surf, 0x00, sizeof(Surf));
				blendmode = HWR_TranstableToAlpha(po_ptrs[i]->translucency, &Surf);
				HWR_AddTransparentPolyobjectFloor(&levelflats[polyobjsector->floorpic], po_ptrs[i], false, polyobjsector->floorheight,
													(light == -1 ? gl_frontsector->lightlevel : *gl_frontsector->lightlist[light].lightlevel), Surf.PolyColor.s.alpha, polyobjsector, blendmode, (light == -1 ? gl_frontsector->extra_colormap : *gl_frontsector->lightlist[light].extra_colormap));
			}
			else
			{
				HWR_GetLevelFlat(&levelflats[polyobjsector->floorpic], false);
				HWR_RenderPolyObjectPlane(po_ptrs[i], false, polyobjsector->floorheight, PF_Occlude,
										(light == -1 ? gl_frontsector->lightlevel : *gl_frontsector->lightlist[light].lightlevel), &levelflats[polyobjsector->floorpic],
										polyobjsector, 255, (light == -1 ? gl_frontsector->extra_colormap : *gl_frontsector->lightlist[light].extra_colormap));
			}
		}

		if (polyobjsector->ceilingheight >= gl_frontsector->floorheight
			&& polyobjsector->ceilingheight <= gl_frontsector->ceilingheight
			&& (viewz > polyobjsector->ceilingheight))
		{
			light = R_GetPlaneLight(gl_frontsector, polyobjsector->ceilingheight, true);
			if (po_ptrs[i]->translucency > 0)
			{
				FSurfaceInfo Surf;
				FBITFIELD blendmode;
				memset(&Surf, 0x00, sizeof(Surf));
				blendmode = HWR_TranstableToAlpha(po_ptrs[i]->translucency, &Surf);
				HWR_AddTransparentPolyobjectFloor(&levelflats[polyobjsector->ceilingpic], po_ptrs[i], true, polyobjsector->ceilingheight,
				                                  (light == -1 ? gl_frontsector->lightlevel : *gl_frontsector->lightlist[light].lightlevel), Surf.PolyColor.s.alpha, polyobjsector, blendmode, (light == -1 ? gl_frontsector->extra_colormap : *gl_frontsector->lightlist[light].extra_colormap));
			}
			else
			{
				HWR_GetLevelFlat(&levelflats[polyobjsector->ceilingpic], false);
				HWR_RenderPolyObjectPlane(po_ptrs[i], true, polyobjsector->ceilingheight, PF_Occlude,
				                          (light == -1 ? gl_frontsector->lightlevel : *gl_frontsector->lightlist[light].lightlevel), &levelflats[polyobjsector->ceilingpic],
				                          polyobjsector, 255, (light == -1 ? gl_frontsector->extra_colormap : *gl_frontsector->lightlist[light].extra_colormap));
			}
		}
	}
}

static FBITFIELD HWR_RippleBlend(sector_t *sector, ffloor_t *rover, boolean ceiling)
{
	(void)sector;
	(void)ceiling;
	return /*R_IsRipplePlane(sector, rover, ceiling)*/ (rover->fofflags & FOF_RIPPLE) ? PF_Ripple : 0;
}

//
// HWR_DoCulling
// Hardware version of R_DoCulling
// (see r_main.c)
static boolean HWR_DoCulling(line_t *cullheight, line_t *viewcullheight, float vz, float bottomh, float toph)
{
	float cullplane;

	if (!cullheight)
		return false;

	cullplane = FIXED_TO_FLOAT(cullheight->frontsector->floorheight);
	if (cullheight->args[1]) // Group culling
	{
		if (!viewcullheight)
			return false;

		// Make sure this is part of the same group
		if (viewcullheight->frontsector == cullheight->frontsector)
		{
			// OK, we can cull
			if (vz > cullplane && toph < cullplane) // Cull if below plane
				return true;

			if (bottomh > cullplane && vz <= cullplane) // Cull if above plane
				return true;
		}
	}
	else // Quick culling
	{
		if (vz > cullplane && toph < cullplane) // Cull if below plane
			return true;

		if (bottomh > cullplane && vz <= cullplane) // Cull if above plane
			return true;
	}

	return false;
}

// -----------------+
// HWR_Subsector    : Determine floor/ceiling planes.
//                  : Add sprites of things in sector.
//                  : Draw one or more line segments.
// Notes            : Sets gl_cursectorlight to the light of the parent sector, to modulate wall textures
// -----------------+
static void HWR_Subsector(size_t num)
{
	INT16 count;
	HWC_ADD(HWC_SUBSECS);
	seg_t *line;
	subsector_t *sub;
	static sector_t tempsec; //SoM: 4/7/2000
	INT32 floorlightlevel;
	INT32 ceilinglightlevel;
	INT32 locFloorHeight, locCeilingHeight;
	INT32 cullFloorHeight, cullCeilingHeight;
	INT32 light = 0;
	extracolormap_t *floorcolormap;
	extracolormap_t *ceilingcolormap;
	ffloor_t *rover;

#ifdef PARANOIA //no risk while developing, enough debugging nights!
	if (num >= addsubsector)
		I_Error("HWR_Subsector: ss %s with numss = %s, addss = %s\n",
			sizeu1(num), sizeu2(numsubsectors), sizeu3(addsubsector));

	/*if (num >= numsubsectors)
		I_Error("HWR_Subsector: ss %i with numss = %i",
		        num,
		        numsubsectors);*/
#endif

	if (num < numsubsectors)
	{
		// subsector
		sub = &subsectors[num];
		// sector
		gl_frontsector = sub->sector;
		// how many linedefs
		count = sub->numlines;
		// first line seg
		line = &segs[sub->firstline];
	}
	else
	{
		// there are no segs but only planes
		sub = &subsectors[0];
		gl_frontsector = sub->sector;
		count = 0;
		line = NULL;
	}

	//SoM: 4/7/2000: Test to make Boom water work in Hardware mode.
	gl_frontsector = R_FakeFlat(gl_frontsector, &tempsec, &floorlightlevel, &ceilinglightlevel, false);

	floorcolormap = ceilingcolormap = gl_frontsector->extra_colormap;

	cullFloorHeight   = P_GetSectorFloorZAt  (gl_frontsector, viewx, viewy);
	cullCeilingHeight = P_GetSectorCeilingZAt(gl_frontsector, viewx, viewy);
	locFloorHeight    = P_GetSectorFloorZAt  (gl_frontsector, gl_frontsector->soundorg.x, gl_frontsector->soundorg.y);
	locCeilingHeight  = P_GetSectorCeilingZAt(gl_frontsector, gl_frontsector->soundorg.x, gl_frontsector->soundorg.y);

	R_CheckSectorLightLists(sub->sector, gl_frontsector, &floorlightlevel, &ceilinglightlevel, &floorcolormap, &ceilingcolormap);

	sub->sector->extra_colormap = gl_frontsector->extra_colormap;

	// render floor ?
	// yeah, easy backface cull! :)
	if (cullFloorHeight < viewz)
	{
		if (gl_frontsector->floorpic != skyflatnum)
		{
			if (sub->validcount != validcount)
			{
				HWR_GetLevelFlat(&levelflats[gl_frontsector->floorpic], false);
				HWR_RenderPlane(sub, &extrasubsectors[num], false,
					// Hack to make things continue to work around slopes.
					locFloorHeight == cullFloorHeight ? locFloorHeight : gl_frontsector->floorheight,
					// We now return you to your regularly scheduled rendering.
					PF_Occlude, floorlightlevel, &levelflats[gl_frontsector->floorpic], NULL, 255, floorcolormap);
			}
		}
	}

	if (cullCeilingHeight > viewz)
	{
		if (gl_frontsector->ceilingpic != skyflatnum)
		{
			if (sub->validcount != validcount)
			{
				HWR_GetLevelFlat(&levelflats[gl_frontsector->ceilingpic], false);
				HWR_RenderPlane(sub, &extrasubsectors[num], true,
					// Hack to make things continue to work around slopes.
					locCeilingHeight == cullCeilingHeight ? locCeilingHeight : gl_frontsector->ceilingheight,
					// We now return you to your regularly scheduled rendering.
					PF_Occlude, ceilinglightlevel, &levelflats[gl_frontsector->ceilingpic], NULL, 255, ceilingcolormap);
			}
		}
	}

	// Moved here because before, when above the ceiling and the floor does not have the sky flat, it doesn't draw the sky
	if (gl_frontsector->ceilingpic == skyflatnum || gl_frontsector->floorpic == skyflatnum)
		drawsky = true;

	if (gl_frontsector->ffloors)
	{
		/// \todo fix light, xoffs, yoffs, extracolormap ?
		for (rover = gl_frontsector->ffloors;
			rover; rover = rover->next)
		{
			fixed_t bottomCullHeight, topCullHeight, centerHeight;

			if (!(rover->fofflags & FOF_EXISTS) || !(rover->fofflags & FOF_RENDERPLANES))
				continue;
			if (sub->validcount == validcount)
				continue;

			// rendering heights for bottom and top planes
			bottomCullHeight = P_GetFFloorBottomZAt(rover, viewx, viewy);
			topCullHeight = P_GetFFloorTopZAt(rover, viewx, viewy);

			if (gl_frontsector->cullheight)
			{
				if (HWR_DoCulling(gl_frontsector->cullheight, viewsector->cullheight, gl_viewz, FIXED_TO_FLOAT(*rover->bottomheight), FIXED_TO_FLOAT(*rover->topheight)))
					continue;
			}

			// bottom plane
			centerHeight = P_GetFFloorBottomZAt(rover, gl_frontsector->soundorg.x, gl_frontsector->soundorg.y);

			if (centerHeight <= locCeilingHeight &&
			    centerHeight >= locFloorHeight &&
			    ((viewz < bottomCullHeight && (rover->fofflags & FOF_BOTHPLANES || !(rover->fofflags & FOF_INVERTPLANES))) ||
			     (viewz > bottomCullHeight && (rover->fofflags & FOF_BOTHPLANES || rover->fofflags & FOF_INVERTPLANES))))
			{
				if (rover->fofflags & FOF_FOG)
				{
					UINT8 alpha;

					light = R_GetPlaneLight(gl_frontsector, centerHeight, viewz < bottomCullHeight ? true : false);
					alpha = HWR_FogBlockAlpha(HWR_FloorLightLevel(rover->master->frontsector, *gl_frontsector->lightlist[light].lightlevel), rover->master->frontsector->extra_colormap);

					HWR_AddTransparentFloor(0,
					                       &extrasubsectors[num],
										   false,
					                       *rover->bottomheight,
					                       HWR_FloorLightLevel(rover->master->frontsector, *gl_frontsector->lightlist[light].lightlevel),
					                       alpha, rover->master->frontsector, PF_Fog|PF_NoTexture,
										   true, false, rover->master->frontsector->extra_colormap);
				}
				else if ((rover->fofflags & FOF_TRANSLUCENT && !((rover->fofflags & FOF_SPLAT) && rover->alpha >= 255)) || rover->blend) // SoM: Flags are more efficient
				{
					light = R_GetPlaneLight(gl_frontsector, centerHeight, viewz < bottomCullHeight ? true : false);

					HWR_AddTransparentFloor(&levelflats[*rover->bottompic],
					                       &extrasubsectors[num],
										   false,
					                       *rover->bottomheight,
					                       HWR_FloorLightLevel(rover->master->frontsector, *gl_frontsector->lightlist[light].lightlevel),
					                       max(0, min(rover->alpha, 255)), rover->master->frontsector,
					                       HWR_RippleBlend(gl_frontsector, rover, false) | (rover->blend ? HWR_GetBlendModeFlag(rover->blend) : PF_Translucent),
					                       false, rover->fofflags & FOF_SPLAT, *gl_frontsector->lightlist[light].extra_colormap);
				}
				else
				{
					HWR_GetLevelFlat(&levelflats[*rover->bottompic], rover->fofflags & FOF_SPLAT);
					light = R_GetPlaneLight(gl_frontsector, centerHeight, viewz < bottomCullHeight ? true : false);
					HWR_RenderPlane(sub, &extrasubsectors[num], false, *rover->bottomheight, HWR_RippleBlend(gl_frontsector, rover, false)|PF_Occlude,
					                HWR_FloorLightLevel(rover->master->frontsector, *gl_frontsector->lightlist[light].lightlevel),
					                &levelflats[*rover->bottompic], rover->master->frontsector, 255, *gl_frontsector->lightlist[light].extra_colormap);
				}
			}

			// top plane
			centerHeight = P_GetFFloorTopZAt(rover, gl_frontsector->soundorg.x, gl_frontsector->soundorg.y);

			if (centerHeight >= locFloorHeight &&
			    centerHeight <= locCeilingHeight &&
			    ((viewz > topCullHeight && (rover->fofflags & FOF_BOTHPLANES || !(rover->fofflags & FOF_INVERTPLANES))) ||
			     (viewz < topCullHeight && (rover->fofflags & FOF_BOTHPLANES || rover->fofflags & FOF_INVERTPLANES))))
			{
				if (rover->fofflags & FOF_FOG)
				{
					UINT8 alpha;

					light = R_GetPlaneLight(gl_frontsector, centerHeight, viewz < topCullHeight ? true : false);
					alpha = HWR_FogBlockAlpha(HWR_CeilingLightLevel(rover->master->frontsector, *gl_frontsector->lightlist[light].lightlevel), rover->master->frontsector->extra_colormap);

					HWR_AddTransparentFloor(0,
					                       &extrasubsectors[num],
										   true,
					                       *rover->topheight,
					                       HWR_CeilingLightLevel(rover->master->frontsector, *gl_frontsector->lightlist[light].lightlevel),
					                       alpha, rover->master->frontsector, PF_Fog|PF_NoTexture,
										   true, false, rover->master->frontsector->extra_colormap);
				}
				else if ((rover->fofflags & FOF_TRANSLUCENT && !((rover->fofflags & FOF_SPLAT) && rover->alpha >= 255)) || rover->blend)
				{
					light = R_GetPlaneLight(gl_frontsector, centerHeight, viewz < topCullHeight ? true : false);

					HWR_AddTransparentFloor(&levelflats[*rover->toppic],
					                        &extrasubsectors[num],
											true,
					                        *rover->topheight,
					                        HWR_CeilingLightLevel(rover->master->frontsector, *gl_frontsector->lightlist[light].lightlevel),
					                        max(0, min(rover->alpha, 255)), rover->master->frontsector,
					                        HWR_RippleBlend(gl_frontsector, rover, false) | (rover->blend ? HWR_GetBlendModeFlag(rover->blend) : PF_Translucent),
					                        false, rover->fofflags & FOF_SPLAT, *gl_frontsector->lightlist[light].extra_colormap);
				}
				else
				{
					HWR_GetLevelFlat(&levelflats[*rover->toppic], rover->fofflags & FOF_SPLAT);
					light = R_GetPlaneLight(gl_frontsector, centerHeight, viewz < topCullHeight ? true : false);
					HWR_RenderPlane(sub, &extrasubsectors[num], true, *rover->topheight, HWR_RippleBlend(gl_frontsector, rover, false)|PF_Occlude,
					                  HWR_CeilingLightLevel(rover->master->frontsector, *gl_frontsector->lightlist[light].lightlevel),
					                  &levelflats[*rover->toppic], rover->master->frontsector, 255, *gl_frontsector->lightlist[light].extra_colormap);
				}
			}
		}
	}

	// Draw all the polyobjects in this subsector
	if (sub->polyList)
	{
		polyobj_t *po = sub->polyList;

		numpolys = 0;

		// Count all the polyobjects, reset the list, and recount them
		while (po)
		{
			++numpolys;
			po = (polyobj_t *)(po->link.next);
		}

		// for render stats
		ps_numpolyobjects.value.i += numpolys;

		// Sort polyobjects
		R_SortPolyObjects(sub);

		// Draw polyobject lines.
		HWR_AddPolyObjectSegs();

		if (sub->validcount != validcount) // This validcount situation seems to let us know that the floors have already been drawn.
		{
			// Draw polyobject planes
			HWR_AddPolyObjectPlanes();
		}
	}

	// Hurdler: here interesting things are happening!
	// we have just drawn the floor and ceiling
	// we now draw the sprites first and then the walls
	// hurdler: false: we only add the sprites, the walls are drawn first
	if (line)
	{
		// draw sprites first, coz they are clipped to the solidsegs of
		// subsectors more 'in front'
		{
			HWP_SPAN_BEGIN(tspr);
			HWR_AddSprites(gl_frontsector);
			HWP_SPAN_END(tspr, HWP_ADDSPR);
		}

		//Hurdler: at this point validcount must be the same, but is not because
		//         gl_frontsector doesn't point anymore to sub->sector due to
		//         the call gl_frontsector = R_FakeFlat(...)
		//         if it's not done, the sprite is drawn more than once,
		//         what looks really bad with translucency or dynamic light,
		//         without talking about the overdraw of course.
		sub->sector->validcount = validcount;/// \todo fix that in a better way

		while (count--)
		{

			if (!line->glseg && !line->polyseg) // ignore segs that belong to polyobjects
				HWR_AddLine(line);
			line++;
		}
	}

	sub->validcount = validcount;
}

//
// Renders all subsectors below a given node,
//  traversing subtree recursively.
// Just call with BSP root.

// BP: big hack for a test in lighning ref : 1249753487AB
fixed_t *hwbbox;

static void HWR_RenderBSPNode(INT32 bspnum)
{
	node_t *bsp = &nodes[bspnum];

	// Decide which side the view point is on
	INT32 side;

	ps_numbspcalls.value.i++;

	// Found a subsector?
	if (bspnum & NF_SUBSECTOR)
	{
		if (bspnum == -1)
		{
			//*(gl_drawsubsector_p++) = 0;
			HWP_SPAN_BEGIN(tsub);
			HWR_Subsector(0);
			HWP_SPAN_END(tsub, HWP_SUBSEC);
		}
		else
		{
			//*(gl_drawsubsector_p++) = bspnum&(~NF_SUBSECTOR);
			HWP_SPAN_BEGIN(tsub);
			HWR_Subsector(bspnum&(~NF_SUBSECTOR));
			HWP_SPAN_END(tsub, HWP_SUBSEC);
		}
		return;
	}

	// Decide which side the view point is on.
	side = R_PointOnSide(viewx, viewy, bsp);

	// BP: big hack for a test in lighning ref : 1249753487AB
	hwbbox = bsp->bbox[side];

	// Recursively divide front space.
	HWR_RenderBSPNode(bsp->children[side]);

	// Possibly divide back space.
	if (HWR_CheckBBox(bsp->bbox[side^1]))
	{
		// BP: big hack for a test in lighning ref : 1249753487AB
		hwbbox = bsp->bbox[side^1];
		HWR_RenderBSPNode(bsp->children[side^1]);
	}
}

// ==========================================================================
// gl_things.c
// ==========================================================================

// sprites are drawn after all wall and planes are rendered, so that
// sprite translucency effects apply on the rendered view (instead of the background sky!!)

static UINT32 gl_visspritecount;
static gl_vissprite_t *gl_visspritechunks[MAXVISSPRITES >> VISSPRITECHUNKBITS] = {NULL};

#ifdef PS2_PROFILE
#define HWR_USING_INTERP() (hwr_fx_interp)
#else
#define HWR_USING_INTERP() (R_UsingFrameInterpolation() && !paused)
#endif

#ifdef PS2_PROFILE
// PS2-HW-240: the reaches of the sprites (HWR_FX_ThingHidden)
typedef struct
{
	float hr, vr; // the largest horizontal reach of a picture of the sprite from its position, and vertical (in pixels, scale 1)
	SINT8 ok; // 0: not made, 1: made, -1: the sprite has no pictures
} hwr_fx_ext_t;

static hwr_fx_ext_t hwr_fx_ext[NUMSPRITES];
static size_t hwr_fx_ext_lumps;
static UINT16 hwr_fx_ext_wads;
static boolean hwr_fx_interp; // R_UsingFrameInterpolation() && !paused, once per view (HWR_ClearSprites)
static boolean hwr_fx_lerp; // this frame's things are drawn between two tics (rendertimefrac < 1): their positions are bounded by the old ones
static patch_t *hwr_fx_dshadow; // the picture of the drop shadow of this frame (PS2-HW-242)
static boolean hwr_fx_blok; // R_BlendLevelVisible(AST_TRANSLUCENT, 0) (a thing of full alpha and the default blend mode), made once per view
static boolean hwr_fx_viewok; // the view's object has not been removed (R_ThingVisible asks for every thing)
static mobj_t *hwr_fx_follow; // ... and the object that follows the view's player
static boolean hwr_fx_plain; // PS2-HW-254: HWR_ProjectPlain may be tried (this view draws sprites, not models)
static float hwr_fx_rsin, hwr_fx_rcos; // the right vector of the view (sprites that face the viewer), made once per view
static boolean HWR_ProjectPlain(mobj_t *thing);
static void HWR_FX_ProjectChecked(mobj_t *thing);
static boolean hwr_fx_lm_ok; // PS2-HW-256: the surface of the last light level (HWR_FX_SpriteBuild), made again in every view
static UINT8 hwr_fx_lm_level;
static FSurfaceInfo hwr_fx_lm_surf;
static int hwr_fx_spr_shader; // the shader slot of SHADER_SPRITE (what HWR_ProcessPolygon asks of HWR_ShaderOfTarget)
static ps2spr_t hwr_fx_chk; // check mode (-hwfx 2): what the stream's builder made of the sprite, HWR_DrawSprite compares it with the polygon it makes
static boolean hwr_fx_chk_ok;
static boolean hwr_fx_cheap; // PS2-HW-257 on for this view
static const ps2cull_t *hwr_fx_cs; // the view of the driver for the sphere tests of this sector's things (HWR_AddSprites), NULL: not valid
static UINT32 hwr_fx_view; // counts the views (HWR_ClearSprites): the patch cache below is made again in every view
static struct { lumpnum_t ln; UINT32 view; patch_t *p; } hwr_fx_pc[256]; // the patches of the sprite lumps of this view (W_CachePatchNum is two calls and a tag change: 150 cycles for each of the 300 sprites of a frame)

// PS2-HW-248: R_ThingVisible with what does not change in a view made once per view (it was 70 cycles for each of the 1771 things of a DEMO_004 frame)
static inline boolean HWR_FX_ThingVisible(mobj_t *thing)
{
	if (thing->sprite == SPR_NULL || (thing->flags2 & MF2_DONTDRAW) || (thing->drawonlyforplayer && thing->drawonlyforplayer != viewplayer))
		return false;
	if (thing->alpha == FRACUNIT && thing->blendmode == AST_TRANSLUCENT)
	{
		if (!hwr_fx_blok)
			return false;
	}
	else if (!R_BlendLevelVisible(thing->blendmode, R_GetThingTransTable(thing->alpha, 0)))
		return false;
	if (hwr_fx_viewok && (r_viewmobj == thing || hwr_fx_follow == thing || r_viewmobj == thing->dontdrawforviewmobj))
		return false;
	return true;
}


// the sphere of radius R around (X, Y, Z) (the coordinates of FOutVector: x, height, y) lies wholly outside one side of the view volume, with every point of it at least
// twice the near plane away from the eye: a quad inside it is hidden for PS2HWD_QuadHidden
static inline boolean HWR_FX_SphereHidden(const ps2cull_t *cs, float X, float Y, float Z, float R)
{
	float cx, cy, cw;

	cw = cs->r[3][0] * X + cs->r[3][1] * Y + cs->r[3][2] * Z + cs->r[3][3];
	if (cw - cs->nw * R < cs->w2)
		return false; // a corner could be near the eye: the clipper decides
	cx = cs->r[0][0] * X + cs->r[0][1] * Y + cs->r[0][2] * Z + cs->r[0][3];
	if ((cx - cw) - cs->nxm * R > 0.0f || (cx + cw) + cs->nxp * R < 0.0f)
		return true;
	cy = cs->r[1][0] * X + cs->r[1][1] * Y + cs->r[1][2] * Z + cs->r[1][3];
	return (cy - cw) - cs->nym * R > 0.0f || (cy + cw) + cs->nyp * R < 0.0f;
}
// PS2-HW-251 (FX3): PS2HWD_QuadHidden of a quad whose corners 1 and 3 are the neighbours of the corner 0 and the corner 2 is the fourth of a parallelogram (the drop shadow:
// the square on the floor or on a slope, the shape of the sprite of cv_shadow 2): from the rows of the transform, without the four transforms. -hwfx 16384: as before.
static boolean HWR_FX_QuadHidden(const FOutVector *v)
{
	if (!(ps2hwd_fx2 & FX3_NOPARA))
	{
		const ps2cull_t *cs = PS2HWD_CullSetup();

		if (cs->valid)
		{
			const float p0[3] = {v[0].x, v[0].y, v[0].z};
			const float r[3] = {v[1].x - v[0].x, v[1].y - v[0].y, v[1].z - v[0].z};
			const float u[3] = {v[3].x - v[0].x, v[3].y - v[0].y, v[3].z - v[0].z};
			const boolean h = PS2HWD_ParaHidden(cs, p0, r, u) != 0;

			if (ps2hwd_fx2 & FX2_PRECHECK)
			{
				static unsigned chk, bad;
				const boolean o = PS2HWD_QuadHidden(v) != 0;

				chk++;
				if (o != h)
				{
					bad++;
					if (bad <= 20)
						CONS_Printf("HWC shadow para MISMATCH %u of %u: new %d old %d\n", bad, chk, (int)h, (int)o);
				}
				else if (!(chk & 1023))
				{
					CONS_Printf("HWC shadow para check: %u quads, %u differ\n", chk, bad);
				}
			}
			return h;
		}
	}
	return PS2HWD_QuadHidden(v) != 0;
}
#endif

// --------------------------------------------------------------------------
// HWR_ClearSprites
// Called at frame start.
// --------------------------------------------------------------------------
static void HWR_ClearSprites(void)
{
	gl_visspritecount = 0;
#ifdef PS2_PROFILE
	hwr_fx_dshadow = NULL;
	hwr_fx_blok = R_BlendLevelVisible(AST_TRANSLUCENT, 0);
	hwr_fx_viewok = !P_MobjWasRemoved(r_viewmobj);
	hwr_fx_follow = (hwr_fx_viewok && r_viewmobj->player) ? r_viewmobj->player->followmobj : NULL;
	hwr_fx_interp = R_UsingFrameInterpolation() && !paused; // PS2-HW-245: asked once per view (the answer costs 100 cycles: the refresh rate of the display mode is looked up), not by every thing
	hwr_fx_lerp = hwr_fx_interp && rendertimefrac != FRACUNIT; // (PS2-HW-240)
	hwr_fx_plain = !cv_glmodels.value && r_renderthings && !(ps2hwd_fx2 & FX3_NOPLAIN); // PS2-HW-254
	hwr_fx_lm_ok = false;
	hwr_fx_cheap = !(ps2hwd_fx2 & FX3_NOLEAN2);
	hwr_fx_view++;
	hwr_fx_spr_shader = HWR_GetShaderFromTarget(SHADER_SPRITE);
	hwr_fx_rsin = FIXED_TO_FLOAT(FINESINE((viewangle + ANGLE_90) >> ANGLETOFINESHIFT));
	hwr_fx_rcos = FIXED_TO_FLOAT(FINECOSINE((viewangle + ANGLE_90) >> ANGLETOFINESHIFT));
	if (hwr_fx_ext_lumps != numspritelumps || hwr_fx_ext_wads != numwadfiles)
	{
		memset(hwr_fx_ext, 0, sizeof hwr_fx_ext); // the sprite pictures have changed (an add-on): the reaches are made again
		hwr_fx_ext_lumps = numspritelumps;
		hwr_fx_ext_wads = numwadfiles;
	}
#endif
}

// --------------------------------------------------------------------------
// HWR_NewVisSprite
// --------------------------------------------------------------------------
static gl_vissprite_t gl_overflowsprite;

static gl_vissprite_t *HWR_GetVisSprite(UINT32 num)
{
		UINT32 chunk = num >> VISSPRITECHUNKBITS;

		// Allocate chunk if necessary
		if (!gl_visspritechunks[chunk])
			Z_Malloc(sizeof(gl_vissprite_t) * VISSPRITESPERCHUNK, PU_LEVEL, &gl_visspritechunks[chunk]);

		return gl_visspritechunks[chunk] + (num & VISSPRITEINDEXMASK);
}

static gl_vissprite_t *HWR_NewVisSprite(void)
{
	if (gl_visspritecount == MAXVISSPRITES)
		return &gl_overflowsprite;

	return HWR_GetVisSprite(gl_visspritecount++);
}

// A hack solution for transparent surfaces appearing on top of linkdraw sprites.
// Keep a list of linkdraw sprites and draw their shapes to the z-buffer after all other
// sprite drawing is done. (effectively the z-buffer drawing of linkdraw sprites is delayed)
// NOTE: This will no longer be necessary once full translucent sorting is implemented, where
// translucent sprites and surfaces are sorted together.

typedef struct
{
	FOutVector verts[4];
	gl_vissprite_t *spr;
} zbuffersprite_t;

// this list is used to store data about linkdraw sprites
#ifdef PS2
// PS2-171 (OPT11-STAB): 172 KB of .bss that a software game never uses: the list grows in the zone (64 entries at first) and is given back with the renderer
static zbuffersprite_t *linkdrawlist;
static UINT32 linkdrawcap;
#else
zbuffersprite_t linkdrawlist[MAXVISSPRITES];
#endif
UINT32 linkdrawcount = 0;

// add the necessary data to the list for delayed z-buffer drawing
static void HWR_LinkDrawHackAdd(FOutVector *verts, gl_vissprite_t *spr)
{
#ifdef PS2
	if (linkdrawcount >= linkdrawcap && linkdrawcap < MAXVISSPRITES)
	{
		const UINT32 ncap = linkdrawcap ? (linkdrawcap * 2 < MAXVISSPRITES ? linkdrawcap * 2 : MAXVISSPRITES) : 64;
		zbuffersprite_t *n = linkdrawlist ? Z_TryReallocAlign(linkdrawlist, ncap * sizeof *linkdrawlist, PU_STATIC, NULL, 4)
			: Z_TryMallocAlign(ncap * sizeof *linkdrawlist, PU_STATIC, NULL, 4);

		if (n) // no room: this sprite's depth-only pass is dropped (a link-draw sprite is a tiny part of a frame)
		{
			linkdrawlist = n;
			linkdrawcap = ncap;
		}
	}
	if (linkdrawcount < linkdrawcap)
#else
	if (linkdrawcount < MAXVISSPRITES)
#endif
	{
		memcpy(linkdrawlist[linkdrawcount].verts, verts, sizeof(FOutVector) * 4);
		linkdrawlist[linkdrawcount].spr = spr;
		linkdrawcount++;
	}
}

// process and clear the list of sprites for delayed z-buffer drawing
static void HWR_LinkDrawHackFinish(void)
{
	UINT32 i;
	FSurfaceInfo surf;
	surf.PolyColor.rgba = 0xFFFFFFFF;
	surf.TintColor.rgba = 0xFFFFFFFF;
	surf.FadeColor.rgba = 0xFFFFFFFF;
	surf.LightInfo.light_level = 0;
	surf.LightInfo.fade_start = 0;
	surf.LightInfo.fade_end = 31;
	for (i = 0; i < linkdrawcount; i++)
	{
		// draw sprite shape, only to z-buffer
		HWR_GetPatch(linkdrawlist[i].spr->gpatch);
		HWR_ProcessPolygon(&surf, linkdrawlist[i].verts, 4, PF_Translucent|PF_Occlude|PF_Invisible, 0, false);
	}
	// reset list
	linkdrawcount = 0;
}

static void HWR_DrawDropShadow(mobj_t *thing, gl_vissprite_t *spr, fixed_t scale)
{
	patch_t *gpatch;
	FOutVector shadowVerts[4];
	FSurfaceInfo sSurf;
	float fscale; float fx; float fy; float offset;
	extracolormap_t *colormap = NULL;
	FBITFIELD blendmode = PF_Translucent|PF_Modulated;
	INT32 shader = SHADER_NONE;
	UINT8 i;
	INT32 heightsec, phs;
	SINT8 flip = P_MobjFlip(thing);

	INT32 light;
	fixed_t scalemul;
	UINT16 alpha;
	fixed_t floordiff;
	fixed_t groundz;
	fixed_t slopez;
	pslope_t *groundslope;
	float shadowlift = 0.05f; // how far above the floor the shadow lies (PS2-HW-124)

	HWD_LOCAL; // OPT11: the parts of this function (HWPROF36, --hwdetail)

	// uncapped/interpolation
	interpmobjstate_t interp = {0};

#ifdef PS2_PROFILE
	if (spr && spr->mobj == thing && spr->ps2_iok)
	{
		// PS2-HW-245: the state HWR_ProjectSprite made for this frame (between two tics: the lerp of x, y, z, the BSP descent for the subsector, once more for every use)
		interp.x = spr->ps2_ix;
		interp.y = spr->ps2_iy;
		interp.z = spr->ps2_iz;
		interp.radius = spr->ps2_ir;
		interp.height = spr->ps2_ih;
		interp.subsector = spr->ps2_isub ? spr->ps2_isub : R_PointInSubsector(interp.x, interp.y);
		HWD_LAP(HWP_SH_A);
		groundz = R_GetShadowZInterp(thing, &interp, &groundslope);
	}
	else if (!(ps2hwd_fx2 & FX2_NOSHADOW) && !hwr_fx_lerp)
	{
		// PS2-HW-242: no frame between two tics: the state is the thing's own (R_InterpolateMobjState with FRACUNIT copies these fields), and R_GetShadowZ does not make it again
		interp.x = thing->x;
		interp.y = thing->y;
		interp.z = thing->z;
		interp.radius = thing->radius;
		interp.height = thing->height;
		interp.subsector = thing->subsector;
		HWD_LAP(HWP_SH_A);
		groundz = R_GetShadowZInterp(thing, &interp, &groundslope);
	}
	else
#endif
	{
	if (HWR_USING_INTERP())
	{
		R_InterpolateMobjState(thing, rendertimefrac, &interp);
	}
	else
	{
		R_InterpolateMobjState(thing, FRACUNIT, &interp);
	}

	HWD_LAP(HWP_SH_A);
	groundz = R_GetShadowZ(thing, &groundslope);
	}
	HWD_LAP(HWP_SH_B);

	heightsec = thing->subsector->sector->heightsec;
	if (viewplayer->mo && viewplayer->mo->subsector)
		phs = viewplayer->mo->subsector->sector->heightsec;
	else
		phs = -1;

	if (heightsec != -1 && phs != -1) // only clip things which are in special sectors
	{
		if (gl_viewz < FIXED_TO_FLOAT(sectors[phs].floorheight) ?
		thing->z >= sectors[heightsec].floorheight :
		thing->z < sectors[heightsec].floorheight)
			return;
		if (gl_viewz > FIXED_TO_FLOAT(sectors[phs].ceilingheight) ?
		thing->z < sectors[heightsec].ceilingheight && gl_viewz >= FIXED_TO_FLOAT(sectors[heightsec].ceilingheight) :
		thing->z >= sectors[heightsec].ceilingheight)
			return;
	}

	floordiff = abs((flip < 0 ? interp.height : 0) + interp.z - groundz);

	alpha = floordiff / (4*FRACUNIT) + 75;
	if (alpha >= 255) return;
	alpha = 255 - alpha;

#ifdef PS2_PROFILE
	if (cv_shadow.value != 2)
	{
		static lumpnum_t dshadow_lump = LUMPERROR; // the lump of the shadow picture is looked up by name once per set of loaded files (a lookup per shadow was 1000+ cycles)
		static UINT16 dshadow_wads;

		if (hwr_fx_dshadow && !(ps2hwd_fx2 & FX2_NOSHADOW))
		{
			gpatch = hwr_fx_dshadow; // PS2-HW-242: the picture of the first shadow of the frame (a PU_SPRITE block stays where it is until the level ends)
		}
		else
		{
			if (dshadow_lump == LUMPERROR || dshadow_wads != numwadfiles)
			{
				dshadow_lump = W_CheckNumForPatchName("DSHADOW");
				dshadow_wads = numwadfiles;
			}
			if (dshadow_lump == LUMPERROR)
				gpatch = (patch_t *)W_CachePatchName("DSHADOW", PU_SPRITE); // as before: the "missing" picture
			else
				gpatch = (patch_t *)W_CachePatchNum(dshadow_lump, PU_SPRITE);
			hwr_fx_dshadow = gpatch;
		}
	}
	else
		gpatch = spr->gpatch;
#else
	gpatch = (cv_shadow.value == 2) ? spr->gpatch : (patch_t *)W_CachePatchName("DSHADOW", PU_SPRITE);
#endif
	if (!(gpatch && gpatch->hardware && ((GLPatch_t *)gpatch->hardware)->mipmap->format)) return;
	HWD_LAP(HWP_SH_C);
#ifdef PS2_PROFILE
	if (ps2hwd_fx2 & FX2_NOSHADOW) // PS2-HW-242: the picture is made resident only for a shadow that is drawn (after the quad test below; 150 cycles for the 70 % that are not)
#endif
	HWR_GetPatch(gpatch);
	HWD_LAP(HWP_SH_D);

	scalemul = FixedMul(FRACUNIT - floordiff/640, scale);
	scalemul = FixedMul(scalemul, (interp.radius*2) / gpatch->height);

	fscale = FIXED_TO_FLOAT(scalemul);
	fx = FIXED_TO_FLOAT(interp.x);
	fy = FIXED_TO_FLOAT(interp.y);

	//  3--2
	//  | /|
	//  |/ |
	//  0--1

	if (thing && fabsf(fscale - 1.0f) > 1.0E-36f)
		offset = ((gpatch->height)/2) * fscale;
	else
		offset = (float)((gpatch->height)/2);

#ifdef PS2_PROFILE
	// PS2-HW-124: the shadow lies 0.05 units above the floor, which is not enough for the GS: the depth of a floor polygon at a pixel is interpolated from vertices
	// snapped to 1/16 pixel, a floor seen from a low camera changes by ~400000 / h depth steps per pixel (h = camera height above it), so the snap alone moves
	// the floor's depth by hundreds of steps, while 0.05 units are 67e6 * 0.05 / d^2 steps (d = distance): the floor won the depth test and the shadow of the
	// player and the enemies was gone on 1/3 of the maps. The lift grows with d^2 / h (four times the snap error), at most 6 units (a pixel or two on the screen).
	// (PS2-HW-242: made before the corners, the sphere test below needs it)
	{
		const float sdx = fx - gl_viewx, sdy = fy - gl_viewy, sdz = gl_viewz - FIXED_TO_FLOAT(groundz);
		float sh = fabsf(sdz), lift;

		sh = sh < 16.0f ? 16.0f : sh;
		lift = (sdx * sdx + sdy * sdy + sdz * sdz) * (1.0f / 640.0f) / sh;
		shadowlift = lift < 0.05f ? 0.05f : lift > 6.0f ? 6.0f : lift;
	}
	if (!(ps2hwd_fx2 & (FX2_NOSHADOW | FX2_PRECHECK)) && !groundslope && cv_shadow.value != 2 && !HWR_PS2_NoCull())
	{
		// PS2-HW-242: a shadow that no pixel of the view can show is not made at all: the quad is a square of half side `offset` on a level floor; the sphere around its
		// centre that holds its corners (and the lift) is wholly outside one side of the view
		const ps2cull_t *cs = PS2HWD_CullSetup();

		if (cs->valid && HWR_FX_SphereHidden(cs, fx, FIXED_TO_FLOAT(groundz) + flip * shadowlift, fy, offset * 1.4143f + shadowlift + 0.5f))
		{
			HWD_ADD(HWC_FX_SHQHID);
			return;
		}
	}
#endif

	shadowVerts[2].x = shadowVerts[3].x = fx + offset;
	shadowVerts[1].x = shadowVerts[0].x = fx - offset;
	shadowVerts[1].z = shadowVerts[2].z = fy - offset;
	shadowVerts[0].z = shadowVerts[3].z = fy + offset;

#ifdef PS2_PROFILE // PS2-HW: no Lua, so the shadow always follows the camera (the default of Banpyura_SpriteShadow_SnapToCamera)
	angle_t shadowangle = 0;
#else
	angle_t shadowangle = Banpyura_SpriteShadow_SnapToCamera ? 0 : (Banpyura_SpriteShadow_Angle - viewangle);
#endif
	fixed_t shadowsin = FINESINE(shadowangle>>ANGLETOFINESHIFT);
	fixed_t shadowcos = FINECOSINE(shadowangle>>ANGLETOFINESHIFT);
	float gl_shadowsin = FixedToFloat(shadowsin);
	float gl_shadowcos = FixedToFloat(shadowcos);

	if (cv_shadow.value == 2)
	{
		shadowVerts[0].x = shadowVerts[3].x = spr->x1;
		shadowVerts[2].x = shadowVerts[1].x = spr->x2;
		shadowVerts[0].z = shadowVerts[3].z = spr->z1;
		shadowVerts[2].z = shadowVerts[1].z = spr->z2;

		if (thing && fabsf(fscale - 1.0f) > 1.0E-36f)
		{
			// Always a pixel above the floor, perfectly flat.
			for (i = 0; i < 4; i++)
			{
				if (groundslope)
					slopez = P_GetSlopeZAt(groundslope, FLOAT_TO_FIXED(shadowVerts[i].x), FLOAT_TO_FIXED(shadowVerts[i].z));
				shadowVerts[i].y = (groundslope ? FIXED_TO_FLOAT(slopez) : FIXED_TO_FLOAT(groundz))/2 + flip * 0.05f;
			}

			// Now transform the TOP vertices along the floor in the direction of the camera

			shadowVerts[3].x = spr->x1 + (gpatch->height + fscale + offset) * gl_viewcos;
			shadowVerts[2].x = spr->x2 + (gpatch->height + fscale + offset) * gl_viewcos;
			shadowVerts[3].z = spr->z1 + (gpatch->height + fscale + offset) * gl_viewsin;
			shadowVerts[2].z = spr->z2 + (gpatch->height + fscale + offset) * gl_viewsin;
		}
		else
		{
			// Always a pixel above the floor, perfectly flat.
			for (i = 0; i < 4; i++)
			{
				if (groundslope)
					slopez = P_GetSlopeZAt(groundslope, FLOAT_TO_FIXED(shadowVerts[i].x), FLOAT_TO_FIXED(shadowVerts[i].z));
				shadowVerts[i].y = (groundslope ? FIXED_TO_FLOAT(slopez) : FIXED_TO_FLOAT(groundz))/2 + flip * 0.05f;
			}

			// Now transform the TOP vertices along the floor in the direction of the camera
			shadowVerts[3].x = spr->x1 + (gpatch->height + offset) * gl_shadowcos;
			shadowVerts[2].x = spr->x2 + (gpatch->height + offset) * gl_shadowcos;
			shadowVerts[3].z = spr->z1 + (gpatch->height + offset) * gl_shadowsin;
			shadowVerts[2].z = spr->z2 + (gpatch->height + offset) * gl_shadowsin;
		}
	}

#ifdef PS2_PROFILE
	if (!(ps2hwd_fx2 & FX2_NOSHADOW)) // PS2-HW-242: the angle is 0 (sin 0, cos 1): the rotation is the identity, written the way the products come out
	{
		for (i = 0; i < 4; i++)
		{
			shadowVerts[i].x = fx + (shadowVerts[i].x - fx);
			shadowVerts[i].z = fy + (shadowVerts[i].z - fy);
		}
	}
	else
#endif
	for (i = 0; i < 4; i++)
	{
		float oldx = shadowVerts[i].x;
		float oldy = shadowVerts[i].z;
		shadowVerts[i].x = fx + ((oldx - fx) * gl_shadowcos) - ((oldy - fy) * gl_shadowsin);
		shadowVerts[i].z = fy + ((oldx - fx) * gl_shadowsin) + ((oldy - fy) * gl_shadowcos);
	}

	if (groundslope)
	{
		for (i = 0; i < 4; i++)
		{
			slopez = P_GetSlopeZAt(groundslope, FLOAT_TO_FIXED(shadowVerts[i].x), FLOAT_TO_FIXED(shadowVerts[i].z));
			shadowVerts[i].y = FIXED_TO_FLOAT(slopez) + flip * shadowlift;
		}
	}
	else
	{
		for (i = 0; i < 4; i++)
			shadowVerts[i].y = FIXED_TO_FLOAT(groundz) + flip * shadowlift;
	}

#ifdef PS2_PROFILE
	// PS2-HW-72: a shadow that cannot put a pixel on the screen is not made (its polygon would be set up, copied into a batch and drawn for nothing)
	shadowVerts[0].s = shadowVerts[1].s = shadowVerts[2].s = shadowVerts[3].s = shadowVerts[0].t = shadowVerts[1].t = shadowVerts[2].t = shadowVerts[3].t = 0.0f;
	HWD_LAP(HWP_SH_E);
	if (!HWR_PS2_NoCull())
	{
		boolean sph = false;
		const ps2cull_t *cs = (ps2hwd_fx2 & FX2_PRECHECK) ? PS2HWD_CullSetup() : NULL;

		if (cs && !cs->valid)
			cs = NULL;
		if (cs && (ps2hwd_fx2 & FX2_PRECHECK) && !(ps2hwd_fx2 & FX2_NOSHADOW) && !groundslope && cv_shadow.value != 2)
			sph = HWR_FX_SphereHidden(cs, fx, shadowVerts[0].y, fy, offset * 1.4143f + shadowlift + 0.5f); // check mode: the exact test below must agree
		// PS2-HW-257: a shadow that goes to the sprite stream (the common case) is not tested exactly: the VU1 program throws out what no pixel centre lies in
		if (!(hwr_fx_cheap && cv_shadow.value != 2 && currently_batching && hwr_sprite_batch && !(ps2hwd_fx2 & (FX3_NOSPR | FX2_PRECHECK))) && HWR_FX_QuadHidden(shadowVerts))
		{
			HWD_ADD(HWC_FX_SHQHID);
			return;
		}
		if (sph) // check mode: the sphere said hidden, the quad is not
		{
			static unsigned bad;

			CONS_Printf("HWC shadow sphere MISMATCH %u\n", ++bad);
		}
	}
	HWD_LAP(HWP_SH_F);
#endif

	HWD_ADD(HWC_FX_SHADOW);
#ifdef PS2_PROFILE
	if (!(ps2hwd_fx2 & FX2_NOSHADOW))
		HWR_GetPatch(gpatch);
#endif
	shadowVerts[0].s = shadowVerts[3].s = 0;
	shadowVerts[2].s = shadowVerts[1].s = ((GLPatch_t *)gpatch->hardware)->max_s;

	shadowVerts[3].t = shadowVerts[2].t = 0;
	shadowVerts[0].t = shadowVerts[1].t = ((GLPatch_t *)gpatch->hardware)->max_t;


	if (cv_shadow.value == 2)
	{
		if (spr->flip)
		{
			shadowVerts[0].s = shadowVerts[3].s = ((GLPatch_t *)gpatch->hardware)->max_s;
			shadowVerts[2].s = shadowVerts[1].s = 0;
		}
		else
		{
			shadowVerts[0].s = shadowVerts[3].s = 0;
			shadowVerts[2].s = shadowVerts[1].s = ((GLPatch_t *)gpatch->hardware)->max_s;
		}
	}

	if (!(thing->renderflags & RF_NOCOLORMAPS))
	{
		if (thing->subsector->sector->numlights)
		{
			// Always use the light at the top instead of whatever I was doing before
			light = R_GetPlaneLight(thing->subsector->sector, groundz, false);

			if (*thing->subsector->sector->lightlist[light].extra_colormap)
				colormap = *thing->subsector->sector->lightlist[light].extra_colormap;
		}
		else if (thing->subsector->sector->extra_colormap)
			colormap = thing->subsector->sector->extra_colormap;
	}

	HWR_Lighting(&sSurf, 0, colormap);
	sSurf.PolyColor.s.alpha = FixedMul(thing->alpha, alpha);

	if (HWR_UseShader())
	{
		shader = SHADER_SPRITE;
		blendmode |= PF_ColorMapped;
		sSurf.LightInfo.light_level = 0;
	}

	if (cv_shadow.value == 2)
	{
		sSurf.PolyColor.s.red = 0x00;
		sSurf.PolyColor.s.blue = 0x00;
		sSurf.PolyColor.s.green = 0x00;
	}

	HWD_LAP(HWP_SH_G);
	HWR_ProcessPolygon(&sSurf, shadowVerts, 4, blendmode, shader, false);
	HWD_LAP(HWP_SH_H);
}

// This is expecting a pointer to an array containing 4 wallVerts for a sprite
static void HWR_RotateSpritePolyToAim(gl_vissprite_t *spr, FOutVector *wallVerts, const boolean precip)
{
	if (cv_glspritebillboarding.value
		&& spr && spr->mobj && !R_ThingIsPaperSprite(spr->mobj)
		&& wallVerts)
	{
		// uncapped/interpolation
		interpmobjstate_t interp = {0};
		float basey, lowy;

		// do interpolation
#ifdef PS2_PROFILE
		if (!precip && spr->ps2_iok)
		{
			interp.z = spr->ps2_iz; // PS2-HW-245: the state of HWR_ProjectSprite (only z and the height are used below)
			interp.height = spr->ps2_ih;
		}
		else
#endif
		if (HWR_USING_INTERP())
		{
			if (precip)
			{
				R_InterpolatePrecipMobjState((precipmobj_t *)spr->mobj, rendertimefrac, &interp);
			}
			else
			{
				R_InterpolateMobjState(spr->mobj, rendertimefrac, &interp);
			}
		}
		else
		{
			if (precip)
			{
				R_InterpolatePrecipMobjState((precipmobj_t *)spr->mobj, FRACUNIT, &interp);
			}
			else
			{
				R_InterpolateMobjState(spr->mobj, FRACUNIT, &interp);
			}
		}

		if (P_MobjFlip(spr->mobj) == -1)
		{
			basey = FIXED_TO_FLOAT(interp.z + interp.height);
		}
		else
		{
			basey = FIXED_TO_FLOAT(interp.z);
		}
		lowy = wallVerts[0].y;

		// Rotate sprites to fully billboard with the camera
		// X, Y, AND Z need to be manipulated for the polys to rotate around the
		// origin, because of how the origin setting works I believe that should
		// be mobj->z or mobj->z + mobj->height
		wallVerts[2].y = wallVerts[3].y = (spr->gzt - basey) * gl_viewludsin + basey;
		wallVerts[0].y = wallVerts[1].y = (lowy - basey) * gl_viewludsin + basey;
		// translate back to be around 0 before translating back
		wallVerts[3].x += ((spr->gzt - basey) * gl_viewludcos) * gl_viewcos;
		wallVerts[2].x += ((spr->gzt - basey) * gl_viewludcos) * gl_viewcos;

		wallVerts[0].x += ((lowy - basey) * gl_viewludcos) * gl_viewcos;
		wallVerts[1].x += ((lowy - basey) * gl_viewludcos) * gl_viewcos;

		wallVerts[3].z += ((spr->gzt - basey) * gl_viewludcos) * gl_viewsin;
		wallVerts[2].z += ((spr->gzt - basey) * gl_viewludcos) * gl_viewsin;

		wallVerts[0].z += ((lowy - basey) * gl_viewludcos) * gl_viewsin;
		wallVerts[1].z += ((lowy - basey) * gl_viewludcos) * gl_viewsin;
	}
}

static void HWR_SplitSprite(gl_vissprite_t *spr)
{
	FOutVector wallVerts[4];
	FOutVector baseWallVerts[4]; // This is what the verts should end up as
	patch_t *gpatch;
	FSurfaceInfo Surf;
	extracolormap_t *colormap = NULL;
	FUINT lightlevel;
	boolean lightset = true;
	FBITFIELD blend = 0;
	FBITFIELD occlusion;
	INT32 shader = SHADER_NONE;
	boolean use_linkdraw_hack = false;
	UINT8 alpha;

	INT32 i;
	float realtop, realbot, top, bot;
	float ttop, tbot, tmult;
	float bheight;
	float realheight, heightmult;
	const sector_t *sector = spr->mobj->subsector->sector;
	const lightlist_t *list = sector->lightlist;
	float endrealtop, endrealbot, endtop, endbot;
	float endbheight;
	float endrealheight;
	fixed_t temp;
	fixed_t v1x, v1y, v2x, v2y;

	gpatch = spr->gpatch;

	// cache the patch in the graphics card memory
	//12/12/99: Hurdler: same comment as above (for md2)
	//Hurdler: 25/04/2000: now support colormap in hardware mode
	HWR_GetMappedPatch(gpatch, spr->colormap);

	baseWallVerts[0].x = baseWallVerts[3].x = spr->x1;
	baseWallVerts[2].x = baseWallVerts[1].x = spr->x2;
	baseWallVerts[0].z = baseWallVerts[3].z = spr->z1;
	baseWallVerts[1].z = baseWallVerts[2].z = spr->z2;

	baseWallVerts[2].y = baseWallVerts[3].y = spr->gzt;
	baseWallVerts[0].y = baseWallVerts[1].y = spr->gz;

	v1x = FLOAT_TO_FIXED(spr->x1);
	v1y = FLOAT_TO_FIXED(spr->z1);
	v2x = FLOAT_TO_FIXED(spr->x2);
	v2y = FLOAT_TO_FIXED(spr->z2);

	if (spr->flip)
	{
		baseWallVerts[0].s = baseWallVerts[3].s = ((GLPatch_t *)gpatch->hardware)->max_s;
		baseWallVerts[2].s = baseWallVerts[1].s = 0;
	}
	else
	{
		baseWallVerts[0].s = baseWallVerts[3].s = 0;
		baseWallVerts[2].s = baseWallVerts[1].s = ((GLPatch_t *)gpatch->hardware)->max_s;
	}

	// flip the texture coords (look familiar?)
	if (spr->vflip)
	{
		baseWallVerts[3].t = baseWallVerts[2].t = ((GLPatch_t *)gpatch->hardware)->max_t;
		baseWallVerts[0].t = baseWallVerts[1].t = 0;
	}
	else
	{
		baseWallVerts[3].t = baseWallVerts[2].t = 0;
		baseWallVerts[0].t = baseWallVerts[1].t = ((GLPatch_t *)gpatch->hardware)->max_t;
	}

	// if it has a dispoffset, push it a little towards the camera
	if (spr->dispoffset) {
		float co = -gl_viewcos*(0.05f*spr->dispoffset);
		float si = -gl_viewsin*(0.05f*spr->dispoffset);
		baseWallVerts[0].z = baseWallVerts[3].z = baseWallVerts[0].z+si;
		baseWallVerts[1].z = baseWallVerts[2].z = baseWallVerts[1].z+si;
		baseWallVerts[0].x = baseWallVerts[3].x = baseWallVerts[0].x+co;
		baseWallVerts[1].x = baseWallVerts[2].x = baseWallVerts[1].x+co;
	}

	// Let dispoffset work first since this adjust each vertex
	HWR_RotateSpritePolyToAim(spr, baseWallVerts, false);

	realtop = top = baseWallVerts[3].y;
	realbot = bot = baseWallVerts[0].y;
	ttop = baseWallVerts[3].t;
	tbot = baseWallVerts[0].t;
	tmult = (tbot - ttop) / (top - bot);

	endrealtop = endtop = baseWallVerts[2].y;
	endrealbot = endbot = baseWallVerts[1].y;

	// copy the contents of baseWallVerts into the drawn wallVerts array
	// baseWallVerts is used to know the final shape to easily get the vertex
	// co-ordinates
	memcpy(wallVerts, baseWallVerts, sizeof(baseWallVerts));

	fixed_t newalpha = spr->mobj->alpha;

	// if sprite has linkdraw, then dont write to z-buffer (by not using PF_Occlude)
	// this will result in sprites drawn afterwards to be drawn on top like intended when using linkdraw.
	if ((spr->mobj->flags2 & MF2_LINKDRAW) && spr->mobj->tracer)
	{
		newalpha = spr->mobj->tracer->alpha;
		occlusion = 0;
	}
	else
		occlusion = PF_Occlude;

	INT32 blendmode;
	if (spr->mobj->frame & FF_BLENDMASK)
		blendmode = ((spr->mobj->frame & FF_BLENDMASK) >> FF_BLENDSHIFT) + 1;
	else
		blendmode = spr->mobj->blendmode;

	if (!cv_translucency.value) // translucency disabled
	{
		Surf.PolyColor.s.alpha = 0xFF;
		blend = PF_Translucent|occlusion;
		if (!occlusion) use_linkdraw_hack = true;
	}
	else if (spr->mobj->flags2 & MF2_SHADOW)
	{
		Surf.PolyColor.s.alpha = 0x40;
		blend = HWR_GetBlendModeFlag(blendmode);
	}
	else if (spr->mobj->frame & FF_TRANSMASK)
	{
		INT32 trans = (spr->mobj->frame & FF_TRANSMASK)>>FF_TRANSSHIFT;
		blend = HWR_SurfaceBlend(blendmode, trans, &Surf);
	}
	else
	{
		// BP: i agree that is little better in environement but it don't
		//     work properly under glide nor with fogcolor to ffffff :(
		// Hurdler: PF_Environement would be cool, but we need to fix
		//          the issue with the fog before
		Surf.PolyColor.s.alpha = 0xFF;
		blend = HWR_GetBlendModeFlag(blendmode)|occlusion;
		if (!occlusion) use_linkdraw_hack = true;
	}

	if (cv_translucency.value && newalpha < FRACUNIT)
	{
		// TODO: The ternary operator is a hack to make alpha values roughly match what their FF_TRANSMASK equivalent would be
		// See if there's a better way of doing this
		Surf.PolyColor.s.alpha = min(FixedMul(newalpha, Surf.PolyColor.s.alpha == 0xFF ? 256 : Surf.PolyColor.s.alpha), 0xFF);
		blend = HWR_GetBlendModeFlag(blendmode);
	}

	if (HWR_UseShader())
	{
		shader = SHADER_SPRITE;
		blend |= PF_ColorMapped;
	}

	alpha = Surf.PolyColor.s.alpha;

	// Start with the lightlevel and colormap from the top of the sprite
	lightlevel = *list[sector->numlights - 1].lightlevel;
	if (!(spr->mobj->renderflags & RF_NOCOLORMAPS))
		colormap = *list[sector->numlights - 1].extra_colormap;

	i = 0;
	temp = FLOAT_TO_FIXED(realtop);

	if (R_ThingIsFullBright(spr->mobj))
		lightlevel = 255;
	else if (R_ThingIsFullDark(spr->mobj))
		lightlevel = 0;
	else
		lightset = false;

	for (i = 1; i < sector->numlights; i++)
	{
		fixed_t h = P_GetLightZAt(&sector->lightlist[i], spr->mobj->x, spr->mobj->y);
		if (h <= temp)
		{
			if (!lightset)
				lightlevel = *list[i-1].lightlevel > 255 ? 255 : *list[i-1].lightlevel;
			if (!(spr->mobj->renderflags & RF_NOCOLORMAPS))
				colormap = *list[i-1].extra_colormap;
			break;
		}
	}

	if (R_ThingIsSemiBright(spr->mobj))
		lightlevel = 128 + (lightlevel>>1);

	for (i = 0; i < sector->numlights; i++)
	{
		if (endtop < endrealbot && top < realbot)
			return;

		// even if we aren't changing colormap or lightlevel, we still need to continue drawing down the sprite
		if (!(list[i].flags & FOF_NOSHADE) && (list[i].flags & FOF_CUTSPRITES))
		{
			if (!lightset)
				lightlevel = *list[i].lightlevel > 255 ? 255 : *list[i].lightlevel;
			if (!(spr->mobj->renderflags & RF_NOCOLORMAPS))
				colormap = *list[i].extra_colormap;
		}

		if (i + 1 < sector->numlights)
		{
			temp = P_GetLightZAt(&list[i+1], v1x, v1y);
			bheight = FIXED_TO_FLOAT(temp);
			temp = P_GetLightZAt(&list[i+1], v2x, v2y);
			endbheight = FIXED_TO_FLOAT(temp);
		}
		else
		{
			bheight = realbot;
			endbheight = endrealbot;
		}

		if (endbheight >= endtop && bheight >= top)
			continue;

		bot = bheight;

		if (bot < realbot)
			bot = realbot;

		endbot = endbheight;

		if (endbot < endrealbot)
			endbot = endrealbot;

		wallVerts[3].t = ttop + ((realtop - top) * tmult);
		wallVerts[2].t = ttop + ((endrealtop - endtop) * tmult);
		wallVerts[0].t = ttop + ((realtop - bot) * tmult);
		wallVerts[1].t = ttop + ((endrealtop - endbot) * tmult);

		wallVerts[3].y = top;
		wallVerts[2].y = endtop;
		wallVerts[0].y = bot;
		wallVerts[1].y = endbot;

		// The x and y only need to be adjusted in the case that it's not a papersprite
		if (cv_glspritebillboarding.value
			&& spr->mobj && !R_ThingIsPaperSprite(spr->mobj))
		{
			// Get the x and z of the vertices so billboarding draws correctly
			realheight = realbot - realtop;
			endrealheight = endrealbot - endrealtop;
			heightmult = (realtop - top) / realheight;
			wallVerts[3].x = baseWallVerts[3].x + (baseWallVerts[3].x - baseWallVerts[0].x) * heightmult;
			wallVerts[3].z = baseWallVerts[3].z + (baseWallVerts[3].z - baseWallVerts[0].z) * heightmult;

			heightmult = (endrealtop - endtop) / endrealheight;
			wallVerts[2].x = baseWallVerts[2].x + (baseWallVerts[2].x - baseWallVerts[1].x) * heightmult;
			wallVerts[2].z = baseWallVerts[2].z + (baseWallVerts[2].z - baseWallVerts[1].z) * heightmult;

			heightmult = (realtop - bot) / realheight;
			wallVerts[0].x = baseWallVerts[3].x + (baseWallVerts[3].x - baseWallVerts[0].x) * heightmult;
			wallVerts[0].z = baseWallVerts[3].z + (baseWallVerts[3].z - baseWallVerts[0].z) * heightmult;

			heightmult = (endrealtop - endbot) / endrealheight;
			wallVerts[1].x = baseWallVerts[2].x + (baseWallVerts[2].x - baseWallVerts[1].x) * heightmult;
			wallVerts[1].z = baseWallVerts[2].z + (baseWallVerts[2].z - baseWallVerts[1].z) * heightmult;
		}

		HWR_Lighting(&Surf, lightlevel, colormap);

		Surf.PolyColor.s.alpha = alpha;

		HWR_ProcessPolygon(&Surf, wallVerts, 4, blend|PF_Modulated, shader, false);

		if (use_linkdraw_hack)
			HWR_LinkDrawHackAdd(wallVerts, spr);

		top = bot;
		endtop = endbot;
	}

	bot = realbot;
	endbot = endrealbot;
	if (endtop <= endrealbot && top <= realbot)
		return;

	// If we're ever down here, somehow the above loop hasn't draw all the light levels of sprite
	wallVerts[3].t = ttop + ((realtop - top) * tmult);
	wallVerts[2].t = ttop + ((endrealtop - endtop) * tmult);
	wallVerts[0].t = ttop + ((realtop - bot) * tmult);
	wallVerts[1].t = ttop + ((endrealtop - endbot) * tmult);

	wallVerts[3].y = top;
	wallVerts[2].y = endtop;
	wallVerts[0].y = bot;
	wallVerts[1].y = endbot;

	HWR_Lighting(&Surf, lightlevel, colormap);

	Surf.PolyColor.s.alpha = alpha;

	HWR_ProcessPolygon(&Surf, wallVerts, 4, blend|PF_Modulated, shader, false);

	if (use_linkdraw_hack)
		HWR_LinkDrawHackAdd(wallVerts, spr);
}

static void HWR_DrawBoundingBox(gl_vissprite_t *vis)
{
	FOutVector v[24];
	FSurfaceInfo Surf = {0};
	RGBA_t *palette = HWR_GetTexturePalette();

	//
	// create a cube (side view)
	//
	//  5--4  3
	//        |
	//        |
	//  0--1  2
	//
	// repeat this 4 times (overhead)
	//
	//
	// 15    16  17    09
	//    14 13  12 08
	// 23 18  *--*  07 10
	//        |  |
	// 22 19  *--*  06 11
	//    20 00  01 02
	// 21    05  04    03
	//

	v[ 0].x = v[ 5].x = v[13].x = v[14].x = v[15].x = v[16].x =
		v[18].x = v[19].x = v[20].x = v[21].x = v[22].x = v[23].x = vis->x1; // west

	v[ 1].x = v[ 2].x = v[ 3].x = v[ 4].x = v[ 6].x = v[ 7].x =
		v[ 8].x = v[ 9].x = v[10].x = v[11].x = v[12].x = v[17].x = vis->x2; // east

	v[ 0].z = v[ 1].z = v[ 2].z = v[ 3].z = v[ 4].z = v[ 5].z =
		v[ 6].z = v[11].z = v[19].z = v[20].z = v[21].z = v[22].z = vis->z1; // south

	v[ 7].z = v[ 8].z = v[ 9].z = v[10].z = v[12].z = v[13].z =
		v[14].z = v[15].z = v[16].z = v[17].z = v[18].z = v[23].z = vis->z2; // north

	v[ 0].y = v[ 1].y = v[ 2].y = v[ 6].y = v[ 7].y = v[ 8].y =
		v[12].y = v[13].y = v[14].y = v[18].y = v[19].y = v[20].y = vis->gz; // bottom

	v[ 3].y = v[ 4].y = v[ 5].y = v[ 9].y = v[10].y = v[11].y =
		v[15].y = v[16].y = v[17].y = v[21].y = v[22].y = v[23].y = vis->gzt; // top

	Surf.PolyColor = palette[R_GetBoundingBoxColor(vis->mobj)];

	HWR_ProcessPolygon(&Surf, v, 24, (cv_renderhitboxgldepth.value ? 0 : PF_NoDepthTest)|PF_Modulated|PF_NoTexture|PF_WireFrame, SHADER_NONE, false);
}

// -----------------+
// HWR_DrawSprite   : Draw flat sprites
//                  : (monsters, bonuses, weapons, lights, ...)
// Returns          :
// -----------------+
#ifdef PS2_PROFILE
// --------------------------------------------------------------------------
// PS2-HW-256 (OPT11 round 3, FX3): the plain sprite of the sprite batch straight to the sprite stream of the driver (ps2_hw_spr.inc).
// HWR_DrawSprite builds four vertices, a surface and the flags for HWR_ProcessPolygon, which copies them into the batch, where the driver unpacks them again into the record it
// draws from; for the common sprite the same numbers (the same operations in the same order: the record is what PS2HWD_SprFromVerts makes of the polygon) go into the record
// at once. A sprite that is not the common one (a floor or paper sprite, a link draw, a sector with 3D floor lights or a colormap, a translated or not yet resident picture, a
// blend that is not opaque) returns false and takes the old way, which is all HWR_DrawSprite did before. -hwfx 2 (check mode): the old way runs for every sprite, and what it hands
// to HWR_ProcessPolygon is compared with the record made here (HWC stream MISMATCH). -hwfx 524288 (FX3_NOSPR2): this builder is off, the stream takes the polygon of HWR_DrawSprite.
// --------------------------------------------------------------------------
static boolean HWR_FX_SpriteBuild(gl_vissprite_t *spr, ps2spr_t *e)
{
	mobj_t *mo = spr->mobj;
	sector_t *sector;
	GLPatch_t *gp;
	GLMipmap_t *m;
	INT32 blendmode;
	UINT8 lightlevel;
	float wx0, wx1, wy0, wy1, wy2, wz0, wz1, wz3;
	float x3, x2c, z3, y3;
	FBITFIELD blend;

	if (spr->bbox || spr->precip || spr->rotated || !mo || !mo->subsector || !spr->gpatch)
		return false;
	if ((mo->flags2 & (MF2_LINKDRAW | MF2_SPLAT)) || (mo->frame & (FF_FLOORSPRITE | FF_PAPERSPRITE | FF_TRANSMASK)) || (mo->renderflags & (RF_FLOORSPRITE | RF_PAPERSPRITE | RF_SHADOWEFFECTS))
		|| (mo->flags2 & MF2_SHADOW) || mo->alpha != FRACUNIT || (spr->colormap && spr->colormap != colormaps) || !cv_translucency.value || !HWR_UseShader())
		return false;
	sector = mo->subsector->sector;
	if (sector->numlights || sector->extra_colormap)
		return false;
	if (cv_glspritebillboarding.value && !spr->ps2_iok)
		return false; // (the aim rotation needs the state of the projection)
	gp = (GLPatch_t *)spr->gpatch->hardware;
	if (!gp)
		return false;
	m = gp->mipmap;
	if (!m->downloaded)
		return false; // not resident: the old way makes the data and the upload

	// the blend: an opaque sprite (the batch takes these) of the thing's blend mode
	if (mo->frame & FF_BLENDMASK)
		blendmode = ((mo->frame & FF_BLENDMASK) >> FF_BLENDSHIFT) + 1;
	else
		blendmode = mo->blendmode;
	blend = HWR_GetBlendModeFlag(blendmode);
	if (blend != PF_Translucent && blend != PF_Masked)
		return false;

	// the corners (HWR_DrawSprite, HWR_RotateSpritePolyToAim)
	wx0 = spr->x1;
	x2c = spr->x2;
	wy0 = spr->gz;
	y3 = spr->gzt;
	wz0 = spr->z1;
	wz1 = spr->z2;
	if (spr->dispoffset)
	{
		const float co = -gl_viewcos * (0.05f * spr->dispoffset);
		const float si = -gl_viewsin * (0.05f * spr->dispoffset);

		wz0 = wz0 + si;
		wz1 = wz1 + si;
		wx0 = wx0 + co;
		x2c = x2c + co;
	}
	wx1 = x2c; // corner 1 (x2, gz, z2); corner 3 (x1, gzt, z1)
	wy1 = wy0;
	wy2 = y3;
	x3 = wx0;
	z3 = wz0;
	wz3 = wz0;
	(void)wy2;
	if (cv_glspritebillboarding.value)
	{
		float basey, lowy;

		basey = P_MobjFlip(mo) == -1 ? FIXED_TO_FLOAT(spr->ps2_iz + spr->ps2_ih) : FIXED_TO_FLOAT(spr->ps2_iz);
		lowy = wy0;
		y3 = (spr->gzt - basey) * gl_viewludsin + basey;
		wy0 = (lowy - basey) * gl_viewludsin + basey;
		wy1 = wy0;
		x3 += ((spr->gzt - basey) * gl_viewludcos) * gl_viewcos;
		wx0 += ((lowy - basey) * gl_viewludcos) * gl_viewcos;
		wx1 += ((lowy - basey) * gl_viewludcos) * gl_viewcos;
		z3 += ((spr->gzt - basey) * gl_viewludcos) * gl_viewsin;
		wz0 += ((lowy - basey) * gl_viewludcos) * gl_viewsin;
		wz1 += ((lowy - basey) * gl_viewludcos) * gl_viewsin;
		wz3 = z3;
	}
	e->p0[0] = wx0;
	e->p0[1] = wy0;
	e->p0[2] = wz0;
	e->r[0] = wx1 - wx0;
	e->r[1] = wy1 - wy0;
	e->r[2] = wz1 - wz0;
	e->u[0] = x3 - wx0;
	e->u[1] = y3 - wy0;
	e->u[2] = wz3 - wz0;
	if (spr->flip)
	{
		e->sl = gp->max_s;
		e->sr = 0;
	}
	else
	{
		e->sl = 0;
		e->sr = gp->max_s;
	}
	if (spr->vflip)
	{
		e->tt = gp->max_t;
		e->tb = 0;
	}
	else
	{
		e->tt = 0;
		e->tb = gp->max_t;
	}

	// the surface: the light of the sector (HWR_Lighting without a colormap)
	if (R_ThingIsFullBright(mo))
		lightlevel = 255;
	else if (R_ThingIsFullDark(mo))
		lightlevel = 0;
	else
		lightlevel = sector->lightlevel > 255 ? 255 : sector->lightlevel;
	if (R_ThingIsSemiBright(mo))
		lightlevel = 128 + (lightlevel >> 1);
	if (!hwr_fx_lm_ok || hwr_fx_lm_level != lightlevel)
	{
		hwr_fx_lm_surf.PolyFlags = 0;
		HWR_Lighting(&hwr_fx_lm_surf, lightlevel, NULL);
		hwr_fx_lm_level = lightlevel;
		hwr_fx_lm_ok = true;
	}
	e->surf = hwr_fx_lm_surf;
	e->surf.PolyColor.s.alpha = 0xFF;
	e->flags = (unsigned int)(blend | PF_Occlude | PF_ColorMapped | PF_Modulated);
	e->shader = hwr_fx_spr_shader;
	e->tex = m;
	return true;
}

// the sprite goes to the stream (true) or takes the old way (false)
static boolean HWR_DrawSpriteStream(gl_vissprite_t *spr)
{
	ps2spr_t *e;
	GLMipmap_t *m;

	if (!(currently_batching && hwr_sprite_batch) || (ps2hwd_fx2 & (FX3_NOSPR | FX3_NOSPR2)))
		return false;
	e = PS2HWD_SprSlot(0);
	if (!e || !HWR_FX_SpriteBuild(spr, e))
		return false;
	m = (GLMipmap_t *)e->tex;
	PS2HWD_TouchTexture(m); // (what HWR_GetMappedPatch does in a batch: the picture stays in the GS pool until the batch is drawn)
	if (m->data)
		Z_ChangeTag(m->data, PU_HWRCACHE_UNLOCKED);
	return PS2HWD_SprCommit(0) != 0;
}

// check mode: the polygon the old way made of the sprite against the record of the builder
static void HWR_FX_SpriteCheck(const FSurfaceInfo *surf, const FOutVector *v, FBITFIELD flags, int shader_target)
{
	static unsigned chk, bad;
	ps2spr_t cur;
	boolean same;

	memset(&cur, 0, sizeof cur);
	chk++;
	if (!PS2HWD_SprFromVerts(&cur, v))
	{
		same = false;
	}
	else
	{
		same = !memcmp(cur.p0, hwr_fx_chk.p0, sizeof cur.p0) && !memcmp(cur.r, hwr_fx_chk.r, sizeof cur.r) && !memcmp(cur.u, hwr_fx_chk.u, sizeof cur.u)
			&& cur.sl == hwr_fx_chk.sl && cur.sr == hwr_fx_chk.sr && cur.tt == hwr_fx_chk.tt && cur.tb == hwr_fx_chk.tb && flags == hwr_fx_chk.flags && hwr_fx_chk.shader == HWR_GetShaderFromTarget(shader_target)
			&& surf->PolyColor.rgba == hwr_fx_chk.surf.PolyColor.rgba && surf->TintColor.rgba == hwr_fx_chk.surf.TintColor.rgba && surf->FadeColor.rgba == hwr_fx_chk.surf.FadeColor.rgba
			&& surf->LightTableId == hwr_fx_chk.surf.LightTableId && surf->LightInfo.light_level == hwr_fx_chk.surf.LightInfo.light_level && surf->LightInfo.fade_start == hwr_fx_chk.surf.LightInfo.fade_start
			&& surf->LightInfo.fade_end == hwr_fx_chk.surf.LightInfo.fade_end;
	}
	if (!same)
	{
		bad++;
		if (bad <= 20)
			CONS_Printf("HWC stream MISMATCH %u of %u: p0 %.3f %.3f %.3f / %.3f %.3f %.3f r %.3f %.3f %.3f / %.3f %.3f %.3f u %.3f %.3f %.3f / %.3f %.3f %.3f fl %x/%x sh %d/%d light %d/%d\n", bad, chk,
				cur.p0[0], cur.p0[1], cur.p0[2], hwr_fx_chk.p0[0], hwr_fx_chk.p0[1], hwr_fx_chk.p0[2], cur.r[0], cur.r[1], cur.r[2], hwr_fx_chk.r[0], hwr_fx_chk.r[1], hwr_fx_chk.r[2],
				cur.u[0], cur.u[1], cur.u[2], hwr_fx_chk.u[0], hwr_fx_chk.u[1], hwr_fx_chk.u[2], (unsigned)flags, (unsigned)hwr_fx_chk.flags, HWR_GetShaderFromTarget(shader_target), hwr_fx_chk.shader,
				(int)surf->LightInfo.light_level, (int)hwr_fx_chk.surf.LightInfo.light_level);
	}
	else if (!(chk & 1023))
	{
		CONS_Printf("HWC stream check: %u sprites, %u differ\n", chk, bad);
	}
}
#endif

static void HWR_DrawSprite(gl_vissprite_t *spr)
{
	FOutVector wallVerts[4];
	patch_t *gpatch;
	FSurfaceInfo Surf;
	const boolean splat = R_ThingIsFloorSprite(spr->mobj);
	HWD_LOCAL; // OPT11: the parts of this function (HWPROF35, --hwdetail)

	if (!spr->mobj)
		return;

	if (!spr->mobj->subsector)
		return;

	if (spr->mobj->subsector->sector->numlights && !splat)
	{
		HWR_SplitSprite(spr);
		return;
	}

#ifdef PS2_PROFILE
	hwr_fx_chk_ok = false;
	if (!splat)
	{
		if (ps2hwd_fx2 & FX2_PRECHECK)
			hwr_fx_chk_ok = (currently_batching && hwr_sprite_batch) && HWR_FX_SpriteBuild(spr, &hwr_fx_chk);
		else if (HWR_DrawSpriteStream(spr))
			return;
	}
#endif

	// cache sprite graphics
	//12/12/99: Hurdler:
	//          OK, I don't change anything for MD2 support because I want to be
	//          sure to do it the right way. So actually, we keep normal sprite
	//          in memory and we add the md2 model if it exists for that sprite

	gpatch = spr->gpatch;

#ifdef ALAM_LIGHTING
	if (!(spr->mobj->flags2 & MF2_DEBRIS) && (spr->mobj->sprite != SPR_PLAY ||
	 (spr->mobj->player && spr->mobj->player->powers[pw_super])))
		HWR_DL_AddLight(spr, gpatch);
#endif

	// create the sprite billboard
	//
	//  3--2
	//  | /|
	//  |/ |
	//  0--1

	if (splat)
	{
		F2DCoord verts[4];
		F2DCoord rotated[4];

		angle_t angle;
		float ca, sa;
		float w, h;
		float xscale, yscale;
		float xoffset, yoffset;
		float leftoffset, topoffset;
		float zoffset = (P_MobjFlip(spr->mobj) * 0.05f);
		pslope_t *splatslope = NULL;
		INT32 i;

		renderflags_t renderflags = spr->renderflags;

		if (spr->rotateflags & SRF_3D || renderflags & RF_NOSPLATBILLBOARD)
			angle = spr->angle;
		else
			angle = viewangle;

		if (!spr->rotated)
			angle += spr->mobj->spriteroll;

		angle = -angle;
		angle += ANGLE_90;

		topoffset = spr->spriteyoffset;
		leftoffset = spr->spritexoffset;
		if (spr->flip)
			leftoffset = ((float)gpatch->width - leftoffset);

		xscale = spr->scale * spr->spritexscale;
		yscale = spr->scale * spr->spriteyscale;

		xoffset = leftoffset * xscale;
		yoffset = topoffset * yscale;

		w = (float)gpatch->width * xscale;
		h = (float)gpatch->height * yscale;

		// Set positions

		// 3--2
		// |  |
		// 0--1

		verts[3].x = -xoffset;
		verts[3].y = yoffset;

		verts[2].x = w - xoffset;
		verts[2].y = yoffset;

		verts[1].x = w - xoffset;
		verts[1].y = -h + yoffset;

		verts[0].x = -xoffset;
		verts[0].y = -h + yoffset;

		ca = FIXED_TO_FLOAT(FINECOSINE((-angle)>>ANGLETOFINESHIFT));
		sa = FIXED_TO_FLOAT(FINESINE((-angle)>>ANGLETOFINESHIFT));

		// Rotate
		for (i = 0; i < 4; i++)
		{
			rotated[i].x = (verts[i].x * ca) - (verts[i].y * sa);
			rotated[i].y = (verts[i].x * sa) + (verts[i].y * ca);
		}

		// Translate
		for (i = 0; i < 4; i++)
		{
			wallVerts[i].x = rotated[i].x + spr->x1;
			wallVerts[i].z = rotated[i].y + spr->z1;
		}

		if (renderflags & (RF_SLOPESPLAT | RF_OBJECTSLOPESPLAT))
		{
			pslope_t *standingslope = spr->mobj->standingslope; // The slope that the object is standing on.

			// The slope that was defined for the sprite.
			if (renderflags & RF_SLOPESPLAT)
				splatslope = spr->mobj->floorspriteslope;

			if (standingslope && (renderflags & RF_OBJECTSLOPESPLAT))
				splatslope = standingslope;
		}

		// Set vertical position
		if (splatslope)
		{
			for (i = 0; i < 4; i++)
			{
				fixed_t slopez = P_GetSlopeZAt(splatslope, FLOAT_TO_FIXED(wallVerts[i].x), FLOAT_TO_FIXED(wallVerts[i].z));
				wallVerts[i].y = FIXED_TO_FLOAT(slopez) + zoffset;
			}
		}
		else
		{
			for (i = 0; i < 4; i++)
				wallVerts[i].y = FIXED_TO_FLOAT(spr->gz) + zoffset;
		}
	}
	else
	{
		// these were already scaled in HWR_ProjectSprite
		wallVerts[0].x = wallVerts[3].x = spr->x1;
		wallVerts[2].x = wallVerts[1].x = spr->x2;
		wallVerts[2].y = wallVerts[3].y = spr->gzt;
		wallVerts[0].y = wallVerts[1].y = spr->gz;

		// make a wall polygon (with 2 triangles), using the floor/ceiling heights,
		// and the 2d map coords of start/end vertices
		wallVerts[0].z = wallVerts[3].z = spr->z1;
		wallVerts[1].z = wallVerts[2].z = spr->z2;
	}

	HWD_LAP(HWP_DS_A);
	// cache the patch in the graphics card memory
	//12/12/99: Hurdler: same comment as above (for md2)
	//Hurdler: 25/04/2000: now support colormap in hardware mode
	HWR_GetMappedPatch(gpatch, spr->colormap);
	HWD_LAP(HWP_DS_B);

	if (spr->flip)
	{
		wallVerts[0].s = wallVerts[3].s = ((GLPatch_t *)gpatch->hardware)->max_s;
		wallVerts[2].s = wallVerts[1].s = 0;
	}else{
		wallVerts[0].s = wallVerts[3].s = 0;
		wallVerts[2].s = wallVerts[1].s = ((GLPatch_t *)gpatch->hardware)->max_s;
	}

	// flip the texture coords (look familiar?)
	if (spr->vflip)
	{
		wallVerts[3].t = wallVerts[2].t = ((GLPatch_t *)gpatch->hardware)->max_t;
		wallVerts[0].t = wallVerts[1].t = 0;
	}else{
		wallVerts[3].t = wallVerts[2].t = 0;
		wallVerts[0].t = wallVerts[1].t = ((GLPatch_t *)gpatch->hardware)->max_t;
	}

	if (!splat)
	{
		// if it has a dispoffset, push it a little towards the camera
		if (spr->dispoffset) {
			float co = -gl_viewcos*(0.05f*spr->dispoffset);
			float si = -gl_viewsin*(0.05f*spr->dispoffset);
			wallVerts[0].z = wallVerts[3].z = wallVerts[0].z+si;
			wallVerts[1].z = wallVerts[2].z = wallVerts[1].z+si;
			wallVerts[0].x = wallVerts[3].x = wallVerts[0].x+co;
			wallVerts[1].x = wallVerts[2].x = wallVerts[1].x+co;
		}

		// Let dispoffset work first since this adjust each vertex
		HWR_RotateSpritePolyToAim(spr, wallVerts, false);
		HWD_LAP(HWP_DS_C);
#ifdef PS2_PROFILE
		if (spr->ps2_hid) // PS2-HW-72 check mode: HWR_ProjectSprite would have culled this sprite: its quad here must hold no pixel centre either
		{
			static unsigned chk, bad;

			chk++;
			if (!PS2HWD_QuadHidden(wallVerts))
			{
				bad++;
				CONS_Printf("HWC sprite cull MISMATCH %u of %u (sprite %s)\n", bad, chk, spr->mobj && (UINT32)spr->mobj->sprite < NUMSPRITES ? sprnames[spr->mobj->sprite] : "?");
			}
			else if (!(chk & 1023))
			{
				CONS_Printf("HWC sprite cull check: %u sprites, %u differ\n", chk, bad);
			}
		}
#endif
	}

	// This needs to be AFTER the shadows so that the regular sprites aren't drawn completely black.
	// sprite lighting by modulating the RGB components
	/// \todo coloured

	// colormap test
	{
		sector_t *sector = spr->mobj->subsector->sector;
		UINT8 lightlevel = 0;
		boolean lightset = true;
		extracolormap_t *colormap = NULL;

		if (R_ThingIsFullBright(spr->mobj))
			lightlevel = 255;
		else if (R_ThingIsFullDark(spr->mobj))
			lightlevel = 0;
		else
			lightset = false;

		if (!(spr->mobj->renderflags & RF_NOCOLORMAPS))
			colormap = sector->extra_colormap;

		if (splat && sector->numlights)
		{
			INT32 light = R_GetPlaneLight(sector, spr->mobj->z, false);

			if (!lightset)
				lightlevel = *sector->lightlist[light].lightlevel > 255 ? 255 : *sector->lightlist[light].lightlevel;

			if (*sector->lightlist[light].extra_colormap && !(spr->mobj->renderflags & RF_NOCOLORMAPS))
				colormap = *sector->lightlist[light].extra_colormap;
		}
		else if (!lightset)
			lightlevel = sector->lightlevel > 255 ? 255 : sector->lightlevel;

		if (R_ThingIsSemiBright(spr->mobj))
			lightlevel = 128 + (lightlevel>>1);

		HWR_Lighting(&Surf, lightlevel, colormap);
	}
	HWD_LAP(HWP_DS_D);

	{
		INT32 shader = SHADER_NONE;
		FBITFIELD blend = 0;
		FBITFIELD occlusion;
		boolean use_linkdraw_hack = false;
		fixed_t newalpha = spr->mobj->alpha;

		// if sprite has linkdraw, then dont write to z-buffer (by not using PF_Occlude)
		// this will result in sprites drawn afterwards to be drawn on top like intended when using linkdraw.
		if ((spr->mobj->flags2 & MF2_LINKDRAW) && spr->mobj->tracer)
		{
			occlusion = 0;
			newalpha = spr->mobj->tracer->alpha;
		}
		else
			occlusion = PF_Occlude;

		INT32 blendmode;
		if (spr->mobj->frame & FF_BLENDMASK)
			blendmode = ((spr->mobj->frame & FF_BLENDMASK) >> FF_BLENDSHIFT) + 1;
		else
			blendmode = spr->mobj->blendmode;

		if (!cv_translucency.value) // translucency disabled
		{
			Surf.PolyColor.s.alpha = 0xFF;
			blend = PF_Translucent|occlusion;
			if (!occlusion) use_linkdraw_hack = true;
		}
		else if (spr->mobj->flags2 & MF2_SHADOW)
		{
			Surf.PolyColor.s.alpha = 0x40;
			blend = HWR_GetBlendModeFlag(blendmode);
		}
		else if (spr->mobj->frame & FF_TRANSMASK)
		{
			INT32 trans = (spr->mobj->frame & FF_TRANSMASK)>>FF_TRANSSHIFT;
			blend = HWR_SurfaceBlend(blendmode, trans, &Surf);
		}
		else
		{
			// BP: i agree that is little better in environement but it don't
			//     work properly under glide nor with fogcolor to ffffff :(
			// Hurdler: PF_Environement would be cool, but we need to fix
			//          the issue with the fog before
			Surf.PolyColor.s.alpha = 0xFF;
			blend = HWR_GetBlendModeFlag(blendmode)|occlusion;
			if (!occlusion) use_linkdraw_hack = true;
		}

		if (cv_translucency.value && newalpha < FRACUNIT)
		{
			// TODO: The ternary operator is a hack to make alpha values roughly match what their FF_TRANSMASK equivalent would be
			// See if there's a better way of doing this
			Surf.PolyColor.s.alpha = min(FixedMul(newalpha, Surf.PolyColor.s.alpha == 0xFF ? 256 : Surf.PolyColor.s.alpha), 0xFF);
			blend = HWR_GetBlendModeFlag(blendmode);
		}

		if (spr->renderflags & RF_SHADOWEFFECTS)
		{
			INT32 alpha = Surf.PolyColor.s.alpha;
			alpha -= ((INT32)(spr->shadowheight / 4.0f)) + 75;
			if (alpha < 1)
				return;

			Surf.PolyColor.s.alpha = (UINT8)(alpha);
			blend = PF_Translucent|occlusion;
			if (!occlusion) use_linkdraw_hack = true;
		}

		if (HWR_UseShader())
		{
			shader = SHADER_SPRITE;
			blend |= PF_ColorMapped;
		}

		HWD_LAP(HWP_DS_E);
#ifdef PS2_HWDETAIL
		if (!splat && !use_linkdraw_hack && PS2HWD_QuadHidden(wallVerts))
			HWD_ADD(HWC_FX_SPRHID); // (measurement only: the sprites drawn although their quad shows no pixel)
		if (spr->mobj->shadowscale && cv_shadow.value)
			HWD_ADD(HWC_FX_SPRSH);
#endif
#ifdef PS2_PROFILE
		if (hwr_fx_chk_ok)
		{
			hwr_fx_chk_ok = false;
			HWR_FX_SpriteCheck(&Surf, wallVerts, blend|PF_Modulated, shader);
		}
#endif
		HWR_ProcessPolygon(&Surf, wallVerts, 4, blend|PF_Modulated, shader, false);
		HWD_LAP(HWP_DS_F);

		if (use_linkdraw_hack)
			HWR_LinkDrawHackAdd(wallVerts, spr);
	}
}

// Sprite drawer for precipitation
static inline void HWR_DrawPrecipitationSprite(gl_vissprite_t *spr)
{
	INT32 shader = SHADER_NONE;
	FBITFIELD blend = 0;
	FOutVector wallVerts[4];
	patch_t *gpatch;
	FSurfaceInfo Surf;

	if (!spr->mobj)
		return;

	if (!spr->mobj->subsector)
		return;

	// cache sprite graphics
	gpatch = spr->gpatch;

	// create the sprite billboard
	//
	//  3--2
	//  | /|
	//  |/ |
	//  0--1
	wallVerts[0].x = wallVerts[3].x = spr->x1;
	wallVerts[2].x = wallVerts[1].x = spr->x2;
	wallVerts[2].y = wallVerts[3].y = spr->gzt;
	wallVerts[0].y = wallVerts[1].y = spr->gz;

	// make a wall polygon (with 2 triangles), using the floor/ceiling heights,
	// and the 2d map coords of start/end vertices
	wallVerts[0].z = wallVerts[3].z = spr->z1;
	wallVerts[1].z = wallVerts[2].z = spr->z2;

	// Let dispoffset work first since this adjust each vertex
	HWR_RotateSpritePolyToAim(spr, wallVerts, true);
#ifdef PS2_PROFILE
	if (spr->ps2_hid) // PS2-HW-72 check mode: this flake was to be culled (HWR_ProjectPrecipitationSprite): its quad here must hold no pixel centre either
	{
		static unsigned chk, bad;

		chk++;
		if (!PS2HWD_QuadHidden(wallVerts))
		{
			bad++;
			CONS_Printf("HWC precipitation cull MISMATCH %u of %u\n", bad, chk);
		}
		else if (!(chk & 1023))
		{
			CONS_Printf("HWC precipitation cull check: %u flakes, %u differ\n", chk, bad);
		}
	}
#endif

	wallVerts[0].s = wallVerts[3].s = 0;
	wallVerts[2].s = wallVerts[1].s = ((GLPatch_t *)gpatch->hardware)->max_s;

	wallVerts[3].t = wallVerts[2].t = 0;
	wallVerts[0].t = wallVerts[1].t = ((GLPatch_t *)gpatch->hardware)->max_t;

	// cache the patch in the graphics card memory
	//12/12/99: Hurdler: same comment as above (for md2)
	//Hurdler: 25/04/2000: now support colormap in hardware mode
	HWR_GetMappedPatch(gpatch, spr->colormap);

	// colormap test
	{
		sector_t *sector = spr->mobj->subsector->sector;
		UINT8 lightlevel = 255;
		extracolormap_t *colormap = sector->extra_colormap;

		if (sector->numlights)
		{
			// Always use the light at the top instead of whatever I was doing before
			INT32 light = R_GetPlaneLight(sector, spr->mobj->z + spr->mobj->height, false);

			if (!R_ThingIsFullBright(spr->mobj))
				lightlevel = *sector->lightlist[light].lightlevel > 255 ? 255 : *sector->lightlist[light].lightlevel;

			if (*sector->lightlist[light].extra_colormap)
				colormap = *sector->lightlist[light].extra_colormap;
		}
		else
		{
			if (!R_ThingIsFullBright(spr->mobj))
				lightlevel = sector->lightlevel > 255 ? 255 : sector->lightlevel;

			if (sector->extra_colormap)
				colormap = sector->extra_colormap;
		}

		HWR_Lighting(&Surf, lightlevel, colormap);
	}

	if (spr->mobj->frame & FF_TRANSMASK)
	{
		INT32 trans = (spr->mobj->frame & FF_TRANSMASK)>>FF_TRANSSHIFT;
		blend = HWR_SurfaceBlend(AST_TRANSLUCENT, trans, &Surf);
	}
	else
	{
		// BP: i agree that is little better in environement but it don't
		//     work properly under glide nor with fogcolor to ffffff :(
		// Hurdler: PF_Environement would be cool, but we need to fix
		//          the issue with the fog before
		Surf.PolyColor.s.alpha = 0xFF;
		blend = HWR_GetBlendModeFlag(spr->mobj->blendmode)|PF_Occlude;
	}

	if (HWR_UseShader())
	{
		shader = SHADER_SPRITE;
		blend |= PF_ColorMapped;
	}

	HWR_ProcessPolygon(&Surf, wallVerts, 4, blend|PF_Modulated, shader, false);
}

// --------------------------------------------------------------------------
// Sort vissprites by distance
// --------------------------------------------------------------------------
gl_vissprite_t* gl_vsprorder[MAXVISSPRITES];

// Note: For more correct transparency the transparent sprites would need to be
// sorted and drawn together with transparent surfaces.
static int CompareVisSprites(const void *p1, const void *p2)
{
	gl_vissprite_t* spr1 = *(gl_vissprite_t*const*)p1;
	gl_vissprite_t* spr2 = *(gl_vissprite_t*const*)p2;
	int idiff;
	float fdiff;
	float tz1, tz2;

	// Make transparent sprites last. Comment from the previous sort implementation:
	// Sryder:	Oh boy, while it's nice having ALL the sprites sorted properly, it fails when we bring MD2's into the
	//			mix and they want to be translucent. So let's place all the translucent sprites and MD2's AFTER
	//			everything else, but still ordered of course, the depth buffer can handle the opaque ones plenty fine.
	//			We just need to move all translucent ones to the end in order
	// TODO:	Fully sort all sprites and MD2s with walls and floors, this part will be unnecessary after that
	int transparency1;
	int transparency2;

	int linkdraw1;
	int linkdraw2;

	// draw bbox after everything else
	if (spr1->bbox || spr2->bbox)
		return (spr1->bbox - spr2->bbox);

	// check for precip first, because then sprX->mobj is actually a precipmobj_t and does not have flags2 or tracer
	linkdraw1 = !spr1->precip && (spr1->mobj->flags2 & MF2_LINKDRAW) && spr1->mobj->tracer;
	linkdraw2 = !spr2->precip && (spr2->mobj->flags2 & MF2_LINKDRAW) && spr2->mobj->tracer;

	// ^ is the XOR operation
	// if comparing a linkdraw and non-linkdraw sprite or 2 linkdraw sprites with different tracers, then use
	// the tracer's properties instead of the main sprite's.
	if ((linkdraw1 && linkdraw2 && spr1->mobj->tracer != spr2->mobj->tracer) || (linkdraw1 ^ linkdraw2))
	{
		if (linkdraw1)
		{
			tz1 = spr1->tracertz;
			transparency1 = (spr1->mobj->tracer->flags2 & MF2_SHADOW) || (spr1->mobj->tracer->frame & FF_TRANSMASK);
		}
		else
		{
			tz1 = spr1->tz;
			transparency1 = (!spr1->precip && (spr1->mobj->flags2 & MF2_SHADOW)) || (spr1->mobj->frame & FF_TRANSMASK);
		}
		if (linkdraw2)
		{
			tz2 = spr2->tracertz;
			transparency2 = (spr2->mobj->tracer->flags2 & MF2_SHADOW) || (spr2->mobj->tracer->frame & FF_TRANSMASK);
		}
		else
		{
			tz2 = spr2->tz;
			transparency2 = (!spr2->precip && (spr2->mobj->flags2 & MF2_SHADOW)) || (spr2->mobj->frame & FF_TRANSMASK);
		}
	}
	else
	{
		tz1 = spr1->tz;
		transparency1 = (!spr1->precip && (spr1->mobj->flags2 & MF2_SHADOW)) || (spr1->mobj->frame & FF_TRANSMASK);
		tz2 = spr2->tz;
		transparency2 = (!spr2->precip && (spr2->mobj->flags2 & MF2_SHADOW)) || (spr2->mobj->frame & FF_TRANSMASK);
	}

	// first compare transparency flags, then compare tz, then compare dispoffset

	idiff = transparency1 - transparency2;
	if (idiff != 0) return idiff;

	fdiff = tz2 - tz1; // this order seems correct when checking with apitrace. Back to front.
	if (fabsf(fdiff) < 1.0E-36f)
		return spr1->dispoffset - spr2->dispoffset; // smallest dispoffset first if sprites are at (almost) same location.
	else if (fdiff > 0)
		return 1;
	else
		return -1;
}

#ifdef PS2_PROFILE
static unsigned sortcheck_ok;
#endif

static void HWR_SortVisSprites(void)
{
	UINT32 i;
#ifdef PS2_PROFILE
	ps2hwp_cnt[HWC_SPRITES] += gl_visspritecount;
#endif
	for (i = 0; i < gl_visspritecount; i++)
	{
		gl_vsprorder[i] = HWR_GetVisSprite(i);
	}
#ifdef PS2_PROFILE
	// PS2-HW-50: qsort with CompareVisSprites (a call per comparison, five loads and several branches each) cost 0.3 M cycles for 530 sprites.
	// Without bounding boxes and linkdraw pairs (the comparator's special cases) the order is a plain key: transparent last, then eye depth
	// far to near, then display offset; a stable merge sort of the keys gives the same order (equal keys keep the traversal order).
	{
		// PS2-HW-50 (HG) made the order a plain key; PS2-HW-71 (HT): a stable LSD radix sort of the 32-bit depth key (HWR_RadixSort32, three passes of 11
		// bits) instead of the merge sort of 64-bit keys through an index array: 0.37 M cycles for 530 sprites. The order is the one of the comparator: the
		// display offset is the lowest key (a stable sort by it first, and only when some sprite has one), then the depth far to near, then the
		// transparent sprites are moved behind the opaque ones (a stable partition).
		static UINT32 dkey[MAXVISSPRITES], dix[MAXVISSPRITES], tk[MAXVISSPRITES], ti[MAXVISSPRITES], k2[MAXVISSPRITES];
		static UINT8 trn[MAXVISSPRITES];
		boolean plain = true, anydisp = false;
		UINT32 n = gl_visspritecount;

		for (i = 0; i < n && plain; i++)
		{
			gl_vissprite_t *spr = gl_vsprorder[i];
			union { float f; UINT32 u; } tz;
			UINT32 s;

			if (!(ps2hwd_fx2 & FX2_NOSORTKEY))
			{
				// PS2-HW-241: the two facts the key needs of the mobj were put in the vissprite when it was made (the mobj is a cache line away, with three loads)
				if (spr->ps2_skey & 2)
				{
					plain = false;
					break;
				}
				trn[i] = spr->ps2_skey & 1;
			}
			else
			{
			if (spr->bbox || (!spr->precip && (spr->mobj->flags2 & MF2_LINKDRAW) && spr->mobj->tracer))
			{
				plain = false;
				break;
			}
			trn[i] = ((!spr->precip && (spr->mobj->flags2 & MF2_SHADOW)) || (spr->mobj->frame & FF_TRANSMASK)) != 0;
			}
			tz.f = spr->tz;
			s = tz.u ^ ((tz.u & 0x80000000u) ? 0xFFFFFFFFu : 0x80000000u); // ascending with the float
			dkey[i] = ~s; // far to near
			dix[i] = i;
			if (spr->dispoffset)
				anydisp = true;
		}
		if (ps2hwd_dbg_flags & 0x40000000) // -hwdbg 1073741824: what the sprites of a frame are (HWSPR lines, one frame in 105)
		{
			static unsigned frn;

			if (++frn % 105 == 60)
			{
				static UINT16 cnt[1024];
				UINT32 pc = 0, tr = 0, k, top;
				int rep;

				memset(cnt, 0, sizeof cnt);
				for (k = 0; k < n; k++)
				{
					const gl_vissprite_t *sp = gl_vsprorder[k];

					if (sp->precip)
						pc++;
					else if (sp->mobj && (UINT32)sp->mobj->sprite < 1024)
					{
						cnt[sp->mobj->sprite]++;
						tr += ((sp->mobj->frame & FF_TRANSMASK) || (sp->mobj->flags2 & MF2_SHADOW)) ? 1 : 0;
					}
				}
				CONS_Printf("HWSPR frame %u: %u vissprites, %u precipitation, %u translucent\n", frn, (unsigned)n, (unsigned)pc, (unsigned)tr);
				{
					// the height of the sprites on the screen (about 120 pixels per world unit at depth 1): < 1, < 2, < 4, < 8, < 16, more
					UINT32 hist[6] = {0, 0, 0, 0, 0, 0};

					for (k = 0; k < n; k++)
					{
						const gl_vissprite_t *sp = gl_vsprorder[k];
						float ph = sp->tz > 1.0f ? 120.0f * (sp->gzt - sp->gz) / sp->tz : 1000.0f;

						hist[ph < 1.0f ? 0 : ph < 2.0f ? 1 : ph < 4.0f ? 2 : ph < 8.0f ? 3 : ph < 16.0f ? 4 : 5]++;
					}
					CONS_Printf("HWSPR   height in pixels <1:%u <2:%u <4:%u <8:%u <16:%u more:%u\n", (unsigned)hist[0], (unsigned)hist[1], (unsigned)hist[2], (unsigned)hist[3], (unsigned)hist[4], (unsigned)hist[5]);
				}
				for (rep = 0; rep < 12; rep++)
				{
					top = 0;
					for (k = 1; k < 1024; k++)
						if (cnt[k] > cnt[top])
							top = k;
					if (!cnt[top])
						break;
					CONS_Printf("HWSPR   %s x%u\n", top < NUMSPRITES ? sprnames[top] : "?", (unsigned)cnt[top]);
					cnt[top] = 0;
				}
			}
		}
		if (plain)
		{
			UINT32 *res; // the sprites (indices of the traversal order) from far to near

			if (anydisp)
			{
				UINT32 *p1, *sc;

				for (i = 0; i < n; i++)
					k2[i] = (UINT32)((INT32)gl_vsprorder[i]->dispoffset + 0x4000) & 0x7FFFu;
				p1 = HWR_RadixSort32(k2, dix, tk, ti, n) ? dix : ti; // the indices in the order of the display offsets
				sc = p1 == dix ? ti : dix; // the arrays the next sort may use as scratch
				for (i = 0; i < n; i++)
					tk[i] = dkey[p1[i]]; // the depth keys in that order
				res = HWR_RadixSort32(tk, p1, k2, sc, n) ? p1 : sc;
			}
			else
			{
				res = HWR_RadixSort32(dkey, dix, tk, ti, n) ? dix : ti;
			}
			{
				static gl_vissprite_t *copy[MAXVISSPRITES]; // gl_vsprorder is rewritten from a copy of itself
				UINT32 o = 0;

				memcpy(copy, gl_vsprorder, n * sizeof copy[0]);
				for (i = 0; i < n; i++) // opaque sprites first, transparent ones behind them, each group in the depth order
					if (!trn[res[i]])
						gl_vsprorder[o++] = copy[res[i]];
				for (i = 0; i < n; i++)
					if (trn[res[i]])
						gl_vsprorder[o++] = copy[res[i]];
				if (ps2hwd_dbg_flags & 0x20000) // -hwdbg 131072: the old qsort of the same sprites; every position where the two orders disagree (ties aside) is reported
				{
					UINT32 bad = 0;

					qsort(copy, gl_visspritecount, sizeof copy[0], CompareVisSprites);
					for (i = 0; i < gl_visspritecount; i++)
						if (CompareVisSprites(&gl_vsprorder[i], &copy[i]) != 0)
							bad++;
					if (bad)
						I_OutputMsg("HWSORTCHECK %u of %u positions differ from qsort\n", (unsigned)bad, (unsigned)gl_visspritecount);
					else
						sortcheck_ok++;
				}
			}
			return;
		}
	}
#endif
	qsort(gl_vsprorder, gl_visspritecount, sizeof(gl_vissprite_t*), CompareVisSprites);
}

// A drawnode is something that points to a 3D floor, 3D side, or masked
// middle texture. This is used for sorting with sprites.
typedef struct
{
	FOutVector    wallVerts[4];
	FSurfaceInfo  Surf;
	INT32         texnum;
	FBITFIELD     blend;
	INT32         drawcount;
	boolean fogwall;
	INT32 lightlevel;
	extracolormap_t *wallcolormap; // Doing the lighting in HWR_RenderWall now for correct fog after sorting
} wallinfo_t;

static wallinfo_t *wallinfo = NULL;
static size_t numwalls = 0; // a list of transparent walls to be drawn

void HWR_RenderWall(FOutVector *wallVerts, FSurfaceInfo *pSurf, FBITFIELD blend, boolean fogwall, INT32 lightlevel, extracolormap_t *wallcolormap);

#define MAX_TRANSPARENTWALL 256

typedef struct
{
	extrasubsector_t *xsub;
	boolean isceiling;
	fixed_t fixedheight;
	INT32 lightlevel;
	levelflat_t *levelflat;
	INT32 alpha;
	sector_t *FOFSector;
	FBITFIELD blend;
	boolean fogplane;
	boolean chromakeyed;
	extracolormap_t *planecolormap;
	INT32 drawcount;
} planeinfo_t;

static size_t numplanes = 0; // a list of transparent floors to be drawn
static planeinfo_t *planeinfo = NULL;

typedef struct
{
	polyobj_t *polysector;
	boolean isceiling;
	fixed_t fixedheight;
	INT32 lightlevel;
	levelflat_t *levelflat;
	INT32 alpha;
	sector_t *FOFSector;
	FBITFIELD blend;
	extracolormap_t *planecolormap;
	INT32 drawcount;
} polyplaneinfo_t;

static size_t numpolyplanes = 0; // a list of transparent poyobject floors to be drawn
static polyplaneinfo_t *polyplaneinfo = NULL;

//Hurdler: 3D water sutffs
typedef struct gl_drawnode_s
{
	planeinfo_t *plane;
	polyplaneinfo_t *polyplane;
	wallinfo_t *wall;
	gl_vissprite_t *sprite;

//	struct gl_drawnode_s *next;
//	struct gl_drawnode_s *prev;
} gl_drawnode_t;

static INT32 drawcount = 0;

#define MAX_TRANSPARENTFLOOR 512

// This will likely turn into a copy of HWR_Add3DWater and replace it.
void HWR_AddTransparentFloor(levelflat_t *levelflat, extrasubsector_t *xsub, boolean isceiling, fixed_t fixedheight, INT32 lightlevel, INT32 alpha, sector_t *FOFSector, FBITFIELD blend, boolean fogplane, boolean chromakeyed, extracolormap_t *planecolormap)
{
	static size_t allocedplanes = 0;

	// Force realloc if buffer has been freed
	if (!planeinfo)
		allocedplanes = 0;

	if (allocedplanes < numplanes + 1)
	{
		allocedplanes += MAX_TRANSPARENTFLOOR;
		Z_Realloc(planeinfo, allocedplanes * sizeof (*planeinfo), PU_LEVEL, &planeinfo);
	}

	planeinfo[numplanes].isceiling = isceiling;
	planeinfo[numplanes].fixedheight = fixedheight;
	planeinfo[numplanes].lightlevel = (planecolormap && (planecolormap->flags & CMF_FOG)) ? lightlevel : 255; // TODO: 2.3: Make transparent FOF planes always use light level
	planeinfo[numplanes].levelflat = levelflat;
	planeinfo[numplanes].xsub = xsub;
	planeinfo[numplanes].alpha = alpha;
	planeinfo[numplanes].FOFSector = FOFSector;
	planeinfo[numplanes].blend = blend;
	planeinfo[numplanes].fogplane = fogplane;
	planeinfo[numplanes].chromakeyed = chromakeyed;
	planeinfo[numplanes].planecolormap = planecolormap;
	planeinfo[numplanes].drawcount = drawcount++;

	numplanes++;
}

// Adding this for now until I can create extrasubsector info for polyobjects
// When that happens it'll just be done through HWR_AddTransparentFloor and HWR_RenderPlane
void HWR_AddTransparentPolyobjectFloor(levelflat_t *levelflat, polyobj_t *polysector, boolean isceiling, fixed_t fixedheight, INT32 lightlevel, INT32 alpha, sector_t *FOFSector, FBITFIELD blend, extracolormap_t *planecolormap)
{
	static size_t allocedpolyplanes = 0;

	// Force realloc if buffer has been freed
	if (!polyplaneinfo)
		allocedpolyplanes = 0;

	if (allocedpolyplanes < numpolyplanes + 1)
	{
		allocedpolyplanes += MAX_TRANSPARENTFLOOR;
		Z_Realloc(polyplaneinfo, allocedpolyplanes * sizeof (*polyplaneinfo), PU_LEVEL, &polyplaneinfo);
	}

	polyplaneinfo[numpolyplanes].isceiling = isceiling;
	polyplaneinfo[numpolyplanes].fixedheight = fixedheight;
	polyplaneinfo[numpolyplanes].lightlevel = (planecolormap && (planecolormap->flags & CMF_FOG)) ? lightlevel : 255;  // TODO: 2.3: Make transparent polyobject planes always use light level
	polyplaneinfo[numpolyplanes].levelflat = levelflat;
	polyplaneinfo[numpolyplanes].polysector = polysector;
	polyplaneinfo[numpolyplanes].alpha = alpha;
	polyplaneinfo[numpolyplanes].FOFSector = FOFSector;
	polyplaneinfo[numpolyplanes].blend = blend;
	polyplaneinfo[numpolyplanes].planecolormap = planecolormap;
	polyplaneinfo[numpolyplanes].drawcount = drawcount++;
	numpolyplanes++;
}

// putting sortindex and sortnode here so the comparator function can see them
gl_drawnode_t *sortnode;
size_t *sortindex;

static int CompareDrawNodes(const void *p1, const void *p2)
{
	size_t n1 = *(const size_t*)p1;
	size_t n2 = *(const size_t*)p2;
	INT32 v1 = 0;
	INT32 v2 = 0;
	INT32 diff;
	if (sortnode[n1].plane)
		v1 = sortnode[n1].plane->drawcount;
	else if (sortnode[n1].polyplane)
		v1 = sortnode[n1].polyplane->drawcount;
	else if (sortnode[n1].wall)
		v1 = sortnode[n1].wall->drawcount;
	else I_Error("CompareDrawNodes: n1 unknown");

	if (sortnode[n2].plane)
		v2 = sortnode[n2].plane->drawcount;
	else if (sortnode[n2].polyplane)
		v2 = sortnode[n2].polyplane->drawcount;
	else if (sortnode[n2].wall)
		v2 = sortnode[n2].wall->drawcount;
	else I_Error("CompareDrawNodes: n2 unknown");

	diff = v2 - v1;
	if (diff == 0) I_Error("CompareDrawNodes: diff is zero");
	return diff;
}

static int CompareDrawNodePlanes(const void *p1, const void *p2)
{
	size_t n1 = *(const size_t*)p1;
	size_t n2 = *(const size_t*)p2;
	if (!sortnode[n1].plane) I_Error("CompareDrawNodePlanes: Uh.. This isn't a plane! (n1)");
	if (!sortnode[n2].plane) I_Error("CompareDrawNodePlanes: Uh.. This isn't a plane! (n2)");
	return abs(sortnode[n2].plane->fixedheight - viewz) - abs(sortnode[n1].plane->fixedheight - viewz);
}

//
// HWR_CreateDrawNodes
// Creates and sorts a list of drawnodes for the scene being rendered.
static void HWR_CreateDrawNodes(void)
{
	UINT32 i = 0, p = 0;
	size_t run_start = 0;

	// Dump EVERYTHING into a huge drawnode list. Then we'll sort it!
	// Could this be optimized into _AddTransparentWall/_AddTransparentPlane?
	// Hell yes! But sort algorithm must be modified to use a linked list.
#ifdef PS2_PROFILE // PS2-HW-40: persistent work arrays instead of two PU_STATIC zone blocks per frame (every one of them walked the arena: Z_MoveFrontier, 1..7 M cycles a frame)
	{
		static gl_drawnode_t *dn_buf;
		static size_t *di_buf;
		static size_t dn_cap;
		const size_t need = numplanes + numpolyplanes + numwalls;

		if (need > dn_cap)
		{
			size_t cap = dn_cap ? dn_cap : 128;

			while (cap < need)
				cap += cap / 2;
			dn_buf = realloc(dn_buf, cap * sizeof *dn_buf);
			di_buf = realloc(di_buf, cap * sizeof *di_buf);
			if (!dn_buf || !di_buf)
				I_Error("HWR_CreateDrawNodes: out of memory (%lu nodes)", (unsigned long)cap);
			dn_cap = cap;
		}
		memset(dn_buf, 0, need * sizeof *dn_buf);
		sortnode = dn_buf;
		sortindex = di_buf;
	}
#else
	sortnode = Z_Calloc((sizeof(planeinfo_t)*numplanes)
					+ (sizeof(polyplaneinfo_t)*numpolyplanes)
					+ (sizeof(wallinfo_t)*numwalls)
					,PU_STATIC, NULL);
	// todo:
	// However, in reality we shouldn't be re-copying and shifting all this information
	// that is already lying around. This should all be in some sort of linked list or lists.
	sortindex = Z_Calloc(sizeof(size_t) * (numplanes + numpolyplanes + numwalls), PU_STATIC, NULL);
#endif

	PS_START_TIMING(ps_hw_nodesorttime);
	HWP_SPAN_BEGIN(hwp_tnsort);

	for (i = 0; i < numplanes; i++, p++)
	{
		sortnode[p].plane = &planeinfo[i];
		sortindex[p] = p;
	}

	for (i = 0; i < numpolyplanes; i++, p++)
	{
		sortnode[p].polyplane = &polyplaneinfo[i];
		sortindex[p] = p;
	}

	for (i = 0; i < numwalls; i++, p++)
	{
		sortnode[p].wall = &wallinfo[i];
		sortindex[p] = p;
	}

	ps_numdrawnodes.value.i = p;

	// p is the number of stuff to sort

	// sort the list based on the value of the 'drawcount' member of the drawnodes.
	qsort(sortindex, p, sizeof(size_t), CompareDrawNodes);

	// an additional pass is needed to correct the order of consecutive planes in the list.
	// for each consecutive run of planes in the list, sort that run based on plane height and view height.
	while (run_start < p-1)// p-1 because a 1 plane run at the end of the list does not count
	{
		// locate run start
		if (sortnode[sortindex[run_start]].plane)
		{
			// found it, now look for run end
			size_t run_end;// (inclusive)
			for (i = run_start+1; i < p; i++)// size_t and UINT32 being used mixed here... shouldnt break anything though..
			{
				if (!sortnode[sortindex[i]].plane) break;
			}
			run_end = i-1;
			if (run_end > run_start)// if there are multiple consecutive planes, not just one
			{
				// consecutive run of planes found, now sort it
				qsort(sortindex + run_start, run_end - run_start + 1, sizeof(size_t), CompareDrawNodePlanes);
			}
			run_start = run_end + 1;// continue looking for runs coming right after this one
		}
		else
		{
			// this wasnt the run start, try next one
			run_start++;
		}
	}

	PS_STOP_TIMING(ps_hw_nodesorttime);
	HWP_SPAN_END(hwp_tnsort, HWP_NODESORT);

	PS_START_TIMING(ps_hw_nodedrawtime);
	HWP_SPAN_BEGIN(hwp_tndraw);

	// Okay! Let's draw it all! Woo!
	HWD.pfnSetTransform(&atransform);

	for (i = 0; i < p; i++)
	{
		if (sortnode[sortindex[i]].plane)
		{
			// We aren't traversing the BSP tree, so make gl_frontsector null to avoid crashes.
			gl_frontsector = NULL;

			if (!(sortnode[sortindex[i]].plane->blend & PF_NoTexture))
				HWR_GetLevelFlat(sortnode[sortindex[i]].plane->levelflat, sortnode[sortindex[i]].plane->chromakeyed);
			HWR_RenderPlane(NULL, sortnode[sortindex[i]].plane->xsub, sortnode[sortindex[i]].plane->isceiling, sortnode[sortindex[i]].plane->fixedheight, sortnode[sortindex[i]].plane->blend, sortnode[sortindex[i]].plane->lightlevel,
				sortnode[sortindex[i]].plane->levelflat, sortnode[sortindex[i]].plane->FOFSector, sortnode[sortindex[i]].plane->alpha, sortnode[sortindex[i]].plane->planecolormap);
		}
		else if (sortnode[sortindex[i]].polyplane)
		{
			// We aren't traversing the BSP tree, so make gl_frontsector null to avoid crashes.
			gl_frontsector = NULL;

			polyobj_t *po = sortnode[sortindex[i]].polyplane->polysector;

			if (!(sortnode[sortindex[i]].polyplane->blend & PF_NoTexture))
				HWR_GetLevelFlat(sortnode[sortindex[i]].polyplane->levelflat, po->flags & POF_SPLAT);
			HWR_RenderPolyObjectPlane(po, sortnode[sortindex[i]].polyplane->isceiling, sortnode[sortindex[i]].polyplane->fixedheight, sortnode[sortindex[i]].polyplane->blend, sortnode[sortindex[i]].polyplane->lightlevel,
				sortnode[sortindex[i]].polyplane->levelflat, sortnode[sortindex[i]].polyplane->FOFSector, sortnode[sortindex[i]].polyplane->alpha, sortnode[sortindex[i]].polyplane->planecolormap);
		}
		else if (sortnode[sortindex[i]].wall)
		{
			if (!(sortnode[sortindex[i]].wall->blend & PF_NoTexture))
				HWR_GetTexture(sortnode[sortindex[i]].wall->texnum, true);
			HWR_RenderWall(sortnode[sortindex[i]].wall->wallVerts, &sortnode[sortindex[i]].wall->Surf, sortnode[sortindex[i]].wall->blend, sortnode[sortindex[i]].wall->fogwall,
				sortnode[sortindex[i]].wall->lightlevel, sortnode[sortindex[i]].wall->wallcolormap);
		}
	}

	PS_STOP_TIMING(ps_hw_nodedrawtime);
	HWP_SPAN_END(hwp_tndraw, HWP_NODEDRAW);

	numwalls = 0;
	numplanes = 0;
	numpolyplanes = 0;

	// No mem leaks, please.
#ifdef PS2_PROFILE
	sortnode = NULL; // persistent buffers (above)
	sortindex = NULL;
#else
	Z_Free(sortnode);
	Z_Free(sortindex);
#endif
}

// --------------------------------------------------------------------------
//  Draw all vissprites
// --------------------------------------------------------------------------

// added the stransform so they can be switched as drawing happenes so MD2s and sprites are sorted correctly with each other
static void HWR_DrawSprites(void)
{
	UINT32 i;
	boolean skipshadow = false; // skip shadow if it was drawn already for a linkdraw sprite encountered earlier in the list
#ifdef PS2_PROFILE
	// PS2-HW-52: the opaque sprites and the shadows are collected and drawn as batches (see HWR_ProcessPolygon); -hwdbg 524288 draws them one by one
	boolean sprbatch = cv_glbatching.value && gl_visspritecount > 1 && !currently_batching && !(ps2hwd_dbg_flags & 0x80000);

	if (sprbatch)
		HWC_ADD(HWC_SPR_ON);
#endif
	HWD.pfnSetSpecialState(HWD_SET_MODEL_LIGHTING, cv_glmodellighting.value);
	for (i = 0; i < gl_visspritecount; i++)
	{
		gl_vissprite_t *spr = gl_vsprorder[i];
#ifdef PS2_PROFILE
		boolean sprwas = false;

		if (ps2hwd_fx2 & FX3_NODRAW) // (measurement only)
			continue;

		if (sprbatch)
		{
			if (!spr->bbox && !spr->precip && spr->dispoffset)
			{
				// sprites with a display offset overlay one another at (nearly) the same depth, where the order of drawing decides: not batched
				HWC_ADD(HWC_SPR_SOLO);
				if (currently_batching)
					HWR_RenderBatches();
				hwr_sprite_batch = false;
			}
			else if (!currently_batching)
			{
				HWR_StartBatching();
				hwr_sprite_batch = true;
			}
			sprwas = currently_batching && hwr_sprite_batch;
		}
#endif
		HWP_SPAN_BEGIN(tsd);
		if ((spr->ps2_skey & 4) && sprwas && hwr_fx_cheap && !(ps2hwd_fx2 & (FX3_NOSPR | FX3_NOSPR2 | FX3_NOSHORT | FX2_PRECHECK)))
		{
			// PS2-HW-258 (OPT11 round 3, FX3): the plain sprite of the sprite batch (made by HWR_ProjectPlain: no link draw, no skin, no model, no hitbox) goes from the sorted list to its drop shadow and
			// its record at once: what HWR_DrawSprites and HWR_DrawSprite do for it before they get there (the kind of the sprite, the link draw pair, the model table) is nothing for such a thing
			if (spr->mobj->shadowscale && cv_shadow.value && !skipshadow) // (the shadow of the tracer of a link draw pair was drawn before the pair)
			{
				hwr_sprite_shadow = true;
				HWC_ADD(HWC_SPR_SHADOW);
				{
				HWP_SPAN_BEGIN(tsh);
				HWR_DrawDropShadow(spr->mobj, spr, spr->mobj->shadowscale);
				HWP_SPAN_END(tsh, HWP_SP_SHADOW);
				}
				hwr_sprite_shadow = false;
			}
			skipshadow = false;
			if (!HWR_DrawSpriteStream(spr))
				HWR_DrawSprite(spr);
		}
		else if (spr->bbox)
			HWR_DrawBoundingBox(spr);
		else if (spr->precip)
			HWR_DrawPrecipitationSprite(spr);
		else
		{
			if (spr->mobj && spr->mobj->shadowscale && cv_shadow.value && !skipshadow)
			{
#ifdef PS2_PROFILE
				hwr_sprite_shadow = true;
				if (currently_batching)
					HWC_ADD(HWC_SPR_SHADOW);
#endif
				{
				HWP_SPAN_BEGIN(tsh);
				HWR_DrawDropShadow(spr->mobj, spr, spr->mobj->shadowscale);
				HWP_SPAN_END(tsh, HWP_SP_SHADOW);
				}
#ifdef PS2_PROFILE
				hwr_sprite_shadow = false;
#endif
			}

			if ((spr->mobj->flags2 & MF2_LINKDRAW) && spr->mobj->tracer)
			{
				// If this linkdraw sprite is behind a sprite that has a shadow,
				// then that shadow has to be drawn first, otherwise the shadow ends up on top of
				// the linkdraw sprite because the linkdraw sprite does not modify the z-buffer.
				// The !skipshadow check is there in case there are multiple linkdraw sprites connected
				// to the same tracer, so the tracer's shadow only gets drawn once.
				if (cv_shadow.value && !skipshadow && spr->dispoffset < 0 && spr->mobj->tracer->shadowscale)
				{
					HWR_DrawDropShadow(spr->mobj->tracer, spr, spr->mobj->tracer->shadowscale);
					skipshadow = true;
					// The next sprite in this loop should be either another linkdraw sprite or the tracer.
					// When the tracer is inevitably encountered, skipshadow will cause it's shadow
					// to get skipped and skipshadow will get set to false by the 'else' clause below.
				}
			}
			else
			{
				skipshadow = false;
			}

			if (spr->mobj && spr->mobj->skin && spr->mobj->sprite == SPR_PLAY)
			{
				if (!cv_glmodels.value || !md2_playermodels[((skin_t*)spr->mobj->skin)->skinnum].found || md2_playermodels[((skin_t*)spr->mobj->skin)->skinnum].scale < 0.0f)
					HWR_DrawSprite(spr);
				else
				{
#ifdef PS2_PROFILE
					if (currently_batching) // a model is drawn by the backend at once
						HWR_RenderBatches();
					hwr_sprite_batch = false;
#endif
					if (!HWR_DrawModel(spr))
						HWR_DrawSprite(spr);
				}
			}
			else
			{
				if (!cv_glmodels.value || !md2_models[spr->mobj->sprite].found || md2_models[spr->mobj->sprite].scale < 0.0f)
					HWR_DrawSprite(spr);
				else
				{
#ifdef PS2_PROFILE
					if (currently_batching)
						HWR_RenderBatches();
					hwr_sprite_batch = false;
#endif
					if (!HWR_DrawModel(spr))
						HWR_DrawSprite(spr);
				}
			}
		}
		HWP_SPAN_END(tsd, HWP_SP_DRAW);
#ifdef PS2_PROFILE
		if (sprwas && !hwr_sprite_batch)
			HWC_ADD(HWC_SPR_FLUSH), sprbatch = false; // the first polygon that was not order independent drew the batch: what follows is drawn in order
#endif
	}
#ifdef PS2_PROFILE
	{
	HWP_SPAN_BEGIN(tsf);
	if (currently_batching)
		HWR_RenderBatches();
	hwr_sprite_batch = false;
	hwr_sprite_shadow = false;
	HWP_SPAN_END(tsf, HWP_SP_FLUSH);
	}
#endif
	HWD.pfnSetSpecialState(HWD_SET_MODEL_LIGHTING, 0);

	// At the end of sprite drawing, draw shapes of linkdraw sprites to z-buffer, so they
	// don't get drawn over by transparent surfaces.
	HWR_LinkDrawHackFinish();
	// Work around a r_opengl.c bug with PF_Invisible by making this SetBlend call
	// where PF_Invisible is off and PF_Masked is on.
	// (Other states probably don't matter. Here I left them same as in LinkDrawHackFinish)
	// Without this workaround the rest of the draw calls in this frame (including UI, screen texture)
	// can get drawn using an incorrect glBlendFunc, resulting in a occasional black screen.
	HWD.pfnSetBlend(PF_Translucent|PF_Occlude|PF_Masked);
}

// --------------------------------------------------------------------------
// HWR_AddSprites
// During BSP traversal, this adds sprites by sector.
// --------------------------------------------------------------------------
static UINT8 sectorlight;
#ifdef PS2_PROFILE
// --------------------------------------------------------------------------
// PS2-HW-240 (OPT11 round 2, FX2): the sphere test of the things.
// HWR_ProjectSprite of a thing outside the view did the frame and the rotation of the sprite, its offsets, the quad and the quad test of the driver (PS2HWD_QuadHidden):
// 1000 cycles for each of the 440 things of a DEMO_004 frame that the test then threw away (and 110 of DEMO_001). Here a sphere around the thing's position that holds
// every corner of the quad the sprite could have is tested against the four side planes of the view volume first (the driver's clip transform of the sphere's centre
// and the gradients of the planes: ps2cull_t); a sphere wholly outside one side holds four corners that are outside it, which is what PS2HWD_QuadHidden calls hidden.
// Only for sprites whose quad the old path tests at all (not the floor sprites, the sprites with a drop shadow, the models, the link draws) and whose size is
// known from the picture alone (no rotation of the sprite, no scaling by the thing, no skin, no overlay).
// -hwfx 1 switches it off, -hwfx 2 checks it (HWC pre MISMATCH when a rejected thing gets a vissprite from the old path).
// --------------------------------------------------------------------------

static void HWR_FX_ExtMake(UINT32 spr, hwr_fx_ext_t *e)
{
	const spritedef_t *def = &sprites[spr];
	float hr = 0.0f, vr = 0.0f;
	size_t f;
	int r;

	e->ok = -1;
	if (!def->numframes || !def->spriteframes)
		return;
	for (f = 0; f < def->numframes; f++)
	{
		const spriteframe_t *sf = &def->spriteframes[f];

		for (r = 0; r < 16; r++)
		{
			const size_t id = sf->lumpid[r];
			float w, off, h, top, a, b;

			if (id >= numspritelumps)
				continue;
			w = FIXED_TO_FLOAT(spritecachedinfo[id].width);
			off = FIXED_TO_FLOAT(spritecachedinfo[id].offset);
			h = FIXED_TO_FLOAT(spritecachedinfo[id].height);
			top = FIXED_TO_FLOAT(spritecachedinfo[id].topoffset);
			a = fabsf(off);
			b = fabsf(w - off);
			a = a > b ? a : b;
			hr = a > hr ? a : hr;
			a = fabsf(top) + h;
			vr = a > vr ? a : vr;
			e->ok = 1;
		}
	}
	e->hr = hr;
	e->vr = vr;
}

static inline boolean HWR_FX_ThingHidden(const mobj_t *thing, const ps2cull_t *cs)
{
	hwr_fx_ext_t *e;
	float sc, hr, vr, X, Y, Z, R;

	// the thing's position inside the side planes: no sphere around it is outside them (the most common answer, it takes three loads and a dozen operations)
	X = FIXED_TO_FLOAT(thing->x);
	Y = FIXED_TO_FLOAT(thing->z);
	Z = FIXED_TO_FLOAT(thing->y);
	{
		const float cw = cs->r[3][0] * X + cs->r[3][1] * Y + cs->r[3][2] * Z + cs->r[3][3];
		const float cx = cs->r[0][0] * X + cs->r[0][1] * Y + cs->r[0][2] * Z + cs->r[0][3];
		const float cy = cs->r[1][0] * X + cs->r[1][1] * Y + cs->r[1][2] * Z + cs->r[1][3];

		if (cx <= cw && cx >= -cw && cy <= cw && cy >= -cw)
			return false;
	}
	if ((UINT32)thing->sprite >= NUMSPRITES || (UINT32)thing->sprite >= numsprites)
		return false;
	if (cv_glmodels.value || !r_renderthings || (cv_shadow.value && thing->shadowscale) || (ps2hwd_dbg_flags & 0x1000000))
		return false; // (the conditions of the quad test in HWR_ProjectSprite)
	if ((thing->flags2 & (MF2_LINKDRAW | MF2_SPLAT)) || (thing->frame & (FF_PAPERSPRITE | FF_FLOORSPRITE))
		|| (thing->renderflags & (RF_PAPERSPRITE | RF_FLOORSPRITE | RF_ABSOLUTEOFFSETS | RF_SHADOWEFFECTS))
		|| thing->type == MT_OVERLAY || thing->skin || thing->spriteroll || thing->old_spriteroll || thing->scale != thing->old_scale)
		return false;
	e = &hwr_fx_ext[thing->sprite];
	if (e->ok <= 0)
	{
		if (e->ok < 0)
			return false;
		HWR_FX_ExtMake((UINT32)thing->sprite, e);
		if (e->ok < 0)
			return false;
	}
	sc = FIXED_TO_FLOAT(thing->scale);
	{
		const fixed_t xs = thing->spritexscale > thing->old_spritexscale ? thing->spritexscale : thing->old_spritexscale;
		const fixed_t ys = thing->spriteyscale > thing->old_spriteyscale ? thing->spriteyscale : thing->old_spriteyscale;

		hr = (e->hr + fabsf(FIXED_TO_FLOAT(thing->spritexoffset))) * sc * FIXED_TO_FLOAT(xs);
		vr = (e->vr + fabsf(FIXED_TO_FLOAT(thing->spriteyoffset))) * sc * FIXED_TO_FLOAT(ys) + FIXED_TO_FLOAT(thing->height);
	}
	R = hr + vr + 0.05f * (float)abs(thing->dispoffset);
	if (hwr_fx_lerp)
		R += FIXED_TO_FLOAT(abs(thing->x - thing->old_x) + abs(thing->y - thing->old_y) + abs(thing->z - thing->old_z));
	return HWR_FX_SphereHidden(cs, X, Y, Z, R * 1.002f + 1.0f);
}
#endif

static void HWR_AddSprites(sector_t *sec)
{
	mobj_t *thing;
	precipmobj_t *precipthing;
	fixed_t limit_dist, hoop_limit_dist;

	// BSP is traversed by subsector.
	// A sector might have been split into several
	//  subsectors during BSP building.
	// Thus we check whether its already added.
	if (sec->validcount == validcount)
		return;

	// Well, now it will be done.
	sec->validcount = validcount;

	// sprite lighting
	sectorlight = sec->lightlevel & 0xff;

	// Handle all things in sector.
	// If a limit exists, handle things a tiny bit different.
	limit_dist = (fixed_t)(cv_drawdist.value) << FRACBITS;
	hoop_limit_dist = (fixed_t)(cv_drawdist_nights.value) << FRACBITS;
#ifdef PS2_PROFILE
	{
		// PS2-HW-240: a thing whose sprite cannot put a pixel on the screen (a sphere around it is outside the view volume) is not projected at all
		const ps2cull_t *cs = (ps2hwd_fx2 & FX2_NOPRE) ? NULL : PS2HWD_CullSetup();

		const boolean boxes = cv_renderhitbox.value != 0; // (R_ThingBoundingBoxVisible answers "no" for every thing when the hitboxes are off)

		if (cs && !cs->valid)
			cs = NULL;
		hwr_fx_cs = cs ? cs : PS2HWD_CullSetup();
		if (!hwr_fx_cs->valid)
			hwr_fx_cs = NULL;
		for (thing = sec->thinglist; thing; thing = thing->snext)
		{
			HWD_ADD(HWC_FX_THINGS);
			// (no draw distance set: the distance is not looked at; R_ThingWithinDist computes it for nothing)
			if ((thing->sprite == SPR_HOOP ? !hoop_limit_dist : !limit_dist) || R_ThingWithinDist(thing, limit_dist, hoop_limit_dist))
			{
				const boolean vis0 = (ps2hwd_fx2 & FX2_NOVIS) ? R_ThingVisible(thing) : HWR_FX_ThingVisible(thing);

				if ((ps2hwd_fx2 & FX2_PRECHECK) && vis0 != R_ThingVisible(thing))
					CONS_Printf("HWC visible MISMATCH\n");
				if (vis0)
				{
					HWD_ADD(HWC_FX_PROJ);
					if (thing->spriteroll || thing->old_spriteroll)
						HWD_ADD(HWC_FX_ROLL); // (measurement: the things the sphere test leaves out because their picture is rotated)
					if (thing->skin || (thing->flags2 & MF2_LINKDRAW) || (thing->frame & (FF_PAPERSPRITE | FF_FLOORSPRITE)) || (thing->renderflags & (RF_PAPERSPRITE | RF_FLOORSPRITE | RF_ABSOLUTEOFFSETS | RF_SHADOWEFFECTS)) || thing->type == MT_OVERLAY || (cv_shadow.value && thing->shadowscale))
						HWD_ADD(HWC_FX_NOEXT);
					if (cs && HWR_FX_ThingHidden(thing, cs))
					{
						HWD_ADD(HWC_FX_PRE);
						if (ps2hwd_fx2 & FX2_PRECHECK) // check mode: project it all the same; a sprite that comes out of it is a mistake of the test
						{
							const UINT32 n0 = gl_visspritecount;
							static unsigned chk, bad;

							HWR_ProjectSprite(thing);
							chk++;
							if (gl_visspritecount != n0)
							{
								bad++;
								CONS_Printf("HWC pre MISMATCH %u of %u (sprite %s)\n", bad, chk, (UINT32)thing->sprite < NUMSPRITES ? sprnames[thing->sprite] : "?");
							}
							else if (!(chk & 1023))
							{
								CONS_Printf("HWC pre check: %u things, %u differ\n", chk, bad);
							}
						}
					}
					else if (hwr_fx_plain && (ps2hwd_fx2 & FX2_PRECHECK))
						HWR_FX_ProjectChecked(thing);
					else if (!(hwr_fx_plain && HWR_ProjectPlain(thing)))
						HWR_ProjectSprite(thing);
				}

				if (boxes)
					HWR_ProjectBoundingBox(thing);
			}
		}
	}
#else
	for (thing = sec->thinglist; thing; thing = thing->snext)
	{
		if (R_ThingWithinDist(thing, limit_dist, hoop_limit_dist))
		{
			if (R_ThingVisible(thing))
			{
				HWR_ProjectSprite(thing);
			}

			HWR_ProjectBoundingBox(thing);
		}
	}
#endif

	// no, no infinite draw distance for precipitation. this option at zero is supposed to turn it off
	if ((limit_dist = (fixed_t)cv_drawdist_precip.value << FRACBITS))
	{
		for (precipthing = sec->preciplist; precipthing; precipthing = precipthing->snext)
		{
			if (R_PrecipThingVisible(precipthing, limit_dist))
				HWR_ProjectPrecipitationSprite(precipthing);
		}
	}
}

#ifdef PS2_PROFILE
// PS2-HW-72: a sprite whose quad cannot put a pixel on the screen is not made (the quad is the one HWR_DrawSprite builds; see PS2HWD_QuadHidden)
static boolean HWR_PS2_NoCull(void) // -hwnocull: every sprite is made (A/B of PS2-HW-72)
{
	static int nocull = -1;

	if (nocull < 0)
		nocull = M_CheckParm("-hwnocull") != 0;
	return nocull;
}

// the quad HWR_DrawSprite would hand to HWR_ProcessPolygon for a sprite (x1..x2 / z1..z2 along the view, gz..gzt in height, the display offset, and for a view
// that looks up or down the turn around the foot of HWR_RotateSpritePolyToAim: basey is the height of the foot, the same formulas)
static boolean HWR_PS2_SpriteHiddenOld(float x1, float x2, float z1, float z2, float gz, float gzt, INT32 dispoffset, float basey, boolean aim)
{
	FOutVector wv[4];

	wv[0].x = wv[3].x = x1;
	wv[2].x = wv[1].x = x2;
	wv[2].y = wv[3].y = gzt;
	wv[0].y = wv[1].y = gz;
	wv[0].z = wv[3].z = z1;
	wv[1].z = wv[2].z = z2;
	wv[0].s = wv[1].s = wv[2].s = wv[3].s = 0.0f;
	wv[0].t = wv[1].t = wv[2].t = wv[3].t = 0.0f;
	if (dispoffset)
	{
		float co = -gl_viewcos*(0.05f*dispoffset);
		float si = -gl_viewsin*(0.05f*dispoffset);
		wv[0].z = wv[3].z = wv[0].z+si;
		wv[1].z = wv[2].z = wv[1].z+si;
		wv[0].x = wv[3].x = wv[0].x+co;
		wv[1].x = wv[2].x = wv[1].x+co;
	}
	if (aim)
	{
		float lowy = wv[0].y;

		wv[2].y = wv[3].y = (gzt - basey) * gl_viewludsin + basey;
		wv[0].y = wv[1].y = (lowy - basey) * gl_viewludsin + basey;
		wv[3].x += ((gzt - basey) * gl_viewludcos) * gl_viewcos;
		wv[2].x += ((gzt - basey) * gl_viewludcos) * gl_viewcos;
		wv[0].x += ((lowy - basey) * gl_viewludcos) * gl_viewcos;
		wv[1].x += ((lowy - basey) * gl_viewludcos) * gl_viewcos;
		wv[3].z += ((gzt - basey) * gl_viewludcos) * gl_viewsin;
		wv[2].z += ((gzt - basey) * gl_viewludcos) * gl_viewsin;
		wv[0].z += ((lowy - basey) * gl_viewludcos) * gl_viewsin;
		wv[1].z += ((lowy - basey) * gl_viewludcos) * gl_viewsin;
	}
	return PS2HWD_QuadHidden(wv) != 0;
}

// PS2-HW-251 (OPT11 round 3, FX3): the same quad as a parallelogram (the corners are P0, P0 + R, P0 + R + U, P0 + U: the display offset moves all of them, the turn around the foot
// of HWR_RotateSpritePolyToAim adds (height - basey) * (cos, sin, ...) to the upper and lower pair), tested from the rows of the transform (PS2HWD_ParaHidden).
// -hwfx 16384 = the four transforms of PS2HWD_QuadHidden; -hwfx 2 (check mode) asks both and reports a difference (HWC para MISMATCH).
static boolean HWR_PS2_SpriteHidden(float x1, float x2, float z1, float z2, float gz, float gzt, INT32 dispoffset, float basey, boolean aim)
{
	const ps2cull_t *cs;

	if (ps2hwd_fx2 & FX3_NOPARA)
		return HWR_PS2_SpriteHiddenOld(x1, x2, z1, z2, gz, gzt, dispoffset, basey, aim);
	cs = PS2HWD_CullSetup();
	if (!cs->valid)
		return HWR_PS2_SpriteHiddenOld(x1, x2, z1, z2, gz, gzt, dispoffset, basey, aim);
	{
		float p0[3], r[3], u[3], co = 0.0f, si = 0.0f;
		boolean h;

		if (dispoffset)
		{
			co = -gl_viewcos * (0.05f * dispoffset);
			si = -gl_viewsin * (0.05f * dispoffset);
		}
		if (aim)
		{
			const float lo = gz - basey, hi = gzt - basey;

			p0[0] = x1 + co + (lo * gl_viewludcos) * gl_viewcos;
			p0[1] = lo * gl_viewludsin + basey;
			p0[2] = z1 + si + (lo * gl_viewludcos) * gl_viewsin;
			u[0] = ((hi * gl_viewludcos) * gl_viewcos) - ((lo * gl_viewludcos) * gl_viewcos);
			u[1] = hi * gl_viewludsin - lo * gl_viewludsin;
			u[2] = ((hi * gl_viewludcos) * gl_viewsin) - ((lo * gl_viewludcos) * gl_viewsin);
		}
		else
		{
			p0[0] = x1 + co;
			p0[1] = gz;
			p0[2] = z1 + si;
			u[0] = 0.0f;
			u[1] = gzt - gz;
			u[2] = 0.0f;
		}
		r[0] = x2 - x1;
		r[1] = 0.0f;
		r[2] = z2 - z1;
		h = PS2HWD_ParaHidden(cs, p0, r, u) != 0;
		if (ps2hwd_fx2 & FX2_PRECHECK)
		{
			static unsigned chk, bad;
			const boolean o = HWR_PS2_SpriteHiddenOld(x1, x2, z1, z2, gz, gzt, dispoffset, basey, aim);

			chk++;
			if (o != h)
			{
				bad++;
				if (bad <= 20)
					CONS_Printf("HWC para MISMATCH %u of %u: new %d old %d (x %.3f..%.3f z %.3f..%.3f gz %.3f gzt %.3f disp %d aim %d)\n", bad, chk, (int)h, (int)o, x1, x2, z1, z2, gz, gzt, (int)dispoffset, (int)aim);
			}
			else if (!(chk & 4095))
			{
				CONS_Printf("HWC para check: %u quads, %u differ\n", chk, bad);
			}
		}
		return h;
	}
}
#endif

#ifdef PS2_PROFILE
// --------------------------------------------------------------------------
// PS2-HW-257 (OPT11 round 3, FX3): what the sprite stream makes cheaper in the projection of a thing (-hwfx 1048576 = FX3_NOLEAN2: as before)
//  * the exact test of the quad (PS2HWD_ParaHidden: no pixel centre inside it, 600 cycles) is replaced by the sphere test of the view (60 cycles): a sprite the VU1 program throws out for
//    nothing costs the EE a record (the sprite is not drawn either way: the stream draws what the GS rasterises, and the GS draws nothing for a quad without a pixel centre);
//  * the patch of a sprite lump is looked up once in a view (W_CachePatchNum: the tag of the block is changed once, the pointer is kept for the other sprites of the lump);
//  * the colour map of a thing that has none (no skin colour, no translation, not a player sprite, not flashing) is not asked for.
// --------------------------------------------------------------------------
static inline boolean HWR_FX_SpriteHiddenCheap(float x1, float x2, float z1, float z2, float gz, float gzt, INT32 dispoffset, float basey)
{
	float R, lo, hi;

	if (!hwr_fx_cs)
		return false;
	// a sphere around the quad that holds every corner of it, whatever the aim rotation (about the foot at basey) and the display offset do to the corners
	lo = fabsf(gz - basey);
	hi = fabsf(gzt - basey);
	R = 0.5f * (fabsf(x2 - x1) + fabsf(z2 - z1)) + 2.0f * (lo > hi ? lo : hi) + 0.05f * (float)abs(dispoffset);
	return HWR_FX_SphereHidden(hwr_fx_cs, 0.5f * (x1 + x2), 0.5f * (gz + gzt), 0.5f * (z1 + z2), R * 1.002f + 1.0f);
}

// PS2-HW-257c: the sphere test lets through a sprite whose quad holds no pixel centre; the VU1 program throws it out, which costs the EE one record. It costs more when the
// picture of the sprite is not in the GS pool yet: the old path then makes the data of the patch and uploads it for a sprite that is not drawn (D2 from the window 7: 52
// such sprites a frame, the GS pool and the zone thrashed, the frames 1.55 times longer). Those take the exact test, as before: the set of pictures made resident is the old one.
static inline boolean HWR_FX_SpriteResident(lumpnum_t ln)
{
	const patch_t *p = (const patch_t *)W_PeekPatchNum(ln);
	const GLPatch_t *gp;

	if (!p || !p->hardware)
		return false;
	gp = (const GLPatch_t *)p->hardware;
	return gp->mipmap && gp->mipmap->downloaded;
}

static inline patch_t *HWR_FX_SpritePatch(lumpnum_t ln)
{
	const UINT32 h = (UINT32)(ln ^ (ln >> 8)) & 255u;

	if (hwr_fx_pc[h].view == hwr_fx_view && hwr_fx_pc[h].ln == ln && hwr_fx_pc[h].p)
		return hwr_fx_pc[h].p;
	{
		patch_t *p = (patch_t *)W_CachePatchNum(ln, PU_SPRITE);

		hwr_fx_pc[h].view = hwr_fx_view;
		hwr_fx_pc[h].ln = ln;
		hwr_fx_pc[h].p = p;
		return p;
	}
}

static inline UINT8 *HWR_FX_ThingTranslation(mobj_t *thing)
{
	if (thing->color == SKINCOLOR_NONE && thing->translation == 0 && thing->sprite != SPR_PLAY && !R_ThingIsFlashing(thing))
		return NULL; // (what R_GetTranslationForThing ends with for such a thing)
	return R_GetTranslationForThing(thing, thing->color, thing->translation);
}

// --------------------------------------------------------------------------
// PS2-HW-254 (OPT11 round 3, FX3): HWR_ProjectSprite for the plain sprite: no skin, no overlay, no link draw, no floor or paper sprite, no roll, no absolute offsets, no shadow
// effects, no model. What the full function does for such a thing, in the same arithmetic and the same order of the checks, without what it is there for the others (the
// skin and rotation sprite paths, the tracer of a link draw, the floor sprite, the caster of a shadow effect), with the per-view values made once (HWR_ClearSprites).
// Returns false (nothing was done) for any other thing: HWR_ProjectSprite then makes it. -hwfx 131072 (FX3_NOPLAIN): always the full function.
// --------------------------------------------------------------------------
static boolean HWR_ProjectPlain(mobj_t *thing)
{
	gl_vissprite_t *vis;
	interpmobjstate_t interp;
	spritedef_t *sprdef;
	spriteframe_t *sprframe;
	size_t lumpoff;
	unsigned rot;
	UINT16 flip;
	float tr_x, tr_y, tz, x1, x2, z1, z2, gz, gzt;
	float this_scale, spritexscale, spriteyscale, this_xscale, this_yscale;
	fixed_t spr_width, spr_height, spr_offset, spr_topoffset;
	INT32 dispoffset, heightsec, phs;
	boolean vflip, hflip;
	const boolean lerp = hwr_fx_lerp && !(ps2hwd_fx2 & FX2_NOINTERP);
	const INT32 frame = thing->frame;
	HWD_LOCAL; // the laps of HWPROF33 as HWR_ProjectSprite has them (detail builds)

	if (thing->skin || thing->type == MT_OVERLAY || (thing->flags2 & (MF2_LINKDRAW | MF2_SPLAT)) || (frame & (FF_PAPERSPRITE | FF_FLOORSPRITE))
		|| (thing->renderflags & (RF_PAPERSPRITE | RF_FLOORSPRITE | RF_ABSOLUTEOFFSETS | RF_SHADOWEFFECTS)) || thing->spriteroll || thing->old_spriteroll)
		return false;
	if ((UINT32)thing->sprite >= (UINT32)numsprites)
		return false;
	sprdef = &sprites[thing->sprite];
	rot = (unsigned)(frame & FF_FRAMEMASK);
	if (rot >= sprdef->numframes)
		return false; // (the error path of the full function)
	sprframe = &sprdef->spriteframes[rot];

	// the visibility by the blend mode
	if (frame & FF_TRANSMASK)
	{
		const INT32 blendmode = (frame & FF_BLENDMASK) ? ((frame & FF_BLENDMASK) >> FF_BLENDSHIFT) + 1 : thing->blendmode;

		if (!R_BlendLevelVisible(blendmode, (frame & FF_TRANSMASK) >> FF_TRANSSHIFT))
			return true;
	}
	dispoffset = thing->dispoffset;

	if (lerp)
	{
		R_InterpolateMobjStateLite(thing, rendertimefrac, &interp);
	}
	else if (hwr_fx_interp && rendertimefrac != FRACUNIT)
	{
		R_InterpolateMobjState(thing, rendertimefrac, &interp);
	}
	else
	{
		interp.x = thing->x;
		interp.y = thing->y;
		interp.z = thing->z;
		interp.scale = thing->scale;
		interp.radius = thing->radius;
		interp.height = thing->height;
		interp.subsector = thing->subsector;
		interp.angle = thing->player ? thing->player->drawangle : thing->angle;
		interp.spritexscale = thing->spritexscale;
		interp.spriteyscale = thing->spriteyscale;
		interp.spritexoffset = thing->spritexoffset;
		interp.spriteyoffset = thing->spriteyoffset;
	}
	HWD_LAP(HWP_PS_A);
	if (interp.spritexscale < 1 || interp.spriteyscale < 1)
		return true;

	this_scale = FIXED_TO_FLOAT(interp.scale);
	spritexscale = FIXED_TO_FLOAT(interp.spritexscale);
	spriteyscale = FIXED_TO_FLOAT(interp.spriteyscale);

	tr_x = FIXED_TO_FLOAT(interp.x) - gl_viewx;
	tr_y = FIXED_TO_FLOAT(interp.y) - gl_viewy;
	tz = (tr_x * gl_viewcos) + (tr_y * gl_viewsin);
	if (tz < ZCLIP_PLANE)
	{
		HWD_ADD(HWC_FX_BEHIND);
		return true;
	}
	tr_x = FIXED_TO_FLOAT(interp.x);
	tr_y = FIXED_TO_FLOAT(interp.y);

	vflip = (!(thing->eflags & MFE_VERTICALFLIP) != !R_ThingVerticallyFlipped(thing));
	hflip = (!R_ThingHorizontallyFlipped(thing) != !thing->mirrored);

	if (sprframe->rotate == SRF_SINGLE)
	{
		rot = 0;
		lumpoff = sprframe->lumpid[0];
		flip = sprframe->flip;
	}
	else
	{
		angle_t ang = R_PointToAngle(interp.x, interp.y) - interp.angle;

		if (thing->mirrored)
			ang = InvAngle(ang);
		if ((sprframe->rotate & SRF_RIGHT) && (ang < ANGLE_180))
			rot = 6;
		else if ((sprframe->rotate & SRF_LEFT) && (ang >= ANGLE_180))
			rot = 2;
		else if (sprframe->rotate & SRF_3DGE)
		{
			rot = (ang + ANGLE_180 + ANGLE_11hh) >> 28;
			rot = ((rot & 1) << 3) | (rot >> 1);
		}
		else
			rot = (ang + ANGLE_202h) >> 29;
		lumpoff = sprframe->lumpid[rot];
		flip = sprframe->flip & (1 << rot);
	}

	HWD_LAP(HWP_PS_B);
	spr_width = spritecachedinfo[lumpoff].width;
	spr_height = spritecachedinfo[lumpoff].height;
	spr_offset = spritecachedinfo[lumpoff].offset;
	spr_topoffset = spritecachedinfo[lumpoff].topoffset;
	{
		SINT8 flipoffset = 1;

		if ((thing->renderflags & RF_FLIPOFFSETS) && flip)
			flipoffset = -1;
		spr_offset += interp.spritexoffset * flipoffset;
		spr_topoffset += interp.spriteyoffset * flipoffset;
	}
	flip = !flip != !hflip;

	this_xscale = spritexscale * this_scale;
	this_yscale = spriteyscale * this_scale;
	if (flip)
	{
		x1 = (FIXED_TO_FLOAT(spr_width - spr_offset) * this_xscale);
		x2 = (FIXED_TO_FLOAT(spr_offset) * this_xscale);
	}
	else
	{
		x1 = (FIXED_TO_FLOAT(spr_offset) * this_xscale);
		x2 = (FIXED_TO_FLOAT(spr_width - spr_offset) * this_xscale);
	}
	z1 = tr_y + x1 * hwr_fx_rsin;
	z2 = tr_y - x2 * hwr_fx_rsin;
	x1 = tr_x + x1 * hwr_fx_rcos;
	x2 = tr_x - x2 * hwr_fx_rcos;
	if (vflip)
	{
		gz = FIXED_TO_FLOAT(interp.z + interp.height) - (FIXED_TO_FLOAT(spr_topoffset) * this_yscale);
		gzt = gz + (FIXED_TO_FLOAT(spr_height) * this_yscale);
	}
	else
	{
		gzt = FIXED_TO_FLOAT(interp.z) + (FIXED_TO_FLOAT(spr_topoffset) * this_yscale);
		gz = gzt - (FIXED_TO_FLOAT(spr_height) * this_yscale);
	}

	HWD_LAP(HWP_PS_C);
	if (thing->subsector->sector->cullheight)
	{
		if (HWR_DoCulling(thing->subsector->sector->cullheight, viewsector->cullheight, gl_viewz, gz, gzt))
			return true;
	}
	heightsec = thing->subsector->sector->heightsec;
	if (viewplayer->mo && viewplayer->mo->subsector)
		phs = viewplayer->mo->subsector->sector->heightsec;
	else
		phs = -1;
	if (heightsec != -1 && phs != -1) // only clip things which are in special sectors
	{
		const float top = gzt;
		const float bottom = FIXED_TO_FLOAT(interp.z);

		if (gl_viewz < FIXED_TO_FLOAT(sectors[phs].floorheight) ?
		bottom >= FIXED_TO_FLOAT(sectors[heightsec].floorheight) :
		top < FIXED_TO_FLOAT(sectors[heightsec].floorheight))
			return true;
		if (gl_viewz > FIXED_TO_FLOAT(sectors[phs].ceilingheight) ?
		top < FIXED_TO_FLOAT(sectors[heightsec].ceilingheight) && gl_viewz >= FIXED_TO_FLOAT(sectors[heightsec].ceilingheight) :
		bottom >= FIXED_TO_FLOAT(sectors[heightsec].ceilingheight))
			return true;
	}

	HWD_LAP(HWP_PS_D);
	{
		UINT8 ps2_hidden = 0;

		// PS2-HW-72: no pixel centre inside the quad (or the quad outside the view): nothing would be drawn, nothing is made of it
		if (!(cv_shadow.value && thing->shadowscale) && !HWR_PS2_NoCull())
		{
			const boolean aim = cv_glspritebillboarding.value && fabsf(gl_viewludcos) > 1.0e-6f;
			const float basey = P_MobjFlip(thing) == -1 ? FIXED_TO_FLOAT(interp.z + interp.height) : FIXED_TO_FLOAT(interp.z);

			if ((hwr_fx_cheap && !(ps2hwd_fx2 & (FX3_NOSPR | FX3_NOSPHERE))) ? (HWR_FX_SpriteHiddenCheap(x1, x2, z1, z2, gz, gzt, dispoffset, basey) || (!HWR_FX_SpriteResident(sprframe->lumppat[rot]) && HWR_PS2_SpriteHidden(x1, x2, z1, z2, gz, gzt, dispoffset, basey, aim))) : HWR_PS2_SpriteHidden(x1, x2, z1, z2, gz, gzt, dispoffset, basey, aim))
			{
				HWD_ADD(HWC_FX_QHID);
				if (!(ps2hwd_dbg_flags & 0x1000000))
					return true;
				ps2_hidden = 1;
			}
		}
		HWD_LAP(HWP_PS_E);
		HWD_ADD(HWC_FX_VIS);
		vis = HWR_NewVisSprite();
		vis->ps2_hid = ps2_hidden;
	}
	vis->ps2_skey = (UINT8)((((thing->flags2 & MF2_SHADOW) || (frame & FF_TRANSMASK)) ? 1 : 0) | 4); // (bit 2: made by HWR_ProjectPlain: no skin, no link draw, no model, no floor or paper sprite)
	vis->ps2_iok = !(ps2hwd_fx2 & FX2_NOINTERP);
	vis->ps2_ix = interp.x;
	vis->ps2_iy = interp.y;
	vis->ps2_iz = interp.z;
	vis->ps2_ih = interp.height;
	vis->ps2_ir = interp.radius;
	vis->ps2_isub = interp.subsector;
	vis->x1 = x1;
	vis->x2 = x2;
	vis->z1 = z1;
	vis->z2 = z2;
	vis->tz = tz;
	vis->tracertz = 0.0f;
	vis->renderflags = thing->renderflags;
	vis->rotateflags = sprframe->rotate;
	vis->shadowheight = 1.0f;
	vis->shadowscale = 1.0f;
	vis->dispoffset = dispoffset;
	vis->flip = flip;
	vis->scale = this_scale;
	vis->spritexscale = spritexscale;
	vis->spriteyscale = spriteyscale;
	vis->spritexoffset = FIXED_TO_FLOAT(spr_offset);
	vis->spriteyoffset = FIXED_TO_FLOAT(spr_topoffset);
	vis->rotated = false;
	vis->gpatch = (hwr_fx_cheap && !(ps2hwd_fx2 & FX3_NOPCACHE)) ? HWR_FX_SpritePatch(sprframe->lumppat[rot]) : (patch_t *)W_CachePatchNum(sprframe->lumppat[rot], PU_SPRITE);
	vis->mobj = thing;
	vis->colormap = hwr_fx_cheap ? HWR_FX_ThingTranslation(thing) : R_GetTranslationForThing(thing, thing->color, thing->translation);
	vis->gzt = gzt;
	vis->gz = gz;
	vis->vflip = vflip;
	vis->precip = false;
	vis->bbox = false;
	vis->angle = interp.angle;
	HWD_LAP(HWP_PS_F);
	return true;
}
#endif

#ifdef PS2_PROFILE
// check mode (-hwfx 2): the plain projection and the full one make the same vissprite (or both none) for the same thing; the full one's is kept
static void HWR_FX_ProjectChecked(mobj_t *thing)
{
	static unsigned chk, bad;
	const UINT32 n0 = gl_visspritecount;
	gl_vissprite_t a;
	boolean made = false, handled;

	handled = HWR_ProjectPlain(thing);
	if (handled && gl_visspritecount > n0)
	{
		a = *HWR_GetVisSprite(n0);
		made = true;
	}
	gl_visspritecount = n0;
	HWR_ProjectSprite(thing);
	if (!handled)
		return;
	chk++;
	{
		const boolean made2 = gl_visspritecount > n0;
		boolean same = made == made2;

		if (same && made)
		{
			const gl_vissprite_t *b = HWR_GetVisSprite(n0);

#define FXC(f) (same = same && !memcmp(&a.f, &b->f, sizeof a.f))
			FXC(x1); FXC(x2); FXC(z1); FXC(z2); FXC(tz); FXC(tracertz); FXC(renderflags); FXC(rotateflags); FXC(shadowheight); FXC(shadowscale); FXC(dispoffset); FXC(flip);
			FXC(scale); FXC(spritexscale); FXC(spriteyscale); FXC(spritexoffset); FXC(spriteyoffset); FXC(rotated); FXC(gpatch); FXC(mobj); FXC(colormap); FXC(gzt); FXC(gz);
			FXC(vflip); FXC(precip); FXC(bbox); FXC(angle); FXC(ps2_hid); same = same && ((a.ps2_skey & 3) == (b->ps2_skey & 3)); FXC(ps2_iok); FXC(ps2_ix); FXC(ps2_iy); FXC(ps2_iz); FXC(ps2_ih); FXC(ps2_ir); FXC(ps2_isub);
#undef FXC
		}
		if (!same)
		{
			bad++;
			if (bad <= 20)
				CONS_Printf("HWC plain MISMATCH %u of %u (sprite %s, made %d / %d)\n", bad, chk, (UINT32)thing->sprite < NUMSPRITES ? sprnames[thing->sprite] : "?", (int)made, (int)made2);
		}
		else if (!(chk & 4095))
		{
			CONS_Printf("HWC plain check: %u things, %u differ\n", chk, bad);
		}
	}
}
#endif

// --------------------------------------------------------------------------
// HWR_ProjectSprite
//  Generates a vissprite for a thing if it might be visible.
// --------------------------------------------------------------------------
// BP why not use xtoviexangle/viewangletox like in bsp ?....
static void HWR_ProjectSprite(mobj_t *thing)
{
	gl_vissprite_t *vis;
	float tr_x, tr_y;
	float tz;
	float tracertz = 0.0f;
	float x1, x2;
	float rightsin, rightcos;
	float this_scale, this_xscale, this_yscale;
	float spritexscale, spriteyscale;
	float shadowheight = 1.0f, shadowscale = 1.0f;
	float gz, gzt;
	spritedef_t *sprdef;
	spriteframe_t *sprframe;
#ifdef ROTSPRITE
	spriteinfo_t *sprinfo;
#endif
	md2_t *md2;
	size_t lumpoff;
	unsigned rot;
	UINT16 flip;
	boolean vflip = (!(thing->eflags & MFE_VERTICALFLIP) != !R_ThingVerticallyFlipped(thing));
	boolean mirrored = thing->mirrored;
	boolean hflip = (!R_ThingHorizontallyFlipped(thing) != !mirrored);
	skincolornum_t color;
	UINT16 translation;
	INT32 dispoffset;

	angle_t ang;
	INT32 heightsec, phs;
	const boolean splat = R_ThingIsFloorSprite(thing);
	const boolean papersprite = (R_ThingIsPaperSprite(thing) && !splat);
	float z1, z2;

	fixed_t spr_width, spr_height;
	fixed_t spr_offset, spr_topoffset;
#ifdef ROTSPRITE
	patch_t *rotsprite = NULL;
	INT32 rollangle = 0;
	angle_t spriterotangle = 0;
#endif

	// uncapped/interpolation
	interpmobjstate_t interp = {0};

	HWD_LOCAL; // OPT11: the parts of this function (HWPROF33, only with --hwdetail)

	if (!r_renderthings)
		return;

	if (!thing)
		return;

	INT32 blendmode;
	if (thing->frame & FF_BLENDMASK)
		blendmode = ((thing->frame & FF_BLENDMASK) >> FF_BLENDSHIFT) + 1;
	else
		blendmode = thing->blendmode;

	// Visibility check by the blend mode.
	if (thing->frame & FF_TRANSMASK)
	{
		if (!R_BlendLevelVisible(blendmode, (thing->frame & FF_TRANSMASK)>>FF_TRANSSHIFT))
			return;
	}

	dispoffset = thing->dispoffset;


#ifdef PS2_PROFILE
	if (hwr_fx_lerp && !(ps2hwd_fx2 & FX2_NOINTERP)) // PS2-HW-245: between two tics, without the subsector and the angles the sprite does not use (what the full interpolation makes of them is 400 cycles)
	{
		R_InterpolateMobjStateLite(thing, rendertimefrac, &interp);
	}
	else
#endif
	if (HWR_USING_INTERP())
	{
		R_InterpolateMobjState(thing, rendertimefrac, &interp);
	}
	else
	{
		R_InterpolateMobjState(thing, FRACUNIT, &interp);
	}

	HWD_LAP(HWP_PS_A);
	if (interp.spritexscale < 1 || interp.spriteyscale < 1)
		return;

	this_scale = FIXED_TO_FLOAT(interp.scale);
	spritexscale = FIXED_TO_FLOAT(interp.spritexscale);
	spriteyscale = FIXED_TO_FLOAT(interp.spriteyscale);

	// transform the origin point
	if (thing->type == MT_OVERLAY) // Handle overlays
		R_ThingOffsetOverlay(thing, &interp.x, &interp.y);
	tr_x = FIXED_TO_FLOAT(interp.x) - gl_viewx;
	tr_y = FIXED_TO_FLOAT(interp.y) - gl_viewy;

	// rotation around vertical axis
	tz = (tr_x * gl_viewcos) + (tr_y * gl_viewsin);

	// thing is behind view plane?
	if (tz < ZCLIP_PLANE && !(papersprite || splat))
	{
		if (cv_glmodels.value) //Yellow: Only MD2's dont disappear
		{
			if (thing->skin && thing->sprite == SPR_PLAY)
				md2 = &md2_playermodels[((skin_t *)thing->skin)->skinnum];
			else
				md2 = &md2_models[thing->sprite];

			if (!md2->found || md2->scale < 0.0f)
				return;
		}
		else
		{
			HWD_ADD(HWC_FX_BEHIND);
			return;
		}
	}

	// The above can stay as it works for cutting sprites that are too close
	tr_x = FIXED_TO_FLOAT(interp.x);
	tr_y = FIXED_TO_FLOAT(interp.y);

	// decide which patch to use for sprite relative to player
#ifdef RANGECHECK
	if ((unsigned)thing->sprite >= numsprites)
		I_Error("HWR_ProjectSprite: invalid sprite number %i ", thing->sprite);
#endif

	rot = thing->frame&FF_FRAMEMASK;

	//Fab : 02-08-98: 'skin' override spritedef currently used for skin
	if (thing->skin && thing->sprite == SPR_PLAY)
	{
		sprdef = P_GetSkinSpritedef(thing->skin, thing->sprite2);
#ifdef ROTSPRITE
		sprinfo = P_GetSkinSpriteInfo(thing->skin, thing->sprite2);
#endif

		if (rot >= sprdef->numframes)
		{
			CONS_Alert(CONS_ERROR, M_GetText("HWR_ProjectSprite: invalid skins[\"%s\"].sprites[SPR2_%s] %sframe %s\n"),
				((skin_t *)thing->skin)->name, spr2names[thing->sprite2 & SPR2F_MASK], (thing->sprite2 & SPR2F_SUPER) ? "super ": "", sizeu5(rot));
			thing->sprite = states[S_UNKNOWN].sprite;
			thing->frame = states[S_UNKNOWN].frame;
			sprdef = &sprites[thing->sprite];
#ifdef ROTSPRITE
			sprinfo = &spriteinfo[thing->sprite];
#endif
			rot = thing->frame&FF_FRAMEMASK;
		}
	}
	else
	{
		sprdef = &sprites[thing->sprite];
#ifdef ROTSPRITE
		sprinfo = &spriteinfo[thing->sprite];
#endif

		if (rot >= sprdef->numframes)
		{
			CONS_Alert(CONS_ERROR, M_GetText("HWR_ProjectSprite: invalid sprite frame %s/%s for %s\n"),
				sizeu1(rot), sizeu2(sprdef->numframes), sprnames[thing->sprite]);
			if (thing->sprite == thing->state->sprite && thing->frame == thing->state->frame)
			{
				thing->state->sprite = states[S_UNKNOWN].sprite;
				thing->state->frame = states[S_UNKNOWN].frame;
			}
			thing->sprite = states[S_UNKNOWN].sprite;
			thing->frame = states[S_UNKNOWN].frame;
			sprdef = &sprites[thing->sprite];
			sprinfo = &spriteinfo[thing->sprite];
			rot = thing->frame&FF_FRAMEMASK;
		}

	}
	sprframe = &sprdef->spriteframes[rot];

#ifdef PARANOIA
	if (!sprframe)
		I_Error("sprframes NULL for sprite %d\n", thing->sprite);
#endif

	if (splat)
	{
		ang = R_PointToAngle2(0, viewz, 0, interp.z);
	}
#ifdef PS2_PROFILE
	else if (!papersprite && sprframe->rotate == SRF_SINGLE)
	{
		ang = 0; // one picture for every view: the angle to the viewer is not used (R_PointToAngle64 is 0.2 M cycles for 1300 sprites)
	}
#endif
	else
	{
		ang = R_PointToAngle (interp.x, interp.y) - interp.angle;
		if (mirrored)
			ang = InvAngle(ang);
	}

	if (sprframe->rotate == SRF_SINGLE)
	{
		// use single rotation for all views
		rot = 0;                        //Fab: for vis->patch below
		lumpoff = sprframe->lumpid[0];     //Fab: see note above
		flip = sprframe->flip; // Will only be 0x00 or 0xFF

		if (papersprite && ang < ANGLE_180)
			flip ^= 0xFFFF;
	}
	else
	{
		// choose a different rotation based on player view
		if ((sprframe->rotate & SRF_RIGHT) && (ang < ANGLE_180)) // See from right
			rot = 6; // F7 slot
		else if ((sprframe->rotate & SRF_LEFT) && (ang >= ANGLE_180)) // See from left
			rot = 2; // F3 slot
		else if (sprframe->rotate & SRF_3DGE) // 16-angle mode
		{
			rot = (ang+ANGLE_180+ANGLE_11hh)>>28;
			rot = ((rot & 1)<<3)|(rot>>1);
		}
		else // Normal behaviour
			rot = (ang+ANGLE_202h)>>29;

		//Fab: lumpid is the index for spritewidth,spriteoffset... tables
		lumpoff = sprframe->lumpid[rot];
		flip = sprframe->flip & (1<<rot);

		if (papersprite && ang < ANGLE_180)
			flip ^= (1<<rot);
	}

	HWD_LAP(HWP_PS_B);
	if (thing->skin && ((skin_t *)thing->skin)->flags & SF_HIRES)
		this_scale *= FIXED_TO_FLOAT(((skin_t *)thing->skin)->highresscale);

	spr_width = spritecachedinfo[lumpoff].width;
	spr_height = spritecachedinfo[lumpoff].height;
	spr_offset = spritecachedinfo[lumpoff].offset;
	spr_topoffset = spritecachedinfo[lumpoff].topoffset;

#ifdef ROTSPRITE
	spriterotangle = R_SpriteRotationAngle(&interp);

	if (spriterotangle != 0
	&& !(splat && !(thing->renderflags & RF_NOSPLATROLLANGLE)))
	{
		if (papersprite)
		{
			// a positive rollangle should should pitch papersprites upwards relative to their facing angle
			rollangle = R_GetRollAngle(InvAngle(spriterotangle));
		}
		else
		{
			rollangle = R_GetRollAngle(spriterotangle);
		}

		rotsprite = Patch_GetRotatedSprite(sprframe, (thing->frame & FF_FRAMEMASK), rot, flip, sprinfo, rollangle);

		if (rotsprite != NULL)
		{
			spr_width = rotsprite->width << FRACBITS;
			spr_height = rotsprite->height << FRACBITS;
			spr_offset = rotsprite->leftoffset << FRACBITS;
			spr_topoffset = rotsprite->topoffset << FRACBITS;
			spr_topoffset += FEETADJUST;

			// flip -> rotate, not rotate -> flip
			flip = 0;
		}
	}
#endif

	if (thing->renderflags & RF_ABSOLUTEOFFSETS)
	{
		spr_offset = interp.spritexoffset;
		spr_topoffset = interp.spriteyoffset;
	}
	else
	{
		SINT8 flipoffset = 1;

		if ((thing->renderflags & RF_FLIPOFFSETS) && flip)
			flipoffset = -1;

		spr_offset += interp.spritexoffset * flipoffset;
		spr_topoffset += interp.spriteyoffset * flipoffset;
	}

	if (papersprite)
	{
		rightsin = FIXED_TO_FLOAT(FINESINE(interp.angle >> ANGLETOFINESHIFT));
		rightcos = FIXED_TO_FLOAT(FINECOSINE(interp.angle >> ANGLETOFINESHIFT));
	}
	else
	{
		rightsin = FIXED_TO_FLOAT(FINESINE((viewangle + ANGLE_90)>>ANGLETOFINESHIFT));
		rightcos = FIXED_TO_FLOAT(FINECOSINE((viewangle + ANGLE_90)>>ANGLETOFINESHIFT));
	}

	flip = !flip != !hflip;

	if (thing->renderflags & RF_SHADOWEFFECTS)
	{
		mobj_t *caster = thing->target;

		if (caster && !P_MobjWasRemoved(caster))
		{
			interpmobjstate_t casterinterp = { 0 };
			fixed_t groundz;
			fixed_t floordiff;

			if (HWR_USING_INTERP())
			{
				R_InterpolateMobjState(caster, rendertimefrac, &casterinterp);
			}
			else
			{
				R_InterpolateMobjState(caster, FRACUNIT, &casterinterp);
			}

			groundz = R_GetShadowZ(thing, NULL);
			floordiff = abs(((thing->eflags & MFE_VERTICALFLIP) ? casterinterp.height : 0) + casterinterp.z - groundz);

			shadowheight = FIXED_TO_FLOAT(floordiff);
			shadowscale = FIXED_TO_FLOAT(FixedMul(FRACUNIT - floordiff/640, casterinterp.scale));

			if (splat)
				spritexscale *= shadowscale;
			spriteyscale *= shadowscale;
		}
	}

	this_xscale = spritexscale * this_scale;
	this_yscale = spriteyscale * this_scale;

	if (splat)
	{
		z1 = z2 = tr_y;
		x1 = x2 = tr_x;
		gz = gzt = interp.z;
	}
	else
	{
		if (flip)
		{
			x1 = (FIXED_TO_FLOAT(spr_width - spr_offset) * this_xscale);
			x2 = (FIXED_TO_FLOAT(spr_offset) * this_xscale);
		}
		else
		{
			x1 = (FIXED_TO_FLOAT(spr_offset) * this_xscale);
			x2 = (FIXED_TO_FLOAT(spr_width - spr_offset) * this_xscale);
		}

		// test if too close
	/*
		if (papersprite)
		{
			z1 = tz - x1 * angle_scalez;
			z2 = tz + x2 * angle_scalez;

			if (max(z1, z2) < ZCLIP_PLANE)
				return;
		}
	*/

		z1 = tr_y + x1 * rightsin;
		z2 = tr_y - x2 * rightsin;
		x1 = tr_x + x1 * rightcos;
		x2 = tr_x - x2 * rightcos;

		if (vflip)
		{
			gz = FIXED_TO_FLOAT(interp.z + interp.height) - (FIXED_TO_FLOAT(spr_topoffset) * this_yscale);
			gzt = gz + (FIXED_TO_FLOAT(spr_height) * this_yscale);
		}
		else
		{
			gzt = FIXED_TO_FLOAT(interp.z) + (FIXED_TO_FLOAT(spr_topoffset) * this_yscale);
			gz = gzt - (FIXED_TO_FLOAT(spr_height) * this_yscale);
		}
	}

	HWD_LAP(HWP_PS_C);
	if (thing->subsector->sector->cullheight)
	{
		if (HWR_DoCulling(thing->subsector->sector->cullheight, viewsector->cullheight, gl_viewz, gz, gzt))
			return;
	}

	heightsec = thing->subsector->sector->heightsec;
	if (viewplayer->mo && viewplayer->mo->subsector)
		phs = viewplayer->mo->subsector->sector->heightsec;
	else
		phs = -1;

	if (heightsec != -1 && phs != -1) // only clip things which are in special sectors
	{
		float top = gzt;
		float bottom = FIXED_TO_FLOAT(interp.z);

		if (R_ThingIsFloorSprite(thing))
			top = bottom;

		if (gl_viewz < FIXED_TO_FLOAT(sectors[phs].floorheight) ?
		bottom >= FIXED_TO_FLOAT(sectors[heightsec].floorheight) :
		top < FIXED_TO_FLOAT(sectors[heightsec].floorheight))
			return;
		if (gl_viewz > FIXED_TO_FLOAT(sectors[phs].ceilingheight) ?
		top < FIXED_TO_FLOAT(sectors[heightsec].ceilingheight) && gl_viewz >= FIXED_TO_FLOAT(sectors[heightsec].ceilingheight) :
		bottom >= FIXED_TO_FLOAT(sectors[heightsec].ceilingheight))
			return;
	}

	if ((thing->flags2 & MF2_LINKDRAW) && thing->tracer)
	{
		interpmobjstate_t tracer_interp = { 0 };

		if (! R_ThingVisible(thing->tracer))
			return;

		if (HWR_USING_INTERP())
		{
			R_InterpolateMobjState(thing->tracer, rendertimefrac, &tracer_interp);
		}
		else
		{
			R_InterpolateMobjState(thing->tracer, FRACUNIT, &tracer_interp);
		}

		// calculate tz for tracer, same way it is calculated for this sprite
		// transform the origin point
		if (thing->tracer->type == MT_OVERLAY) // Handle overlays
			R_ThingOffsetOverlay(thing->tracer, &tracer_interp.x, &tracer_interp.y);
		tr_x = FIXED_TO_FLOAT(tracer_interp.x) - gl_viewx;
		tr_y = FIXED_TO_FLOAT(tracer_interp.y) - gl_viewy;

		// rotation around vertical axis
		tracertz = (tr_x * gl_viewcos) + (tr_y * gl_viewsin);

		// Software does not render the linkdraw sprite if the tracer is behind the view plane,
		// so do the same check here.
		// NOTE: This check has the same flaw as the view plane check at the beginning of HWR_ProjectSprite:
		// the view aiming angle is not taken into account, leading to sprites disappearing too early when they
		// can still be seen when looking down/up at steep angles.
		if (tracertz < ZCLIP_PLANE)
			return;

		// if the sprite is behind the tracer, invert dispoffset, putting the sprite behind the tracer
		if (tz > tracertz)
			dispoffset *= -1;
	}

	HWD_LAP(HWP_PS_D);
#ifdef PS2_PROFILE
	UINT8 ps2_hidden = 0;

	// PS2-HW-72: no pixel centre inside the quad (or the quad outside the view): nothing would be drawn, nothing is made of it. Not for the sprites that have
	// more to draw than the quad (the drop shadow, the model, the link draw hack), nor the floor sprites.
	if (!splat && !cv_glmodels.value && !((thing->flags2 & MF2_LINKDRAW) && thing->tracer) && !(cv_shadow.value && thing->shadowscale) && !HWR_PS2_NoCull())
	{
		const boolean aim = cv_glspritebillboarding.value && !papersprite && fabsf(gl_viewludcos) > 1.0e-6f; // as HWR_RotateSpritePolyToAim: not for a view that looks level
		const float basey = P_MobjFlip(thing) == -1 ? FIXED_TO_FLOAT(interp.z + interp.height) : FIXED_TO_FLOAT(interp.z);

		if ((hwr_fx_cheap && !(ps2hwd_fx2 & (FX3_NOSPR | FX3_NOSPHERE))) ? (HWR_FX_SpriteHiddenCheap(x1, x2, z1, z2, gz, gzt, dispoffset, basey) || (!HWR_FX_SpriteResident(sprframe->lumppat[rot]) && HWR_PS2_SpriteHidden(x1, x2, z1, z2, gz, gzt, dispoffset, basey, aim))) : HWR_PS2_SpriteHidden(x1, x2, z1, z2, gz, gzt, dispoffset, basey, aim))
		{
			HWD_ADD(HWC_FX_QHID);
			if (!(ps2hwd_dbg_flags & 0x1000000)) // -hwdbg 16777216 (HWDBG_COMPOSE): the sprite is made all the same, HWR_DrawSprite checks the quad it builds
				return;
			ps2_hidden = 1;
		}
	}
#endif

	HWD_LAP(HWP_PS_E);
	HWD_ADD(HWC_FX_VIS);
	// store information in a vissprite
	vis = HWR_NewVisSprite();
#ifdef PS2_PROFILE
	vis->ps2_hid = ps2_hidden;
	vis->ps2_skey = (UINT8)((((thing->flags2 & MF2_SHADOW) || (thing->frame & FF_TRANSMASK)) ? 1 : 0) | (((thing->flags2 & MF2_LINKDRAW) && thing->tracer) ? 2 : 0));
	vis->ps2_iok = !(ps2hwd_fx2 & FX2_NOINTERP) && !(thing->type == MT_OVERLAY); // PS2-HW-245: the state is kept for the shadow and the aim rotation (an overlay's position is moved by R_ThingOffsetOverlay below the state)
	vis->ps2_ix = interp.x;
	vis->ps2_iy = interp.y;
	vis->ps2_iz = interp.z;
	vis->ps2_ih = interp.height;
	vis->ps2_ir = interp.radius;
	vis->ps2_isub = interp.subsector;
#endif
	vis->x1 = x1;
	vis->x2 = x2;
	vis->z1 = z1;
	vis->z2 = z2;

	vis->tz = tz; // Keep tz for the simple sprite sorting that happens
	vis->tracertz = tracertz;

	vis->renderflags = thing->renderflags;
	vis->rotateflags = sprframe->rotate;

	vis->shadowheight = shadowheight;
	vis->shadowscale = shadowscale;
	vis->dispoffset = dispoffset; // Monster Iestyn: 23/11/15: HARDWARE SUPPORT AT LAST
	vis->flip = flip;

	vis->scale = this_scale;
	vis->spritexscale = spritexscale;
	vis->spriteyscale = spriteyscale;
	vis->spritexoffset = FIXED_TO_FLOAT(spr_offset);
	vis->spriteyoffset = FIXED_TO_FLOAT(spr_topoffset);

	vis->rotated = false;

#ifdef ROTSPRITE
	if (rotsprite)
	{
		vis->gpatch = (patch_t *)rotsprite;
		vis->rotated = true;
	}
	else
#endif
		vis->gpatch = (patch_t *)W_CachePatchNum(sprframe->lumppat[rot], PU_SPRITE);

	vis->mobj = thing;

	if ((thing->flags2 & MF2_LINKDRAW) && thing->tracer && thing->color == SKINCOLOR_NONE)
		color = thing->tracer->color;
	else
		color = thing->color;

	if ((thing->flags2 & MF2_LINKDRAW) && thing->tracer && thing->translation == 0)
		translation = thing->tracer->translation;
	else
		translation = thing->translation;

	//Hurdler: 25/04/2000: now support colormap in hardware mode
	if ((thing->flags2 & MF2_LINKDRAW) && thing->tracer)
		vis->colormap = R_GetTranslationForThing(thing->tracer, color, translation);
	else
		vis->colormap = R_GetTranslationForThing(thing, color, translation);

	// set top/bottom coords
	vis->gzt = gzt;
	vis->gz = gz;

	//CONS_Debug(DBG_RENDER, "------------------\nH: sprite  : %d\nH: frame   : %x\nH: type    : %d\nH: sname   : %s\n\n",
	//            thing->sprite, thing->frame, thing->type, sprnames[thing->sprite]);

	vis->vflip = vflip;

	vis->precip = false;
	vis->bbox = false;

	vis->angle = interp.angle;
	HWD_LAP(HWP_PS_F);
}

// Precipitation projector for hardware mode
static void HWR_ProjectPrecipitationSprite(precipmobj_t *thing)
{
	gl_vissprite_t *vis;
	float tr_x, tr_y;
	float tz;
	float x1, x2;
	float z1, z2;
	float rightsin, rightcos;
	spritedef_t *sprdef;
	spriteframe_t *sprframe;
	size_t lumpoff;
	unsigned rot = 0;
	UINT8 flip;
#ifdef PS2_PROFILE
	UINT8 ps2_hidden = 0;
#endif

	if (!thing)
		return;

	// Visibility check by the blend mode.
	if (thing->frame & FF_TRANSMASK)
	{
		if (!R_BlendLevelVisible(thing->blendmode, (thing->frame & FF_TRANSMASK)>>FF_TRANSSHIFT))
			return;
	}

	// uncapped/interpolation
	interpmobjstate_t interp = {0};

	// do interpolation
	if (HWR_USING_INTERP())
	{
		R_InterpolatePrecipMobjState(thing, rendertimefrac, &interp);
	}
	else
	{
		R_InterpolatePrecipMobjState(thing, FRACUNIT, &interp);
	}

	// transform the origin point
	tr_x = FIXED_TO_FLOAT(interp.x) - gl_viewx;
	tr_y = FIXED_TO_FLOAT(interp.y) - gl_viewy;

	// rotation around vertical axis
	tz = (tr_x * gl_viewcos) + (tr_y * gl_viewsin);

	// thing is behind view plane?
	if (tz < ZCLIP_PLANE)
		return;

	tr_x = FIXED_TO_FLOAT(interp.x);
	tr_y = FIXED_TO_FLOAT(interp.y);

	// decide which patch to use for sprite relative to player
	if ((unsigned)thing->sprite >= numsprites)
#ifdef RANGECHECK
		I_Error("HWR_ProjectPrecipitationSprite: invalid sprite number %i ",
		        thing->sprite);
#else
		return;
#endif

	sprdef = &sprites[thing->sprite];

	if ((size_t)(thing->frame&FF_FRAMEMASK) >= sprdef->numframes)
#ifdef RANGECHECK
		I_Error("HWR_ProjectPrecipitationSprite: invalid sprite frame %i : %i for %s",
		        thing->sprite, thing->frame, sprnames[thing->sprite]);
#else
		return;
#endif

	sprframe = &sprdef->spriteframes[thing->frame & FF_FRAMEMASK];

	// use single rotation for all views
	lumpoff = sprframe->lumpid[0];
	flip = sprframe->flip; // Will only be 0x00 or 0xFF

	rightsin = FIXED_TO_FLOAT(FINESINE((viewangle + ANGLE_90)>>ANGLETOFINESHIFT));
	rightcos = FIXED_TO_FLOAT(FINECOSINE((viewangle + ANGLE_90)>>ANGLETOFINESHIFT));
	if (flip)
	{
		x1 = FIXED_TO_FLOAT(spritecachedinfo[lumpoff].width - spritecachedinfo[lumpoff].offset);
		x2 = FIXED_TO_FLOAT(spritecachedinfo[lumpoff].offset);
	}
	else
	{
		x1 = FIXED_TO_FLOAT(spritecachedinfo[lumpoff].offset);
		x2 = FIXED_TO_FLOAT(spritecachedinfo[lumpoff].width - spritecachedinfo[lumpoff].offset);
	}

	z1 = tr_y + x1 * rightsin;
	z2 = tr_y - x2 * rightsin;
	x1 = tr_x + x1 * rightcos;
	x2 = tr_x - x2 * rightcos;

#ifdef PS2_PROFILE
	{
		// PS2-HW-72: the same for the snow and the rain: a flake that holds no pixel centre (or lies outside the view) is not made
		const float gzt_p = FIXED_TO_FLOAT(interp.z + spritecachedinfo[lumpoff].topoffset);
		const float gz_p = gzt_p - FIXED_TO_FLOAT(spritecachedinfo[lumpoff].height);
		const boolean aim = cv_glspritebillboarding.value && !R_ThingIsPaperSprite((mobj_t *)thing) && fabsf(gl_viewludcos) > 1.0e-6f;
		const float basey = P_MobjFlip((mobj_t *)thing) == -1 ? FIXED_TO_FLOAT(interp.z + interp.height) : FIXED_TO_FLOAT(interp.z);

		if (!HWR_PS2_NoCull() && HWR_PS2_SpriteHidden(x1, x2, z1, z2, gz_p, gzt_p, 0, basey, aim))
		{
			if (!(ps2hwd_dbg_flags & 0x1000000)) // -hwdbg 16777216: made all the same, HWR_DrawPrecipitationSprite checks its quad
			{
				// the thinker of the flake is run all the same (the weather moves whether it is seen or not)
				if (!(thing->precipflags & PCF_THUNK))
				{
					if (thing->precipflags & PCF_RAIN)
						P_RainThinker(thing);
					else
						P_SnowThinker(thing);
					thing->precipflags |= PCF_THUNK;
				}
				return;
			}
			ps2_hidden = 1;
		}
	}
#endif

	//
	// store information in a vissprite
	//
	vis = HWR_NewVisSprite();
#ifdef PS2_PROFILE
	vis->ps2_hid = ps2_hidden;
	vis->ps2_iok = false; // (the precipitation: not kept)
	vis->ps2_skey = (thing->frame & FF_TRANSMASK) ? 1 : 0;
#endif
	vis->x1 = x1;
	vis->x2 = x2;
	vis->z1 = z1;
	vis->z2 = z2;
	vis->tz = tz;
	vis->dispoffset = 0; // Monster Iestyn: 23/11/15: HARDWARE SUPPORT AT LAST
	vis->gpatch = (patch_t *)W_CachePatchNum(sprframe->lumppat[rot], PU_SPRITE);
	vis->flip = flip;
	vis->mobj = (mobj_t *)thing;

	vis->colormap = NULL;

	// set top/bottom coords
	vis->gzt = FIXED_TO_FLOAT(interp.z + spritecachedinfo[lumpoff].topoffset);
	vis->gz = vis->gzt - FIXED_TO_FLOAT(spritecachedinfo[lumpoff].height);

	vis->precip = true;
	vis->bbox = false;

	// okay... this is a hack, but weather isn't networked, so it should be ok
	if (!(thing->precipflags & PCF_THUNK))
	{
		if (thing->precipflags & PCF_RAIN)
			P_RainThinker(thing);
		else
			P_SnowThinker(thing);
		thing->precipflags |= PCF_THUNK;
	}
}

static void HWR_ProjectBoundingBox(mobj_t *thing)
{
	gl_vissprite_t *vis;
	float tr_x, tr_y;
	float tz;

	if (!thing)
		return;

	if (!R_ThingBoundingBoxVisible(thing))
		return;

	// uncapped/interpolation
	boolean interpolate = cv_renderhitboxinterpolation.value;
	interpmobjstate_t interp = {0};

	if (HWR_USING_INTERP() && interpolate)
	{
		R_InterpolateMobjState(thing, rendertimefrac, &interp);
	}
	else
	{
		R_InterpolateMobjState(thing, FRACUNIT, &interp);
	}

	// transform the origin point
	tr_x = FIXED_TO_FLOAT(interp.x) - gl_viewx;
	tr_y = FIXED_TO_FLOAT(interp.y) - gl_viewy;

	// rotation around vertical axis
	tz = (tr_x * gl_viewcos) + (tr_y * gl_viewsin);

	// thing is behind view plane?
	if (tz < ZCLIP_PLANE)
		return;

	tr_x += gl_viewx;
	tr_y += gl_viewy;

	vis = HWR_NewVisSprite();
	vis->x1 = tr_x - FIXED_TO_FLOAT(interp.radius);
	vis->x2 = tr_x + FIXED_TO_FLOAT(interp.radius);
	vis->z1 = tr_y - FIXED_TO_FLOAT(interp.radius);
	vis->z2 = tr_y + FIXED_TO_FLOAT(interp.radius);
	vis->gz = FIXED_TO_FLOAT(interp.z);
	vis->gzt = vis->gz + FIXED_TO_FLOAT(interp.height);
	vis->mobj = thing;

	vis->precip = false;
	vis->bbox = true;
#ifdef PS2_PROFILE
	vis->ps2_iok = false;
	vis->ps2_skey = 2;
#endif
}

// ==========================================================================
// Sky dome rendering, ported from PrBoom+
// ==========================================================================

static gl_sky_t gl_sky;

static void HWR_SkyDomeVertex(gl_sky_t *sky, gl_skyvertex_t *vbo, int r, int c, signed char yflip, float delta, boolean foglayer)
{
	const float radians = (float)(M_PIl / 180.0f);
	const float scale = 10000.0f;
	const float maxSideAngle = 60.0f;

	float topAngle = (c / (float)sky->columns * 360.0f);
	float sideAngle = (maxSideAngle * (sky->rows - r) / sky->rows);
	float height = (float)(sin(sideAngle * radians));
	float realRadius = (float)(scale * cos(sideAngle * radians));
	float x = (float)(realRadius * cos(topAngle * radians));
	float y = (!yflip) ? scale * height : -scale * height;
	float z = (float)(realRadius * sin(topAngle * radians));
	float timesRepeat = (4 * (256.0f / sky->width));
	if (fpclassify(timesRepeat) == FP_ZERO)
		timesRepeat = 1.0f;

	if (!foglayer)
	{
		vbo->r = 255;
		vbo->g = 255;
		vbo->b = 255;
		vbo->a = (r == 0 ? 0 : 255);

		// And the texture coordinates.
		vbo->u = (-timesRepeat * c / (float)sky->columns);
		if (!yflip)	// Flipped Y is for the lower hemisphere.
			vbo->v = (r / (float)sky->rows) + 0.5f;
		else
			vbo->v = 1.0f + ((sky->rows - r) / (float)sky->rows) + 0.5f;
	}

	if (r != 4)
		y += 300.0f;

	// And finally the vertex.
	vbo->x = x;
	vbo->y = y + delta;
	vbo->z = z;
}

// Clears the sky dome.
void HWR_ClearSkyDome(void)
{
	gl_sky_t *sky = &gl_sky;

	if (sky->loops)
		free(sky->loops);
	if (sky->data)
		free(sky->data);

	sky->loops = NULL;
	sky->data = NULL;

	sky->vbo = 0;
	sky->rows = sky->columns = 0;
	sky->loopcount = 0;

	sky->detail = 0;
	sky->texture = -1;
	sky->width = sky->height = 0;

	sky->rebuild = true;
}

void HWR_BuildSkyDome(void)
{
	int c, r;
	signed char yflip;
	int row_count = 4;
	int col_count = 4;
	float delta;

	gl_sky_t *sky = &gl_sky;
	gl_skyvertex_t *vertex_p;
	texture_t *texture = textures[texturetranslation[skytexture]];

	sky->detail = 16;
	col_count *= sky->detail;

	if ((sky->columns != col_count) || (sky->rows != row_count))
		HWR_ClearSkyDome();

	sky->columns = col_count;
	sky->rows = row_count;
	sky->vertex_count = 2 * sky->rows * (sky->columns * 2 + 2) + sky->columns * 2;

	if (!sky->loops)
		sky->loops = malloc((sky->rows * 2 + 2) * sizeof(sky->loops[0]));

	// create vertex array
	if (!sky->data)
		sky->data = malloc(sky->vertex_count * sizeof(sky->data[0]));

	sky->texture = texturetranslation[skytexture];
	sky->width = texture->width;
	sky->height = texture->height;

	vertex_p = &sky->data[0];
	sky->loopcount = 0;

	for (yflip = 0; yflip < 2; yflip++)
	{
		sky->loops[sky->loopcount].mode = HWD_SKYLOOP_FAN;
		sky->loops[sky->loopcount].vertexindex = vertex_p - &sky->data[0];
		sky->loops[sky->loopcount].vertexcount = col_count;
		sky->loops[sky->loopcount].use_texture = false;
		sky->loopcount++;

		delta = 0.0f;

		for (c = 0; c < col_count; c++)
		{
			HWR_SkyDomeVertex(sky, vertex_p, 1, c, yflip, 0.0f, true);
			vertex_p->r = 255;
			vertex_p->g = 255;
			vertex_p->b = 255;
			vertex_p->a = 255;
			vertex_p++;
		}

		delta = (yflip ? 5.0f : -5.0f) / 128.0f;

		for (r = 0; r < row_count; r++)
		{
			sky->loops[sky->loopcount].mode = HWD_SKYLOOP_STRIP;
			sky->loops[sky->loopcount].vertexindex = vertex_p - &sky->data[0];
			sky->loops[sky->loopcount].vertexcount = 2 * col_count + 2;
			sky->loops[sky->loopcount].use_texture = true;
			sky->loopcount++;

			for (c = 0; c <= col_count; c++)
			{
				HWR_SkyDomeVertex(sky, vertex_p++, r + (yflip ? 1 : 0), (c ? c : 0), yflip, delta, false);
				HWR_SkyDomeVertex(sky, vertex_p++, r + (yflip ? 0 : 1), (c ? c : 0), yflip, delta, false);
			}
		}
	}
}

static void HWR_DrawSkyBackground(player_t *player)
{
	if (HWR_IsWireframeMode())
		return;

	HWD.pfnSetBlend(PF_Translucent|PF_NoDepthTest|PF_Modulated);

	HWR_GetTexture(texturetranslation[skytexture], false);

	if (cv_glskydome.value)
	{
		FTransform dometransform;

		memcpy(&dometransform, &atransform, sizeof(FTransform));

		dometransform.x      = 0.0;
		dometransform.y      = 0.0;
		dometransform.z      = 0.0;

		//04/01/2000: Hurdler: added for T&L
		//                     It should replace all other gl_viewxxx when finished
		HWR_SetTransformAiming(&dometransform, player, false);
		dometransform.angley = (float)((viewangle-ANGLE_270)>>ANGLETOFINESHIFT)*(360.0f/(float)FINEANGLES);

		if (gl_sky.texture != texturetranslation[skytexture])
		{
			HWR_ClearSkyDome();
			HWR_BuildSkyDome();
		}

		if (HWR_UseShader())
			HWD.pfnSetShader(HWR_GetShaderFromTarget(SHADER_SKY));
		HWD.pfnSetTransform(&dometransform);
		HWD.pfnRenderSkyDome(&gl_sky);
	}
	else
	{
		FOutVector v[4];
		angle_t angle;
		float dimensionmultiply;
		float aspectratio;
		float angleturn;

		aspectratio = (float)vid.width/(float)vid.height;

		//Hurdler: the sky is the only texture who need 4.0f instead of 1.0
		//         because it's called just after clearing the screen
		//         and thus, the near clipping plane is set to 3.99
		// Sryder: Just use the near clipping plane value then

		//  3--2
		//  | /|
		//  |/ |
		//  0--1
		v[0].x = v[3].x = -ZCLIP_PLANE-1;
		v[1].x = v[2].x =  ZCLIP_PLANE+1;
		v[0].y = v[1].y = -ZCLIP_PLANE-1;
		v[2].y = v[3].y =  ZCLIP_PLANE+1;

		v[0].z = v[1].z = v[2].z = v[3].z = ZCLIP_PLANE+1;

		// X

		// NOTE: This doesn't work right with texture widths greater than 1024
		// software doesn't draw any further than 1024 for skies anyway, but this doesn't overlap properly
		// The only time this will probably be an issue is when a sky wider than 1024 is used as a sky AND a regular wall texture

		angle = (viewangle + xtoviewangle[0]);

		dimensionmultiply = ((float)textures[texturetranslation[skytexture]]->width/256.0f);

		v[0].s = v[3].s = (-1.0f * angle) / (((float)ANGLE_90-1.0f)*dimensionmultiply); // left
		v[2].s = v[1].s = v[0].s + (1.0f/dimensionmultiply); // right (or left + 1.0f)
		// use +angle and -1.0f above instead if you wanted old backwards behavior

		// Y
		angle = aimingangle;
		dimensionmultiply = ((float)textures[texturetranslation[skytexture]]->height/(128.0f*aspectratio));

		if (splitscreen)
		{
			dimensionmultiply *= 2;
			angle *= 2;
		}

		// Middle of the sky should always be at angle 0
		// need to keep correct aspect ratio with X
		if (atransform.flip)
		{
			// During vertical flip the sky should be flipped and it's y movement should also be flipped obviously
			v[3].t = v[2].t = -(0.5f-(0.5f/dimensionmultiply)); // top
			v[0].t = v[1].t = v[3].t - (1.0f/dimensionmultiply); // bottom (or top - 1.0f)
		}
		else
		{
			v[0].t = v[1].t = -(0.5f-(0.5f/dimensionmultiply)); // bottom
			v[3].t = v[2].t = v[0].t - (1.0f/dimensionmultiply); // top (or bottom - 1.0f)
		}

		angleturn = (((float)ANGLE_45-1.0f)*aspectratio)*dimensionmultiply;

		if (angle > ANGLE_180) // Do this because we don't want the sky to suddenly teleport when crossing over 0 to 360 and vice versa
		{
			angle = InvAngle(angle);
			v[3].t = v[2].t += ((float) angle / angleturn);
			v[0].t = v[1].t += ((float) angle / angleturn);
		}
		else
		{
			v[3].t = v[2].t -= ((float) angle / angleturn);
			v[0].t = v[1].t -= ((float) angle / angleturn);
		}

		HWD.pfnUnSetShader();
		HWD.pfnDrawPolygon(NULL, v, 4, 0);
	}
}


// -----------------+
// HWR_ClearView : clear the viewwindow, with maximum z value
// -----------------+
static inline void HWR_ClearView(void)
{
	//  3--2
	//  | /|
	//  |/ |
	//  0--1

	/// \bug faB - enable depth mask, disable color mask

	HWD.pfnGClipRect((INT32)viewwindowx,
	                 (INT32)viewwindowy,
	                 (INT32)(viewwindowx + viewwidth),
	                 (INT32)(viewwindowy + viewheight),
	                 ZCLIP_PLANE);
	HWD.pfnClearBuffer(false, true, 0);

	//disable clip window - set to full size
	// rem by Hurdler
	// HWD.pfnGClipRect(0, 0, vid.width, vid.height);
}


// -----------------+
// HWR_SetViewSize  : set projection and scaling values
// -----------------+
void HWR_SetViewSize(void)
{
	HWD.pfnFlushScreenTextures();
}

// Set view aiming, for the sky dome, the skybox,
// and the normal view, all with a single function.
static void HWR_SetTransformAiming(FTransform *trans, player_t *player, boolean skybox)
{
	// 1 = always on
	// 2 = chasecam only
	if (cv_glshearing.value == 1 || (cv_glshearing.value == 2 && R_IsViewpointThirdPerson(player, skybox)))
	{
		fixed_t fixedaiming = AIMINGTODY(aimingangle);
		trans->viewaiming = FIXED_TO_FLOAT(fixedaiming) * ((float)vid.width / vid.height) / ((float)BASEVIDWIDTH / BASEVIDHEIGHT);
		if (splitscreen)
			trans->viewaiming *= 2.125; // splitscreen adjusts fov with 0.8, so compensate (but only halfway, since splitscreen means only half the screen is used)
		trans->shearing = true;
		gl_aimingangle = 0;
	}
	else
	{
		trans->shearing = false;
		gl_aimingangle = aimingangle;
	}

	trans->anglex = (float)(gl_aimingangle>>ANGLETOFINESHIFT)*(360.0f/(float)FINEANGLES);
}

//
// Sets the shader state.
//
static void HWR_SetShaderState(void)
{
	HWD.pfnSetSpecialState(HWD_SET_SHADERS, HWR_UseShader());
}

static void HWR_SetupView(player_t *player, INT32 viewnumber, float fpov, boolean skybox)
{
	postimg_t *type;

	if (splitscreen && player == &players[secondarydisplayplayer])
		type = &postimgtype2;
	else
		type = &postimgtype;

	if (!HWR_ShouldUsePaletteRendering())
	{
		// do we really need to save player (is it not the same)?
		player_t *saved_player = stplyr;
		HWP_SPAN_BEGIN(tpal);
		stplyr = player;
		ST_doPaletteStuff();
		stplyr = saved_player;
		HWP_SPAN_END(tpal, HWP_S_PAL);
#ifdef ALAM_LIGHTING
		HWR_SetLights(viewnumber);
#else
		(void)viewnumber;
#endif
	}

	// note: sets viewangle, viewx, viewy, viewz
	{
		HWP_SPAN_BEGIN(tfr);
		if (skybox)
			R_SkyboxFrame(player);
		else
			R_SetupFrame(player);
		HWP_SPAN_END(tfr, HWP_S_FRAME);
	}

	gl_viewx = FixedToFloat(viewx);
	gl_viewy = FixedToFloat(viewy);
	gl_viewz = FixedToFloat(viewz);
	gl_viewsin = FixedToFloat(viewsin);
	gl_viewcos = FixedToFloat(viewcos);

	//04/01/2000: Hurdler: added for T&L
	//                     It should replace all other gl_viewxxx when finished
	memset(&atransform, 0x00, sizeof(FTransform));

	HWR_SetTransformAiming(&atransform, player, skybox);
	atransform.angley = (float)(viewangle>>ANGLETOFINESHIFT)*(360.0f/(float)FINEANGLES);

	gl_viewludsin = FixedToFloat(FINECOSINE(gl_aimingangle>>ANGLETOFINESHIFT));
	gl_viewludcos = FixedToFloat(-FINESINE(gl_aimingangle>>ANGLETOFINESHIFT));

	if (*type == postimg_flip)
		atransform.flip = true;
	else
		atransform.flip = false;

	atransform.x      = gl_viewx;
	atransform.y      = gl_viewy;
	atransform.z      = gl_viewz;
	atransform.scalex = 1;
	atransform.scaley = (float)vid.width/vid.height;
	atransform.scalez = 1;

	atransform.fovxangle = fpov; // Tails
	atransform.fovyangle = fpov; // Tails
	if (player->viewrollangle != 0)
	{
		fixed_t rol = AngleFixed(player->viewrollangle);
		atransform.rollangle = FixedToFloat(rol);
		atransform.roll = true;
		atransform.rollx = 1.0f;
		atransform.rollz = 0.0f;
	}
	atransform.splitscreen = splitscreen;

	gl_fovlud = (float)(1.0l/tan((double)(fpov*M_PIl/360l)));
}

// ==========================================================================
// Same as rendering the player view, but from the skybox object
// ==========================================================================
void HWR_RenderSkyboxView(INT32 viewnumber, player_t *player)
{
	const float fpov = FixedToFloat(R_GetPlayerFov(player));
	HWP_SPAN_BEGIN(tk0);

#ifdef PS2_PROFILE
	ps2hwp_skyview = 1;
	HWR_GCacheFrame();
#endif
	HWR_SetupView(player, viewnumber, fpov, true);
	HWP_SPAN_END(tk0, HWP_K_SET);

	// check for new console commands.
	NetUpdate();

	//------------------------------------------------------------------------
	HWP_SPAN_BEGIN(tk1);
	HWR_ClearView();
	HWP_SPAN_END(tk1, HWP_K_CLR);

	{
	HWP_SPAN_BEGIN(tkd);
	if (drawsky)
		HWR_DrawSkyBackground(player);
	HWP_SPAN_END(tkd, HWP_K_DOME);
	}

	//Hurdler: it doesn't work in splitscreen mode
	drawsky = splitscreen;

	HWP_SPAN_BEGIN(tkc);
	HWR_ClearSprites();

	drawcount = 0;

	if (rendermode == render_opengl)
	{
		angle_t a1 = gld_FrustumAngle(fpov, gl_aimingangle);
		gld_clipper_Clear();
		gld_clipper_SafeAddClipRange(viewangle + a1, viewangle - a1);
#ifdef HAVE_SPHEREFRUSTRUM
		gld_FrustrumSetup();
#endif
	}

	//04/01/2000: Hurdler: added for T&L
	//                     Actually it only works on Walls and Planes
	HWD.pfnSetTransform(&atransform);

	// Reset the shader state.
	HWR_SetShaderState();

	if (HWR_IsWireframeMode())
		HWD.pfnSetSpecialState(HWD_SET_WIREFRAME, 1);

	validcount++;
	HWP_SPAN_END(tkc, HWP_K_CLIP);
	HWP_SPAN_END(tk1, HWP_K_BG);

	{
	HWP_SPAN_BEGIN(tk2);
	if (cv_glbatching.value)
		HWR_StartBatching();

	HWR_RenderBSPNode((INT32)numnodes-1);
	HWP_SPAN_END(tk2, HWP_K_BSP);
	}

	{
	HWP_SPAN_BEGIN(tk3);
	if (cv_glbatching.value)
		HWR_RenderBatches();
	HWP_SPAN_END(tk3, HWP_K_BAT);
	}

	// Check for new console commands.
	NetUpdate();

#ifdef ALAM_LIGHTING
	//14/11/99: Hurdler: moved here because it doesn't work with
	// subsector, see other comments;
	HWR_ResetLights();
#endif

	// Draw MD2 and sprites
	{
	HWP_SPAN_BEGIN(tk4);
	HWR_SortVisSprites();
	HWR_DrawSprites();
	HWP_SPAN_END(tk4, HWP_K_SPR);
	}

#ifdef NEWCORONAS
	//Hurdler: they must be drawn before translucent planes, what about gl fog?
	HWR_DrawCoronas();
#endif

	if (numplanes || numpolyplanes || numwalls) //Hurdler: render 3D water and transparent walls after everything
	{
		HWP_SPAN_BEGIN(tk5);
		HWR_CreateDrawNodes();
		HWP_SPAN_END(tk5, HWP_K_NODE);
	}

	if (HWR_IsWireframeMode())
		HWD.pfnSetSpecialState(HWD_SET_WIREFRAME, 0);

	HWD.pfnSetTransform(NULL);
	HWD.pfnUnSetShader();

	// Check for new console commands.
	NetUpdate();

	// added by Hurdler for correct splitscreen
	// moved here by hurdler so it works with the new near clipping plane
	HWD.pfnGClipRect(0, 0, vid.width, vid.height, NZCLIP_PLANE);
#ifdef PS2_PROFILE
	ps2hwp_skyview = 0;
#endif
}

// ==========================================================================
//
// ==========================================================================
void HWR_RenderPlayerView(INT32 viewnumber, player_t *player)
{
	HWP_LOCAL;
	HWP_SPAN_BEGIN(hwp_tsetup);
	const float fpov = FixedToFloat(R_GetPlayerFov(player));

	const boolean skybox = (skyboxmo[0] && cv_skybox.value); // True if there's a skybox object and skyboxes are on

	FRGBAFloat ClearColor;

	ClearColor.red = 0.0f;
	ClearColor.green = 0.0f;
	ClearColor.blue = 0.0f;
	ClearColor.alpha = 1.0f;

	if (cv_glshaders.value)
		HWD.pfnSetShaderInfo(HWD_SHADERINFO_LEVELTIME, (INT32)leveltime); // The water surface shader needs the leveltime.

	if (viewnumber == 0) // Only do it if it's the first screen being rendered
	{
		HWP_SPAN_BEGIN(tc1);
		HWD.pfnClearBuffer(true, false, &ClearColor); // Clear the Color Buffer, stops HOMs. Also seems to fix the skybox issue on Intel GPUs.
		HWP_SPAN_END(tc1, HWP_S_CLR1);
	}

	PS_START_TIMING(ps_hw_skyboxtime);
	if (skybox && drawsky) // If there's a skybox and we should be drawing the sky, draw the skybox
		HWR_RenderSkyboxView(viewnumber, player); // This is drawn before everything else so it is placed behind
	PS_STOP_TIMING(ps_hw_skyboxtime);

	{
	HWP_SPAN_BEGIN(tms);
#ifdef PS2_PROFILE
	HWR_GCacheFrame();
#endif
	HWR_SetupView(player, viewnumber, fpov, false);
	HWP_SPAN_END(tms, HWP_M_SETUP);
	}

	framecount++; // timedemo

	// check for new console commands.
	{
		HWP_SPAN_BEGIN(tnet);
		NetUpdate();
		HWP_SPAN_END(tnet, HWP_S_NET);
	}

	//------------------------------------------------------------------------
	{
		HWP_SPAN_BEGIN(tc2);
		HWR_ClearView(); // Clears the depth buffer and resets the view I believe
		HWP_SPAN_END(tc2, HWP_S_CLR2);
	}
	HWP_SPAN_END(hwp_tsetup, HWP_SETUP);

	if (!skybox && drawsky) // Don't draw the regular sky if there's a skybox
	{
		HWP_SPAN_BEGIN(hwp_tsky);
		HWR_DrawSkyBackground(player);
		HWP_SPAN_END(hwp_tsky, HWP_SKY);
	}

	//Hurdler: it doesn't work in splitscreen mode
	drawsky = splitscreen;

	HWP_SPAN_BEGIN(tmc);
	HWR_ClearSprites();

	drawcount = 0;

	if (rendermode == render_opengl)
	{
		angle_t a1 = gld_FrustumAngle(fpov, gl_aimingangle);
		gld_clipper_Clear();
		gld_clipper_SafeAddClipRange(viewangle + a1, viewangle - a1);
#ifdef HAVE_SPHEREFRUSTRUM
		gld_FrustrumSetup();
#endif
	}

	//04/01/2000: Hurdler: added for T&L
	//                     Actually it only works on Walls and Planes
	HWD.pfnSetTransform(&atransform);

	// Reset the shader state.
	HWR_SetShaderState();

	if (HWR_IsWireframeMode())
		HWD.pfnSetSpecialState(HWD_SET_WIREFRAME, 1);

	ps_numbspcalls.value.i = 0;
	ps_numpolyobjects.value.i = 0;
	HWP_SPAN_END(tmc, HWP_M_CLIP);
	HWP_LAP(HWP_CLEAR);
	PS_START_TIMING(ps_bsptime);

	validcount++;

	if (cv_glbatching.value)
		HWR_StartBatching();

	HWR_RenderBSPNode((INT32)numnodes-1);

	PS_STOP_TIMING(ps_bsptime);
	HWP_LAP(HWP_BSP);

	if (cv_glbatching.value)
		HWR_RenderBatches();
	HWP_LAP(HWP_BATCH);

	// Check for new console commands.
	NetUpdate();

#ifdef ALAM_LIGHTING
	//14/11/99: Hurdler: moved here because it doesn't work with
	// subsector, see other comments;
	HWR_ResetLights();
#endif

	// Draw MD2 and sprites
	ps_numsprites.value.i = gl_visspritecount;
	PS_START_TIMING(ps_hw_spritesorttime);
	{
		HWP_SPAN_BEGIN(tsort);
		HWR_SortVisSprites();
		HWP_SPAN_END(tsort, HWP_SPRSORT);
	}
	PS_STOP_TIMING(ps_hw_spritesorttime);
	PS_START_TIMING(ps_hw_spritedrawtime);
	{
		HWP_SPAN_BEGIN(tdraw);
		HWR_DrawSprites();
		HWP_SPAN_END(tdraw, HWP_SPRDRAW);
	}
	PS_STOP_TIMING(ps_hw_spritedrawtime);
	HWP_LAP(HWP_SPRITES);

#ifdef NEWCORONAS
	//Hurdler: they must be drawn before translucent planes, what about gl fog?
	HWR_DrawCoronas();
#endif

	ps_numdrawnodes.value.i = 0;
	ps_hw_nodesorttime.value.p = 0;
	ps_hw_nodedrawtime.value.p = 0;
	if (numplanes || numpolyplanes || numwalls) //Hurdler: render 3D water and transparent walls after everything
	{
		HWR_CreateDrawNodes();
	}
	HWP_LAP(HWP_NODES);

	if (HWR_IsWireframeMode())
		HWD.pfnSetSpecialState(HWD_SET_WIREFRAME, 0);

	HWD.pfnSetTransform(NULL);
	HWD.pfnUnSetShader();

	HWR_DoPostProcessor(player);

	// Check for new console commands.
	NetUpdate();

	// added by Hurdler for correct splitscreen
	// moved here by hurdler so it works with the new near clipping plane
	HWD.pfnGClipRect(0, 0, vid.width, vid.height, NZCLIP_PLANE);
	HWP_LAP(HWP_POST);
}

// Returns whether palette rendering is "actually enabled."
// Can't have palette rendering if shaders are disabled.
boolean HWR_ShouldUsePaletteRendering(void)
{
	// PS2-HW-71 (OPT10, replaces PS2-HW-62): palette rendering is on as on the PC (default gr_paletterendering On): the GS lights palette textures with the
	// CLUT of the colormap row (ps2_hw_pal.inc), the colours are those of the software renderer; the engine's non palette equations stay for the rest
	return (pMasterPalette != NULL && cv_glpaletterendering.value && HWR_UseShader());
}

// enable or disable palette rendering state depending on settings and availability
// called when relevant settings change
// shader recompilation is done in the cvar callback
static void HWR_TogglePaletteRendering(void)
{
	// which state should we go to?
	if (HWR_ShouldUsePaletteRendering())
	{
		// are we not in that state already?
		if (!gl_palette_rendering_state)
		{
			gl_palette_rendering_state = true;

			// The textures will still be converted to RGBA by r_opengl.
			// This however makes hw_cache use paletted blending for composite textures!
			// (patchformat is not touched)
#ifndef PS2_PROFILE // PS2-HW-71: the GS keeps the map textures as palette indices (P_8 + chroma key): the CLUT lights them, no 16 bit alpha format
			textureformat = GL_TEXFMT_AP_88;
#endif

			HWR_SetMapPalette();
			HWR_SetPalette(pLocalPalette);

			// If the r_opengl "texture palette" stays the same during this switch, these textures
			// will not be cleared out. However they are still out of date since the
			// composite texture blending method has changed. Therefore they need to be cleared.
			HWR_LoadMapTextures(numtextures);
		}
	}
	else
	{
		// are we not in that state already?
		if (gl_palette_rendering_state)
		{
			gl_palette_rendering_state = false;
#ifndef PS2_PROFILE
			textureformat = GL_TEXFMT_RGBA;
#endif
			HWR_SetPalette(pLocalPalette);
			// If the r_opengl "texture palette" stays the same during this switch, these textures
			// will not be cleared out. However they are still out of date since the
			// composite texture blending method has changed. Therefore they need to be cleared.
			HWR_LoadMapTextures(numtextures);
		}
	}
}

void HWR_LoadLevel(void)
{
#ifdef ALAM_LIGHTING
	// BP: reset light between levels (we draw preview frame lights on current frame)
	HWR_ResetLights();
#endif

	HWR_CreatePlanePolygons((INT32)numnodes - 1);
#ifdef PS2_PROFILE
	HWR_PlaneCacheReset();
#endif

	// Build the sky dome
	HWR_ClearSkyDome();
	HWR_BuildSkyDome();

	if (HWR_ShouldUsePaletteRendering())
		HWR_SetMapPalette();

	gl_maploaded = true;
}

// ==========================================================================
//                                                         3D ENGINE COMMANDS
// ==========================================================================

static CV_PossibleValue_t glshaders_cons_t[] = {{0, "Off"}, {1, "On"}, {2, "Ignore custom shaders"}, {0, NULL}};
static CV_PossibleValue_t glmodelinterpolation_cons_t[] = {{0, "Off"}, {1, "Sometimes"}, {2, "Always"}, {0, NULL}};
static CV_PossibleValue_t glfakecontrast_cons_t[] = {{0, "Off"}, {1, "On"}, {2, "Smooth"}, {0, NULL}};
static CV_PossibleValue_t glshearing_cons_t[] = {{0, "Off"}, {1, "On"}, {2, "Third-person"}, {0, NULL}};

static void CV_glfiltermode_OnChange(void);
static void CV_glanisotropic_OnChange(void);
static void CV_glmodellighting_OnChange(void);
static void CV_glpaletterendering_OnChange(void);
static void CV_glpalettedepth_OnChange(void);
static void CV_glshaders_OnChange(void);
static void CV_gllightdithering_OnChange(void);

static CV_PossibleValue_t glfiltermode_cons_t[]= {{HWD_SET_TEXTUREFILTER_POINTSAMPLED, "Nearest"},
	{HWD_SET_TEXTUREFILTER_BILINEAR, "Bilinear"}, {HWD_SET_TEXTUREFILTER_TRILINEAR, "Trilinear"},
	{HWD_SET_TEXTUREFILTER_MIXED1, "Linear_Nearest"},
	{HWD_SET_TEXTUREFILTER_MIXED2, "Nearest_Linear"},
	{HWD_SET_TEXTUREFILTER_MIXED3, "Nearest_Mipmap"},
	{0, NULL}};
CV_PossibleValue_t glanisotropicmode_cons_t[] = {{1, "MIN"}, {16, "MAX"}, {0, NULL}};

consvar_t cv_glshaders = CVAR_INIT ("gr_shaders", "On", CV_SAVE|CV_CALL, glshaders_cons_t, CV_glshaders_OnChange);

#ifdef ALAM_LIGHTING
consvar_t cv_gldynamiclighting = CVAR_INIT ("gr_dynamiclighting", "On", CV_SAVE, CV_OnOff, NULL);
consvar_t cv_glstaticlighting  = CVAR_INIT ("gr_staticlighting", "On", CV_SAVE, CV_OnOff, NULL);
consvar_t cv_glcoronas = CVAR_INIT ("gr_coronas", "On", CV_SAVE, CV_OnOff, NULL);
consvar_t cv_glcoronasize = CVAR_INIT ("gr_coronasize", "1", CV_SAVE|CV_FLOAT, 0, NULL);
#endif

consvar_t cv_glmodels = CVAR_INIT ("gr_models", "Off", CV_SAVE, CV_OnOff, NULL);
consvar_t cv_glmodelinterpolation = CVAR_INIT ("gr_modelinterpolation", "Sometimes", CV_SAVE, glmodelinterpolation_cons_t, NULL);
consvar_t cv_glmodellighting = CVAR_INIT ("gr_modellighting", "Off", CV_SAVE|CV_CALL, CV_OnOff, CV_glmodellighting_OnChange);

consvar_t cv_glshearing = CVAR_INIT ("gr_shearing", "Off", CV_SAVE, glshearing_cons_t, NULL);
consvar_t cv_glspritebillboarding = CVAR_INIT ("gr_spritebillboarding", "Off", CV_SAVE, CV_OnOff, NULL);
consvar_t cv_glskydome = CVAR_INIT ("gr_skydome", "On", CV_SAVE, CV_OnOff, NULL);
consvar_t cv_glfakecontrast = CVAR_INIT ("gr_fakecontrast", "Smooth", CV_SAVE, glfakecontrast_cons_t, NULL);
consvar_t cv_glslopecontrast = CVAR_INIT ("gr_slopecontrast", "Off", CV_SAVE, CV_OnOff, NULL);

consvar_t cv_glfiltermode = CVAR_INIT ("gr_filtermode", "Nearest", CV_SAVE|CV_CALL, glfiltermode_cons_t, CV_glfiltermode_OnChange);
consvar_t cv_glanisotropicmode = CVAR_INIT ("gr_anisotropicmode", "1", CV_SAVE|CV_CALL, glanisotropicmode_cons_t, CV_glanisotropic_OnChange);

consvar_t cv_glsolvetjoin = CVAR_INIT ("gr_solvetjoin", "On", 0, CV_OnOff, NULL);

consvar_t cv_glbatching = CVAR_INIT ("gr_batching", "On", 0, CV_OnOff, NULL);

static CV_PossibleValue_t glpalettedepth_cons_t[] = {{16, "16 bits"}, {24, "24 bits"}, {0, NULL}};

consvar_t cv_glpaletterendering = CVAR_INIT ("gr_paletterendering", "On", CV_SAVE|CV_CALL, CV_OnOff, CV_glpaletterendering_OnChange);
consvar_t cv_glpalettedepth = CVAR_INIT ("gr_palettedepth", "16 bits", CV_SAVE|CV_CALL, glpalettedepth_cons_t, CV_glpalettedepth_OnChange);
consvar_t cv_gllightdither = CVAR_INIT ("gr_lightdithering", "Off", CV_SAVE|CV_CALL|CV_CLIENT, CV_OnOff, CV_gllightdithering_OnChange);

#define ONLY_IF_GL_LOADED if (vid.glstate != VID_GL_LIBRARY_LOADED) return;
consvar_t cv_glwireframe = CVAR_INIT ("gr_wireframe", "Off", 0, CV_OnOff, NULL);

static void CV_glfiltermode_OnChange(void)
{
	ONLY_IF_GL_LOADED
	HWD.pfnSetSpecialState(HWD_SET_TEXTUREFILTERMODE, cv_glfiltermode.value);
}

static void CV_glanisotropic_OnChange(void)
{
	ONLY_IF_GL_LOADED
	HWD.pfnSetSpecialState(HWD_SET_TEXTUREANISOTROPICMODE, cv_glanisotropicmode.value);
}

static void CV_glmodellighting_OnChange(void)
{
	ONLY_IF_GL_LOADED
	// if shaders have been compiled, then they now need to be recompiled.
	if (gl_shadersavailable)
		HWR_CompileShaders();
}

static void CV_glpaletterendering_OnChange(void)
{
	ONLY_IF_GL_LOADED
	if (gl_shadersavailable)
	{
		HWR_CompileShaders();
		HWR_TogglePaletteRendering();
	}
}

static void CV_glpalettedepth_OnChange(void)
{
	ONLY_IF_GL_LOADED
	// refresh the screen palette
	if (HWR_ShouldUsePaletteRendering())
		HWR_SetPalette(pLocalPalette);
}

static void CV_glshaders_OnChange(void)
{
	ONLY_IF_GL_LOADED
	HWR_SetShaderState();
	if (cv_glpaletterendering.value)
	{
		// can't do palette rendering without shaders, so update the state if needed
		HWR_TogglePaletteRendering();
	}
}

static void CV_gllightdithering_OnChange(void)
{
	ONLY_IF_GL_LOADED
	if (gl_shadersavailable)
	{
		HWR_CompileShaders();
	}
}

//added by Hurdler: console varibale that are saved
void HWR_AddCommands(void)
{
#ifdef ALAM_LIGHTING
	CV_RegisterVar(&cv_glstaticlighting);
	CV_RegisterVar(&cv_gldynamiclighting);
	CV_RegisterVar(&cv_glcoronasize);
	CV_RegisterVar(&cv_glcoronas);
#endif

	CV_RegisterVar(&cv_glmodellighting);
	CV_RegisterVar(&cv_glmodelinterpolation);
	CV_RegisterVar(&cv_glmodels);

	CV_RegisterVar(&cv_glskydome);
	CV_RegisterVar(&cv_glspritebillboarding);
	CV_RegisterVar(&cv_glfakecontrast);
	CV_RegisterVar(&cv_glslopecontrast);
	CV_RegisterVar(&cv_glshearing);
	CV_RegisterVar(&cv_glshaders);

	CV_RegisterVar(&cv_glfiltermode);
	CV_RegisterVar(&cv_glanisotropicmode);
	CV_RegisterVar(&cv_glsolvetjoin);

	CV_RegisterVar(&cv_glbatching);

	CV_RegisterVar(&cv_glpaletterendering);
	CV_RegisterVar(&cv_glpalettedepth);
	CV_RegisterVar(&cv_gllightdither);
	CV_RegisterVar(&cv_glwireframe);
}

// --------------------------------------------------------------------------
// Setup the hardware renderer
// --------------------------------------------------------------------------
void HWR_Startup(void)
{
	if (!gl_init)
	{
		CONS_Printf("HWR_Startup()...\n");

#ifdef PS2_PROFILE // PS2-HW-05: the caches keep palette indices (1 / 2 bytes per texel instead of 4); the GS draws them through a CLUT
		textureformat = GL_TEXFMT_P_8;
		patchformat = GL_TEXFMT_AP_88;
#else
		textureformat = patchformat = GL_TEXFMT_RGBA;
#endif

		HWR_InitPolyPool();
		HWR_InitMapTextures();
		HWR_InitModels();
#ifdef ALAM_LIGHTING
		HWR_InitLight();
#endif

		gl_shadersavailable = HWR_InitShaders();
		HWR_SetShaderState();
		HWR_LoadAllCustomShaders();
		HWR_TogglePaletteRendering();
	}

	gl_init = true;
}

// --------------------------------------------------------------------------
// Called after switching to the hardware renderer
// --------------------------------------------------------------------------
void HWR_Switch(void)
{
	// Set special states from CVARs
	HWD.pfnSetSpecialState(HWD_SET_TEXTUREFILTERMODE, cv_glfiltermode.value);
	HWD.pfnSetSpecialState(HWD_SET_TEXTUREANISOTROPICMODE, cv_glanisotropicmode.value);

	// Load textures
	if (!gl_maptexturesloaded)
		HWR_LoadMapTextures(numtextures);

	// Create plane polygons
	if (!gl_maploaded && (gamestate == GS_LEVEL || (gamestate == GS_TITLESCREEN && titlemapinaction)))
	{
		HWR_ClearAllTextures();
		HWR_LoadLevel();
	}
}

// --------------------------------------------------------------------------
// Free resources allocated by the hardware renderer
// --------------------------------------------------------------------------
void HWR_Shutdown(void)
{
	CONS_Printf("HWR_Shutdown()\n");
	HWR_FreeExtraSubsectors();
	HWR_FreePolyPool();
	HWR_FreeMapTextures();
	HWD.pfnFlushScreenTextures();
#ifdef PS2
	Z_Free(linkdrawlist); // PS2-171
	linkdrawlist = NULL;
	linkdrawcap = linkdrawcount = 0;
	// PS2-HW-140 (OPT11-STAB): a frame abandoned by an out-of-memory jump (ps2_hwfb.c) leaves the lists of transparent walls and planes filled (they are emptied at the end of
	// HWR_CreateDrawNodes): the next hardware frame then sorted old entries (freed levels' pointers) with duplicate drawcounts: "CompareDrawNodes: diff is zero"
	numwalls = numplanes = numpolyplanes = 0;
	drawcount = 0;
	gl_maploaded = false;
	// Keep model/shader CPU initialization; their owners remain valid across GS reacquisition.
#endif
}

void transform(float *cx, float *cy, float *cz)
{
	float tr_x,tr_y;
	// translation
	tr_x = *cx - gl_viewx;
	tr_y = *cz - gl_viewy;
//	*cy = *cy;

	// rotation around vertical y axis
	*cx = (tr_x * gl_viewsin) - (tr_y * gl_viewcos);
	tr_x = (tr_x * gl_viewcos) + (tr_y * gl_viewsin);

	//look up/down ----TOTAL SUCKS!!!--- do the 2 in one!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	tr_y = *cy - gl_viewz;

	*cy = (tr_x * gl_viewludcos) + (tr_y * gl_viewludsin);
	*cz = (tr_x * gl_viewludsin) - (tr_y * gl_viewludcos);

	//scale y before frustum so that frustum can be scaled to screen height
	*cy *= ORIGINAL_ASPECT * gl_fovlud;
	*cx *= gl_fovlud;
}

void HWR_AddTransparentWall(FOutVector *wallVerts, FSurfaceInfo *pSurf, INT32 texnum, FBITFIELD blend, boolean fogwall, INT32 lightlevel, extracolormap_t *wallcolormap)
{
	static size_t allocedwalls = 0;

	if (!r_renderwalls)
		return;

#ifdef PS2_PROFILE
	if (hwr_grec_on)
		HWR_GCRecTWall(wallVerts, pSurf, texnum, blend, fogwall, lightlevel, wallcolormap); // OPT11 PS2-HW-80
#endif

	// Force realloc if buffer has been freed
	if (!wallinfo)
		allocedwalls = 0;

	if (allocedwalls < numwalls + 1)
	{
		allocedwalls += MAX_TRANSPARENTWALL;
		Z_Realloc(wallinfo, allocedwalls * sizeof (*wallinfo), PU_LEVEL, &wallinfo);
	}

	M_Memcpy(wallinfo[numwalls].wallVerts, wallVerts, sizeof (wallinfo[numwalls].wallVerts));
	M_Memcpy(&wallinfo[numwalls].Surf, pSurf, sizeof (FSurfaceInfo));
	wallinfo[numwalls].texnum = texnum;
	wallinfo[numwalls].blend = blend;
	wallinfo[numwalls].drawcount = drawcount++;
	wallinfo[numwalls].fogwall = fogwall;
	wallinfo[numwalls].lightlevel = lightlevel;
	wallinfo[numwalls].wallcolormap = wallcolormap;
	numwalls++;
}

void HWR_RenderWall(FOutVector *wallVerts, FSurfaceInfo *pSurf, FBITFIELD blend, boolean fogwall, INT32 lightlevel, extracolormap_t *wallcolormap)
{
	FBITFIELD blendmode = blend;
	UINT8 alpha = pSurf->PolyColor.s.alpha; // retain the alpha

	INT32 shader = SHADER_NONE;

	if (!r_renderwalls)
		return;

	// Lighting is done here instead so that fog isn't drawn incorrectly on transparent walls after sorting
	HWR_Lighting(pSurf, lightlevel, wallcolormap);

	pSurf->PolyColor.s.alpha = alpha; // put the alpha back after lighting

	if (blend & PF_Environment)
		blendmode |= PF_Occlude;	// PF_Occlude must be used for solid objects

	if (HWR_UseShader())
	{
		if (fogwall)
			shader = SHADER_FOG;
		else
			shader = SHADER_WALL;

		blendmode |= PF_ColorMapped;
	}

	if (fogwall)
		blendmode |= PF_Fog;

	blendmode |= PF_Modulated;	// No PF_Occlude means overlapping (incorrect) transparency
	HWR_ProcessPolygon(pSurf, wallVerts, 4, blendmode, shader, false);
}

INT32 HWR_GetTextureUsed(void)
{
	return HWD.pfnGetTextureUsed();
}

void HWR_DoPostProcessor(player_t *player)
{
	postimg_t *type;

	HWD.pfnUnSetShader();

	if (splitscreen && player == &players[secondarydisplayplayer])
		type = &postimgtype2;
	else
		type = &postimgtype;

	// Armageddon Blast Flash!
	// Could this even be considered postprocessor?
	if (player->flashcount && !HWR_ShouldUsePaletteRendering())
	{
		FOutVector      v[4];
		FSurfaceInfo Surf;

		v[0].x = v[2].y = v[3].x = v[3].y = -4.0f;
		v[0].y = v[1].x = v[1].y = v[2].x = 4.0f;
		v[0].z = v[1].z = v[2].z = v[3].z = 4.0f; // 4.0 because of the same reason as with the sky, just after the screen is cleared so near clipping plane is 3.99

		// This won't change if the flash palettes are changed unfortunately, but it works for its purpose
		if (player->flashpal == PAL_NUKE)
		{
			Surf.PolyColor.s.red = 0xff;
			Surf.PolyColor.s.green = Surf.PolyColor.s.blue = 0x7F; // The nuke palette is kind of pink-ish
		}
		else
			Surf.PolyColor.s.red = Surf.PolyColor.s.green = Surf.PolyColor.s.blue = 0xff;

		Surf.PolyColor.s.alpha = 0xc0; // match software mode

		HWD.pfnDrawPolygon(&Surf, v, 4, PF_Modulated|PF_Additive|PF_NoTexture|PF_NoDepthTest);
	}

	// Capture the screen for intermission and screen waving
#ifdef PS2_PROFILE // PS2-HW-16: a capture costs 1280 GS blocks (a third of the texture pool): only when it is read (the water/heat redraw below, the last frame of a level)
	if (gamestate != GS_INTERMISSION && (lastdraw || *type == postimg_water || *type == postimg_heat))
#else
	if(gamestate != GS_INTERMISSION)
#endif
		HWD.pfnMakeScreenTexture(HWD_SCREENTEXTURE_GENERIC1);

	if (splitscreen) // Not supported in splitscreen - someone want to add support?
		return;

	// Drunken vision! WooOOooo~
	if (*type == postimg_water || *type == postimg_heat)
	{
		// 10 by 10 grid. 2 coordinates (xy)
		float v[SCREENVERTS][SCREENVERTS][2];
		float disStart = (leveltime-1) + FIXED_TO_FLOAT(rendertimefrac);

		UINT8 x, y;
		INT32 WAVELENGTH;
		INT32 AMPLITUDE;
		INT32 FREQUENCY;

		// Modifies the wave.
		if (*type == postimg_water)
		{
			WAVELENGTH = 5;
			AMPLITUDE = 40;
			FREQUENCY = 8;
		}
		else
		{
			WAVELENGTH = 10;
			AMPLITUDE = 60;
			FREQUENCY = 4;
		}

		for (x = 0; x < SCREENVERTS; x++)
		{
			for (y = 0; y < SCREENVERTS; y++)
			{
				// Change X position based on its Y position.
				v[x][y][0] = (x/((float)(SCREENVERTS-1.0f)/9.0f))-4.5f + (float)sin((disStart+(y*WAVELENGTH))/FREQUENCY)/AMPLITUDE;
				v[x][y][1] = (y/((float)(SCREENVERTS-1.0f)/9.0f))-4.5f;
			}
		}
		HWD.pfnPostImgRedraw(v);

		// Capture the screen again for screen waving on the intermission
		if(gamestate != GS_INTERMISSION)
			HWD.pfnMakeScreenTexture(HWD_SCREENTEXTURE_GENERIC1);
	}
	// Flipping of the screen isn't done here anymore
}

void HWR_StartScreenWipe(void)
{
	//CONS_Debug(DBG_RENDER, "In HWR_StartScreenWipe()\n");
	HWD.pfnMakeScreenTexture(HWD_SCREENTEXTURE_WIPE_START);
}

void HWR_EndScreenWipe(void)
{
	//CONS_Debug(DBG_RENDER, "In HWR_EndScreenWipe()\n");
	HWD.pfnMakeScreenTexture(HWD_SCREENTEXTURE_WIPE_END);
}

void HWR_DrawIntermissionBG(void)
{
	HWD.pfnDrawScreenTexture(HWD_SCREENTEXTURE_GENERIC1, NULL, 0);
}

//
// hwr mode wipes
//
static lumpnum_t wipelumpnum;

// puts wipe lumpname in wipename[9]
static boolean HWR_WipeCheck(UINT8 wipenum, UINT8 scrnnum)
{
	static char lumpname[9] = "FADEmmss";
	size_t lsize;

	// not a valid wipe number
	if (wipenum > 99 || scrnnum > 99)
		return false; // shouldn't end up here really, the loop should've stopped running beforehand

	// puts the numbers into the wipename
	lumpname[4] = '0'+(wipenum/10);
	lumpname[5] = '0'+(wipenum%10);
	lumpname[6] = '0'+(scrnnum/10);
	lumpname[7] = '0'+(scrnnum%10);
	wipelumpnum = W_CheckNumForName(lumpname);

	// again, shouldn't be here really
	if (wipelumpnum == LUMPERROR)
		return false;

	lsize = W_LumpLength(wipelumpnum);
	if (!(lsize == 256000 || lsize == 64000 || lsize == 16000 || lsize == 4000))
	{
		CONS_Alert(CONS_WARNING, "Fade mask lump %s of incorrect size, ignored\n", lumpname);
		return false; // again, shouldn't get here if it is a bad size
	}

	return true;
}

void HWR_DoWipe(UINT8 wipenum, UINT8 scrnnum)
{
	if (!HWR_WipeCheck(wipenum, scrnnum))
		return;

	HWR_GetFadeMask(wipelumpnum);
	if (wipestyle == WIPESTYLE_COLORMAP && HWR_UseShader())
	{
		FSurfaceInfo surf = {0};
		FBITFIELD polyflags = PF_Modulated|PF_NoDepthTest;

		polyflags |= (wipestyleflags & WSF_TOWHITE) ? PF_Additive : PF_ReverseSubtract;
		surf.PolyColor.s.red = FADEREDFACTOR;
		surf.PolyColor.s.green = FADEGREENFACTOR;
		surf.PolyColor.s.blue = FADEBLUEFACTOR;
		// polycolor alpha communicates fadein / fadeout to the shader and the backend
		surf.PolyColor.s.alpha = (wipestyleflags & WSF_FADEIN) ? 255 : 0;

		HWD.pfnSetShader(HWR_GetShaderFromTarget(SHADER_UI_TINTED_WIPE));
		HWD.pfnDoScreenWipe(HWD_SCREENTEXTURE_WIPE_START, HWD_SCREENTEXTURE_WIPE_END,
			&surf, polyflags);
		HWD.pfnUnSetShader();
	}
	else
	{
		HWD.pfnDoScreenWipe(HWD_SCREENTEXTURE_WIPE_START, HWD_SCREENTEXTURE_WIPE_END,
			NULL, 0);
	}
}

void HWR_MakeScreenFinalTexture(void)
{
#ifdef PS2_PROFILE // PS2-HW-71: no palette post process on the GS (the CLUTs carry the palette): the frame is its own final picture
	int tex = HWD_SCREENTEXTURE_GENERIC2;
#else
	int tex = HWR_ShouldUsePaletteRendering() ? HWD_SCREENTEXTURE_GENERIC3 : HWD_SCREENTEXTURE_GENERIC2;
#endif
	HWD.pfnMakeScreenTexture(tex);
}

void HWR_DrawScreenFinalTexture(int width, int height)
{
#ifdef PS2_PROFILE
	int tex = HWD_SCREENTEXTURE_GENERIC2;
#else
	int tex = HWR_ShouldUsePaletteRendering() ? HWD_SCREENTEXTURE_GENERIC3 : HWD_SCREENTEXTURE_GENERIC2;
#endif
	HWD.pfnDrawScreenFinalTexture(tex, width, height);
}

#endif // HWRENDER
