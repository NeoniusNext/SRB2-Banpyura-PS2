// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1993-1996 by id Software, Inc.
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2025 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  r_plane.c
/// \brief Here is a core component: drawing the floors and ceilings,
///        while maintaining a per column clipping list only.
///        Moreover, the sky areas have to be determined.

#include "doomdef.h"
#include "console.h"
#include "m_easing.h" // For Easing_InOutSine, used in R_UpdatePlaneRipple
#include "g_game.h"
#include "p_setup.h" // levelflats
#include "p_slopes.h"
#include "r_data.h"
#include "r_textures.h"
#include "r_local.h"
#include "r_state.h"
#include "r_splats.h" // faB(21jan):testing
#include "r_sky.h"
#include "r_portal.h"

#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"
#include "p_tick.h"
#include "r_slopeq.h" // PS2_OPT_SLOPE integer slope setup
#ifdef PS2_OPT_REND
#include "ps2/ps2_rdraw.h"
#endif

//
// opening
//

//SoM: 3/23/2000: Use Boom visplane hashing.

visplane_t *visplanes[MAXVISPLANES];
static visplane_t *freetail;
static visplane_t **freehead = &freetail;

visplane_t *floorplane;
visplane_t *ceilingplane;
static visplane_t *currentplane;

visffloor_t ffloor[MAXFFLOORS];
INT32 numffloors;

//SoM: 3/23/2000: Boom visplane hashing routine.
#define visplane_hash(picnum,lightlevel,height) \
  ((unsigned)((picnum)*3+(lightlevel)+(height)*7) & VISPLANEHASHMASK)

//
// Clip values are the solid pixel bounding the range.
//  floorclip starts out SCREENHEIGHT
//  ceilingclip starts out -1
//
INT16 floorclip[MAXVIDWIDTH], ceilingclip[MAXVIDWIDTH];
fixed_t frontscale[MAXVIDWIDTH];

//
// spanstart holds the start of a plane span
// initialized to 0 at start
//
static INT32 spanstart[MAXVIDHEIGHT];

//
// texture mapping
//
lighttable_t **planezlight;
static fixed_t planeheight;

//added : 10-02-98: yslopetab is what yslope used to be,
//                yslope points somewhere into yslopetab,
//                now (viewheight/2) slopes are calculated above and
//                below the original viewheight for mouselook
//                (this is to calculate yslopes only when really needed)
//                (when mouselookin', yslope is moving into yslopetab)
//                Check R_SetupFrame, R_SetViewSize for more...
fixed_t yslopetab[MAXVIDHEIGHT*16];
fixed_t *yslope;

static INT64 xoffs, yoffs;
#ifndef PS2_OPT_SLOPE
static dvector3_t slope_origin, slope_u, slope_v;
static dvector3_t slope_lightu, slope_lightv;
#endif

static void CalcSlopePlaneVectors(visplane_t *pl, fixed_t xoff, fixed_t yoff);
#ifndef PS2_OPT_SLOPE
static void CalcSlopeLightVectors(pslope_t *slope, fixed_t xpos, fixed_t ypos, double height, float ang, angle_t plangle);

static void DoSlopeCrossProducts(void);
static void DoSlopeLightCrossProduct(void);
#endif

//
// Water ripple effect
// Needs the height of the plane, and the vertical position of the span.
// Sets planeripple.xfrac and planeripple.yfrac, added to ds_xfrac and ds_yfrac, if the span is not tilted.
//

static struct
{
	INT32 offset;
	fixed_t xfrac, yfrac;
	boolean active;
} planeripple;

// ripples da water texture
static fixed_t R_CalculateRippleOffset(INT32 y)
{
	fixed_t distance = FixedMul(planeheight, yslope[y]);
	const INT32 yay = (planeripple.offset + (distance>>9)) & 8191;
	return FixedDiv(FINESINE(yay), (1<<12) + (distance>>11));
}

static void R_CalculatePlaneRipple(angle_t angle)
{
	angle >>= ANGLETOFINESHIFT;
	angle = (angle + 2048) & 8191; // 90 degrees
	planeripple.xfrac = FixedMul(FINECOSINE(angle), ds_bgofs);
	planeripple.yfrac = FixedMul(FINESINE(angle), ds_bgofs);
}

static void R_UpdatePlaneRipple(void)
{
	// ds_waterofs oscillates between 0 and 16384 every other tic
	// Now that frame interpolation is a thing, HOW does it oscillate?
	// The difference between linear interpolation and a sine wave is miniscule here,
	// but a sine wave is ever so slightly smoother and sleeker
	ds_waterofs = Easing_InOutSine(((leveltime & 1)*FRACUNIT) + rendertimefrac,16384,0);

	// Meanwhile, planeripple.offset just counts up, so it gets simple linear interpolation
	planeripple.offset = ((leveltime-1)*140) + ((rendertimefrac*140) / FRACUNIT);
}

#ifdef PS2_OPT_REND
// PS2-85: per-plane constants of R_MapPlane (set by R_DrawSinglePlane before its span loop)
static angle_t mp_planecos, mp_planesin;
static fixed_t mp_xnum, mp_ynum; // FixedMul(planesin/planecos, planeheight)
#endif

