// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2024 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  r_draw8_npo2.c
/// \brief 8bpp span drawer functions (for non-powers-of-two flat dimensions)
/// \note  no includes because this is included as part of r_draw.c

// ==========================================================================
// SPANS
// ==========================================================================

#define SPANSIZE 16
#define INVSPAN 0.0625f

#if defined(__GNUC__) || defined(__clang__) // Suppress intentional libdivide compiler warnings - Also added to libdivide.h
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Waggregate-return"
#endif

#ifdef PS2_OPT_DRAW
// A plane's spans share their flat dimensions. Keep two bounded reciprocal
// descriptors per axis across all tilted drawers, including alternating
// floor/sprite dimensions. Each miss needs only a 32-bit divide; copy the
// reciprocal into span-local storage before pixel stores.
// No source/cache pointer is retained, so flat eviction and mode/level changes
// need no invalidation. Zero dimensions still take libdivide's error path.
static void R_NPO2SlopeReciprocals(UINT32 *x, UINT32 *y)
{
	// Keep each reciprocal word aligned, with the same two-entry cache sizes.
	static struct
	{
		UINT32 reciprocal;
		UINT16 dimension;
	} xcached[2], ycached[2];
	static UINT8 xnext, ynext;
	const UINT16 flatwidth = ds_flatwidth, flatheight = ds_flatheight;
	UINT32 xi = 0, yi = 0;

	if (!flatwidth || xcached[0].dimension != flatwidth)
	{
		xi = 1;
		if (!flatwidth || xcached[1].dimension != flatwidth)
		{
			if (!flatwidth)
				(void)libdivide_u32_gen(flatwidth);
			const UINT32 reciprocal = 0x7fffffffU / flatwidth + 1U;
			xi = xnext;
			xcached[xi].reciprocal = reciprocal;
			xcached[xi].dimension = flatwidth;
			xnext ^= 1;
		}
	}
	if (!flatheight || ycached[0].dimension != flatheight)
	{
		yi = 1;
		if (!flatheight || ycached[1].dimension != flatheight)
		{
			if (!flatheight)
				(void)libdivide_u32_gen(flatheight);
			const UINT32 reciprocal = 0x7fffffffU / flatheight + 1U;
			yi = ynext;
			ycached[yi].reciprocal = reciprocal;
			ycached[yi].dimension = flatheight;
			ynext ^= 1;
		}
	}
	*x = xcached[xi].reciprocal;
	*y = ycached[yi].reciprocal;
}

// Perspective texture coordinates become signed 16-bit integers before wrapping.
// For 0 <= n <= 32767 and 1 <= d <= 65535, ceil(2^31 / d) gives the exact
// quotient as high32((n << 1) * reciprocal): the reciprocal's excess is too
// small to cross the next multiple of d. Complementing negative coordinates
// preserves the original Euclidean wrap, including exact negative multiples.
static inline UINT32 R_NPO2SlopeWrap(fixed_t coordinate, UINT16 dimension, UINT32 reciprocal)
{
	const UINT32 sign = (UINT32)(coordinate >> 31);
	const UINT32 magnitude = (UINT32)coordinate ^ sign;
	const UINT32 quotient = libdivide_mullhi_u32(magnitude << 1, reciprocal);
	return (UINT32)coordinate - (quotient ^ sign) * dimension;
}
#endif

/**	\brief The R_DrawSpan_NPO2_8 function
	Draws the actual span.
*/
void R_DrawSpan_NPO2_8 (void)
{
	fixed_t xposition;
	fixed_t yposition;
	fixed_t xstep, ystep;
	fixed_t x, y;
	fixed_t fixedwidth, fixedheight;

	UINT8 *source;
	UINT8 *colormap;
	UINT8 *dest;
	const UINT8 *deststop = screens[0] + vid.rowbytes * vid.height;

	size_t count = (ds_x2 - ds_x1 + 1);

	xposition = ds_xfrac; yposition = ds_yfrac;
	xstep = ds_xstep; ystep = ds_ystep;

	source = ds_source;
	colormap = ds_colormap;
	dest = &topleft[ds_y*vid.width + ds_x1];

	if (dest+8 > deststop)
		return;

	fixedwidth = ds_flatwidth << FRACBITS;
	fixedheight = ds_flatheight << FRACBITS;

	// Fix xposition and yposition if they are out of bounds.
	if (xposition < 0)
		xposition = fixedwidth - ((UINT32)(fixedwidth - xposition) % fixedwidth);
	else if (xposition >= fixedwidth)
		xposition %= fixedwidth;
	if (yposition < 0)
		yposition = fixedheight - ((UINT32)(fixedheight - yposition) % fixedheight);
	else if (yposition >= fixedheight)
		yposition %= fixedheight;

#ifdef PS2_OPT_DRAW
	// With bounded steps, unsigned range tests need one correction per axis.
	// Keep the original loop for overflow-sized flats/steps and noncanonical starts.
	if (count >= 32 && fixedwidth > 0 && fixedwidth <= 0x40000000
		&& fixedheight > 0 && fixedheight <= 0x40000000
		&& (UINT32)xposition < (UINT32)fixedwidth
		&& (UINT32)yposition < (UINT32)fixedheight
		&& xstep > -fixedwidth && xstep < fixedwidth
		&& ystep > -fixedheight && ystep < fixedheight)
	{
		const INT32 flatwidth = ds_flatwidth;
		const fixed_t xwrap = xstep < 0 ? fixedwidth : -fixedwidth;
		const fixed_t ywrap = ystep < 0 ? fixedheight : -fixedheight;
		// Preserve the original inclusive destination bound once per span.
		if (count > (size_t)(deststop - dest) + 1)
			count = (size_t)(deststop - dest) + 1;
		while (count--)
		{
			if ((UINT32)xposition >= (UINT32)fixedwidth)
				xposition += xwrap;
			if ((UINT32)yposition >= (UINT32)fixedheight)
				yposition += ywrap;
			x = (xposition >> FRACBITS);
			y = (yposition >> FRACBITS);

			*dest++ = colormap[source[((y * flatwidth) + x)]];
			xposition += xstep;
			yposition += ystep;
		}
		return;
	}
#endif

	while (count-- && dest <= deststop)
	{
		// The loops here keep the texture coordinates within the texture.
		// They will rarely iterate multiple times, and are cheaper than a modulo operation,
		// even if using libdivide.
		if (xstep < 0) // These if statements are hopefully hoisted by the compiler to above this loop
			while (xposition < 0)
				xposition += fixedwidth;
		else
			while (xposition >= fixedwidth)
				xposition -= fixedwidth;
		if (ystep < 0)
			while (yposition < 0)
				yposition += fixedheight;
		else
			while (yposition >= fixedheight)
				yposition -= fixedheight;

		x = (xposition >> FRACBITS);
		y = (yposition >> FRACBITS);

		*dest++ = colormap[source[((y * ds_flatwidth) + x)]];
		xposition += xstep;
		yposition += ystep;
	}
}

/**	\brief The R_DrawTiltedSpan_NPO2_8 function
	Draw slopes! Holy sheit!
*/
// Retain per-span parameters without changing the live source/lookup reads.
// Framebuffer storage is separate from the renderer globals themselves.
#ifdef PS2_OPT_DRAW
#define NPO2_TILT_WIDTH npo2_width
#define NPO2_TILT_HEIGHT npo2_height
#define NPO2_TILT_LIGHT npo2_pzl[tiltlighting[npo2_x++]] + npo2_cmofs
#define NPO2_TILT_TRANSMAP npo2_transmap
#else
#define NPO2_TILT_WIDTH ds_flatwidth
#define NPO2_TILT_HEIGHT ds_flatheight
#define NPO2_TILT_LIGHT planezlight[tiltlighting[ds_x1++]] + (ds_colormap - colormaps)
#define NPO2_TILT_TRANSMAP ds_transmap
#endif

