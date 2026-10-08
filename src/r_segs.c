// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1993-1996 by id Software, Inc.
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2024 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  r_segs.c
/// \brief All the clipping: columns, horizontal spans, sky columns

#include "doomdef.h"
#include "r_local.h"
#include "r_main.h"
#include "r_sky.h"

#include "r_portal.h"

#include "w_wad.h"
#include "z_zone.h"
#include "p_slopes.h"
#include "taglist.h"
#include "ps2_sub.h" // PS2SUB probes (inert without -DPS2_SUBPROF)
#ifdef PS2_OPT_REND
#include "ps2/ps2_rdraw.h"
#endif
#include "r_slopeq.h" // experimental PS2_OPT_SEGS replacements of the double code

// OPTIMIZE: closed two sided lines as single sided

// True if any of the segs textures might be visible.
static boolean segtextured;
static boolean markfloor; // False if the back side is the same plane.
static boolean markceiling;

static boolean maskedtexture;
static INT32 toptexture, bottomtexture, midtexture;
static INT32 numthicksides, numbackffloors;
#ifdef PS2_OPT_REND
extern INT32 ffloor_clipdirty; // r_plane.c
#endif

angle_t rw_normalangle;
// angle to line origin
angle_t rw_angle1;
fixed_t rw_distance;

//
// regular wall
//
static INT32 rw_x, rw_stopx;
static angle_t rw_centerangle;
static fixed_t rw_offset, rw_offsetx;
static fixed_t rw_offset_top, rw_offset_mid, rw_offset_bottom;
static fixed_t rw_scale, rw_scalestep;
static fixed_t rw_midtexturemid, rw_toptexturemid, rw_bottomtexturemid;
static INT32 worldtop, worldbottom, worldhigh, worldlow;
static INT32 worldtopslope, worldbottomslope, worldhighslope, worldlowslope; // worldtop/bottom at end of slope
static fixed_t rw_toptextureslide, rw_midtextureslide, rw_bottomtextureslide; // Defines how to adjust Y offsets along the wall for slopes
static fixed_t rw_midtextureback, rw_midtexturebackslide; // Values for masked midtexture height calculation
static fixed_t rw_midtexturescalex, rw_midtexturescaley;
static fixed_t rw_toptexturescalex, rw_toptexturescaley;
static fixed_t rw_bottomtexturescalex, rw_bottomtexturescaley;
static fixed_t rw_invmidtexturescalex, rw_invtoptexturescalex, rw_invbottomtexturescalex;

// Lactozilla: 3D floor clipping
static boolean rw_floormarked = false;
static boolean rw_ceilingmarked = false;

static INT32 *rw_silhouette = NULL;
static fixed_t *rw_tsilheight = NULL;
static fixed_t *rw_bsilheight = NULL;

static fixed_t pixhigh, pixlow, pixhighstep, pixlowstep;
static fixed_t topfrac, topstep;
static fixed_t bottomfrac, bottomstep;

static lighttable_t **walllights;
static fixed_t *maskedtexturecol = NULL;
static fixed_t *maskedtextureheight = NULL;
static fixed_t *thicksidecol = NULL;
static fixed_t *invscale = NULL;

static boolean texcoltables;

//SoM: 3/23/2000: Use boom opening limit removal
static size_t numopenings;
static INT16 *openings, *lastopening;

static size_t texturecolumntablesize;
static fixed_t *texturecolumntable, *curtexturecolumntable;

void R_ClearSegTables(void)
{
	lastopening = openings;
	curtexturecolumntable = texturecolumntable;
}

static size_t maxdrawsegs = 0; // (was local to R_StoreWallRange; PS2-172 needs it for R_ReleaseDrawSegScales)

#ifdef PS2_PROFILE
// PS2-172 (OPT11-STAB): the scale arrays of the drawsegs that own FOF planes grow to the busiest view of the session (874 KB after the FOF-heavy
// maps of a campaign) and stayed for ever: a new level starts without them (called by P_LoadLevel after the caches were flushed; no frame is drawing).
void R_ReleaseDrawSegScales(void)
{
	size_t i;

	if (!drawsegs)
		return;
	for (i = 0; i < maxdrawsegs; i++)
		if (drawsegs[i].frontscale)
		{
			Z_Free(drawsegs[i].frontscale);
			drawsegs[i].frontscale = NULL;
			drawsegs[i].frontscalewidth = 0;
		}
	// PS2-179: and the array of the drawsegs itself (128 entries grown by half to the busiest view: 0.6 MB after MAPF2, a block of the long-lived side that no level
	// ever released); the first wall of the next level makes it again, as at the start of the game
	Z_Free(drawsegs);
	drawsegs = ds_p = curdrawsegs = NULL;
	firstseg = NULL;
	maxdrawsegs = 0;
}

void R_AllocDrawSegFrontScale(drawseg_t *ds)
{
	// Retain each slot's old values and high-water view width between frames.
	// No zone owner points inside drawsegs: the array itself may move on growth.
	if (viewwidth < 1 || viewwidth > MAXVIDWIDTH)
		I_Error("R_AllocDrawSegFrontScale: invalid view width");
	if (ds->frontscalewidth < viewwidth)
	{
		ds->frontscale = Z_Realloc(ds->frontscale,
			(size_t)viewwidth * sizeof (*ds->frontscale), PU_STATIC, NULL);
		ds->frontscalewidth = viewwidth;
	}
}
#endif

transnum_t R_GetLinedefTransTable(fixed_t alpha)
{
	return (20*(FRACUNIT - alpha - 1) + FRACUNIT) >> (FRACBITS+1);
}

static UINT8 R_SideLightLevel(side_t *side, INT16 base_lightlevel)
{
#ifdef SIDE_UDMF
	return max(max(0, cv_secbright.value), min(255, side->light +
		((side->lightabsolute) ? 0 : base_lightlevel)));
#else
	(void)side; // PS2-OPT-02: light 0, relative
	return max(max(0, cv_secbright.value), min(255, base_lightlevel));
#endif
}

/* TODO: implement per-texture lighting
static UINT8 R_TopLightLevel(side_t *side, INT16 base_lightlevel)
{
	return max(0, min(255, side->light_top +
		((side->lightabsolute_top) ? 0 : R_SideLightLevel(side, base_lightlevel))));
}

static UINT8 R_MidLightLevel(side_t *side, INT16 base_lightlevel)
{
	return max(0, min(255, side->light_mid +
		((side->lightabsolute_mid) ? 0 : R_SideLightLevel(side, base_lightlevel))));
}

static UINT8 R_BottomLightLevel(side_t *side, INT16 base_lightlevel)
{
	return max(0, min(255, side->light_bottom +
		((side->lightabsolute_bottom) ? 0 : R_SideLightLevel(side, base_lightlevel))));
}
*/

// If we have a multi-patch texture on a 2sided wall (rare) then we draw
//  it using R_DrawColumn, else we draw it using R_DrawMaskedColumn, this
//  way we don't have to store extra post_t info with each column for
//  multi-patch textures. They are not normally needed as multi-patch
//  textures don't have holes in it. At least not for now.
static void R_Render2sidedMultiPatchColumn(column_t *column, unsigned lengthcol)
{
	INT32 topscreen, bottomscreen;

	post_t *post = &column->posts[0];
	if (!post->length)
		return;

	topscreen = sprtopscreen;
	bottomscreen = topscreen + spryscale * lengthcol;

	dc_yl = (sprtopscreen+FRACUNIT-1)>>FRACBITS;
	dc_yh = (bottomscreen-1)>>FRACBITS;

	if (windowtop != INT32_MAX && windowbottom != INT32_MAX)
	{
		dc_yl = ((windowtop + FRACUNIT)>>FRACBITS);
		dc_yh = (windowbottom - 1)>>FRACBITS;
	}

	if (dc_yh >= mfloorclip[dc_x])
		dc_yh =  mfloorclip[dc_x] - 1;
	if (dc_yl <= mceilingclip[dc_x])
		dc_yl =  mceilingclip[dc_x] + 1;

	if (dc_yl >= vid.height || dc_yh < 0)
		return;

	if (dc_yl <= dc_yh && dc_yh < vid.height && dc_yh > 0)
	{
		dc_source = column->pixels + post->data_offset;
		dc_postlength = post->length;

		if (colfunc == colfuncs[BASEDRAWFUNC])
			(colfuncs[COLDRAWFUNC_TWOSMULTIPATCH])();
		else if (colfunc == colfuncs[COLDRAWFUNC_FUZZY])
			(colfuncs[COLDRAWFUNC_TWOSMULTIPATCHTRANS])();
		else
			colfunc();
	}
}

static void R_RenderFlipped2sidedMultiPatchColumn(column_t *column, unsigned lengthcol)
{
	INT32 topscreen, bottomscreen;

	void (*localcolfunc)(void);

	post_t *post = &column->posts[0];
	if (!post->length)
		return;

	topscreen = sprtopscreen;
	bottomscreen = topscreen + spryscale * lengthcol;

	dc_yl = (sprtopscreen+FRACUNIT-1)>>FRACBITS;
	dc_yh = (bottomscreen-1)>>FRACBITS;

	if (windowtop != INT32_MAX && windowbottom != INT32_MAX)
	{
		dc_yl = ((windowtop + FRACUNIT)>>FRACBITS);
		dc_yh = (windowbottom - 1)>>FRACBITS;
	}

	if (dc_yh >= mfloorclip[dc_x])
		dc_yh =  mfloorclip[dc_x] - 1;
	if (dc_yl <= mceilingclip[dc_x])
		dc_yl =  mceilingclip[dc_x] + 1;

	if (dc_yl >= vid.height || dc_yh < 0)
		return;

	if (dc_yl <= dc_yh && dc_yh < vid.height && dc_yh > 0)
	{
		dc_postlength = post->length;

		if (colfunc == colfuncs[BASEDRAWFUNC])
			localcolfunc = colfuncs[COLDRAWFUNC_TWOSMULTIPATCH];
		else if (colfunc == colfuncs[COLDRAWFUNC_FUZZY])
			localcolfunc = colfuncs[COLDRAWFUNC_TWOSMULTIPATCHTRANS];
		else
			localcolfunc = colfunc;

		R_DrawFlippedPost(column->pixels + post->data_offset, post->length, localcolfunc);
	}
}

void R_RenderMaskedSegRange(drawseg_t *ds, INT32 x1, INT32 x2)
{
#ifdef PS2_OPT_REND
#define LRFIX lightresfix
#else
#define LRFIX LIGHTRESOLUTIONFIX
#endif
	size_t pindex;
	column_t *col;
	INT32 lightnum, texnum, i;
	fixed_t height, realbot;
	lightlist_t *light;
	r_lightlist_t *rlight;
	void (*colfunc_2s)(column_t *, unsigned);
	line_t *ldef;
	sector_t *front, *back;
	INT32 times, repeats;
	INT64 overflow_test;
	INT32 range;
	UINT8 vertflip;
	unsigned lengthcol;

	fixed_t wall_scaley;
	fixed_t scalestep;
	fixed_t scale1;

	// Calculate light table.
	// Use different light tables
	//   for horizontal / vertical / diagonal. Diagonal?
	// OPTIMIZE: get rid of LIGHTSEGSHIFT globally
	curline = ds->curline;

	frontsector = curline->frontsector;
	backsector = curline->backsector;
	sidedef = curline->sidedef;
	texnum = R_GetTextureNum(sidedef->midtexture);
	windowbottom = windowtop = sprbotscreen = INT32_MAX;

	ldef = curline->linedef;
	if (!ldef->alpha)
		return;

	if (ldef->blendmode == AST_FOG)
	{
		colfunc = colfuncs[COLDRAWFUNC_FOG];
		windowtop = frontsector->ceilingheight;
		windowbottom = frontsector->floorheight;
	}
	else if (ldef->blendmode)
	{
		if (ldef->alpha == NUMTRANSMAPS || ldef->blendmode == AST_MODULATE)
			dc_transmap = R_GetBlendTable(ldef->blendmode, 0);
		else
			dc_transmap = R_GetBlendTable(ldef->blendmode, R_GetLinedefTransTable(ldef->alpha));
		colfunc = colfuncs[COLDRAWFUNC_FUZZY];
	}
	else if (ldef->alpha > 0 && ldef->alpha < FRACUNIT)
	{
		dc_transmap = R_GetTranslucencyTable(R_GetLinedefTransTable(ldef->alpha));
		colfunc = colfuncs[COLDRAWFUNC_FUZZY];
	}
	else
		colfunc = colfuncs[BASEDRAWFUNC];

	if (curline->polyseg && curline->polyseg->translucency > 0)
	{
		if (curline->polyseg->translucency >= NUMTRANSMAPS)
			return;

		dc_transmap = R_GetTranslucencyTable(curline->polyseg->translucency);
		colfunc = colfuncs[COLDRAWFUNC_FUZZY];
	}

	vertflip = textures[texnum]->flip & 2;

	wall_scaley = SIDE_SCALEY_MID(sidedef);
	if (wall_scaley < 0)
	{
		wall_scaley = -wall_scaley;
		vertflip = !vertflip;
	}

#ifdef PS2_OPT_REND
	// PS2-86: FixedDiv(x, FRACUNIT) is x unless |x| >= 2^30 (saturation)
	if (wall_scaley == FRACUNIT && ds->scalestep > -0x40000000 && ds->scalestep < 0x40000000)
		scalestep = ds->scalestep;
	else
		scalestep = FixedDiv(ds->scalestep, wall_scaley);
	if (wall_scaley == FRACUNIT && ds->scale1 > -0x40000000 && ds->scale1 < 0x40000000)
		scale1 = ds->scale1;
	else
		scale1 = FixedDiv(ds->scale1, wall_scaley);
#else
	scalestep = FixedDiv(ds->scalestep, wall_scaley);
	scale1 = FixedDiv(ds->scale1, wall_scaley);
#endif

	range = max(ds->x2-ds->x1, 1);
	rw_scalestep = scalestep;
	spryscale = scale1 + (x1 - ds->x1)*rw_scalestep;

	// Texture must be cached
	R_CheckTextureCache(texnum);

	// handle case where multipatch texture is drawn on a 2sided wall, multi-patch textures
	// are not stored per-column with post info in SRB2
	if (!textures[texnum]->transparency)
	{
		if (vertflip) // vertically flipped?
			colfunc_2s = R_RenderFlipped2sidedMultiPatchColumn;
		else
			colfunc_2s = R_Render2sidedMultiPatchColumn;
	}
	else if (vertflip) // vertically flipped?
		colfunc_2s = R_DrawFlippedMaskedColumn;
	else
		colfunc_2s = R_DrawMaskedColumn; // render the usual 2sided single-patch packed texture

	lengthcol = textures[texnum]->height;

	// Setup lighting based on the presence/lack-of 3D floors.
	dc_numlights = 0;
	if (frontsector->numlights)
	{
		dc_numlights = frontsector->numlights;
		if (dc_numlights > dc_maxlights)
		{
			dc_maxlights = dc_numlights;
			dc_lightlist = Z_Realloc(dc_lightlist, sizeof (*dc_lightlist) * dc_maxlights, PU_STATIC, NULL);
		}

		for (i = 0; i < dc_numlights; i++)
		{
			fixed_t leftheight, rightheight;
			light = &frontsector->lightlist[i];
			rlight = &dc_lightlist[i];
			leftheight  = P_GetLightZAt(light, ds-> leftpos.x, ds-> leftpos.y);
			rightheight = P_GetLightZAt(light, ds->rightpos.x, ds->rightpos.y);

			leftheight  -= viewz;
			rightheight -= viewz;

			rlight->height     = (centeryfrac) - FixedMul(leftheight , ds->scale1);
			rlight->heightstep = (centeryfrac) - FixedMul(rightheight, ds->scale2);
			rlight->heightstep = (rlight->heightstep-rlight->height)/(range);
			//if (x1 > ds->x1)
				//rlight->height -= (x1 - ds->x1)*rlight->heightstep;
			rlight->startheight = rlight->height; // keep starting value here to reset for each repeat
			rlight->lightlevel = *light->lightlevel;
			rlight->extra_colormap = *light->extra_colormap;
			rlight->flags = light->flags;

			if ((colfunc != colfuncs[COLDRAWFUNC_FUZZY])
				|| (rlight->flags & FOF_FOG)
				|| (rlight->extra_colormap && (rlight->extra_colormap->flags & CMF_FOG)))
				lightnum = R_SideLightLevel(curline->sidedef, rlight->lightlevel) >> LIGHTSEGSHIFT;
			else
				lightnum = LIGHTLEVELS - 1;

			if (rlight->extra_colormap && (rlight->extra_colormap->flags & CMF_FOG))
				;
			else if (curline->v1->y == curline->v2->y)
				lightnum--;
			else if (curline->v1->x == curline->v2->x)
				lightnum++;

			rlight->lightnum = lightnum;
		}
	}
	else
	{
		if ((colfunc != colfuncs[COLDRAWFUNC_FUZZY])
			|| (frontsector->extra_colormap && (frontsector->extra_colormap->flags & CMF_FOG)))
			lightnum = R_SideLightLevel(curline->sidedef, frontsector->lightlevel) >> LIGHTSEGSHIFT;
		else
			lightnum = LIGHTLEVELS - 1;

		if (colfunc == colfuncs[COLDRAWFUNC_FOG]
			|| (frontsector->extra_colormap && (frontsector->extra_colormap->flags & CMF_FOG)))
			;
		else if (curline->v1->y == curline->v2->y)
			lightnum--;
		else if (curline->v1->x == curline->v2->x)
			lightnum++;

		if (lightnum < 0)
			walllights = scalelight[0];
		else if (lightnum >= LIGHTLEVELS)
			walllights = scalelight[LIGHTLEVELS - 1];
		else
			walllights = scalelight[lightnum];
	}

	maskedtexturecol = ds->maskedtexturecol;

	mfloorclip = ds->sprbottomclip;
	mceilingclip = ds->sprtopclip;

	if (frontsector->heightsec != -1)
		front = &sectors[frontsector->heightsec];
	else
		front = frontsector;

	if (backsector->heightsec != -1)
		back = &sectors[backsector->heightsec];
	else
		back = backsector;

	if (sidedef->repeatcnt)
		repeats = 1 + sidedef->repeatcnt;
	else if (ldef->flags & ML_WRAPMIDTEX)
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

		repeats = (high - low)/textureheight[texnum];
		if ((high-low)%textureheight[texnum])
			repeats++; // tile an extra time to fill the gap -- Monster Iestyn
	}
	else
		repeats = 1;

	for (times = 0; times < repeats; times++)
	{
		if (times > 0)
		{
			rw_scalestep = scalestep;
			spryscale = scale1 + (x1 - ds->x1)*rw_scalestep;

			// reset all lights to their starting heights
			for (i = 0; i < dc_numlights; i++)
			{
				rlight = &dc_lightlist[i];
				rlight->height = rlight->startheight;
			}
		}

		dc_texheight = textureheight[texnum]>>FRACBITS;

#ifdef PS2_OPT_REND
		// PS2-86: loop invariants of the column loop (nothing in it writes them): the texture height, the MIDPEG flag,
		// the light-resolution factor (640*fovtan/vid.width is an integer division) and the (32x32 -> 64) overflow product
		// done with one mult instead of a libgcc 64-bit multiply
		const fixed_t texh = textureheight[texnum];
		const boolean midpeg = (curline->linedef->flags & ML_MIDPEG) != 0;
		const fixed_t lightresfix = LIGHTRESOLUTIONFIX;
#endif

		// draw the columns
		for (dc_x = x1; dc_x <= x2; dc_x++)
		{
			dc_texturemid = ds->maskedtextureheight[dc_x];

#ifdef PS2_OPT_REND
			if (midpeg)
				dc_texturemid += texh*times + texh;
			else
				dc_texturemid -= texh*times;

			overflow_test = (INT64)centeryfrac - (PS2_MulS32(dc_texturemid, spryscale)>>FRACBITS);
#else
			if (curline->linedef->flags & ML_MIDPEG)
				dc_texturemid += (textureheight[texnum])*times + textureheight[texnum];
			else
				dc_texturemid -= (textureheight[texnum])*times;

			// Check for overflows first
			overflow_test = (INT64)centeryfrac - (((INT64)dc_texturemid*spryscale)>>FRACBITS);
#endif
			if (overflow_test < 0) overflow_test = -overflow_test;
			if ((UINT64)overflow_test&0xFFFFFFFF80000000ULL)
			{
				// Eh, no, go away, don't waste our time
				if (dc_numlights)
				{
					for (i = 0; i < dc_numlights; i++)
					{
						rlight = &dc_lightlist[i];
						rlight->height += rlight->heightstep;
					}
				}
				spryscale += rw_scalestep;
				continue;
			}

			// calculate lighting
			if (dc_numlights)
			{
				lighttable_t **xwalllights;

				sprtopscreen = windowtop = (centeryfrac - FixedMul(dc_texturemid, spryscale));

				realbot = FixedMul(textureheight[texnum], spryscale) + sprtopscreen;
				dc_iscale = FixedMul(ds->invscale[dc_x], wall_scaley);

				windowbottom = realbot;

				// draw the texture
				col = R_GetColumn(texnum, maskedtexturecol[dc_x] >> FRACBITS);

				for (i = 0; i < dc_numlights; i++)
				{
					rlight = &dc_lightlist[i];

					if ((rlight->flags & FOF_NOSHADE))
						continue;

					if (rlight->lightnum < 0)
						xwalllights = scalelight[0];
					else if (rlight->lightnum >= LIGHTLEVELS)
						xwalllights = scalelight[LIGHTLEVELS-1];
					else
						xwalllights = scalelight[rlight->lightnum];

					pindex = FixedMul(spryscale, LRFIX)>>LIGHTSCALESHIFT;

					if (pindex >= MAXLIGHTSCALE)
						pindex = MAXLIGHTSCALE - 1;

					if (rlight->extra_colormap)
						rlight->rcolormap = rlight->extra_colormap->colormap + (xwalllights[pindex] - colormaps);
					else
						rlight->rcolormap = xwalllights[pindex];

					height = rlight->height;
					rlight->height += rlight->heightstep;

					if (height <= windowtop)
					{
						dc_colormap = rlight->rcolormap;
						continue;
					}

					windowbottom = height;
					if (windowbottom >= realbot)
					{
						windowbottom = realbot;
						colfunc_2s(col, lengthcol);
						for (i++; i < dc_numlights; i++)
						{
							rlight = &dc_lightlist[i];
							rlight->height += rlight->heightstep;
						}

						continue;
					}
					colfunc_2s(col, lengthcol);
					windowtop = windowbottom + 1;
					dc_colormap = rlight->rcolormap;
				}
				windowbottom = realbot;
				if (windowtop < windowbottom)
					colfunc_2s(col, lengthcol);

				spryscale += rw_scalestep;
				continue;
			}

			// calculate lighting
			pindex = FixedMul(spryscale, LRFIX)>>LIGHTSCALESHIFT;

			if (pindex >= MAXLIGHTSCALE)
				pindex = MAXLIGHTSCALE - 1;

			dc_colormap = walllights[pindex];

			if (frontsector->extra_colormap)
				dc_colormap = frontsector->extra_colormap->colormap + (dc_colormap - colormaps);

			sprtopscreen = centeryfrac - FixedMul(dc_texturemid, spryscale);
			dc_iscale = FixedMul(ds->invscale[dc_x], wall_scaley);

			// draw the texture
			col = R_GetColumn(texnum, maskedtexturecol[dc_x] >> FRACBITS);
			colfunc_2s(col, lengthcol);

			spryscale += rw_scalestep;
		}
	}
	colfunc = colfuncs[BASEDRAWFUNC];
}
#undef LRFIX

