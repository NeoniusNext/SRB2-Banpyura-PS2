// Lossless oversized-source residency. A permanent 64-block image keeps its
// anchor; an immediately preceding free 64-block prefix can be borrowed for a
// wide image and returned before eviction. Queued DMA reads only copied ring
// packets; a source record owns no GS blocks.
#define TILE_SIZE 64
#define TILE_CORE (TILE_SIZE - 2)
#define TILE_MAX_SIZE 128
#define TILE_REGION_WIDE 4

static int tile_general_extent(const texrec_t *r)
{
	return r->psm == PSM_T8 && !r->screen ? TILE_MAX_SIZE : TILE_SIZE;
}

static int tile_wide_dimensions(const texrec_t *r, int *width, int *height)
{
	if (r->psm == PSM_T8 && !r->screen)
	{
		*width = 128; *height = 256;
		return 1;
	}
	if (r->screen && r->psm == PSM_CT32)
	{
		*width = 128; *height = 64;
		return 1;
	}
	*width = *height = 0;
	return 0;
}

static int tile_layout_dimensions(const texrec_t *r, int region, int *width, int *height)
{
	if (region & TILE_REGION_WIDE)
		return tile_wide_dimensions(r, width, height);
	*width = *height = region ? tile_general_extent(r) : TILE_SIZE;
	return 1;
}

static u32 tile_cpu_source_bytes(const GLMipmap_t *m)
{
	size_t pixels = (size_t)m->width * m->height;

#ifdef HWR_PS2_PACKED_PATCHES
	if (HWR_PS2_IsPackedPatch(m))
	{
		pixels = (size_t)m->ps2_uw * m->ps2_uh;
		return (u32)(pixels + (pixels + 7u) / 8u);
	}
#endif
	return (u32)(pixels * (m->format == GL_TEXFMT_RGBA ? 4u
		: m->format == GL_TEXFMT_AP_88 || m->format == GL_TEXFMT_ALPHA_INTENSITY_88 ? 2u : 1u));
}

static int tile_source_record(GLMipmap_t *m, int psm, int clut, int baked,
	int holes, int shareable, int has255, u32 w, u32 h)
{
	void *retained = !m->regen_kind ? m->data : NULL;
	int ri;
	texrec_t *r;

	// Format alone does not identify a discoverable canonical patch recipe.
	// Keep all non-regenerable model/engine sources, including arbitrary AP88.
	// Keep their existing source in place; no second full image is allocated.
	if (retained) HWR_PS2_LockData(retained);
	ri = rec_new();

	if (ri == NOREC)
	{
		tex_fail_why = "tile source record";
		if (retained) HWR_PS2_UnlockData(retained);
		return NOREC;
	}
	r = &H.rec[ri];
	if (m->format == GL_TEXFMT_RGBA && psm == PSM_T8)
	{
		// Indexed RGBA must keep the index chosen at upload time even if the
		// palette later changes. Keep 1 KiB of colour->index history, not a
		// second full source image. pal_build's lowest matching index wins.
		r->tile_palette = malloc(sizeof H.pal);
		if (!r->tile_palette)
		{
			rec_release(ri);
			if (retained) HWR_PS2_UnlockData(retained);
			tex_fail_why = "tile palette history";
			return NOREC;
		}
		memcpy(r->tile_palette, H.pal, sizeof H.pal);
	}
	r->owner = m;
	r->tiled = 1;
	r->tile_source = retained;
	if (retained) m->ps2_cpu |= HWR_PS2_CPU_TILED_SOURCE;
	r->tile_source_bytes = retained ? tile_cpu_source_bytes(m) : 0u;
	r->w = r->uw = (u16)w;
	r->h = r->vh = (u16)h;
	r->sw = m->width;
	r->sh = m->height;
	r->tw = (u8)log2_u32(w);
	r->th = (u8)log2_u32(h);
	r->psm = (u8)psm;
	r->clut = (u8)clut;
	r->wrapx = (m->flags & TF_WRAPX) != 0;
	r->wrapy = (m->flags & TF_WRAPY) != 0;
	r->baked = (u8)baked;
	r->stamp = H.frame_no;
	r->npot_x = (w & (w - 1u)) != 0;
	r->npot_y = (h & (h - 1u)) != 0;
	r->region = (u8)((r->npot_x ? 1 : 0) | (r->npot_y ? 2 : 0));
	r->sxs = (float)m->width / (float)(1u << r->tw);
	r->sys = (float)m->height / (float)(1u << r->th);
	r->shareable = (u8)shareable;
	r->has255 = (u8)has255;
	if (holes && clut == 2 && (m->format == GL_TEXFMT_P_8 || m->format == GL_TEXFMT_AP_88))
		HW_SET_TRANSPARENT(m);
	r->nearest_only = (m->flags & TF_TRANSPARENT) != 0;
	lru_push_head(ri);
	m->downloaded = (UINT32)(ri + 1);
	return ri;
}

