// PS2 GS hardware renderer driver: the frame plan of the batches (OPT9, PS2-HW-34); included by ps2_hwd.c after ps2_hw_draw.inc.
//
// HWR_RenderBatches (hardware/hw_batching.c) shows the driver every collected polygon before the first of them is drawn. For the big map
// textures and flats (the ones that do not fit the pool in numbers: the three 1024 x 1024 cloud planes of GFZ1 alone are 3 MiB, the pool 2.9 MiB)
// the driver measures what the view sees of each polygon: the polygon is transformed and clipped like for drawing, and at every vertex of the
// clipped polygon the screen size of one texel is taken from the plane's mapping (poly_plane / poly_lod, ps2_hw_tex.inc). The coarsest mip
// level at which the largest texel anywhere in the visible part still covers at most one pixel is the level the texture needs: a plane that is
// seen far away, a wall at a grazing angle, the sky - nothing visible is magnified compared with the full size, so the picture shows the same
// detail the PC driver shows. A texture none of whose polygons is visible is not uploaded at all.
// hw_SetTexture stores the texture at that level (tex_want); a texture that is drawn without a plan (not batched) is stored at full size and
// stays so for a while (ps2_full_fr).

static int plan_active;

void PS2HWD_PlanBegin(void)
{
	plan_active = H.up && !(ps2hwd_dbg_flags & HWDBG_NOPLAN);
	if (plan_active)
		xf_update();
}

void PS2HWD_PlanPolygon(GLMipmap_t *m, const void *vp, unsigned int n)
{
	const FOutVector *verts = (const FOutVector *)vp;
	GLMipmap_t *k;
	const float gx = (float)H.guard_x * (1.0f / 1024.0f), gy = (float)H.guard_y * (1.0f / 1024.0f);
	cv_t *v;
	int i, cnt, and_ = 0x3F, or_ = 0, lvl;

	if (!plan_active || !m || n < 3 || n > PS2HWD_MAXPOLY || (m->regen_kind != 1 && m->regen_kind != 2) || m->format != GL_TEXFMT_P_8
		|| (u32)m->width * m->height < PLAN_MIN_TEXELS)
		return;
	k = m->ps2_twin ? m->ps2_twin : m;
	if (k->ps2_planfr != H.frame_no + 1)
	{
		k->ps2_planfr = H.frame_no + 1;
		k->ps2_want = 255;
		k->ps2_vis = 0;
	}
	if (k->ps2_vis && !k->ps2_want)
		return; // PS2-HW-59: a polygon of this texture already needs the full size: the level only goes down to 0 and nothing a later polygon says changes that
	for (i = 0; i < (int)n; i++)
	{
		int oc;

		xform_vertex(&verts[i], NULL, &vbuf_a[i]);
		oc = vbuf_a[i].w <= 0.0f ? 0x3F : outcode(&vbuf_a[i], gx, gy);
		and_ &= oc;
		or_ |= oc;
	}
	if (and_)
		return; // wholly outside the view volume: it draws nothing
	poly_plane(vbuf_a, (int)n, (float)m->width, (float)m->height); // before the clipping reuses the buffer
	v = vbuf_a;
	cnt = (int)n;
	if (or_)
	{
		cnt = clip_poly(vbuf_a, vbuf_b, (int)n, 2, or_, &v);
		if (cnt < 3)
			return;
	}
	lvl = poly_lod(v, cnt);
	k->ps2_vis = 1;
	if (lvl < (int)k->ps2_want)
		k->ps2_want = (u8)lvl;
}

void PS2HWD_PlanEnd(void)
{
	plan_active = 0;
}

// the mip level texture m is stored at when it is made resident now (0 = full size), and whether any polygon of the frame sees it
static u32 tex_want(const GLMipmap_t *m, int *visible)
{
	const GLMipmap_t *k = m->ps2_twin ? m->ps2_twin : m;
	u32 lv;

	*visible = 1;
	if (k->ps2_planfr != H.frame_no + 1 || batch_phase != 2)
		return 0;
	if (k->ps2_full_fr && H.frame_no + 1 - k->ps2_full_fr < PLAN_FULL_KEEP)
		return 0;
	if (!k->ps2_vis)
	{
		*visible = 0;
		return 0;
	}
	lv = k->ps2_want == 255 ? 0 : k->ps2_want;
	// a level must leave at least 16 texels per axis and divide the size
	while (lv > 0 && ((m->width >> lv) < 16 || (m->height >> lv) < 16 || (m->width & ((1u << lv) - 1)) || (m->height & ((1u << lv) - 1))))
		lv--;
	return lv;
}