void R_DrawTiltedSpan_NPO2_8(void)
{
	// x1, x2 = ds_x1, ds_x2
	int width = ds_x2 - ds_x1;
	slopereal_t iz, uz, vz;
	UINT32 u, v;
	int i;

	UINT8 *source;
	UINT8 *colormap;
	UINT8 *dest;

	slopereal_t startz, startu, startv;
	slopereal_t izstep, uzstep, vzstep;
	slopereal_t endz, endu, endv;
	UINT32 stepu, stepv;

#ifdef PS2_OPT_DRAW
	UINT32 x_reciprocal, y_reciprocal;
	R_NPO2SlopeReciprocals(&x_reciprocal, &y_reciprocal);
#else
	struct libdivide_u32_t x_divider = libdivide_u32_gen(ds_flatwidth);
	struct libdivide_u32_t y_divider = libdivide_u32_gen(ds_flatheight);
#endif

	iz = ds_sz.z + ds_sz.y*(centery-ds_y) + ds_sz.x*(ds_x1-centerx);
	uz = ds_su.z + ds_su.y*(centery-ds_y) + ds_su.x*(ds_x1-centerx);
	vz = ds_sv.z + ds_sv.y*(centery-ds_y) + ds_sv.x*(ds_x1-centerx);

	R_CalcSlopeLight();

	dest = &topleft[ds_y*vid.width + ds_x1];
	source = ds_source;
	//colormap = ds_colormap;

#ifdef PS2_OPT_DRAW
	const UINT16 npo2_width = ds_flatwidth, npo2_height = ds_flatheight;
	lighttable_t **const npo2_pzl = planezlight;
	const ptrdiff_t npo2_cmofs = ds_colormap - colormaps;
	INT32 npo2_x = ds_x1;
#endif

#if 0	// The "perfect" reference version of this routine. Pretty slow.
		// Use it only to see how things are supposed to look.
	i = 0;
	do
	{
		slopereal_t z = 1.f/iz;
		u = (INT64)(uz*z);
		v = (INT64)(vz*z);

		colormap = NPO2_TILT_LIGHT;

		// Lactozilla: Non-powers-of-two
		{
			fixed_t x = (((fixed_t)u) >> FRACBITS);
			fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
			x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
			y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
			// Carefully align all of my Friends.
			if (x < 0)
				x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
			else
				x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
			if (y < 0)
				y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
			else
				y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

			*dest = colormap[source[((y * NPO2_TILT_WIDTH) + x)]];
		}
		dest++;
		iz += ds_sz.x;
		uz += ds_su.x;
		vz += ds_sv.x;
	} while (--width >= 0);
#else
	startz = 1.f/iz;
	startu = uz*startz;
	startv = vz*startz;

	izstep = ds_sz.x * SPANSIZE;
	uzstep = ds_su.x * SPANSIZE;
	vzstep = ds_sv.x * SPANSIZE;
	//x1 = 0;
	width++;

	while (width >= SPANSIZE)
	{
		iz += izstep;
		uz += uzstep;
		vz += vzstep;

		endz = 1.f/iz;
		endu = uz*endz;
		endv = vz*endz;
		stepu = SLOPE_U32((endu - startu) * INVSPAN);
		stepv = SLOPE_U32((endv - startv) * INVSPAN);
		u = SLOPE_U32(startu);
		v = SLOPE_U32(startv);

		for (i = SPANSIZE-1; i >= 0; i--)
		{
			colormap = NPO2_TILT_LIGHT;
			// Lactozilla: Non-powers-of-two
			{
				fixed_t x = (((fixed_t)u) >> FRACBITS);
				fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
				x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
				y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
				// Carefully align all of my Friends.
				if (x < 0)
					x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
				else
					x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
				if (y < 0)
					y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
				else
					y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

				*dest = colormap[source[((y * NPO2_TILT_WIDTH) + x)]];
			}
			dest++;
			u += stepu;
			v += stepv;
		}
		startu = endu;
		startv = endv;
		width -= SPANSIZE;
	}
	if (width > 0)
	{
		if (width == 1)
		{
			u = SLOPE_U32(startu);
			v = SLOPE_U32(startv);
			colormap = NPO2_TILT_LIGHT;
			// Lactozilla: Non-powers-of-two
			{
				fixed_t x = (((fixed_t)u) >> FRACBITS);
				fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
				x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
				y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
				// Carefully align all of my Friends.
				if (x < 0)
					x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
				else
					x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
				if (y < 0)
					y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
				else
					y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

				*dest = colormap[source[((y * NPO2_TILT_WIDTH) + x)]];
			}
		}
		else
		{
			slopereal_t left = width;
			iz += ds_sz.x * left;
			uz += ds_su.x * left;
			vz += ds_sv.x * left;

			endz = 1.f/iz;
			endu = uz*endz;
			endv = vz*endz;
			left = 1.f/left;
			stepu = SLOPE_U32((endu - startu) * left);
			stepv = SLOPE_U32((endv - startv) * left);
			u = SLOPE_U32(startu);
			v = SLOPE_U32(startv);

			for (; width != 0; width--)
			{
				colormap = NPO2_TILT_LIGHT;
				// Lactozilla: Non-powers-of-two
				{
					fixed_t x = (((fixed_t)u) >> FRACBITS);
					fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
					x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
					y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
					// Carefully align all of my Friends.
					if (x < 0)
						x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
					else
						x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
					if (y < 0)
						y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
					else
						y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

					*dest = colormap[source[((y * NPO2_TILT_WIDTH) + x)]];
				}
				dest++;
				u += stepu;
				v += stepv;
			}
		}
	}
#endif
#ifdef PS2_OPT_DRAW
	ds_x1 = npo2_x;
#endif
}

/**	\brief The R_DrawTiltedTranslucentSpan_NPO2_8 function
	Like DrawTiltedSpan_NPO2, but translucent
*/
void R_DrawTiltedTranslucentSpan_NPO2_8(void)
{
	// x1, x2 = ds_x1, ds_x2
	int width = ds_x2 - ds_x1;
	slopereal_t iz, uz, vz;
	UINT32 u, v;
	int i;

	UINT8 *source;
	UINT8 *colormap;
	UINT8 *dest;

	slopereal_t startz, startu, startv;
	slopereal_t izstep, uzstep, vzstep;
	slopereal_t endz, endu, endv;
	UINT32 stepu, stepv;

#ifdef PS2_OPT_DRAW
	UINT32 x_reciprocal, y_reciprocal;
	R_NPO2SlopeReciprocals(&x_reciprocal, &y_reciprocal);
#else
	struct libdivide_u32_t x_divider = libdivide_u32_gen(ds_flatwidth);
	struct libdivide_u32_t y_divider = libdivide_u32_gen(ds_flatheight);
#endif

	iz = ds_sz.z + ds_sz.y*(centery-ds_y) + ds_sz.x*(ds_x1-centerx);
	uz = ds_su.z + ds_su.y*(centery-ds_y) + ds_su.x*(ds_x1-centerx);
	vz = ds_sv.z + ds_sv.y*(centery-ds_y) + ds_sv.x*(ds_x1-centerx);

	R_CalcSlopeLight();

	dest = &topleft[ds_y*vid.width + ds_x1];
	source = ds_source;
	//colormap = ds_colormap;

#ifdef PS2_OPT_DRAW
	const UINT16 npo2_width = ds_flatwidth, npo2_height = ds_flatheight;
	lighttable_t **const npo2_pzl = planezlight;
	const ptrdiff_t npo2_cmofs = ds_colormap - colormaps;
	INT32 npo2_x = ds_x1;
	const UINT8 *const npo2_transmap = ds_transmap;
#endif

#if 0	// The "perfect" reference version of this routine. Pretty slow.
		// Use it only to see how things are supposed to look.
	i = 0;
	do
	{
		slopereal_t z = 1.f/iz;
		u = (INT64)(uz*z);
		v = (INT64)(vz*z);

		colormap = NPO2_TILT_LIGHT;
		// Lactozilla: Non-powers-of-two
		{
			fixed_t x = (((fixed_t)u) >> FRACBITS);
			fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
			x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
			y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
			// Carefully align all of my Friends.
			if (x < 0)
				x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
			else
				x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
			if (y < 0)
				y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
			else
				y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

			*dest = *(NPO2_TILT_TRANSMAP + (colormap[source[((y * NPO2_TILT_WIDTH) + x)]] << 8) + *dest);
		}
		dest++;
		iz += ds_sz.x;
		uz += ds_su.x;
		vz += ds_sv.x;
	} while (--width >= 0);
#else
	startz = 1.f/iz;
	startu = uz*startz;
	startv = vz*startz;

	izstep = ds_sz.x * SPANSIZE;
	uzstep = ds_su.x * SPANSIZE;
	vzstep = ds_sv.x * SPANSIZE;
	//x1 = 0;
	width++;

	while (width >= SPANSIZE)
	{
		iz += izstep;
		uz += uzstep;
		vz += vzstep;

		endz = 1.f/iz;
		endu = uz*endz;
		endv = vz*endz;
		stepu = SLOPE_U32((endu - startu) * INVSPAN);
		stepv = SLOPE_U32((endv - startv) * INVSPAN);
		u = SLOPE_U32(startu);
		v = SLOPE_U32(startv);

		for (i = SPANSIZE-1; i >= 0; i--)
		{
			colormap = NPO2_TILT_LIGHT;
			// Lactozilla: Non-powers-of-two
			{
				fixed_t x = (((fixed_t)u) >> FRACBITS);
				fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
				x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
				y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
				// Carefully align all of my Friends.
				if (x < 0)
					x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
				else
					x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
				if (y < 0)
					y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
				else
					y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

				*dest = *(NPO2_TILT_TRANSMAP + (colormap[source[((y * NPO2_TILT_WIDTH) + x)]] << 8) + *dest);
			}
			dest++;
			u += stepu;
			v += stepv;
		}
		startu = endu;
		startv = endv;
		width -= SPANSIZE;
	}
	if (width > 0)
	{
		if (width == 1)
		{
			u = SLOPE_U32(startu);
			v = SLOPE_U32(startv);
			colormap = NPO2_TILT_LIGHT;
			// Lactozilla: Non-powers-of-two
			{
				fixed_t x = (((fixed_t)u) >> FRACBITS);
				fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
				x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
				y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
				// Carefully align all of my Friends.
				if (x < 0)
					x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
				else
					x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
				if (y < 0)
					y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
				else
					y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

				*dest = *(NPO2_TILT_TRANSMAP + (colormap[source[((y * NPO2_TILT_WIDTH) + x)]] << 8) + *dest);
			}
		}
		else
		{
			slopereal_t left = width;
			iz += ds_sz.x * left;
			uz += ds_su.x * left;
			vz += ds_sv.x * left;

			endz = 1.f/iz;
			endu = uz*endz;
			endv = vz*endz;
			left = 1.f/left;
			stepu = SLOPE_U32((endu - startu) * left);
			stepv = SLOPE_U32((endv - startv) * left);
			u = SLOPE_U32(startu);
			v = SLOPE_U32(startv);

			for (; width != 0; width--)
			{
				colormap = NPO2_TILT_LIGHT;
				// Lactozilla: Non-powers-of-two
				{
					fixed_t x = (((fixed_t)u) >> FRACBITS);
					fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
					x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
					y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
					// Carefully align all of my Friends.
					if (x < 0)
						x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
					else
						x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
					if (y < 0)
						y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
					else
						y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

					*dest = *(NPO2_TILT_TRANSMAP + (colormap[source[((y * NPO2_TILT_WIDTH) + x)]] << 8) + *dest);
				}
				dest++;
				u += stepu;
				v += stepv;
			}
		}
	}
#endif
#ifdef PS2_OPT_DRAW
	ds_x1 = npo2_x;
#endif
}