// Loop through R_DrawMaskedColumn calls
static fixed_t repeatscroll = 0;

static void R_DrawRepeatMaskedColumn(column_t *col, unsigned lengthcol)
{
	fixed_t topscreen = sprtopscreen;
	fixed_t bottomscreen = sprbotscreen;

	fixed_t texheight = dc_texheight*spryscale;

	fixed_t scroll = -repeatscroll;
	if (scroll < 0)
	{
		scroll = -FixedMul((abs(scroll) % (dc_texheight*FRACUNIT)), spryscale);
		bottomscreen += texheight; // Draw an extra time
	}
	else if (scroll)
	{
		scroll = FixedMul(scroll % (dc_texheight*FRACUNIT), spryscale);
		topscreen -= texheight; // Draw an extra time
	}

	while (topscreen < bottomscreen)
	{
		sprtopscreen = topscreen + scroll;

		R_DrawMaskedColumn(col, lengthcol);

		if ((INT64)sprtopscreen + (INT64)dc_texheight*spryscale > (INT64)INT32_MAX) // prevent overflow
			break;

		topscreen += texheight;
	}

	sprtopscreen = sprbotscreen;
}

static void R_DrawRepeatFlippedMaskedColumn(column_t *col, unsigned lengthcol)
{
	fixed_t topscreen = sprtopscreen;
	fixed_t bottomscreen = sprbotscreen;

	fixed_t texheight = dc_texheight*spryscale;

	fixed_t scroll = -repeatscroll;
	if (scroll < 0)
	{
		scroll = -FixedMul((abs(scroll) % (dc_texheight*FRACUNIT)), spryscale);
		bottomscreen += texheight; // Draw an extra time
	}
	else if (scroll)
	{
		scroll = FixedMul(scroll % (dc_texheight*FRACUNIT), spryscale);
		topscreen -= texheight; // Draw an extra time
	}

	while (topscreen < bottomscreen)
	{
		sprtopscreen = topscreen + scroll;
		sprbotscreen = bottomscreen + scroll;

		R_DrawFlippedMaskedColumn(col, lengthcol);

		if ((INT64)sprtopscreen + (INT64)dc_texheight*spryscale > (INT64)INT32_MAX) // prevent overflow
			break;

		topscreen += texheight;
	}

	sprtopscreen = sprbotscreen;
}

// Returns true if a fake floor is translucent.
static boolean R_IsFFloorTranslucent(visffloor_t *pfloor)
{
	if (pfloor->polyobj)
		return (pfloor->polyobj->translucency > 0);

	// Polyobjects have no ffloors, and they're handled in the conditional above.
	if (pfloor->ffloor != NULL)
		return (pfloor->ffloor->fofflags & (FOF_TRANSLUCENT|FOF_FOG));

	return false;
}

static fixed_t R_GetSlopeTextureSlide(pslope_t *slope, angle_t lineangle)
{
	return FixedMul(slope->zdelta, FINECOSINE((lineangle-slope->xydirection)>>ANGLETOFINESHIFT));
}

//
// R_RenderThickSideRange
// Renders all the thick sides in the given range.

static fixed_t ffloortexturecolumn[MAXVIDWIDTH];

void R_RenderThickSideRange(drawseg_t *ds, INT32 x1, INT32 x2, ffloor_t *pfloor)
{
	size_t          pindex;
	column_t *      col;
	INT32             lightnum;
	INT32            texnum;
	sector_t        tempsec;
	INT32             templight;
	INT32             i, p;
	lightlist_t     *light;
	r_lightlist_t   *rlight;
	INT32           range;
	// Render FOF sides kinda like normal sides, with the frac and step and everything
	// NOTE: INT64 instead of fixed_t because overflow concerns
	INT64         top_frac, top_step, bottom_frac, bottom_step;
	// skew FOF walls with slopes?
	fixed_t       ffloortextureslide = 0;
	fixed_t       oldtexturecolumn = -1;
	fixed_t       left_top, left_bottom; // needed here for slope skewing
	pslope_t      *skewslope = NULL;
	boolean do_texture_skew;
	boolean dont_peg_bottom;
	fixed_t wall_offsetx;
	fixed_t wall_scalex, wall_scaley;
	UINT8 vertflip;
	unsigned lengthcol;
	boolean fog = false;
	boolean fuzzy = false;

	void (*colfunc_2s) (column_t *, unsigned);

	// Calculate light table.
	// Use different light tables
	//   for horizontal / vertical / diagonal. Diagonal?
	// OPTIMIZE: get rid of LIGHTSEGSHIFT globally

	curline = ds->curline;
	backsector = pfloor->target;
	frontsector = curline->frontsector == pfloor->target ? curline->backsector : curline->frontsector;
	sidedef = R_GetFFloorSide(curline->linedef, pfloor, pfloor->target);

	if (pfloor->master->flags & ML_TFERLINE)
	{
		line_t *newline = R_GetFFloorLine(curline->linedef, pfloor, pfloor->target);
		do_texture_skew = newline->flags & ML_SKEWTD;
		dont_peg_bottom = newline->flags & ML_DONTPEGBOTTOM;
	}
	else
	{
		do_texture_skew = pfloor->master->flags & ML_SKEWTD;
		dont_peg_bottom = curline->linedef->flags & ML_DONTPEGBOTTOM;
	}

	texnum = R_GetTextureNum(sidedef->midtexture);
	vertflip = textures[texnum]->flip & 2;

	if (pfloor->fofflags & FOF_TRANSLUCENT)
	{
		fuzzy = true;

		// Hacked up support for alpha value in software mode Tails 09-24-2002
		// ...unhacked by toaster 04-01-2021, re-hacked a little by sphere 19-11-2021
		{
			INT32 trans = (10*((256+12) - pfloor->alpha))/255;
			if (trans >= 10)
				return; // Don't even draw it
			if (pfloor->blend) // additive, (reverse) subtractive, modulative
				dc_transmap = R_GetBlendTable(pfloor->blend, trans);
			else if (!(dc_transmap = R_GetTranslucencyTable(trans)) || trans == 0)
				fuzzy = false; // Opaque
		}
	}
	else if (pfloor->fofflags & FOF_FOG)
	{
		colfunc = colfuncs[COLDRAWFUNC_FOG];
		fog = true;
	}

	range = max(ds->x2-ds->x1, 1);

	dc_numlights = 0;
	if (frontsector->numlights)
	{
		dc_numlights = frontsector->numlights;
		if (dc_numlights > dc_maxlights)
		{
			dc_maxlights = dc_numlights;
			dc_lightlist = Z_Realloc(dc_lightlist, sizeof (*dc_lightlist) * dc_maxlights, PU_STATIC, NULL);
		}

		for (i = p = 0; i < dc_numlights; i++)
		{
			fixed_t leftheight, rightheight;
			fixed_t pfloorleft, pfloorright;
			INT64 overflow_test;
			light = &frontsector->lightlist[i];
			rlight = &dc_lightlist[p];

#define SLOPEPARAMS(slope, end1, end2, normalheight) \
	end1 = P_GetZAt(slope, ds-> leftpos.x, ds-> leftpos.y, normalheight); \
	end2 = P_GetZAt(slope, ds->rightpos.x, ds->rightpos.y, normalheight);

			SLOPEPARAMS(light->slope,     leftheight, rightheight, light->height)
			SLOPEPARAMS(*pfloor->b_slope, pfloorleft, pfloorright, *pfloor->bottomheight)

			if (leftheight < pfloorleft && rightheight < pfloorright)
				continue;

			SLOPEPARAMS(*pfloor->t_slope, pfloorleft, pfloorright, *pfloor->topheight)

			if (leftheight > pfloorleft && rightheight > pfloorright && i+1 < dc_numlights)
			{
				lightlist_t *nextlight = &frontsector->lightlist[i+1];
				if (P_GetZAt(nextlight->slope, ds-> leftpos.x, ds-> leftpos.y, nextlight->height) > pfloorleft
				 && P_GetZAt(nextlight->slope, ds->rightpos.x, ds->rightpos.y, nextlight->height) > pfloorright)
					continue;
			}

			leftheight -= viewz;
			rightheight -= viewz;

#define CLAMPMAX INT32_MAX
#define CLAMPMIN (-INT32_MAX) // This is not INT32_MIN on purpose! INT32_MIN makes the drawers freak out.
			// Monster Iestyn (25/03/18): do not skip these lights if they fail overflow test, just clamp them instead so they behave.
			overflow_test = (INT64)centeryfrac - (((INT64)leftheight*ds->scale1)>>FRACBITS);
			if      (overflow_test > (INT64)CLAMPMAX) rlight->height = CLAMPMAX;
			else if (overflow_test > (INT64)CLAMPMIN) rlight->height = (fixed_t)overflow_test;
			else                                      rlight->height = CLAMPMIN;

			overflow_test = (INT64)centeryfrac - (((INT64)rightheight*ds->scale2)>>FRACBITS);
			if      (overflow_test > (INT64)CLAMPMAX) rlight->heightstep = CLAMPMAX;
			else if (overflow_test > (INT64)CLAMPMIN) rlight->heightstep = (fixed_t)overflow_test;
			else                                      rlight->heightstep = CLAMPMIN;
			rlight->heightstep = (rlight->heightstep-rlight->height)/(range);
			rlight->flags = light->flags;
			if (light->flags & FOF_CUTLEVEL)
			{
				SLOPEPARAMS(*light->caster->b_slope, leftheight, rightheight, *light->caster->bottomheight)
#undef SLOPEPARAMS
				leftheight -= viewz;
				rightheight -= viewz;

				// Monster Iestyn (25/03/18): do not skip these lights if they fail overflow test, just clamp them instead so they behave.
				overflow_test = (INT64)centeryfrac - (((INT64)leftheight*ds->scale1)>>FRACBITS);
				if      (overflow_test > (INT64)CLAMPMAX) rlight->botheight = CLAMPMAX;
				else if (overflow_test > (INT64)CLAMPMIN) rlight->botheight = (fixed_t)overflow_test;
				else                                      rlight->botheight = CLAMPMIN;

				overflow_test = (INT64)centeryfrac - (((INT64)rightheight*ds->scale2)>>FRACBITS);
				if      (overflow_test > (INT64)CLAMPMAX) rlight->botheightstep = CLAMPMAX;
				else if (overflow_test > (INT64)CLAMPMIN) rlight->botheightstep = (fixed_t)overflow_test;
				else                                      rlight->botheightstep = CLAMPMIN;
				rlight->botheightstep = (rlight->botheightstep-rlight->botheight)/(range);
			}

			rlight->lightlevel = *light->lightlevel;
			rlight->extra_colormap = *light->extra_colormap;

			// Check if the current light effects the colormap/lightlevel
			if (pfloor->fofflags & FOF_FOG)
				rlight->lightnum = R_SideLightLevel(curline->sidedef, pfloor->master->frontsector->lightlevel) >> LIGHTSEGSHIFT;
			else
				rlight->lightnum = R_SideLightLevel(curline->sidedef, rlight->lightlevel) >> LIGHTSEGSHIFT;

			if (pfloor->fofflags & FOF_FOG || rlight->flags & FOF_FOG || (rlight->extra_colormap && (rlight->extra_colormap->flags & CMF_FOG)))
				;
			else if (curline->v1->y == curline->v2->y)
				rlight->lightnum--;
			else if (curline->v1->x == curline->v2->x)
				rlight->lightnum++;

			p++;
		}

		dc_numlights = p;
	}
	else
	{
		// Get correct light level!
		if ((frontsector->extra_colormap && (frontsector->extra_colormap->flags & CMF_FOG)))
			lightnum = R_SideLightLevel(curline->sidedef, frontsector->lightlevel) >> LIGHTSEGSHIFT;
		else if (fog)
			lightnum = R_SideLightLevel(curline->sidedef, pfloor->master->frontsector->lightlevel) >> LIGHTSEGSHIFT;
		else if (fuzzy)
			lightnum = LIGHTLEVELS-1;
		else
			lightnum = R_SideLightLevel(curline->sidedef, R_FakeFlat(frontsector, &tempsec, &templight, &templight, false)->lightlevel) >> LIGHTSEGSHIFT;

		if (pfloor->fofflags & FOF_FOG || (frontsector->extra_colormap && (frontsector->extra_colormap->flags & CMF_FOG)));
			else if (curline->v1->y == curline->v2->y)
			lightnum--;
		else if (curline->v1->x == curline->v2->x)
			lightnum++;

		if (lightnum < 0)
			walllights = scalelight[0];
		else if (lightnum >= LIGHTLEVELS)
			walllights = scalelight[LIGHTLEVELS-1];
		else
			walllights = scalelight[lightnum];
	}

	wall_scalex = FixedDiv(FRACUNIT, SIDE_SCALEX_MID(sidedef));
	wall_scaley = SIDE_SCALEY_MID(sidedef);
	if (wall_scaley < 0)
	{
		wall_scaley = -wall_scaley;
		vertflip = !vertflip;
	}

	//SoM: Moved these up here so they are available for my lightlist calculations
	// Lactozilla: Moved them back down
	// This uses floating point math now, because the fixed-point imprecisions
	// become more severe the bigger the texture is scaled.
#ifdef PS2_OPT_SEGS
	// PS2-16: with an unscaled wall (every binary map) the double values are sums of 16.16 numbers and exactly
	// representable, so the same result comes out of integer arithmetic. Scaled walls keep the double code.
	const boolean ysexact = (wall_scaley == FRACUNIT);
	INT64 ysfix = (INT64)ds->scale1 + (INT64)(x1 - ds->x1) * ds->scalestep;
	double dwall_scaley = ysexact ? 1.0 : FixedToDouble(wall_scaley);
	double scalestep = ysexact ? 0.0 : FixedToDouble(ds->scalestep) / dwall_scaley;
	double yscale = ysexact ? 0.0 : (FixedToDouble(ds->scale1) + (x1 - ds->x1)*scalestep) / dwall_scaley;
#define YSCALE_NEXT() do { if (ysexact) ysfix += ds->scalestep; else yscale += scalestep; } while (0)
#define YSCALE_FIXED() (ysexact ? (fixed_t)ysfix : DoubleToFixed(yscale))
#else
	double dwall_scaley = FixedToDouble(wall_scaley);
	double scalestep = FixedToDouble(ds->scalestep) / dwall_scaley;
	double yscale = (FixedToDouble(ds->scale1) + (x1 - ds->x1)*scalestep) / dwall_scaley;
#define YSCALE_NEXT() (yscale += scalestep)
#define YSCALE_FIXED() DoubleToFixed(yscale)
#endif

	thicksidecol = ffloortexturecolumn;

	wall_offsetx = ds->offsetx + SIDE_OFFSETX_MID(sidedef);

	if (wall_scalex == FRACUNIT)
	{
		for (INT32 x = x1; x <= x2; x++)
			thicksidecol[x] = ds->thicksidecol[x];
	}
	else
	{
		for (INT32 x = x1; x <= x2; x++)
			thicksidecol[x] = FixedDiv(ds->thicksidecol[x], wall_scalex);
	}

	mfloorclip = ds->sprbottomclip;
	mceilingclip = ds->sprtopclip;
	dc_texheight = textureheight[texnum]>>FRACBITS;

	// calculate both left ends
	left_top    = P_GetFFloorTopZAt   (pfloor, ds->leftpos.x, ds->leftpos.y) - viewz;
	left_bottom = P_GetFFloorBottomZAt(pfloor, ds->leftpos.x, ds->leftpos.y) - viewz;

	if (do_texture_skew)
	{
		skewslope = *pfloor->t_slope; // skew using top slope by default
		dc_texturemid = FixedMul(left_top, wall_scaley);
	}
	else
		dc_texturemid = FixedMul(*pfloor->topheight - viewz, wall_scaley);

	repeatscroll = sidedef->rowoffset + SIDE_OFFSETY_MID(sidedef);

	if (dont_peg_bottom)
	{
		if (do_texture_skew)
		{
			skewslope = *pfloor->b_slope; // skew using bottom slope
			dc_texturemid = FixedMul(left_bottom, wall_scaley);
		}
		else
			repeatscroll -= FixedMul(*pfloor->topheight - *pfloor->bottomheight, wall_scaley);
	}

	if (skewslope)
	{
		angle_t lineangle = R_PointToAngle2(curline->v1->x, curline->v1->y, curline->v2->x, curline->v2->y);
		ffloortextureslide = FixedMul(R_GetSlopeTextureSlide(skewslope, lineangle), wall_scaley);
	}

	dc_texturemid += repeatscroll;

	// Texture must be cached
	R_CheckTextureCache(texnum);

	// handle case where multipatch texture is drawn on a 2sided wall, multi-patch textures
	// are not stored per-column with post info in SRB2
	if (!textures[texnum]->transparency)
	{
		if (vertflip) // vertically flipped?
			colfunc_2s = R_RenderFlipped2sidedMultiPatchColumn;
		else
			colfunc_2s = R_Render2sidedMultiPatchColumn;
	}
	else if (vertflip) // vertically flipped?
		colfunc_2s = R_DrawRepeatFlippedMaskedColumn;
	else
		colfunc_2s = R_DrawRepeatMaskedColumn;

	lengthcol = textures[texnum]->height;

	// Set heights according to plane, or slope, whichever
	{
		fixed_t right_top, right_bottom;

		// calculate right ends now
		right_top    = P_GetFFloorTopZAt   (pfloor, ds->rightpos.x, ds->rightpos.y) - viewz;
		right_bottom = P_GetFFloorBottomZAt(pfloor, ds->rightpos.x, ds->rightpos.y) - viewz;

		// using INT64 to avoid 32bit overflow
		top_frac =    (INT64)centeryfrac - (((INT64)left_top     * ds->scale1) >> FRACBITS);
		bottom_frac = (INT64)centeryfrac - (((INT64)left_bottom  * ds->scale1) >> FRACBITS);
		top_step =    (INT64)centeryfrac - (((INT64)right_top    * ds->scale2) >> FRACBITS);
		bottom_step = (INT64)centeryfrac - (((INT64)right_bottom * ds->scale2) >> FRACBITS);

		top_step = (top_step-top_frac)/(range);
		bottom_step = (bottom_step-bottom_frac)/(range);

		top_frac += top_step * (x1 - ds->x1);
		bottom_frac += bottom_step * (x1 - ds->x1);
	}

	// draw the columns
	for (dc_x = x1; dc_x <= x2; dc_x++)
	{
		// skew FOF walls
		if (ffloortextureslide)
		{
			if (oldtexturecolumn != -1)
				dc_texturemid += FixedMul(ffloortextureslide, oldtexturecolumn-ds->thicksidecol[dc_x]);
			oldtexturecolumn = ds->thicksidecol[dc_x];
		}

		// Calculate bounds
		// clamp the values if necessary to avoid overflows and rendering glitches caused by them
		if      (top_frac > (INT64)CLAMPMAX) sprtopscreen = windowtop = CLAMPMAX;
		else if (top_frac > (INT64)CLAMPMIN) sprtopscreen = windowtop = (fixed_t)top_frac;
		else                                 sprtopscreen = windowtop = CLAMPMIN;
		if      (bottom_frac > (INT64)CLAMPMAX) sprbotscreen = windowbottom = CLAMPMAX;
		else if (bottom_frac > (INT64)CLAMPMIN) sprbotscreen = windowbottom = (fixed_t)bottom_frac;
		else                                    sprbotscreen = windowbottom = CLAMPMIN;

		fixed_t bottomclip = sprbotscreen;

		top_frac += top_step;
		bottom_frac += bottom_step;

		// SoM: If column is out of range, why bother with it??
		if (windowbottom < 0 || windowtop > (viewheight << FRACBITS))
		{
			if (dc_numlights)
			{
				for (i = 0; i < dc_numlights; i++)
				{
					rlight = &dc_lightlist[i];
					rlight->height += rlight->heightstep;
					if (rlight->flags & FOF_CUTLEVEL)
						rlight->botheight += rlight->botheightstep;
				}
			}
			YSCALE_NEXT();
			continue;
		}

		// Get data for the column
		col = R_GetColumn(texnum, ((thicksidecol[dc_x] + wall_offsetx) >> FRACBITS));

		spryscale = YSCALE_FIXED();

		// Eh. I tried fixing the scaling artifacts but it still wasn't perfect.
		// So this checks if the column has gaps in it or not, and if it does, uses a version of the column drawers
		// that prevents the artifacts from being visible.
		// Note that if rendering fog then none of this matters because there's no texture mapping to be done
		if (!fog)
		{
			dc_iscale = 0xffffffffu / (unsigned)spryscale;

			// Column has a single post and it matches the texture height, use regular column drawers
			if (col->num_posts == 1 && col->posts[0].topdelta == 0 && col->posts[0].length == (unsigned)dc_texheight)
			{
				if (fuzzy)
					colfunc = colfuncs[COLDRAWFUNC_FUZZY];
				else
					colfunc = colfuncs[BASEDRAWFUNC];
			}
			else
			{
				// Otherwise use column drawers with extra checks
				if (fuzzy)
					colfunc = colfuncs[COLDRAWFUNC_CLAMPEDTRANS];
				else
					colfunc = colfuncs[COLDRAWFUNC_CLAMPED];
			}
		}

		// SoM: New code does not rely on R_DrawColumnShadowed_8 which
		// will (hopefully) put less strain on the stack.
		if (dc_numlights)
		{
			lighttable_t **xwalllights;
			fixed_t height;
			fixed_t bheight = 0;
			boolean lighteffect = false;

			for (i = 0; i < dc_numlights; i++)
			{
				// Check if the current light effects the colormap/lightlevel
				rlight = &dc_lightlist[i];
				lighteffect = !(rlight->flags & FOF_NOSHADE);
				if (lighteffect)
				{
					lightnum = rlight->lightnum;

					if (lightnum < 0)
						xwalllights = scalelight[0];
					else if (lightnum >= LIGHTLEVELS)
						xwalllights = scalelight[LIGHTLEVELS-1];
					else
						xwalllights = scalelight[lightnum];

					pindex = FixedMul(spryscale, LIGHTRESOLUTIONFIX)>>LIGHTSCALESHIFT;

					if (pindex >= MAXLIGHTSCALE)
						pindex = MAXLIGHTSCALE-1;

					if (pfloor->fofflags & FOF_FOG)
					{
						if (pfloor->master->frontsector->extra_colormap)
							rlight->rcolormap = pfloor->master->frontsector->extra_colormap->colormap + (xwalllights[pindex] - colormaps);
						else
							rlight->rcolormap = xwalllights[pindex];
					}
					else
					{
						if (rlight->extra_colormap)
							rlight->rcolormap = rlight->extra_colormap->colormap + (xwalllights[pindex] - colormaps);
						else
							rlight->rcolormap = xwalllights[pindex];
					}
				}

				// Check if the current light can cut the current 3D floor.
				boolean solid = false;

				if (rlight->flags & FOF_CUTSOLIDS && !(pfloor->fofflags & FOF_EXTRA))
					solid = true;
				else if (rlight->flags & FOF_CUTEXTRA && pfloor->fofflags & FOF_EXTRA)
				{
					if (rlight->flags & FOF_EXTRA)
					{
						// The light is from an extra 3D floor... Check the flags so
						// there are no undesired cuts.
						if ((rlight->flags & (FOF_FOG|FOF_SWIMMABLE)) == (pfloor->fofflags & (FOF_FOG|FOF_SWIMMABLE)))
							solid = true;
					}
					else
						solid = true;
				}
				else
					solid = false;

				height = rlight->height;
				rlight->height += rlight->heightstep;

				if (solid)
				{
					bheight = rlight->botheight - (FRACUNIT >> 1);
					rlight->botheight += rlight->botheightstep;
				}

				if (height <= windowtop)
				{
					if (lighteffect)
						dc_colormap = rlight->rcolormap;
					if (solid && windowtop < bheight)
						sprtopscreen = windowtop = bheight;
					continue;
				}

				sprbotscreen = windowbottom = height;
				if (windowbottom >= bottomclip)
				{
					sprbotscreen = windowbottom = bottomclip;
					// draw the texture
					colfunc_2s (col, lengthcol);
					for (i++; i < dc_numlights; i++)
					{
						rlight = &dc_lightlist[i];
						rlight->height += rlight->heightstep;
						if (rlight->flags & FOF_CUTLEVEL)
							rlight->botheight += rlight->botheightstep;
					}
					continue;
				}
				// draw the texture
				colfunc_2s (col, lengthcol);
				if (solid)
					windowtop = bheight;
				else
					windowtop = windowbottom + 1;
				sprtopscreen = windowtop;
				if (lighteffect)
					dc_colormap = rlight->rcolormap;
			}
			sprbotscreen = windowbottom = bottomclip;
			// draw the texture, if there is any space left
			if (windowtop < windowbottom)
				colfunc_2s (col, lengthcol);

			YSCALE_NEXT();
			continue;
		}

		// calculate lighting
		pindex = FixedMul(spryscale, LIGHTRESOLUTIONFIX)>>LIGHTSCALESHIFT;

		if (pindex >= MAXLIGHTSCALE)
			pindex = MAXLIGHTSCALE - 1;

		dc_colormap = walllights[pindex];

		if (pfloor->fofflags & FOF_FOG && pfloor->master->frontsector->extra_colormap)
			dc_colormap = pfloor->master->frontsector->extra_colormap->colormap + (dc_colormap - colormaps);
		else if (frontsector->extra_colormap)
			dc_colormap = frontsector->extra_colormap->colormap + (dc_colormap - colormaps);

		// draw the texture
		colfunc_2s (col, lengthcol);
		YSCALE_NEXT();
	}
	colfunc = colfuncs[BASEDRAWFUNC];

#undef CLAMPMAX
#undef CLAMPMIN
}

