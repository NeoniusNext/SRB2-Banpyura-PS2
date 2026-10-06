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
	PS2HWD_VIDEO_AUTO, 0, 0, 1, 0, 1, 0, 3, 320, 200, 0, 0, 0,
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
	u32 wr; // qwords used in ring[cur]
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