static void R_MapPlane(INT32 y, INT32 x1, INT32 x2)
{
#ifdef PS2_OPT_REND
	const angle_t planecos = mp_planecos, planesin = mp_planesin;
#else
	angle_t angle, planecos, planesin;
#endif
	fixed_t distance = 0, span;
	size_t pindex;

#ifdef RANGECHECK
	if (x2 < x1 || x1 < 0 || x2 >= viewwidth || y > viewheight)
		I_Error("R_MapPlane: %d, %d at %d", x1, x2, y);
#endif

	if (x1 >= vid.width)
		x1 = vid.width - 1;

#ifndef PS2_OPT_REND
	angle = (currentplane->viewangle + currentplane->plangle)>>ANGLETOFINESHIFT;
	planecos = FINECOSINE(angle);
	planesin = FINESINE(angle);
#endif

	// [RH] Notice that I dumped the caching scheme used by Doom.
	// It did not offer any appreciable speedup.
	distance = FixedMul(planeheight, yslope[y]);
	span = abs(centery - y);

	if (span) // Don't divide by zero
	{
#ifdef PS2_OPT_REND
		ds_xstep = mp_xnum / span;
		ds_ystep = mp_ynum / span;
#else
		ds_xstep = FixedMul(planesin, planeheight) / span;
		ds_ystep = FixedMul(planecos, planeheight) / span;
#endif
		ds_xstep = FixedMul(currentplane->xscale, ds_xstep);
		ds_ystep = FixedMul(currentplane->yscale, ds_ystep);
	}
	else
		ds_xstep = ds_ystep = FRACUNIT;

	// [RH] Instead of using the xtoviewangle array, I calculated the fractional values
	// at the middle of the screen, then used the calculated ds_xstep and ds_ystep
	// to step from those to the proper texture coordinate to start drawing at.
	// That way, the texture coordinate is always calculated by its position
	// on the screen and not by its position relative to the edge of the visplane.
	ds_xfrac = xoffs + FixedMul(currentplane->xscale, FixedMul(planecos, distance)) + (x1 - centerx) * ds_xstep;
	ds_yfrac = yoffs - FixedMul(currentplane->yscale, FixedMul(planesin, distance)) + (x1 - centerx) * ds_ystep;

	// Water ripple effect
	if (planeripple.active)
	{
		ds_bgofs = R_CalculateRippleOffset(y);

		R_CalculatePlaneRipple(currentplane->viewangle + currentplane->plangle);

		ds_xfrac += FixedMul(planeripple.xfrac, currentplane->xscale);
		ds_yfrac += FixedMul(planeripple.yfrac, currentplane->yscale);
		ds_bgofs >>= FRACBITS;

		if ((y + ds_bgofs) >= viewheight)
			ds_bgofs = viewheight-y-1;
		if ((y + ds_bgofs) < 0)
			ds_bgofs = -y;
	}

	pindex = distance >> LIGHTZSHIFT;
	if (pindex >= MAXLIGHTZ)
		pindex = MAXLIGHTZ - 1;

	ds_colormap = planezlight[pindex];
	if (currentplane->extra_colormap)
		ds_colormap = currentplane->extra_colormap->colormap + (ds_colormap - colormaps);

	ds_y = y;
	ds_x1 = x1;
	ds_x2 = x2;

	spanfunc();
}

static void R_MapTiltedPlane(INT32 y, INT32 x1, INT32 x2)
{
#ifdef RANGECHECK
	if (x2 < x1 || x1 < 0 || x2 >= viewwidth || y > viewheight)
		I_Error("R_MapTiltedPlane: %d, %d at %d", x1, x2, y);
#endif

	if (x1 >= vid.width)
		x1 = vid.width - 1;

	// Water ripple effect
	if (planeripple.active)
	{
		ds_bgofs = R_CalculateRippleOffset(y);

		R_CalculatePlaneRipple(currentplane->viewangle + currentplane->plangle);

		CalcSlopePlaneVectors(currentplane, (xoffs + planeripple.xfrac), (yoffs + planeripple.yfrac));

		ds_bgofs >>= FRACBITS;

		if ((y + ds_bgofs) >= viewheight)
			ds_bgofs = viewheight-y-1;
		if ((y + ds_bgofs) < 0)
			ds_bgofs = -y;
	}

	if (currentplane->extra_colormap)
		ds_colormap = currentplane->extra_colormap->colormap;
	else
		ds_colormap = colormaps;

	ds_y = y;
	ds_x1 = x1;
	ds_x2 = x2;

	spanfunc();
}

static void R_MapFogPlane(INT32 y, INT32 x1, INT32 x2)
{
	fixed_t distance;
	size_t pindex;

#ifdef RANGECHECK
	if (x2 < x1 || x1 < 0 || x2 >= viewwidth || y > viewheight)
		I_Error("R_MapFogPlane: %d, %d at %d", x1, x2, y);
#endif

	if (x1 >= vid.width)
		x1 = vid.width - 1;

	distance = FixedMul(planeheight, yslope[y]);

	pindex = distance >> LIGHTZSHIFT;
	if (pindex >= MAXLIGHTZ)
		pindex = MAXLIGHTZ - 1;

	ds_colormap = planezlight[pindex];
	if (currentplane->extra_colormap)
		ds_colormap = currentplane->extra_colormap->colormap + (ds_colormap - colormaps);

	ds_y = y;
	ds_x1 = x1;
	ds_x2 = x2;

	spanfunc();
}

static void R_MapTiltedFogPlane(INT32 y, INT32 x1, INT32 x2)
{
#ifdef RANGECHECK
	if (x2 < x1 || x1 < 0 || x2 >= viewwidth || y > viewheight)
		I_Error("R_MapTiltedFogPlane: %d, %d at %d", x1, x2, y);
#endif

	if (x1 >= vid.width)
		x1 = vid.width - 1;

	if (currentplane->extra_colormap)
		ds_colormap = currentplane->extra_colormap->colormap;
	else
		ds_colormap = colormaps;

	ds_y = y;
	ds_x1 = x1;
	ds_x2 = x2;

	spanfunc();
}

#ifdef PS2_OPT_REND
// PS2-30: the 3D floor clip arrays are only written by R_RenderSegLoop (slots below numbackffloors), so a clear
// has to touch the slots written since the previous clear, plus everything whenever the view size changed.
INT32 ffloor_clipdirty = MAXFFLOORS; // slots whose clip arrays may differ from the cleared state
static INT32 ffloor_clipw = -1, ffloor_cliph = -1; // view size the cleared state was made for

static void R_ClearFFloorClipArrays(void)
{
	INT32 p, n = ffloor_clipdirty;
	const INT32 w = viewwidth;
	const UINT32 fill = ((UINT32)(UINT16)viewheight << 16) | (UINT16)viewheight;

	if (w != ffloor_clipw || viewheight != ffloor_cliph)
		n = MAXFFLOORS;
	if (n > MAXFFLOORS)
		n = MAXFFLOORS;

	for (p = 0; p < n; p++)
	{
		INT16 *f = ffloor[p].f_clip;
		INT32 i = 0;
		memset(ffloor[p].c_clip, 0xFF, sizeof (INT16) * (size_t)w); // -1
		if (w > 0 && ((size_t)f & 2)) // 32-bit stores need a word-aligned start
			f[i++] = (INT16)viewheight;
		for (; i + 1 < w; i += 2)
			memcpy(f + i, &fill, sizeof fill);
		if (i < w)
			f[i] = (INT16)viewheight;
	}

	ffloor_clipdirty = 0;
	ffloor_clipw = w;
	ffloor_cliph = viewheight;
}
#else
static void R_ClearFFloorClipArrays(void)
{
	INT32 i, p;

	// opening / clipping determination
	for (i = 0; i < viewwidth; i++)
	{
		for (p = 0; p < MAXFFLOORS; p++)
		{
			ffloor[p].f_clip[i] = (INT16)viewheight;
			ffloor[p].c_clip[i] = -1;
		}
	}
}
#endif