void R_DrawTiltedSplat_NPO2_8(void)
{
	// x1, x2 = ds_x1, ds_x2
	int width = ds_x2 - ds_x1;
	slopereal_t iz, uz, vz;
	UINT32 u, v;
	int i;

	UINT8 *source;
	UINT8 *colormap;
	UINT8 *dest;

	UINT8 val;

	slopereal_t startz, startu, startv;
	slopereal_t izstep, uzstep, vzstep;
	slopereal_t endz, endu, endv;
	UINT32 stepu, stepv;

#ifdef PS2_OPT_DRAW
	UINT32 x_reciprocal, y_reciprocal;
	R_NPO2SlopeReciprocals(&x_reciprocal, &y_reciprocal);
#else
	struct libdivide_u32_t x_divider = libdivide_u32_gen(ds_flatwidth);
	struct libdivide_u32_t y_divider = libdivide_u32_gen(ds_flatheight);
#endif

	iz = ds_sz.z + ds_sz.y*(centery-ds_y) + ds_sz.x*(ds_x1-centerx);
	uz = ds_su.z + ds_su.y*(centery-ds_y) + ds_su.x*(ds_x1-centerx);
	vz = ds_sv.z + ds_sv.y*(centery-ds_y) + ds_sv.x*(ds_x1-centerx);

	R_CalcSlopeLight();

	dest = &topleft[ds_y*vid.width + ds_x1];
	source = ds_source;
	//colormap = ds_colormap;

#ifdef PS2_OPT_DRAW
	const UINT16 npo2_width = ds_flatwidth, npo2_height = ds_flatheight;
	lighttable_t **const npo2_pzl = planezlight;
	const ptrdiff_t npo2_cmofs = ds_colormap - colormaps;
	INT32 npo2_x = ds_x1;
#endif

#if 0	// The "perfect" reference version of this routine. Pretty slow.
		// Use it only to see how things are supposed to look.
	i = 0;
	do
	{
		slopereal_t z = 1.f/iz;
		u = (INT64)(uz*z);
		v = (INT64)(vz*z);

		colormap = NPO2_TILT_LIGHT;

		// Lactozilla: Non-powers-of-two
		{
			fixed_t x = (((fixed_t)u) >> FRACBITS);
			fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
			x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
			y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
			// Carefully align all of my Friends.
			if (x < 0)
				x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
			else
				x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
			if (y < 0)
				y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
			else
				y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

			val = source[((y * NPO2_TILT_WIDTH) + x)];
		}

		if (val != TRANSPARENTPIXEL)
			*dest = colormap[val];

		dest++;
		iz += ds_sz.x;
		uz += ds_su.x;
		vz += ds_sv.x;
	} while (--width >= 0);
#else
	startz = 1.f/iz;
	startu = uz*startz;
	startv = vz*startz;

	izstep = ds_sz.x * SPANSIZE;
	uzstep = ds_su.x * SPANSIZE;
	vzstep = ds_sv.x * SPANSIZE;
	//x1 = 0;
	width++;

	while (width >= SPANSIZE)
	{
		iz += izstep;
		uz += uzstep;
		vz += vzstep;

		endz = 1.f/iz;
		endu = uz*endz;
		endv = vz*endz;
		stepu = SLOPE_U32((endu - startu) * INVSPAN);
		stepv = SLOPE_U32((endv - startv) * INVSPAN);
		u = SLOPE_U32(startu);
		v = SLOPE_U32(startv);

		for (i = SPANSIZE-1; i >= 0; i--)
		{
			colormap = NPO2_TILT_LIGHT;
			// Lactozilla: Non-powers-of-two
			{
				fixed_t x = (((fixed_t)u) >> FRACBITS);
				fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
				x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
				y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
				// Carefully align all of my Friends.
				if (x < 0)
					x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
				else
					x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
				if (y < 0)
					y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
				else
					y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

				val = source[((y * NPO2_TILT_WIDTH) + x)];
			}
			if (val != TRANSPARENTPIXEL)
				*dest = colormap[val];
			dest++;
			u += stepu;
			v += stepv;
		}
		startu = endu;
		startv = endv;
		width -= SPANSIZE;
	}
	if (width > 0)
	{
		if (width == 1)
		{
			u = SLOPE_U32(startu);
			v = SLOPE_U32(startv);
			colormap = NPO2_TILT_LIGHT;
			// Lactozilla: Non-powers-of-two
			{
				fixed_t x = (((fixed_t)u) >> FRACBITS);
				fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
				x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
				y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
				// Carefully align all of my Friends.
				if (x < 0)
					x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
				else
					x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
				if (y < 0)
					y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
				else
					y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

				val = source[((y * NPO2_TILT_WIDTH) + x)];
			}
			if (val != TRANSPARENTPIXEL)
				*dest = colormap[val];
		}
		else
		{
			slopereal_t left = width;
			iz += ds_sz.x * left;
			uz += ds_su.x * left;
			vz += ds_sv.x * left;

			endz = 1.f/iz;
			endu = uz*endz;
			endv = vz*endz;
			left = 1.f/left;
			stepu = SLOPE_U32((endu - startu) * left);
			stepv = SLOPE_U32((endv - startv) * left);
			u = SLOPE_U32(startu);
			v = SLOPE_U32(startv);

			for (; width != 0; width--)
			{
				colormap = NPO2_TILT_LIGHT;
				// Lactozilla: Non-powers-of-two
				{
					fixed_t x = (((fixed_t)u) >> FRACBITS);
					fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
					x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
					y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
					// Carefully align all of my Friends.
					if (x < 0)
						x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
					else
						x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
					if (y < 0)
						y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
					else
						y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

					val = source[((y * NPO2_TILT_WIDTH) + x)];
				}
				if (val != TRANSPARENTPIXEL)
					*dest = colormap[val];
				dest++;
				u += stepu;
				v += stepv;
			}
		}
	}
#endif
#ifdef PS2_OPT_DRAW
	ds_x1 = npo2_x;
#endif
}