// R_ExpandPlaneY
//
// A simple function to modify a visplane's top and bottom for a particular column
// Sort of like R_ExpandPlane in r_plane.c, except this is vertical expansion
static inline void R_ExpandPlaneY(visplane_t *pl, INT32 x, INT16 top, INT16 bottom)
{
	// Expand the plane, don't shrink it!
	// note: top and bottom default to 0xFFFF and 0x0000 respectively, which is totally compatible with this
	if (pl->top[x] > top)       pl->top[x] = top;
	if (pl->bottom[x] < bottom) pl->bottom[x] = bottom;
}

// R_FFloorCanClip
//
// Returns true if a fake floor can clip a column away.
static boolean R_FFloorCanClip(visffloor_t *pfloor)
{
	return (cv_ffloorclip.value && !R_IsFFloorTranslucent(pfloor) && !pfloor->polyobj);
}

//
// R_RenderSegLoop
// Draws zero, one, or two textures (and possibly a masked
//  texture) for walls.
// Can draw or mark the starting pixel of floor and ceiling
//  textures.
// CALLED: CORE LOOPING ROUTINE.
//
#define HEIGHTBITS              12
#define HEIGHTUNIT              (1<<HEIGHTBITS)

#ifndef PS2_OPT_REND // the PS2 loop dispatches the three cases itself
static void R_DrawRegularWall(UINT8 *source, INT32 height)
{
	dc_source = source;
	dc_texheight = height;
	colfunc();
}

static void R_DrawFlippedWall(UINT8 *source, INT32 height)
{
	dc_texheight = height;
	R_DrawFlippedPost(source, (unsigned)height, colfunc);
}

static void R_DrawNoWall(UINT8 *source, INT32 height)
{
	(void)source;
	(void)height;
}
#endif

#ifdef PS2_OPT_REND
// column of a texture (R_GetColumn without the cache bookkeeping): wrap by mask for power-of-two widths
#if defined(PS2_NEGCTL) && PS2_NEGCTL == 4
#define SEG_COLUMN(cols, w, c) (&(cols)[((c) + 1) & ((w) - 1)])
#else
#define SEG_COLUMN(cols, w, c) (((w) & ((w) - 1)) ? &(cols)[(UINT32)(c) % (w)] : &(cols)[(c) & ((w) - 1)])
#endif
#ifdef PS2_SUBPROF
#define PS2SUB_RESET(bv) (ps2sb_##bv = PS2Sub_Count())
#define PS2SUB_CATADD(base, bv) do { ps2sub_cyc[(base) + ps2_segcat] += (unsigned)(PS2Sub_Count() - ps2sb_##bv); ps2sub_calls[(base) + ps2_segcat]++; } while (0)
#define PS2SUB_CAT() do { ps2sub_cyc[64 + ps2_segcat] += (unsigned)(PS2Sub_Count() - ps2sb_60); ps2sub_calls[64 + ps2_segcat]++; } while (0)
extern int ps2_segcat;
#else
#define PS2SUB_CAT() ((void)0)
#define PS2SUB_CATADD(base, bv) ((void)0)
#define PS2SUB_RESET(bv) ((void)0)
#endif
#define SEGT_ANY 8 // tiers value: which wall tiers exist is decided at run time
// PS2-80: R_RenderSegLoop is instantiated twice from one body. plain == true is chosen when the range has no 3D floors,
// no back 3D floors and no light list: the dead blocks vanish, the register pressure drops (the original keeps ~40 live
// values and spills most of them). Every skipped computation is free of side effects (see the per-item notes); all
// stores happen in the original order.
#if defined(__GNUC__)
#define R_FORCEINLINE static inline __attribute__((always_inline))
#else
#define R_FORCEINLINE static ATTRINLINE
#endif
R_FORCEINLINE void R_RenderSegLoopT(const boolean plain, const INT32 tiers)
{
	PS2SUB_B(79);
	angle_t angle;
	fixed_t textureoffset = 0;
	size_t pindex;
	INT32 yl;
	INT32 yh;

	INT32 mid;
	fixed_t texturecolumn = 0;
	fixed_t toptexturecolumn = 0;
	fixed_t bottomtexturecolumn = 0;
	fixed_t oldtexturecolumn = -1;
	fixed_t oldtexturecolumn_top = -1;
	fixed_t oldtexturecolumn_bottom = -1;
	INT32 top;
	INT32 bottom;
	INT32 i;

	fixed_t topscaley = rw_toptexturescaley;
	fixed_t midscaley = rw_midtexturescaley;
	fixed_t bottomscaley = rw_bottomtexturescaley;

	// 0 = R_DrawRegularWall, 1 = R_DrawFlippedWall, 2 = R_DrawNoWall
	INT32 drawtop = 0, drawmiddle = 0, drawbottom = 0;

	// loop invariants (nothing in the loop writes them; the column drawers do not touch them)
	INT16 *const cclip = ceilingclip;
	INT16 *const fclip = floorclip;
	fixed_t *const fscale = frontscale;
	const INT32 vheight = viewheight;
	const fixed_t lightresfix = LIGHTRESOLUTIONFIX; // 640*fovtan/vid.width: one integer division per use otherwise
	UINT16 *const ctop = ceilingplane ? ceilingplane->top : NULL;
	UINT16 *const cbot = ceilingplane ? ceilingplane->bottom : NULL;
	UINT16 *const ftop = floorplane ? floorplane->top : NULL;
	UINT16 *const fbot = floorplane ? floorplane->bottom : NULL;
	const boolean domarkceil = markceiling && ceilingplane;
	const boolean domarkfloor = markfloor && floorplane;
	const boolean texd = segtextured;
	// PS2-90: tiers is a compile-time constant in every instantiation but SEGT_ANY: the code of absent wall tiers vanishes
	const boolean hasmid = tiers == SEGT_ANY ? midtexture != 0 : (tiers & 1) != 0;
	const boolean hastop = tiers == SEGT_ANY ? toptexture != 0 : (tiers & 2) != 0;
	const boolean hasbot = tiers == SEGT_ANY ? bottomtexture != 0 : (tiers & 4) != 0;
	const boolean hasextra = frontsector->extra_colormap != NULL;
	const fixed_t midtexheight = hasmid ? textureheight[midtexture]>>FRACBITS : 0;
	const fixed_t toptexheight = hastop ? textureheight[toptexture]>>FRACBITS : 0;
	const fixed_t bottexheight = hasbot ? textureheight[bottomtexture]>>FRACBITS : 0;
	const boolean masktc = maskedtexturecol != NULL;
	const boolean masktheight = maskedtextureheight != NULL;
	const boolean thickcol = thicksidecol != NULL;
	const boolean midwork = hasmid || masktheight;
	UINT32 invs = 0; // 0xffffffffu / (unsigned)rw_scale, divided at most once per column
	boolean haveinvs;
	lighttable_t **xwl[32]; // per light: the scalelight row of the range (it depends on the side and the light only)
	boolean lightpre = false;
	boolean lightsdone = true;

#if defined(PS2_NEGCTL) && PS2_NEGCTL == 2 // negative control: the light colormaps of the shadowed columns are never set
#define SEG_LIGHTS() ((void)0)
#else
#define SEG_LIGHTS() do { \
		if (!lightsdone) \
		{ \
			lightsdone = true; \
			pindex = FixedMul(cscale, lightresfix)>>LIGHTSCALESHIFT; \
			if (pindex >=  MAXLIGHTSCALE) \
				pindex = MAXLIGHTSCALE-1; \
			for (i = 0; i < dc_numlights; i++) \
			{ \
				if (dc_lightlist[i].extra_colormap) \
					dc_lightlist[i].rcolormap = dc_lightlist[i].extra_colormap->colormap + (xwl[i][pindex] - colormaps); \
				else \
					dc_lightlist[i].rcolormap = xwl[i][pindex]; \
			} \
		} \
	} while (0)