void R_ClearFFloorClips (void)
{
	R_ClearFFloorClipArrays();
	numffloors = 0;
}

//
// R_ClearPlanes
// At begining of frame.
//
void R_ClearPlanes(void)
{
	INT32 i;

	// opening / clipping determination
	for (i = 0; i < viewwidth; i++)
	{
		floorclip[i] = (INT16)viewheight;
		ceilingclip[i] = -1;
		frontscale[i] = INT32_MAX;
	}
	R_ClearFFloorClipArrays();

	for (i = 0; i < MAXVISPLANES; i++)
	for (*freehead = visplanes[i], visplanes[i] = NULL;
		freehead && *freehead ;)
	{
		freehead = &(*freehead)->next;
	}
}

static visplane_t *new_visplane(unsigned hash)
{
	visplane_t *check = freetail;
#if defined(PS2) || defined(PS2_PROFILE)
	// Only recycled planes can resize: active planes keep stable addresses.
	if (viewwidth <= 0 || viewwidth > MAXVIDWIDTH)
		I_Error("new_visplane: invalid view width %d", viewwidth);
	if (check)
	{
		freetail = check->next;
		if (!freetail)
			freehead = &freetail;
		if (check->clipwidth < viewwidth)
		{
			Z_Free(check);
			check = NULL;
		}
	}
	if (!check)
	{
		const size_t strip = (size_t)viewwidth + 2;
		const size_t bytes = sizeof (*check) + 2 * strip * sizeof (UINT16);
		check = Z_Malloc(bytes, PU_STATIC, NULL);
		check->top = (UINT16 *)(check + 1) + 1;
		check->bottom = check->top + strip;
		check->clipwidth = viewwidth;
	}
#else
	if (!check)
	{
		check = malloc(sizeof (*check));
		if (check == NULL) I_Error("%s: Out of memory", "new_visplane"); // FIXME: ugly
	}
	else
	{
		freetail = freetail->next;
		if (!freetail)
			freehead = &freetail;
	}
#endif
	check->next = visplanes[hash];
	visplanes[hash] = check;
	return check;
}

static void R_ResetPlaneClip(visplane_t *pl)
{
#if defined(PS2) || defined(PS2_PROFILE)
#ifdef PS2_OPT_REND
	PS2_Fill16(pl->top, 0xFFFF, (size_t)viewwidth);
	PS2_Fill16(pl->bottom, 0, (size_t)viewwidth);
#else
	const size_t bytes = (size_t)viewwidth * sizeof (*pl->top);
	memset(pl->top, 0xff, bytes);
	memset(pl->bottom, 0x00, bytes);
#endif
#else
	memset(pl->top, 0xff, sizeof pl->top);
	memset(pl->bottom, 0x00, sizeof pl->bottom);
#endif
}

//
// R_FindPlane: Seek a visplane having the identical values:
//              Same height, same flattexture, same lightlevel.
//              If not, allocates another of them.
//
visplane_t *R_FindPlane(sector_t *sector, fixed_t height, INT32 picnum, INT32 lightlevel,
	fixed_t xoff, fixed_t yoff, fixed_t xscale, fixed_t yscale,
	angle_t plangle, extracolormap_t *planecolormap,
	ffloor_t *pfloor, polyobj_t *polyobj, pslope_t *slope, sectorportal_t *portalsector)
{
	visplane_t *check;
	unsigned hash;

#ifdef PS2_OPT_REND
	// PS2-82: a zero offset gives zero for any scale (0/s = +-0, converted to 0); in-range floats convert through the
	// 32-bit truncating instruction instead of the libgcc 64-bit routine (same value: both truncate toward zero)
	INT64 offset_x, offset_y;
	if (xoff == 0)
		offset_x = 0;
	else
	{
		const float f = (FixedToFloat(xoff) / FixedToFloat(xscale ? xscale : 1)) * FRACUNIT;
		offset_x = (f > -2147483648.0f && f < 2147483648.0f) ? (INT64)(INT32)f : (INT64)f;
	}
	if (yoff == 0)
		offset_y = 0;
	else
	{
		const float f = (FixedToFloat(yoff) / FixedToFloat(yscale ? yscale : 1)) * FRACUNIT;
		offset_y = (f > -2147483648.0f && f < 2147483648.0f) ? (INT64)(INT32)f : (INT64)f;
	}
#else
	float offset_xd = FixedToFloat(xoff) / FixedToFloat(xscale ? xscale : 1);
	float offset_yd = FixedToFloat(yoff) / FixedToFloat(yscale ? yscale : 1);

	INT64 offset_x = offset_xd * FRACUNIT;
	INT64 offset_y = offset_yd * FRACUNIT;
#endif

	if (!slope) // Don't mess with this right now if a slope is involved
	{
		offset_x += viewx;
		offset_y -= viewy;

		if (plangle != 0)
		{
			// Add the view offset, rotated by the plane angle.
#ifdef PS2_OPT_SLOPE // PS2-16: libm double sin/cos replaced by the integer rotation of r_slopeq.h
			float x = RQ_RoundToFloat24(offset_x) / (float)FRACUNIT;
			float y = RQ_RoundToFloat24(offset_y) / (float)FRACUNIT;
			offset_x = RQ_QToFixed(RQ_Rotate(x, y, RQ_Ang2Rad(plangle), false));
			offset_y = RQ_QToFixed(RQ_Rotate(y, -x, RQ_Ang2Rad(plangle), false));
#else
			float ang = ANG2RAD(plangle);
			float x = offset_x / (float)FRACUNIT;
			float y = offset_y / (float)FRACUNIT;
			offset_x = (x * cos(ang) + y * sin(ang)) * FRACUNIT;
			offset_y = (-x * sin(ang) + y * cos(ang)) * FRACUNIT;
#endif
		}
	}

	if (polyobj)
	{
		if (polyobj->angle != 0)
		{
#ifdef PS2_OPT_SLOPE
			const float ang = RQ_Ang2Rad(polyobj->angle);
			const float x = FixedToFloat(polyobj->centerPt.x);
			const float y = FixedToFloat(polyobj->centerPt.y);
			offset_x = RQ_QToFixed(RQ_FixedToQ(offset_x) - RQ_Rotate(x, y, ang, false));
			offset_y = RQ_QToFixed(RQ_FixedToQ(offset_y) - RQ_Rotate(x, -y, ang, true));
#else
			float ang = ANG2RAD(polyobj->angle);
			float x = FixedToFloat(polyobj->centerPt.x);
			float y = FixedToFloat(polyobj->centerPt.y);
			offset_x -= (x * cos(ang) + y * sin(ang)) * FRACUNIT;
			offset_y -= (x * sin(ang) - y * cos(ang)) * FRACUNIT;
#endif
		}
		else
		{
			offset_x -= polyobj->centerPt.x;
			offset_y += polyobj->centerPt.y;
		}
	}

#ifdef PS2_OPT_REND
	// x * FRACUNIT / FRACUNIT is x (|x| < 2^34 here, no overflow); the libgcc 64-bit multiply is skipped for unit scales
	if (xscale != FRACUNIT)
		offset_x = ((INT64)offset_x * xscale) / FRACUNIT;
	if (yscale != FRACUNIT)
		offset_y = ((INT64)offset_y * yscale) / FRACUNIT;
#else
	offset_x = ((INT64)offset_x * xscale) / FRACUNIT;
	offset_y = ((INT64)offset_y * yscale) / FRACUNIT;
#endif

	// This appears to fix the Nimbus Ruins sky bug.
	if (picnum == skyflatnum && pfloor)
	{
		height = 0; // all skies map together
		lightlevel = 0;
	}

	if (!pfloor)
	{
		hash = visplane_hash(picnum, lightlevel, height);
		for (check = visplanes[hash]; check; check = check->next)
		{
			if (height == check->height && picnum == check->picnum
				&& lightlevel == check->lightlevel
				&& offset_x == check->xoffs && offset_y == check->yoffs
				&& xscale == check->xscale && yscale == check->yscale
				&& planecolormap == check->extra_colormap
				&& check->viewx == viewx && check->viewy == viewy && check->viewz == viewz
				&& check->viewangle == viewangle
				&& check->plangle == plangle
				&& check->slope == slope
				&& check->polyobj == polyobj
				&& P_CompareSectorPortals(check->portalsector, portalsector))
			{
				return check;
			}
		}
	}
	else
	{
		hash = MAXVISPLANES - 1;
	}

	check = new_visplane(hash);

	check->height = height;
	check->picnum = picnum;
	check->lightlevel = lightlevel;
	check->minx = vid.width;
	check->maxx = -1;
	check->xoffs = offset_x;
	check->yoffs = offset_y;
	check->xscale = xscale;
	check->yscale = yscale;
	check->extra_colormap = planecolormap;
	check->ffloor = pfloor;
	check->viewx = viewx;
	check->viewy = viewy;
	check->viewz = viewz;
	check->viewangle = viewangle;
	check->plangle = plangle;
	check->sector = sector;
	check->portalsector = portalsector;
	check->polyobj = polyobj;
	check->slope = slope;

	R_ResetPlaneClip(check);

	return check;
}