void R_DrawTiltedTranslucentSplat_NPO2_8(void)
{
	// x1, x2 = ds_x1, ds_x2
	int width = ds_x2 - ds_x1;
	slopereal_t iz, uz, vz;
	UINT32 u, v;
	int i;

	UINT8 *source;
	UINT8 *colormap;
	UINT8 *dest;

	UINT8 val;

	slopereal_t startz, startu, startv;
	slopereal_t izstep, uzstep, vzstep;
	slopereal_t endz, endu, endv;
	UINT32 stepu, stepv;

#ifdef PS2_OPT_DRAW
	UINT32 x_reciprocal, y_reciprocal;
	R_NPO2SlopeReciprocals(&x_reciprocal, &y_reciprocal);
#else
	struct libdivide_u32_t x_divider = libdivide_u32_gen(ds_flatwidth);
	struct libdivide_u32_t y_divider = libdivide_u32_gen(ds_flatheight);
#endif

	iz = ds_sz.z + ds_sz.y*(centery-ds_y) + ds_sz.x*(ds_x1-centerx);
	uz = ds_su.z + ds_su.y*(centery-ds_y) + ds_su.x*(ds_x1-centerx);
	vz = ds_sv.z + ds_sv.y*(centery-ds_y) + ds_sv.x*(ds_x1-centerx);

	R_CalcSlopeLight();

	dest = &topleft[ds_y*vid.width + ds_x1];
	source = ds_source;
	//colormap = ds_colormap;

#ifdef PS2_OPT_DRAW
	const UINT16 npo2_width = ds_flatwidth, npo2_height = ds_flatheight;
	lighttable_t **const npo2_pzl = planezlight;
	const ptrdiff_t npo2_cmofs = ds_colormap - colormaps;
	INT32 npo2_x = ds_x1;
	const UINT8 *const npo2_transmap = ds_transmap;
#endif

#if 0	// The "perfect" reference version of this routine. Pretty slow.
		// Use it only to see how things are supposed to look.
	i = 0;
	do
	{
		slopereal_t z = 1.f/iz;
		u = (INT64)(uz*z);
		v = (INT64)(vz*z);

		colormap = NPO2_TILT_LIGHT;

		// Lactozilla: Non-powers-of-two
		{
			fixed_t x = (((fixed_t)u) >> FRACBITS);
			fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
			x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
			y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
			// Carefully align all of my Friends.
			if (x < 0)
				x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
			else
				x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
			if (y < 0)
				y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
			else
				y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

			val = source[((y * NPO2_TILT_WIDTH) + x)];
		}

		if (val != TRANSPARENTPIXEL)
			*dest = *(NPO2_TILT_TRANSMAP + (colormap[val] << 8) + *dest);

		dest++;
		iz += ds_sz.x;
		uz += ds_su.x;
		vz += ds_sv.x;
	} while (--width >= 0);
#else
	startz = 1.f/iz;
	startu = uz*startz;
	startv = vz*startz;

	izstep = ds_sz.x * SPANSIZE;
	uzstep = ds_su.x * SPANSIZE;
	vzstep = ds_sv.x * SPANSIZE;
	//x1 = 0;
	width++;

	while (width >= SPANSIZE)
	{
		iz += izstep;
		uz += uzstep;
		vz += vzstep;

		endz = 1.f/iz;
		endu = uz*endz;
		endv = vz*endz;
		stepu = SLOPE_U32((endu - startu) * INVSPAN);
		stepv = SLOPE_U32((endv - startv) * INVSPAN);
		u = SLOPE_U32(startu);
		v = SLOPE_U32(startv);

		for (i = SPANSIZE-1; i >= 0; i--)
		{
			colormap = NPO2_TILT_LIGHT;
			// Lactozilla: Non-powers-of-two
			{
				fixed_t x = (((fixed_t)u) >> FRACBITS);
				fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
				x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
				y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
				// Carefully align all of my Friends.
				if (x < 0)
					x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
				else
					x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
				if (y < 0)
					y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
				else
					y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

				val = source[((y * NPO2_TILT_WIDTH) + x)];
			}
			if (val != TRANSPARENTPIXEL)
				*dest = *(NPO2_TILT_TRANSMAP + (colormap[val] << 8) + *dest);
			dest++;
			u += stepu;
			v += stepv;
		}
		startu = endu;
		startv = endv;
		width -= SPANSIZE;
	}
	if (width > 0)
	{
		if (width == 1)
		{
			u = SLOPE_U32(startu);
			v = SLOPE_U32(startv);
			colormap = NPO2_TILT_LIGHT;
			// Lactozilla: Non-powers-of-two
			{
				fixed_t x = (((fixed_t)u) >> FRACBITS);
				fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
				x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
				y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
				// Carefully align all of my Friends.
				if (x < 0)
					x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
				else
					x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
				if (y < 0)
					y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
				else
					y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

				val = source[((y * NPO2_TILT_WIDTH) + x)];
			}
			if (val != TRANSPARENTPIXEL)
				*dest = *(NPO2_TILT_TRANSMAP + (colormap[val] << 8) + *dest);
		}
		else
		{
			slopereal_t left = width;
			iz += ds_sz.x * left;
			uz += ds_su.x * left;
			vz += ds_sv.x * left;

			endz = 1.f/iz;
			endu = uz*endz;
			endv = vz*endz;
			left = 1.f/left;
			stepu = SLOPE_U32((endu - startu) * left);
			stepv = SLOPE_U32((endv - startv) * left);
			u = SLOPE_U32(startu);
			v = SLOPE_U32(startv);

			for (; width != 0; width--)
			{
				colormap = NPO2_TILT_LIGHT;
				// Lactozilla: Non-powers-of-two
				{
					fixed_t x = (((fixed_t)u) >> FRACBITS);
					fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
					x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
					y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
					// Carefully align all of my Friends.
					if (x < 0)
						x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
					else
						x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
					if (y < 0)
						y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
					else
						y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

					val = source[((y * NPO2_TILT_WIDTH) + x)];
				}
				if (val != TRANSPARENTPIXEL)
					*dest = *(NPO2_TILT_TRANSMAP + (colormap[val] << 8) + *dest);
				dest++;
				u += stepu;
				v += stepv;
			}
		}
	}
#endif
#ifdef PS2_OPT_DRAW
	ds_x1 = npo2_x;
#endif
}

/**	\brief The R_DrawSplat_NPO2_8 function
	Just like R_DrawSpan_NPO2_8, but skips transparent pixels.
*/
void R_DrawSplat_NPO2_8 (void)
{
	fixed_t xposition;
	fixed_t yposition;
	fixed_t xstep, ystep;
	fixed_t x, y;
	fixed_t fixedwidth, fixedheight;

	UINT8 *source;
	UINT8 *colormap;
	UINT8 *dest;
	const UINT8 *deststop = screens[0] + vid.rowbytes * vid.height;

	size_t count = (ds_x2 - ds_x1 + 1);
	UINT32 val;

	xposition = ds_xfrac; yposition = ds_yfrac;
	xstep = ds_xstep; ystep = ds_ystep;

	source = ds_source;
	colormap = ds_colormap;
	dest = &topleft[ds_y*vid.width + ds_x1];

	fixedwidth = ds_flatwidth << FRACBITS;
	fixedheight = ds_flatheight << FRACBITS;

	// Fix xposition and yposition if they are out of bounds.
	if (xposition < 0)
		xposition = fixedwidth - ((UINT32)(fixedwidth - xposition) % fixedwidth);
	else if (xposition >= fixedwidth)
		xposition %= fixedwidth;
	if (yposition < 0)
		yposition = fixedheight - ((UINT32)(fixedheight - yposition) % fixedheight);
	else if (yposition >= fixedheight)
		yposition %= fixedheight;

#ifdef PS2_OPT_DRAW
	// With bounded steps, unsigned range tests need one correction per axis.
	// Keep the original loop for overflow-sized flats/steps and noncanonical starts.
	if (count >= 32 && fixedwidth > 0 && fixedwidth <= 0x40000000
		&& fixedheight > 0 && fixedheight <= 0x40000000
		&& (UINT32)xposition < (UINT32)fixedwidth
		&& (UINT32)yposition < (UINT32)fixedheight
		&& xstep > -fixedwidth && xstep < fixedwidth
		&& ystep > -fixedheight && ystep < fixedheight)
	{
		const INT32 flatwidth = ds_flatwidth;
		const fixed_t xwrap = xstep < 0 ? fixedwidth : -fixedwidth;
		const fixed_t ywrap = ystep < 0 ? fixedheight : -fixedheight;
		// Preserve the original inclusive destination bound once per span.
		if (count > (size_t)(deststop - dest) + 1)
			count = (size_t)(deststop - dest) + 1;
		while (count--)
		{
			if ((UINT32)xposition >= (UINT32)fixedwidth)
				xposition += xwrap;
			if ((UINT32)yposition >= (UINT32)fixedheight)
				yposition += ywrap;
			x = (xposition >> FRACBITS);
			y = (yposition >> FRACBITS);
			val = source[((y * flatwidth) + x)];
			if (val != TRANSPARENTPIXEL)
				*dest = colormap[val];
			dest++;
			xposition += xstep;
			yposition += ystep;
		}
		return;
	}
#endif

	while (count-- && dest <= deststop)
	{
		// The loops here keep the texture coordinates within the texture.
		// They will rarely iterate multiple times, and are cheaper than a modulo operation,
		// even if using libdivide.
		if (xstep < 0) // These if statements are hopefully hoisted by the compiler to above this loop
			while (xposition < 0)
				xposition += fixedwidth;
		else
			while (xposition >= fixedwidth)
				xposition -= fixedwidth;
		if (ystep < 0)
			while (yposition < 0)
				yposition += fixedheight;
		else
			while (yposition >= fixedheight)
				yposition -= fixedheight;

		x = (xposition >> FRACBITS);
		y = (yposition >> FRACBITS);
		val = source[((y * ds_flatwidth) + x)];
		if (val != TRANSPARENTPIXEL)
			*dest = colormap[val];
		dest++;
		xposition += xstep;
		yposition += ystep;
	}
}