#endif
	PS2SUB_ADD(56, rw_stopx - rw_x);
	if (segtextured)
		PS2SUB_ADD(57, rw_stopx - rw_x);
	if (!plain)
		PS2SUB_ADD(59, rw_stopx - rw_x);
	if (!plain && dc_numlights)
		PS2SUB_ADD(53, rw_stopx - rw_x);
	if (!plain && numffloors)
		PS2SUB_ADD(52, rw_stopx - rw_x);

	if (!plain && dc_numlights)
		colfunc = colfuncs[COLDRAWFUNC_SHADOWED];

	if (hastop && topscaley < 0)
	{
		topscaley = -topscaley;
		drawtop = 1;
	}
	if (hasmid && midscaley < 0)
	{
		midscaley = -midscaley;
		drawmiddle = 1;
	}
	if (hasbot && bottomscaley < 0)
	{
		bottomscaley = -bottomscaley;
		drawbottom = 1;
	}

	if (!r_renderwalls)
	{
		drawtop = 2;
		drawmiddle = 2;
		drawbottom = 2;
	}

	if (hasmid)
		R_CheckTextureCache(midtexture);
	if (hastop)
		R_CheckTextureCache(toptexture);
	if (hasbot)
		R_CheckTextureCache(bottomtexture);
	// PS2-90: R_GetColumn per column fetched the column table and width again and touched the cache block each time; the
	// blocks were stamped for this frame by the checks above (and stay: the 3D view holds the purge lock), so the loop
	// indexes the tables directly. A tier whose block was evicted by the next tier's generation is checked again first.
	for (i = 0; i < 3 && ((hasmid && !texturecache[midtexture]) || (hastop && !texturecache[toptexture]) || (hasbot && !texturecache[bottomtexture])); i++)
	{
		if (hasmid)
			R_CheckTextureCache(midtexture);
		if (hastop)
			R_CheckTextureCache(toptexture);
		if (hasbot)
			R_CheckTextureCache(bottomtexture);
	}
	column_t *const midcols = hasmid ? texturecolumns[midtexture] : NULL;
	column_t *const topcols = hastop ? texturecolumns[toptexture] : NULL;
	column_t *const botcols = hasbot ? texturecolumns[bottomtexture] : NULL;
	const INT32 midw = hasmid ? texturewidth[midtexture] : 1;
	const INT32 topw = hastop ? texturewidth[toptexture] : 1;
	const INT32 botw = hasbot ? texturewidth[bottomtexture] : 1;

	if (!plain && dc_numlights)
		colfunc = colfuncs[COLDRAWFUNC_SHADOWED];

	if (plain && rw_x < rw_stopx)
		rw_floormarked = rw_ceilingmarked = false; // only the 3D floor block ever sets them

	INT32 cx = rw_x;
	fixed_t cscale = rw_scale, ctopfrac = topfrac, cbotfrac = bottomfrac, cpixhigh = pixhigh, cpixlow = pixlow;
	const fixed_t sstep_ = rw_scalestep, tstep_ = topstep, bstep_ = bottomstep, phstep_ = pixhighstep, plstep_ = pixlowstep;
	const INT32 stopx_ = rw_stopx;
	// PS2-90: file-scope values the column loop only reads live in locals (every access is lui+lw, and the stores of the
	// drawers/clip arrays keep the compiler from reusing them); the four texture origins that the loop itself steps are
	// written back after it
	const angle_t centerangle_ = rw_centerangle;
	const fixed_t offset_ = rw_offset, distance_ = rw_distance, offsetx_ = rw_offsetx;
	const fixed_t invmidx_ = rw_invmidtexturescalex, invtopx_ = rw_invtoptexturescalex, invbotx_ = rw_invbottomtexturescalex;
	const fixed_t offtop_ = rw_offset_top, offbot_ = rw_offset_bottom;
	const boolean toppeg_ = rw_toptexturescalex < 0, botpeg_ = rw_bottomtexturescalex < 0, midneg_ = rw_invmidtexturescalex < 0;
	const fixed_t midslide_ = rw_midtextureslide, midbackslide_ = rw_midtexturebackslide;
	const fixed_t topslide_ = rw_toptextureslide, botslide_ = rw_bottomtextureslide;
	lighttable_t **const wlights = walllights;
	const lighttable_t *const cmaps = colormaps;
	lighttable_t *const extracm = hasextra ? frontsector->extra_colormap->colormap : NULL;
	fixed_t midtexturemid_ = rw_midtexturemid, midtextureback_ = rw_midtextureback;
	fixed_t toptexturemid_ = rw_toptexturemid, bottomtexturemid_ = rw_bottomtexturemid;
	if (plain && !texd)
	{
		// PS2-80: a range without any texture (about 70% of the columns of a typical frame: openings between sectors) only
		// marks the floor/ceiling planes, updates the clip arrays and stores the scale. Same stores, in the same order, but
		// the stepped values live in registers instead of the file-scope variables.
		INT32 x = rw_x;
		const INT32 stop = rw_stopx;
		fixed_t scale = rw_scale, tfrac = topfrac, bfrac = bottomfrac;
		const fixed_t sstep = rw_scalestep, tstep = topstep, bstep = bottomstep;

		for (; x < stop; x++, scale += sstep, tfrac += tstep, bfrac += bstep)
		{
			const INT32 cc = cclip[x], fc = fclip[x];

			yl = (tfrac+HEIGHTUNIT-1)>>HEIGHTBITS;
			top = cc+1;
			if (yl < top)
				yl = top;

			if (markceiling)
			{
				bottom = yl > fc ? fc : yl;
				if (top <= --bottom && domarkceil)
				{
					const INT16 t16 = (INT16)top, b16 = (INT16)bottom;
					if (ctop[x] > t16) ctop[x] = t16;
					if (cbot[x] < b16) cbot[x] = b16;
				}
			}

			yh = bfrac>>HEIGHTBITS;
			bottom = fc-1;
			if (yh > bottom)
				yh = bottom;

			if (markfloor)
			{
				top = yh < cc ? cc : yh;
				if (++top <= bottom && domarkfloor)
				{
					const INT16 t16 = (INT16)top, b16 = (INT16)bottom;
					if (ftop[x] > t16) ftop[x] = t16;
					if (fbot[x] < b16) fbot[x] = b16;
				}
			}

			fscale[x] = scale;

			// no textures: only the clip arrays change (the originals' topclip/bottomclip)
			if (markceiling)
				cclip[x] = (yl >= 0) ? ((yl > vheight) ? (INT16)vheight : (INT16)((INT16)yl - 1)) : -1;
			if (markfloor)
				fclip[x] = (yh < vheight) ? ((yh < -1) ? -1 : (INT16)((INT16)yh + 1)) : (INT16)vheight;
		}

		rw_x = x;
		rw_scale = scale;
		topfrac = tfrac;
		bottomfrac = bfrac;
		return;
	}

	if (!plain && dc_numlights <= 32)
	{
		for (i = 0; i < dc_numlights; i++)
		{
			INT32 lightnum = R_SideLightLevel(curline->sidedef, dc_lightlist[i].lightlevel) >> LIGHTSEGSHIFT;

			if (dc_lightlist[i].extra_colormap)
				;
			else if (curline->v1->y == curline->v2->y)
				lightnum--;
			else if (curline->v1->x == curline->v2->x)
				lightnum++;

			if (lightnum < 0)
				xwl[i] = scalelight[0];
			else if (lightnum >= LIGHTLEVELS)
				xwl[i] = scalelight[LIGHTLEVELS-1];
			else
				xwl[i] = scalelight[lightnum];
		}
		lightpre = true;
	}

	PS2SUB_CATADD(80, 79);
	PS2SUB_RESET(79);
	for (; cx < stopx_; cx++)
	{
		// mark floor / ceiling areas
		yl = (ctopfrac+HEIGHTUNIT-1)>>HEIGHTBITS;

		top = cclip[cx]+1;

		// no space above wall?
		if (yl < top)
			yl = top;

		if (markceiling)
		{
			bottom = yl > fclip[cx] ? fclip[cx] : yl;

			if (top <= --bottom && domarkceil)
			{
				const INT16 t16 = (INT16)top, b16 = (INT16)bottom;
				if (ctop[cx] > t16) ctop[cx] = t16;
				if (cbot[cx] < b16) cbot[cx] = b16;
			}
		}

		yh = cbotfrac>>HEIGHTBITS;

		bottom = fclip[cx]-1;

		if (yh > bottom)
			yh = bottom;

		if (markfloor)
		{
			top = yh < cclip[cx] ? cclip[cx] : yh;

			if (++top <= bottom && domarkfloor)
			{
				const INT16 t16 = (INT16)top, b16 = (INT16)bottom;
				if (ftop[cx] > t16) ftop[cx] = t16;
				if (fbot[cx] < b16) fbot[cx] = b16;
			}
		}

		haveinvs = false;
		lightsdone = true;

		if (!plain)
		{
			rw_floormarked = false;
			rw_ceilingmarked = false;

			if (numffloors)
			{
				INT16 fftop, ffbottom;

				firstseg->frontscale[cx] = fscale[cx];
				top = cclip[cx]+1; // PRBoom
				bottom = fclip[cx]-1; // PRBoom

				for (i = 0; i < numffloors; i++)
				{
					if (ffloor[i].polyobj && (!curline->polyseg || ffloor[i].polyobj != curline->polyseg))
						continue;

					if (ffloor[i].height < viewz)
					{
						INT32 top_w = (ffloor[i].f_frac >> HEIGHTBITS) + 1;
						INT32 bottom_w = ffloor[i].f_clip[cx];

						if (top_w < top)
							top_w = top;

						if (bottom_w > bottom)
							bottom_w = bottom;

						// Polyobject-specific hack to fix plane leaking -Red
						if (ffloor[i].polyobj && top_w >= bottom_w)
						{
							ffloor[i].plane->top[cx] = 0xFFFF;
							ffloor[i].plane->bottom[cx] = 0x0000; // fix for sky plane drawing crashes - Monster Iestyn 25/05/18
						}
						else
						{
							if (top_w <= bottom_w)
							{
								fftop = (INT16)top_w;
								ffbottom = (INT16)bottom_w;

								ffloor[i].plane->top[cx] = fftop;
								ffloor[i].plane->bottom[cx] = ffbottom;

								// Lactozilla: Cull part of the column by the 3D floor if it can't be seen
								// "bottom" is the top pixel of the floor column
								if (ffbottom >= bottom-1 && R_FFloorCanClip(&ffloor[i]) && !curline->polyseg)
								{
									rw_floormarked = true;
									fclip[cx] = fftop;
									if (yh > fftop)
										yh = fftop;

									if (markfloor && floorplane)
										floorplane->top[cx] = bottom;

									if (rw_silhouette)
									{
										(*rw_silhouette) |= SIL_BOTTOM;
										(*rw_bsilheight) = INT32_MAX;
									}
								}
							}
						}
					}
					else if (ffloor[i].height > viewz)
					{
						INT32 top_w = ffloor[i].c_clip[cx] + 1;
						INT32 bottom_w = (ffloor[i].f_frac >> HEIGHTBITS);

						if (top_w < top)
							top_w = top;

						if (bottom_w > bottom)
							bottom_w = bottom;

						// Polyobject-specific hack to fix plane leaking -Red
						if (ffloor[i].polyobj && top_w >= bottom_w)
						{
							ffloor[i].plane->top[cx] = 0xFFFF;
							ffloor[i].plane->bottom[cx] = 0x0000; // fix for sky plane drawing crashes - Monster Iestyn 25/05/18
						}
						else
						{
							if (top_w <= bottom_w)
							{
								fftop = (INT16)top_w;
								ffbottom = (INT16)bottom_w;

								ffloor[i].plane->top[cx] = fftop;
								ffloor[i].plane->bottom[cx] = ffbottom;

								// Lactozilla: Cull part of the column by the 3D floor if it can't be seen
								// "top" is the height of the ceiling column
								if (fftop <= top+1 && R_FFloorCanClip(&ffloor[i]) && !curline->polyseg)
								{
									rw_ceilingmarked = true;
									cclip[cx] = ffbottom;
									if (yl < ffbottom)
										yl = ffbottom;

									if (markceiling && ceilingplane)
										ceilingplane->bottom[cx] = top;

									if (rw_silhouette)
									{
										(*rw_silhouette) |= SIL_TOP;
										(*rw_tsilheight) = INT32_MIN;
									}
								}
							}
						}
					}
				}
			}
		}

		//SoM: Calculate offsets for Thick fake floors.
		// calculate texture offset; texturecolumn and lighting are independent of wall tiers. A range without any
		// texture (plane marking only) never reads them, and the divisions have no side effects.
		if (texd)
		{
			angle = (centerangle_ + xtoviewangle[cx])>>ANGLETOFINESHIFT;
			textureoffset = offset_ - FixedMul(FINETANGENT(angle), distance_);
			// FixedDiv(x, FRACUNIT) is x unless |x| >= 2^30 (saturation)
			if (hasmid || masktc)
			{
				if (invmidx_ == FRACUNIT && textureoffset > -0x40000000 && textureoffset < 0x40000000)
					texturecolumn = textureoffset;
				else
					texturecolumn = FixedDiv(textureoffset, invmidx_);
			}

			// calculate lighting
			pindex = FixedMul(cscale, lightresfix)>>LIGHTSCALESHIFT;

			if (pindex >=  MAXLIGHTSCALE)
				pindex = MAXLIGHTSCALE-1;

			dc_colormap = wlights[pindex];
			dc_x = cx;

			if (hasextra)
				dc_colormap = extracm + (dc_colormap - cmaps);
		}

		if (!plain && dc_numlights && lightpre)
		{
			lightsdone = false; // PS2-90: rcolormap is read only by the column drawer: it is set when this column is drawn
		}
		else if (!plain && dc_numlights)
		{
			lighttable_t **xwalllights;
			for (i = 0; i < dc_numlights; i++)
			{
				INT32 lightnum;
				lightnum = R_SideLightLevel(curline->sidedef, dc_lightlist[i].lightlevel) >> LIGHTSEGSHIFT;

				if (dc_lightlist[i].extra_colormap)
					;
				else if (curline->v1->y == curline->v2->y)
					lightnum--;
				else if (curline->v1->x == curline->v2->x)
					lightnum++;

				if (lightnum < 0)
					xwalllights = scalelight[0];
				else if (lightnum >= LIGHTLEVELS)
					xwalllights = scalelight[LIGHTLEVELS-1];
				else
					xwalllights = scalelight[lightnum];

				pindex = FixedMul(cscale, lightresfix)>>LIGHTSCALESHIFT;

				if (pindex >=  MAXLIGHTSCALE)
					pindex = MAXLIGHTSCALE-1;

				if (dc_lightlist[i].extra_colormap)
					dc_lightlist[i].rcolormap = dc_lightlist[i].extra_colormap->colormap + (xwalllights[pindex] - colormaps);
				else
					dc_lightlist[i].rcolormap = xwalllights[pindex];
			}
		}

		fscale[cx] = cscale;

		// draw the wall tiers
		if (hasmid)
		{
			// single sided line
			if (yl <= yh && yh >= 0 && yl < vheight)
			{
				fixed_t offset = texturecolumn + offsetx_;
				UINT8 *pixels;

				dc_yl = yl;
				dc_yh = yh;
				dc_texturemid = midtexturemid_;
				dc_texheight = midtexheight;
				if (!haveinvs) { invs = 0xffffffffu / (unsigned)cscale; haveinvs = true; }
				dc_iscale = FixedMul((fixed_t)invs, midscaley);
				SEG_LIGHTS();
				pixels = SEG_COLUMN(midcols, midw, offset >> FRACBITS)->pixels;
				if (drawmiddle == 0)
				{
					dc_source = pixels;
					{ PS2SUB_B(60); PS2SUB_ADD(58, dc_yh - dc_yl + 1); colfunc(); PS2SUB_E(60); PS2SUB_CAT(); }
				}
				else if (drawmiddle == 1)
					R_DrawFlippedPost(pixels, (unsigned)midtexheight, colfunc);

				// dont draw anything more for this column, since
				// a midtexture blocks the view
				if (!rw_ceilingmarked)
					cclip[cx] = (INT16)vheight;
				if (!rw_floormarked)
					fclip[cx] = -1;
			}
			else
			{
				// note: don't use min/max macros, since casting from INT32 to INT16 is involved here
				if (markceiling && (!rw_ceilingmarked))
					cclip[cx] = (yl >= 0) ? ((yl > vheight) ? (INT16)vheight : (INT16)((INT16)yl - 1)) : -1;
				if (markfloor && (!rw_floormarked))
					fclip[cx] = (yh < vheight) ? ((yh < -1) ? -1 : (INT16)((INT16)yh + 1)) : (INT16)vheight;
			}
		}
		else
		{
			INT16 topclip = (yl >= 0) ? ((yl > vheight) ? (INT16)vheight : (INT16)((INT16)yl - 1)) : -1;
			INT16 bottomclip = (yh < vheight) ? ((yh < -1) ? -1 : (INT16)((INT16)yh + 1)) : (INT16)vheight;

			// two sided line
			if (hastop)
			{
				// top wall
				mid = cpixhigh>>HEIGHTBITS;
				cpixhigh += phstep_;

				if (mid >= fclip[cx])
					mid = fclip[cx]-1;

				if (invtopx_ == FRACUNIT && textureoffset > -0x40000000 && textureoffset < 0x40000000)
					toptexturecolumn = textureoffset;
				else
					toptexturecolumn = FixedDiv(textureoffset, invtopx_);

				if (mid >= yl) // back ceiling lower than front ceiling ?
				{
					if (yl >= vheight) // entirely off bottom of screen
					{
						if (!rw_ceilingmarked)
							cclip[cx] = (INT16)vheight;
					}
					else if (mid >= 0) // safe to draw top texture
					{
						fixed_t offset = offtop_;
						UINT8 *pixels;
						if (toppeg_)
							offset = -offset;
						offset = toptexturecolumn + offset;

						dc_yl = yl;
						dc_yh = mid;
						dc_texturemid = toptexturemid_;
						dc_texheight = toptexheight;
						if (!haveinvs) { invs = 0xffffffffu / (unsigned)cscale; haveinvs = true; }
						dc_iscale = FixedMul((fixed_t)invs, topscaley);
						SEG_LIGHTS();
						pixels = SEG_COLUMN(topcols, topw, offset >> FRACBITS)->pixels;
						if (drawtop == 0)
						{
							dc_source = pixels;
							{ PS2SUB_B(60); PS2SUB_ADD(58, dc_yh - dc_yl + 1); colfunc(); PS2SUB_E(60); PS2SUB_CAT(); }
						}
						else if (drawtop == 1)
							R_DrawFlippedPost(pixels, (unsigned)toptexheight, colfunc);
						cclip[cx] = (INT16)mid;
					}
					else if (!rw_ceilingmarked) // entirely off top of screen
						cclip[cx] = -1;
				}
				else if (!rw_ceilingmarked)
					cclip[cx] = topclip;

				if (topslide_ && oldtexturecolumn_top != -1) // PS2-165: FixedMul(0, x) is 0 (the slide is zero on every wall without slopes)
					toptexturemid_ += FixedMul(topslide_, oldtexturecolumn_top-textureoffset);
				oldtexturecolumn_top = textureoffset;
			}
			else if (markceiling && (!rw_ceilingmarked)) // no top wall
				cclip[cx] = topclip;

			if (hasbot)
			{
				// bottom wall
				mid = (cpixlow+HEIGHTUNIT-1)>>HEIGHTBITS;
				cpixlow += plstep_;

				// no space above wall?
				if (mid <= cclip[cx])
					mid = cclip[cx]+1;

				if (invbotx_ == FRACUNIT && textureoffset > -0x40000000 && textureoffset < 0x40000000)
					bottomtexturecolumn = textureoffset;
				else
					bottomtexturecolumn = FixedDiv(textureoffset, invbotx_);

				if (mid <= yh) // back floor higher than front floor ?
				{
					if (yh < 0) // entirely off top of screen
					{
						if (!rw_floormarked)
							fclip[cx] = -1;
					}
					else if (mid < vheight) // safe to draw bottom texture
					{
						fixed_t offset = offbot_;
						UINT8 *pixels;
						if (botpeg_)
							offset = -offset;
						offset = bottomtexturecolumn + offset;

						dc_yl = mid;
						dc_yh = yh;
						dc_texturemid = bottomtexturemid_;
						dc_texheight = bottexheight;
						if (!haveinvs) { invs = 0xffffffffu / (unsigned)cscale; haveinvs = true; }
						dc_iscale = FixedMul((fixed_t)invs, bottomscaley);
						SEG_LIGHTS();
						pixels = SEG_COLUMN(botcols, botw, offset >> FRACBITS)->pixels;
						if (drawbottom == 0)
						{
							dc_source = pixels;
							{ PS2SUB_B(60); PS2SUB_ADD(58, dc_yh - dc_yl + 1); colfunc(); PS2SUB_E(60); PS2SUB_CAT(); }
						}
						else if (drawbottom == 1)
							R_DrawFlippedPost(pixels, (unsigned)bottexheight, colfunc);
						fclip[cx] = (INT16)mid;
					}
					else if (!rw_floormarked)  // entirely off bottom of screen
						fclip[cx] = (INT16)vheight;
				}
				else if (!rw_floormarked)
					fclip[cx] = bottomclip;

				if (botslide_ && oldtexturecolumn_bottom != -1)
					bottomtexturemid_ += FixedMul(botslide_, oldtexturecolumn_bottom-textureoffset);
				oldtexturecolumn_bottom = textureoffset;
			}
			else if (markfloor && (!rw_floormarked)) // no bottom wall
				fclip[cx] = bottomclip;
		}

		if (masktc)
			maskedtexturecol[cx] = texturecolumn + offsetx_;

		if (thickcol)
			thicksidecol[cx] = textureoffset;

		if (masktheight)
		{
			if (curline->linedef->flags & ML_MIDPEG)
				maskedtextureheight[cx] = max(midtexturemid_, midtextureback_);
			else
				maskedtextureheight[cx] = min(midtexturemid_, midtextureback_);
		}

		if (midwork)
		{
			if ((midslide_ | midbackslide_) && oldtexturecolumn != -1)
			{
				INT32 diff = oldtexturecolumn-textureoffset;
				if (midneg_)
					diff = -diff;
				midtexturemid_ += FixedMul(midslide_, diff);
				midtextureback_ += FixedMul(midbackslide_, diff);
			}

			oldtexturecolumn = textureoffset;
		}

		if (invscale)
		{
			if (!haveinvs) { invs = 0xffffffffu / (unsigned)cscale; haveinvs = true; }
			invscale[cx] = invs;
		}

		if (!plain)
		{
			if (dc_numlights)
			{
				for (i = 0; i < dc_numlights; i++)
				{
					dc_lightlist[i].height += dc_lightlist[i].heightstep;
					if (dc_lightlist[i].flags & FOF_CUTSOLIDS)
						dc_lightlist[i].botheight += dc_lightlist[i].botheightstep;
				}
			}

			for (i = 0; i < numffloors; i++)
			{
				if (curline->polyseg && (ffloor[i].polyobj != curline->polyseg))
					continue;

				ffloor[i].f_frac += ffloor[i].f_step;
			}

			for (i = 0; i < numbackffloors; i++)
			{
				if (curline->polyseg && (ffloor[i].polyobj != curline->polyseg))
					continue;

				ffloor[i].f_clip[cx] = ffloor[i].c_clip[cx] = (INT16)((ffloor[i].b_frac >> HEIGHTBITS) & 0xFFFF);
				ffloor[i].b_frac += ffloor[i].b_step;
			}
		}

		cscale += sstep_;
		ctopfrac += tstep_;
		cbotfrac += bstep_;
	}
	PS2SUB_CATADD(84, 79);
	rw_x = cx;
	rw_scale = cscale;
	topfrac = ctopfrac;
	bottomfrac = cbotfrac;
	pixhigh = cpixhigh;
	pixlow = cpixlow;
	rw_midtexturemid = midtexturemid_;
	rw_midtextureback = midtextureback_;
	rw_toptexturemid = toptexturemid_;
	rw_bottomtexturemid = bottomtexturemid_;
}
#ifdef PS2_SUBPROF
int ps2_segcat;
#endif
static void R_RenderSegLoopX(void);
static void R_RenderSegLoop(void)
{
#ifdef PS2_SUBPROF
	// per-category cost: 70 plain/untextured, 72 plain/textured, 74 lights only, 76 ffloors (cycles/calls), 71/73/75/77 columns,
	// 64+n: cycles in colfunc of the category
	const int cat = (!numffloors && !numbackffloors && !dc_numlights) ? (segtextured ? 1 : 0) : (numffloors || numbackffloors ? 3 : 2);
	const INT32 ncols = rw_stopx - rw_x;
	PS2SUB_B(78);
	ps2_segcat = cat;
	R_RenderSegLoopX();
	ps2sub_cyc[70 + 2 * cat] += (unsigned)(PS2Sub_Count() - ps2sb_78);
	ps2sub_calls[70 + 2 * cat]++;
	ps2sub_calls[71 + 2 * cat] += (unsigned)ncols;
#else
	R_RenderSegLoopX();
#endif
}
static void R_RenderSegLoopX(void)
{
	if (!numffloors && !numbackffloors && !dc_numlights)
	{
		if (!segtextured)
		{
			R_RenderSegLoopT(true, 0);
			return;
		}
		switch ((midtexture ? 1 : 0) | (toptexture ? 2 : 0) | (bottomtexture ? 4 : 0))
		{
			case 0: R_RenderSegLoopT(true, 0); break;
			case 1: R_RenderSegLoopT(true, 1); break;
			case 2: R_RenderSegLoopT(true, 2); break;
			case 4: R_RenderSegLoopT(true, 4); break;
			case 6: R_RenderSegLoopT(true, 6); break;
			default: R_RenderSegLoopT(true, SEGT_ANY); break;
		}
	}
	else
		R_RenderSegLoopT(false, SEGT_ANY);
}
#else
static void R_RenderSegLoop (void)
{
	angle_t angle;
	fixed_t textureoffset;
	size_t pindex;
	INT32 yl;
	INT32 yh;

	INT32 mid;
	fixed_t texturecolumn = 0;
	fixed_t toptexturecolumn = 0;
	fixed_t bottomtexturecolumn = 0;
	fixed_t oldtexturecolumn = -1;
	fixed_t oldtexturecolumn_top = -1;
	fixed_t oldtexturecolumn_bottom = -1;
	INT32 top;
	INT32 bottom;
	INT32 i;

	fixed_t topscaley = rw_toptexturescaley;
	fixed_t midscaley = rw_midtexturescaley;
	fixed_t bottomscaley = rw_bottomtexturescaley;

	void (*drawtop)(UINT8 *, INT32) = R_DrawRegularWall;
	void (*drawmiddle)(UINT8 *, INT32) = R_DrawRegularWall;
	void (*drawbottom)(UINT8 *, INT32) = R_DrawRegularWall;

	if (dc_numlights)
		colfunc = colfuncs[COLDRAWFUNC_SHADOWED];

	if (toptexture && topscaley < 0)
	{
		topscaley = -topscaley;
		drawtop = R_DrawFlippedWall;
	}
	if (midtexture && midscaley < 0)
	{
		midscaley = -midscaley;
		drawmiddle = R_DrawFlippedWall;
	}
	if (bottomtexture && bottomscaley < 0)
	{
		bottomscaley = -bottomscaley;
		drawbottom = R_DrawFlippedWall;
	}

	if (!r_renderwalls)
	{
		drawtop = R_DrawNoWall;
		drawmiddle = R_DrawNoWall;
		drawbottom = R_DrawNoWall;
	}

	if (midtexture)
		R_CheckTextureCache(midtexture);
	if (toptexture)
		R_CheckTextureCache(toptexture);
	if (bottomtexture)
		R_CheckTextureCache(bottomtexture);

	if (dc_numlights)
		colfunc = colfuncs[COLDRAWFUNC_SHADOWED];

	for (; rw_x < rw_stopx; rw_x++)
	{
		// mark floor / ceiling areas
		yl = (topfrac+HEIGHTUNIT-1)>>HEIGHTBITS;

		top = ceilingclip[rw_x]+1;

		// no space above wall?
		if (yl < top)
			yl = top;

		if (markceiling)
		{
#if 0
			bottom = yl-1;

			if (bottom >= floorclip[rw_x])
				bottom = floorclip[rw_x]-1;

			if (top <= bottom)
#else
			bottom = yl > floorclip[rw_x] ? floorclip[rw_x] : yl;

			if (top <= --bottom && ceilingplane)
#endif
				R_ExpandPlaneY(ceilingplane, rw_x, top, bottom);
		}


		yh = bottomfrac>>HEIGHTBITS;

		bottom = floorclip[rw_x]-1;

		if (yh > bottom)
			yh = bottom;

		if (markfloor)
		{
			top = yh < ceilingclip[rw_x] ? ceilingclip[rw_x] : yh;

			if (++top <= bottom && floorplane)
				R_ExpandPlaneY(floorplane, rw_x, top, bottom);
		}

		rw_floormarked = false;
		rw_ceilingmarked = false;

		if (numffloors)
		{
			INT16 fftop, ffbottom;

			firstseg->frontscale[rw_x] = frontscale[rw_x];
			top = ceilingclip[rw_x]+1; // PRBoom
			bottom = floorclip[rw_x]-1; // PRBoom

			for (i = 0; i < numffloors; i++)
			{
				if (ffloor[i].polyobj && (!curline->polyseg || ffloor[i].polyobj != curline->polyseg))
					continue;

				if (ffloor[i].height < viewz)
				{
					INT32 top_w = (ffloor[i].f_frac >> HEIGHTBITS) + 1;
					INT32 bottom_w = ffloor[i].f_clip[rw_x];

					if (top_w < top)
						top_w = top;

					if (bottom_w > bottom)
						bottom_w = bottom;

					// Polyobject-specific hack to fix plane leaking -Red
					if (ffloor[i].polyobj && top_w >= bottom_w)
					{
						ffloor[i].plane->top[rw_x] = 0xFFFF;
						ffloor[i].plane->bottom[rw_x] = 0x0000; // fix for sky plane drawing crashes - Monster Iestyn 25/05/18
					}
					else
					{
						if (top_w <= bottom_w)
						{
							fftop = (INT16)top_w;
							ffbottom = (INT16)bottom_w;

							ffloor[i].plane->top[rw_x] = fftop;
							ffloor[i].plane->bottom[rw_x] = ffbottom;

							// Lactozilla: Cull part of the column by the 3D floor if it can't be seen
							// "bottom" is the top pixel of the floor column
							if (ffbottom >= bottom-1 && R_FFloorCanClip(&ffloor[i]) && !curline->polyseg)
							{
								rw_floormarked = true;
								floorclip[rw_x] = fftop;
								if (yh > fftop)
									yh = fftop;

								if (markfloor && floorplane)
									floorplane->top[rw_x] = bottom;

								if (rw_silhouette)
								{
									(*rw_silhouette) |= SIL_BOTTOM;
									(*rw_bsilheight) = INT32_MAX;
								}
							}
						}
					}
				}
				else if (ffloor[i].height > viewz)
				{
					INT32 top_w = ffloor[i].c_clip[rw_x] + 1;
					INT32 bottom_w = (ffloor[i].f_frac >> HEIGHTBITS);

					if (top_w < top)
						top_w = top;

					if (bottom_w > bottom)
						bottom_w = bottom;

					// Polyobject-specific hack to fix plane leaking -Red
					if (ffloor[i].polyobj && top_w >= bottom_w)
					{
						ffloor[i].plane->top[rw_x] = 0xFFFF;
						ffloor[i].plane->bottom[rw_x] = 0x0000; // fix for sky plane drawing crashes - Monster Iestyn 25/05/18
					}
					else
					{
						if (top_w <= bottom_w)
						{
							fftop = (INT16)top_w;
							ffbottom = (INT16)bottom_w;

							ffloor[i].plane->top[rw_x] = fftop;
							ffloor[i].plane->bottom[rw_x] = ffbottom;

							// Lactozilla: Cull part of the column by the 3D floor if it can't be seen
							// "top" is the height of the ceiling column
							if (fftop <= top+1 && R_FFloorCanClip(&ffloor[i]) && !curline->polyseg)
							{
								rw_ceilingmarked = true;
								ceilingclip[rw_x] = ffbottom;
								if (yl < ffbottom)
									yl = ffbottom;

								if (markceiling && ceilingplane)
									ceilingplane->bottom[rw_x] = top;

								if (rw_silhouette)
								{
									(*rw_silhouette) |= SIL_TOP;
									(*rw_tsilheight) = INT32_MIN;
								}
							}
						}
					}
				}
			}
		}

		//SoM: Calculate offsets for Thick fake floors.
		// calculate texture offset
		angle = (rw_centerangle + xtoviewangle[rw_x])>>ANGLETOFINESHIFT;
		textureoffset = rw_offset - FixedMul(FINETANGENT(angle), rw_distance);
		texturecolumn = FixedDiv(textureoffset, rw_invmidtexturescalex);

		// texturecolumn and lighting are independent of wall tiers
		if (segtextured)
		{
			// calculate lighting
			pindex = FixedMul(rw_scale, LIGHTRESOLUTIONFIX)>>LIGHTSCALESHIFT;

			if (pindex >=  MAXLIGHTSCALE)
				pindex = MAXLIGHTSCALE-1;

			dc_colormap = walllights[pindex];
			dc_x = rw_x;

			if (frontsector->extra_colormap)
				dc_colormap = frontsector->extra_colormap->colormap + (dc_colormap - colormaps);
		}

		if (dc_numlights)
		{
			lighttable_t **xwalllights;
			for (i = 0; i < dc_numlights; i++)
			{
				INT32 lightnum;
				lightnum = R_SideLightLevel(curline->sidedef, dc_lightlist[i].lightlevel) >> LIGHTSEGSHIFT;

				if (dc_lightlist[i].extra_colormap)
					;
				else if (curline->v1->y == curline->v2->y)
					lightnum--;
				else if (curline->v1->x == curline->v2->x)
					lightnum++;

				if (lightnum < 0)
					xwalllights = scalelight[0];
				else if (lightnum >= LIGHTLEVELS)
					xwalllights = scalelight[LIGHTLEVELS-1];
				else
					xwalllights = scalelight[lightnum];

				pindex = FixedMul(rw_scale, LIGHTRESOLUTIONFIX)>>LIGHTSCALESHIFT;

				if (pindex >=  MAXLIGHTSCALE)
					pindex = MAXLIGHTSCALE-1;

				if (dc_lightlist[i].extra_colormap)
					dc_lightlist[i].rcolormap = dc_lightlist[i].extra_colormap->colormap + (xwalllights[pindex] - colormaps);
				else
					dc_lightlist[i].rcolormap = xwalllights[pindex];
			}
		}

		frontscale[rw_x] = rw_scale;

		// draw the wall tiers
		if (midtexture)
		{
			// single sided line
			if (yl <= yh && yh >= 0 && yl < viewheight)
			{
				fixed_t offset = texturecolumn + rw_offsetx;

				dc_yl = yl;
				dc_yh = yh;
				dc_texturemid = rw_midtexturemid;
				dc_texheight = textureheight[midtexture]>>FRACBITS;
				dc_iscale = FixedMul(0xffffffffu / (unsigned)rw_scale, midscaley);
				drawmiddle(R_GetColumn(midtexture, offset >> FRACBITS)->pixels, dc_texheight);

				// dont draw anything more for this column, since
				// a midtexture blocks the view
				if (!rw_ceilingmarked)
					ceilingclip[rw_x] = (INT16)viewheight;
				if (!rw_floormarked)
					floorclip[rw_x] = -1;
			}
			else
			{
				// note: don't use min/max macros, since casting from INT32 to INT16 is involved here
				if (markceiling && (!rw_ceilingmarked))
					ceilingclip[rw_x] = (yl >= 0) ? ((yl > viewheight) ? (INT16)viewheight : (INT16)((INT16)yl - 1)) : -1;
				if (markfloor && (!rw_floormarked))
					floorclip[rw_x] = (yh < viewheight) ? ((yh < -1) ? -1 : (INT16)((INT16)yh + 1)) : (INT16)viewheight;
			}
		}
		else
		{
			INT16 topclip = (yl >= 0) ? ((yl > viewheight) ? (INT16)viewheight : (INT16)((INT16)yl - 1)) : -1;
			INT16 bottomclip = (yh < viewheight) ? ((yh < -1) ? -1 : (INT16)((INT16)yh + 1)) : (INT16)viewheight;

			// two sided line
			if (toptexture)
			{
				// top wall
				mid = pixhigh>>HEIGHTBITS;
				pixhigh += pixhighstep;

				if (mid >= floorclip[rw_x])
					mid = floorclip[rw_x]-1;

				toptexturecolumn = FixedDiv(textureoffset, rw_invtoptexturescalex);

				if (mid >= yl) // back ceiling lower than front ceiling ?
				{
					if (yl >= viewheight) // entirely off bottom of screen
					{
						if (!rw_ceilingmarked)
							ceilingclip[rw_x] = (INT16)viewheight;
					}
					else if (mid >= 0) // safe to draw top texture
					{
						fixed_t offset = rw_offset_top;
						if (rw_toptexturescalex < 0)
							offset = -offset;
						offset = toptexturecolumn + offset;

						dc_yl = yl;
						dc_yh = mid;
						dc_texturemid = rw_toptexturemid;
						dc_texheight = textureheight[toptexture]>>FRACBITS;
						dc_iscale = FixedMul(0xffffffffu / (unsigned)rw_scale, topscaley);
						drawtop(R_GetColumn(toptexture, offset >> FRACBITS)->pixels, dc_texheight);
						ceilingclip[rw_x] = (INT16)mid;
					}
					else if (!rw_ceilingmarked) // entirely off top of screen
						ceilingclip[rw_x] = -1;
				}
				else if (!rw_ceilingmarked)
					ceilingclip[rw_x] = topclip;

				if (oldtexturecolumn_top != -1)
					rw_toptexturemid += FixedMul(rw_toptextureslide, oldtexturecolumn_top-textureoffset);
				oldtexturecolumn_top = textureoffset;
			}
			else if (markceiling && (!rw_ceilingmarked)) // no top wall
				ceilingclip[rw_x] = topclip;

			if (bottomtexture)
			{
				// bottom wall
				mid = (pixlow+HEIGHTUNIT-1)>>HEIGHTBITS;
				pixlow += pixlowstep;

				// no space above wall?
				if (mid <= ceilingclip[rw_x])
					mid = ceilingclip[rw_x]+1;

				bottomtexturecolumn = FixedDiv(textureoffset, rw_invbottomtexturescalex);

				if (mid <= yh) // back floor higher than front floor ?
				{
					if (yh < 0) // entirely off top of screen
					{
						if (!rw_floormarked)
							floorclip[rw_x] = -1;
					}
					else if (mid < viewheight) // safe to draw bottom texture
					{
						fixed_t offset = rw_offset_bottom;
						if (rw_bottomtexturescalex < 0)
							offset = -offset;
						offset = bottomtexturecolumn + offset;

						dc_yl = mid;
						dc_yh = yh;
						dc_texturemid = rw_bottomtexturemid;
						dc_texheight = textureheight[bottomtexture]>>FRACBITS;
						dc_iscale = FixedMul(0xffffffffu / (unsigned)rw_scale, bottomscaley);
						drawbottom(R_GetColumn(bottomtexture, offset >> FRACBITS)->pixels, dc_texheight);
						floorclip[rw_x] = (INT16)mid;
					}
					else if (!rw_floormarked)  // entirely off bottom of screen
						floorclip[rw_x] = (INT16)viewheight;
				}
				else if (!rw_floormarked)
					floorclip[rw_x] = bottomclip;

				if (oldtexturecolumn_bottom != -1)
					rw_bottomtexturemid += FixedMul(rw_bottomtextureslide, oldtexturecolumn_bottom-textureoffset);
				oldtexturecolumn_bottom = textureoffset;
			}
			else if (markfloor && (!rw_floormarked)) // no bottom wall
				floorclip[rw_x] = bottomclip;
		}

		if (maskedtexturecol)
			maskedtexturecol[rw_x] = texturecolumn + rw_offsetx;

		if (thicksidecol)
			thicksidecol[rw_x] = textureoffset;

		if (maskedtextureheight)
		{
			if (curline->linedef->flags & ML_MIDPEG)
				maskedtextureheight[rw_x] = max(rw_midtexturemid, rw_midtextureback);
			else
				maskedtextureheight[rw_x] = min(rw_midtexturemid, rw_midtextureback);
		}

		if (midtexture || maskedtextureheight)
		{
			if (oldtexturecolumn != -1)
			{
				INT32 diff = oldtexturecolumn-textureoffset;
				if (rw_invmidtexturescalex < 0)
					diff = -diff;
				rw_midtexturemid += FixedMul(rw_midtextureslide, diff);
				rw_midtextureback += FixedMul(rw_midtexturebackslide, diff);
			}

			oldtexturecolumn = textureoffset;
		}

		if (invscale)
			invscale[rw_x] = 0xffffffffu / (unsigned)rw_scale;

		if (dc_numlights)
		{
			for (i = 0; i < dc_numlights; i++)
			{
				dc_lightlist[i].height += dc_lightlist[i].heightstep;
				if (dc_lightlist[i].flags & FOF_CUTSOLIDS)
					dc_lightlist[i].botheight += dc_lightlist[i].botheightstep;
			}
		}

		for (i = 0; i < numffloors; i++)
		{
			if (curline->polyseg && (ffloor[i].polyobj != curline->polyseg))
				continue;

			ffloor[i].f_frac += ffloor[i].f_step;
		}

		for (i = 0; i < numbackffloors; i++)
		{
			if (curline->polyseg && (ffloor[i].polyobj != curline->polyseg))
				continue;

			ffloor[i].f_clip[rw_x] = ffloor[i].c_clip[rw_x] = (INT16)((ffloor[i].b_frac >> HEIGHTBITS) & 0xFFFF);
			ffloor[i].b_frac += ffloor[i].b_step;
		}

		rw_scale += rw_scalestep;
		topfrac += topstep;
		bottomfrac += bottomstep;
	}
}
#endif