//
// R_CheckPlane: return same visplane or alloc a new one if needed
//
visplane_t *R_CheckPlane(visplane_t *pl, INT32 start, INT32 stop)
{
	INT32 intrl, intrh;
	INT32 unionl, unionh;
	INT32 x;

	if (start < pl->minx)
	{
		intrl = pl->minx;
		unionl = start;
	}
	else
	{
		unionl = pl->minx;
		intrl = start;
	}

	if (stop > pl->maxx)
	{
		intrh = pl->maxx;
		unionh = stop;
	}
	else
	{
		unionh = pl->maxx;
		intrh = stop;
	}

	// 0xff is not equal to -1 with shorts...
	for (x = intrl; x <= intrh; x++)
		if (pl->top[x] != 0xffff || pl->bottom[x] != 0x0000)
			break;

	if (x > intrh) /* Can use existing plane; extend range */
	{
		pl->minx = unionl;
		pl->maxx = unionh;
	}
	else /* Cannot use existing plane; create a new one */
	{
		visplane_t *new_pl;
		if (pl->ffloor)
		{
			new_pl = new_visplane(MAXVISPLANES - 1);
		}
		else
		{
			unsigned hash =
				visplane_hash(pl->picnum, pl->lightlevel, pl->height);
			new_pl = new_visplane(hash);
		}

		new_pl->height = pl->height;
		new_pl->picnum = pl->picnum;
		new_pl->lightlevel = pl->lightlevel;
		new_pl->xoffs = pl->xoffs;
		new_pl->yoffs = pl->yoffs;
		new_pl->xscale = pl->xscale;
		new_pl->yscale = pl->yscale;
		new_pl->extra_colormap = pl->extra_colormap;
		new_pl->ffloor = pl->ffloor;
		new_pl->viewx = pl->viewx;
		new_pl->viewy = pl->viewy;
		new_pl->viewz = pl->viewz;
		new_pl->viewangle = pl->viewangle;
		new_pl->plangle = pl->plangle;
		new_pl->sector = pl->sector;
		new_pl->polyobj = pl->polyobj;
		new_pl->slope = pl->slope;
		new_pl->portalsector = pl->portalsector;
		pl = new_pl;
		pl->minx = start;
		pl->maxx = stop;
		R_ResetPlaneClip(pl);
	}
	return pl;
}


//
// R_ExpandPlane
//
// This function basically expands the visplane.
// The reason for this is that when creating 3D floor planes, there is no
// need to create new ones with R_CheckPlane, because 3D floor planes
// are created by subsector and there is no way a subsector can graphically
// overlap.
void R_ExpandPlane(visplane_t *pl, INT32 start, INT32 stop)
{
	// Don't expand polyobject planes here - we do that on our own.
	if (pl->polyobj)
		return;

	if (pl->minx > start) pl->minx = start;
	if (pl->maxx < stop)  pl->maxx = stop;
}

static void R_MakeSpans(void (*mapfunc)(INT32, INT32, INT32), INT32 x, INT32 t1, INT32 b1, INT32 t2, INT32 b2)
{
	//    Alam: from r_splats's R_RasterizeFloorSplat
	if (t1 >= vid.height) t1 = vid.height-1;
	if (b1 >= vid.height) b1 = vid.height-1;
	if (t2 >= vid.height) t2 = vid.height-1;
	if (b2 >= vid.height) b2 = vid.height-1;
	if (x-1 >= vid.width) x = vid.width;

	while (t1 < t2 && t1 <= b1)
	{
		mapfunc(t1, spanstart[t1], x - 1);
		t1++;
	}
	while (b1 > b2 && b1 >= t1)
	{
		mapfunc(b1, spanstart[b1], x - 1);
		b1--;
	}

	while (t2 < t1 && t2 <= b2)
		spanstart[t2++] = x;
	while (b2 > b1 && b2 >= t2)
		spanstart[b2--] = x;
}

