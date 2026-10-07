// PS2 GS hardware renderer driver: shared definitions (included by ps2_hwd.c; not a separate translation unit).
// Design and VRAM layout: docs/HW_RENDERER.md.

// GS registers (A+D addresses)
#define GSR_PRIM 0x00
#define GSR_RGBAQ 0x01
#define GSR_ST 0x02
#define GSR_UV 0x03
#define GSR_XYZ2 0x05
#define GSR_TEX0_1 0x06
#define GSR_CLAMP_1 0x08
#define GSR_FOG 0x0a
#define GSR_TEX1_1 0x14
#define GSR_MIPTBP1_1 0x34
#define GSR_MIPTBP2_1 0x36
#define GSR_XYOFFSET_1 0x18
#define GSR_PRMODECONT 0x1a
#define GSR_TEXCLUT 0x1c
#define GSR_FOGCOL 0x3d
#define GSR_TEXA 0x3b
#define GSR_TEXFLUSH 0x3f
#define GSR_SCISSOR_1 0x40
#define GSR_ALPHA_1 0x42
#define GSR_DIMX 0x44
#define GSR_DTHE 0x45
#define GSR_COLCLAMP 0x46
#define GSR_TEST_1 0x47
#define GSR_PABE 0x49
#define GSR_FBA_1 0x4a
#define GSR_FRAME_1 0x4c
#define GSR_ZBUF_1 0x4e
#define GSR_BITBLTBUF 0x50
#define GSR_TRXPOS 0x51
#define GSR_TRXREG 0x52
#define GSR_TRXDIR 0x53
#define GSR_FINISH 0x61

// pixel formats as used in TEX0/BITBLTBUF (6 bits) ...
#define PSM_CT32 0x00
#define PSM_CT16 0x02
#define PSM_CT16S 0x0A
#define PSM_T8 0x13
#define PSM_Z24 0x31
#define PSM_Z32 0x30
// ... and in FRAME/ZBUF (the Z formats there are 4 bits)
#define ZPSM_32 0x00
#define ZPSM_24 0x01
#define ZPSM_16S 0x0A

// GS memory
#define VRAM_BLOCKS 16384 // 4 MiB in 256-byte blocks
#define PAGE_BLOCKS 32 // 8 KiB page
#define VRAM_RANGES (VRAM_BLOCKS / 2 + 1) // worst case: alternating one-block free/used ranges

// DMA channel 2 (GIF)
#define D2_CHCR ((volatile u32 *)0x1000A000)
#define D2_MADR ((volatile u32 *)0x1000A010)
#define D2_QWC ((volatile u32 *)0x1000A020)
#define D2_TADR ((volatile u32 *)0x1000A030)

// primitive types
#define PRIM_POINT 0
#define PRIM_LINE 1
#define PRIM_LINESTRIP 2
#define PRIM_TRI 3
#define PRIM_TRISTRIP 4
#define PRIM_TRIFAN 5
#define PRIM_SPRITE 6

typedef union
{
	u64 d[2];
	u32 w[4];
	float f[4];
} __attribute__((aligned(16))) qw_t;

static inline u32 cyc(void)
{
#ifdef __mips__
	u32 v;
	__asm__ volatile("mfc0 %0,$9" : "=r"(v));
	return v;
#else
	return 0; // host test build
#endif
}

static inline u32 phys(const void *p)
{
	return (u32)(uintptr_t)p & 0x0FFFFFFFu;
}

// OPT10 (PS2-HW-53): floorf of newlib is a call of ~40 cycles; the integer conversion gives the same value for every float of int range
// (above 2^24 a float is an integer already), and the EE has MIN.S / MAX.S instructions that gcc does not use for the ?: form (NaN rules).
static inline float hw_floorf(float x)
{
	if (x > -2147483000.0f && x < 2147483000.0f)
	{
		const int i = (int)x;

		return (float)(i - (x < (float)i));
	}
	return __builtin_floorf(x);
}
#define floorf hw_floorf

