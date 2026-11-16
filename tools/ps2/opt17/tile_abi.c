#include "ps2_hwd.c"
typedef struct
{
	GLMipmap_t *owner;
	u16 blk; // first 256-byte block in VRAM: 0..VRAM_BLOCKS-1, fits the checked u16 range above
	u16 nblk; // an admitted image occupies at most VRAM_BLOCKS (including GS page/block holes)
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
	u16 uw, vh; // used dimensions: <=16384 with AP88 crop, <=1024 for other images, split pieces and captures
	float sxs, sys; // engine texture coordinate -> GS ST scale: w / 2^tw, h / 2^th (1 for power of two sizes)
	u16 sw, sh; // OPT8: the engine's size of the texture (the stored size is w x h = sw >> dx, sh >> dy)
	u8 dx, dy; // decimation shifts of the stored texture
	u8 capi; // footprint cap index (CAPS[]) the stored plan was made for
	u8 csa; // PSMT4: original aligned 16-entry palette band; zero for other formats
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
} tile_baseline_texrec_t;
_Static_assert(sizeof(texrec_t)==140,"tile record EE ABI grew");
_Static_assert(offsetof(texrec_t, owner) == offsetof(tile_baseline_texrec_t, owner), "prefix offset owner");
_Static_assert(offsetof(texrec_t, blk) == offsetof(tile_baseline_texrec_t, blk), "prefix offset blk");
_Static_assert(offsetof(texrec_t, nblk) == offsetof(tile_baseline_texrec_t, nblk), "prefix offset nblk");
_Static_assert(offsetof(texrec_t, w) == offsetof(tile_baseline_texrec_t, w), "prefix offset w");
_Static_assert(offsetof(texrec_t, h) == offsetof(tile_baseline_texrec_t, h), "prefix offset h");
_Static_assert(offsetof(texrec_t, tw) == offsetof(tile_baseline_texrec_t, tw), "prefix offset tw");
_Static_assert(offsetof(texrec_t, th) == offsetof(tile_baseline_texrec_t, th), "prefix offset th");
_Static_assert(offsetof(texrec_t, tbw) == offsetof(tile_baseline_texrec_t, tbw), "prefix offset tbw");
_Static_assert(offsetof(texrec_t, psm) == offsetof(tile_baseline_texrec_t, psm), "prefix offset psm");
_Static_assert(offsetof(texrec_t, clut) == offsetof(tile_baseline_texrec_t, clut), "prefix offset clut");
_Static_assert(offsetof(texrec_t, wrapx) == offsetof(tile_baseline_texrec_t, wrapx), "prefix offset wrapx");
_Static_assert(offsetof(texrec_t, wrapy) == offsetof(tile_baseline_texrec_t, wrapy), "prefix offset wrapy");
_Static_assert(offsetof(texrec_t, nearest_only) == offsetof(tile_baseline_texrec_t, nearest_only), "prefix offset nearest_only");
_Static_assert(offsetof(texrec_t, baked) == offsetof(tile_baseline_texrec_t, baked), "prefix offset baked");
_Static_assert(offsetof(texrec_t, screen) == offsetof(tile_baseline_texrec_t, screen), "prefix offset screen");
_Static_assert(offsetof(texrec_t, notcc) == offsetof(tile_baseline_texrec_t, notcc), "prefix offset notcc");
_Static_assert(offsetof(texrec_t, region) == offsetof(tile_baseline_texrec_t, region), "prefix offset region");
_Static_assert(offsetof(texrec_t, npot_x) == offsetof(tile_baseline_texrec_t, npot_x), "prefix offset npot_x");
_Static_assert(offsetof(texrec_t, npot_y) == offsetof(tile_baseline_texrec_t, npot_y), "prefix offset npot_y");
_Static_assert(offsetof(texrec_t, used) == offsetof(tile_baseline_texrec_t, used), "prefix offset used");
_Static_assert(offsetof(texrec_t, pin) == offsetof(tile_baseline_texrec_t, pin), "prefix offset pin");
_Static_assert(offsetof(texrec_t, stamp) == offsetof(tile_baseline_texrec_t, stamp), "prefix offset stamp");
_Static_assert(offsetof(texrec_t, seq) == offsetof(tile_baseline_texrec_t, seq), "prefix offset seq");
_Static_assert(offsetof(texrec_t, done) == offsetof(tile_baseline_texrec_t, done), "prefix offset done");
_Static_assert(offsetof(texrec_t, ws) == offsetof(tile_baseline_texrec_t, ws), "prefix offset ws");
_Static_assert(offsetof(texrec_t, wgt) == offsetof(tile_baseline_texrec_t, wgt), "prefix offset wgt");
_Static_assert(offsetof(texrec_t, prev) == offsetof(tile_baseline_texrec_t, prev), "prefix offset prev");
_Static_assert(offsetof(texrec_t, next) == offsetof(tile_baseline_texrec_t, next), "prefix offset next");
_Static_assert(offsetof(texrec_t, uw) == offsetof(tile_baseline_texrec_t, uw), "prefix offset uw");
_Static_assert(offsetof(texrec_t, vh) == offsetof(tile_baseline_texrec_t, vh), "prefix offset vh");
_Static_assert(offsetof(texrec_t, sxs) == offsetof(tile_baseline_texrec_t, sxs), "prefix offset sxs");
_Static_assert(offsetof(texrec_t, sys) == offsetof(tile_baseline_texrec_t, sys), "prefix offset sys");
_Static_assert(offsetof(texrec_t, sw) == offsetof(tile_baseline_texrec_t, sw), "prefix offset sw");
_Static_assert(offsetof(texrec_t, sh) == offsetof(tile_baseline_texrec_t, sh), "prefix offset sh");
_Static_assert(offsetof(texrec_t, dx) == offsetof(tile_baseline_texrec_t, dx), "prefix offset dx");
_Static_assert(offsetof(texrec_t, dy) == offsetof(tile_baseline_texrec_t, dy), "prefix offset dy");
_Static_assert(offsetof(texrec_t, capi) == offsetof(tile_baseline_texrec_t, capi), "prefix offset capi");
_Static_assert(offsetof(texrec_t, csa) == offsetof(tile_baseline_texrec_t, csa), "prefix offset csa");
_Static_assert(offsetof(texrec_t, fp) == offsetof(tile_baseline_texrec_t, fp), "prefix offset fp");
_Static_assert(offsetof(texrec_t, share) == offsetof(tile_baseline_texrec_t, share), "prefix offset share");
_Static_assert(offsetof(texrec_t, sec) == offsetof(tile_baseline_texrec_t, sec), "prefix offset sec");
_Static_assert(offsetof(texrec_t, sec_next) == offsetof(tile_baseline_texrec_t, sec_next), "prefix offset sec_next");
_Static_assert(offsetof(texrec_t, has255) == offsetof(tile_baseline_texrec_t, has255), "prefix offset has255");
_Static_assert(offsetof(texrec_t, shareable) == offsetof(tile_baseline_texrec_t, shareable), "prefix offset shareable");
_Static_assert(offsetof(texrec_t, dmapend) == offsetof(tile_baseline_texrec_t, dmapend), "prefix offset dmapend");
_Static_assert(offsetof(texrec_t, tiled) == offsetof(tile_baseline_texrec_t, pad3), "prefix offset pad3");
_Static_assert(offsetof(texrec_t, imgws) == offsetof(tile_baseline_texrec_t, imgws), "prefix offset imgws");
_Static_assert(offsetof(texrec_t, lodfr) == offsetof(tile_baseline_texrec_t, lodfr), "prefix offset lodfr");
_Static_assert(offsetof(texrec_t, lod) == offsetof(tile_baseline_texrec_t, lod), "prefix offset lod");
_Static_assert(offsetof(texrec_t, psplit) == offsetof(tile_baseline_texrec_t, psplit), "prefix offset psplit");
_Static_assert(offsetof(texrec_t, pnext) == offsetof(tile_baseline_texrec_t, pnext), "prefix offset pnext");
_Static_assert(offsetof(texrec_t, ppar) == offsetof(tile_baseline_texrec_t, ppar), "prefix offset ppar");
_Static_assert(offsetof(texrec_t, pt0) == offsetof(tile_baseline_texrec_t, pt0), "prefix offset pt0");
_Static_assert(offsetof(texrec_t, pt1) == offsetof(tile_baseline_texrec_t, pt1), "prefix offset pt1");
_Static_assert(offsetof(texrec_t, ptk) == offsetof(tile_baseline_texrec_t, ptk), "prefix offset ptk");

_Static_assert(sizeof(GLMipmap_t)==68,"engine mipmap EE ABI grew");