void R_DrawPlanes(void)
{
	visplane_t *pl;
	INT32 i;

	if (!r_renderfloors)
		return;

	R_UpdatePlaneRipple();

	for (i = 0; i < MAXVISPLANES; i++, pl++)
	{
		for (pl = visplanes[i]; pl; pl = pl->next)
		{
			if (pl->ffloor != NULL || pl->polyobj != NULL)
				continue;

			R_DrawSinglePlane(pl);
		}
	}
}

// R_DrawSkyPlane
//
// Draws the sky within the plane's top/bottom bounds
// Note: this uses column drawers instead of span drawers, since the sky is always a texture
//
static void R_DrawSkyPlane(visplane_t *pl)
{
	INT32 texture = texturetranslation[skytexture];

	// Reset column drawer function (note: couldn't we just call colfuncs[BASEDRAWFUNC] directly?)
	// (that is, unless we'll need to switch drawers in future for some reason)
	colfunc = colfuncs[BASEDRAWFUNC];

	dc_iscale = skyscale;

	dc_colormap = colormaps;
	dc_texturemid = skytexturemid;
	dc_texheight = textureheight[texture]>>FRACBITS;

	R_CheckTextureCache(texture);

	for (INT32 x = pl->minx; x <= pl->maxx; x++)
	{
		dc_yl = pl->top[x];
		dc_yh = pl->bottom[x];

		if (dc_yl <= dc_yh)
		{
			INT32 angle = (pl->viewangle + xtoviewangle[x])>>ANGLETOSKYSHIFT;
			dc_iscale = FixedMul(skyscale, FINECOSINE(xtoviewangle[x]>>ANGLETOFINESHIFT));
			dc_x = x;
			dc_source = R_GetColumn(texture, -angle)->pixels; // get negative of angle for each column to display sky correct way round! --Monster Iestyn 27/01/18
			colfunc();
		}
	}
#ifdef PS2
	R_ReleaseTextureCache(texture);
#endif
}

#ifndef PS2_OPT_SLOPE
// Returns the height of the sloped plane at (x, y) as a double
static double R_GetSlopeZAt(const pslope_t *slope, INT64 x, INT64 y)
{
	// If you want to reimplement this using just the equation constants, use this instead:
	// (d + a*x + b*y) * -(1.0 / c)

	double px = (x / (double)FRACUNIT) - slope->dorigin.x;
	double py = (y / (double)FRACUNIT) - slope->dorigin.y;

	double dist = (px * slope->dnormdir.x) + (py * slope->dnormdir.y);

	return slope->dorigin.z + (dist * slope->dzdelta);
}

// Sets the texture origin vector of the sloped plane.
static void R_SetSlopePlaneOrigin(pslope_t *slope, fixed_t xpos, fixed_t ypos, fixed_t zpos, INT64 xoff, INT64 yoff, fixed_t angle)
{
	INT64 vx = (INT64)xpos + xoff;
	INT64 vy = (INT64)ypos - yoff;

	float vxf = vx / (float)FRACUNIT;
	float vyf = vy / (float)FRACUNIT;
	float ang = ANG2RAD(ANGLE_270 - angle);

	// slope_origin is the texture origin in view space
	// Don't add in the offsets at this stage, because doing so can result in
	// errors if the flat is rotated.
	slope_origin.x = vxf * cos(ang) - vyf * sin(ang);
	slope_origin.z = vxf * sin(ang) + vyf * cos(ang);
	slope_origin.y = R_GetSlopeZAt(slope, -xoff, yoff) - FixedToDouble(zpos);
}

// This function calculates all of the vectors necessary for drawing a sloped plane.
void R_SetSlopePlane(pslope_t *slope, fixed_t xpos, fixed_t ypos, fixed_t zpos, fixed_t xoff, fixed_t yoff, angle_t angle, angle_t plangle)
{
	// I copied ZDoom's code and adapted it to SRB2... -Red
	double height, z_at_xy;
	float ang;

	if (slope->moved)
	{
		P_CalculateSlopeVectors(slope);
		slope->moved = false;
	}

	R_SetSlopePlaneOrigin(slope, xpos, ypos, zpos, (INT64)xoff, (INT64)yoff, angle);
	height = R_GetSlopeZAt(slope, xpos, ypos);
	zeroheight = height - FixedToDouble(zpos);

	ang = ANG2RAD(ANGLE_180 - (angle + plangle));

	CalcSlopeLightVectors(slope, xpos, ypos, height, ang, plangle);

	if (ds_solidcolor || ds_fog)
	{
		DoSlopeLightCrossProduct();
		return;
	}

	// the v direction vector in view space
	slope_v.x = cos(ang);
	slope_v.z = sin(ang);

	// the u direction vector in view space
	slope_u.x = sin(ang);
	slope_u.z = -cos(ang);

	plangle >>= ANGLETOFINESHIFT;
	z_at_xy = R_GetSlopeZAt(slope, xpos + FINESINE(plangle), ypos + FINECOSINE(plangle));
	slope_v.y = z_at_xy - height;
	z_at_xy = R_GetSlopeZAt(slope, xpos + FINECOSINE(plangle), ypos - FINESINE(plangle));
	slope_u.y = z_at_xy - height;

	DoSlopeCrossProducts();
	DoSlopeLightCrossProduct();
}

// This function calculates all of the vectors necessary for drawing a sloped and scaled plane.
void R_SetScaledSlopePlane(pslope_t *slope, fixed_t xpos, fixed_t ypos, fixed_t zpos, fixed_t xs, fixed_t ys, INT64 xoff, INT64 yoff, angle_t angle, angle_t plangle)
{
	double height, z_at_xy;
	float ang;

	if (slope->moved)
	{
		P_CalculateSlopeVectors(slope);
		slope->moved = false;
	}

	R_SetSlopePlaneOrigin(slope, xpos, ypos, zpos, xoff, yoff, angle);
	height = R_GetSlopeZAt(slope, xpos, ypos);
	zeroheight = height - FixedToDouble(zpos);

	ang = ANG2RAD(ANGLE_180 - (angle + plangle));

	CalcSlopeLightVectors(slope, xpos, ypos, height, ang, plangle);

	if (ds_solidcolor || ds_fog)
	{
		DoSlopeLightCrossProduct();
		return;
	}

	float xscale = FixedToFloat(xs);
	float yscale = FixedToFloat(ys);

	// the v direction vector in view space
	slope_v.x = yscale * cos(ang);
	slope_v.z = yscale * sin(ang);

	// the u direction vector in view space
	slope_u.x = xscale * sin(ang);
	slope_u.z = -xscale * cos(ang);

	ang = ANG2RAD(plangle);
	z_at_xy = R_GetSlopeZAt(slope, xpos + FloatToFixed(yscale * sin(ang)), ypos + FloatToFixed(yscale * cos(ang)));
	slope_v.y = z_at_xy - height;
	z_at_xy = R_GetSlopeZAt(slope, xpos + FloatToFixed(xscale * cos(ang)), ypos - FloatToFixed(xscale * sin(ang)));
	slope_u.y = z_at_xy - height;

	DoSlopeCrossProducts();
	DoSlopeLightCrossProduct();
}