#ifdef __mips__
static inline float hw_minf(float a, float b)
{
	float r;

	__asm__("min.s %0,%1,%2" : "=f"(r) : "f"(a), "f"(b));
	return r;
}

static inline float hw_maxf(float a, float b)
{
	float r;

	__asm__("max.s %0,%1,%2" : "=f"(r) : "f"(a), "f"(b));
	return r;
}
#else
static inline float hw_minf(float a, float b)
{
	return b < a ? b : a;
}

static inline float hw_maxf(float a, float b)
{
	return b > a ? b : a;
}
#endif

#define HWGIF_TAG(nloop, eop, pre, prim, flg, nreg) \
	((u64)(nloop) | ((u64)(eop) << 15) | ((u64)(pre) << 46) | ((u64)(prim) << 47) | ((u64)(flg) << 58) | ((u64)(nreg) << 60))
#define GIF_REGS_AD 0xEull
#define GIF_REGS_STQ_RGBA_XYZ (2ull | (1ull << 4) | (5ull << 8)) // ST, RGBAQ, XYZ2
#define GIF_REGS_STQ_RGBA_XYZF (2ull | (1ull << 4) | (4ull << 8)) // ST, RGBAQ, XYZF2
#define GIF_REGS_RGBA_XYZ (1ull | (5ull << 4)) // RGBAQ, XYZ2
#define GIF_REGS_STQ_XYZ (2ull | (5ull << 4)) // ST, XYZ2: the colour was set before (flat primitives)
#define GIF_REGS_STQ_XYZF (2ull | (4ull << 4)) // ST, XYZF2: as above with a fog coefficient per vertex
#define GIF_REGS_XYZ (5ull) // XYZ2
#define GIF_REGS_XYZF (4ull) // XYZF2
#define GIF_REGS_UV_XYZ (3ull | (5ull << 4)) // UV, XYZ2 (sprites with texel coordinates)

#define PS2HWD_MAXPOLY 192 // vertices of one polygon; longer fans are split
#define PS2HWD_CLIPEXTRA 12 // extra vertices clipping can add (6 planes)
#define PS2HWD_ZMAX 16777215.0f // Z24

// Poly flags that arrive from the engine (enum EPolyFlags) are used directly.

// ---- configuration and run-time state ----
static ps2hwd_config_t cfg = {
	PS2HWD_VIDEO_AUTO, 0, 0, 1, 0, 1, 0, 3, 320, 200, 0, 0, 0, 0,
};

#define NBUF 4
#define BUF_QW 8192 // 128 KiB per DMA buffer
#define NSCR NUMSCREENTEXTURES

// Dynamic CLUT images (CT32 16x16, 1 KiB each): palette variants for tint, multiplicative channels, masks...
#define CLUT_FIXED 4 // opaque, keyed, alpha ramp, white keyed: always present
#define CLUT_SLOTS 124 // fixed + cache: 124 KiB, rounded up to 16 pages
#define CLUT_BLOCKS 4 // 256-byte blocks per CLUT image

typedef struct
{
	u64 key; // kind | param << 8; 0 = free
	u32 stamp;
	u32 gen; // palette generation it was built from
} clutent_t;

// OPT8 (PS2-HW-24): footprint caps of one texture in 256-byte blocks. Index 0 = no cap (the GS limit of 1024 texels per axis only).
// The adaptive controller (tex_cap_control) picks the largest cap whose predicted working set fits the pool.
#define NCAPS 8
static const u32 CAPS[NCAPS] = {0u, 4096u, 1024u, 512u, 256u, 128u, 64u, 32u};

