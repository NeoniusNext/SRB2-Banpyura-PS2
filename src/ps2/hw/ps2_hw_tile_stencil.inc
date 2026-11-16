// Z24's unused high byte gates original full-scissor geometry without changing
// its coverage or interpolation. CT32 aliases Z32 blocks with x XOR 32, y XOR 16.
// Only the alpha byte of that alias is writable; the original low 24 depth bits stay intact.
typedef struct
{
	int x0, x1;
	u16 y0, y1;
} tile_mask_rect_t;

static struct
{
	int selected, x0, y0, x1, y1;
	int clear_x0, clear_y0, clear_x1, clear_y1;
	const tile_mask_rect_t *rects; // borrowed through emit_piece and its final OV flush
	int count;
	size_t stride;
	tile_mask_rect_t single;
} tile_stencil_region;

static void tile_stencil_reset_region(void)
{
	memset(&tile_stencil_region, 0, sizeof tile_stencil_region);
}

static void tile_stencil_release_group(void)
{
	tile_stencil_region.selected = 0;
	tile_stencil_region.rects = NULL;
	tile_stencil_region.count = 0;
	tile_stencil_region.stride = 0;
}

static void tile_stencil_state(u32 high)
{
	const int rows = (H.fbh + 31) & ~31;
	gsstate_t s;

	memset(&s, 0, sizeof s);
	s.frame = (u64)(H.z_addr / 8192u) | ((u64)(H.fbw / 64) << 16)
		| ((u64)PSM_CT32 << 24) | (0x00ffffffull << 32);
	s.zbuf = (u64)(H.z_addr / 8192u) | ((u64)ZPSM_32 << 24) | (1ull << 32);
	s.test = (1ull << 16) | (1ull << 17); // Z ALWAYS, alpha and destination-alpha tests disabled
	s.scissor = scissor_reg(0, 0, H.fbw - 1, rows - 1);
	s.rgbaq = rgbaq_of(high << 24);
	state_flush(&s);
}

static void tile_stencil_sprite(int x0, int y0, int x1, int y1)
{
	qw_t *p = pk_alloc(3);

	p[0].d[0] = HWGIF_TAG(2, 1, 1, PRIM_SPRITE, 0, 1);
	p[0].d[1] = GIF_REGS_XYZ; // untextured, unblended context 1, XYZ2 only
	p[1].w[0] = (u32)((2048 + x0) * 16);
	p[1].w[1] = (u32)((2048 + y0) * 16);
	p[1].w[2] = p[1].w[3] = 0;
	p[2].w[0] = (u32)((2048 + x1) * 16);
	p[2].w[1] = (u32)((2048 + y1) * 16);
	p[2].w[2] = p[2].w[3] = 0;
	tile_stencil_mask_sprites++;
	H.gsr.valid &= ~(u32)RV_RGBAQ; // make the next draw's colour/Q explicit after the mask primitive
}

static int tile_stencil_rect(int x0, int y0, int x1, int y1, u32 high)
{
	int x, y;

	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > H.vw) x1 = H.vw;
	if (y1 > H.vh) y1 = H.vh;
	if (x0 >= x1 || y0 >= y1)
		return 0;
	tile_stencil_state(high);
	// Split at each XOR discontinuity; every submitted sprite is contiguous in the CT32 alias.
	for (y = y0; y < y1;)
	{
		int ey = (y | 15) + 1;
		if (ey > y1) ey = y1;
		for (x = x0; x < x1;)
		{
			int ex = (x | 31) + 1;
			const int ax = x ^ 32, ay = y ^ 16;

			if (ex > x1) ex = x1;
			tile_stencil_sprite(ax, ay, ax + ex - x, ay + ey - y);
			x = ex;
		}
		y = ey;
	}
	return 1;
}

static void tile_stencil_begin(int x0, int y0, int x1, int y1)
{
	if (OV.n) ov_flush();
	tile_stencil_active = 0;
	tile_stencil_zor = 0;
	tile_stencil_release_group();
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > H.vw) x1 = H.vw;
	if (y1 > H.vh) y1 = H.vh;
	if (x0 >= x1 || y0 >= y1)
		return;
	// The original GS-vertex bbox contains every possible fragment. Round it
	// outward to whole Z pages: XOR32/XOR16 only permutes pixels within each
	// 64x32 page, so one CT32 sprite clears this same bounded Z rectangle.
	tile_stencil_region.clear_x0 = x0 & ~63;
	tile_stencil_region.clear_y0 = y0 & ~31;
	tile_stencil_region.clear_x1 = (x1 + 63) & ~63;
	tile_stencil_region.clear_y1 = (y1 + 31) & ~31;
	tile_stencil_active = 1;
	tile_stencil_state(4); // leave margin above the NoDepth high byte 1 and GS interpolation endpoints
	tile_stencil_sprite(tile_stencil_region.clear_x0, tile_stencil_region.clear_y0,
		tile_stencil_region.clear_x1, tile_stencil_region.clear_y1);
}