/**	\brief The R_DrawTranslucentSplat_NPO2_8 function
	Just like R_DrawSplat_NPO2_8, but is translucent!
*/
void R_DrawTranslucentSplat_NPO2_8 (void)
{
	fixed_t xposition;
	fixed_t yposition;
	fixed_t xstep, ystep;
	fixed_t x, y;
	fixed_t fixedwidth, fixedheight;

	UINT8 *source;
	UINT8 *colormap;
	UINT8 *dest;
	const UINT8 *deststop = screens[0] + vid.rowbytes * vid.height;

	size_t count = (ds_x2 - ds_x1 + 1);
	UINT32 val;

	xposition = ds_xfrac; yposition = ds_yfrac;
	xstep = ds_xstep; ystep = ds_ystep;

	source = ds_source;
	colormap = ds_colormap;
	dest = &topleft[ds_y*vid.width + ds_x1];

	fixedwidth = ds_flatwidth << FRACBITS;
	fixedheight = ds_flatheight << FRACBITS;

	// Fix xposition and yposition if they are out of bounds.
	if (xposition < 0)
		xposition = fixedwidth - ((UINT32)(fixedwidth - xposition) % fixedwidth);
	else if (xposition >= fixedwidth)
		xposition %= fixedwidth;
	if (yposition < 0)
		yposition = fixedheight - ((UINT32)(fixedheight - yposition) % fixedheight);
	else if (yposition >= fixedheight)
		yposition %= fixedheight;

#ifdef PS2_OPT_DRAW
	// With bounded steps, unsigned range tests need one correction per axis.
	// Keep the original loop for overflow-sized flats/steps and noncanonical starts.
	if (count >= 32 && fixedwidth > 0 && fixedwidth <= 0x40000000
		&& fixedheight > 0 && fixedheight <= 0x40000000
		&& (UINT32)xposition < (UINT32)fixedwidth
		&& (UINT32)yposition < (UINT32)fixedheight
		&& xstep > -fixedwidth && xstep < fixedwidth
		&& ystep > -fixedheight && ystep < fixedheight)
	{
		const INT32 flatwidth = ds_flatwidth;
		const fixed_t xwrap = xstep < 0 ? fixedwidth : -fixedwidth;
		const fixed_t ywrap = ystep < 0 ? fixedheight : -fixedheight;
		// Preserve the original inclusive destination bound once per span.
		if (count > (size_t)(deststop - dest) + 1)
			count = (size_t)(deststop - dest) + 1;
		while (count--)
		{
			if ((UINT32)xposition >= (UINT32)fixedwidth)
				xposition += xwrap;
			if ((UINT32)yposition >= (UINT32)fixedheight)
				yposition += ywrap;
			x = (xposition >> FRACBITS);
			y = (yposition >> FRACBITS);
			val = source[((y * flatwidth) + x)];
			if (val != TRANSPARENTPIXEL)
				*dest = *(ds_transmap + (colormap[val] << 8) + *dest);
			dest++;
			xposition += xstep;
			yposition += ystep;
		}
		return;
	}
#endif

	while (count-- && dest <= deststop)
	{
		// The loops here keep the texture coordinates within the texture.
		// They will rarely iterate multiple times, and are cheaper than a modulo operation,
		// even if using libdivide.
		if (xstep < 0) // These if statements are hopefully hoisted by the compiler to above this loop
			while (xposition < 0)
				xposition += fixedwidth;
		else
			while (xposition >= fixedwidth)
				xposition -= fixedwidth;
		if (ystep < 0)
			while (yposition < 0)
				yposition += fixedheight;
		else
			while (yposition >= fixedheight)
				yposition -= fixedheight;

		x = (xposition >> FRACBITS);
		y = (yposition >> FRACBITS);
		val = source[((y * ds_flatwidth) + x)];
		if (val != TRANSPARENTPIXEL)
			*dest = *(ds_transmap + (colormap[val] << 8) + *dest);
		dest++;
		xposition += xstep;
		yposition += ystep;
	}
}

/**	\brief The R_DrawFloorSprite_NPO2_8 function
	Just like R_DrawSplat_NPO2_8, but for floor sprites.
*/
void R_DrawFloorSprite_NPO2_8 (void)
{
	fixed_t xposition;
	fixed_t yposition;
	fixed_t xstep, ystep;
	fixed_t x, y;
	fixed_t fixedwidth, fixedheight;

	UINT16 *source;
	UINT8 *translation;
	UINT8 *colormap;
	UINT8 *dest;
	const UINT8 *deststop = screens[0] + vid.rowbytes * vid.height;

	size_t count = (ds_x2 - ds_x1 + 1);
	UINT32 val;

	xposition = ds_xfrac; yposition = ds_yfrac;
	xstep = ds_xstep; ystep = ds_ystep;

	source = (UINT16 *)ds_source;
	colormap = ds_colormap;
	translation = ds_translation;
	dest = &topleft[ds_y*vid.width + ds_x1];

	fixedwidth = ds_flatwidth << FRACBITS;
	fixedheight = ds_flatheight << FRACBITS;

	// Fix xposition and yposition if they are out of bounds.
	if (xposition < 0)
		xposition = fixedwidth - ((UINT32)(fixedwidth - xposition) % fixedwidth);
	else if (xposition >= fixedwidth)
		xposition %= fixedwidth;
	if (yposition < 0)
		yposition = fixedheight - ((UINT32)(fixedheight - yposition) % fixedheight);
	else if (yposition >= fixedheight)
		yposition %= fixedheight;

#ifdef PS2_OPT_DRAW
	// With bounded steps, unsigned range tests need one correction per axis.
	// Keep the original loop for overflow-sized flats/steps and noncanonical starts.
	if (count >= 32 && fixedwidth > 0 && fixedwidth <= 0x40000000
		&& fixedheight > 0 && fixedheight <= 0x40000000
		&& (UINT32)xposition < (UINT32)fixedwidth
		&& (UINT32)yposition < (UINT32)fixedheight
		&& xstep > -fixedwidth && xstep < fixedwidth
		&& ystep > -fixedheight && ystep < fixedheight)
	{
		const INT32 flatwidth = ds_flatwidth;
		const fixed_t xwrap = xstep < 0 ? fixedwidth : -fixedwidth;
		const fixed_t ywrap = ystep < 0 ? fixedheight : -fixedheight;
		// Preserve the original inclusive destination bound once per span.
		if (count > (size_t)(deststop - dest) + 1)
			count = (size_t)(deststop - dest) + 1;
		while (count--)
		{
			if ((UINT32)xposition >= (UINT32)fixedwidth)
				xposition += xwrap;
			if ((UINT32)yposition >= (UINT32)fixedheight)
				yposition += ywrap;
			x = (xposition >> FRACBITS);
			y = (yposition >> FRACBITS);
			val = source[((y * flatwidth) + x)];
			if (val & 0xFF00)
				*dest = colormap[translation[val & 0xFF]];
			dest++;
			xposition += xstep;
			yposition += ystep;
		}
		return;
	}
#endif

	while (count-- && dest <= deststop)
	{
		// The loops here keep the texture coordinates within the texture.
		// They will rarely iterate multiple times, and are cheaper than a modulo operation,
		// even if using libdivide.
		if (xstep < 0) // These if statements are hopefully hoisted by the compiler to above this loop
			while (xposition < 0)
				xposition += fixedwidth;
		else
			while (xposition >= fixedwidth)
				xposition -= fixedwidth;
		if (ystep < 0)
			while (yposition < 0)
				yposition += fixedheight;
		else
			while (yposition >= fixedheight)
				yposition -= fixedheight;

		x = (xposition >> FRACBITS);
		y = (yposition >> FRACBITS);
		val = source[((y * ds_flatwidth) + x)];
		if (val & 0xFF00)
			*dest = colormap[translation[val & 0xFF]];
		dest++;
		xposition += xstep;
		yposition += ystep;
	}
}