typedef struct
{
	GLMipmap_t *owner;
	u32 blk; // first 256-byte block in VRAM
	u32 nblk;
	u16 w, h; // exact engine texel dimensions; unsupported sizes fail without resampling
	u8 tw, th; // log2 of the power of two >= w, h (TEX0 TW/TH)
	u8 tbw;
	u8 psm;
	u8 clut; // 0 none (direct colour), 1 palette, 2 palette with index 255 = hole, 3 alpha ramp (ALPHA_8)
	u8 wrapx, wrapy;
	u8 nearest_only; // engine TF_TRANSPARENT: nearest filtering, matching the PC driver's policy
	u8 baked; // direct colour made from the palette: must be rebuilt when the palette changes
	u8 screen; // screen texture slot + 1 (0 = ordinary texture)
	u8 notcc; // the texture has no alpha of its own (TCC = 0)
	u8 region; // only w x h texels are valid: clamp to that region (REGION_CLAMP) instead of the power of two size
	u8 npot_x, npot_y; // the size is not a power of two: a repeat is made by cutting the polygon at whole texture widths
	u8 used;
	u8 pin; // never evicted (the depth ramp)
	u32 stamp;
	u32 seq; // position in the recency order (grows with every touch); eviction weights use it
	u32 done; // frame_no + 1 of the frame in which a batch drew the texture
	u32 ws; // frame_no + 1 of the last frame that counted the texture into the working set statistics
	u32 wgt; // eviction weight per block, set by best_window()
	int prev, next; // LRU list, indices into the table (-1 = none)
	u32 uw, vh; // the used part of a screen texture in texels
	float sxs, sys; // engine texture coordinate -> GS ST scale: w / 2^tw, h / 2^th (1 for power of two sizes)
	u16 sw, sh; // OPT8: the engine's size of the texture (the stored size is w x h = sw >> dx, sh >> dy)
	u8 dx, dy; // decimation shifts of the stored texture
	u8 capi; // footprint cap index (CAPS[]) the stored plan was made for
	u8 pad2;
	u16 fp[NCAPS]; // footprint in blocks the texture would take under each cap (working set prediction)
	// OPT9 (PS2-HW-30): one VRAM image for the CLUT variants of a map texture / flat (chroma keyed or not): the variants are separate records
	// (own owner, clut, flags) and only the first one owns the blocks.
	int share; // NOREC = this record owns its blocks (the image); else the owning record (this one is a view: same blk/tbw/psm, nblk = size of the image)
	int sec; // owner: first view of the image
	int sec_next; // view: next view of the same image
	u8 has255; // owner: the image has texels of index 255 (holes under the keyed CLUT)
	u8 shareable; // owner: P_8 data of a map texture or flat: views for the other variants may share it
	u8 dmapend; // zero-copy upload (PS2-HW-32): the image data is read by queued DMA
	u8 pad3;
	u32 imgws; // owner: frame_no + 1 of the last frame in which a draw used the image (through any of its records)
	u32 lodfr; // owner: frame_no + 1 of the frame lod belongs to
	u8 lod; // owner: the finest mip level (0 = full size) a polygon of the frame needed (instrumentation, PS2-HW-34)
	// OPT10 (PS2-HW-70): an image of more than 1024 rows (the GS limit) is stored as two images, the rows [0, 1024) and the rest, each with its own blocks
	u8 psplit; // 0 = an ordinary image, 1 = the first piece (what the engine's texture is bound to), 2 = the second piece
	int pnext, ppar; // first piece: the record of the second (NOREC = none); second piece: the first. One of them goes, the other goes with it
	float pt0, pt1, ptk; // the piece holds the engine texture rows [pt0, pt1) of one repeat (fractions of the height); a polygon's t in the piece is (t - pt0) * ptk
} texrec_t;

#define NOREC (-1)

// state of the stream as the GS knows it
typedef struct
{
	u64 test, alpha, clamp, tex0, tex1, zbuf, frame, scissor, fogcol, rgbaq;
	u32 valid; // bit per register above: value is known
} gsregs_t;

