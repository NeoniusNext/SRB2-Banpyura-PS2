"""PS2-177 (APPLIED AND VERIFIED BIT-EXACT BY OPT11-CORE AND OPT12-CORE, NO GAIN: 0.5 %, NOT KEPT, see docs/GATES/g1/opt12-CORE.md section 4): two-pass column loop for plain textured wall ranges (docs/GATES/g1/opt10-SW.md, section 3.18).
Run from the repository root: python3 tools/ps2/sw_segloop2.patch.py   (edits src/r_segs.c in place; revert with git checkout src/r_segs.c).
Status: bit-exact (host A/B and EE golden on 4 demos), measured: D1 -0.5 %, D2 -0.1 %, D4 0. Not worth 368 lines in the hottest loop: not merged."""
p = 'src/r_segs.c'
s = open(p).read()

# ---- 1. helpers before R_RenderSegLoopT
anchor = "R_FORCEINLINE void R_RenderSegLoopT(const boolean plain, const INT32 tiers)\n{"
assert s.count(anchor) == 1
helpers = r'''#ifndef PS2_NOOPT_SEGLOOP2
// PS2-177: a textured range without 3D floors, light lists and texture slides is drawn in two passes over its columns instead of one, and the
// parts that only a drawn column needs are out of line. The first pass (R_SegMarkPass) does what depends on the plane/clip arrays of the column
// only: the top and bottom of the wall (yl, yh, kept in two tables), the marking of the floor and ceiling planes, the scale store. The second
// pass (in R_RenderSegLoopT) decides per tier whether anything is drawn and updates the clip arrays; the texture offset, the light and the draw
// setup (R_SegTex1, R_SegTex2, R_SegDraw) run only for the columns that draw (or whose offset a table needs). The columns do not depend on each
// other (every array is indexed by the column, the drawers write the screen only), and the operations of one column keep the original order
// (marking, then the tiers), so the result is the same. What changes is the register pressure: the one-pass loop keeps ~45 values live and
// reloads most of them from the stack in every column (~340 cycles per column against ~70 for the marking alone).
typedef struct
{
	INT32 x0, stop;
	fixed_t scale, tfrac, bfrac; // in: first column; out: after the last column
	fixed_t sstep, tstep, bstep;
	INT16 *cclip, *fclip;
	fixed_t *fscale;
	UINT16 *ctop, *cbot, *ftop, *fbot;
	INT32 *ylb, *yhb;
	boolean domc, domf;
} segmark_t;

typedef struct
{
	INT32 tex1_cx, tex2_cx, invs_cx; // the columns the values below were made for (-1: none)
	fixed_t toff, tcol; // textureoffset, texturecolumn (the middle tier's)
	UINT32 invs; // 0xffffffffu / (unsigned)scale
	angle_t centerangle;
	fixed_t offset, distance;
	boolean needtc, hasextra; // texturecolumn is needed (hasmid || masktc); the front sector has an extra colormap
	fixed_t invx[3]; // rw_inv*texturescalex: middle, top, bottom
	fixed_t off[3]; // added to the texture column: offsetx_, +-offtop_, +-offbot_
	column_t *cols[3];
	INT32 w[3];
	fixed_t texmid[3], texheight[3], scaley[3];
	INT32 mode[3]; // 0 regular, 1 flipped, 2 none
	fixed_t lightresfix;
	lighttable_t **wlights;
	const lighttable_t *cmaps;
	lighttable_t *extracm;
} segdraw_t;

static INT32 seg_ylb[MAXVIDWIDTH + 2], seg_yhb[MAXVIDWIDTH + 2];
static segdraw_t seg_draw;

static void R_SegMarkPass(segmark_t *m)
{
	INT32 x = m->x0;
	const INT32 stop = m->stop;
	fixed_t scale = m->scale, tfrac = m->tfrac, bfrac = m->bfrac;
	const fixed_t sstep = m->sstep, tstep = m->tstep, bstep = m->bstep;
	INT16 *const cclip = m->cclip, *const fclip = m->fclip;
	fixed_t *const fscale = m->fscale;
	UINT16 *const ctop = m->ctop, *const cbot = m->cbot, *const ftop = m->ftop, *const fbot = m->fbot;
	INT32 *const ylb = m->ylb, *const yhb = m->yhb;
	const boolean domc = m->domc, domf = m->domf;

	for (; x < stop; x++, scale += sstep, tfrac += tstep, bfrac += bstep)
	{
		const INT32 cc = cclip[x], fc = fclip[x];
		INT32 yl, yh, top, bottom;

		yl = (tfrac+HEIGHTUNIT-1)>>HEIGHTBITS;
		top = cc+1;
		if (yl < top)
			yl = top;

		if (domc)
		{
			bottom = yl > fc ? fc : yl;
			if (top <= --bottom)
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

		if (domf)
		{
			top = yh < cc ? cc : yh;
			if (++top <= bottom)
			{
				const INT16 t16 = (INT16)top, b16 = (INT16)bottom;
				if (ftop[x] > t16) ftop[x] = t16;
				if (fbot[x] < b16) fbot[x] = b16;
			}
		}

		fscale[x] = scale;
#if defined(PS2_NEGCTL) && PS2_NEGCTL == 17 // negative control of the host A/B: the top of the wall is one pixel low
		ylb[x] = yl + 1;
#else
		ylb[x] = yl;
#endif
		yhb[x] = yh;
	}

	m->scale = scale;
	m->tfrac = tfrac;
	m->bfrac = bfrac;
}

// the texture offset of a column (and the middle tier's texture column), as the one-pass loop made them at the top of every column
static void R_SegTex1(segdraw_t *c, const INT32 cx)
{
#if defined(PS2_NEGCTL) && PS2_NEGCTL == 16 // negative control of the host A/B: the offset is made for the next column
	const angle_t angle = (c->centerangle + xtoviewangle[cx + 1])>>ANGLETOFINESHIFT;
#else
	const angle_t angle = (c->centerangle + xtoviewangle[cx])>>ANGLETOFINESHIFT;
#endif
	const fixed_t to = c->offset - FixedMul(FINETANGENT(angle), c->distance);

	c->toff = to;
	if (c->needtc)
	{
		// FixedDiv(x, FRACUNIT) is x unless |x| >= 2^30 (saturation)
		if (c->invx[0] == FRACUNIT && to > -0x40000000 && to < 0x40000000)
			c->tcol = to;
		else
			c->tcol = FixedDiv(to, c->invx[0]);
	}
	c->tex1_cx = cx;
}

// the light of a column: dc_colormap and dc_x
static void R_SegTex2(segdraw_t *c, const INT32 cx, const fixed_t cscale)
{
	size_t pindex = FixedMul(cscale, c->lightresfix)>>LIGHTSCALESHIFT;

	if (pindex >=  MAXLIGHTSCALE)
		pindex = MAXLIGHTSCALE-1;
	dc_colormap = c->wlights[pindex];
	dc_x = cx;
	if (c->hasextra)
		dc_colormap = c->extracm + (dc_colormap - c->cmaps);
	c->tex2_cx = cx;
}

// draw one tier (0 middle, 1 top, 2 bottom) of a column from dyl to dyh
static void R_SegDraw(const INT32 t, const INT32 cx, const fixed_t cscale, const INT32 dyl, const INT32 dyh)
{
	segdraw_t *const c = &seg_draw;
	fixed_t offset;
	UINT8 *pixels;

	if (c->tex1_cx != cx)
		R_SegTex1(c, cx);
	if (c->tex2_cx != cx)
		R_SegTex2(c, cx, cscale);

	if (t == 0)
		offset = c->tcol + c->off[0];
	else
	{
		fixed_t col;
		// FixedDiv(x, FRACUNIT) is x unless |x| >= 2^30 (saturation)
		if (c->invx[t] == FRACUNIT && c->toff > -0x40000000 && c->toff < 0x40000000)
			col = c->toff;
		else
			col = FixedDiv(c->toff, c->invx[t]);
		offset = col + c->off[t];
	}

	dc_yl = dyl;
	dc_yh = dyh;
	dc_texturemid = c->texmid[t];
	dc_texheight = c->texheight[t];
	if (c->invs_cx != cx)
	{
		c->invs = 0xffffffffu / (unsigned)cscale;
		c->invs_cx = cx;
	}
	dc_iscale = FixedMul((fixed_t)c->invs, c->scaley[t]);
	pixels = SEG_COLUMN(c->cols[t], c->w[t], offset >> FRACBITS)->pixels;
	if (c->mode[t] == 0)
	{
		dc_source = pixels;
		{ PS2SUB_B(60); PS2SUB_ADD(58, dc_yh - dc_yl + 1); colfunc(); PS2SUB_E(60); PS2SUB_CAT(); }
	}
	else if (c->mode[t] == 1)
		R_DrawFlippedPost(pixels, (unsigned)c->texheight[t], colfunc);
}
#endif

'''
s = s.replace(anchor, helpers + anchor, 1)