/**	\brief The R_DrawTranslucentFloorSprite_NPO2_8 function
	Just like R_DrawFloorSprite_NPO2_8, but is translucent!
*/
void R_DrawTranslucentFloorSprite_NPO2_8 (void)
{
	fixed_t xposition;
	fixed_t yposition;
	fixed_t xstep, ystep;
	fixed_t x, y;
	fixed_t fixedwidth, fixedheight;

	UINT16 *source;
	UINT8 *translation;
	UINT8 *colormap;
	UINT8 *dest;
	const UINT8 *deststop = screens[0] + vid.rowbytes * vid.height;

	size_t count = (ds_x2 - ds_x1 + 1);
	UINT32 val;

	xposition = ds_xfrac; yposition = ds_yfrac;
	xstep = ds_xstep; ystep = ds_ystep;

	source = (UINT16 *)ds_source;
	colormap = ds_colormap;
	translation = ds_translation;
	dest = &topleft[ds_y*vid.width + ds_x1];

	fixedwidth = ds_flatwidth << FRACBITS;
	fixedheight = ds_flatheight << FRACBITS;

	// Fix xposition and yposition if they are out of bounds.
	if (xposition < 0)
		xposition = fixedwidth - ((UINT32)(fixedwidth - xposition) % fixedwidth);
	else if (xposition >= fixedwidth)
		xposition %= fixedwidth;
	if (yposition < 0)
		yposition = fixedheight - ((UINT32)(fixedheight - yposition) % fixedheight);
	else if (yposition >= fixedheight)
		yposition %= fixedheight;

#ifdef PS2_OPT_DRAW
	// With bounded steps, unsigned range tests need one correction per axis.
	// Keep the original loop for overflow-sized flats/steps and noncanonical starts.
	if (count >= 32 && fixedwidth > 0 && fixedwidth <= 0x40000000
		&& fixedheight > 0 && fixedheight <= 0x40000000
		&& (UINT32)xposition < (UINT32)fixedwidth
		&& (UINT32)yposition < (UINT32)fixedheight
		&& xstep > -fixedwidth && xstep < fixedwidth
		&& ystep > -fixedheight && ystep < fixedheight)
	{
		const INT32 flatwidth = ds_flatwidth;
		const fixed_t xwrap = xstep < 0 ? fixedwidth : -fixedwidth;
		const fixed_t ywrap = ystep < 0 ? fixedheight : -fixedheight;
		// Preserve the original inclusive destination bound once per span.
		if (count > (size_t)(deststop - dest) + 1)
			count = (size_t)(deststop - dest) + 1;
		while (count--)
		{
			if ((UINT32)xposition >= (UINT32)fixedwidth)
				xposition += xwrap;
			if ((UINT32)yposition >= (UINT32)fixedheight)
				yposition += ywrap;
			x = (xposition >> FRACBITS);
			y = (yposition >> FRACBITS);
			val = source[((y * flatwidth) + x)];
			if (val & 0xFF00)
				*dest = *(ds_transmap + (colormap[translation[val & 0xFF]] << 8) + *dest);
			dest++;
			xposition += xstep;
			yposition += ystep;
		}
		return;
	}
#endif

	while (count-- && dest <= deststop)
	{
		// The loops here keep the texture coordinates within the texture.
		// They will rarely iterate multiple times, and are cheaper than a modulo operation,
		// even if using libdivide.
		if (xstep < 0) // These if statements are hopefully hoisted by the compiler to above this loop
			while (xposition < 0)
				xposition += fixedwidth;
		else
			while (xposition >= fixedwidth)
				xposition -= fixedwidth;
		if (ystep < 0)
			while (yposition < 0)
				yposition += fixedheight;
		else
			while (yposition >= fixedheight)
				yposition -= fixedheight;

		x = (xposition >> FRACBITS);
		y = (yposition >> FRACBITS);
		val = source[((y * ds_flatwidth) + x)];
		if (val & 0xFF00)
			*dest = *(ds_transmap + (colormap[translation[val & 0xFF]] << 8) + *dest);
		dest++;
		xposition += xstep;
		yposition += ystep;
	}
}

/**	\brief The R_DrawTiltedFloorSprite_NPO2_8 function
	Draws a tilted floor sprite.
*/
void R_DrawTiltedFloorSprite_NPO2_8(void)
{
	// x1, x2 = ds_x1, ds_x2
	int width = ds_x2 - ds_x1;
	slopereal_t iz, uz, vz;
	UINT32 u, v;
	int i;

	UINT16 *source;
	UINT8 *colormap;
	UINT8 *translation;
	UINT8 *dest;
	UINT16 val;

	slopereal_t startz, startu, startv;
	slopereal_t izstep, uzstep, vzstep;
	slopereal_t endz, endu, endv;
	UINT32 stepu, stepv;

#ifdef PS2_OPT_DRAW
	UINT32 x_reciprocal, y_reciprocal;
	R_NPO2SlopeReciprocals(&x_reciprocal, &y_reciprocal);
#else
	struct libdivide_u32_t x_divider = libdivide_u32_gen(ds_flatwidth);
	struct libdivide_u32_t y_divider = libdivide_u32_gen(ds_flatheight);
#endif

	iz = ds_sz.z + ds_sz.y*(centery-ds_y) + ds_sz.x*(ds_x1-centerx);
	uz = ds_su.z + ds_su.y*(centery-ds_y) + ds_su.x*(ds_x1-centerx);
	vz = ds_sv.z + ds_sv.y*(centery-ds_y) + ds_sv.x*(ds_x1-centerx);

	dest = &topleft[ds_y*vid.width + ds_x1];
	source = (UINT16 *)ds_source;
	colormap = ds_colormap;
	translation = ds_translation;

#ifdef PS2_OPT_DRAW
	const UINT16 npo2_width = ds_flatwidth, npo2_height = ds_flatheight;
#endif

	startz = 1.f/iz;
	startu = uz*startz;
	startv = vz*startz;

	izstep = ds_sz.x * SPANSIZE;
	uzstep = ds_su.x * SPANSIZE;
	vzstep = ds_sv.x * SPANSIZE;
	//x1 = 0;
	width++;

	while (width >= SPANSIZE)
	{
		iz += izstep;
		uz += uzstep;
		vz += vzstep;

		endz = 1.f/iz;
		endu = uz*endz;
		endv = vz*endz;
		stepu = SLOPE_U32((endu - startu) * INVSPAN);
		stepv = SLOPE_U32((endv - startv) * INVSPAN);
		u = SLOPE_U32(startu);
		v = SLOPE_U32(startv);

		for (i = SPANSIZE-1; i >= 0; i--)
		{
			// Lactozilla: Non-powers-of-two
			fixed_t x = (((fixed_t)u) >> FRACBITS);
			fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
			x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
			y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
			// Carefully align all of my Friends.
			if (x < 0)
				x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
			else
				x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
			if (y < 0)
				y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
			else
				y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

			val = source[((y * NPO2_TILT_WIDTH) + x)];
			if (val & 0xFF00)
				*dest = colormap[translation[val & 0xFF]];
			dest++;

			u += stepu;
			v += stepv;
		}
		startu = endu;
		startv = endv;
		width -= SPANSIZE;
	}
	if (width > 0)
	{
		if (width == 1)
		{
			u = SLOPE_U32(startu);
			v = SLOPE_U32(startv);
			// Lactozilla: Non-powers-of-two
			{
				fixed_t x = (((fixed_t)u) >> FRACBITS);
				fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
				x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
				y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
				// Carefully align all of my Friends.
				if (x < 0)
					x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
				else
					x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
				if (y < 0)
					y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
				else
					y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

				val = source[((y * NPO2_TILT_WIDTH) + x)];
				if (val & 0xFF00)
					*dest = colormap[translation[val & 0xFF]];
			}
		}
		else
		{
			slopereal_t left = width;
			iz += ds_sz.x * left;
			uz += ds_su.x * left;
			vz += ds_sv.x * left;

			endz = 1.f/iz;
			endu = uz*endz;
			endv = vz*endz;
			left = 1.f/left;
			stepu = SLOPE_U32((endu - startu) * left);
			stepv = SLOPE_U32((endv - startv) * left);
			u = SLOPE_U32(startu);
			v = SLOPE_U32(startv);

			for (; width != 0; width--)
			{
				// Lactozilla: Non-powers-of-two
				fixed_t x = (((fixed_t)u) >> FRACBITS);
				fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
				x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
				y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
				// Carefully align all of my Friends.
				if (x < 0)
					x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
				else
					x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
				if (y < 0)
					y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
				else
					y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

				val = source[((y * NPO2_TILT_WIDTH) + x)];
				if (val & 0xFF00)
					*dest = colormap[translation[val & 0xFF]];
				dest++;

				u += stepu;
				v += stepv;
			}
		}
	}
}