enum
{
	RV_TEST = 1,
	RV_ALPHA = 2,
	RV_CLAMP = 4,
	RV_TEX0 = 8,
	RV_TEX1 = 16,
	RV_ZBUF = 32,
	RV_FRAME = 64,
	RV_SCISSOR = 128,
	RV_FOGCOL = 256,
	RV_RGBAQ = 512,
};

static struct
{
	int up;
	int mode;
	int outid; // docs/VIDEO_MODES.md table B format used (0 NTSC field, 1 PAL field, 2 480p, 3 NTSC frame, 4 PAL frame)
	int fbw, fbh, fb32, fbpsm; // frame buffer: line stride in pixels (multiple of 64), rows, CT32 flag, pixel format
	int vw, vh; // visible internal size = what the CRTC magnifies to the screen
	int magh, magv; // CRTC magnification (clock units per pixel, lines per row)
	int crtc_dx, crtc_dw; // gsKit's own display start / width before the correction (diagnostics)
	u32 fb_addr[2], z_addr; // bytes
	GSGLOBAL *gs;
	int sema_vbl, intc_vbl, vbl_installed;
	int sema_dma, dmac;
	int dma_irq;
	volatile int irq_seen;

	// DMA ring
	qw_t *ring[NBUF];
	int cur;
	u32 wr; // qwords used in ring[cur] (OPT9: the first one is the DMA tag of the first run)
	u32 run; // OPT9 (PS2-HW-32): index in ring[cur] of the tag of the open DMA run (the buffer is a source chain: runs of inline data and references to engine data)
	u32 q_seq; // transfers queued (FIFO)
	volatile u32 done_seq; // transfers completed: buffer i is free when buf_seq[i] <= done_seq
	u32 buf_seq[NBUF];
	int gate; // OPT9 (PS2-HW-35): the EE prepares the next frame while the GS finishes and shows the previous one (buffers of the next frame are held until the flip)
	u32 fid, pend_fid; // frame id of the open frame / of the finished frame waiting for its flip
	u32 q_fid[NBUF]; // frame id of each queued buffer
	volatile int pending; // buffers queued or in flight
	volatile int dma_busy;
	int dma_cur;
	int q_idx[NBUF];
	u32 q_qwc[NBUF];
	int q_head, q_n;

	// frame
	int frame_open;
	int need_copy; // the new frame starts as a copy of the previous one unless the engine clears the colour first
	int target; // frame buffer being drawn
	int last_drawn;
	int have_drawn; // a frame was completed since Init
	volatile int disp;
	volatile int pend;
	volatile int pend_buf;
	volatile int pend_done; // the pending frame is known to be completely drawn (FINISH was seen by the EE or by the vblank handler)
	volatile u32 flip_vbl; // H.vbl at the last flip (watchdog)
	volatile int finish_pending;
	volatile u32 vbl, flips;
	u32 frame_no;

	// VRAM pool
	u32 pool_base, pool_blocks; // blocks
	u32 clut_base; // first block of the CLUT area (CLUT_SLOTS images)
	clutent_t clut_ent[CLUT_SLOTS];
	u32 clut_tick;
	u32 free_start[VRAM_RANGES], free_len[VRAM_RANGES];
	int free_n;
	u32 used_blocks;