// Uses precalculated seg->length
static INT64 R_CalcSegDist(seg_t* seg, INT64 x2, INT64 y2)
{
	if (!seg->linedef->dy)
		return llabs(y2 - seg->v1->y);
	else if (!seg->linedef->dx)
		return llabs(x2 - seg->v1->x);
	else
	{
		INT64 dx = (seg->v2->x)-(seg->v1->x);
		INT64 dy = (seg->v2->y)-(seg->v1->y);
		INT64 vdx = x2-(seg->v1->x);
		INT64 vdy = y2-(seg->v1->y);
		return ((dy*vdx)-(dx*vdy))/(seg->length);
	}
}

//SoM: Code to remove limits on openings.
static void R_AllocClippingTables(size_t range)
{
	size_t pos = lastopening - openings;
	size_t need = range * 2; // for both sprtopclip and sprbottomclip

	if (pos + need < numopenings)
		return;

	INT16 *oldopenings = openings;
	INT16 *oldlast = lastopening;

	if (numopenings == 0)
		numopenings = 16384;

	numopenings += need;
	openings = Z_Realloc(openings, numopenings * sizeof (*openings), PU_STATIC, NULL);
	lastopening = openings + pos;

	if (oldopenings == NULL)
		return;

	// borrowed fix from *cough* zdoom *cough*
	// [RH] We also need to adjust the openings pointers that
	//    were already stored in drawsegs.
	for (drawseg_t *ds = drawsegs; ds < ds_p; ds++)
	{
		// Check if it's in range of the openings
		if (ds->sprtopclip + ds->x1 >= oldopenings && ds->sprtopclip + ds->x1 <= oldlast)
			ds->sprtopclip = (ds->sprtopclip - oldopenings) + openings;
		if (ds->sprbottomclip + ds->x1 >= oldopenings && ds->sprbottomclip + ds->x1 <= oldlast)
			ds->sprbottomclip = (ds->sprbottomclip - oldopenings) + openings;
	}
}

