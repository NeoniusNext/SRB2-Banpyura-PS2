// PS2 GS hardware renderer driver: VRAM pool, texture records, conversion and upload (included by ps2_hwd.c).
//
// Palette indexed engine data (P_8, AP_88, ALPHA_8, and RGBA whose colours all come from the current palette) lives in the
// pool as PSMT8 plus a CLUT; every other texture is CT32 with the GS alpha scale (0x80 = 1.0). Textures keep their exact
// size (up to the GS limit of 1024 texels); a repeat of a size that is not a power of two is made by cutting the polygon
// at whole texture widths (ps2_hw_draw.inc), so no texel is ever resampled. A texture handle is its index in the record
// table + 1, stored in GLMipmap_t.downloaded.

// ---- VRAM pool: free list of block ranges sorted by address ----

#define WIPE_DEAD_FRAMES 12 // frames a wipe image may stay unused before pool pressure may take it

static void ov_flush_all(void);
static int scr_spill_victim(void);

static u32 vram_alloc(u32 n, u32 align)
{
	int i;

	for (i = 0; i < H.free_n; i++)
	{
		u32 fs = H.free_start[i], fe = fs + H.free_len[i];
		u32 s = (fs + align - 1) & ~(align - 1);

		if (s + n <= fe)
		{
			u32 tail = fe - (s + n);

			// A full list cannot represent a split: try another range, never lose blocks.
			if (s > fs && tail && H.free_n == VRAM_RANGES)
				continue;

			if (s > fs && tail && H.free_n < VRAM_RANGES)
			{
				int j;

				for (j = H.free_n; j > i + 1; j--)
				{
					H.free_start[j] = H.free_start[j - 1];
					H.free_len[j] = H.free_len[j - 1];
				}
				H.free_n++;
				H.free_len[i] = s - fs;
				H.free_start[i + 1] = s + n;
				H.free_len[i + 1] = tail;
			}
			else if (s > fs)
			{
				H.free_len[i] = s - fs;
			}
			else if (tail)
			{
				H.free_start[i] = s + n;
				H.free_len[i] = tail;
			}
			else
			{
				int j;

				for (j = i; j < H.free_n - 1; j++)
				{
					H.free_start[j] = H.free_start[j + 1];
					H.free_len[j] = H.free_len[j + 1];
				}
				H.free_n--;
			}
			H.used_blocks += n;
			return s;
		}
	}
	return 0;
}

// the same from the top of the pool: screen captures live there, so they do not cut the pool into pieces too small for a big texture
static u32 vram_alloc_hi(u32 n, u32 align)
{
	int i;

	for (i = H.free_n - 1; i >= 0; i--)
	{
		u32 fs = H.free_start[i], fe = fs + H.free_len[i];

		if (fe >= n && fe - n >= fs)
		{
			u32 s = (fe - n) & ~(align - 1);

			if (s >= fs)
			{
				u32 tail = fe - (s + n);

				if (s > fs && tail && H.free_n == VRAM_RANGES)
					continue;
				if (s > fs && tail)
				{
					int j;

					for (j = H.free_n; j > i + 1; j--)
					{
						H.free_start[j] = H.free_start[j - 1];
						H.free_len[j] = H.free_len[j - 1];
					}
					H.free_n++;
					H.free_len[i] = s - fs;
					H.free_start[i + 1] = s + n;
					H.free_len[i + 1] = tail;
				}
				else if (s > fs)
				{
					H.free_len[i] = s - fs;
				}
				else if (tail)
				{
					H.free_start[i] = s + n;
					H.free_len[i] = tail;
				}
				else
				{
					int j;

					for (j = i; j < H.free_n - 1; j++)
					{
						H.free_start[j] = H.free_start[j + 1];
						H.free_len[j] = H.free_len[j + 1];
					}
					H.free_n--;
				}
				H.used_blocks += n;
				return s;
			}
		}
	}
	return 0;
}

static void vram_free(u32 s, u32 n)
{
	int i, j;

	H.used_blocks -= n;
	for (i = 0; i < H.free_n && H.free_start[i] < s; i++)
		;
	// merge with the previous range
	if (i > 0 && H.free_start[i - 1] + H.free_len[i - 1] == s)
	{
		H.free_len[i - 1] += n;
		if (i < H.free_n && H.free_start[i - 1] + H.free_len[i - 1] == H.free_start[i])
		{
			H.free_len[i - 1] += H.free_len[i];
			for (j = i; j < H.free_n - 1; j++)
			{
				H.free_start[j] = H.free_start[j + 1];
				H.free_len[j] = H.free_len[j + 1];
			}
			H.free_n--;
		}
		return;
	}
	if (i < H.free_n && s + n == H.free_start[i])
	{
		H.free_start[i] = s;
		H.free_len[i] += n;
		return;
	}
	if (H.free_n >= VRAM_RANGES)
	{
		CONS_Alert(CONS_ERROR, "PS2 GS: VRAM free-list capacity exhausted\n");
		abort();
	}
	for (j = H.free_n; j > i; j--)
	{
		H.free_start[j] = H.free_start[j - 1];
		H.free_len[j] = H.free_len[j - 1];
	}
	H.free_start[i] = s;
	H.free_len[i] = n;
	H.free_n++;
}

// ---- footprint of a texture buffer in 256-byte blocks (GS page/block layout) ----