	// textures
	texrec_t *rec;
	int rec_n, rec_cap;
	int rec_free; // head of the free list (chained through .next)
	int lru_head, lru_tail; // MRU / LRU
	u32 lru_seq; // last recency number handed out
	int cur_tex; // record index or NOREC
	int cur_missing; // the engine selected a texture that could not be uploaded: draws that need it are skipped
	int scr_rec[NSCR]; // screen texture slots
	void *scr_data[NSCR]; // lossless EE backing for captures spilled out of VRAM (aligned 64)
	u32 screen_bytes, screen_peak_bytes;
	u32 tex_cap_blocks; // footprint cap of one texture in blocks (0 = none): bigger ones are stored decimated
	int cap_adapt; // 1 = the controller (tex_cap_control) chooses tex_cap_blocks from the working set; 0 = fixed (config)
	int cap_idx; // index into CAPS[] of the cap in force (adaptive mode)
	u32 cap_gen; // bumped when the cap changes: textures stored under an older cap are re-made when they are drawn
	u32 pred[NCAPS]; // this frame: footprint sum of the textures drawn so far, as if every one were stored under cap k
	u32 pred_frame; // frame_no + 1 the sums belong to
	int cap_up_count; // frames in a row in which a larger cap would have fitted
	int cap_down_count; // frames in a row in which the working set did not fit
	u32 upg_blocks; // blocks of textures re-made at a larger size in this frame (budget)
	u32 plan_reserve; // OPT10 (PS2-HW-37): blocks the frame plan leaves for the textures it does not plan (sprites, patches, small textures): measured on the previous frames
	u32 unplanned_cur; // blocks of those textures drawn so far in this frame
	u32 plan_last, plan_exact; // the last plan's working set (budget applied / exact levels), blocks
	u32 plan_pct, plan_calm, thrash_n, thrash_blk; // PS2-HW-39: the budget of the plan in per cent of the pool (a controller: an image of the frame evicted in the frame lowers it, calm frames raise it)
	GLMipmap_t *imm_tex; // PS2-HW-37: a big map texture selected outside the batches: made resident (at the level its polygon needs) when the polygon is drawn
	int tex_filter; // HWD_SET_TEXTUREFILTER_*
	u32 pal[256]; // GS CLUT entries, 0x00BBGGRR, no alpha, in index order
	int pal_set;
	u32 pal_gen; // bumped by every palette change; CLUT variants older than this are rebuilt
	u32 clut_loaded; // block of the CLUT in the GS CLUT buffer, 0 = unknown

	// engine view
	int scr_w, scr_h;
	float sx, sy; // engine -> frame buffer scale
	float vpx, vpy, vpw, vph; // viewport in frame buffer pixels (top-down)
	float near_plane;
	int mat_dirty;
	float mv[16] __attribute__((aligned(16))), proj[16] __attribute__((aligned(16))), mvp[16] __attribute__((aligned(16))); // column vectors: v' = M * v, m[col*4+row]
	int split_special; // SetTransform's static special_splitscreen
	u32 cur_flags; // last SetBlend/DrawPolygon flags
	int guard_x, guard_y;

	// shader state set by the engine (SetShader / SetShaderInfo / SetSpecialState)
	int shader; // slot, -1 = none
	int shaders_on; // SetSpecialState(HWD_SET_SHADERS)
	int model_light; // SetSpecialState(HWD_SET_MODEL_LIGHTING)
	int leveltime;

	gsregs_t gsr;
	u32 serial; // bumped by everything that writes GS state or CLUT data (A+D packets, uploads): a polygon plan set up earlier is then stale
	u32 sciss_x0, sciss_y0, sciss_x1, sciss_y1;

	ps2hwd_stats_t st;
	u32 warned;
} H __attribute__((aligned(16)));

int ps2hwd_dbg_flags;
int ps2hwd_force_lt = -1;
int ps2hwd_hash_on; // -hwhash (OPT10 HG): a 64 bit hash of every GIF packet buffer of a frame is printed (HWHASH lines): equal streams of two runs = equal pictures, bit for bit
static u64 hash_frame;

// -hwdbg flags of the texture manager (OPT9, HT); bits below 0x100000 belong to the other HW agents and the host tests
#define HWDBG_TEX 0x100000 // HWTEX lines every 105 frames (texture statistics, instrumentation of the working set)
#define HWDBG_NOZC 0x200000 // no zero-copy uploads (DMA references): the texels are copied into the ring
#define HWDBG_BATCHORDER 0x400000 // HWBO lines: the textures of one frame per window in the order the batches draw them (hit / upload)
#define HWDBG_NOGATE 0x800000 // the old frame start: wait for the flip of the previous frame before drawing the next one
#define HWDBG_COMPOSE 0x1000000 // hw_cache.c: every composed P_8 texture is checked against the original column loops
#define HWDBG_NOPLAN 0x2000000 // no frame plan: every texture is stored at full size
#define HWDBG_IMMDBG 0x40000000 // HWIMM lines: every upload / re-make of a big map texture (frame, name, level asked for, level stored, action)
#define HWDBG_LODDBG 0x40000000 // HWLOD lines: the polygons of the frame plan of frame % 105 == 60 (texture, size, vertices, texel size in pixels, level)