static void R_AllocTextureColumnTables(size_t range)
{
	size_t pos = curtexturecolumntable - texturecolumntable;
	size_t need = range * 4;

	if (pos + need < texturecolumntablesize)
		return;

	fixed_t *oldtable = texturecolumntable;
	fixed_t *oldlast = curtexturecolumntable;

	if (texturecolumntablesize == 0)
		texturecolumntablesize = 16384;

	texturecolumntablesize += need;
	texturecolumntable = Z_Realloc(texturecolumntable, texturecolumntablesize * sizeof (*texturecolumntable), PU_STATIC, NULL);
	curtexturecolumntable = texturecolumntable + pos;

	if (oldtable == NULL)
		return;

	for (drawseg_t *ds = drawsegs; ds < ds_p; ds++)
	{
		// Check if it's in range of the tables
#define CHECK(which) \
		if (which + ds->x1 >= oldtable && which + ds->x1 <= oldlast) \
			which = (which - oldtable) + texturecolumntable

		CHECK(ds->maskedtexturecol);
		CHECK(ds->maskedtextureheight);
		CHECK(ds->thicksidecol);
		CHECK(ds->invscale);

#undef CHECK
	}
}

//
// R_ScaleFromGlobalAngle
// Returns the texture mapping scale for the current line (horizontal span)
//  at the given angle.
// rw_distance must be calculated first.
//
// killough 5/2/98: reformatted, cleaned up
//
// note: THIS IS USED ONLY FOR WALLS!
static fixed_t R_ScaleFromGlobalAngle(angle_t visangle)
{
	angle_t anglea = ANGLE_90 + (visangle-viewangle);
	angle_t angleb = ANGLE_90 + (visangle-rw_normalangle);
	fixed_t den = FixedMul(rw_distance, FINESINE(anglea>>ANGLETOFINESHIFT));
	// proff 11/06/98: Changed for high-res
	fixed_t num = FixedMul(projectiony, FINESINE(angleb>>ANGLETOFINESHIFT));

	if (den > num>>16)
	{
		num = FixedDiv(num, den);
		if (num > 64*FRACUNIT)
			return 64*FRACUNIT;
		if (num < 256)
			return 256;
		return num;
	}
	return 64*FRACUNIT;
}