static void CalcSlopeLightVectors(pslope_t *slope, fixed_t xpos, fixed_t ypos, double height, float ang, angle_t plangle)
{
	double z_at_xy;

	slope_lightv.x = cos(ang);
	slope_lightv.z = sin(ang);

	slope_lightu.x = sin(ang);
	slope_lightu.z = -cos(ang);

	plangle >>= ANGLETOFINESHIFT;
	z_at_xy = R_GetSlopeZAt(slope, xpos + FINESINE(plangle), ypos + FINECOSINE(plangle));
	slope_lightv.y = z_at_xy - height;
	z_at_xy = R_GetSlopeZAt(slope, xpos + FINECOSINE(plangle), ypos - FINESINE(plangle));
	slope_lightu.y = z_at_xy - height;
}

static void DoSlopeCrossProducts(void)
{
	DVector3_Cross(&slope_origin, &slope_v, &ds_su);
	DVector3_Cross(&slope_origin, &slope_u, &ds_sv);
	DVector3_Cross(&slope_v, &slope_u, &ds_sz);

	ds_su.z *= focallengthf;
	ds_sv.z *= focallengthf;
	ds_sz.z *= focallengthf;

	if (ds_solidcolor)
		return;

	// Premultiply the texture vectors with the scale factors
	float sfmult = 65536.f;

	if (ds_powersoftwo)
		sfmult *= 1 << nflatshiftup;

	ds_su.x *= sfmult;
	ds_su.y *= sfmult;
	ds_su.z *= sfmult;
	ds_sv.x *= sfmult;
	ds_sv.y *= sfmult;
	ds_sv.z *= sfmult;
}

static void DoSlopeLightCrossProduct(void)
{
	DVector3_Cross(&slope_lightv, &slope_lightu, &ds_slopelight);

	ds_slopelight.z *= focallengthf;
}
#else // PS2_OPT_SLOPE

// PS2-16: the plane vectors come from the integer setup of r_slopeq.h (the EE has no double FPU), converted to the
// single-precision vectors of the span drawers.
static void R_SetSlopePlaneVectors(pslope_t *slope, fixed_t xpos, fixed_t ypos, fixed_t zpos, INT64 xoff, INT64 yoff,
	boolean scaled, fixed_t xs, fixed_t ys, angle_t angle, angle_t plangle)
{
	rq_slope_t q;
	rq_planein_t in;
	rq_planeout_t out;

	if (slope->moved)
	{
		P_CalculateSlopeVectors(slope);
		slope->moved = false;
	}

	memset(&out, 0, sizeof out);
	RQ_LoadSlope(&q, &slope->dorigin, &slope->dnormdir, &slope->dzdelta);
	in.slope = &q;
	in.xpos = xpos;
	in.ypos = ypos;
	in.zpos = zpos;
	in.xoff = xoff;
	in.yoff = yoff;
	in.angle = angle;
	in.plangle = plangle;
	in.plsin = FINESINE(plangle >> ANGLETOFINESHIFT);
	in.plcos = FINECOSINE(plangle >> ANGLETOFINESHIFT);
	in.scaled = scaled;
	in.xscale = scaled ? FixedToFloat(xs) : 1.0f;
	in.yscale = scaled ? FixedToFloat(ys) : 1.0f;
	in.lightonly = (ds_solidcolor || ds_fog);
	in.sfshift = 16 + (ds_powersoftwo ? nflatshiftup : 0);
	in.focallength = focallengthf;
	RQ_SetSlopePlane(&in, &out);

	ds_lightscale = RQ_LightScale(BASEVIDWIDTH * BASEVIDWIDTH / vid.width, out.zeroheight, fovtan);
	ds_slopelight.x = out.light[0];
	ds_slopelight.y = out.light[1];
	ds_slopelight.z = out.light[2];
	if (in.lightonly)
		return;
	ds_su.x = out.su[0];
	ds_su.y = out.su[1];
	ds_su.z = out.su[2];
	ds_sv.x = out.sv[0];
	ds_sv.y = out.sv[1];
	ds_sv.z = out.sv[2];
	ds_sz.x = out.sz[0];
	ds_sz.y = out.sz[1];
	ds_sz.z = out.sz[2];
}

// This function calculates all of the vectors necessary for drawing a sloped plane.
void R_SetSlopePlane(pslope_t *slope, fixed_t xpos, fixed_t ypos, fixed_t zpos, fixed_t xoff, fixed_t yoff, angle_t angle, angle_t plangle)
{
	R_SetSlopePlaneVectors(slope, xpos, ypos, zpos, (INT64)xoff, (INT64)yoff, false, 0, 0, angle, plangle);
}

// This function calculates all of the vectors necessary for drawing a sloped and scaled plane.
void R_SetScaledSlopePlane(pslope_t *slope, fixed_t xpos, fixed_t ypos, fixed_t zpos, fixed_t xs, fixed_t ys, INT64 xoff, INT64 yoff, angle_t angle, angle_t plangle)
{
	R_SetSlopePlaneVectors(slope, xpos, ypos, zpos, xoff, yoff, true, xs, ys, angle, plangle);
}

#endif // PS2_OPT_SLOPE