#define BIG_IMAGE_BLOCKS 512u // PS2-HW-39: images of at least this many blocks are placed from the top of the pool (screen captures are there too), the others from the bottom
#define PLAN_MIN_TEXELS 65536u // PS2-HW-34: smaller textures are always stored at full size: their share of the working set is small and the estimate costs cycles
#define PLAN_FULL_KEEP 150 // frames a texture that was drawn unplanned keeps its full size in the plans

// OPT9 (HT): texture statistics of the window of 105 frames; printed as HWTEX lines when ps2hwd_dbg_flags has bit 1024 (-hwdbg 1024)
typedef struct
{
	u32 frames;
	u32 shared; // views made (the other CLUT variant of a resident image: no upload)
	u32 up_n[4], up_blk[4]; // uploads by kind: 0 map texture, 1 flat, 2 patch or other 8-bit, 3 direct colour
	u32 reup_n, reup_blk; // uploads of a texture that was uploaded already in the same frame (evicted in between)
	u32 regen_n, regen_cyc; // data the engine had to make again at draw time
	u32 up_cyc; // EE cycles of the uploads (conversion / copy / tags)
	u32 need_blk, need_n; // images drawn per frame, summed over the window
	u32 zc_n, zc_blk; // uploads made by DMA reference (zero copy)
	u32 sync_cyc; // EE cycles of SyncDCache for those
	u32 dmawait_cyc;
	u32 lod_blk, lod_lvl[6]; // blocks of the working set if every image were stored at the level the frame needs; per level
	u32 lvl_up[6]; // uploads of images stored at a mip level (1..5), PS2-HW-34
	u32 upgrades; // images made again at a finer level than they were stored at
	u32 downgrades; // images made again at a coarser level than they were stored at (PS2-HW-37)
	u32 invisible; // textures not uploaded: no polygon of the frame can see them
	u32 plan_blk, plan_exact, unplanned, need_in_plan, need_out_plan, plan_cyc, imm_cyc, alloc_fail, alloc_frag, alloc_frag_blk, thrash_n, thrash_blk, lvl_cyc, plan_np, plan_mcyc, split_up, split_poly, split_sub; // PS2-HW-37: planned working set per frame (budget applied / exact), summed over the window; the textures that are not planned
} txstats_t;
static txstats_t TX;

enum
{
	HW_SHADER = 1, HW_PALETTE_SHADER = 2, HW_LIGHT_TABLE = 4,
	HW_MODEL_LIGHT = 8, HW_ANISO = 16, HW_WIREFRAME = 32,
	HW_SUBTRACT = 64, HW_MULTIPLY = 128, HW_ENVIRONMENT = 256,
	HW_FOG = 512, HW_CORONA = 1024, HW_TEXTURE_LOSS = 2048,
	HW_MISSING = 4096, HW_SCREEN_LOSS = 8192, HW_WIPE = 16384,
	HW_INVALID = 32768, HW_FILTER = 65536
};

static void hw_limit(u32 bit, const char *message)
{
	H.st.unsupported_calls++;
	H.st.unsupported_mask |= bit;
	if (!(H.warned & bit))
	{
		H.warned |= bit;
		CONS_Alert(CONS_WARNING, "PS2 GS experimental limitation: %s\n", message);
	}
}

static void (*resource_error_handler)(const char *message);

static void hw_failure(const char *message)
{
	hw_limit(HW_MISSING, message);
	if (resource_error_handler)
		resource_error_handler(message);
}