//
// R_StoreWallRange
// A wall segment will be drawn
//  between start and stop pixels (inclusive).
//
void R_StoreWallRange(INT32 start, INT32 stop)
{
	PS2SUB_B(0);
	PS2SUB_B(1);
	fixed_t       hyp;
	fixed_t       sineval;
	angle_t       distangle, offsetangle;
	boolean longboi;
	INT32           lightnum;
	INT32           i, p;
	lightlist_t   *light;
	r_lightlist_t *rlight;
	INT32 range;
	vertex_t segleft, segright;
	fixed_t ceilingfrontslide, floorfrontslide, ceilingbackslide, floorbackslide;

	maskedtexturecol = NULL;
	maskedtextureheight = NULL;
	thicksidecol = NULL;
	invscale = NULL;

	//initialize segleft and segright
	memset(&segleft, 0x00, sizeof(segleft));
	memset(&segright, 0x00, sizeof(segright));

	colfunc = colfuncs[BASEDRAWFUNC];

	if (ds_p == drawsegs+maxdrawsegs)
	{
		size_t curpos = curdrawsegs - drawsegs;
		size_t pos = ds_p - drawsegs;
#if defined(PS2) || defined(PS2_PROFILE)
		size_t newmax;
		if (maxdrawsegs > SIZE_MAX - maxdrawsegs / 2)
			I_Error("R_StoreWallRange: drawseg capacity overflow");
		newmax = maxdrawsegs ? maxdrawsegs + maxdrawsegs / 2 : 128;
		if (newmax > SIZE_MAX / sizeof (*drawsegs))
			I_Error("R_StoreWallRange: drawseg allocation overflow");
		size_t firstpos = firstseg ? (size_t)(firstseg - drawsegs) : 0;
		boolean hadfirstseg = firstseg != NULL;
#else
		size_t newmax = maxdrawsegs ? maxdrawsegs*2 : 128;
#endif
#if !defined(PS2) && !defined(PS2_PROFILE)
		if (firstseg)
			firstseg = (drawseg_t *)(firstseg - drawsegs);
#endif
		drawsegs = Z_Realloc(drawsegs, newmax*sizeof (*drawsegs), PU_STATIC, NULL);
		ds_p = drawsegs + pos;
		maxdrawsegs = newmax;
		curdrawsegs = drawsegs + curpos;
#if defined(PS2) || defined(PS2_PROFILE)
		if (hadfirstseg)
			firstseg = drawsegs + firstpos;
#else
		if (firstseg)
			firstseg = drawsegs + (size_t)firstseg;
#endif
	}

	sidedef = curline->sidedef;
	linedef = curline->linedef;

	// calculate rw_distance for scale calculation
	rw_normalangle = curline->angle + ANGLE_90;
	offsetangle = abs((INT32)(rw_normalangle-rw_angle1));

	if (offsetangle > ANGLE_90)
		offsetangle = ANGLE_90;

	distangle = ANGLE_90 - offsetangle;
	sineval = FINESINE(distangle>>ANGLETOFINESHIFT);

	hyp = R_PointToDist(curline->v1->x, curline->v1->y);
	rw_distance = FixedMul(hyp, sineval);
	longboi = (hyp >= INT32_MAX);

	// big room fix
	if (longboi)
		rw_distance = (fixed_t)R_CalcSegDist(curline,viewx,viewy);

	PS2SUB_E(1);
	PS2SUB_B(2);
	ds_p->x1 = rw_x = start;
	ds_p->x2 = stop;
	ds_p->curline = curline;
	rw_stopx = stop+1;

	// calculate scale at both ends and step
	ds_p->scale1 = rw_scale = R_ScaleFromGlobalAngle(viewangle + xtoviewangle[start]);

	if (stop > start)
	{
		ds_p->scale2 = R_ScaleFromGlobalAngle(viewangle + xtoviewangle[stop]);
		range = stop-start;
	}
	else
	{
		// UNUSED: try to fix the stretched line bug
#if 0
		if (rw_distance < FRACUNIT/2)
		{
			fixed_t         tr_x,tr_y;
			fixed_t         gxt,gyt;
			CONS_Debug(DBG_RENDER, "TRYING TO FIX THE STRETCHED ETC\n");

			tr_x = curline->v1->x - viewx;
			tr_y = curline->v1->y - viewy;

			gxt = FixedMul(tr_x, viewcos);
			gyt = -FixedMul(tr_y, viewsin);
			ds_p->scale1 = FixedDiv(projection, gxt - gyt);
		}
#endif
		ds_p->scale2 = ds_p->scale1;
		range = 1;
	}

	ds_p->scalestep = rw_scalestep = (ds_p->scale2 - rw_scale) / (range);

	PS2SUB_E(2);
	// calculate texture boundaries
	//  and decide if floor / ceiling marks are needed
	// Figure out map coordinates of where start and end are mapping to on seg, so we can clip right for slope bullshit
	PS2SUB_B(3);
	if (frontsector->hasslope || (backsector && backsector->hasslope)) // Commenting this out for FOFslop. -Red
	{
		angle_t temp;

		// left
		temp = xtoviewangle[start]+viewangle;

		{
#ifdef PS2_OPT_SEGS // PS2-16 candidate: rational intersection (double rounding can differ)
			RQ_RayHitsSeg(curline->v1->x, curline->v1->y, curline->v2->x, curline->v2->y, viewx, viewy,
				FINESINE(temp>>ANGLETOFINESHIFT), FINECOSINE(temp>>ANGLETOFINESHIFT), &segleft.x, &segleft.y);
			ds_p->leftpos.x = segleft.x;
			ds_p->leftpos.y = segleft.y;
#else
			// Both lines can be written in slope-intercept form, so figure out line intersection
			double a1, b1, c1, a2, b2, c2, det; // 1 is the seg, 2 is the view angle vector...
			///TODO: convert to fixed point

			a1 = FixedToDouble(curline->v2->y-curline->v1->y);
			b1 = FixedToDouble(curline->v1->x-curline->v2->x);
			c1 = a1*FixedToDouble(curline->v1->x) + b1*FixedToDouble(curline->v1->y);

			a2 = -FixedToDouble(FINESINE(temp>>ANGLETOFINESHIFT));
			b2 = FixedToDouble(FINECOSINE(temp>>ANGLETOFINESHIFT));
			c2 = a2*FixedToDouble(viewx) + b2*FixedToDouble(viewy);

			det = a1*b2 - a2*b1;

			ds_p->leftpos.x = segleft.x = DoubleToFixed((b2*c1 - b1*c2)/det);
			ds_p->leftpos.y = segleft.y = DoubleToFixed((a1*c2 - a2*c1)/det);
#endif
		}

		// right
		temp = xtoviewangle[stop]+viewangle;

		{
#ifdef PS2_OPT_SEGS
			RQ_RayHitsSeg(curline->v1->x, curline->v1->y, curline->v2->x, curline->v2->y, viewx, viewy,
				FINESINE(temp>>ANGLETOFINESHIFT), FINECOSINE(temp>>ANGLETOFINESHIFT), &segright.x, &segright.y);
			ds_p->rightpos.x = segright.x;
			ds_p->rightpos.y = segright.y;
#else
			// Both lines can be written in slope-intercept form, so figure out line intersection
			double a1, b1, c1, a2, b2, c2, det; // 1 is the seg, 2 is the view angle vector...
			///TODO: convert to fixed point

			a1 = FixedToDouble(curline->v2->y-curline->v1->y);
			b1 = FixedToDouble(curline->v1->x-curline->v2->x);
			c1 = a1*FixedToDouble(curline->v1->x) + b1*FixedToDouble(curline->v1->y);

			a2 = -FixedToDouble(FINESINE(temp>>ANGLETOFINESHIFT));
			b2 = FixedToDouble(FINECOSINE(temp>>ANGLETOFINESHIFT));
			c2 = a2*FixedToDouble(viewx) + b2*FixedToDouble(viewy);

			det = a1*b2 - a2*b1;

			ds_p->rightpos.x = segright.x = DoubleToFixed((b2*c1 - b1*c2)/det);
			ds_p->rightpos.y = segright.y = DoubleToFixed((a1*c2 - a2*c1)/det);
#endif
		}
	}

	PS2SUB_E(3);

#define SLOPEPARAMS(slope, end1, end2, normalheight) \
	end1 = P_GetZAt(slope,  segleft.x,  segleft.y, normalheight); \
	end2 = P_GetZAt(slope, segright.x, segright.y, normalheight);

	PS2SUB_B(4);
	SLOPEPARAMS(frontsector->c_slope, worldtop,    worldtopslope,    frontsector->ceilingheight)
	SLOPEPARAMS(frontsector->f_slope, worldbottom, worldbottomslope, frontsector->floorheight)
	// subtract viewz from these to turn them into
	// positions relative to the camera's z position
	worldtop -= viewz;
	worldtopslope -= viewz;
	worldbottom -= viewz;
	worldbottomslope -= viewz;

	midtexture = toptexture = bottomtexture = maskedtexture = 0;
	ds_p->maskedtexturecol = NULL;
	ds_p->maskedtextureheight = NULL;
	ds_p->numthicksides = numthicksides = 0;
	ds_p->thicksidecol = NULL;
	ds_p->invscale = NULL;
	ds_p->tsilheight = 0;

	texcoltables = false;

	numbackffloors = 0;

#ifndef PS2_OPT_REND // PS2-90: thicksides[] is read only below numthicksides, which the code below writes entry by entry
	for (i = 0; i < MAXFFLOORS; i++)
		ds_p->thicksides[i] = NULL;
#else
	i = 0; // (every user of i below assigns it first; this only keeps the compiler's flow analysis happy)
#endif

	if (numffloors)
	{
		for (i = 0; i < numffloors; i++)
		{
			if (ffloor[i].polyobj && (!ds_p->curline->polyseg || ffloor[i].polyobj != ds_p->curline->polyseg))
				continue;

			ffloor[i].f_pos       = P_GetZAt(ffloor[i].slope, segleft .x, segleft .y, ffloor[i].height) - viewz;
			ffloor[i].f_pos_slope = P_GetZAt(ffloor[i].slope, segright.x, segright.y, ffloor[i].height) - viewz;
		}
	}

	PS2SUB_E(4);
	PS2SUB_B(5);
	// Set up texture Y offset slides for sloped walls
	rw_toptextureslide = rw_midtextureslide = rw_bottomtextureslide = 0;
	ceilingfrontslide = floorfrontslide = ceilingbackslide = floorbackslide = 0;

#ifdef PS2_OPT_REND
	// PS2-80: the line angle is only read by the slope branches below (R_PointToAngle2 has no side effects)
	if (frontsector->f_slope || frontsector->c_slope || (backsector && (backsector->f_slope || backsector->c_slope)))
#endif
	{
		angle_t lineangle = R_PointToAngle2(curline->v1->x, curline->v1->y, curline->v2->x, curline->v2->y);

		if (frontsector->f_slope)
			floorfrontslide = R_GetSlopeTextureSlide(frontsector->f_slope, lineangle);

		if (frontsector->c_slope)
			ceilingfrontslide = R_GetSlopeTextureSlide(frontsector->c_slope, lineangle);

		if (backsector)
		{
			if (backsector->f_slope)
				floorbackslide = R_GetSlopeTextureSlide(backsector->f_slope, lineangle);

			if (backsector->c_slope)
				ceilingbackslide = R_GetSlopeTextureSlide(backsector->c_slope, lineangle);
		}
	}

	PS2SUB_E(5);
	PS2SUB_B(6);
	rw_midtexturescalex = SIDE_SCALEX_MID(sidedef);
	rw_midtexturescaley = SIDE_SCALEY_MID(sidedef);
#ifdef PS2_OPT_REND
	rw_invmidtexturescalex = (rw_midtexturescalex == FRACUNIT) ? FRACUNIT : FixedDiv(FRACUNIT, rw_midtexturescalex); // PS2-80: FixedDiv(FRACUNIT, FRACUNIT) == FRACUNIT
#else
	rw_invmidtexturescalex = FixedDiv(FRACUNIT, rw_midtexturescalex);
#endif

	if (!backsector)
	{
		midtexture = R_GetTextureNum(sidedef->midtexture);

		// a single sided line is terminal, so it must mark ends
		markfloor = markceiling = true;

		fixed_t rowoffset = sidedef->rowoffset + SIDE_OFFSETY_MID(sidedef);
		fixed_t texheight = textureheight[midtexture];
		fixed_t scaley = abs(rw_midtexturescaley);

		if (rw_midtexturescaley > 0)
		{
			if (linedef->flags & ML_NOSKEW)
			{
				if (linedef->flags & ML_DONTPEGBOTTOM)
					rw_midtexturemid = FixedMul(frontsector->floorheight - viewz, scaley) + texheight;
				else
					rw_midtexturemid = FixedMul(frontsector->ceilingheight - viewz, scaley);
			}
			else if (linedef->flags & ML_DONTPEGBOTTOM)
			{
				rw_midtexturemid = FixedMul(worldbottom, scaley) + texheight;
				rw_midtextureslide = FixedMul(floorfrontslide, scaley);
			}
			else
			{
				// top of texture at top
				rw_midtexturemid = FixedMul(worldtop, scaley);
				rw_midtextureslide = FixedMul(ceilingfrontslide, scaley);
			}
		}
		else
		{
			// Upside down
			rowoffset = -rowoffset;

			if (linedef->flags & ML_NOSKEW)
			{
				if (linedef->flags & ML_DONTPEGBOTTOM)
					rw_midtexturemid = FixedMul(frontsector->floorheight - viewz, scaley);
				else
					rw_midtexturemid = FixedMul(frontsector->ceilingheight - viewz, scaley) + texheight;
			}
			else if (linedef->flags & ML_DONTPEGBOTTOM)
			{
				rw_midtexturemid = FixedMul(worldbottom, scaley);
				rw_midtextureslide = FixedMul(floorfrontslide, scaley);
			}
			else
			{
				// top of texture at top
				rw_midtexturemid = FixedMul(worldtop, scaley) + texheight;
				rw_midtextureslide = FixedMul(ceilingfrontslide, scaley);
			}
		}

		rw_midtexturemid += rowoffset;

		ds_p->silhouette = SIL_BOTH;
		ds_p->sprtopclip = screenheightarray;
		ds_p->sprbottomclip = negonearray;
		ds_p->bsilheight = INT32_MAX;
		ds_p->tsilheight = INT32_MIN;
	}
	else
	{
		// two sided line
		SLOPEPARAMS(backsector->c_slope, worldhigh, worldhighslope, backsector->ceilingheight)
		SLOPEPARAMS(backsector->f_slope, worldlow,  worldlowslope,  backsector->floorheight)
		worldhigh -= viewz;
		worldhighslope -= viewz;
		worldlow -= viewz;
		worldlowslope -= viewz;

		ds_p->sprtopclip = ds_p->sprbottomclip = NULL;
		ds_p->silhouette = 0;

		if (!bothfloorssky)
		{
			if (worldbottomslope > worldlowslope || worldbottom > worldlow)
			{
				ds_p->silhouette = SIL_BOTTOM;
				if (P_GetSectorFloorZAt(backsector, viewx, viewy) > viewz)
					ds_p->bsilheight = INT32_MAX;
				else
					ds_p->bsilheight = (frontsector->f_slope ? INT32_MAX : frontsector->floorheight);
			}
			else if (P_GetSectorFloorZAt(backsector, viewx, viewy) > viewz)
			{
				ds_p->silhouette = SIL_BOTTOM;
				ds_p->bsilheight = INT32_MAX;
				// ds_p->sprbottomclip = negonearray;
			}
		}

		if (!bothceilingssky)
		{
			if (worldtopslope < worldhighslope || worldtop < worldhigh)
			{
				ds_p->silhouette |= SIL_TOP;
				if (P_GetSectorCeilingZAt(backsector, viewx, viewy) < viewz)
					ds_p->tsilheight = INT32_MIN;
				else
					ds_p->tsilheight = (frontsector->c_slope ? INT32_MIN : frontsector->ceilingheight);
			}
			else if (P_GetSectorCeilingZAt(backsector, viewx, viewy) < viewz)
			{
				ds_p->silhouette |= SIL_TOP;
				ds_p->tsilheight = INT32_MIN;
				// ds_p->sprtopclip = screenheightarray;
			}
		}

		if (!bothceilingssky && !bothfloorssky)
		{
			if (worldhigh <= worldbottom && worldhighslope <= worldbottomslope)
			{
				ds_p->sprbottomclip = negonearray;
				ds_p->bsilheight = INT32_MAX;
				ds_p->silhouette |= SIL_BOTTOM;
			}

			if (worldlow >= worldtop && worldlowslope >= worldtopslope)
			{
				ds_p->sprtopclip = screenheightarray;
				ds_p->tsilheight = INT32_MIN;
				ds_p->silhouette |= SIL_TOP;
			}
		}

		//SoM: 3/25/2000: This code fixes an automap bug that didn't check
		// frontsector->ceiling and backsector->floor to see if a door was closed.
		// Without the following code, sprites get displayed behind closed doors.
		if (!bothceilingssky && !bothfloorssky)
		{
			if (doorclosed || (worldhigh <= worldbottom && worldhighslope <= worldbottomslope))
			{
				ds_p->sprbottomclip = negonearray;
				ds_p->bsilheight = INT32_MAX;
				ds_p->silhouette |= SIL_BOTTOM;
			}
			if (doorclosed || (worldlow >= worldtop && worldlowslope >= worldtopslope))
			{                   // killough 1/17/98, 2/8/98
				ds_p->sprtopclip = screenheightarray;
				ds_p->tsilheight = INT32_MIN;
				ds_p->silhouette |= SIL_TOP;
			}
		}

		if (bothfloorssky)
		{
			// see double ceiling skies comment
			// this is the same but for upside down thok barriers where the floor is sky and the ceiling is normal
			markfloor = false;
		}
		else if (worldlow != worldbottom
			|| worldlowslope != worldbottomslope
			|| backsector->f_slope != frontsector->f_slope
			|| backsector->floorpic != frontsector->floorpic
			|| backsector->lightlevel != frontsector->lightlevel
			//SoM: 3/22/2000: Check floor x and y offsets.
			|| backsector->floorxoffset != frontsector->floorxoffset
			|| backsector->flooryoffset != frontsector->flooryoffset
			|| backsector->floorxscale != frontsector->floorxscale
			|| backsector->flooryscale != frontsector->flooryscale
			|| backsector->floorangle != frontsector->floorangle
			//SoM: 3/22/2000: Prevents bleeding.
			|| (frontsector->heightsec != -1 && frontsector->floorpic != skyflatnum)
			|| backsector->floorlightlevel != frontsector->floorlightlevel
			|| backsector->floorlightabsolute != frontsector->floorlightabsolute
			|| backsector->floorlightsec != frontsector->floorlightsec
			//SoM: 4/3/2000: Check for colormaps
			|| frontsector->extra_colormap != backsector->extra_colormap
			|| !P_CompareSectorPortals(P_SectorGetFloorPortal(frontsector), P_SectorGetFloorPortal(backsector))
			|| (frontsector->ffloors != backsector->ffloors && !Tag_Compare(&frontsector->tags, &backsector->tags)))
		{
			markfloor = true;
		}
		else
		{
			// same plane on both sides
			markfloor = false;
		}

		if (bothceilingssky)
		{
			// double ceiling skies are special
			// we don't want to lower the ceiling clipping, (no new plane is drawn anyway)
			// so we can see the floor of thok barriers always regardless of sector properties
			markceiling = false;
		}
		else if (worldhigh != worldtop
			|| worldhighslope != worldtopslope
			|| backsector->c_slope != frontsector->c_slope
			|| backsector->ceilingpic != frontsector->ceilingpic
			|| backsector->lightlevel != frontsector->lightlevel
			//SoM: 3/22/2000: Check floor x and y offsets.
			|| backsector->ceilingxoffset != frontsector->ceilingxoffset
			|| backsector->ceilingyoffset != frontsector->ceilingyoffset
			|| backsector->ceilingxscale != frontsector->ceilingxscale
			|| backsector->ceilingyscale != frontsector->ceilingyscale
			|| backsector->ceilingangle != frontsector->ceilingangle
			//SoM: 3/22/2000: Prevents bleeding.
			|| (frontsector->heightsec != -1 && frontsector->ceilingpic != skyflatnum)
			|| backsector->ceilinglightlevel != frontsector->ceilinglightlevel
			|| backsector->ceilinglightabsolute != frontsector->ceilinglightabsolute
			|| backsector->ceilinglightsec != frontsector->ceilinglightsec
			//SoM: 4/3/2000: Check for colormaps
			|| frontsector->extra_colormap != backsector->extra_colormap
			|| !P_CompareSectorPortals(P_SectorGetCeilingPortal(frontsector), P_SectorGetCeilingPortal(backsector))
			|| (frontsector->ffloors != backsector->ffloors && !Tag_Compare(&frontsector->tags, &backsector->tags)))
		{
			markceiling = true;
		}
		else
		{
			// same plane on both sides
			markceiling = false;
		}

		if (!bothceilingssky && !bothfloorssky)
		{
			if ((worldhigh <= worldbottom && worldhighslope <= worldbottomslope)
			 || (worldlow >= worldtop && worldlowslope >= worldtopslope))
			{
				// closed door
				markceiling = markfloor = true;
			}
		}

		fixed_t toprowoffset = sidedef->rowoffset + SIDE_OFFSETY_TOP(sidedef);
		fixed_t botrowoffset = sidedef->rowoffset + SIDE_OFFSETY_BOTTOM(sidedef);

		// check TOP TEXTURE
		if (!bothceilingssky // never draw the top texture if on
			&& (worldhigh < worldtop || worldhighslope < worldtopslope))
		{
			toptexture = R_GetTextureNum(sidedef->toptexture);

			rw_toptexturescalex = SIDE_SCALEX_TOP(sidedef);
			rw_toptexturescaley = SIDE_SCALEY_TOP(sidedef);

#ifdef PS2_OPT_REND
			rw_invtoptexturescalex = (rw_toptexturescalex == FRACUNIT) ? FRACUNIT : FixedDiv(FRACUNIT, rw_toptexturescalex);
#else
			rw_invtoptexturescalex = FixedDiv(FRACUNIT, rw_toptexturescalex);
#endif

			if (rw_toptexturescaley < 0)
				toprowoffset = -toprowoffset;

			fixed_t texheight = textureheight[toptexture];

			// Ignore slopes for lower/upper textures unless flag is checked
			if (!(linedef->flags & ML_SKEWTD))
			{
				if (linedef->flags & ML_DONTPEGTOP)
					rw_toptexturemid = frontsector->ceilingheight - viewz;
				else
					rw_toptexturemid = backsector->ceilingheight - viewz;
			}
			else if (linedef->flags & ML_DONTPEGTOP)
			{
				// top of texture at top
				rw_toptexturemid = worldtop;
				rw_toptextureslide = FixedMul(ceilingfrontslide, abs(rw_toptexturescaley));
			}
			else
			{
				rw_toptexturemid = worldhigh + texheight;
				rw_toptextureslide = FixedMul(ceilingbackslide, abs(rw_toptexturescaley));
			}

			rw_toptexturemid = FixedMul(rw_toptexturemid, abs(rw_toptexturescaley));
			rw_toptexturemid += toprowoffset;
		}

		// check BOTTOM TEXTURE
		if (!bothfloorssky // never draw the bottom texture if on
			&& (worldlow > worldbottom || worldlowslope > worldbottomslope)) // Only if VISIBLE!!!
		{
			// bottom texture
			bottomtexture = R_GetTextureNum(sidedef->bottomtexture);

			rw_bottomtexturescalex = SIDE_SCALEX_BOTTOM(sidedef);
			rw_bottomtexturescaley = SIDE_SCALEY_BOTTOM(sidedef);

#ifdef PS2_OPT_REND
			rw_invbottomtexturescalex = (rw_bottomtexturescalex == FRACUNIT) ? FRACUNIT : FixedDiv(FRACUNIT, rw_bottomtexturescalex);
#else
			rw_invbottomtexturescalex = FixedDiv(FRACUNIT, rw_bottomtexturescalex);
#endif

			if (rw_bottomtexturescaley < 0)
				botrowoffset = -botrowoffset;

			// Ignore slopes for lower/upper textures unless flag is checked
			if (!(linedef->flags & ML_SKEWTD))
			{
				if (linedef->flags & ML_DONTPEGBOTTOM)
					rw_bottomtexturemid = frontsector->floorheight - viewz;
				else
					rw_bottomtexturemid = backsector->floorheight - viewz;
			}
			else if (linedef->flags & ML_DONTPEGBOTTOM)
			{
				// bottom of texture at bottom
				// top of texture at top
				rw_bottomtexturemid = worldbottom;
				rw_bottomtextureslide = FixedMul(floorfrontslide, abs(rw_bottomtexturescaley));
			}
			else
			{
				// top of texture at top
				rw_bottomtexturemid = worldlow;
				rw_bottomtextureslide = FixedMul(floorbackslide, abs(rw_bottomtexturescaley));
			}

			rw_bottomtexturemid = FixedMul(rw_bottomtexturemid, abs(rw_bottomtexturescaley));
			rw_bottomtexturemid += botrowoffset;
		}

		// allocate space for masked texture tables
		R_AllocTextureColumnTables(rw_stopx - start);

		texcoltables = true;

		if (frontsector && backsector && !Tag_Compare(&frontsector->tags, &backsector->tags) && (backsector->ffloors || frontsector->ffloors))
		{
			ffloor_t *rover;
			ffloor_t *r2;
			fixed_t   lowcut, highcut;
			fixed_t lowcutslope, highcutslope;

			// Used for height comparisons and etc across FOFs and slopes
			fixed_t high1, highslope1, low1, lowslope1, high2, highslope2, low2, lowslope2;

			maskedtexture = true;

			ds_p->thicksidecol = thicksidecol = curtexturecolumntable - rw_x;
			curtexturecolumntable += rw_stopx - rw_x;

			lowcut = max(worldbottom, worldlow) + viewz;
			highcut = min(worldtop, worldhigh) + viewz;
			lowcutslope = max(worldbottomslope, worldlowslope) + viewz;
			highcutslope = min(worldtopslope, worldhighslope) + viewz;

			if (frontsector->ffloors && backsector->ffloors)
			{
				i = 0;
				for (rover = backsector->ffloors; rover && i < MAXFFLOORS; rover = rover->next)
				{
					if (!(rover->fofflags & FOF_RENDERSIDES) || !(rover->fofflags & FOF_EXISTS))
						continue;
					if (!(rover->fofflags & FOF_ALLSIDES) && rover->fofflags & FOF_INVERTSIDES)
						continue;

					if (rover->norender == leveltime)
						continue;

					SLOPEPARAMS(*rover->t_slope, high1, highslope1, *rover->topheight)
					SLOPEPARAMS(*rover->b_slope, low1,  lowslope1,  *rover->bottomheight)

					if ((high1 < lowcut && highslope1 < lowcutslope) || (low1 > highcut && lowslope1 > highcutslope))
						continue;

					for (r2 = frontsector->ffloors; r2; r2 = r2->next)
					{
						if (r2->master == rover->master) // Skip if same control line.
							break;

						if (!(r2->fofflags & FOF_EXISTS) || !(r2->fofflags & FOF_RENDERSIDES))
							continue;

						if (r2->norender == leveltime)
							continue;

						if (rover->fofflags & FOF_EXTRA)
						{
							if (!(r2->fofflags & FOF_CUTEXTRA))
								continue;

							if (r2->fofflags & FOF_EXTRA && (r2->fofflags & (FOF_TRANSLUCENT|FOF_FOG)) != (rover->fofflags & (FOF_TRANSLUCENT|FOF_FOG)))
								continue;
						}
						else
						{
							if (!(r2->fofflags & FOF_CUTSOLIDS))
								continue;
						}

						SLOPEPARAMS(*r2->t_slope, high2, highslope2, *r2->topheight)
						SLOPEPARAMS(*r2->b_slope, low2,  lowslope2,  *r2->bottomheight)

						if ((high2 < lowcut || highslope2 < lowcutslope) || (low2 > highcut || lowslope2 > highcutslope))
							continue;
						if ((high1 > high2 || highslope1 > highslope2) || (low1 < low2 || lowslope1 < lowslope2))
							continue;

						break;
					}
					if (r2)
						continue;

					ds_p->thicksides[i] = rover;
					i++;
				}

				for (rover = frontsector->ffloors; rover && i < MAXFFLOORS; rover = rover->next)
				{
					if (!(rover->fofflags & FOF_RENDERSIDES) || !(rover->fofflags & FOF_EXISTS))
						continue;
					if (!(rover->fofflags & FOF_ALLSIDES || rover->fofflags & FOF_INVERTSIDES))
						continue;

					if (rover->norender == leveltime)
						continue;

					SLOPEPARAMS(*rover->t_slope, high1, highslope1, *rover->topheight)
					SLOPEPARAMS(*rover->b_slope, low1,  lowslope1,  *rover->bottomheight)

					if ((high1 < lowcut && highslope1 < lowcutslope) || (low1 > highcut && lowslope1 > highcutslope))
						continue;

					for (r2 = backsector->ffloors; r2; r2 = r2->next)
					{
						if (r2->master == rover->master) // Skip if same control line.
							break;

						if (!(r2->fofflags & FOF_EXISTS) || !(r2->fofflags & FOF_RENDERSIDES))
							continue;

						if (r2->norender == leveltime)
							continue;

						if (rover->fofflags & FOF_EXTRA)
						{
							if (!(r2->fofflags & FOF_CUTEXTRA))
								continue;

							if (r2->fofflags & FOF_EXTRA && (r2->fofflags & (FOF_TRANSLUCENT|FOF_FOG)) != (rover->fofflags & (FOF_TRANSLUCENT|FOF_FOG)))
								continue;
						}
						else
						{
							if (!(r2->fofflags & FOF_CUTSOLIDS))
								continue;
						}

						SLOPEPARAMS(*r2->t_slope, high2, highslope2, *r2->topheight)
						SLOPEPARAMS(*r2->b_slope, low2,  lowslope2,  *r2->bottomheight)
#undef SLOPEPARAMS
						if ((high2 < lowcut || highslope2 < lowcutslope) || (low2 > highcut || lowslope2 > highcutslope))
							continue;
						if ((high1 > high2 || highslope1 > highslope2) || (low1 < low2 || lowslope1 < lowslope2))
							continue;

						break;
					}
					if (r2)
						continue;

					ds_p->thicksides[i] = rover;
					i++;
				}
			}
			else if (backsector->ffloors)
			{
				for (rover = backsector->ffloors, i = 0; rover && i < MAXFFLOORS; rover = rover->next)
				{
					if (!(rover->fofflags & FOF_RENDERSIDES) || !(rover->fofflags & FOF_EXISTS))
						continue;
					if (!(rover->fofflags & FOF_ALLSIDES) && rover->fofflags & FOF_INVERTSIDES)
						continue;
					if (rover->norender == leveltime)
						continue;

					// Oy vey.
					if (      ((P_GetFFloorTopZAt   (rover, segleft .x, segleft .y)) <= worldbottom      + viewz
					        && (P_GetFFloorTopZAt   (rover, segright.x, segright.y)) <= worldbottomslope + viewz)
					        ||((P_GetFFloorBottomZAt(rover, segleft .x, segleft .y)) >= worldtop         + viewz
					        && (P_GetFFloorBottomZAt(rover, segright.x, segright.y)) >= worldtopslope    + viewz))
						continue;

					ds_p->thicksides[i] = rover;
					i++;
				}
			}
			else if (frontsector->ffloors)
			{
				for (rover = frontsector->ffloors, i = 0; rover && i < MAXFFLOORS; rover = rover->next)
				{
					if (!(rover->fofflags & FOF_RENDERSIDES) || !(rover->fofflags & FOF_EXISTS))
						continue;
					if (!(rover->fofflags & FOF_ALLSIDES || rover->fofflags & FOF_INVERTSIDES))
						continue;
					if (rover->norender == leveltime)
						continue;
					// Oy vey.
					if (      (P_GetFFloorTopZAt   (rover, segleft .x, segleft .y) <= worldbottom      + viewz
					        && P_GetFFloorTopZAt   (rover, segright.x, segright.y) <= worldbottomslope + viewz)
					        ||(P_GetFFloorBottomZAt(rover, segleft .x, segleft .y) >= worldtop         + viewz
					        && P_GetFFloorBottomZAt(rover, segright.x, segright.y) >= worldtopslope    + viewz))
						continue;

					if (      (P_GetFFloorTopZAt   (rover, segleft .x, segleft .y) <= worldlow       + viewz
					        && P_GetFFloorTopZAt   (rover, segright.x, segright.y) <= worldlowslope  + viewz)
					        ||(P_GetFFloorBottomZAt(rover, segleft .x, segleft .y) >= worldhigh      + viewz
					        && P_GetFFloorBottomZAt(rover, segright.x, segright.y) >= worldhighslope + viewz))
						continue;

					ds_p->thicksides[i] = rover;
					i++;
				}
			}

			ds_p->numthicksides = numthicksides = i;
		}

		// masked midtexture
		if (sidedef->midtexture > 0 && sidedef->midtexture < numtextures)
		{
			ds_p->maskedtexturecol = maskedtexturecol = curtexturecolumntable - rw_x;
			curtexturecolumntable += rw_stopx - rw_x;

			ds_p->maskedtextureheight = maskedtextureheight = curtexturecolumntable - rw_x;
			curtexturecolumntable += rw_stopx - rw_x;

			maskedtexture = true;

			if (curline->polyseg)
			{
				// use REAL front and back floors please, so midtexture rendering isn't mucked up
				rw_midtextureslide = rw_midtexturebackslide = 0;
				if (linedef->flags & ML_MIDPEG)
					rw_midtexturemid = rw_midtextureback = max(curline->frontsector->floorheight, curline->backsector->floorheight) - viewz;
				else
					rw_midtexturemid = rw_midtextureback = min(curline->frontsector->ceilingheight, curline->backsector->ceilingheight) - viewz;
			}
			else
			{
				// Set midtexture starting height
				if (linedef->flags & ML_NOSKEW)
				{
					// Ignore slopes when texturing
					rw_midtextureslide = rw_midtexturebackslide = 0;
					if (linedef->flags & ML_MIDPEG)
						rw_midtexturemid = rw_midtextureback = max(frontsector->floorheight, backsector->floorheight) - viewz;
					else
						rw_midtexturemid = rw_midtextureback = min(frontsector->ceilingheight, backsector->ceilingheight) - viewz;

				}
				else if (linedef->flags & ML_MIDPEG)
				{
					rw_midtexturemid = worldbottom;
					rw_midtextureslide = floorfrontslide;
					rw_midtextureback = worldlow;
					rw_midtexturebackslide = floorbackslide;
				}
				else
				{
					rw_midtexturemid = worldtop;
					rw_midtextureslide = ceilingfrontslide;
					rw_midtextureback = worldhigh;
					rw_midtexturebackslide = ceilingbackslide;
				}
			}

			rw_midtexturemid = FixedMul(rw_midtexturemid, abs(rw_midtexturescaley));
			rw_midtextureback = FixedMul(rw_midtextureback, abs(rw_midtexturescaley));

			rw_midtextureslide = FixedMul(rw_midtextureslide, abs(rw_midtexturescaley));
			rw_midtexturebackslide = FixedMul(rw_midtexturebackslide, abs(rw_midtexturescaley));

			rw_midtexturemid += sidedef->rowoffset + SIDE_OFFSETY_MID(sidedef);
			rw_midtextureback += sidedef->rowoffset + SIDE_OFFSETY_MID(sidedef);
		}
	}

	PS2SUB_E(6);
	PS2SUB_B(9);
	// calculate rw_offset (only needed for textured lines)
	segtextured = midtexture || toptexture || bottomtexture || maskedtexture || (numthicksides > 0);

	if (segtextured)
	{
		fixed_t sideoffset = sidedef->textureoffset;

		offsetangle = rw_normalangle-rw_angle1;

		if (offsetangle > ANGLE_180)
			offsetangle = -(signed)offsetangle;

		if (offsetangle > ANGLE_90)
			offsetangle = ANGLE_90;

		sineval = FINESINE(offsetangle>>ANGLETOFINESHIFT);
		rw_offset = FixedMul(hyp, sineval);

		// big room fix
		if (longboi)
		{
			INT64 dx = (curline->v2->x)-(curline->v1->x);
			INT64 dy = (curline->v2->y)-(curline->v1->y);
			INT64 vdx = viewx-(curline->v1->x);
			INT64 vdy = viewy-(curline->v1->y);
			rw_offset = ((dx*vdx-dy*vdy))/(curline->length);
		}

		if (rw_normalangle-rw_angle1 < ANGLE_180)
			rw_offset = -rw_offset;

		rw_offset += curline->offset;
		rw_centerangle = ANGLE_90 + viewangle - rw_normalangle;

		rw_offset_top = sideoffset + SIDE_OFFSETX_TOP(sidedef);
		rw_offset_mid = sideoffset + SIDE_OFFSETX_MID(sidedef);
		rw_offset_bottom = sideoffset + SIDE_OFFSETX_BOTTOM(sidedef);

		rw_offsetx = rw_offset_mid;
		if (rw_midtexturescalex < 0)
			rw_offsetx = -rw_offsetx;

		if (numthicksides)
			ds_p->offsetx = rw_offsetx;

		// calculate light table
		//  use different light tables
		//  for horizontal / vertical / diagonal
		// OPTIMIZE: get rid of LIGHTSEGSHIFT globally
		lightnum = R_SideLightLevel(curline->sidedef, frontsector->lightlevel) >> LIGHTSEGSHIFT;

		if (curline->v1->y == curline->v2->y)
			lightnum--;
		else if (curline->v1->x == curline->v2->x)
			lightnum++;

		if (lightnum < 0)
			walllights = scalelight[0];
		else if (lightnum >= LIGHTLEVELS)
			walllights = scalelight[LIGHTLEVELS - 1];
		else
			walllights = scalelight[lightnum];
	}

	if (maskedtexture)
	{
		ds_p->invscale = invscale = curtexturecolumntable - rw_x;
		curtexturecolumntable += rw_stopx - rw_x;
	}

	PS2SUB_E(9);
	PS2SUB_B(10);
	// if a floor / ceiling plane is on the wrong side
	//  of the view plane, it is definitely invisible
	//  and doesn't need to be marked.
	if (frontsector->heightsec == -1)
	{
		if (frontsector->floorpic != skyflatnum && P_GetSectorFloorZAt(frontsector, viewx, viewy) >= viewz)
		{
			// above view plane
			markfloor = false;
		}

		if (frontsector->ceilingpic != skyflatnum && P_GetSectorCeilingZAt(frontsector, viewx, viewy) <= viewz)
		{
			// below view plane
			markceiling = false;
		}
	}

	// calculate incremental stepping values for texture edges
	worldtop >>= 4;
	worldbottom >>= 4;
	worldtopslope >>= 4;
	worldbottomslope >>= 4;

	if (horizonline) { // HORIZON LINES
		topstep = bottomstep = 0;
		topfrac = bottomfrac = (centeryfrac>>4);
		topfrac++; // Prevent 1px HOM
	} else {
		topstep = -FixedMul (rw_scalestep, worldtop);
		topfrac = (centeryfrac>>4) - FixedMul (worldtop, rw_scale);

		bottomstep = -FixedMul (rw_scalestep,worldbottom);
		bottomfrac = (centeryfrac>>4) - FixedMul (worldbottom, rw_scale);

		if (frontsector->c_slope) {
			fixed_t topfracend = (centeryfrac>>4) - FixedMul (worldtopslope, ds_p->scale2);
			topstep = (topfracend-topfrac)/(range);
		}
		if (frontsector->f_slope) {
			fixed_t bottomfracend = (centeryfrac>>4) - FixedMul (worldbottomslope, ds_p->scale2);
			bottomstep = (bottomfracend-bottomfrac)/(range);
		}
	}

	dc_numlights = 0;

	if (frontsector->numlights)
	{
		dc_numlights = frontsector->numlights;
		if (dc_numlights > dc_maxlights)
		{
			dc_maxlights = dc_numlights;
			dc_lightlist = Z_Realloc(dc_lightlist, sizeof (*dc_lightlist) * dc_maxlights, PU_STATIC, NULL);
		}

		for (i = p = 0; i < dc_numlights; i++)
		{
			fixed_t leftheight, rightheight;

			light = &frontsector->lightlist[i];
			rlight = &dc_lightlist[p];

			leftheight  = P_GetLightZAt(light,  segleft.x,  segleft.y);
			rightheight = P_GetLightZAt(light, segright.x, segright.y);

			if (light->slope)
				// Flag sector as having slopes
				frontsector->hasslope = true;

			leftheight -= viewz;
			rightheight -= viewz;

			leftheight >>= 4;
			rightheight >>= 4;

			if (i != 0)
			{
				if (leftheight < worldbottom && rightheight < worldbottomslope)
					continue;

				if (leftheight > worldtop && rightheight > worldtopslope && i+1 < dc_numlights && frontsector->lightlist[i+1].height > frontsector->ceilingheight)
					continue;
			}

			rlight->height = (centeryfrac>>4) - FixedMul(leftheight, rw_scale);
			rlight->heightstep = (centeryfrac>>4) - FixedMul(rightheight, ds_p->scale2);
			rlight->heightstep = (rlight->heightstep-rlight->height)/(range);
			rlight->flags = light->flags;

			if (light->caster && light->caster->fofflags & FOF_CUTSOLIDS)
			{
				leftheight  = P_GetFFloorBottomZAt(light->caster,  segleft.x,  segleft.y);
				rightheight = P_GetFFloorBottomZAt(light->caster, segright.x, segright.y);

				if (*light->caster->b_slope)
					// Flag sector as having slopes
					frontsector->hasslope = true;

				leftheight  -= viewz;
				rightheight -= viewz;

				leftheight  >>= 4;
				rightheight >>= 4;

				rlight->botheight = (centeryfrac>>4) - FixedMul(leftheight, rw_scale);
				rlight->botheightstep = (centeryfrac>>4) - FixedMul(rightheight, ds_p->scale2);
				rlight->botheightstep = (rlight->botheightstep-rlight->botheight)/(range);

			}

			rlight->lightlevel = *light->lightlevel;
			rlight->extra_colormap = *light->extra_colormap;
			p++;
		}

		dc_numlights = p;
	}

	if (numffloors)
	{
		for (i = 0; i < numffloors; i++)
		{
			ffloor[i].f_pos >>= 4;
			ffloor[i].f_pos_slope >>= 4;
			if (horizonline) // Horizon lines extend FOFs in contact with them too.
			{
				ffloor[i].f_step = 0;
				ffloor[i].f_frac = (centeryfrac>>4);
				topfrac++; // Prevent 1px HOM
			}
			else
			{
				ffloor[i].f_frac = (centeryfrac>>4) - FixedMul(ffloor[i].f_pos, rw_scale);
				ffloor[i].f_step = ((centeryfrac>>4) - FixedMul(ffloor[i].f_pos_slope, ds_p->scale2) - ffloor[i].f_frac)/(range);
			}
		}
	}

	PS2SUB_E(10);
	PS2SUB_B(11);
	if (backsector)
	{
		worldhigh >>= 4;
		worldlow >>= 4;
		worldhighslope >>= 4;
		worldlowslope >>= 4;

		if (toptexture)
		{
			pixhigh = (centeryfrac>>4) - FixedMul (worldhigh, rw_scale);
			pixhighstep = -FixedMul (rw_scalestep,worldhigh);

			if (backsector->c_slope) {
				fixed_t topfracend = (centeryfrac>>4) - FixedMul (worldhighslope, ds_p->scale2);
				pixhighstep = (topfracend-pixhigh)/(range);
			}
		}

		if (bottomtexture)
		{
			pixlow = (centeryfrac>>4) - FixedMul (worldlow, rw_scale);
			pixlowstep = -FixedMul (rw_scalestep,worldlow);
			if (backsector->f_slope) {
				fixed_t bottomfracend = (centeryfrac>>4) - FixedMul (worldlowslope, ds_p->scale2);
				pixlowstep = (bottomfracend-pixlow)/(range);
			}
		}

		{
			ffloor_t * rover;
			fixed_t roverleft, roverright;
			fixed_t planevistest;
			i = 0;

			if (backsector->ffloors)
			{
				for (rover = backsector->ffloors; rover && i < MAXFFLOORS; rover = rover->next)
				{
					if (!(rover->fofflags & FOF_EXISTS) || !(rover->fofflags & FOF_RENDERPLANES))
						continue;
					if (rover->norender == leveltime)
						continue;

					// Let the renderer know this sector is sloped.
					if (*rover->b_slope || *rover->t_slope)
						backsector->hasslope = true;

					roverleft    = P_GetFFloorBottomZAt(rover, segleft .x, segleft .y) - viewz;
					roverright   = P_GetFFloorBottomZAt(rover, segright.x, segright.y) - viewz;
					planevistest = P_GetFFloorBottomZAt(rover, viewx, viewy);

					if ((roverleft>>4 <= worldhigh || roverright>>4 <= worldhighslope) &&
					    (roverleft>>4 >= worldlow || roverright>>4 >= worldlowslope) &&
					    ((viewz < planevistest && (rover->fofflags & FOF_BOTHPLANES || !(rover->fofflags & FOF_INVERTPLANES))) ||
					     (viewz > planevistest && (rover->fofflags & FOF_BOTHPLANES || rover->fofflags & FOF_INVERTPLANES))))
					{
						//ffloor[i].slope = *rover->b_slope;
						ffloor[i].b_pos = roverleft;
						ffloor[i].b_pos_slope = roverright;
						ffloor[i].b_pos >>= 4;
						ffloor[i].b_pos_slope >>= 4;
						ffloor[i].b_frac = (centeryfrac >> 4) - FixedMul(ffloor[i].b_pos, rw_scale);
						ffloor[i].b_step = (centeryfrac >> 4) - FixedMul(ffloor[i].b_pos_slope, ds_p->scale2);
						ffloor[i].b_step = (ffloor[i].b_step-ffloor[i].b_frac)/(range);
						i++;
					}

					if (i >= MAXFFLOORS)
						break;

					roverleft    = P_GetFFloorTopZAt(rover, segleft .x, segleft .y) - viewz;
					roverright   = P_GetFFloorTopZAt(rover, segright.x, segright.y) - viewz;
					planevistest = P_GetFFloorTopZAt(rover, viewx, viewy);

					if ((roverleft>>4 <= worldhigh || roverright>>4 <= worldhighslope) &&
					    (roverleft>>4 >= worldlow || roverright>>4 >= worldlowslope) &&
					    ((viewz > planevistest && (rover->fofflags & FOF_BOTHPLANES || !(rover->fofflags & FOF_INVERTPLANES))) ||
					     (viewz < planevistest && (rover->fofflags & FOF_BOTHPLANES || rover->fofflags & FOF_INVERTPLANES))))
					{
						//ffloor[i].slope = *rover->t_slope;
						ffloor[i].b_pos = roverleft;
						ffloor[i].b_pos_slope = roverright;
						ffloor[i].b_pos >>= 4;
						ffloor[i].b_pos_slope >>= 4;
						ffloor[i].b_frac = (centeryfrac >> 4) - FixedMul(ffloor[i].b_pos, rw_scale);
						ffloor[i].b_step = (centeryfrac >> 4) - FixedMul(ffloor[i].b_pos_slope, ds_p->scale2);
						ffloor[i].b_step = (ffloor[i].b_step-ffloor[i].b_frac)/(range);
						i++;
					}
				}
			}
			else if (frontsector && frontsector->ffloors)
			{
				for (rover = frontsector->ffloors; rover && i < MAXFFLOORS; rover = rover->next)
				{
					if (!(rover->fofflags & FOF_EXISTS) || !(rover->fofflags & FOF_RENDERPLANES))
						continue;
					if (rover->norender == leveltime)
						continue;

					// Let the renderer know this sector is sloped.
					if (*rover->b_slope || *rover->t_slope)
						frontsector->hasslope = true;

					roverleft  = P_GetFFloorBottomZAt(rover, segleft .x, segleft .y) - viewz;
					roverright = P_GetFFloorBottomZAt(rover, segright.x, segright.y) - viewz;
					planevistest = P_GetFFloorBottomZAt(rover, viewx, viewy);

					if ((roverleft>>4 <= worldhigh || roverright>>4 <= worldhighslope) &&
					    (roverleft>>4 >= worldlow || roverright>>4 >= worldlowslope) &&
					    ((viewz < planevistest && (rover->fofflags & FOF_BOTHPLANES || !(rover->fofflags & FOF_INVERTPLANES))) ||
					     (viewz > planevistest && (rover->fofflags & FOF_BOTHPLANES || rover->fofflags & FOF_INVERTPLANES))))
					{
						//ffloor[i].slope = *rover->b_slope;
						ffloor[i].b_pos = roverleft;
						ffloor[i].b_pos_slope = roverright;
						ffloor[i].b_pos >>= 4;
						ffloor[i].b_pos_slope >>= 4;
						ffloor[i].b_frac = (centeryfrac >> 4) - FixedMul(ffloor[i].b_pos, rw_scale);
						ffloor[i].b_step = (centeryfrac >> 4) - FixedMul(ffloor[i].b_pos_slope, ds_p->scale2);
						ffloor[i].b_step = (ffloor[i].b_step-ffloor[i].b_frac)/(range);
						i++;
					}

					if (i >= MAXFFLOORS)
						break;

					roverleft  = P_GetFFloorTopZAt(rover, segleft .x, segleft .y) - viewz;
					roverright = P_GetFFloorTopZAt(rover, segright.x, segright.y) - viewz;
					planevistest = P_GetFFloorTopZAt(rover, viewx, viewy);

					if ((roverleft>>4 <= worldhigh || roverright>>4 <= worldhighslope) &&
					    (roverleft>>4 >= worldlow || roverright>>4 >= worldlowslope) &&
					    ((viewz > planevistest && (rover->fofflags & FOF_BOTHPLANES || !(rover->fofflags & FOF_INVERTPLANES))) ||
					     (viewz < planevistest && (rover->fofflags & FOF_BOTHPLANES || rover->fofflags & FOF_INVERTPLANES))))
					{
						//ffloor[i].slope = *rover->t_slope;
						ffloor[i].b_pos = roverleft;
						ffloor[i].b_pos_slope = roverright;
						ffloor[i].b_pos >>= 4;
						ffloor[i].b_pos_slope >>= 4;
						ffloor[i].b_frac = (centeryfrac >> 4) - FixedMul(ffloor[i].b_pos, rw_scale);
						ffloor[i].b_step = (centeryfrac >> 4) - FixedMul(ffloor[i].b_pos_slope, ds_p->scale2);
						ffloor[i].b_step = (ffloor[i].b_step-ffloor[i].b_frac)/(range);
						i++;
					}
				}
			}
			if (curline->polyseg && frontsector && (curline->polyseg->flags & POF_RENDERPLANES))
			{
				while (i < numffloors && ffloor[i].polyobj != curline->polyseg) i++;
				if (i < numffloors && backsector->floorheight <= frontsector->ceilingheight &&
					backsector->floorheight >= frontsector->floorheight &&
					(viewz < backsector->floorheight))
				{
					if (ffloor[i].plane->minx > ds_p->x1)
						ffloor[i].plane->minx = ds_p->x1;

					if (ffloor[i].plane->maxx < ds_p->x2)
						ffloor[i].plane->maxx = ds_p->x2;

					ffloor[i].slope = NULL;
					ffloor[i].b_pos = backsector->floorheight;
					ffloor[i].b_pos = (ffloor[i].b_pos - viewz) >> 4;
					ffloor[i].b_step = FixedMul(-rw_scalestep, ffloor[i].b_pos);
					ffloor[i].b_frac = (centeryfrac >> 4) - FixedMul(ffloor[i].b_pos, rw_scale);
					i++;
				}
				if (i < numffloors && backsector->ceilingheight >= frontsector->floorheight &&
					backsector->ceilingheight <= frontsector->ceilingheight &&
					(viewz > backsector->ceilingheight))
				{
					if (ffloor[i].plane->minx > ds_p->x1)
						ffloor[i].plane->minx = ds_p->x1;

					if (ffloor[i].plane->maxx < ds_p->x2)
						ffloor[i].plane->maxx = ds_p->x2;

					ffloor[i].slope = NULL;
					ffloor[i].b_pos = backsector->ceilingheight;
					ffloor[i].b_pos = (ffloor[i].b_pos - viewz) >> 4;
					ffloor[i].b_step = FixedMul(-rw_scalestep, ffloor[i].b_pos);
					ffloor[i].b_frac = (centeryfrac >> 4) - FixedMul(ffloor[i].b_pos, rw_scale);
					i++;
				}
			}

			numbackffloors = i;
#ifdef PS2_OPT_REND
			if (i > ffloor_clipdirty)
				ffloor_clipdirty = i; // R_RenderSegLoop writes ffloor[0..i-1].f_clip/c_clip
#endif
		}
	}

	PS2SUB_E(11);
	PS2SUB_B(12);
	// get a new or use the same visplane
	if (markceiling)
	{
		if (ceilingplane) //SoM: 3/29/2000: Check for null ceiling planes
			ceilingplane = R_CheckPlane (ceilingplane, rw_x, rw_stopx-1);
		else
			markceiling = false;

		// Don't mark ceiling flat lines for polys unless this line has an upper texture, otherwise we get flat leakage pulling downward
		// (If it DOES have an upper texture and we do this, the ceiling won't render at all)
		if (curline->polyseg && !curline->sidedef->toptexture)
			markceiling = false;
	}

	// get a new or use the same visplane
	if (markfloor)
	{
		if (floorplane) //SoM: 3/29/2000: Check for null planes
			floorplane = R_CheckPlane (floorplane, rw_x, rw_stopx-1);
		else
			markfloor = false;

		// Don't mark floor flat lines for polys unless this line has a lower texture, otherwise we get flat leakage pulling upward
		// (If it DOES have a lower texture and we do this, the floor won't render at all)
		if (curline->polyseg && !curline->sidedef->bottomtexture)
			markfloor = false;
	}

	ds_p->numffloorplanes = 0;
	if (numffloors)
	{
		if (!firstseg)
		{
#ifdef PS2_PROFILE
			R_AllocDrawSegFrontScale(ds_p);
#endif
			ds_p->numffloorplanes = numffloors;

			for (i = 0; i < numffloors; i++)
			{
				ds_p->ffloorplanes[i] = ffloor[i].plane =
					R_CheckPlane(ffloor[i].plane, rw_x, rw_stopx - 1);
			}

			firstseg = ds_p;
		}
		else
		{
			for (i = 0; i < numffloors; i++)
				R_ExpandPlane(ffloor[i].plane, rw_x, rw_stopx - 1);
		}
		// FIXME hack to fix planes disappearing when a seg goes behind the camera. This NEEDS to be changed to be done properly. -Red
		if (curline->polyseg)
		{
			for (i = 0; i < numffloors; i++)
			{
				if (!ffloor[i].polyobj || ffloor[i].polyobj != curline->polyseg)
					continue;
				if (ffloor[i].plane->minx > rw_x)
					ffloor[i].plane->minx = rw_x;

				if (ffloor[i].plane->maxx < rw_stopx - 1)
					ffloor[i].plane->maxx = rw_stopx - 1;
			}
		}
	}

	PS2SUB_E(12);
	rw_silhouette = &(ds_p->silhouette);
	rw_tsilheight = &(ds_p->tsilheight);
	rw_bsilheight = &(ds_p->bsilheight);

	PS2SUB_B(13);
	R_RenderSegLoop();
	PS2SUB_E(13);
	PS2SUB_B(14);
#ifdef PS2
	// Opaque wall sources are consumed; later masked passes fetch their own columns.
	if (midtexture > 0)
		R_ReleaseTextureCache(midtexture);
	if (toptexture > 0)
		R_ReleaseTextureCache(toptexture);
	if (bottomtexture > 0)
		R_ReleaseTextureCache(bottomtexture);
#endif
	colfunc = colfuncs[BASEDRAWFUNC];

	if (portalline) // if curline is a portal, set portalrender for drawseg
		ds_p->portalpass = portalrender+1;
	else
		ds_p->portalpass = 0;

	// save sprite clipping info
	if (maskedtexture || (ds_p->silhouette & (SIL_TOP | SIL_BOTTOM)))
	{
		R_AllocClippingTables(rw_stopx - start);

		if (((ds_p->silhouette & SIL_TOP) || maskedtexture) && !ds_p->sprtopclip)
		{
			M_Memcpy(lastopening, ceilingclip + start, 2*(rw_stopx - start));
			ds_p->sprtopclip = lastopening - start;
			lastopening += rw_stopx - start;
		}

		if (((ds_p->silhouette & SIL_BOTTOM) || maskedtexture) && !ds_p->sprbottomclip)
		{
			M_Memcpy(lastopening, floorclip + start, 2*(rw_stopx - start));
			ds_p->sprbottomclip = lastopening - start;
			lastopening += rw_stopx - start;
		}
	}

	if (maskedtexture && !(ds_p->silhouette & SIL_TOP))
	{
		ds_p->silhouette |= SIL_TOP;
		ds_p->tsilheight = (sidedef->midtexture > 0 && sidedef->midtexture < numtextures) ? INT32_MIN : INT32_MAX;
	}
	if (maskedtexture && !(ds_p->silhouette & SIL_BOTTOM))
	{
		ds_p->silhouette |= SIL_BOTTOM;
		ds_p->bsilheight = (sidedef->midtexture > 0 && sidedef->midtexture < numtextures) ? INT32_MAX : INT32_MIN;
	}
	PS2SUB_E(14);
	PS2SUB_E(0);
	ds_p++;
}