static void tile_release_source(texrec_t *r)
{
	// Engine Update/Delete may follow an explicit free, which clears m->data.
	// Never pass that retired pointer back to the zone allocator.
	if (r->owner) r->owner->ps2_cpu &= ~(u8)HWR_PS2_CPU_TILED_SOURCE;
	if (r->tile_source && r->owner && r->owner->data == r->tile_source)
		HWR_PS2_UnlockData(r->tile_source);
	r->tile_source = NULL;
	r->tile_source_bytes = 0;
}

static int tile_cache_rec = NOREC;
static struct
{
	int valid, x, y, wrapx, wrapy, region;
	const void *data;
	const GLMipmap_t *owner;
	u32 frame, palette;
	u8 screen, psm;
} tile_cache;

// Actual allocator occupancy remains in H.used_blocks; planning eligibility
// leaves the pinned streaming image out of the ordinary-texture capacity.
static u32 tex_usable_blocks(void)
{
	if (tile_cache_rec >= 0 && tile_cache_rec < H.rec_n
		&& H.rec[tile_cache_rec].used && H.rec[tile_cache_rec].tiled == 2)
		return H.pool_blocks - 64u;
	return H.pool_blocks;
}

static void tile_invalidate(void)
{
	tile_cache.valid = 0;
}

static int tile_cache_get(void)
{
	u32 blk, blocks = tex_footprint(PSM_CT32, TILE_SIZE, TILE_SIZE, TILE_SIZE / 64);
	texrec_t *r;
	int ri;

	if (tile_cache_rec >= 0 && tile_cache_rec < H.rec_n
		&& H.rec[tile_cache_rec].used && H.rec[tile_cache_rec].tiled == 2)
		return tile_cache_rec;
	tile_invalidate();
	blk = vram_alloc_evicting(blocks, PAGE_BLOCKS, 1);
	if (!blk)
		return NOREC;
	ri = rec_new();
	if (ri == NOREC)
	{
		vram_free(blk, blocks);
		return NOREC;
	}
	r = &H.rec[ri];
	r->blk = (u16)blk;
	r->nblk = (u16)blocks;
	r->pin = 1;
	r->tiled = 2;
	r->w = r->h = r->uw = r->vh = TILE_SIZE;
	r->tw = r->th = (u8)log2_u32(TILE_SIZE);
	r->tbw = TILE_SIZE / 64;
	r->psm = PSM_CT32;
	r->stamp = H.frame_no;
	own_set(ri);
	lru_push_head(ri);
	tile_cache_rec = ri;
	return ri;
}

// The high 64 blocks remain the permanent image, even while its prefix is borrowed.
static u32 tile_cache_borrowed_blocks(void)
{
	if (tile_cache_rec >= 0 && tile_cache_rec < H.rec_n
		&& H.rec[tile_cache_rec].used && H.rec[tile_cache_rec].tiled == 2
		&& H.rec[tile_cache_rec].nblk > 64)
		return (u32)H.rec[tile_cache_rec].nblk - 64u;
	return 0;
}