/**	\brief The R_DrawTiltedTranslucentFloorSprite_NPO2_8 function
	Draws a tilted, translucent, floor sprite.
*/
void R_DrawTiltedTranslucentFloorSprite_NPO2_8(void)
{
	// x1, x2 = ds_x1, ds_x2
	int width = ds_x2 - ds_x1;
	slopereal_t iz, uz, vz;
	UINT32 u, v;
	int i;

	UINT16 *source;
	UINT8 *colormap;
	UINT8 *translation;
	UINT8 *dest;
	UINT16 val;

	slopereal_t startz, startu, startv;
	slopereal_t izstep, uzstep, vzstep;
	slopereal_t endz, endu, endv;
	UINT32 stepu, stepv;

#ifdef PS2_OPT_DRAW
	UINT32 x_reciprocal, y_reciprocal;
	R_NPO2SlopeReciprocals(&x_reciprocal, &y_reciprocal);
#else
	struct libdivide_u32_t x_divider = libdivide_u32_gen(ds_flatwidth);
	struct libdivide_u32_t y_divider = libdivide_u32_gen(ds_flatheight);
#endif

	iz = ds_sz.z + ds_sz.y*(centery-ds_y) + ds_sz.x*(ds_x1-centerx);
	uz = ds_su.z + ds_su.y*(centery-ds_y) + ds_su.x*(ds_x1-centerx);
	vz = ds_sv.z + ds_sv.y*(centery-ds_y) + ds_sv.x*(ds_x1-centerx);

	dest = &topleft[ds_y*vid.width + ds_x1];
	source = (UINT16 *)ds_source;
	colormap = ds_colormap;
	translation = ds_translation;

#ifdef PS2_OPT_DRAW
	const UINT16 npo2_width = ds_flatwidth, npo2_height = ds_flatheight;
	const UINT8 *const npo2_transmap = ds_transmap;
#endif

	startz = 1.f/iz;
	startu = uz*startz;
	startv = vz*startz;

	izstep = ds_sz.x * SPANSIZE;
	uzstep = ds_su.x * SPANSIZE;
	vzstep = ds_sv.x * SPANSIZE;
	//x1 = 0;
	width++;

	while (width >= SPANSIZE)
	{
		iz += izstep;
		uz += uzstep;
		vz += vzstep;

		endz = 1.f/iz;
		endu = uz*endz;
		endv = vz*endz;
		stepu = SLOPE_U32((endu - startu) * INVSPAN);
		stepv = SLOPE_U32((endv - startv) * INVSPAN);
		u = SLOPE_U32(startu);
		v = SLOPE_U32(startv);

		for (i = SPANSIZE-1; i >= 0; i--)
		{
			// Lactozilla: Non-powers-of-two
			fixed_t x = (((fixed_t)u) >> FRACBITS);
			fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
			x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
			y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
			// Carefully align all of my Friends.
			if (x < 0)
				x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
			else
				x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
			if (y < 0)
				y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
			else
				y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

			val = source[((y * NPO2_TILT_WIDTH) + x)];
			if (val & 0xFF00)
				*dest = *(NPO2_TILT_TRANSMAP + (colormap[translation[val & 0xFF]] << 8) + *dest);
			dest++;

			u += stepu;
			v += stepv;
		}
		startu = endu;
		startv = endv;
		width -= SPANSIZE;
	}
	if (width > 0)
	{
		if (width == 1)
		{
			u = SLOPE_U32(startu);
			v = SLOPE_U32(startv);
			// Lactozilla: Non-powers-of-two
			{
				fixed_t x = (((fixed_t)u) >> FRACBITS);
				fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
				x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
				y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
				// Carefully align all of my Friends.
				if (x < 0)
					x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
				else
					x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
				if (y < 0)
					y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
				else
					y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

				val = source[((y * NPO2_TILT_WIDTH) + x)];
				if (val & 0xFF00)
					*dest = *(NPO2_TILT_TRANSMAP + (colormap[translation[val & 0xFF]] << 8) + *dest);
			}
		}
		else
		{
			slopereal_t left = width;
			iz += ds_sz.x * left;
			uz += ds_su.x * left;
			vz += ds_sv.x * left;

			endz = 1.f/iz;
			endu = uz*endz;
			endv = vz*endz;
			left = 1.f/left;
			stepu = SLOPE_U32((endu - startu) * left);
			stepv = SLOPE_U32((endv - startv) * left);
			u = SLOPE_U32(startu);
			v = SLOPE_U32(startv);

			for (; width != 0; width--)
			{
				// Lactozilla: Non-powers-of-two
				fixed_t x = (((fixed_t)u) >> FRACBITS);
				fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
				x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
				y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
				// Carefully align all of my Friends.
				if (x < 0)
					x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
				else
					x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
				if (y < 0)
					y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
				else
					y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

				val = source[((y * NPO2_TILT_WIDTH) + x)];
				if (val & 0xFF00)
					*dest = *(NPO2_TILT_TRANSMAP + (colormap[translation[val & 0xFF]] << 8) + *dest);
				dest++;

				u += stepu;
				v += stepv;
			}
		}
	}
}

/**	\brief The R_DrawTranslucentSpan_NPO2_8 function
	Draws the actual span with translucency.
*/
void R_DrawTranslucentSpan_NPO2_8 (void)
{
	fixed_t xposition;
	fixed_t yposition;
	fixed_t xstep, ystep;
	fixed_t x, y;
	fixed_t fixedwidth, fixedheight;

	UINT8 *source;
	UINT8 *colormap;
	UINT8 *dest;
	const UINT8 *deststop = screens[0] + vid.rowbytes * vid.height;

	size_t count = (ds_x2 - ds_x1 + 1);
	UINT32 val;

	xposition = ds_xfrac; yposition = ds_yfrac;
	xstep = ds_xstep; ystep = ds_ystep;

	source = ds_source;
	colormap = ds_colormap;
	dest = &topleft[ds_y*vid.width + ds_x1];

	fixedwidth = ds_flatwidth << FRACBITS;
	fixedheight = ds_flatheight << FRACBITS;

	// Fix xposition and yposition if they are out of bounds.
	if (xposition < 0)
		xposition = fixedwidth - ((UINT32)(fixedwidth - xposition) % fixedwidth);
	else if (xposition >= fixedwidth)
		xposition %= fixedwidth;
	if (yposition < 0)
		yposition = fixedheight - ((UINT32)(fixedheight - yposition) % fixedheight);
	else if (yposition >= fixedheight)
		yposition %= fixedheight;

#ifdef PS2_OPT_DRAW
	// With bounded steps, unsigned range tests need one correction per axis.
	// Keep the original loop for overflow-sized flats/steps and noncanonical starts.
	if (count >= 32 && fixedwidth > 0 && fixedwidth <= 0x40000000
		&& fixedheight > 0 && fixedheight <= 0x40000000
		&& (UINT32)xposition < (UINT32)fixedwidth
		&& (UINT32)yposition < (UINT32)fixedheight
		&& xstep > -fixedwidth && xstep < fixedwidth
		&& ystep > -fixedheight && ystep < fixedheight)
	{
		const INT32 flatwidth = ds_flatwidth;
		// Retain the blend address; lookup contents remain live pixel reads.
		const UINT8 *const transmap = ds_transmap;
		const fixed_t xwrap = xstep < 0 ? fixedwidth : -fixedwidth;
		const fixed_t ywrap = ystep < 0 ? fixedheight : -fixedheight;
		// Preserve the original inclusive destination bound once per span.
		if (count > (size_t)(deststop - dest) + 1)
			count = (size_t)(deststop - dest) + 1;
		while (count--)
		{
			if ((UINT32)xposition >= (UINT32)fixedwidth)
				xposition += xwrap;
			if ((UINT32)yposition >= (UINT32)fixedheight)
				yposition += ywrap;
			x = (xposition >> FRACBITS);
			y = (yposition >> FRACBITS);
			val = ((y * flatwidth) + x);
			*dest = *(transmap + (colormap[source[val]] << 8) + *dest);
			dest++;
			xposition += xstep;
			yposition += ystep;
		}
		return;
	}
#endif

	while (count-- && dest <= deststop)
	{
		// The loops here keep the texture coordinates within the texture.
		// They will rarely iterate multiple times, and are cheaper than a modulo operation,
		// even if using libdivide.
		if (xstep < 0) // These if statements are hopefully hoisted by the compiler to above this loop
			while (xposition < 0)
				xposition += fixedwidth;
		else
			while (xposition >= fixedwidth)
				xposition -= fixedwidth;
		if (ystep < 0)
			while (yposition < 0)
				yposition += fixedheight;
		else
			while (yposition >= fixedheight)
				yposition -= fixedheight;

		x = (xposition >> FRACBITS);
		y = (yposition >> FRACBITS);
		val = ((y * ds_flatwidth) + x);
		*dest = *(ds_transmap + (colormap[source[val]] << 8) + *dest);
		dest++;
		xposition += xstep;
		yposition += ystep;
	}
}

void R_DrawWaterSpan_NPO2_8(void)
{
	fixed_t xposition;
	fixed_t yposition;
	fixed_t xstep, ystep;
	fixed_t x, y;
	fixed_t fixedwidth, fixedheight;

	UINT8 *source;
	UINT8 *colormap;
	UINT8 *dest;
	UINT8 *dsrc;
	const UINT8 *deststop = screens[0] + vid.rowbytes * vid.height;

	size_t count = (ds_x2 - ds_x1 + 1);

	xposition = ds_xfrac; yposition = (ds_yfrac + ds_waterofs);
	xstep = ds_xstep; ystep = ds_ystep;

	source = ds_source;
	colormap = ds_colormap;
	dest = &topleft[ds_y*vid.width + ds_x1];
	dsrc = screens[1] + (ds_y+ds_bgofs)*vid.width + ds_x1;

	fixedwidth = ds_flatwidth << FRACBITS;
	fixedheight = ds_flatheight << FRACBITS;

	// Fix xposition and yposition if they are out of bounds.
	if (xposition < 0)
		xposition = fixedwidth - ((UINT32)(fixedwidth - xposition) % fixedwidth);
	else if (xposition >= fixedwidth)
		xposition %= fixedwidth;
	if (yposition < 0)
		yposition = fixedheight - ((UINT32)(fixedheight - yposition) % fixedheight);
	else if (yposition >= fixedheight)
		yposition %= fixedheight;

#ifdef PS2_OPT_DRAW
	// With bounded steps, unsigned range tests need one correction per axis.
	// Keep the original loop for overflow-sized flats/steps and noncanonical starts.
	if (count >= 32 && fixedwidth > 0 && fixedwidth <= 0x40000000
		&& fixedheight > 0 && fixedheight <= 0x40000000
		&& (UINT32)xposition < (UINT32)fixedwidth
		&& (UINT32)yposition < (UINT32)fixedheight
		&& xstep > -fixedwidth && xstep < fixedwidth
		&& ystep > -fixedheight && ystep < fixedheight)
	{
		const INT32 flatwidth = ds_flatwidth;
		// Retain the blend address; lookup contents remain live pixel reads.
		const UINT8 *const transmap = ds_transmap;
		const fixed_t xwrap = xstep < 0 ? fixedwidth : -fixedwidth;
		const fixed_t ywrap = ystep < 0 ? fixedheight : -fixedheight;
		// Preserve the original inclusive destination bound once per span.
		if (count > (size_t)(deststop - dest) + 1)
			count = (size_t)(deststop - dest) + 1;
		while (count--)
		{
			if ((UINT32)xposition >= (UINT32)fixedwidth)
				xposition += xwrap;
			if ((UINT32)yposition >= (UINT32)fixedheight)
				yposition += ywrap;
			x = (xposition >> FRACBITS);
			y = (yposition >> FRACBITS);
			*dest++ = colormap[*(transmap + (source[((y * flatwidth) + x)] << 8) + *dsrc++)];
			xposition += xstep;
			yposition += ystep;
		}
		return;
	}
#endif

	while (count-- && dest <= deststop)
	{
		// The loops here keep the texture coordinates within the texture.
		// They will rarely iterate multiple times, and are cheaper than a modulo operation,
		// even if using libdivide.
		if (xstep < 0) // These if statements are hopefully hoisted by the compiler to above this loop
			while (xposition < 0)
				xposition += fixedwidth;
		else
			while (xposition >= fixedwidth)
				xposition -= fixedwidth;
		if (ystep < 0)
			while (yposition < 0)
				yposition += fixedheight;
		else
			while (yposition >= fixedheight)
				yposition -= fixedheight;

		x = (xposition >> FRACBITS);
		y = (yposition >> FRACBITS);
		*dest++ = colormap[*(ds_transmap + (source[((y * ds_flatwidth) + x)] << 8) + *dsrc++)];
		xposition += xstep;
		yposition += ystep;
	}
}