static const u8 blk_32_8[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23}, {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
static const u8 blk_16[8][4] = {{0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15}, {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
static const u8 blk_16s[8][4] = {{0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27}, {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};

static u32 tex_footprint(int psm, u32 w, u32 h, u32 tbw)
{
	u32 pw, ph, bw, bh, ppr, pcols, prows, lastpage, lw, lh, nbx, nby, bx, by, maxblk = 0;

	switch (psm)
	{
		case PSM_T8:
			pw = 128; ph = 64; bw = 16; bh = 16;
			break;
		case PSM_CT32:
			pw = 64; ph = 32; bw = 8; bh = 8;
			break;
		default: // CT16, CT16S
			pw = 64; ph = 64; bw = 16; bh = 8;
			break;
	}
	ppr = (tbw * 64 + pw - 1) / pw;
	pcols = (w + pw - 1) / pw;
	prows = (h + ph - 1) / ph;
	lastpage = (prows - 1) * ppr + (pcols - 1);
	lw = w - (pcols - 1) * pw;
	lh = h - (prows - 1) * ph;
	nbx = (lw + bw - 1) / bw;
	nby = (lh + bh - 1) / bh;
	for (by = 0; by < nby; by++)
		for (bx = 0; bx < nbx; bx++)
		{
			u32 b = (psm == PSM_CT16) ? blk_16[by][bx] : (psm == PSM_CT16S) ? blk_16s[by][bx] : blk_32_8[by][bx];

			if (b > maxblk)
				maxblk = b;
		}
	return lastpage * PAGE_BLOCKS + maxblk + 1;
}

// ---- record table and LRU ----

static void lru_unlink(int i)
{
	texrec_t *r = &H.rec[i];

	if (r->prev != NOREC)
		H.rec[r->prev].next = r->next;
	else if (H.lru_head == i)
		H.lru_head = r->next;
	if (r->next != NOREC)
		H.rec[r->next].prev = r->prev;
	else if (H.lru_tail == i)
		H.lru_tail = r->prev;
	r->prev = r->next = NOREC;
}

static void lru_push_head(int i)
{
	texrec_t *r = &H.rec[i];

	r->prev = NOREC;
	r->seq = ++H.lru_seq;
	r->next = H.lru_head;
	if (H.lru_head != NOREC)
		H.rec[H.lru_head].prev = i;
	H.lru_head = i;
	if (H.lru_tail == NOREC)
		H.lru_tail = i;
}

static int rec_new(void)
{
	int i;

	if (H.rec_free != NOREC)
	{
		i = H.rec_free;
		H.rec_free = H.rec[i].next;
	}
	else
	{
		if (H.rec_n == H.rec_cap)
		{
			int cap = H.rec_cap ? H.rec_cap * 2 : 512;
			texrec_t *n = realloc(H.rec, (size_t)cap * sizeof(texrec_t));

			if (!n)
				return NOREC;
			H.rec = n;
			H.rec_cap = cap;
		}
		i = H.rec_n++;
	}
	memset(&H.rec[i], 0, sizeof(texrec_t));
	H.rec[i].used = 1;
	H.rec[i].prev = H.rec[i].next = NOREC;
	return i;
}

static void rec_release(int i)
{
	lru_unlink(i);
	if (H.cur_tex == i)
		H.cur_tex = NOREC;
	H.rec[i].used = 0;
	H.rec[i].next = H.rec_free;
	H.rec_free = i;
}

static void own_clear(u32 blk, u32 n);

// drop a texture from VRAM; the engine finds `downloaded` cleared and regenerates/uploads it when it needs it again
static void tex_drop(int i, int count_eviction)
{
	texrec_t *r = &H.rec[i];

	if (r->owner)
		r->owner->downloaded = 0;
	if (r->screen)
		H.scr_rec[r->screen - 1] = NOREC;
	own_clear(r->blk, r->nblk);
	vram_free(r->blk, r->nblk);
	if (count_eviction)
		H.st.evictions++;
	rec_release(i);
}

// The engine batches the polygons of the BSP walk (hardware/hw_batching.c): 1 = collecting (nothing is drawn, textures are only
// noted), 2 = the collected batches are being drawn (textures are made resident when their batch comes), 0 = not batching
// (draws follow the selection of their texture immediately).
static int batch_phase;

void PS2HWD_BatchBegin(void)
{
	batch_phase = 1;
}

void PS2HWD_BatchDraw(void)
{
	batch_phase = 2;
}

void PS2HWD_BatchEnd(void)
{
	batch_phase = 0;
}

// ---- eviction by window ----
// Block ownership: blk_owner[b] = record + 1 for the 256-byte blocks of resident textures (0 = free or not managed here).
// When no free hole is big enough, the cheapest aligned window of the pool is emptied: its cost is the sum of the per-block
// weights of the records it overlaps (older = cheaper), and a window that touches a record that must stay is not a candidate.
// A request that no window can satisfy fails WITHOUT evicting anything (a failed 1 MiB sky must not flush the working set).
static u16 blk_owner[VRAM_BLOCKS];

static void own_set(int ri)
{
	const texrec_t *r = &H.rec[ri];
	u32 b;

	for (b = r->blk; b < r->blk + r->nblk && b < VRAM_BLOCKS; b++)
		blk_owner[b] = (u16)(ri + 1);
}

static void own_clear(u32 blk, u32 n)
{
	u32 b;

	for (b = blk; b < blk + n && b < VRAM_BLOCKS; b++)
		blk_owner[b] = 0;
}

#define WEIGHT_STAY 0xFFFFFFFFu
#define NOWIN 0xFFFFFFFFu

// Cost of evicting record r per block (WEIGHT_STAY: it cannot be evicted now): 1 for the least recently used texture up to 64
// for the most recent one.
static inline u32 evict_weight(const texrec_t *r, int i, u32 seq_lo, u32 shift)
{
	if (r->pin)
		return WEIGHT_STAY;
	if (r->screen)
	{
		// a wipe image that no wipe has drawn for a while is dead (the engine captures both again before every wipe)
		if (r->screen <= 2 && H.frame_no - r->stamp > WIPE_DEAD_FRAMES)
			return 1;
		return WEIGHT_STAY; // other captures cannot be regenerated from engine data
	}
	if (i == H.cur_tex)
		return WEIGHT_STAY; // the texture being drawn
	if (r->stamp == H.frame_no)
	{
		if (batch_phase == 1)
			return WEIGHT_STAY;
		if (batch_phase == 2)
			return r->done == H.frame_no + 1 ? 40u : 60u; // drawn already (it is needed again next frame) / still to be drawn this frame
		return 64u; // not batching: its draws are already in the stream
	}
	return 1u + ((r->seq - seq_lo) >> shift); // 1 (least recently used) .. 31
}

static void weights_prepare(void)
{
	u32 lo = 0, span = 1, shift = 0, i;

	if (H.lru_tail != NOREC && H.lru_head != NOREC)
	{
		lo = H.rec[H.lru_tail].seq;
		span = H.rec[H.lru_head].seq - lo + 1;
	}
	while ((span >> shift) > 30)
		shift++;
	for (i = 0; i < (u32)H.rec_n; i++)
		if (H.rec[i].used)
			H.rec[i].wgt = evict_weight(&H.rec[i], (int)i, lo, shift);
}

static inline u32 block_weight(u32 b)
{
	u16 o = blk_owner[b];

	return o ? H.rec[o - 1].wgt : 0;
}

// start block of the cheapest window of n blocks (aligned), or NOWIN
static u32 best_window(u32 n, u32 align)
{
	u32 lo = H.pool_base, hi = H.pool_base + H.pool_blocks, s, e, best = NOWIN, best_cost = 0xFFFFFFFFu, bad = 0, cost = 0;
	u32 first = (lo + align - 1) & ~(align - 1);

	weights_prepare();
	if (n > hi - lo || first + n > hi)
		return NOWIN;
	for (e = first; e < first + n; e++)
	{
		u32 w = block_weight(e);

		if (w == WEIGHT_STAY)
			bad++;
		else
			cost += w;
	}
	for (s = first;;)
	{
		if (!bad && ((ps2hwd_dbg_flags & 32) ? best == NOWIN : cost <= best_cost)) /* flag 32: negative control of the host test, first feasible window */
		{
			best = s;
			best_cost = cost;
		}
		if (s + align + n > hi)
			break;
		for (e = s; e < s + align; e++) // leaves the window
		{
			u32 w = block_weight(e);

			if (w == WEIGHT_STAY)
				bad--;
			else
				cost -= w;
		}
		for (e = s + n; e < s + n + align; e++) // enters the window
		{
			u32 w = block_weight(e);

			if (w == WEIGHT_STAY)
				bad++;
			else
				cost += w;
		}
		s += align;
	}
	return best;
}

// The texture to drop for a small request: the least recently used one that is not needed in this frame, then one that a batch has
// drawn already (it is needed again next frame), then one still to be drawn. NOREC when everything left must stay.
static int small_victim(void)
{
	int v, pass;

	for (pass = 0; pass < 3; pass++)
		for (v = H.lru_tail; v != NOREC; v = H.rec[v].prev)
		{
			u32 w = evict_weight(&H.rec[v], v, 0, 31); // seq 0, shift 31: every texture of an earlier frame weighs 1

			if (w != WEIGHT_STAY && (pass == 0 ? w < 40u : pass == 1 ? w == 40u : 1))
				return v;
		}
	return NOREC;
}

#define SMALL_REQUEST 128 // blocks: up to this size the least recently used textures are dropped one by one (cheap), above it a window is searched

static u32 vram_alloc_evicting(u32 n, u32 align, int hi)
{
	u32 b, w, e;
	int tries;

	if (!n || n > H.pool_blocks)
		return 0;
	for (tries = 0; tries < 64; tries++)
	{
		b = hi ? vram_alloc_hi(n, align) : vram_alloc(n, align);
		if (b)
			return b;
		if (n <= SMALL_REQUEST && !(ps2hwd_dbg_flags & 32))
		{
			int v = small_victim();

			if (v != NOREC)
			{
				tex_drop(v, 1);
				continue;
			}
		}
		w = best_window(n, align);
		if (w == NOWIN)
		{
			if (scr_spill_victim()) // a capture can still leave the pool through the EE RAM backing
				continue;
			return 0;
		}
		for (e = w; e < w + n;)
		{
			u16 o = blk_owner[e];

			if (!o)
			{
				e++;
				continue;
			}
			e = H.rec[o - 1].blk + H.rec[o - 1].nblk; // the whole record goes
			tex_drop(o - 1, 1);
		}
	}
	return 0;
}

static inline texrec_t *rec_of(const GLMipmap_t *m)
{
	UINT32 h = m->downloaded;

	if (!h || h > (UINT32)H.rec_n)
		return NULL;
	if (!H.rec[h - 1].used || H.rec[h - 1].owner != m)
		return NULL;
	return &H.rec[h - 1];
}

// ---- palette, CLUT images, reverse palette lookup ----

static u32 pal_key[1024]; // 0 = empty, else rgb24 | 1u << 24
static u8 pal_val[1024];

// engine alpha 0..255 -> GS alpha 0..0x80 (0x80 = 1.0), rounded
static inline u32 a8_to_gs(u32 a)
{
	return (a * 128u + 127u) / 255u;
}

static inline u32 pal_hash(u32 k)
{
	return (k * 2654435761u) >> 22;
}

static int pal_lookup(u32 rgb) // -1 when the colour is not in the palette
{
	u32 h = pal_hash(rgb), key = rgb | (1u << 24);

	for (;;)
	{
		if (pal_key[h] == key)
			return pal_val[h];
		if (!pal_key[h])
			return -1;
		h = (h + 1) & 1023;
	}
}

// every palette colour -> its lowest index (index 255 included: opaque texels of that colour keep it)
static void pal_build(void)
{
	int i;

	memset(pal_key, 0, sizeof pal_key);
	for (i = 255; i >= 0; i--) // the lowest index wins a colour that occurs twice
	{
		u32 h = pal_hash(H.pal[i]), key = H.pal[i] | (1u << 24);

		for (;;)
		{
			if (!pal_key[h] || pal_key[h] == key)
			{
				pal_key[h] = key;
				pal_val[h] = (u8)i;
				break;
			}
			h = (h + 1) & 1023;
		}
	}
}

static inline u32 clut_slot(u32 i) // CT32 CLUT in CSM1: bits 3 and 4 of the index are swapped
{
	if (ps2hwd_dbg_flags & 16)
		return i; // negative control of tools/ps2/hw_test.c
	return (i & ~0x18u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1);
}

// 16x16 CT32 image into VRAM (the CLUT format of a PSMT8 CSM1 CLUT)
static void clut_write(u32 blk, const u32 *entries)
{
	qw_t *h = pk_alloc(1 + 4 + 1 + 64);
	qw_t *d;
	int i;

	H.serial++;

	h[0].d[0] = HWGIF_TAG(4, 0, 0, 0, 0, 1);
	h[0].d[1] = GIF_REGS_AD;
	ad_set(&h[1], ((u64)blk << 32) | (1ull << 48) | ((u64)PSM_CT32 << 56), GSR_BITBLTBUF);
	ad_set(&h[2], 0, GSR_TRXPOS);
	ad_set(&h[3], 16ull | (16ull << 32), GSR_TRXREG);
	ad_set(&h[4], 0, GSR_TRXDIR);
	h[5].d[0] = HWGIF_TAG(64, 1, 0, 0, 2, 0);
	h[5].d[1] = 0;
	d = &h[6];
	for (i = 0; i < 64; i++)
	{
		d[i].w[0] = entries[i * 4];
		d[i].w[1] = entries[i * 4 + 1];
		d[i].w[2] = entries[i * 4 + 2];
		d[i].w[3] = entries[i * 4 + 3];
	}
	if (blk == H.clut_loaded)
		H.clut_loaded = 0; // the GS holds a copy of the old contents
	H.st.clut_uploads++;
}

enum
{
	CK_OPAQUE = 1, // palette, every index opaque
	CK_KEYED, // palette, index 255 transparent
	CK_ALPHA, // white, alpha = index (ALPHA_8 masks)
	CK_WHITEKEY, // white, index 255 transparent (a mask of a keyed texture)
	CK_TINT, // PC tint shader equation applied to the palette: param = tint RGBA (colormap), flag = keyed
	CK_MULT, // alpha = one channel of the palette colour (multiplicative blend, one pass per channel): param = channel
	CK_RAMP, // darkness / fog ramps (ps2_hw_draw.inc), built from a rampspec_t
	CK_WIPE, // tinted wipe, one colour channel: param = channel | fade-in << 2 | polygon colour << 8
};

// the 256 entries of a CLUT image in index order (0x80 alpha = opaque); the caller swizzles
static void clut_entries(u32 kind, u32 param, u32 keyed, u32 *e)
{
	int i;

	switch (kind)
	{
		case CK_OPAQUE:
		case CK_KEYED:
			for (i = 0; i < 256; i++)
				e[i] = H.pal[i] | (0x80u << 24);
			if (kind == CK_KEYED)
				e[255] = H.pal[255];
			break;
		case CK_ALPHA:
			for (i = 0; i < 256; i++)
				e[i] = 0x00FFFFFFu | (a8_to_gs((u32)i) << 24);
			break;
		case CK_WHITEKEY:
			for (i = 0; i < 256; i++)
				e[i] = 0x00FFFFFFu | (0x80u << 24);
			e[255] = 0x00FFFFFFu;
			break;
		case CK_TINT:
		{
			// GLSL_SOFTWARE_TINT_EQUATION on the colour of every palette entry (poly_color is white):
			// bright = |rgb|, strength = sqrt(tint.a), out = clamp(bright * tint.rgb * strength + rgb * (1 - strength))
			float tr = (float)(param & 255) * (1.0f / 255.0f), tg = (float)((param >> 8) & 255) * (1.0f / 255.0f);
			float tb = (float)((param >> 16) & 255) * (1.0f / 255.0f), ta = (float)(param >> 24) * (1.0f / 255.0f);
			float st = sqrtf(ta);

			for (i = 0; i < 256; i++)
			{
				float r = (float)(H.pal[i] & 255) * (1.0f / 255.0f), g = (float)((H.pal[i] >> 8) & 255) * (1.0f / 255.0f), b = (float)((H.pal[i] >> 16) & 255) * (1.0f / 255.0f);
				float bright = sqrtf(r * r + g * g + b * b), o[3];
				int k;

				o[0] = bright * (tr * st) + r * (1.0f - st);
				o[1] = bright * (tg * st) + g * (1.0f - st);
				o[2] = bright * (tb * st) + b * (1.0f - st);
				for (k = 0; k < 3; k++)
					o[k] = o[k] < 0.0f ? 0.0f : o[k] > 1.0f ? 1.0f : o[k];
				e[i] = (u32)(o[0] * 255.0f + 0.5f) | ((u32)(o[1] * 255.0f + 0.5f) << 8) | ((u32)(o[2] * 255.0f + 0.5f) << 16) | (0x80u << 24);
			}
			if (keyed)
				e[255] &= 0x00FFFFFFu;
			break;
		}
		case CK_WIPE:
		{
			// GLSL_UI_TINTED_WIPE_FRAGMENT_SHADER: out = clamp(poly * 24.875 * m'), m' = fade-in ? 1 - m : m, m = mask alpha;
			// the GS adds / subtracts (colour * alpha): colour = min(255, K) and the alpha carries the rest
			float K = (float)((param >> 8) & 255) * 24.875f, Kc = K > 255.0f ? 255.0f : K;
			int fadein = (int)((param >> 2) & 1);

			for (i = 0; i < 256; i++)
			{
				float m = (float)i * (1.0f / 255.0f), mp = fadein ? 1.0f - m : m, v = K * mp, a = Kc > 0.0f ? (v >= Kc ? 1.0f : v / Kc) : 0.0f;

				e[i] = (u32)(Kc + 0.5f) * 0x010101u | ((u32)(a * 128.0f + 0.5f) << 24);
			}
			break;
		}
		case CK_MULT:
			for (i = 0; i < 256; i++)
			{
				u32 channel = (H.pal[i] >> (8 * (param & 3))) & 255u;
				u32 modulation = (param >> 8) & 255u;
				// Quantize the final factor once, rather than separately rounding texture and polygon channels.
				e[i] = 0x00FFFFFFu | (((channel * modulation * 128u + 32512u) / 65025u) << 24);
			}
			// a hole must leave the destination as it is: multiply by 1.0
			if (keyed)
				e[255] = 0x00FFFFFFu | (0x80u << 24);
			break;
	}
}

static int clut_palette_dependent(u32 kind)
{
	return kind == CK_OPAQUE || kind == CK_KEYED || kind == CK_TINT || kind == CK_MULT;
}

static void clut_build_into(u32 blk, u32 kind, u32 param, u32 keyed)
{
	static u32 e[256] __attribute__((aligned(64)));
	static u32 sw[256] __attribute__((aligned(64)));
	int i;

	clut_entries(kind, param, keyed, e);
	for (i = 0; i < 256; i++)
		sw[clut_slot((u32)i)] = e[i];
	clut_write(blk, sw);
}

static inline u32 clut_fixed_blk(u32 kind) // kind 1..4
{
	return H.clut_base + (kind - 1) * CLUT_BLOCKS;
}

// the four fixed images (palette variants are rebuilt when the palette changed)
static void clut_fixed_refresh(int all)
{
	qw_t *p;

	if (all || (H.clut_ent[0].gen != H.pal_gen))
	{
		clut_build_into(clut_fixed_blk(CK_OPAQUE), CK_OPAQUE, 0, 0);
		clut_build_into(clut_fixed_blk(CK_KEYED), CK_KEYED, 0, 0);
		H.clut_ent[0].gen = H.pal_gen;
	}
	if (all)
	{
		clut_build_into(clut_fixed_blk(CK_ALPHA), CK_ALPHA, 0, 0);
		clut_build_into(clut_fixed_blk(CK_WHITEKEY), CK_WHITEKEY, 0, 0);
	}
	p = ad_alloc(1);
	ad_set(p, 0, GSR_TEXFLUSH);
	H.clut_loaded = 0;
}

// the block of a CLUT variant, built (and uploaded) when it is not cached
static u32 clut_get(u32 kind, u32 param, u32 keyed)
{
	u64 key = (u64)kind | ((u64)(keyed ? 1 : 0) << 8) | ((u64)param << 16);
	int i, freeslot = -1, lru = -1, victim;

	if (kind <= CK_WHITEKEY)
	{
		if (clut_palette_dependent(kind) && H.clut_ent[0].gen != H.pal_gen)
			clut_fixed_refresh(0);
		return clut_fixed_blk(kind);
	}
	H.clut_tick++;
	for (i = CLUT_FIXED; i < CLUT_SLOTS; i++)
	{
		if (H.clut_ent[i].key == key)
		{
			if (clut_palette_dependent(kind) && H.clut_ent[i].gen != H.pal_gen)
			{
				clut_build_into(H.clut_base + (u32)i * CLUT_BLOCKS, kind, param, keyed);
				H.clut_ent[i].gen = H.pal_gen;
			}
			H.clut_ent[i].stamp = H.clut_tick;
			return H.clut_base + (u32)i * CLUT_BLOCKS;
		}
		if (!H.clut_ent[i].key)
		{
			if (freeslot < 0)
				freeslot = i;
		}
		else if (lru < 0 || H.clut_ent[i].stamp < H.clut_ent[lru].stamp)
		{
			lru = i;
		}
	}
	victim = freeslot >= 0 ? freeslot : lru;
	if (freeslot < 0)
		ov_flush_all(); // queued lighting may still reference the CLUT being replaced
	H.clut_ent[victim].key = key;
	H.clut_ent[victim].stamp = H.clut_tick;
	H.clut_ent[victim].gen = H.pal_gen;
	clut_build_into(H.clut_base + (u32)victim * CLUT_BLOCKS, kind, param, keyed);
	return H.clut_base + (u32)victim * CLUT_BLOCKS;
}

// ---- upload of one texture ----

static u8 *scratch;
static size_t scratch_cap;

static u8 *scratch_get(size_t n)
{
	if (n > scratch_cap)
	{
		u8 *s = realloc(scratch, n);

		if (!s)
			return NULL;
		scratch = s;
		scratch_cap = n;
	}
	return scratch;
}

static inline u32 log2_u32(u32 v)
{
	u32 l = 0;

	while ((1u << l) < v)
		l++;
	return l;
}

// RGBA -> palette indices in scratch. Returns 0 when a texel is not a palette colour (or has partial alpha); *holes = some texel
// has alpha 0 (index 255 in dst); *uses255 = some opaque texel has the colour of index 255 and keeps that index.
static int rgba_to_indices(const u32 *src, u32 n, u8 *dst, int *holes, int *uses255)
{
	u32 i, last = 0, lastidx = 0;
	int h = 0, u = 0;

	for (i = 0; i < n; i++)
	{
		u32 c = src[i];

		if (c == last && i)
		{
			dst[i] = (u8)lastidx;
			h |= (c >> 24) == 0;
			u |= (c >> 24) != 0 && lastidx == 255;
			continue;
		}
		if ((c >> 24) == 0)
		{
			// Alpha-zero RGB still participates in bilinear filtering. A keyed CLUT has only palette[255]'s RGB.
			if ((c & 0xFFFFFFu) != H.pal[255])
				return 0;
			lastidx = 255;
			h = 1;
		}
		else if ((c >> 24) != 255)
		{
			return 0;
		}
		else
		{
			int k = pal_lookup(c & 0xFFFFFF);

			if (k < 0)
				return 0;
			if (k == 255)
				u = 1;
			lastidx = (u32)k;
		}
		last = c;
		dst[i] = (u8)lastidx;
	}
	*holes = h;
	*uses255 = u;
	return 1;
}

// write rows [r0, r0+nr) of the stored texture: W2 pixels per row of bpp bytes, produced by fill(row, dst)
typedef void (*rowfill_t)(void *ctx, u32 row, u8 *dst);

static void upload_bands(u32 blk, u32 tbw, u32 psm, u32 W2, u32 H2, u32 bpp, rowfill_t fill, void *ctx)
{
	u32 rowbytes = W2 * bpp;
	u32 band = (65536u / rowbytes) & ~15u; // rows per band, multiple of 16
	u32 r0, nr, y;
	qw_t *p;

	H.serial++;
	if (!band)
		band = 16;
	for (r0 = 0; r0 < H2; r0 += nr)
	{
		qw_t *h;
		u8 *dst;
		u32 nqw;

		nr = H2 - r0 < band ? H2 - r0 : band;
		nqw = (rowbytes * nr + 15) / 16;
		h = pk_alloc(1 + 4 + 1 + nqw);
		h[0].d[0] = HWGIF_TAG(4, 0, 0, 0, 0, 1);
		h[0].d[1] = GIF_REGS_AD;
		ad_set(&h[1], ((u64)blk << 32) | ((u64)tbw << 48) | ((u64)psm << 56), GSR_BITBLTBUF);
		ad_set(&h[2], (u64)r0 << 48, GSR_TRXPOS);
		ad_set(&h[3], (u64)W2 | ((u64)nr << 32), GSR_TRXREG);
		ad_set(&h[4], 0, GSR_TRXDIR);
		h[5].d[0] = HWGIF_TAG(nqw, 1, 0, 0, 2, 0);
		h[5].d[1] = 0;
		dst = (u8 *)&h[6];
		for (y = 0; y < nr; y++)
			fill(ctx, r0 + y, dst + (size_t)y * rowbytes);
		if ((rowbytes * nr) & 15)
			memset(dst + rowbytes * nr, 0, 16 - ((rowbytes * nr) & 15));
	}
	p = ad_alloc(1);
	ad_set(p, 0, GSR_TEXFLUSH);
}

typedef struct
{
	const u8 *src; // source rows of `sbpp` bytes per texel
	u32 sw, sh, W2, H2; // source size, stored size
	u32 dx, dy; // the stored texture takes every (1 << dx)-th column / (1 << dy)-th row of the source (PS2-HW-20); 0 = all of them
	int mode; // fill-specific
	int holes;
	u8 pad[4];
} fillctx_t;

// Without decimation uploads preserve source rows and columns exactly. A decimated texture (dx/dy > 0) keeps the texel at the centre of
// each (1 << dx) x (1 << dy) block of the source: no new colours (palette indices stay indices), the nearest-neighbour sample
// of the full-size texture at the position of the stored texel's centre.
static inline u32 src_row(const fillctx_t *c, u32 y)
{
	return (y << c->dy) + ((1u << c->dy) >> 1);
}

static inline u32 src_col(const fillctx_t *c, u32 x)
{
	return (x << c->dx) + ((1u << c->dx) >> 1);
}

// 8-bit index data (P_8 or indices from scratch)
static void fill_idx(void *ctx, u32 row, u8 *dst)
{
	fillctx_t *c = ctx;
	const u8 *s = c->src + (size_t)src_row(c, row) * c->sw;
	u32 x;

	if (!c->dx)
	{
		memcpy(dst, s, c->W2);
		return;
	}
	for (x = 0; x < c->W2; x++)
		dst[x] = s[src_col(c, x)];
}

// AP_88 -> index with alpha 0 = index 255 (mode 0), or the chroma keyed variant: the index is kept, the alpha ignored (mode 1)
static void fill_ap88(void *ctx, u32 row, u8 *dst)
{
	fillctx_t *c = ctx;
	const u8 *s = c->src + (size_t)src_row(c, row) * c->sw * 2;
	u32 x;

	for (x = 0; x < c->W2; x++)
	{
		const u8 *t = s + (size_t)src_col(c, x) * 2;

		dst[x] = (c->mode == 0 && t[1] == 0) ? 255 : t[0];
	}
}

// AP_88 with partial alpha (or opaque index 255 next to holes) needs direct colour: palette colour, GS alpha.
static void fill_ap88_ct32(void *ctx, u32 row, u8 *dst)
{
	fillctx_t *c = ctx;
	const u8 *s = c->src + (size_t)src_row(c, row) * c->sw * 2;
	u32 *d = (u32 *)(void *)dst, x;

	for (x = 0; x < c->W2; x++)
	{
		const u8 *p = s + (size_t)src_col(c, x) * 2;

		d[x] = H.pal[p[0]] | (a8_to_gs(p[1]) << 24);
	}
}

// RGBA -> CT32 with the GS alpha scale (0x80 = opaque)
static void fill_ct32(void *ctx, u32 row, u8 *dst)
{
	fillctx_t *c = ctx;
	const u32 *s = (const u32 *)(const void *)(c->src + (size_t)src_row(c, row) * c->sw * 4);
	u32 *d = (u32 *)(void *)dst;
	u32 x;

	for (x = 0; x < c->W2; x++)
	{
		u32 t = s[src_col(c, x)];

		d[x] = (t & 0xFFFFFF) | (a8_to_gs(t >> 24) << 24);
	}
}

// ALPHA_INTENSITY_88 -> CT32 grey with alpha
static void fill_ia88(void *ctx, u32 row, u8 *dst)
{
	fillctx_t *c = ctx;
	const u8 *s = c->src + (size_t)src_row(c, row) * c->sw * 2;
	u32 *d = (u32 *)(void *)dst;
	u32 x;

	for (x = 0; x < c->W2; x++)
	{
		const u8 *t = s + (size_t)src_col(c, x) * 2;

		d[x] = (u32)t[0] * 0x010101u | (a8_to_gs(t[1]) << 24);
	}
}

// why the last tex_upload failed (diagnostics of hw_SetTexture)
static const char *tex_fail_why;

// returns the record index or NOREC (nothing was changed then)
static int tex_upload(GLMipmap_t *m)
{
	u32 sw = m->width, sh = m->height;
	u32 W2, H2, bpp, tbw, nblk, align, blk, i, n;
	int psm, clut, baked = 0, holes = 0, uses255 = 0, ri;
	rowfill_t fill;
	fillctx_t fc, *c = &fc;
	u32 t0 = cyc();
	const u8 *data = m->data;

	tex_fail_why = "ok";
	if (!data || !sw || !sh)
	{
		tex_fail_why = !data ? "no data (purged)" : "zero size";
		return NOREC;
	}
	if ((sw > 1024 && (sw & 1)) || (sh > 1024 && (sh & 1)) || sw > 16384 || sh > 16384)
	{
		tex_fail_why = "larger than 1024 and not divisible";
		return NOREC;
	}

	memset(c, 0, sizeof *c);
	n = sw * sh;

	switch (m->format)
	{
		case GL_TEXFMT_P_8:
			psm = PSM_T8;
			clut = (m->flags & TF_CHROMAKEYED) ? 2 : 1;
			fill = fill_idx;
			if (clut == 2)
				for (i = 0; i < n; i++)
					if (data[i] == 255)
					{
						holes = 1;
						break;
					}
			break;
		case GL_TEXFMT_AP_88:
		{
			int partial = 0, hole = 0, opaque255 = 0, hole_rgb = 0;

			psm = PSM_T8;
			fill = fill_ap88;
			if (m->flags & TF_CHROMAKEYED) // index 255 is the hole, the alpha byte is ignored
			{
				clut = 2;
				c->mode = 1;
				for (i = 0; i < n; i++)
					if (data[i * 2] == 255)
					{
						holes = 1;
						break;
					}
				break;
			}
			for (i = 0; i < n; i++)
			{
				u8 a = data[i * 2 + 1];

				if (a != 0 && a != 255)
				{
					partial = 1;
					break;
				}
				hole |= a == 0;
				hole_rgb |= a == 0 && H.pal[data[i * 2]] != H.pal[255];
				opaque255 |= a == 255 && data[i * 2] == 255;
			}
			if (partial || hole_rgb || (hole && opaque255))
			{
				psm = PSM_CT32;
				clut = 0;
				baked = 1;
				fill = fill_ap88_ct32;
			}
			else
			{
				clut = hole ? 2 : 1;
			}
			break;
		}
		case GL_TEXFMT_ALPHA_8:
			psm = PSM_T8;
			clut = 3;
			fill = fill_idx;
			break;
		case GL_TEXFMT_RGBA:
		{
			u8 *sc;

			psm = PSM_CT32;
			clut = 0;
			fill = fill_ct32;
			if (H.pal_set && (sc = scratch_get((size_t)n)) != NULL && rgba_to_indices((const u32 *)(const void *)data, n, sc, &holes, &uses255))
			{
				if (!(holes && uses255)) // both would need index 255 for two meanings
				{
					psm = PSM_T8;
					clut = holes ? 2 : 1;
					fill = fill_idx;
					data = sc;
				}
			}
			break;
		}
		case GL_TEXFMT_ALPHA_INTENSITY_88:
			psm = PSM_CT32;
			clut = 0;
			fill = fill_ia88;
			break;
		default:
			tex_fail_why = "unknown format";
			return NOREC;
	}

	// The GS takes at most 1024 texels per axis, and the pool of the PS2 is a fifth of what the engine's textures can add up to
	// in a frame: a texture over the limit (or over the configured footprint cap) is stored decimated by powers of two, see PS2-HW-20.
	bpp = psm == PSM_T8 ? 1 : 4;
	{
		u32 dx = 0, dy = 0;

		while ((sw >> dx) > 1024 && !(sw & ((2u << dx) - 1)))
			dx++;
		while ((sh >> dy) > 1024 && !(sh & ((2u << dy) - 1)))
			dy++;
		if ((sw >> dx) > 1024 || (sh >> dy) > 1024)
		{
			tex_fail_why = "larger than 1024 and not divisible";
			return NOREC;
		}
		while (H.tex_cap_blocks)
		{
			u32 w1 = sw >> dx, h1 = sh >> dy, tb = psm == PSM_T8 ? ((w1 + 127) / 128) * 2 : (w1 + 63) / 64;

			if (tex_footprint(psm, w1, h1, tb) <= H.tex_cap_blocks)
				break;
			if (w1 >= h1 && w1 > 16 && !(sw & ((2u << dx) - 1)))
				dx++;
			else if (h1 > 16 && !(sh & ((2u << dy) - 1)))
				dy++;
			else if (w1 > 16 && !(sw & ((2u << dx) - 1)))
				dx++;
			else
				break;
		}
		c->dx = dx;
		c->dy = dy;
		W2 = sw >> dx;
		H2 = sh >> dy;
		if (dx || dy)
			H.st.tex_decimated++;
	}

	c->src = data;
	c->sw = sw;
	c->sh = sh;
	c->W2 = W2;
	c->H2 = H2;

	tbw = psm == PSM_T8 ? ((W2 + 127) / 128) * 2 : (W2 + 63) / 64;
	nblk = tex_footprint(psm, W2, H2, tbw);
	if (cfg.tex_max_bytes > 0 && nblk > (u32)cfg.tex_max_bytes / 256u)
	{
		tex_fail_why = "footprint over budget";
		hw_failure("exact texture GS footprint exceeds configured budget");
		return NOREC;
	}
	align = 1;
	while (align < nblk && align < PAGE_BLOCKS)
		align <<= 1;
	blk = vram_alloc_evicting(nblk, align, 0);
	if (!blk)
	{
		tex_fail_why = "no VRAM block (pool)";
		return NOREC;
	}

	upload_bands(blk, tbw, psm, W2, H2, bpp, fill, c);

	ri = rec_new();
	if (ri == NOREC)
	{
		tex_fail_why = "record table";
		vram_free(blk, nblk);
		return NOREC;
	}
	{
		texrec_t *r = &H.rec[ri];

		r->owner = m;
		r->blk = blk;
		r->nblk = nblk;
		own_set(ri);
		r->w = (u16)W2;
		r->h = (u16)H2;
		r->tw = (u8)log2_u32(W2);
		r->th = (u8)log2_u32(H2);
		r->tbw = (u8)tbw;
		r->psm = (u8)psm;
		r->clut = (u8)clut;
		r->wrapx = (m->flags & TF_WRAPX) ? 1 : 0;
		r->wrapy = (m->flags & TF_WRAPY) ? 1 : 0;
		r->baked = (u8)baked;
		r->stamp = H.frame_no;
		r->uw = W2;
		r->vh = H2;
		r->npot_x = (W2 & (W2 - 1)) != 0;
		r->npot_y = (H2 & (H2 - 1)) != 0;
		r->region = (u8)((r->npot_x ? 1 : 0) | (r->npot_y ? 2 : 0));
		r->sxs = (float)W2 / (float)(1u << r->tw);
		r->sys = (float)H2 / (float)(1u << r->th);
		if (holes && clut == 2 && (m->format == GL_TEXFMT_P_8 || m->format == GL_TEXFMT_AP_88))
			m->flags |= TF_TRANSPARENT; // as r_opengl.c does for a chroma keyed texture with a hole
		r->nearest_only = (m->flags & TF_TRANSPARENT) ? 1 : 0;
		lru_push_head(ri);
	}
	m->downloaded = (UINT32)(ri + 1);
	H.st.uploads++;
	H.st.upload_bytes += W2 * H2 * bpp;
	H.st.cyc_tex += cyc() - t0;
	return ri;
}

// drop every texture; the engine finds `downloaded` cleared and regenerates what it still needs
static void tex_free_all(void)
{
	int i;

	for (i = 0; i < H.rec_n; i++)
		if (H.rec[i].used)
		{
			if (H.rec[i].owner)
				H.rec[i].owner->downloaded = 0;
			vram_free(H.rec[i].blk, H.rec[i].nblk);
			H.rec[i].used = 0;
		}
	memset(blk_owner, 0, sizeof blk_owner);
	H.rec_n = 0;
	H.rec_free = NOREC;
	H.lru_head = H.lru_tail = NOREC;
	H.cur_tex = NOREC;
	H.cur_missing = 0;
	for (i = 0; i < NSCR; i++)
	{
		H.scr_rec[i] = NOREC;
		free(H.scr_data[i]);
		H.scr_data[i] = NULL;
	}
	H.screen_bytes = 0;
}

// ---- palette change (SetTexturePalette) ----
// The engine flushes its texture cache on a palette change; the VRAM textures here are palette indices, so only the CLUT
// images change. Textures that could not be indexed (colours outside the palette) were stored as direct colour: those go.

static void palette_set(const RGBA_t *pp)
{
	u32 np[256];
	int i, changed;

	for (i = 0; i < 256; i++)
		np[i] = (u32)pp[i].s.red | ((u32)pp[i].s.green << 8) | ((u32)pp[i].s.blue << 16);
	changed = !H.pal_set || memcmp(np, H.pal, sizeof np) != 0;
	if (!changed)
		return;
	ov_flush_all(); // pending draws must consume the previous palette before its CLUTs/records change
	memcpy(H.pal, np, sizeof np);
	H.pal_set = 1;
	pal_build();
	H.pal_gen++;
	for (i = 0; i < H.rec_n; i++)
		if (H.rec[i].used && H.rec[i].baked)
			tex_drop(i, 0);
}

// ---- screen textures: the frame (or part of it) at full resolution, in the frame buffer format ----

static u32 scr_bytes(void)
{
	return (u32)H.fbw * (u32)H.vh * (H.fb32 ? 4u : 2u);
}

static void scr_backing_free(int slot)
{
	if (H.scr_data[slot])
	{
		free(H.scr_data[slot]);
		H.scr_data[slot] = NULL;
		H.screen_bytes -= scr_bytes();
	}
}

// Spill only after a successful synchronized read: even current-frame slots remain recoverable.
static int scr_spill_victim(void)
{
	int v, best = NOREC, slot;
	u32 bytes = scr_bytes(), budget = cfg.screen_max_bytes ? cfg.screen_max_bytes : 8u * 1024u * 1024u;
	void *data;

	for (v = H.lru_tail; v != NOREC; v = H.rec[v].prev)
		if (H.rec[v].screen && !H.rec[v].pin)
		{
			best = v;
			break;
		}
	if (best == NOREC)
	{
		CONS_Printf("HWD spill: no screen victim lru_tail %d head %d\n", H.lru_tail, H.lru_head);
		return 0;
	}
	slot = H.rec[best].screen - 1;
	if (!H.scr_data[slot])
	{
		if (H.screen_bytes > budget || bytes > budget - H.screen_bytes)
		{
			CONS_Printf("HWD spill: budget\n");
			return 0;
		}
		data = memalign(64, bytes);
		if (!data)
		{
			CONS_Printf("HWD spill: no memory for %u bytes\n", (unsigned)bytes);
			return 0;
		}
		{
			int rr = PS2HWD_ReadVram(data, H.rec[best].blk, H.rec[best].tbw, H.fbpsm, 0, 0, H.fbw, H.vh);
			if (rr != 0)
			{
				CONS_Printf("HWD spill: ReadVram failed %d\n", rr);
				free(data);
				return 0;
			}
		}
		H.scr_data[slot] = data;
		H.screen_bytes += bytes;
		if (H.screen_bytes > H.screen_peak_bytes)
			H.screen_peak_bytes = H.screen_bytes;
	}
	tex_drop(best, 1);
	H.st.screen_spills++;
	return 1;
}

static void fill_screen(void *ctx, u32 row, u8 *dst)
{
	u32 rowbytes = (u32)H.fbw * (H.fb32 ? 4u : 2u);
	memcpy(dst, (const u8 *)ctx + (size_t)row * rowbytes, rowbytes);
}

// the record of screen texture `slot`, created (page aligned: it is also a render target) when missing; NOREC on failure
static int scr_tex_get(int slot)
{
	int ri;
	u32 pages, nblk, blk, tw = log2_u32((u32)H.vw), th = log2_u32((u32)H.vh);

	if (H.scr_rec[slot] != NOREC && H.rec[H.scr_rec[slot]].used)
		return H.scr_rec[slot];
	pages = fb_pages(H.fbw, H.fbh, H.fb32);
	nblk = pages * PAGE_BLOCKS;
	blk = vram_alloc_evicting(nblk, PAGE_BLOCKS, 1);
	if (!blk)
		return NOREC;
	ri = rec_new();
	if (ri == NOREC)
	{
		vram_free(blk, nblk);
		return NOREC;
	}
	{
		texrec_t *r = &H.rec[ri];

		r->blk = blk;
		r->nblk = nblk;
		own_set(ri);
		r->w = (u16)H.vw;
		r->h = (u16)H.vh;
		r->tw = (u8)tw;
		r->th = (u8)th;
		r->tbw = (u8)(H.fbw / 64);
		r->psm = (u8)H.fbpsm;
		r->screen = (u8)(slot + 1);
		r->notcc = 1;
		r->region = 3;
		r->uw = (u32)H.vw;
		r->vh = (u32)H.vh;
		r->sxs = (float)H.vw / (float)(1u << tw);
		r->sys = (float)H.vh / (float)(1u << th);
		r->stamp = H.frame_no;
		lru_push_head(ri);
	}
	H.scr_rec[slot] = ri;
	return ri;
}

static int scr_resident(int slot)
{
	int ri;

	if (slot < 0 || slot >= NSCR || (H.scr_rec[slot] == NOREC && !H.scr_data[slot]))
		return NOREC;
	if (H.scr_rec[slot] != NOREC)
		return H.scr_rec[slot];
	ri = scr_tex_get(slot);
	if (ri != NOREC)
	{
		upload_bands(H.rec[ri].blk, H.rec[ri].tbw, H.fbpsm, (u32)H.fbw, (u32)H.vh,
			H.fb32 ? 4u : 2u, fill_screen, H.scr_data[slot]);
		H.st.screen_restores++;
	}
	return ri;
}