// Return availability: reuse an existing borrow or claim only the exact free prefix.
// The caller creates the permanent cache first; growing it never searches other holes or evicts.
static int tile_cache_try_borrow(void)
{
	texrec_t *r;
	u32 anchor, prefix, b;
	int i, j;

	if (tile_cache_rec < 0 || tile_cache_rec >= H.rec_n
		|| !H.rec[tile_cache_rec].used || H.rec[tile_cache_rec].tiled != 2)
		return 0;
	r = &H.rec[tile_cache_rec];
	if (r->nblk == 128)
		return 1;
	if (r->nblk != 64)
		return 0;
	anchor = r->blk;
	if ((anchor & (PAGE_BLOCKS - 1u)) || anchor < H.pool_base + 64u
		|| anchor + 64u > H.pool_base + H.pool_blocks || anchor + 64u > VRAM_BLOCKS)
		return 0;
	prefix = anchor - 64u;
	ov_flush_all(); // consume any deferred pass before this image can be overwritten
	for (i = 0; i < H.free_n; i++)
		if (H.free_range[i].start <= prefix
			&& (u32)H.free_range[i].start + H.free_range[i].len == anchor)
		{
			for (b = prefix; b < anchor; b++)
				if (blk_owner[b])
					return 0;
			if (H.free_range[i].len == 64)
			{
				for (j = i; j < H.free_n - 1; j++)
					H.free_range[j] = H.free_range[j + 1];
				H.free_n--;
			}
			else
				H.free_range[i].len -= 64;
			r = &H.rec[tile_cache_rec];
			r->blk = (u16)prefix;
			r->nblk = 128;
			H.used_blocks += 64u;
			own_set(tile_cache_rec);
			tile_invalidate();
			return 1;
		}
	return 0;
}

static int tile_cache_return_borrow(void)
{
	texrec_t *r;
	u32 prefix;

	if (tile_cache_borrowed_blocks() != 64)
		return 0;
	ov_flush_all(); // queue pending readers before the allocator can reuse the prefix
	if (tile_cache_borrowed_blocks() != 64)
		return 0;
	r = &H.rec[tile_cache_rec];
	prefix = r->blk;
	own_clear(prefix, 64);
	r->blk = (u16)(prefix + 64u);
	r->nblk = 64;
	vram_free(prefix, 64);
	tile_invalidate();
	return 1;
}

static int tile_coord(int p, int size, int wrap)
{
	if (wrap)
	{
		p %= size;
		return p < 0 ? p + size : p;
	}
	return p < 0 ? 0 : p >= size ? size - 1 : p;
}

typedef struct
{
	texrec_t source;
	const u8 *data;
	int x, y, wrapx, wrapy, region;
	int sx[TILE_MAX_SIZE];
	struct { u16 sx; u8 dst, count, same; } runs[4];
	u32 run_count;
} tile_fill_t;

// The column mapping is constant for all rows of this synchronous upload.
// Keep it on the upload context stack; cache hits need no preparation.
static void tile_fill_columns(tile_fill_t *c)
{
	int width, height;

	if (!tile_layout_dimensions(&c->source, c->region, &width, &height))
	{
		c->run_count = 0;
		return;
	}
	for (int x = 0; x < width; x++)
	{
		int dx = x - 1;
		if (c->region)
		{
			dx = (x - (c->x & (width - 1)) + width + width / 4) & (width - 1);
			dx -= width / 4;
		}
		c->sx[x] = tile_coord(c->x + dx, c->source.w, c->wrapx);
	}
	c->run_count = 0;
	if (c->source.screen || c->source.psm != PSM_T8 || !c->source.owner
		|| (c->source.owner->format != GL_TEXFMT_P_8 && c->source.owner->format != GL_TEXFMT_ALPHA_8))
		return;
	for (int x = 0; x < width;)
	{
		const int start = x, sx = c->sx[x];
		const int same = x + 1 == width || c->sx[x + 1] != sx + 1;
		const u32 r = c->run_count;

		if (r == 4) { c->run_count = 0; return; }
		x++;
		while (x < width && (same ? c->sx[x] == sx : c->sx[x] == sx + x - start)) x++;
		c->runs[r].sx = (u16)sx;
		c->runs[r].dst = (u8)start;
		c->runs[r].count = (u8)(x - start);
		c->runs[r].same = (u8)same;
		c->run_count++;
	}
}