static void CalcSlopePlaneVectors(visplane_t *pl, fixed_t xoff, fixed_t yoff)
{
	if (!ds_fog && (pl->xscale != FRACUNIT || pl->yscale != FRACUNIT))
	{
		float offset_x = FixedToFloat(xoff) / FixedToFloat(pl->xscale ? pl->xscale : 1);
		float offset_y = FixedToFloat(yoff) / FixedToFloat(pl->yscale ? pl->yscale : 1);
		R_SetScaledSlopePlane(pl->slope, pl->viewx, pl->viewy, pl->viewz,
			FixedDiv(FRACUNIT, pl->xscale), FixedDiv(FRACUNIT, pl->yscale),
			(INT64)(offset_x * FRACUNIT), (INT64)(offset_y * FRACUNIT), pl->viewangle, pl->plangle);
	}
	else
		R_SetSlopePlane(pl->slope, pl->viewx, pl->viewy, pl->viewz, xoff, yoff, pl->viewangle, pl->plangle);
}

static inline void R_AdjustSlopeCoordinates(vector3_t *origin)
{
	const fixed_t modmask = ((1 << (32-nflatshiftup)) - 1);

	fixed_t ox = (origin->x & modmask);
	fixed_t oy = -(origin->y & modmask);

	xoffs &= modmask;
	yoffs &= modmask;

	xoffs -= (origin->x - ox);
	yoffs += (origin->y + oy);
}

static inline void R_AdjustSlopeCoordinatesNPO2(vector3_t *origin)
{
	const fixed_t modmaskw = (ds_flatwidth << FRACBITS);
	const fixed_t modmaskh = (ds_flatheight << FRACBITS);

	fixed_t ox = (origin->x % modmaskw);
	fixed_t oy = -(origin->y % modmaskh);

	xoffs %= modmaskw;
	yoffs %= modmaskh;

	xoffs -= (origin->x - ox);
	yoffs += (origin->y + oy);
}