// Each address names an actual tile_mask_rect_t member in the caller's batch.
// All coordinates are copied into ring packets synchronously; DMA never borrows the batch.
static const tile_mask_rect_t *tile_stencil_group_rect(int i)
{
	return (const tile_mask_rect_t *)(const void *)((const u8 *)(const void *)tile_stencil_region.rects
		+ (size_t)i * tile_stencil_region.stride);
}

static void tile_stencil_group_end(void)
{
	int i;

	if (OV.n) ov_flush();
	if (tile_stencil_active && tile_stencil_region.selected)
		for (i = 0; i < tile_stencil_region.count; i++)
		{
			const tile_mask_rect_t *r = tile_stencil_group_rect(i);
			tile_stencil_rect(r->x0, r->y0, r->x1, r->y1, 4);
		}
	tile_stencil_release_group();
	tile_stencil_zor = 0;
}

// Store a stable borrowed batch and count valid rectangles without submitting a mask.
static int tile_stencil_borrow_group(const tile_mask_rect_t *rects, int n, size_t stride)
{
	int i;

	tile_stencil_group_end(); // finish the previous borrow before selecting another source cell
	if (!tile_stencil_active || !rects || n <= 0)
		return 0;
	tile_stencil_region.rects = rects;
	tile_stencil_region.count = n;
	tile_stencil_region.stride = stride;
	tile_stencil_region.selected = 1;
	for (i = 0; i < n; i++)
	{
		const tile_mask_rect_t *r = tile_stencil_group_rect(i);
		if (r->x0 < r->x1 && r->x0 < H.vw && r->x1 > 0
			&& r->y0 < r->y1 && r->y0 < H.vh)
			tile_stencil_rectangles++;
	}
	return 1;
}

// Compatibility API: select immediately, including before a caller-specific draw path.
static inline void tile_stencil_group(const tile_mask_rect_t *rects, int n, size_t stride)
{
	int i;

	if (!tile_stencil_borrow_group(rects, n, stride))
		return;
	for (i = 0; i < n; i++)
	{
		const tile_mask_rect_t *r = tile_stencil_group_rect(i);
		tile_stencil_rect(r->x0, r->y0, r->x1, r->y1, 0);
	}
}

// emit_piece always calls before_pass before emitting vertices. Its first reset
// activates this group; intervening texture IMAGE uploads do not draw to FRAME/Z.
static void tile_stencil_group_deferred(const tile_mask_rect_t *rects, int n, size_t stride)
{
	(void)tile_stencil_borrow_group(rects, n, stride);
}

static inline void tile_stencil_select(int x0, int y0, int x1, int y1)
{
	tile_mask_rect_t *r = &tile_stencil_region.single;

	tile_stencil_group_end(); // release the old single before replacing its owned coordinates
	tile_stencil_region.x0 = x0;
	tile_stencil_region.y0 = y0;
	tile_stencil_region.x1 = x1;
	tile_stencil_region.y1 = y1;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > H.vw) x1 = H.vw;
	if (y1 > H.vh) y1 = H.vh;
	if (x0 >= x1 || y0 >= y1)
		return;
	r->x0 = x0;
	r->x1 = x1;
	r->y0 = (u16)y0;
	r->y1 = (u16)y1;
	tile_stencil_group(r, 1, sizeof *r);
}

// A preceding depth-writing pass can leave high byte 1 inside any selected run.
// Reset the whole group before each pass, retaining that pass's original ALWAYS/GEQUAL choice.
static void tile_stencil_before_pass(int depth_always)
{
	int i;

	if (!tile_stencil_active)
		return;
	if (OV.n) ov_flush();
	if (tile_stencil_region.selected)
		for (i = 0; i < tile_stencil_region.count; i++)
		{
			const tile_mask_rect_t *r = tile_stencil_group_rect(i);
			tile_stencil_rect(r->x0, r->y0, r->x1, r->y1, 0);
		}
	tile_stencil_zor = depth_always ? 0x01000000u : 0;
}

static inline void tile_stencil_unselect(int x0, int y0, int x1, int y1)
{
	if (OV.n) ov_flush();
	tile_stencil_rect(x0, y0, x1, y1, 4);
	tile_stencil_release_group();
}

static void tile_stencil_end(void)
{
	if (!tile_stencil_active) return;
	if (OV.n) ov_flush();
	tile_stencil_state(0);
	tile_stencil_sprite(tile_stencil_region.clear_x0, tile_stencil_region.clear_y0,
		tile_stencil_region.clear_x1, tile_stencil_region.clear_y1);
	tile_stencil_active = 0;
	tile_stencil_zor = 0;
	tile_stencil_release_group();
	// Leave the hardware in an ordinary context1 target/Z24 configuration;
	// future plans still rebuild all registers from their own state.
	{
		gsstate_t s;
		memset(&s, 0, sizeof s);
		s.frame = frame_reg(H.target, 0);
		s.zbuf = zbuf_reg(1);
		s.test = (1ull << 16) | (2ull << 17);
		s.scissor = scissor_current();
		s.rgbaq = rgbaq_of(0x80808080u);
		state_flush(&s);
		qw_t *fp = ad_alloc(1);
		ad_set(fp, 255ull << 56, GSR_FOG);
		H.gsr.valid = 0;
	}
}