static void tile_fill(void *ctx, u32 row, u8 *dst)
{
	const tile_fill_t *c = ctx;
	const texrec_t *r = &c->source;
	const GLMipmap_t *m = r->owner;
	int width, height;
	if (!tile_layout_dimensions(r, c->region, &width, &height))
		return;
	int dy = (int)row - 1;
	if (c->region)
	{
		dy = ((int)row - (c->y & (height - 1)) + height + height / 4) & (height - 1);
		dy -= height / 4;
	}
	const int sy = tile_coord(c->y + dy, r->h, c->wrapy);
	int x;

	if (c->run_count)
	{
		const u8 *src = c->data + (size_t)sy * m->width;
		for (u32 i = 0; i < c->run_count; i++)
			if (c->runs[i].same)
				memset(dst + c->runs[i].dst, src[c->runs[i].sx], c->runs[i].count);
			else
				memcpy(dst + c->runs[i].dst, src + c->runs[i].sx, c->runs[i].count);
		return;
	}
	for (x = 0; x < width; x++)
	{
		const int sx = c->sx[x];
		u32 value = 0;
		if (r->screen)
		{
			const size_t at = (size_t)sy * H.fbw + sx;
			if (H.fb32)
				((u32 *)(void *)dst)[x] = ((const u32 *)(const void *)c->data)[at];
			else
				((u16 *)(void *)dst)[x] = ((const u16 *)(const void *)c->data)[at];
			continue;
		}
		switch (m->format)
		{
			case GL_TEXFMT_P_8:
			case GL_TEXFMT_ALPHA_8:
				value = c->data[(size_t)sy * m->width + sx];
				break;
			case GL_TEXFMT_AP_88:
			{
				u16 pixel;
#ifdef HWR_PS2_PACKED_PATCHES
				if (HWR_PS2_IsPackedPatch(m))
					pixel = HWR_PS2_ReadAP88(m, (u32)sx, (u32)sy);
				else
#endif
					pixel = ((const u16 *)(const void *)c->data)[(size_t)sy * m->width + sx];
				if (r->psm == PSM_T8)
					value = (!(m->flags & TF_CHROMAKEYED) && !(pixel >> 8)) ? 255u : pixel & 255u;
				else
					value = H.pal[pixel & 255u] | (a8_to_gs(pixel >> 8) << 24);
				break;
			}
			case GL_TEXFMT_RGBA:
			{
				const u32 pixel = ((const u32 *)(const void *)c->data)[(size_t)sy * m->width + sx];
				if (r->psm == PSM_T8)
				{
					int index;
					value = 255;
					if (pixel >> 24)
						for (index = 0; index < 256; index++)
							if (r->tile_palette[index] == (pixel & 0xFFFFFFu))
							{
								value = (u32)index;
								break;
							}
				}
				else
					value = (pixel & 0xFFFFFFu) | (a8_to_gs(pixel >> 24) << 24);
				break;
			}
			case GL_TEXFMT_ALPHA_INTENSITY_88:
			{
				const u8 *p = c->data + ((size_t)sy * m->width + sx) * 2u;
				value = (u32)p[0] * 0x010101u | (a8_to_gs(p[1]) << 24);
				break;
			}
			default:
				break;
		}
		if (r->psm == PSM_T8)
			dst[x] = (u8)value;
		else
			((u32 *)(void *)dst)[x] = value;
	}
}

// Balance only this synchronous reader. Older queued zero-copy uploads may
// still own the same cache block, including while pk_alloc polls retirement.
static void tile_source_unlock(void *locked, dcent_t *dc_source, int retained)
{
	if (!locked || retained)
		return;
	if (dc_source)
	{
		if (!--dc_source->busy)
			HWR_PS2_UnlockData(locked);
	}
	else
		HWR_PS2_UnlockData(locked);
}