# ---- 2. the second pass, after the untextured block
anchor2 = '''		rw_x = x;
		rw_scale = scale;
		topfrac = tfrac;
		bottomfrac = bfrac;
		return;
	}

	if (!plain && dc_numlights <= 32)
'''
assert s.count(anchor2) == 1
passB = r'''		rw_x = x;
		rw_scale = scale;
		topfrac = tfrac;
		bottomfrac = bfrac;
		return;
	}

#ifndef PS2_NOOPT_SEGLOOP2
	if (plain && !(topslide_ | botslide_ | midslide_ | midbackslide_))
	{
		// PS2-177: the two-pass loop (see R_SegMarkPass). Without slides the texture offset, the light and the draw setup of a column are needed
		// only by the tiers that draw and by the masked-texture tables, so they are made there (R_SegTex1/2, tagged with the column).
		// dc_x and dc_colormap are set once more after the loop for the last column, as the one-pass loop left them.
		segmark_t mk;
		segdraw_t *const sd = &seg_draw;
		const boolean mkc = markceiling, mkf = markfloor;
		const boolean extra = masktc || thickcol || masktheight || invscale != NULL;
		const fixed_t mth_ = masktheight ? ((curline->linedef->flags & ML_MIDPEG) ? max(midtexturemid_, midtextureback_) : min(midtexturemid_, midtextureback_)) : 0;
		INT32 *const ylb = seg_ylb, *const yhb = seg_yhb;

		mk.x0 = cx; mk.stop = stopx_;
		mk.scale = cscale; mk.tfrac = ctopfrac; mk.bfrac = cbotfrac;
		mk.sstep = sstep_; mk.tstep = tstep_; mk.bstep = bstep_;
		mk.cclip = cclip; mk.fclip = fclip; mk.fscale = fscale;
		mk.ctop = ctop; mk.cbot = cbot; mk.ftop = ftop; mk.fbot = fbot;
		mk.ylb = ylb; mk.yhb = yhb;
		mk.domc = domarkceil; mk.domf = domarkfloor;

		sd->tex1_cx = sd->tex2_cx = sd->invs_cx = -1;
		sd->centerangle = centerangle_;
		sd->offset = offset_;
		sd->distance = distance_;
		sd->needtc = hasmid || masktc;
		sd->hasextra = hasextra;
		sd->invx[0] = invmidx_; sd->invx[1] = invtopx_; sd->invx[2] = invbotx_;
		sd->off[0] = offsetx_;
		sd->off[1] = toppeg_ ? -offtop_ : offtop_;
		sd->off[2] = botpeg_ ? -offbot_ : offbot_;
		sd->cols[0] = midcols; sd->cols[1] = topcols; sd->cols[2] = botcols;
		sd->w[0] = midw; sd->w[1] = topw; sd->w[2] = botw;
		sd->texmid[0] = midtexturemid_; sd->texmid[1] = toptexturemid_; sd->texmid[2] = bottomtexturemid_;
		sd->texheight[0] = midtexheight; sd->texheight[1] = toptexheight; sd->texheight[2] = bottexheight;
		sd->scaley[0] = midscaley; sd->scaley[1] = topscaley; sd->scaley[2] = bottomscaley;
		sd->mode[0] = drawmiddle; sd->mode[1] = drawtop; sd->mode[2] = drawbottom;
		sd->lightresfix = lightresfix;
		sd->wlights = wlights;
		sd->cmaps = cmaps;
		sd->extracm = extracm;

		PS2SUB_CATADD(80, 79);
		PS2SUB_RESET(79);
		R_SegMarkPass(&mk);

		for (; cx < stopx_; cx++)
		{
			const INT32 yl = ylb[cx], yh = yhb[cx];

			// draw the wall tiers
			if (hasmid)
			{
				// single sided line
				if (yl <= yh && yh >= 0 && yl < vheight)
				{
					R_SegDraw(0, cx, cscale, yl, yh);

					// dont draw anything more for this column, since
					// a midtexture blocks the view
					cclip[cx] = (INT16)vheight;
					fclip[cx] = -1;
				}
				else
				{
					// note: don't use min/max macros, since casting from INT32 to INT16 is involved here
					if (mkc)
						cclip[cx] = (yl >= 0) ? ((yl > vheight) ? (INT16)vheight : (INT16)((INT16)yl - 1)) : -1;
					if (mkf)
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

					if (mid >= yl) // back ceiling lower than front ceiling ?
					{
						if (yl >= vheight) // entirely off bottom of screen
							cclip[cx] = (INT16)vheight;
						else if (mid >= 0) // safe to draw top texture
						{
							R_SegDraw(1, cx, cscale, yl, mid);
							cclip[cx] = (INT16)mid;
						}
						else // entirely off top of screen
							cclip[cx] = -1;
					}
					else
						cclip[cx] = topclip;
				}
				else if (mkc) // no top wall
					cclip[cx] = topclip;

				if (hasbot)
				{
					// bottom wall
					mid = (cpixlow+HEIGHTUNIT-1)>>HEIGHTBITS;
					cpixlow += plstep_;

					// no space above wall?
					if (mid <= cclip[cx])
						mid = cclip[cx]+1;

					if (mid <= yh) // back floor higher than front floor ?
					{
						if (yh < 0) // entirely off top of screen
							fclip[cx] = -1;
						else if (mid < vheight) // safe to draw bottom texture
						{
							R_SegDraw(2, cx, cscale, mid, yh);
							fclip[cx] = (INT16)mid;
						}
						else // entirely off bottom of screen
							fclip[cx] = (INT16)vheight;
					}
					else
						fclip[cx] = bottomclip;
				}
				else if (mkf) // no bottom wall
					fclip[cx] = bottomclip;
			}

			if (extra)
			{
				if (masktc || thickcol)
				{
					if (sd->tex1_cx != cx)
						R_SegTex1(sd, cx);
					if (masktc)
						maskedtexturecol[cx] = sd->tcol + offsetx_;
					if (thickcol)
						thicksidecol[cx] = sd->toff;
				}

				if (masktheight)
					maskedtextureheight[cx] = mth_;

				if (invscale)
				{
					if (sd->invs_cx != cx)
					{
						sd->invs = 0xffffffffu / (unsigned)cscale;
						sd->invs_cx = cx;
					}
					invscale[cx] = sd->invs;
				}
			}

			cscale += sstep_;
		}
		if (mk.x0 < stopx_)
			R_SegTex2(sd, stopx_ - 1, cscale - sstep_); // the last column's values of the two drawer globals that the one-pass loop stored for every column
		PS2SUB_CATADD(84, 79);
		rw_x = cx;
		rw_scale = cscale;
		topfrac = mk.tfrac;
		bottomfrac = mk.bfrac;
		pixhigh = cpixhigh;
		pixlow = cpixlow;
		rw_midtexturemid = midtexturemid_;
		rw_midtextureback = midtextureback_;
		rw_toptexturemid = toptexturemid_;
		rw_bottomtexturemid = bottomtexturemid_;
		return;
	}
#endif

	if (!plain && dc_numlights <= 32)
'''
s = s.replace(anchor2, passB, 1)
open(p, 'w').write(s)