void R_DrawSinglePlane(visplane_t *pl)
{
	INT32 light = 0;
	INT32 x, stop;
	ffloor_t *rover;
	INT32 spanfunctype = BASEDRAWFUNC;
	void (*mapfunc)(INT32, INT32, INT32);

	if (!(pl->minx <= pl->maxx))
		return;

	// sky flat
	if (pl->picnum == skyflatnum)
	{
		R_DrawSkyPlane(pl);
		return;
	}

	ds_powersoftwo = ds_solidcolor = ds_fog = false;

	planeripple.active = false;

	if (pl->polyobj)
	{
		// Hacked up support for alpha value in software mode Tails 09-24-2002 (sidenote: ported to polys 10-15-2014, there was no time travel involved -Red)
		if (pl->polyobj->translucency >= 10)
			return; // Don't even draw it
		else if (pl->polyobj->translucency > 0)
		{
			spanfunctype = (pl->polyobj->flags & POF_SPLAT) ? SPANDRAWFUNC_TRANSSPLAT : SPANDRAWFUNC_TRANS;
			ds_transmap = R_GetTranslucencyTable(pl->polyobj->translucency);
		}
		else if (pl->polyobj->flags & POF_SPLAT) // Opaque, but allow transparent flat pixels
			spanfunctype = SPANDRAWFUNC_SPLAT;

		if (pl->polyobj->translucency == 0 || (pl->extra_colormap && (pl->extra_colormap->flags & CMF_FOG)))
			light = (max(pl->lightlevel, cv_secbright.value) >> LIGHTSEGSHIFT);
		else // TODO: 2.3: Make transparent polyobject planes always use light level
			light = LIGHTLEVELS-1;
	}
	else
	{
		if (pl->ffloor)
		{
			// Don't draw planes that shouldn't be drawn.
			for (rover = pl->ffloor->target->ffloors; rover; rover = rover->next)
			{
				if ((pl->ffloor->fofflags & FOF_CUTEXTRA) && (rover->fofflags & FOF_EXTRA))
				{
					if (pl->ffloor->fofflags & FOF_EXTRA)
					{
						// The plane is from an extra 3D floor... Check the flags so
						// there are no undesired cuts.
						if (((pl->ffloor->fofflags & (FOF_FOG|FOF_SWIMMABLE)) == (rover->fofflags & (FOF_FOG|FOF_SWIMMABLE)))
							&& pl->height < *rover->topheight
							&& pl->height > *rover->bottomheight)
							return;
					}
				}
			}

			if (pl->ffloor->fofflags & FOF_TRANSLUCENT)
			{
				spanfunctype = (pl->ffloor->fofflags & FOF_SPLAT) ? SPANDRAWFUNC_TRANSSPLAT : SPANDRAWFUNC_TRANS;

				// Hacked up support for alpha value in software mode Tails 09-24-2002
				// ...unhacked by toaster 04-01-2021, re-hacked a little by sphere 19-11-2021
				{
					INT32 trans = (10*((256+12) - pl->ffloor->alpha))/255;
					if (trans >= 10)
						return; // Don't even draw it
					if (pl->ffloor->blend) // additive, (reverse) subtractive, modulative
						ds_transmap = R_GetBlendTable(pl->ffloor->blend, trans);
					else if (!(ds_transmap = R_GetTranslucencyTable(trans)) || trans == 0)
						spanfunctype = SPANDRAWFUNC_SPLAT; // Opaque, but allow transparent flat pixels
				}

				if ((spanfunctype == SPANDRAWFUNC_SPLAT) || (pl->extra_colormap && (pl->extra_colormap->flags & CMF_FOG)))
					light = (max(pl->lightlevel, cv_secbright.value) >> LIGHTSEGSHIFT);
				else // TODO: 2.3: Make transparent FOF planes use light level instead of always being fullbright
					light = LIGHTLEVELS-1;
			}
			else if (pl->ffloor->fofflags & FOF_FOG)
			{
				ds_fog = true;
				spanfunctype = SPANDRAWFUNC_FOG;
				light = (max(pl->lightlevel, cv_secbright.value) >> LIGHTSEGSHIFT);
			}
			else light = (max(pl->lightlevel, cv_secbright.value) >> LIGHTSEGSHIFT);

			if (pl->ffloor->fofflags & FOF_RIPPLE && !ds_fog)
			{
				planeripple.active = true;

				if (spanfunctype == SPANDRAWFUNC_TRANS)
				{
					// Copy the current scene, ugh
					INT32 top = pl->high-8;
					INT32 bottom = pl->low+8;

					if (top < 0)
						top = 0;
					if (bottom > vid.height)
						bottom = vid.height;

					spanfunctype = SPANDRAWFUNC_WATER;

					// Only copy the part of the screen we need
					VID_BlitLinearScreen((splitscreen && viewplayer == &players[secondarydisplayplayer]) ? screens[0] + (top+(vid.height>>1))*vid.width : screens[0]+((top)*vid.width), screens[1]+((top)*vid.width),
										 vid.width, bottom-top,
										 vid.width, vid.width);
				}
			}
		}
		else
			light = (max(pl->lightlevel, cv_secbright.value) >> LIGHTSEGSHIFT);
	}

	if (ds_fog)
	{
		// Since all fog planes do is apply a colormap, it's not required
		// to know any information about their textures.
		mapfunc = R_MapFogPlane;
	}
	else
	{
		levelflat_t *levelflat = &levelflats[pl->picnum];

		// Get the texture
		ds_source = (UINT8 *)R_GetFlat(levelflat);
		if (ds_source == NULL)
			return;

		texture_t *texture = textures[R_GetTextureNumForFlat(levelflat)];
		ds_flatwidth = texture->width;
		ds_flatheight = texture->height;

		if (R_CheckSolidColorFlat())
			ds_solidcolor = true;
		else if (R_CheckPowersOfTwo())
		{
			R_SetFlatVars(ds_flatwidth * ds_flatheight);
			ds_powersoftwo = true;
		}

		mapfunc = R_MapPlane;

		if (ds_solidcolor)
		{
			switch (spanfunctype)
			{
				case SPANDRAWFUNC_WATER:
					spanfunctype = SPANDRAWFUNC_WATERSOLID;
					break;
				case SPANDRAWFUNC_TRANS:
					spanfunctype = SPANDRAWFUNC_TRANSSOLID;
					break;
				default:
					spanfunctype = SPANDRAWFUNC_SOLID;
					break;
			}
		}
	}

	xoffs = pl->xoffs;
	yoffs = pl->yoffs;

	if (light >= LIGHTLEVELS)
		light = LIGHTLEVELS-1;

	if (light < 0)
		light = 0;

	if (pl->slope)
	{
		if (ds_fog)
			mapfunc = R_MapTiltedFogPlane;
		else
		{
			mapfunc = R_MapTiltedPlane;

			if (!pl->plangle && !ds_solidcolor && pl->xscale == FRACUNIT && pl->yscale == FRACUNIT)
			{
				if (ds_powersoftwo)
					R_AdjustSlopeCoordinates(&pl->slope->o);
				else
					R_AdjustSlopeCoordinatesNPO2(&pl->slope->o);
			}
		}

		if (!ds_fog && planeripple.active)
			planeheight = abs(P_GetSlopeZAt(pl->slope, pl->viewx, pl->viewy) - pl->viewz);
		else
			CalcSlopePlaneVectors(pl, xoffs, yoffs);

		switch (spanfunctype)
		{
			case SPANDRAWFUNC_WATER:
				spanfunctype = SPANDRAWFUNC_TILTEDWATER;
				break;
			case SPANDRAWFUNC_TRANS:
				spanfunctype = SPANDRAWFUNC_TILTEDTRANS;
				break;
			case SPANDRAWFUNC_SPLAT:
				spanfunctype = SPANDRAWFUNC_TILTEDSPLAT;
				break;
			case SPANDRAWFUNC_TRANSSPLAT:
				spanfunctype = SPANDRAWFUNC_TILTEDTRANSSPLAT;
				break;
			case SPANDRAWFUNC_SOLID:
				spanfunctype = SPANDRAWFUNC_TILTEDSOLID;
				break;
			case SPANDRAWFUNC_TRANSSOLID:
				spanfunctype = SPANDRAWFUNC_TILTEDTRANSSOLID;
				break;
			case SPANDRAWFUNC_WATERSOLID:
				spanfunctype = SPANDRAWFUNC_TILTEDWATERSOLID;
				break;
			case SPANDRAWFUNC_FOG:
				spanfunctype = SPANDRAWFUNC_TILTEDFOG;
				break;
			default:
				spanfunctype = SPANDRAWFUNC_TILTED;
				break;
		}

		planezlight = scalelight[light];
	}
	else
	{
		planeheight = abs(pl->height - pl->viewz);
		planezlight = zlight[light];
	}

	// Set the span drawer
	if (!ds_powersoftwo)
	{
		if (spanfuncs_npo2[spanfunctype])
			spanfunc = spanfuncs_npo2[spanfunctype];
		else
			spanfunc = spanfuncs[spanfunctype];
	}
	else
		spanfunc = spanfuncs[spanfunctype];

	// set the maximum value for unsigned
	pl->top[pl->maxx+1] = 0xffff;
	pl->top[pl->minx-1] = 0xffff;
	pl->bottom[pl->maxx+1] = 0x0000;
	pl->bottom[pl->minx-1] = 0x0000;

	currentplane = pl;
	stop = pl->maxx + 1;
#ifdef PS2_OPT_REND
	{
		const angle_t a = (pl->viewangle + pl->plangle)>>ANGLETOFINESHIFT;
		mp_planecos = FINECOSINE(a);
		mp_planesin = FINESINE(a);
		mp_xnum = FixedMul(mp_planesin, planeheight);
		mp_ynum = FixedMul(mp_planecos, planeheight);
	}
#endif

#ifdef PS2_OPT_REND
	// PS2-85: R_MakeSpans does nothing when the column has the same top and bottom as the previous one (all four loops
	// fail their first test), so those calls are skipped; the previous column is carried instead of reloaded
	{
		INT32 t1 = pl->top[pl->minx-1], b1 = pl->bottom[pl->minx-1];
		for (x = pl->minx; x <= stop; x++)
		{
			const INT32 t2 = pl->top[x], b2 = pl->bottom[x];
			if (t1 != t2 || b1 != b2)
				R_MakeSpans(mapfunc, x, t1, b1, t2, b2);
			t1 = t2;
			b1 = b2;
		}
	}
#else
	for (x = pl->minx; x <= stop; x++)
		R_MakeSpans(mapfunc, x, pl->top[x-1], pl->bottom[x-1], pl->top[x], pl->bottom[x]);
#endif
#ifdef PS2
	// Every span has consumed its flat; the next plane fetches its own source.
	if (!ds_fog)
		R_ReleaseFlatCache(R_GetTextureNumForFlat(&levelflats[pl->picnum]));
#endif
}

void R_PlaneBounds(visplane_t *plane)
{
	INT32 i;
	INT32 hi, low;

	hi = plane->top[plane->minx];
	low = plane->bottom[plane->minx];

	for (i = plane->minx + 1; i <= plane->maxx; i++)
	{
		if (plane->top[i] < hi)
		hi = plane->top[i];
		if (plane->bottom[i] > low)
		low = plane->bottom[i];
	}
	plane->high = hi;
	plane->low = low;
}
