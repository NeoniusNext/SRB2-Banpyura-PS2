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
static u32 tex_want(const GLMipmap_t *m, int *visible); // ps2_hw_plan.inc

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
	H.rec[i].share = H.rec[i].sec = H.rec[i].sec_next = NOREC;
	H.rec[i].pnext = H.rec[i].ppar = NOREC;
	return i;
}

// the record that owns the image of record i (itself unless it is a view)
static inline int img_of(int i)
{
	return H.rec[i].share != NOREC ? H.rec[i].share : i;
}

// the blocks an image holds (both pieces of a split image)
static inline u32 rec_blocks(const texrec_t *r)
{
	return r->nblk + (r->pnext != NOREC ? H.rec[r->pnext].nblk : 0u);
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
static void lod_account(texrec_t *r);
static int batch_phase; // (defined below)

static int drop_reason; // OPT12 HWDRV (PS2-HW-441): who drops the image (0 = the default: eviction when counted, 5 otherwise); set by the callers that drop for another reason

static void tex_drop(int i, int count_eviction)
{
	texrec_t *r = &H.rec[i];

	if (r->owner)
		r->owner->ps2_drop = (u8)(drop_reason ? drop_reason : (count_eviction ? 1 : 5));

	if (ps2hwd_dbg_flags & HWDBG_TEX)
		lod_account(r);

	if (r->share != NOREC)
	{
		// a view: the image stays for the other variants
		texrec_t *o = &H.rec[r->share];
		int *link = &o->sec;

		while (*link != NOREC && *link != i)
			link = &H.rec[*link].sec_next;
		if (*link == i)
			*link = r->sec_next;
		if (r->owner)
			r->owner->downloaded = 0;
		rec_release(i);
		return;
	}
	if (r->pnext != NOREC || r->ppar != NOREC)
	{
		// PS2-HW-70: the two pieces of an image taller than the GS limit live and die together (a polygon needs both)
		const int o = r->pnext != NOREC ? r->pnext : r->ppar;

		r->pnext = r->ppar = NOREC;
		H.rec[o].pnext = H.rec[o].ppar = NOREC;
		tex_drop(o, count_eviction);
	}
	if (count_eviction && !r->screen && !r->pin && r->stamp == H.frame_no)
	{
		H.thrash_n++; // an image the frame uses is evicted in the frame: it has to be uploaded again when it is drawn (PS2-HW-39: feeds the budget controller)
		H.thrash_blk += r->nblk;
	}
	if (count_eviction && (ps2hwd_dbg_flags & HWDBG_IMMDBG) && r->nblk >= 128)
		CONS_Printf("HWIMM f=%u EVICT %s %ux%u lvl=%d blk=%u age=%u done=%d cur=%d phase=%d\n", (unsigned)H.frame_no, r->owner ? HWR_PS2_TexName(r->owner) : "-", (unsigned)r->sw, (unsigned)r->sh, (int)r->dx,
			(unsigned)r->nblk, (unsigned)(H.frame_no - r->stamp), (int)(r->done == H.frame_no + 1), H.cur_tex == i, batch_phase);
	// the image goes, and every view of it with it (their engine owners find `downloaded` cleared)
	while (r->sec != NOREC)
	{
		int v = r->sec;

		r->sec = H.rec[v].sec_next;
		if (H.rec[v].owner)
			H.rec[v].owner->downloaded = 0;
		rec_release(v);
	}
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

unsigned int PS2HWD_ScanDirection(void)
{
	return H.frame_no & 1u;
}

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
static u16 (*blk_owner_p)[VRAM_BLOCKS]; // PS2-171: the VRAM block owner map: zone memory while the driver runs (hwbig_alloc, ps2_hwd.c)
#define blk_owner (*blk_owner_p)

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

// Cost of evicting record r per block (WEIGHT_STAY: it cannot be evicted now): 1 for the least recently used texture up to 31 for the
// most recent one; 60 for a texture of this frame's batches that has not been drawn yet. OPT9 (PS2-HW-31): textures drawn in this frame
// are no longer weighed apart from the others: the least recently DRAWN goes first. The batches of a frame are drawn by texture and the
// direction of that scan alternates from frame to frame (hw_batching.c), so the textures drawn first in a frame are the ones needed last
// in the next frame: dropping the least recently used is then the optimal choice (Belady) for a cyclic scan that does not fit the pool.
static inline u32 evict_weight(const texrec_t *r, int i, u32 seq_lo, u32 shift)
{
	if (r->share != NOREC)
		return WEIGHT_STAY; // a view owns no blocks
	if (r->ppar != NOREC)
		return evict_weight(&H.rec[r->ppar], r->ppar, seq_lo, shift); // PS2-HW-70: the second piece of an image goes when the first one may
	if (r->pin)
		return WEIGHT_STAY;
	if (r->screen)
	{
		// a wipe image that no wipe has drawn for a while is dead (the engine captures both again before every wipe)
		if (r->screen <= 2 && H.frame_no - r->stamp > WIPE_DEAD_FRAMES)
			return 1;
		return WEIGHT_STAY; // other captures cannot be regenerated from engine data
	}
	if (H.cur_tex != NOREC && (i == H.cur_tex || H.rec[H.cur_tex].share == i))
		return WEIGHT_STAY; // the texture being drawn
	if (r->stamp == H.frame_no)
	{
		if (batch_phase == 1)
			return WEIGHT_STAY;
		if (batch_phase == 2 && r->done != H.frame_no + 1)
			return 60u; // still to be drawn this frame
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

// The texture to drop for a small request: the least recently used one that is not still to be drawn in this frame, then (nothing else
// left) one that is. NOREC when everything left must stay.
static int small_victim(void)
{
	int v, pass;

	for (pass = 0; pass < 2; pass++)
		for (v = H.lru_tail; v != NOREC; v = H.rec[v].prev)
		{
			u32 w = evict_weight(&H.rec[v], v, 0, 31); // seq 0, shift 31: every texture that is not pending weighs 1

			if (w != WEIGHT_STAY && (pass == 1 || w < 60u))
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
		if (!tries)
		{
			TX.alloc_fail++; // no free range fits: something has to go (PS2-HW-39: how often there was room in total, but not in one piece)
			if (H.pool_blocks - H.used_blocks >= n)
			{
				TX.alloc_frag++;
				TX.alloc_frag_blk += n;
			}
		}
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
	qw_t *h = pk_alloc(1 + 4 + 1 + 64 + 2);
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
	// PS2-HW-102 (real GS, not PCSX2): a CLUT image written over a slot that was loaded before (the row of a light table, a palette change) must not come out of
	// the texture cache of the GS: TEXFLUSH behind the transfer (the program of the VU1 path reloads CLUTs of the cache with TEX2 / CLD all the time)
	d[64].d[0] = HWGIF_TAG(1, 1, 0, 0, 0, 1);
	d[64].d[1] = GIF_REGS_AD;
	ad_set(&d[65], 0, GSR_TEXFLUSH);
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
	CK_LIGHT, // PS2-HW-71 palette rendering: a row of a colormap light table through the screen palette: param = pal_param(), flag = keyed
};

// the 256 entries of a CLUT image in index order (0x80 alpha = opaque); the caller swizzles
static void clut_entries(u32 kind, u32 param, u32 keyed, u32 *e)
{
	int i;
	const u32 *op = out_pal(); // PS2-HW-71: the screen palette (flash, 565 crush) in palette rendering mode, else the texture palette

	switch (kind)
	{
		case CK_LIGHT:
			pal_light_entries(param, keyed, e);
			break;
		case CK_OPAQUE:
		case CK_KEYED:
			for (i = 0; i < 256; i++)
				e[i] = op[i] | (0x80u << 24);
			if (kind == CK_KEYED)
				e[255] = op[255];
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
				float r = (float)(op[i] & 255) * (1.0f / 255.0f), g = (float)((op[i] >> 8) & 255) * (1.0f / 255.0f), b = (float)((op[i] >> 16) & 255) * (1.0f / 255.0f);
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
				u32 channel = (op[i] >> (8 * (param & 3))) & 255u;
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
	return kind == CK_OPAQUE || kind == CK_KEYED || kind == CK_TINT || kind == CK_MULT || kind == CK_LIGHT;
}

static void clut_build_into(u32 blk, u32 kind, u32 param, u32 keyed)
{
	static u32 e[256] __attribute__((aligned(64)));
	static u32 sw[256] __attribute__((aligned(64)));
	int i;

	H.clut_epoch++; // PS2-HW-101
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

static u8 clut_hint[256]; // hash of a CLUT key -> slot + 1 (verified against the key, a stale hint only costs the linear search)

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
	// PS2-HW-72: palette rendering asks for a CLUT per colormap row of every lit polygon: a direct mapped index in front of the linear search (124 slots)
	{
		const u32 h = (u32)((key ^ (key >> 20) ^ (key >> 40)) * 2654435761u) >> 24;
		const int hi = (int)clut_hint[h] - 1;

		if (hi >= CLUT_FIXED && H.clut_ent[hi].key == key)
		{
			if (clut_palette_dependent(kind) && H.clut_ent[hi].gen != H.pal_gen)
			{
				clut_build_into(H.clut_base + (u32)hi * CLUT_BLOCKS, kind, param, keyed);
				H.clut_ent[hi].gen = H.pal_gen;
			}
			H.clut_ent[hi].stamp = H.clut_tick;
			return H.clut_base + (u32)hi * CLUT_BLOCKS;
		}
	}
	for (i = CLUT_FIXED; i < CLUT_SLOTS; i++)
	{
		if (H.clut_ent[i].key == key)
		{
			clut_hint[(u32)((key ^ (key >> 20) ^ (key >> 40)) * 2654435761u) >> 24] = (u8)(i + 1);
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
	clut_hint[(u32)((key ^ (key >> 20) ^ (key >> 40)) * 2654435761u) >> 24] = (u8)(victim + 1);
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

// PS2-HW-73: a transfer must stay inside the 4 MiB of GS memory: the GS wraps the address, PCSX2 too, and the picture of another texture is overwritten
// without any error. The pool allocator keeps to it by construction; this is the check that says so when it does not.
static inline void vram_guard(u32 blk, int psm, u32 W2, u32 H2, u32 tbw, const char *what)
{
	if (blk < H.pool_base || blk + tex_footprint(psm, W2, H2, tbw) > VRAM_BLOCKS)
		hw_failure(what);
}

static void upload_bands(u32 blk, u32 tbw, u32 psm, u32 W2, u32 H2, u32 bpp, rowfill_t fill, void *ctx)
{
	u32 rowbytes = W2 * bpp;

	vram_guard(blk, (int)psm, W2, H2, tbw, "texture upload outside the texture pool of the GS memory");
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

// ---- texture data cache and zero-copy uploads (OPT9, PS2-HW-32) ----
// The texels of a map texture or flat are P_8 rows exactly as the PSMT8 transfer needs them: the GIF packet takes them straight from memory
// by a DMA reference (pk_ref) instead of a copy into the ring. The memory must not move or be freed until the DMA has read it, and the engine
// frees / purges its texture data whenever it likes (and unlocks it again on every GetTexture), so the driver takes the block over: the data
// cache. A block of it is a purgable zone block owned by the driver (the zone evicts it under memory pressure like any cache and clears
// dc[i].p), locked while a queued DMA reads it (busy). The cache is keyed by (kind, texture number, size, level), so the chroma keyed and
// the plain variant of a texture share an entry, and an evicted texture is uploaded again without the engine composing it again.

extern void HWR_PS2_LockData(void *data);
extern void HWR_PS2_UnlockData(void *data);
extern void HWR_PS2_FreeData(void *data);
extern void *HWR_PS2_StealData(GLMipmap_t *m, void **newuser);
extern void *HWR_PS2_AllocData(size_t bytes, void **newuser);
extern const u8 *HWR_PS2_FlatPin(const GLMipmap_t *m);
extern boolean HWR_PS2_FlatStreamable(const GLMipmap_t *m);
extern boolean HWR_PS2_FlatRows(const GLMipmap_t *m, UINT32 row0, UINT32 nrows, UINT8 *dest);
extern unsigned int ps2hwt_mkpatch_n, ps2hwt_mkpatch_cyc; // hw_cache.c
extern void HWR_PS2_FlatUnpin(const u8 *p, size_t bytes);
static const u8 *tex_flatpin; // PS2-HW-38: the engine's flat pinned by the last tex_upload (the caller unpins it)

#define DC_MAX 192
#define DC_BUDGET (4u * 1024u * 1024u) // bytes of texels the cache may hold (the zone may take them back sooner)
typedef struct
{
	u8 *p; // first: the owner pointer of the zone block (cleared by the zone when it purges the block)
	u32 key, bytes, stamp;
	u16 w, h;
	u8 lvl, busy; // busy: queued uploads that read the block (locked)
} dcent_t;
static dcent_t dc[DC_MAX];
static u32 dc_bytes, dc_seq;

static inline u32 dc_key(const GLMipmap_t *m)
{
	return ((u32)m->regen_kind << 28) | ((u32)m->regen_id & 0x0FFFFFFFu);
}

static void dc_drop(dcent_t *e)
{
	Z_SetRebuildCost((void **)(void *)&e->p, 0); // OPT13 IZ (PS2-603): the entry is another block next time
	if (e->p)
		HWR_PS2_FreeData(e->p);
	dc_bytes -= e->bytes;
	memset(e, 0, sizeof *e);
}

static dcent_t *dc_find(u32 key, u32 w, u32 h, u32 lvl)
{
	int i;

	for (i = 0; i < DC_MAX; i++)
	{
		dcent_t *e = &dc[i];

		if (e->key != key)
			continue;
		if (!e->p && !e->busy)
		{
			dc_drop(e); // the zone purged the block
			continue;
		}
		if (e->w == w && e->h == h && e->lvl == lvl)
		{
			e->stamp = ++dc_seq;
			return e;
		}
	}
	return NULL;
}

// the cached level of the texture that is the nearest finer one than `lv` but not the full size (levels 1 .. lv-1): the source of a coarser level
// (a fourth of the work of making it from the full size, and no need for the full size texels at all)
static dcent_t *dc_find_finer(u32 key, u32 w, u32 h, u32 lv, u32 *slv)
{
	u32 l;

	for (l = lv - 1; l >= 1 && l < lv; l--)
	{
		dcent_t *e = dc_find(key, w >> l, h >> l, l);

		if (e)
		{
			*slv = l;
			return e;
		}
	}
	return NULL;
}

// a free entry for `bytes` more texels, evicting the least recently used idle ones; NULL when the budget cannot be met
static dcent_t *dc_slot(u32 bytes)
{
	for (;;)
	{
		dcent_t *lru = NULL, *fr = NULL;
		int i;

		for (i = 0; i < DC_MAX; i++)
		{
			dcent_t *e = &dc[i];

			if (!e->key)
			{
				fr = e;
			}
			else if (!e->p && !e->busy)
			{
				dc_drop(e);
				fr = e;
			}
			else if (!e->busy && (!lru || e->stamp < lru->stamp))
			{
				lru = e;
			}
		}
		if (fr && dc_bytes + bytes <= DC_BUDGET)
			return fr;
		if (!lru)
			return NULL;
		dc_drop(lru);
	}
}

static void dc_flush(void)
{
	int i;

	for (i = 0; i < DC_MAX; i++)
		if (dc[i].key && !dc[i].busy)
			dc_drop(&dc[i]);
}

// ---- PS2-HW-442 (OPT12 HWDRV): the data of the textures between two uses ----
// A map texture of the hardware renderer is composed from patches that the engine reads from the pack (HWR_GenerateTexture): about 4 M cycles for every lump that is not in the zone's cache
// (the read of one lump costs the same for 2 KB and for 60 KB). The composite is released to the zone right after the upload (PU_HWRCACHE_UNLOCKED), and that tag is "purgable whenever
// needed": any allocation that does not fit frees it. When the VRAM image is dropped (evicted, or made again at another level of the plan) the composite is gone as well and the 81 patches of
// THROCK4 are read again: DEMO_002 lost 260..320 M cycles (one second) at a time, six times in 700 frames. z_zone.c turns the tag into PU_HWRCACHE_LRU (-hwkeep 0, the default; -hwkeep 1
// is the old rule): an LRU cache block that goes when the room is needed. keep_off is the A/B switch of the whole.
static int keep_off; // -hwkeep N (0 = on, the default; 1 = the old behaviour): A/B on one ELF (the bits of -hwdbg belong to several files)

#ifdef __mips__
extern void Z_HWCacheStats(unsigned int *conv, unsigned int *flushes);
static void keep_prof(unsigned int frames)
{
	unsigned int conv, flushes;

	(void)frames;
	Z_HWCacheStats(&conv, &flushes);
	I_OutputMsg("HWPROF46 texture data cache (PS2-HW-442): %s, blocks made LRU cache blocks=%u, freed at once by an allocation nothing else could serve=%u (zone free %u K)\n", keep_off ? "off (-hwkeep)" : "on", conv, flushes, (unsigned)(Z_ArenaFree() >> 10));
}
#endif

#define REL_MAX 160
#define REL_BYTES_MAX (1024u * 1024u) // texels read by queued DMA that the cache must keep locked: bounds the memory the zone cannot take back
static struct
{
	u8 dc[REL_MAX]; // index into dc[]
	int n;
} rel_buf[NBUF];
static int rel_total;
static u32 rel_bytes;

static void rel_release_buf(int i)
{
	int k;

	for (k = 0; k < rel_buf[i].n; k++)
	{
		dcent_t *e = &dc[rel_buf[i].dc[k]];

		if (e->busy && --e->busy == 0 && e->p)
			HWR_PS2_UnlockData(e->p);
		rel_bytes -= e->bytes;
	}
	rel_total -= rel_buf[i].n;
	rel_buf[i].n = 0;
}

static void rel_release_all(void)
{
	int i;

	for (i = 0; i < NBUF; i++)
		rel_release_buf(i);
}

// release the cache entries of every queued buffer whose transfer has completed (the open buffer's list waits for its flush)
static void rel_poll(void)
{
	int i;

	if (!rel_total)
		return;
	for (i = 0; i < NBUF; i++)
		if (rel_buf[i].n && i != H.cur && H.buf_seq[i] <= H.done_seq)
			rel_release_buf(i);
}

// the entry is read by the packet being built: locked until the buffer that holds the reference has been transferred
static void rel_add(dcent_t *e)
{
	if (rel_buf[H.cur].n >= REL_MAX)
	{
		pk_flush(); // the list stays with the flushed buffer, the new open one has an empty list (pk_flush)
		ring_wait(0);
		rel_poll();
	}
	if (!e->busy++)
		HWR_PS2_LockData(e->p);
	rel_buf[H.cur].dc[rel_buf[H.cur].n++] = (u8)(e - dc);
	rel_bytes += e->bytes;
	rel_total++;
}

// room for `bytes` more locked data: wait for the DMA of what is queued (the EE would wait for the ring anyway) when over the limit
static void rel_room(u32 bytes)
{
	u32 c0;

	if (rel_bytes + bytes <= REL_BYTES_MAX || !rel_total)
		return;
	c0 = cyc();
	pk_flush();
	ring_wait(0);
	rel_poll();
	TX.dmawait_cyc += cyc() - c0;
}

// Everything queued has been read by the DMAC: the entries are idle (the engine may free the texels it keeps, the cache may be flushed).
static void dma_fence(void)
{
	if (!rel_total)
		return;
	pk_flush();
	ring_wait(0);
	rel_poll();
	if (rel_total) // the open buffer's list (nothing was flushed): only when the buffer holds no reference any more
		rel_release_buf(H.cur);
}

static void upload_ref(u32 blk, u32 tbw, u32 psm, u32 W2, u32 H2, const u8 *data)
{
	u32 band = (262144u / W2) & ~15u, r0, nr, c0 = cyc();
	qw_t *p;

	vram_guard(blk, (int)psm, W2, H2, tbw, "zero-copy texture upload outside the texture pool of the GS memory");
	H.serial++;
	if (band < 16)
		band = 16;
	SyncDCache((void *)data, (void *)(data + (size_t)W2 * H2));
	TX.sync_cyc += cyc() - c0;
	// OPT11-STAB: an open VU1 chunk is closed BEFORE the header of the image is written: pk_ref closes it (vu_chunk_flush), and that put the chunk between the GIF tag of the
	// image (path 2) and its data, a path 1 packet inside a path 2 image: "GIF DMA ownership timeout (ring_wait)" for good (MAPMD, first frame with the world: HWD WATCHDOG
	// twice, six times before, the hardware renderer is given up; -hwdbg 2097152 (no zero copy) or -hwdbg 268435456 (no VU1) do not hang)
	if (V.chunk_open)
		vu_chunk_flush();
	for (r0 = 0; r0 < H2; r0 += nr)
	{
		qw_t *h;
		u32 bytes, nqw, full;

		nr = H2 - r0 < band ? H2 - r0 : band;
		bytes = W2 * nr;
		nqw = (bytes + 15) >> 4;
		full = bytes >> 4;
		// the header, its reference and the partial last quadword (pk_ref_tail: two more slots) stay in one buffer
		if (H.wr + 8 + ((bytes & 15) ? 2 : 0) > BUF_QW)
			pk_flush();
		h = pk_alloc(6);
		h[0].d[0] = HWGIF_TAG(4, 0, 0, 0, 0, 1);
		h[0].d[1] = GIF_REGS_AD;
		ad_set(&h[1], ((u64)blk << 32) | ((u64)tbw << 48) | ((u64)psm << 56), GSR_BITBLTBUF);
		ad_set(&h[2], (u64)r0 << 48, GSR_TRXPOS);
		ad_set(&h[3], (u64)W2 | ((u64)nr << 32), GSR_TRXREG);
		ad_set(&h[4], 0, GSR_TRXDIR);
		h[5].d[0] = HWGIF_TAG(nqw, 1, 0, 0, 2, 0);
		h[5].d[1] = 0;
		if (full)
			pk_ref(data + (size_t)r0 * W2, full);
		if (bytes & 15)
			pk_ref_tail(data + (size_t)r0 * W2 + (size_t)(full << 4), bytes & 15);
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

	if (!c->dx && !((uintptr_t)s & 7) && ((ps2hwd_fx2 & FX3_NOFILL) ? (!(c->W2 & 3) && !((uintptr_t)dst & 3)) : c->W2 >= 4))
	{
		// four texels (index, alpha pairs) in one 64-bit load: the index bytes are gathered, the alpha bytes that are zero become 0xFF (mode 0: a hole is index 255)
		// PS2-HW-252 (FX3): any width and any alignment of the row (a patch is stored at the size of its picture: 3 of 4 rows of the patches were made texel by texel): four bytes
		// at a time (an unaligned store is the swl / swr pair), the last 1..3 texels by the byte loop below
		const u64 *s8 = (const u64 *)(const void *)s;
		const int keyed = c->mode != 0;
		const u32 x4 = c->W2 & ~3u;
		u8 *dp = dst;

		for (x = 0; x < x4; x += 4)
		{
			const u64 v = *s8++;
			u32 idx = (u32)(v & 0xFFu) | (u32)((v >> 8) & 0xFF00u) | (u32)((v >> 16) & 0xFF0000u) | (u32)((v >> 24) & 0xFF000000u);

			if (!keyed)
			{
				const u64 av = v >> 8;
				const u32 a = (u32)(av & 0xFFu) | (u32)((av >> 8) & 0xFF00u) | (u32)((av >> 16) & 0xFF0000u) | (u32)((av >> 24) & 0xFF000000u);
				const u32 nz = (((a & 0x7F7F7F7Fu) + 0x7F7F7F7Fu) | a) & 0x80808080u; // bit 7 of every byte that is not zero
				const u32 zero = ~nz & 0x80808080u;

				idx |= (zero >> 7) * 0xFFu;
			}
			if ((uintptr_t)dp & 3)
				memcpy(dp, &idx, 4);
			else
				*(u32 *)(void *)dp = idx;
			dp += 4;
		}
		for (; x < c->W2; x++) // the last texels of the row (W2 not a multiple of 4)
		{
			const u8 *t = s + (size_t)x * 2;

			dst[x] = (!keyed && t[1] == 0) ? 255 : t[0];
		}
		if (ps2hwd_dbg_flags & HWDBG_COMPOSE) // -hwdbg 16777216: the row is made again by the byte loop and compared (PS2-HW-71)
		{
			static unsigned fchecked, fbad;

			for (x = 0; x < c->W2; x++)
			{
				const u8 *t = s + (size_t)x * 2;

				if (dst[x] != ((c->mode == 0 && t[1] == 0) ? 255 : t[0]))
				{
					fbad++;
					break;
				}
			}
			if (!(++fchecked & 1023) || (x < c->W2 && fbad < 5))
				CONS_Printf("HWC fill check: %u rows, %u differ\n", fchecked, fbad);
		}
		return;
	}
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
static int zc_last; // the last tex_upload sent the texels by DMA reference: the engine data stays locked (rel_add)

// ---- adaptive footprint cap (OPT8, PS2-HW-24) ----
// The pool (3 MiB) holds less than the textures one frame draws (GFZ1 alone 7 MiB at full size): drawing them every frame means uploading
// them every frame (measured: 4-7 MiB per frame, 20-30 vblanks per frame in PCSX2). The cap is the largest footprint one texture may take;
// a bigger one is stored decimated. tex_cap_control() runs at every frame end: it knows what the frame drew (H.pred[k] = the sum of the
// footprints under cap k) and keeps the working set at a fixed share of the pool.
#define CAP_BUDGET_NUM 3 // the working set of a frame may fill 3/4 of the pool (the rest absorbs fragmentation, sprites of the next frame)
#define CAP_BUDGET_DEN 4
#define CAP_UP_FRAMES 150 // frames a larger cap must have fitted before it is taken (one step)
#define CAP_DOWN_FRAMES 2 // frames the working set must have overflowed before the cap goes down (an emergency goes down at once)
#define CAP_UPGRADE_BLOCKS 1200 // blocks of textures that may be re-made larger per frame

static void cap_step(int idx, const char *why)
{
	static int logged;

	if (idx < 0)
		idx = 0;
	if (idx >= NCAPS)
		idx = NCAPS - 1;
	if (idx == H.cap_idx)
		return;
	if (logged < 40)
	{
		logged++;
		CONS_Printf("HWD texture cap %u -> %u blocks (%s)\n", (unsigned)CAPS[H.cap_idx], (unsigned)CAPS[idx], why);
	}
	H.cap_idx = idx;
	H.tex_cap_blocks = CAPS[idx];
	H.cap_gen++;
	H.cap_up_count = H.cap_down_count = 0;
	H.st.cap_changes++;
}

static void tex_cap_control(void)
{
	u32 budget = H.pool_blocks / CAP_BUDGET_DEN * CAP_BUDGET_NUM;
	int k;

	if (!H.cap_adapt || H.pred_frame != H.frame_no + 1)
		return;
	H.st.pred_ws = H.pred[H.cap_idx];
	H.upg_blocks = 0;
	if (H.pred[H.cap_idx] > budget)
	{
		H.cap_up_count = 0;
		for (k = H.cap_idx + 1; k < NCAPS - 1 && H.pred[k] > budget; k++)
			;
		if (H.pred[H.cap_idx] > H.pool_blocks / 10u * 9u || ++H.cap_down_count >= CAP_DOWN_FRAMES)
			cap_step(k, "working set over budget");
		return;
	}
	H.cap_down_count = 0;
	if (H.cap_idx > 0 && H.pred[H.cap_idx - 1] <= budget * 9u / 10u)
	{
		if (++H.cap_up_count >= CAP_UP_FRAMES)
			cap_step(H.cap_idx - 1, "working set fits a larger cap");
	}
	else
	{
		H.cap_up_count = 0;
	}
}

// The stored size of a texture: every axis over the GS limit of 1024 texels is halved (by taking every (1 << d)-th texel), then the larger
// axis is halved until the GS footprint is within `cap` blocks (cap 0 = no cap). Returns 0 when the size cannot be made valid.
static int tex_nosplit; // -hwnosplit: images over 1024 rows are decimated (A/B of PS2-HW-70)
static u32 tex_split_rows = 1024; // rows of the first piece of a split image: the GS limit; -hwsplitrows N (64..1024, a multiple of 64) makes every map texture of N+1..2N rows two images (the test of the split on ordinary content)

// splittable: the texture may be stored as two images when it has more than 1024 rows (PS2-HW-70: a P_8 map texture or flat of 1025..2048 rows and at most
// 1024 columns, no footprint cap in force) instead of with every second row
static int tex_plan(int psm, u32 sw, u32 sh, u32 cap, int splittable, u32 *pdx, u32 *pdy)
{
	u32 dx = 0, dy = 0;

	if (!cap && splittable && !tex_nosplit && psm == PSM_T8 && sw <= 1024 && sh > 1024 && sh <= 2048 && !(sh & 1) && tex_split_rows == 1024)
	{
		*pdx = *pdy = 0;
		return 1;
	}
	while ((sw >> dx) > 1024 && !(sw & ((2u << dx) - 1)))
		dx++;
	while ((sh >> dy) > 1024 && !(sh & ((2u << dy) - 1)))
		dy++;
	if ((sw >> dx) > 1024 || (sh >> dy) > 1024)
		return 0;
	while (cap)
	{
		u32 w1 = sw >> dx, h1 = sh >> dy, tb = psm == PSM_T8 ? ((w1 + 127) / 128) * 2 : (w1 + 63) / 64;

		if (tex_footprint(psm, w1, h1, tb) <= cap)
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
	*pdx = dx;
	*pdy = dy;
	return 1;
}

static u32 tex_plan_blocks(int psm, u32 sw, u32 sh, u32 dx, u32 dy)
{
	u32 w1 = sw >> dx, h1 = sh >> dy, tb = psm == PSM_T8 ? ((w1 + 127) / 128) * 2 : (w1 + 63) / 64;

	return tex_footprint(psm, w1, h1, tb);
}

// statistics: kind of upload and uploads of the same texture within one frame
static struct
{
	const GLMipmap_t *m;
	u32 frame1;
} up_hist[2048];

static void tx_note_upload(const GLMipmap_t *m, int psm, u32 nblk)
{
	u32 k = psm != PSM_T8 ? 3u : m->regen_kind == 1 ? 0u : m->regen_kind == 2 ? 1u : 2u;
	u32 h = (u32)(((uintptr_t)m >> 4) * 2654435761u) >> 21;

	TX.up_n[k]++;
	TX.up_blk[k] += nblk;
	if (up_hist[h].m == m && up_hist[h].frame1 == H.frame_no + 1)
	{
		TX.reup_n++;
		TX.reup_blk += nblk;
	}
	up_hist[h].m = m;
	up_hist[h].frame1 = H.frame_no + 1;
}

// does a byte of d[0..n) equal 255 (the chroma key / hole index)? Eight bytes per step on the 64-bit R5900 registers.
static int scan_has_ff(const u8 *d, u32 n)
{
	u32 i = 0;

	while (i < n && ((uintptr_t)(d + i) & 7))
	{
		if (d[i] == 255)
			return 1;
		i++;
	}
	if (i + 8 <= n)
	{
		const u64 *w = (const u64 *)(const void *)(d + i);

		for (; i + 8 <= n; i += 8, w++)
		{
			u64 x = ~*w; // a byte of 255 becomes 0

			if ((x - 0x0101010101010101ull) & ~x & 0x8080808080808080ull)
				return 1;
		}
	}
	for (; i < n; i++)
		if (d[i] == 255)
			return 1;
	return 0;
}

// Level `lv` of a P_8 image (PS2-HW-34): every texel stands for a block of 2^lv x 2^lv texels. Palette indices cannot be averaged, so one of
// four samples of the block (its corners inside a margin of a quarter) is taken: the one whose luminance is farthest from the mean of the four,
// which keeps a thin dark or light line visible where picking the middle texel would lose it (the PC driver's point sampling shows such a line
// in a part of the pixels, never in none). Index 255 (hole / chroma key) is kept when at least two samples have it, else it is not picked.
static void dec_level_ref(const u8 *src, u32 sw, u32 sh, u32 lv, u8 *dst)
{
	u32 lum[256], i, x, y, bw = 1u << lv, o0 = bw >> 2, o1 = bw - 1 - o0, ow = sw >> lv, oh = sh >> lv;

	for (i = 0; i < 256; i++)
		lum[i] = ((H.pal[i] & 255u) * 77u + ((H.pal[i] >> 8) & 255u) * 150u + ((H.pal[i] >> 16) & 255u) * 29u) >> 8;
	for (y = 0; y < oh; y++)
	{
		const u8 *r0 = src + (size_t)(y * bw + o0) * sw, *r1 = src + (size_t)(y * bw + o1) * sw;

		for (x = 0; x < ow; x++)
		{
			u32 xa = x * bw + o0, xb = x * bw + o1, s[4], n = 0, sum = 0, best = 0, bestd = 0, k, holes = 0, found = 0;

			s[0] = r0[xa];
			s[1] = r0[xb];
			s[2] = r1[xa];
			s[3] = r1[xb];
			for (k = 0; k < 4; k++)
			{
				if (s[k] == 255)
				{
					holes++;
				}
				else
				{
					sum += lum[s[k]];
					n++;
				}
			}
			if (holes >= 2 || !n)
			{
				dst[(size_t)y * ow + x] = 255;
				continue;
			}
			sum = (sum + (n >> 1)) / n;
			for (k = 0; k < 4; k++)
			{
				u32 d;

				if (s[k] == 255)
					continue;
				d = lum[s[k]] > sum ? lum[s[k]] - sum : sum - lum[s[k]];
				if (!found || d > bestd)
				{
					best = s[k];
					bestd = d;
					found = 1;
				}
			}
			dst[(size_t)y * ow + x] = (u8)best;
		}
	}
}

// The same as dec_level_ref, made fast (PS2-HW-39): 200 cycles per output texel became about 40 (a level 1 of a 1024 x 1024 plane took 55 M cycles = a
// frame hitch of 0.2 s). A block of four equal samples is copied; the division of the mean is a multiplication (exact for the sums that occur).
static void dec_level(const u8 *src, u32 sw, u32 sh, u32 lv, u8 *dst)
{
	u8 lum[256];
	u32 i, x, y, bw = 1u << lv, o0 = bw >> 2, o1 = bw - 1 - o0, ow = sw >> lv, oh = sh >> lv;

	for (i = 0; i < 256; i++)
		lum[i] = (u8)(((H.pal[i] & 255u) * 77u + ((H.pal[i] >> 8) & 255u) * 150u + ((H.pal[i] >> 16) & 255u) * 29u) >> 8);
	for (y = 0; y < oh; y++)
	{
		const u8 *r0 = src + (size_t)(y * bw + o0) * sw + o0, *r1 = src + (size_t)(y * bw + o1) * sw + o0;
		const u32 dxb = o1 - o0; // distance of the second sample column from the first
		u8 *o = dst + (size_t)y * ow;

		for (x = 0; x < ow; x++, r0 += bw, r1 += bw)
		{
			const u32 a = r0[0], b = r0[dxb], c = r1[0], d = r1[dxb];
			u32 sum, mean, la, lb, lc, ld, da, db, dc, dd, best, bestd, holes;

			if (!((a ^ b) | (a ^ c) | (a ^ d)))
			{
				o[x] = (u8)a; // four equal samples: that colour (index 255 stays a hole)
				continue;
			}
			holes = (a == 255) + (b == 255) + (c == 255) + (d == 255);
			if (holes >= 2)
			{
				o[x] = 255;
				continue;
			}
			la = lum[a];
			lb = lum[b];
			lc = lum[c];
			ld = lum[d];
			if (holes)
			{
				// three samples count: the hole contributes nothing
				sum = (a != 255 ? la : 0) + (b != 255 ? lb : 0) + (c != 255 ? lc : 0) + (d != 255 ? ld : 0);
				mean = ((sum + 1) * 21846u) >> 16; // (sum + 1) / 3
			}
			else
			{
				sum = la + lb + lc + ld;
				mean = (sum + 2) >> 2;
			}
			// the sample whose luminance is farthest from the mean (the first of equals); holes are not candidates
			da = a == 255 ? 0 : (la > mean ? la - mean : mean - la);
			db = b == 255 ? 0 : (lb > mean ? lb - mean : mean - lb);
			dc = c == 255 ? 0 : (lc > mean ? lc - mean : mean - lc);
			dd = d == 255 ? 0 : (ld > mean ? ld - mean : mean - ld);
			best = 256; // none yet
			bestd = 0;
			if (a != 255)
			{
				best = a;
				bestd = da;
			}
			if (b != 255 && (best == 256 || db > bestd))
			{
				best = b;
				bestd = db;
			}
			if (c != 255 && (best == 256 || dc > bestd))
			{
				best = c;
				bestd = dc;
			}
			if (d != 255 && (best == 256 || dd > bestd))
			{
				best = d;
				bestd = dd;
			}
			o[x] = (u8)best;
		}
	}
}

// PS2-HW-38: level `lv` of a big raw flat made band by band from its lump into a data cache entry (NULL: no room / the lump cannot be read)
static dcent_t *flat_make_level(GLMipmap_t *m, u32 lv)
{
	enum { BAND = 64 }; // rows per read: a multiple of the block of the coarsest level (32)
	const u32 sw = m->width, sh = m->height, W2 = sw >> lv, H2 = sh >> lv;
	u32 row0, t0 = cyc();
	dcent_t *ne = dc_slot(W2 * H2);
	u8 *band;

	if (!ne || !HWR_PS2_AllocData(W2 * H2, (void **)(void *)&ne->p))
		return NULL;
	HWR_PS2_LockData(ne->p); // the zone may purge an unlocked block at any allocation (malloc below, the lump read): the caller unlocks it when it is done with it
	band = malloc((size_t)sw * BAND);
	if (!band)
	{
		HWR_PS2_FreeData(ne->p);
		ne->p = NULL;
		return NULL;
	}
	for (row0 = 0; row0 < sh; row0 += BAND)
	{
		u32 rows = sh - row0 < BAND ? sh - row0 : BAND;

		if (!HWR_PS2_FlatRows(m, row0, rows, band))
		{
			free(band);
			HWR_PS2_FreeData(ne->p);
			ne->p = NULL;
			return NULL;
		}
		dec_level(band, sw, rows, lv, ne->p + (size_t)(row0 >> lv) * W2);
	}
	free(band);
	ne->key = dc_key(m);
	ne->bytes = W2 * H2;
	ne->w = (u16)W2;
	ne->h = (u16)H2;
	ne->lvl = (u8)lv;
	ne->busy = 0;
	ne->stamp = ++dc_seq;
	dc_bytes += ne->bytes;
	TX.lvl_cyc += cyc() - t0;
	Z_SetRebuildCost((void **)(void *)&ne->p, cyc() - t0); // OPT13 IZ (PS2-603): a level of a 1 MiB cloud plane made from its lump: 10..12 M cycles; the zone evicts it last
	{
		static int logged;

		if (logged++ < 40)
			CONS_Printf("HWD level %u of %s %ux%u made in %u cycles from the lump (bands)\n", (unsigned)lv, HWR_PS2_TexName(m), (unsigned)sw, (unsigned)sh, (unsigned)(cyc() - t0));
	}
	return ne;
}

// The classification of the picture of a patch (AP_88: index, alpha) for tex_upload: partial alpha anywhere (a partial one ends the scan: the others are not valid then), a hole (alpha 0),
// an opaque texel of index 255, a hole whose index is not 255 and whose colour is not the hole colour of the palette. fast: four texels in one 64-bit word (PS2-HW-252, FX3).
static void ap88_scan(const u16 *d16, u32 sw, u32 cw, u32 ch, u32 pal255, int fast, int *partial, int *hole, int *opaque255, int *hole_rgb)
{
	u32 y, x;

	for (y = 0; y < ch && !*partial; y++)
	{
		const u16 *row = d16 + (size_t)y * sw;

		x = 0;
		if (fast)
		{
			// The alpha of a lane (0..255) is 255 when (A + 1) has bit 8 set, not 0 when (A + 255) has it; a lane that is one but not the other is a partial alpha.
			const u64 *r8 = (const u64 *)(const void *)row;
			const u64 lanes = 0x00FF00FF00FF00FFull, one = 0x0001000100010001ull, bit8 = 0x0100010001000100ull;
			const u32 x4 = cw & ~3u;

			for (; x < x4; x += 4)
			{
				const u64 v = *r8++, a = (v >> 8) & lanes, iv = v & lanes;
				const u64 ge1 = (a + lanes) & bit8; // alpha != 0
				const u64 ff = (a + one) & bit8; // alpha == 255
				const u64 i255 = (iv + one) & bit8; // index == 255
				u64 zero;

				if (ge1 != ff)
				{
					*partial = 1;
					return;
				}
				*opaque255 |= (ff & i255) != 0;
				zero = bit8 & ~ge1; // lanes with alpha 0
				if (zero)
				{
					*hole = 1;
					if (zero & ~i255) // a hole whose index is not 255: its colour decides
					{
						u32 k;

						for (k = 0; k < 4; k++)
							if (((zero >> (16 * k + 8)) & 1u) && ((iv >> (16 * k)) & 255u) != 255u && H.pal[(iv >> (16 * k)) & 255u] != pal255)
								*hole_rgb = 1;
					}
				}
			}
		}
		for (; x < cw; x++)
		{
			const u32 v = row[x], a = v >> 8, idx = v & 255u;

			if (a == 255)
			{
				*opaque255 |= idx == 255;
			}
			else if (a == 0)
			{
				*hole = 1;
				if (idx != 255 && H.pal[idx] != pal255)
					*hole_rgb = 1;
			}
			else
			{
				*partial = 1;
				return;
			}
		}
	}
}

// returns the record index or NOREC (nothing was changed then)
static int tex_upload(GLMipmap_t *m)
{
	u32 sw = m->width, sh = m->height;
	u32 W2, H2, bpp, tbw, nblk, align, blk, i, n;
	int psm, clut, baked = 0, holes = 0, uses255 = 0, ri, shareable = 0, has255 = 0;
	rowfill_t fill;
	fillctx_t fc, *c = &fc;
	u32 t0 = cyc();
	const u8 *data = m->data;
	u32 cw = sw, ch = sh; // the part of the block that is stored (PS2-HW-39)
	int crop = 0;
	dcent_t *dce = NULL, *l0 = NULL, *dsrc = NULL, *dlock = NULL; // dlock: a level this call made, locked until the upload has read it
	u32 ssw = sw, ssh = sh, slv = 0; // size and level of the texels `data` holds when it is a cached level (dsrc) and not the full size
	u32 lv = 0; // mip level the texture is stored at (PS2-HW-34); the texels of a level are made by dec_level
	int lvdec = 0; // the stored texels are those of the cache entry of that level
	int split = 0; // PS2-HW-70: the image has more than 1024 rows and is stored as two images (rows [0, H2) and the hb rows after them)
	u32 hb = 0, nblkA = 0;

	tex_fail_why = "ok";
	zc_last = 0;
	if (m->format == GL_TEXFMT_P_8 && (m->regen_kind == 1 || m->regen_kind == 2) && sw * sh >= 2048)
	{
		if (sw * sh >= PLAN_MIN_TEXELS && !H.tex_cap_blocks && sw <= 1024 && sh <= 2048)
		{
			int vis;

			lv = tex_want(m, &vis);
			if (lv && (dce = dc_find(dc_key(m), sw >> lv, sh >> lv, lv)) != NULL)
			{
				data = dce->p; // that level is in the data cache already
				lvdec = 1;
			}
		}
		if (lv && !dce && (dsrc = dc_find_finer(dc_key(m), sw, sh, lv, &slv)) != NULL)
		{
			data = dsrc->p; // PS2-HW-39: a finer level is cached: the coarser one is made from it
			ssw = sw >> slv;
			ssh = sh >> slv;
		}
		if (!dce && !dsrc && (l0 = dc_find(dc_key(m), sw, sh, 0)) != NULL)
			data = l0->p; // the texels of this texture (or its other variant) are in the data cache: the engine need not make them again
		if (lv && !dce && !dsrc && !l0 && !m->data && m->format == GL_TEXFMT_P_8 && HWR_PS2_FlatStreamable(m) && (dce = flat_make_level(m, lv)) != NULL)
		{
			data = dce->p; // PS2-HW-38: made now from the lump; from here on it is a cached level like any other
			dlock = dce;
			lvdec = 1;
		}
	}
	if (!data && lv && m->regen_kind == 2 && m->format == GL_TEXFMT_P_8 && (tex_flatpin = HWR_PS2_FlatPin(m)) != NULL)
		data = tex_flatpin; // PS2-HW-38: a level of a flat is made from the engine's flat itself
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
	// PS2-HW-39: a patch (sprite) is a block of the next power of two size around the real picture (a 100 x 150 sprite is 128 x 256): only the picture
	// is stored (texture coordinates are scaled by the size of the block, clamping at the picture's edge), a quarter to a half of the blocks and bytes
	if (m->regen_kind == 0 && m->format == GL_TEXFMT_AP_88 && m->ps2_uw && m->ps2_uh && m->ps2_uw <= sw && m->ps2_uh <= sh && (m->ps2_uw < sw || m->ps2_uh < sh) && !H.tex_cap_blocks)
	{
		cw = m->ps2_uw;
		ch = m->ps2_uh;
		crop = 1;
	}

	switch (m->format)
	{
		case GL_TEXFMT_P_8:
			psm = PSM_T8;
			clut = (m->flags & TF_CHROMAKEYED) ? 2 : 1;
			fill = fill_idx;
			if (m->regen_kind == 1 || m->regen_kind == 2)
			{
				// PS2-HW-30: the plain and the chroma keyed variant of a map texture / flat have the same texels, the VRAM image is shared
				// (tex_view); the keyed one only needs the keyed CLUT when some texel has the index of the key
				shareable = 1;
				if (lvdec)
				{
					has255 = scan_has_ff(data, (sw >> lv) * (sh >> lv)); // the stored texels are those of the level (not cached: the full size may differ)
				}
				else
				{
					if (!m->ps2_h255)
						m->ps2_h255 = (m->ps2_twin && m->ps2_twin->ps2_h255) ? m->ps2_twin->ps2_h255 : (UINT8)(scan_has_ff(data, dsrc ? ssw * ssh : n) ? 2 : 1);
					has255 = m->ps2_h255 == 2;
				}
				if (clut == 2 && !has255)
					clut = 1;
				holes = clut == 2;
			}
			else if (clut == 2)
			{
				for (i = 0; i < n; i++)
					if (data[i] == 255)
					{
						holes = 1;
						break;
					}
			}
			break;
		case GL_TEXFMT_AP_88:
		{
			int partial = 0, hole = 0, opaque255 = 0, hole_rgb = 0;

			psm = PSM_T8;
			fill = fill_ap88;
			if (m->flags & TF_CHROMAKEYED) // index 255 is the hole, the alpha byte is ignored
			{
				u32 y, x;

				clut = 2;
				c->mode = 1;
				for (y = 0; y < ch && !holes; y++)
					for (x = 0; x < cw; x++)
						if (data[((size_t)y * sw + x) * 2] == 255)
						{
							holes = 1;
							break;
						}
				break;
			}
			{
				// one pass over the stored part (the real picture of a patch): binary alpha (0 / 255), holes, an opaque texel of index 255, holes whose colour is not
				// the hole colour of the palette
				const u16 *d16 = (const u16 *)(const void *)data;
				const int fast = !(ps2hwd_fx2 & FX3_NOFILL) && !((uintptr_t)d16 & 7) && !(sw & 3);

				ap88_scan(d16, sw, cw, ch, H.pal[255], fast, &partial, &hole, &opaque255, &hole_rgb);
				if (fast && (ps2hwd_dbg_flags & HWDBG_COMPOSE)) // -hwdbg 16777216: the scan again texel by texel, the same answer
				{
					static unsigned scanned, sbad;
					int p2 = 0, h2 = 0, o2 = 0, r2 = 0;

					ap88_scan(d16, sw, cw, ch, H.pal[255], 0, &p2, &h2, &o2, &r2);
					scanned++;
					if (partial != p2 || (!partial && (hole != h2 || opaque255 != o2 || hole_rgb != r2)))
					{
						sbad++;
						CONS_Printf("HWC scan MISMATCH %u of %u (%dx%d: %d %d %d %d against %d %d %d %d)\n", sbad, scanned, (int)cw, (int)ch, partial, hole, opaque255, hole_rgb, p2, h2, o2, r2);
					}
					else if (!(scanned & 255))
					{
						CONS_Printf("HWC scan check: %u patches, %u differ\n", scanned, sbad);
					}
				}
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
	// in a frame: a texture over the limit (or over the footprint cap in force) is stored decimated by powers of two (PS2-HW-20).
	// OPT8 (PS2-HW-24): the cap is chosen by tex_cap_control from the working set, and a texture that does not fit the pool even
	// after everything else was evicted lowers the cap and is made smaller instead of being skipped (a skipped draw is a hole).
	bpp = psm == PSM_T8 ? 1 : 4;
	for (;;)
	{
		u32 dx = 0, dy = 0;
		const int fit = lv && !H.tex_cap_blocks && (sw >> lv) <= 1024 && (sh >> lv) <= 1024; // the level the plan asks for is within the GS limit

		if (!fit && !tex_plan(psm, sw, sh, H.tex_cap_blocks, shareable && !crop, &dx, &dy))
		{
			tex_fail_why = "larger than 1024 and not divisible";
			return NOREC;
		}
		if (lv && !dx && !dy && psm == PSM_T8 && shareable)
			dx = dy = lv; // stored at level lv: the texels come from dec_level (or the cache) below, the plain decimation of the fill functions is the fallback
		else
			lv = 0;
		c->dx = dx;
		c->dy = dy;
		W2 = sw >> dx;
		H2 = sh >> dy;
		if (crop)
		{
			W2 = cw;
			H2 = ch;
		}
		split = !lv && !dx && !dy && !crop && shareable && psm == PSM_T8 && sw <= 1024 && sh > tex_split_rows && sh <= 2 * tex_split_rows && !tex_nosplit && !H.tex_cap_blocks; // tex_plan let it through whole (or it is a test: -hwsplitrows)
		hb = 0;
		if (split)
		{
			H2 = tex_split_rows;
			hb = sh - tex_split_rows;
		}
		tbw = psm == PSM_T8 ? ((W2 + 127) / 128) * 2 : (W2 + 63) / 64;
		nblkA = tex_footprint(psm, W2, H2, tbw);
		if (split)
			nblkA = (nblkA + PAGE_BLOCKS - 1) & ~(u32)(PAGE_BLOCKS - 1); // the second piece starts on a page (the first one owns the few blocks in between)
		nblk = nblkA;
		if (split)
			nblk += tex_footprint(psm, W2, hb, tbw); // both pieces in one range
		if (cfg.tex_max_bytes > 0 && nblk > (u32)cfg.tex_max_bytes / 256u)
		{
			tex_fail_why = "footprint over budget";
			hw_failure("exact texture GS footprint exceeds configured budget");
			return NOREC;
		}
		align = 1;
		while (align < nblk && align < PAGE_BLOCKS)
			align <<= 1;
		blk = vram_alloc_evicting(nblk, align, nblk >= BIG_IMAGE_BLOCKS ? 1 : 0); // PS2-HW-39: big images from the top of the pool, small ones from the bottom: they do not cut each other's room into pieces
		if (blk)
			break;
		if (!H.cap_adapt || H.cap_idx >= NCAPS - 1)
		{
			tex_fail_why = "no VRAM block (pool)";
			return NOREC;
		}
		cap_step(H.cap_idx + 1, "texture does not fit the pool");
	}
	if (lv)
	{
		// level lv of the image: from the data cache, or made now from the full size texels (and kept in the cache)
		if (!dce)
		{
			dcent_t *ne;
			void *srcblk = dsrc ? (void *)dsrc->p : l0 ? (void *)l0->p : (void *)m->data;
			u32 dt0 = cyc();

			if (srcblk && data == (const u8 *)srcblk)
				HWR_PS2_LockData(srcblk); // the allocation below may purge the zone's caches, the source among them
			if (l0)
				l0->busy++;
			if (dsrc)
				dsrc->busy++;
			ne = dc_slot(W2 * H2);
			if (ne && HWR_PS2_AllocData(W2 * H2, (void **)(void *)&ne->p))
			{
				HWR_PS2_LockData(ne->p); // not purgable until the upload has read it (the check below, the record tables: any allocation may purge a free block)
				dlock = ne;
				dec_level(data, ssw, ssh, lv - slv, ne->p);
				if (ps2hwd_dbg_flags & HWDBG_COMPOSE)
				{
					// the reference version of the same level: the bytes must be the same (PS2-HW-39)
					static unsigned dchecked, dbad;
					u8 *ref = malloc((size_t)W2 * H2);

					if (ref)
					{
						dec_level_ref(data, ssw, ssh, lv - slv, ref);
						dchecked++;
						if (memcmp(ref, ne->p, (size_t)W2 * H2))
						{
							u32 kk = 0;

							dbad++;
							while (kk < W2 * H2 && ref[kk] == ne->p[kk])
								kk++;
							CONS_Printf("HWC level MISMATCH %s %ux%u level %u (%u of %u differ)\n", HWR_PS2_TexName(m), (unsigned)sw, (unsigned)sh, (unsigned)lv, dbad, dchecked);
							CONS_Printf("HWC   first at %u of %u: made %u, reference %u; source %p %ux%u (slv %u, from %s, m->data %p) output %p %u bytes\n", (unsigned)kk, (unsigned)(W2 * H2), (unsigned)ne->p[kk], (unsigned)ref[kk],
								(const void *)data, (unsigned)ssw, (unsigned)ssh, (unsigned)slv, dsrc ? "a cached level" : l0 ? "the cached full size" : "the engine's data", (const void *)m->data, (const void *)ne->p, (unsigned)(W2 * H2));
						}
						else if (!(dchecked & 7))
						{
							CONS_Printf("HWC level check: %u levels identical, %u differ\n", dchecked - dbad, dbad);
						}
						free(ref);
					}
				}
				TX.lvl_cyc += cyc() - dt0;
				Z_SetRebuildCost((void **)(void *)&ne->p, cyc() - dt0); // OPT13 IZ (PS2-603)
				{
					static int dl_logged;

					if (dl_logged++ < 40)
						CONS_Printf("HWD level %u of %s %ux%u made in %u cycles from %s\n", (unsigned)lv, HWR_PS2_TexName(m), (unsigned)sw, (unsigned)sh, (unsigned)(cyc() - dt0),
							dsrc ? "a cached level" : l0 ? "the cached full size" : tex_flatpin ? "the engine's flat" : "the engine's data");
				}
				ne->key = dc_key(m);
				ne->bytes = W2 * H2;
				ne->w = (u16)W2;
				ne->h = (u16)H2;
				ne->lvl = (u8)lv;
				ne->busy = 0;
				ne->stamp = ++dc_seq;
				dc_bytes += ne->bytes;
				dce = ne;
				data = ne->p;
			}
			if (l0)
				l0->busy--;
			if (dsrc)
				dsrc->busy--;
			if (srcblk && data == (const u8 *)srcblk)
				HWR_PS2_UnlockData(srcblk);
		}
		else
		{
			data = dce->p;
		}
		if (!dce && dsrc)
			c->dx = c->dy = lv - slv; // no room for the level: plain decimation of the cached level
		if (dce)
		{
			lvdec = 1;
			c->dx = c->dy = 0; // the texels are decimated already
		}
		TX.lvl_up[lv > 5 ? 5 : lv]++;
	}
	else if (split)
	{
		static int slogged;

		if (slogged++ < 8)
			CONS_Printf("HWD split texture %s %ux%u -> %ux%u + %ux%u (blocks %u + %u)\n", HWR_PS2_TexName(m), (unsigned)sw, (unsigned)sh, (unsigned)W2, (unsigned)H2, (unsigned)W2, (unsigned)hb, (unsigned)nblkA, (unsigned)(nblk - nblkA));
	}
	else if (c->dx || c->dy)
	{
		static int logged;

		H.st.tex_decimated++;
		if (logged++ < 8)
			CONS_Printf("HWD decimated texture %s %ux%u -> %ux%u (dx %u dy %u, cap %u)\n", HWR_PS2_TexName(m), (unsigned)sw, (unsigned)sh, (unsigned)(sw >> c->dx), (unsigned)(sh >> c->dy), (unsigned)c->dx, (unsigned)c->dy, (unsigned)H.tex_cap_blocks);
	}
	if (lv && !dce)
	{
		static int logged2;

		if (logged2++ < 8)
			CONS_Printf("HWD level %u of %s %ux%u made without the data cache (no zone memory): plain decimation\n", (unsigned)lv, HWR_PS2_TexName(m), (unsigned)sw, (unsigned)sh);
	}

	if (!dce && l0 && !lv && !c->dx && !c->dy)
		dce = l0; // the full size texels are in the data cache: the upload reads them from there
	c->src = data;
	c->sw = ssw;
	c->sh = ssh;
	c->W2 = W2;
	c->H2 = H2;

	if (!dce && fill == fill_idx && psm == PSM_T8 && !c->dx && !c->dy && data == (const u8 *)m->data && (m->regen_kind == 1 || m->regen_kind == 2)
		&& n >= 2048 && !(ps2hwd_dbg_flags & HWDBG_NOZC) && !((uintptr_t)data & 15) && (dce = dc_slot(n)) != NULL)
	{
		// the engine's block becomes the driver's: purgable zone block owned by the cache entry, the engine keeps no pointer to it
		void *p = HWR_PS2_StealData(m, (void **)(void *)&dce->p);

		dce->key = dc_key(m);
		dce->bytes = n;
		dce->w = (u16)sw;
		dce->h = (u16)sh;
		dce->lvl = 0;
		dce->busy = 0;
		dce->stamp = ++dc_seq;
		dc_bytes += n;
		data = (const u8 *)p;
		c->src = data;
	}
	if (dce && fill == fill_idx && psm == PSM_T8 && !c->dx && !c->dy && !(ps2hwd_dbg_flags & HWDBG_NOZC) && !((uintptr_t)data & 15))
	{
		// PS2-HW-32: the texels go by DMA reference, from the cache entry (locked until the DMA has read it)
		rel_room(n);
		upload_ref(blk, tbw, psm, W2, H2, data);
		rel_add(dce);
		if (split)
		{
			upload_ref(blk + nblkA, tbw, psm, W2, hb, data + (size_t)H2 * sw); // rows 1024 on: 1024 * sw is a multiple of 16
			rel_add(dce);
		}
		zc_last = 1;
		TX.zc_n++;
		TX.zc_blk += nblk;
	}
	else
	{
		upload_bands(blk, tbw, psm, W2, H2, bpp, fill, c);
		if (split)
		{
			c->src = data + (size_t)H2 * sw;
			c->H2 = hb;
			upload_bands(blk + nblkA, tbw, psm, W2, hb, bpp, fill, c);
		}
	}

	ri = rec_new();
	if (ri == NOREC)
	{
		tex_fail_why = "record table";
		vram_free(blk, nblk);
		if (dlock && dlock->p && !dlock->busy)
			HWR_PS2_UnlockData(dlock->p);
		return NOREC;
	}
	{
		texrec_t *r = &H.rec[ri];

		r->owner = m;
		r->blk = blk;
		r->nblk = nblkA;
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
		r->sxs = (float)(crop ? sw : W2) / (float)(1u << r->tw); // the engine's texture coordinate 1.0 is the whole block: sw texels, of which W2 are stored
		r->sys = (float)(crop ? sh : H2) / (float)(1u << r->th);
		if (holes && clut == 2 && (m->format == GL_TEXFMT_P_8 || m->format == GL_TEXFMT_AP_88))
			m->flags |= TF_TRANSPARENT; // as r_opengl.c does for a chroma keyed texture with a hole
		r->nearest_only = (m->flags & TF_TRANSPARENT) ? 1 : 0;
		r->shareable = (u8)shareable;
		r->has255 = (u8)has255;
		r->sw = (u16)sw;
		r->sh = (u16)sh;
		r->dx = (u8)(lvdec ? lv : c->dx);
		r->dy = (u8)(lvdec ? lv : c->dy);
		r->capi = (u8)H.cap_idx;
		lru_push_head(ri);
	}
	if (split)
	{
		// PS2-HW-70: the second piece: its own image (record, blocks) with the texels of the rows after the first piece; the engine's texture is bound to the first
		const int bi = rec_new();
		texrec_t *a, *b;

		if (bi == NOREC)
		{
			tex_fail_why = "record table";
			vram_free(blk + nblkA, nblk - nblkA);
			tex_drop(ri, 0);
			if (dlock && dlock->p && !dlock->busy)
				HWR_PS2_UnlockData(dlock->p);
			return NOREC;
		}
		a = &H.rec[ri];
		b = &H.rec[bi];
		*b = *a;
		b->prev = b->next = NOREC;
		b->share = b->sec = b->sec_next = NOREC;
		b->blk = blk + nblkA;
		b->nblk = nblk - nblkA;
		b->h = (u16)hb;
		b->th = (u8)log2_u32(hb);
		b->vh = hb;
		b->npot_y = (hb & (hb - 1)) != 0;
		b->region = (u8)((b->npot_x ? 1 : 0) | (b->npot_y ? 2 : 0));
		b->sys = (float)hb / (float)(1u << b->th);
		b->psplit = 2;
		b->ppar = ri;
		b->pnext = NOREC;
		b->pt0 = (float)H2 / (float)sh;
		b->pt1 = 1.0f;
		b->ptk = (float)sh / (float)hb;
		a->psplit = 1;
		a->pnext = bi;
		a->pt0 = 0.0f;
		a->pt1 = (float)H2 / (float)sh;
		a->ptk = (float)sh / (float)H2;
		own_set(bi);
		lru_push_head(bi);
		TX.split_up++;
	}
	m->downloaded = (UINT32)(ri + 1);
	H.st.uploads++;
	H.st.upload_bytes += W2 * (H2 + hb) * bpp;
	H.st.cyc_tex += cyc() - t0;
	tx_note_upload(m, psm, nblk);
	if (dlock && dlock->p && !dlock->busy)
		HWR_PS2_UnlockData(dlock->p); // a zero-copy upload keeps the lock (busy) until its DMA has read the texels
	return ri;
}

// PS2-HW-30: a record for the other CLUT variant (chroma keyed / plain) of the image that record `owner_rec` owns or views. It has the
// texels of the image, its own owner, flags and CLUT kind; the blocks belong to the image (see tex_drop). Returns the record or NOREC.
static int tex_view(GLMipmap_t *m, int owner_rec)
{
	int ow = img_of(owner_rec), ri = rec_new();
	texrec_t *r, *o;

	if (ri == NOREC)
		return NOREC;
	o = &H.rec[ow];
	r = &H.rec[ri];
	*r = *o;
	r->prev = r->next = NOREC;
	r->pnext = r->ppar = NOREC; // a view of the first piece: its image (img_of) knows the second piece
	r->used = 1;
	r->pin = 0;
	r->owner = m;
	r->share = ow;
	r->sec = NOREC;
	r->sec_next = o->sec;
	o->sec = ri;
	r->wrapx = (m->flags & TF_WRAPX) ? 1 : 0;
	r->wrapy = (m->flags & TF_WRAPY) ? 1 : 0;
	r->clut = (u8)(((m->flags & TF_CHROMAKEYED) && o->has255) ? 2 : 1);
	if (r->clut == 2)
		m->flags |= TF_TRANSPARENT; // as tex_upload does for a chroma keyed texture with a hole
	r->nearest_only = (m->flags & TF_TRANSPARENT) ? 1 : 0;
	r->stamp = H.frame_no;
	r->done = 0;
	r->ws = 0;
	m->downloaded = (UINT32)(ri + 1);
	return ri;
}

// PS2-HW-34 (instrumentation, later the planner): the mip level a polygon needs. On the plane of the polygon the clip coordinates x, y, w are
// affine in the texel coordinates (u, v): x = x0 + ax u + bx v, ... From three vertices of the unclipped polygon (their clip coordinates and
// texel coordinates) the coefficients follow, and at every vertex of the CLIPPED polygon (the part the view sees) the screen size of a texel
// is the length of (d(x/w)/du, d(y/w)/du) in pixels (and the same along v). A texel of level L is 2^L times wider: level L is enough when the
// largest texel anywhere in the visible polygon (it is at a vertex: the size is monotone along every line) still covers at most one pixel
// at level L, i.e. nothing is magnified compared with level 0.
static float lodc[3][2]; // d(x, y, w) / d(u, v) of the polygon being drawn (poly_plane)
static int lodc_ok;
static int lodc_planar; // every vertex of the polygon lies on the plane the three vertices define (a kinked polygon, the horizon fill of the engine, does not)

static void poly_plane(const cv_t *un, int n, float sw, float sh)
{
	float u1 = (un[1].a[0] - un[0].a[0]) * sw, v1 = (un[1].a[1] - un[0].a[1]) * sh, det = 0.0f, u2 = 0.0f, v2 = 0.0f;
	int k, third = 2;

	lodc_ok = 0;
	if (n < 3)
		return;
	for (; third < n; third++) // a vertex that is not on the line through the first two
	{
		u2 = (un[third].a[0] - un[0].a[0]) * sw;
		v2 = (un[third].a[1] - un[0].a[1]) * sh;
		det = u1 * v2 - u2 * v1;
		if (fabsf(det) > 1e-2f * (fabsf(u1) + fabsf(v1) + 1e-3f) * (fabsf(u2) + fabsf(v2) + 1e-3f))
			break;
	}
	if (third >= n)
		return;
	{
		const float rdet = 1.0f / det; // PS2-HW-229: one division instead of six

		for (k = 0; k < 3; k++)
		{
			float c0 = k == 0 ? un[0].x : k == 1 ? un[0].y : un[0].w, c1 = k == 0 ? un[1].x : k == 1 ? un[1].y : un[1].w, cc = k == 0 ? un[third].x : k == 1 ? un[third].y : un[third].w;

			lodc[k][0] = ((c1 - c0) * v2 - (cc - c0) * v1) * rdet;
			lodc[k][1] = ((cc - c0) * u1 - (c1 - c0) * u2) * rdet;
		}
	}
	lodc_ok = 1;
	lodc_planar = 1;
	for (k = 2; k < n; k++) // the other vertices must be where the plane puts them
	{
		float du = (un[k].a[0] - un[0].a[0]) * sw, dv = (un[k].a[1] - un[0].a[1]) * sh;
		float px = un[0].x + lodc[0][0] * du + lodc[0][1] * dv, py = un[0].y + lodc[1][0] * du + lodc[1][1] * dv, pw = un[0].w + lodc[2][0] * du + lodc[2][1] * dv;
		float tol = 2e-3f * (fabsf(un[k].x) + fabsf(un[k].y) + fabsf(un[k].w) + 1.0f);

		if (k == third)
			continue;
		if (fabsf(px - un[k].x) > tol || fabsf(py - un[k].y) > tol || fabsf(pw - un[k].w) > tol)
		{
			lodc_planar = 0;
			break;
		}
	}
}

static float lod_su, lod_sv; // the largest texel (pixels) along u and v that poly_lod found (the plan, ps2_hw_plan.inc; -hwdbg 0x8000000)

// the screen size (pixels) of one texel along u and along v at every vertex of the clipped polygon `cl` (in the viewport, w > 0): the plane
// coefficients lodc come from poly_plane. lod_su / lod_sv = the largest; returns 0 when the polygon cannot be measured.
static int poly_density(const cv_t *cl, int cnt)
{
	float px = 0.5f * H.vpw, py = 0.5f * H.vph, su = 0.0f, sv = 0.0f;
	int i;

	lod_su = lod_sv = 0.0f;
	if (!lodc_ok || cnt < 3)
		return 0;
	for (i = 0; i < cnt; i++)
	{
		float w = cl[i].w, iw2, dxs, dys, sz;

		if (w < 1e-4f)
			return 0;
		iw2 = 1.0f / (w * w);
		dxs = (lodc[0][0] * w - cl[i].x * lodc[2][0]) * iw2 * px;
		dys = (lodc[1][0] * w - cl[i].y * lodc[2][0]) * iw2 * py;
		sz = dxs * dxs + dys * dys;
		if (sz > su)
			su = sz;
		dxs = (lodc[0][1] * w - cl[i].x * lodc[2][1]) * iw2 * px;
		dys = (lodc[1][1] * w - cl[i].y * lodc[2][1]) * iw2 * py;
		sz = dxs * dxs + dys * dys;
		if (sz > sv)
			sv = sz;
	}
	lod_su = sqrtf(su);
	lod_sv = sqrtf(sv);
	return 1;
}

// the coarsest level at which no texel is magnified by more than `tol` (1 = nothing is magnified compared with the full size)
static int lod_level_for(float smax, float tol)
{
	int lvl = 0;

	while (lvl < 5 && smax * 2.0f <= tol)
	{
		smax *= 2.0f;
		lvl++;
	}
	return lvl;
}

static int poly_lod(const cv_t *cl, int cnt)
{
	if (!poly_density(cl, cnt))
		return 0;
	return lod_level_for(lod_su > lod_sv ? lod_su : lod_sv, 1.0f);
}

static void lod_note(const cv_t *cl, int cnt)
{
	texrec_t *im = &H.rec[img_of(H.cur_tex)];
	int lvl = poly_lod(cl, cnt);

	if (im->lodfr != H.frame_no + 1)
	{
		im->lodfr = H.frame_no + 1;
		im->lod = (u8)lvl;
	}
	else if (lvl < im->lod)
	{
		im->lod = (u8)lvl;
	}
}

// the working set of the frame as it would be with each image kept at the level its polygons need (called for every image when the frame
// ends and when an image is dropped: an image evicted during the frame is part of the working set too)
static void lod_account(texrec_t *r)
{
	u32 b;

	if (!r->used || r->share != NOREC || r->screen || r->lodfr != H.frame_no + 1)
		return;
	r->lodfr = 0; // once per frame and image
	b = r->nblk >> (2 * r->lod);
	if (!b)
		b = 1;
	TX.lod_blk += b;
	TX.lod_lvl[r->lod] += r->nblk;
}

static void lod_frame_end(void)
{
	int i;

	for (i = 0; i < H.rec_n; i++)
		lod_account(&H.rec[i]);
}

// HWTEX line of the window just ended (per-frame averages of the block counts; counts of uploads are window totals)
static unsigned spr_hid_test, spr_hid_yes; // PS2-HW-72 (ps2_hw_plan.inc)
static int qh_mode; // PS2-HW-220
static int plan_mode; // PS2-HW-229 (-hwplan)
static unsigned pm_cmp, pm_dcnt, pm_dval; // PS2-HW-229: -hwplan 2: polygons compared, different number of parts, different values
static unsigned qh_diff;

static void tx_report(void)
{
	static int win;
	u32 f = TX.frames ? TX.frames : 1;

	CONS_Printf("HWTEX win=%d frames=%u need=%u/%u up_map=%u/%u up_flat=%u/%u up_t8o=%u/%u up_ct32=%u/%u reup=%u/%u shared=%u regen=%u/%u zc=%u/%u upcyc=%u pool=%u/%u free=%d lod=%u lvl=%u/%u/%u/%u/%u/%u lvlup=%u/%u/%u/%u/%u upg=%u/%u invis=%u plan=%u/%u unpl=%u res=%u inplan=%u outplan=%u plancyc=%u immcyc=%u mkpatch=%u/%u allocfail=%u/%u/%u thrash=%u/%u pct=%u lvlcyc=%u synccyc=%u planpoly=%u planmeas=%u planinv=%u\n",
		win++, (unsigned)TX.frames, (unsigned)(TX.need_blk / f), (unsigned)(TX.need_n / f),
		(unsigned)TX.up_n[0], (unsigned)(TX.up_blk[0] / f), (unsigned)TX.up_n[1], (unsigned)(TX.up_blk[1] / f), (unsigned)TX.up_n[2], (unsigned)(TX.up_blk[2] / f),
		(unsigned)TX.up_n[3], (unsigned)(TX.up_blk[3] / f), (unsigned)TX.reup_n, (unsigned)(TX.reup_blk / f), (unsigned)TX.shared, (unsigned)TX.regen_n, (unsigned)(TX.regen_cyc / f),
		(unsigned)TX.zc_n, (unsigned)(TX.zc_blk / f), (unsigned)(TX.up_cyc / f), (unsigned)H.used_blocks, (unsigned)H.pool_blocks, H.free_n,
		(unsigned)(TX.lod_blk / f), (unsigned)(TX.lod_lvl[0] / f), (unsigned)(TX.lod_lvl[1] / f), (unsigned)(TX.lod_lvl[2] / f), (unsigned)(TX.lod_lvl[3] / f), (unsigned)(TX.lod_lvl[4] / f), (unsigned)(TX.lod_lvl[5] / f),
		(unsigned)TX.lvl_up[1], (unsigned)TX.lvl_up[2], (unsigned)TX.lvl_up[3], (unsigned)TX.lvl_up[4], (unsigned)TX.lvl_up[5], (unsigned)TX.upgrades, (unsigned)TX.downgrades, (unsigned)TX.invisible,
		(unsigned)(TX.plan_blk / f), (unsigned)(TX.plan_exact / f), (unsigned)(TX.unplanned / f), (unsigned)H.plan_reserve, (unsigned)(TX.need_in_plan / f), (unsigned)(TX.need_out_plan / f), (unsigned)(TX.plan_cyc / f), (unsigned)(TX.imm_cyc / f), ps2hwt_mkpatch_n, ps2hwt_mkpatch_cyc / f, (unsigned)TX.alloc_fail, (unsigned)TX.alloc_frag, (unsigned)TX.alloc_frag_blk, (unsigned)(TX.thrash_n / f), (unsigned)(TX.thrash_blk / f), (unsigned)H.plan_pct, (unsigned)(TX.lvl_cyc / f), (unsigned)(TX.sync_cyc / f), (unsigned)(TX.plan_np / f), (unsigned)(TX.plan_mcyc / f), (unsigned)(TX.plan_inv / f));
	ps2hwt_mkpatch_n = ps2hwt_mkpatch_cyc = 0;
	if (spr_hid_test)
	{
		CONS_Printf("HWSPRCULL win=%d tests=%u hidden=%u (per frame %u / %u) differ=%u\n", win - 1, spr_hid_test, spr_hid_yes, spr_hid_test / f, spr_hid_yes / f, qh_diff);
		spr_hid_test = spr_hid_yes = 0;
	}
	if (pm_cmp)
	{
		CONS_Printf("HWPLANCMP win=%d compared=%u cntdiff=%u valdiff=%u\n", win - 1, pm_cmp, pm_dcnt, pm_dval);
		pm_cmp = pm_dcnt = pm_dval = 0;
	}
	if (TX.split_up || TX.split_poly)
		CONS_Printf("HWSPLIT win=%d uploads=%u polygons=%u parts=%u\n", win - 1, (unsigned)TX.split_up, (unsigned)TX.split_poly, (unsigned)TX.split_sub);
	memset(&TX, 0, sizeof TX);
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
			if (H.rec[i].share == NOREC) // a view owns no blocks
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