/**	\brief The R_DrawTiltedWaterSpan_NPO2_8 function
	Like DrawTiltedTranslucentSpan_NPO2, but for water
*/
void R_DrawTiltedWaterSpan_NPO2_8(void)
{
	// x1, x2 = ds_x1, ds_x2
	int width = ds_x2 - ds_x1;
	slopereal_t iz, uz, vz;
	UINT32 u, v;
	int i;

	UINT8 *source;
	UINT8 *colormap;
	UINT8 *dest;
	UINT8 *dsrc;

	slopereal_t startz, startu, startv;
	slopereal_t izstep, uzstep, vzstep;
	slopereal_t endz, endu, endv;
	UINT32 stepu, stepv;

#ifdef PS2_OPT_DRAW
	UINT32 x_reciprocal, y_reciprocal;
	R_NPO2SlopeReciprocals(&x_reciprocal, &y_reciprocal);
#else
	struct libdivide_u32_t x_divider = libdivide_u32_gen(ds_flatwidth);
	struct libdivide_u32_t y_divider = libdivide_u32_gen(ds_flatheight);
#endif

	iz = ds_sz.z + ds_sz.y*(centery-ds_y) + ds_sz.x*(ds_x1-centerx);
	uz = ds_su.z + ds_su.y*(centery-ds_y) + ds_su.x*(ds_x1-centerx);
	vz = ds_sv.z + ds_sv.y*(centery-ds_y) + ds_sv.x*(ds_x1-centerx);

	R_CalcSlopeLight();

	dest = &topleft[ds_y*vid.width + ds_x1];
	dsrc = screens[1] + (ds_y+ds_bgofs)*vid.width + ds_x1;
	source = ds_source;
	//colormap = ds_colormap;

#ifdef PS2_OPT_DRAW
	const UINT16 npo2_width = ds_flatwidth, npo2_height = ds_flatheight;
	lighttable_t **const npo2_pzl = planezlight;
	const ptrdiff_t npo2_cmofs = ds_colormap - colormaps;
	INT32 npo2_x = ds_x1;
	const UINT8 *const npo2_transmap = ds_transmap;
#endif

#if 0	// The "perfect" reference version of this routine. Pretty slow.
		// Use it only to see how things are supposed to look.
	i = 0;
	do
	{
		slopereal_t z = 1.f/iz;
		u = (INT64)(uz*z);
		v = (INT64)(vz*z);

		colormap = NPO2_TILT_LIGHT;
		// Lactozilla: Non-powers-of-two
		{
			fixed_t x = (((fixed_t)u) >> FRACBITS);
			fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
			x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
			y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
			// Carefully align all of my Friends.
			if (x < 0)
				x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
			else
				x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
			if (y < 0)
				y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
			else
				y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

			*dest = *(NPO2_TILT_TRANSMAP + (colormap[source[((y * NPO2_TILT_WIDTH) + x)]] << 8) + *dsrc++);
		}
		dest++;
		iz += ds_sz.x;
		uz += ds_su.x;
		vz += ds_sv.x;
	} while (--width >= 0);
#else
	startz = 1.f/iz;
	startu = uz*startz;
	startv = vz*startz;

	izstep = ds_sz.x * SPANSIZE;
	uzstep = ds_su.x * SPANSIZE;
	vzstep = ds_sv.x * SPANSIZE;
	//x1 = 0;
	width++;

	while (width >= SPANSIZE)
	{
		iz += izstep;
		uz += uzstep;
		vz += vzstep;

		endz = 1.f/iz;
		endu = uz*endz;
		endv = vz*endz;
		stepu = SLOPE_U32((endu - startu) * INVSPAN);
		stepv = SLOPE_U32((endv - startv) * INVSPAN);
		u = SLOPE_U32(startu);
		v = SLOPE_U32(startv);

		for (i = SPANSIZE-1; i >= 0; i--)
		{
			colormap = NPO2_TILT_LIGHT;
			// Lactozilla: Non-powers-of-two
			{
				fixed_t x = (((fixed_t)u) >> FRACBITS);
				fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
				x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
				y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
				// Carefully align all of my Friends.
				if (x < 0)
					x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
				else
					x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
				if (y < 0)
					y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
				else
					y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

				*dest = *(NPO2_TILT_TRANSMAP + (colormap[source[((y * NPO2_TILT_WIDTH) + x)]] << 8) + *dsrc++);
			}
			dest++;
			u += stepu;
			v += stepv;
		}
		startu = endu;
		startv = endv;
		width -= SPANSIZE;
	}
	if (width > 0)
	{
		if (width == 1)
		{
			u = SLOPE_U32(startu);
			v = SLOPE_U32(startv);
			colormap = NPO2_TILT_LIGHT;
			// Lactozilla: Non-powers-of-two
			{
				fixed_t x = (((fixed_t)u) >> FRACBITS);
				fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
				x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
				y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
				// Carefully align all of my Friends.
				if (x < 0)
					x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
				else
					x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
				if (y < 0)
					y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
				else
					y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

				*dest = *(NPO2_TILT_TRANSMAP + (colormap[source[((y * NPO2_TILT_WIDTH) + x)]] << 8) + *dsrc++);
			}
		}
		else
		{
			slopereal_t left = width;
			iz += ds_sz.x * left;
			uz += ds_su.x * left;
			vz += ds_sv.x * left;

			endz = 1.f/iz;
			endu = uz*endz;
			endv = vz*endz;
			left = 1.f/left;
			stepu = SLOPE_U32((endu - startu) * left);
			stepv = SLOPE_U32((endv - startv) * left);
			u = SLOPE_U32(startu);
			v = SLOPE_U32(startv);

			for (; width != 0; width--)
			{
				colormap = NPO2_TILT_LIGHT;
				// Lactozilla: Non-powers-of-two
				{
					fixed_t x = (((fixed_t)u) >> FRACBITS);
					fixed_t y = (((fixed_t)v) >> FRACBITS);

#ifdef PS2_OPT_DRAW
					x = R_NPO2SlopeWrap(x, NPO2_TILT_WIDTH, x_reciprocal);
					y = R_NPO2SlopeWrap(y, NPO2_TILT_HEIGHT, y_reciprocal);
#else
					// Carefully align all of my Friends.
					if (x < 0)
						x += (libdivide_u32_do((UINT32)(-x-1), &x_divider) + 1) * NPO2_TILT_WIDTH;
					else
						x -= libdivide_u32_do((UINT32)x, &x_divider) * NPO2_TILT_WIDTH;
					if (y < 0)
						y += (libdivide_u32_do((UINT32)(-y-1), &y_divider) + 1) * NPO2_TILT_HEIGHT;
					else
						y -= libdivide_u32_do((UINT32)y, &y_divider) * NPO2_TILT_HEIGHT;
#endif

					*dest = *(NPO2_TILT_TRANSMAP + (colormap[source[((y * NPO2_TILT_WIDTH) + x)]] << 8) + *dsrc++);
				}
				dest++;
				u += stepu;
				v += stepv;
			}
		}
	}
#endif
#ifdef PS2_OPT_DRAW
	ds_x1 = npo2_x;
#endif
}

#undef NPO2_TILT_WIDTH
#undef NPO2_TILT_HEIGHT
#undef NPO2_TILT_LIGHT
#undef NPO2_TILT_TRANSMAP

#if defined(__GNUC__) || defined(__clang__) // Stop suppressing intentional libdivide compiler warnings
    #pragma GCC diagnostic pop
#endif