// Source is copied before allocations can move H.rec. Original EE bytes are
// locked only while copying into ring packets; the ring owns every DMA byte.
static int tile_load_impl(const texrec_t *source, int x, int y, int wrapx, int wrapy, int region, texrec_t *out)
{
	tile_fill_t c;
	dcent_t *dc_source = NULL;
	void *locked = NULL;
	int ri, hit, retained = 0;
	u32 bpp;
	int width, height;
	const int wide = (region & TILE_REGION_WIDE) != 0;

	if (!tile_layout_dimensions(source, region, &width, &height)
		|| (wide && tile_cache_borrowed_blocks() != 64))
		return 0;
	const int layout = region | (width == TILE_MAX_SIZE ? 2 : 0) | (height == 256 ? 8 : 0);

	memset(&c, 0, sizeof c);
	c.source = *source;
	c.x = x; c.y = y; c.wrapx = wrapx; c.wrapy = wrapy; c.region = region;
	if (source->screen)
		c.data = H.scr_data[source->screen - 1];
	else if (source->owner)
	{
		GLMipmap_t *m = source->owner;
		c.data = m->data;
		if (!c.data && m->format == GL_TEXFMT_P_8 && (m->regen_kind == 1 || m->regen_kind == 2))
		{
			dc_source = dc_find(dc_key(m), m->width, m->height, 0);
			if (dc_source) c.data = dc_source->p;
		}
		if (!c.data)
		{
			HWR_PS2_RegenerateMipmap(m);
			c.data = m->data;
		}
		locked = (void *)c.data;
		retained = locked && locked == c.source.tile_source;
		if (retained)
		{
			// The proxy owns this persistent lock; this upload only borrows it.
		}
		else if (locked && dc_source)
		{
			if (dc_source->busy == 255)
				dma_fence();
			if (dc_source->busy == 255)
				return 0; // no wrapping a live reference count
			if (!dc_source->busy++)
				HWR_PS2_LockData(locked);
		}
		else if (locked)
			HWR_PS2_LockData(locked);
	}
	if (!c.data)
		return 0;
	ri = wide ? tile_cache_rec : tile_cache_get();
	if (ri == NOREC || (wide && tile_cache_borrowed_blocks() != 64))
	{
		tile_source_unlock(locked, dc_source, retained);
		return 0;
	}
	hit = tile_cache.valid && tile_cache.frame == H.frame_no
		&& tile_cache.owner == c.source.owner && tile_cache.screen == c.source.screen
		&& tile_cache.data == c.data
		&& tile_cache.palette == H.pal_gen && tile_cache.psm == c.source.psm
		&& tile_cache.x == x && tile_cache.y == y
		&& tile_cache.wrapx == wrapx && tile_cache.wrapy == wrapy && tile_cache.region == layout;
	*out = c.source;
	const u32 anchor = (u32)H.rec[ri].blk + H.rec[ri].nblk - 64u;
	out->blk = wide ? H.rec[ri].blk : (u16)anchor;
	out->nblk = wide ? 128 : 64;
	out->tiled = 0;
	out->w = out->uw = (u16)width;
	out->h = out->vh = (u16)height;
	out->tw = (u8)log2_u32((u32)width);
	out->th = (u8)log2_u32((u32)height);
	out->tbw = out->psm == PSM_T8 ? 2 : width / 64;
	out->region = 3;
	out->sxs = out->sys = 1.0f;
	out->npot_x = out->npot_y = out->wrapx = out->wrapy = 0;
	if (!hit)
	{
		tile_fill_columns(&c);
		bpp = c.source.psm == PSM_T8 ? 1u : c.source.screen && !H.fb32 ? 2u : 4u;
		upload_bands(out->blk, out->tbw, out->psm, (u32)width, (u32)height, bpp, tile_fill, &c);
		tile_cache.valid = 1;
		tile_cache.owner = c.source.owner;
		tile_cache.screen = c.source.screen;
		tile_cache.data = c.data;
		tile_cache.palette = H.pal_gen;
		tile_cache.frame = H.frame_no;
		tile_cache.psm = c.source.psm;
		tile_cache.x = x; tile_cache.y = y;
		tile_cache.wrapx = wrapx; tile_cache.wrapy = wrapy; tile_cache.region = layout;
		H.st.uploads++;
		H.st.upload_bytes += (u32)width * (u32)height * bpp;
	}
	tile_source_unlock(locked, dc_source, retained);
	return 1;
}

static int tile_load(const texrec_t *source, int x, int y, int wrapx, int wrapy, texrec_t *out)
{
	return tile_load_impl(source, x, y, wrapx, wrapy, 0, out);
}

static int tile_load_region(const texrec_t *source, int x, int y, int wrapx, int wrapy, texrec_t *out)
{
	return tile_load_impl(source, x, y, wrapx, wrapy, 1, out);
}

static int tile_load_wide(const texrec_t *source, int x, int y, int wrapx, int wrapy, texrec_t *out)
{
	return tile_load_impl(source, x, y, wrapx, wrapy, 1 | TILE_REGION_WIDE, out);
}
