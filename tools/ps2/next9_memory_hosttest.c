/* Actual full-width mipmap metadata and its production lifetime operations.
 * GPU uploads, resource conversion and libc are explicit service boundaries.
 * The zone and spill allocation policy are production code. */
#include <stdint.h>
#ifdef __mips__
#include <malloc.h>
void init_scr(void);
#define aligned_alloc memalign
#endif
typedef uint8_t UINT8;
static int netgame;
static int PS2Lua_InCall(void) { return 0; }
void PS2Spill_Reset(void);
#define max(a,b) ((a) > (b) ? (a) : (b))
#include MEMORY_ZONE
typedef uint16_t UINT16;
typedef int16_t INT16;
typedef UINT32 u32;
#define ROTSPRITE
#define ATTRPACK __attribute__((packed))
#include MEMORY_TYPES

/* This controlled libc boundary has a struct _reent * prototype in newlib;
 * the production wrapper deliberately declares its opaque argument as void *.
 * Keep the fixture boundary separate from the SDK's real libc symbol. */
#define _malloc_usable_size_r memory_usable_size_r

typedef struct { void *pointer; size_t bytes; } libc_record_t;
static libc_record_t libc_records[1024];
static size_t libc_now, libc_peak, libc_calls, request_bytes;
static int libc_denied, allocation_calls, allocation_fail_at;
static int GetThreadId(void) { return 1; }
void *__real__malloc_r(void *r, size_t n)
{
	(void)r;
	libc_calls++;
	if (libc_denied) return NULL;
	void *p = malloc(n ? n : 1);
	if (!p) return NULL;
	for (unsigned i = 0; i < 1024; i++) if (!libc_records[i].pointer)
	{
		libc_records[i].pointer = p; libc_records[i].bytes = n;
		libc_now += n; if (libc_now > libc_peak) libc_peak = libc_now;
		return p;
	}
	check(0, "libc boundary ledger capacity"); return NULL;
}
size_t _malloc_usable_size_r(void *r, void *p)
{
	(void)r;
	for (unsigned i = 0; i < 1024; i++) if (libc_records[i].pointer == p) return libc_records[i].bytes;
	check(0, "known libc pointer"); return 0;
}
void __real__free_r(void *r, void *p)
{
	(void)r;
	if (!p) return;
	for (unsigned i = 0; i < 1024; i++) if (libc_records[i].pointer == p)
	{
		libc_now -= libc_records[i].bytes;
		libc_records[i].pointer = NULL; libc_records[i].bytes = 0;
		free(p); return;
	}
	check(0, "libc boundary free owns pointer");
}
void *__real__realloc_r(void *r, void *p, size_t n)
{
	if (!p) return __real__malloc_r(r, n);
	if (!n) { __real__free_r(r, p); return NULL; }
	size_t old = _malloc_usable_size_r(r, p);
	void *q = __real__malloc_r(r, n);
	if (q) { memcpy(q, p, old < n ? old : n); __real__free_r(r, p); }
	return q;
}
void *__real__memalign_r(void *r, size_t align, size_t n)
{ (void)align; return __real__malloc_r(r, n); }
#include MEMORY_SPILL
static void *memory_calloc(size_t count, size_t bytes)
{
	allocation_calls++; request_bytes += count * bytes;
	if (allocation_fail_at == allocation_calls) return NULL;
	return __wrap__calloc_r(NULL, count, bytes);
}
static void memory_free(void *p) { __wrap__free_r(NULL, p); }

typedef struct { INT16 width, height; boolean transparency; } texture_t;
typedef struct { INT32 type, texture_id; } levelflat_t;
enum { LEVELFLAT_NONE, LEVELFLAT_TEXTURE, GL_TEXFMT_BOUND = 0x22 };
#define TF_WRAPXY 3u
#define TF_CHROMAKEYED 16u
#define TF_TRANSPARENT 64u
#define FRACUNIT 65536
static GLMapTexture_t *gl_textures, *gl_flats;
static size_t gl_numtextures;
static boolean gl_maptexturesloaded, currently_batching;
static texture_t **textures;
static INT32 *texturetranslation;
static INT32 textureformat = GL_TEXFMT_P_8;
static UINT32 hwr_texsig;
static GLMipmap_t *current_texture;
static unsigned deletes, uploads, updates, cacheflushes, texc_resets, made_patches;
static UINT8 colormaps_data[256], flat_source[256];
static UINT8 *colormaps = colormaps_data;
static struct { int glstate; } vid;
#define VID_GL_LIBRARY_LOADED 1
#define HWR_PATCH_UNLOCKED(data) Z_ChangeTag((data), PU_HWRCACHE_UNLOCKED)
static void HWR_GCacheFlush(void) { cacheflushes++; }
static void PS2TexC_Reset(void) { texc_resets++; }
static void HWR_SetCurrentTexture(GLMipmap_t *m) { current_texture = m; }
static void gpu_delete(GLMipmap_t *m) { check(m != NULL, "driver deletion receives live metadata"); deletes++; m->downloaded = 0; }
static void gpu_set(GLMipmap_t *m) { check(m != NULL, "driver selection receives live metadata"); uploads++; m->downloaded = 0xF0000000u | uploads; }
static void gpu_update(GLMipmap_t *m) { check(m && m->downloaded, "driver update receives uploaded metadata"); updates++; }
static struct { void (*pfnDeleteTexture)(GLMipmap_t *); void (*pfnSetTexture)(GLMipmap_t *); void (*pfnUpdateTexture)(GLMipmap_t *); } HWD = {gpu_delete, gpu_set, gpu_update};
static const UINT8 *R_TryGetFlatForTexture(size_t index) { check(index < gl_numtextures, "flat lookup index"); return flat_source; }
static void HWR_GenerateTexture(INT32 index, GLMapTexture_t *t, GLMipmap_t *m)
{
	check(index >= 0 && (size_t)index < gl_numtextures && t == &gl_textures[index], "regeneration selects matching map root");
	size_t n = (size_t)m->width * m->height;
	m->data = Z_Calloc(n, PU_HWRCACHE, &m->data);
	memset(m->data, (unsigned char)(index ^ 0xA3), n);
}
static void HWR_MakePatch(const patch_t *patch, GLPatch_t *info, GLMipmap_t *m, boolean bitmap)
{
	check(info == patch->hardware, "patch converter receives actual hardware owner");
	made_patches++;
	m->width = (UINT16)patch->width; m->height = (UINT16)patch->height;
	m->format = GL_TEXFMT_P_8; m->flags = TF_CHROMAKEYED;
	if (bitmap && !m->data)
	{
		size_t n = (size_t)m->width * m->height;
		m->data = Z_Calloc(n, PU_HWRCACHE, &m->data);
		for (size_t i = 0; i < n; i++) ((UINT8 *)m->data)[i] = (UINT8)(i ^ (m->colormap ? m->colormap->data[i % 256] : 0xB7));
	}
}
#define calloc memory_calloc
#define free memory_free
#include MEMORY_INIT
#include MEMORY_TEXTURE
#include MEMORY_FLAT
#include MEMORY_PATCH
#include MEMORY_FREE
#include MEMORY_STEAL
#undef free
#undef calloc

#define PLAN_MIN_TEXELS 2048u
#define PLAN_FULL_KEEP 3u
#define PLAN_MAX 8u
typedef struct { GLMipmap_t *key; UINT32 untouched[4]; } planinfo_t;
static struct { UINT32 frame_no; } H;
static unsigned plan_n;
static planinfo_t plan_info[PLAN_MAX];
static int imm_level = -1, batch_phase;
static void plan_sync(void) {}
#include MEMORY_DRIVER

static FILE *snapshot_file;
static unsigned cases;
static void *pointer_ids[128];
static unsigned pointer_count;
static UINT32 pointer_id(const void *p)
{
	if (!p) return 0;
	for (unsigned i = 0; i < pointer_count; i++) if (pointer_ids[i] == p) return i + 1;
	check(pointer_count < 128, "normalized pointer identity capacity");
	pointer_ids[pointer_count++] = (void *)p; return pointer_count;
}
static void word(UINT32 value)
{
	UINT8 bytes[4] = {(UINT8)value, (UINT8)(value >> 8), (UINT8)(value >> 16), (UINT8)(value >> 24)};
	check(fwrite(bytes, 1, sizeof bytes, snapshot_file) == sizeof bytes, "literal snapshot write");
}
static void snapshot(const GLMipmap_t *m)
{
	/* Pixel cache addresses can be reused after a real free. Their exact live
	 * owners and full pointer identities are asserted at each transition; only
	 * cache presence is serialized across different host/EE allocator ABIs. */
	word((UINT32)m->format); word(m->data != NULL); word(m->flags); word(m->height); word(m->width);
	word(m->downloaded); word(pointer_id(m->nextcolormap)); word(pointer_id(m->colormap));
	word((UINT32)m->regen_id); word(m->regen_kind); word(pointer_id(m->ps2_twin)); word(m->ps2_h255);
	word(m->ps2_planfr); word(m->ps2_want); word(m->ps2_pi); word(m->ps2_uw); word(m->ps2_uh);
	word(m->ps2_ihint); word(m->ps2_ihfr); word(m->ps2_imfr); word(m->ps2_vis); word(m->ps2_full_fr);
	word(m->ps2_gcv); word(m->ps2_spv); word(m->ps2_nup); word(m->ps2_drop); word(m->ps2_cost);
}
static void begin(int sides, int guard, int deny)
{
	check(libc_now == 0 && spill_now == 0 && spill_blocks == 0, "previous libc and spill roots released");
	arena_reset(16u << 20, sides, ship_prefer, guard);
	libc_peak = libc_calls = request_bytes = 0; libc_denied = deny;
	allocation_calls = allocation_fail_at = 0;
	spill_peak = spill_total = spill_fail = spill_foreign = 0;
	PS2Spill_Init(); pointer_count = 0;
	uploads = updates = deletes = cacheflushes = texc_resets = made_patches = 0;
	current_texture = NULL; vid.glstate = VID_GL_LIBRARY_LOADED; currently_batching = false;
	HWR_InitMapTextures();
}
static void finish(unsigned kind, size_t count, size_t request)
{
	zastats_t st; consistent(); ZA_Stats(&st);
	printf("MEMORY9 case=%u kind=%u count=%zu request=%zu used=%zu peak=%zu libc=%zu libcpeak=%zu spill=%zu blocks=%zu calls=%d\n",
		++cases, kind, count, request, st.used, st.peakused, libc_now, libc_peak, spill_now, spill_blocks, allocation_calls);
	check(!libc_now && !spill_now && !spill_blocks, "complete libc and spill cleanup");
	check(st.used == 0, "complete actual arena cleanup");
	memory_backing_check();
}

static UINT32 sentinel[16];
static UINT32 bits(size_t index, unsigned field) { return (UINT32)index * 0x9E3779B9u ^ (field * 0x46A59D13u) ^ 0x80000001u; }
static void fill(GLMipmap_t *m, size_t i)
{
	m->format = (GLTextureFormat_t)bits(i, 0); m->data = &sentinel[(i + 1) % 16];
	m->flags = bits(i, 2); m->height = (UINT16)bits(i, 3); m->width = (UINT16)bits(i, 4);
	m->downloaded = bits(i, 5); m->nextcolormap = (GLMipmap_t *)&sentinel[(i + 6) % 16];
	m->colormap = (GLColormap_t *)&sentinel[(i + 7) % 16]; m->regen_id = (INT32)bits(i, 8);
	m->regen_kind = (UINT8)bits(i, 9); m->ps2_twin = (GLMipmap_t *)&sentinel[(i + 10) % 16];
	m->ps2_h255 = (UINT8)bits(i, 11); m->ps2_planfr = bits(i, 12); m->ps2_want = (UINT8)bits(i, 13);
	m->ps2_pi = (UINT16)bits(i, 14); m->ps2_uw = (UINT16)bits(i, 15); m->ps2_uh = (UINT16)bits(i, 16);
	m->ps2_ihint = (UINT8)bits(i, 17); m->ps2_ihfr = bits(i, 18); m->ps2_imfr = bits(i, 19);
	m->ps2_vis = (UINT8)bits(i, 20); m->ps2_full_fr = bits(i, 21); m->ps2_gcv = bits(i, 22);
	m->ps2_spv = bits(i, 23); m->ps2_nup = (UINT8)bits(i, 24); m->ps2_drop = (UINT8)bits(i, 25);
	m->ps2_cost = (UINT8)bits(i, 26);
}
#define EXPECT(field, value) check((m->field) == (value), "full " #field " survives representation/copy")
static void fields(const GLMipmap_t *m, size_t i)
{
	EXPECT(format, (GLTextureFormat_t)bits(i, 0)); EXPECT(data, &sentinel[(i + 1) % 16]); EXPECT(flags, bits(i, 2));
	EXPECT(height, (UINT16)bits(i, 3)); EXPECT(width, (UINT16)bits(i, 4)); EXPECT(downloaded, bits(i, 5));
	EXPECT(nextcolormap, (GLMipmap_t *)&sentinel[(i + 6) % 16]); EXPECT(colormap, (GLColormap_t *)&sentinel[(i + 7) % 16]);
	EXPECT(regen_id, (INT32)bits(i, 8)); EXPECT(regen_kind, (UINT8)bits(i, 9)); EXPECT(ps2_twin, (GLMipmap_t *)&sentinel[(i + 10) % 16]);
	EXPECT(ps2_h255, (UINT8)bits(i, 11)); EXPECT(ps2_planfr, bits(i, 12)); EXPECT(ps2_want, (UINT8)bits(i, 13));
	EXPECT(ps2_pi, (UINT16)bits(i, 14)); EXPECT(ps2_uw, (UINT16)bits(i, 15)); EXPECT(ps2_uh, (UINT16)bits(i, 16));
	EXPECT(ps2_ihint, (UINT8)bits(i, 17)); EXPECT(ps2_ihfr, bits(i, 18)); EXPECT(ps2_imfr, bits(i, 19));
	EXPECT(ps2_vis, (UINT8)bits(i, 20)); EXPECT(ps2_full_fr, bits(i, 21)); EXPECT(ps2_gcv, bits(i, 22));
	EXPECT(ps2_spv, bits(i, 23)); EXPECT(ps2_nup, (UINT8)bits(i, 24)); EXPECT(ps2_drop, (UINT8)bits(i, 25)); EXPECT(ps2_cost, (UINT8)bits(i, 26));
}
#undef EXPECT

static void table_case(size_t count, int sides, int guard)
{
	begin(sides, guard, 0);
	GLMipmap_t *values = Z_Malloc(count * sizeof *values, PU_STATIC, NULL);
	UINT8 *neighbor = Z_Malloc(113, PU_STATIC, NULL); memset(neighbor, 0x5D, 113);
	GLMipmap_t *copy = Z_Malloc(count * sizeof *copy, PU_STATIC, NULL);
	memset(values, 0xA7, count * sizeof *values); memset(copy, 0x3E, count * sizeof *copy);
	for (size_t i = 0; i < count; i++) fill(&values[i], i);
	memcpy(copy, values, count * sizeof *copy);
	for (size_t i = 0; i < count; i++) { fields(&copy[i], i); snapshot(&copy[i]); }
	for (size_t i = 0; i < count; i++) { GLMipmap_t one = copy[i]; fields(&one, i); }
	for (unsigned i = 0; i < 113; i++) check(neighbor[i] == 0x5D, "neighbor redirection canary");
	consistent();
	zastats_t st; ZA_Stats(&st); size_t used = st.used, peak = st.peakused;
	Z_Free(copy); Z_Free(neighbor); Z_Free(values); consistent();
	printf("MEMORY9 case=%u kind=0 count=%zu request=%zu used=%zu peak=%zu libc=0 libcpeak=0 spill=0 blocks=0 calls=0\n",
		++cases, count, 2 * count * sizeof *values + 113, used, peak);
	check(Z_TotalUsage() == 0, "typed arrays released"); memory_backing_check();
}

static void map_case(size_t count, int sides, int guard, int deny)
{
	begin(sides, guard, deny);
	HWR_LoadMapTextures(count);
	check(gl_numtextures == count && gl_maptexturesloaded && gl_textures && gl_flats, "actual map constructor publishes complete roots");
	check(gl_textures != gl_flats && allocation_calls == 2, "two independent map roots at existing construction points");
	check(ZA_Contains(gl_textures) == (deny || count * sizeof *gl_textures >= SPILL_FIRST), "actual texture table allocation follows unchanged spill threshold");
	check(ZA_Contains(gl_flats) == (deny || count * sizeof *gl_flats >= SPILL_FIRST), "actual flat table allocation follows unchanged spill threshold");
	if (ZA_Contains(gl_textures)) check(ZA_BLOCK(gl_textures)->user == NULL, "spill table keeps original ownerless static root");
	if (ZA_Contains(gl_flats)) check(ZA_BLOCK(gl_flats)->user == NULL, "spill flat table keeps original ownerless static root");
	printf("MEMORY9 roots count=%zu textures=%p flats=%p texturezone=%d flatzone=%d bytes=%zu zoneused=%zu libcpayload=%zu\n",
		count, (void *)gl_textures, (void *)gl_flats, ZA_Contains(gl_textures), ZA_Contains(gl_flats), count * sizeof *gl_textures, Z_TotalUsage(), libc_now);
	for (size_t i = 0; i < count; i++)
	{
		const UINT8 *a = (const UINT8 *)&gl_textures[i];
		const UINT8 *b = (const UINT8 *)&gl_flats[i];
		for (size_t j = 0; j < sizeof *gl_textures; j++) check(!a[j] && !b[j], "actual calloc initializes whole map elements");
	}
	size_t selected[3] = {0, count / 2, count - 1};
	for (unsigned k = 0; k < 3; k++)
	{
		size_t i = selected[k];
		GLMapTexture_t *t = HWR_GetTexture((INT32)i, false);
		check(t == &gl_textures[i] && current_texture == &t->mipmap, "map selection returns live table alias");
		check(t->mipmap.regen_kind == 1 && t->mipmap.regen_id == (INT32)i, "map full regeneration identity");
		HWR_GetTexture((INT32)i, true);
		GLMipmap_t *v = t->mipmap.nextcolormap;
		check(v && current_texture == v && v->ps2_twin == &t->mipmap && v->regen_id == (INT32)i, "map variant links original full pointer and identity");
		GLMipmap_t *same = v; HWR_GetTexture((INT32)i, true);
		check(same == current_texture && same == t->mipmap.nextcolormap, "variant reuse preserves pointer and count");
		HWR_PS2_RegenerateMipmap(v);
		check(v->data && ZA_BLOCK(v->data)->user == &v->data, "map pixels owned by unchanged leading data field");
		UINT8 expected = (UINT8)(i ^ 0xA3);
		for (size_t p = 0; p < (size_t)v->width * v->height; p++) check(((UINT8 *)v->data)[p] == expected, "map regeneration byte oracle");
		void *stolen = NULL; void *old = v->data;
		check(HWR_PS2_StealData(v, &stolen) == old && !v->data && stolen == old, "actual steal moves live owner without copying data");
		check(ZA_BLOCK(stolen)->user == &stolen, "actual zone data owner changes to driver alias");
		Z_Free(stolen); check(stolen == NULL, "free clears driver live alias");
		HWR_PS2_RegenerateMipmap(v); check(v->data != NULL, "map rebuild after released data");
		snapshot(&t->mipmap); snapshot(v);
		UINT32 sx, sy; memcpy(&sx, &t->scaleX, 4); memcpy(&sy, &t->scaleY, 4); word(sx); word(sy);
		levelflat_t flat = {LEVELFLAT_TEXTURE, (INT32)i};
		HWR_GetLevelFlat(&flat, false);
		check(current_texture == &gl_flats[i].mipmap && current_texture->regen_kind == 2, "flat selection keeps correct table alias");
		HWR_GetLevelFlat(&flat, true); GLMipmap_t *fv = current_texture;
		check(fv == gl_flats[i].mipmap.nextcolormap && fv->ps2_twin == &gl_flats[i].mipmap, "flat chroma variant keeps original owner");
		HWR_PS2_RegenerateMipmap(fv);
		check(fv->data && ZA_BLOCK(fv->data)->user == &fv->data, "flat regenerated data owner");
		check(memcmp(fv->data, flat_source, (size_t)fv->width * fv->height) == 0, "actual flat regeneration literal bytes");
		/* Production flat teardown deletes the variant GPU reference and frees its
		 * shared pixels through the original. A PS2 regenerated variant owns its
		 * pixels, so release that private cache explicitly before the generic free. */
		Z_Free(fv->data); check(fv->data == NULL, "flat private cache released by owner");
		snapshot(&gl_flats[i].mipmap); snapshot(fv);
	}
	levelflat_t absent = {LEVELFLAT_NONE, -1}; HWR_GetLevelFlat(&absent, true); check(!current_texture, "empty flat clears selected alias");
	HWR_GetTexture(-1, false); check(current_texture == &gl_textures[0].mipmap, "invalid texture retains baseline index-zero fallback");
	/* Exercise the actual grow/reload entry while the old roots and their
	 * variants are still published. The loader frees them before replacement. */
	int before_reload = allocation_calls;
	HWR_LoadMapTextures(count + 1);
	check(gl_numtextures == count + 1 && gl_maptexturesloaded && gl_textures && gl_flats, "actual reload publishes increased full count");
	check(allocation_calls == before_reload + 2, "reload retains two-root allocation policy");
	check(!gl_textures[count].mipmap.nextcolormap && !gl_flats[count].mipmap.data, "reloaded extension is initialized");
	HWR_FreeMapTextures();
	check(!gl_textures && !gl_flats && !gl_numtextures && !gl_maptexturesloaded, "actual teardown clears both roots and publication state");
	word(uploads); word(updates); word(deletes); word(cacheflushes); word(texc_resets);
	finish(1, count, request_bytes);
}

static void patch_case(int sides, int guard, int deny)
{
	begin(sides, guard, deny);
	patch_t *p = Z_Calloc(sizeof *p, PU_PATCH, NULL); p->width = 13; p->height = 17;
	GLPatch_t *hw = Patch_AllocateHardwarePatch(p);
	check(hw == p->hardware && hw->mipmap && ZA_BLOCK(hw)->user == &p->hardware && ZA_BLOCK(hw->mipmap)->user == (void **)&hw->mipmap,
		"actual hardware patch and mipmap have original zone owners");
	check(Patch_AllocateHardwarePatch(p) == hw, "hardware allocation cache hit preserves roots");
	UINT8 maps[2][256]; for (unsigned i = 0; i < 256; i++) { maps[0][i] = (UINT8)i; maps[1][i] = (UINT8)(255 - i); }
	HWR_GetMappedPatch(p, NULL); check(current_texture == hw->mipmap, "default mapped patch selects root");
	HWR_GetMappedPatch(p, maps[0]); GLMipmap_t *a = current_texture;
	HWR_GetMappedPatch(p, maps[1]); GLMipmap_t *b = current_texture;
	check(a != b && hw->mipmap->nextcolormap == a && a->nextcolormap == b, "mapped patch list preserves order and full aliases");
	check(a->colormap->source == maps[0] && b->colormap->source == maps[1], "colormap source pointer identities");
	for (unsigned i = 0; i < 256; i++) check(a->colormap->data[i] == maps[0][i] && b->colormap->data[i] == maps[1][i], "copied colormap byte oracle");
	unsigned before = allocation_calls; HWR_GetMappedPatch(p, maps[0]); check(allocation_calls == (int)before && current_texture == a, "mapped hit has no allocation");
	maps[0][37] ^= 0xA5; HWR_GetMappedPatch(p, maps[0]);
	check(a->colormap->data[37] == maps[0][37] && updates == 1, "existing colormap mutation updates same mipmap");
	snapshot(hw->mipmap); snapshot(a); snapshot(b);
	HWR_FreeTextureColormaps(p); check(!hw->mipmap->nextcolormap, "actual colormap teardown unlinks chain");
	HWR_FreeTextureData(p); check(!hw->mipmap->data, "actual pixel teardown clears root data owner");
	HWR_GetPatch(p); check(hw->mipmap->data != NULL, "patch pixels rebuilt through same metadata after release");
	HWR_FreeTexture(p); check(!p->hardware, "actual hardware teardown clears patch live alias");
	Z_Free(p); word(uploads); word(updates); word(deletes); word(made_patches);
	finish(2, 1, request_bytes);
}

static void driver_case(int sides, int guard)
{
	begin(sides, guard, 0);
	GLMipmap_t values[10], variant; memset(values, 0, sizeof values); memset(&variant, 0, sizeof variant);
	plan_n = 0; H.frame_no = 0xFFFFFFFEu; batch_phase = 2; imm_level = -1;
	for (unsigned i = 0; i < 10; i++)
	{
		GLMipmap_t *m = &values[i]; m->format = GL_TEXFMT_P_8; m->width = 64; m->height = 32; m->regen_kind = (UINT8)(1 + i % 2); m->regen_id = (INT32)(0xF0000000u + i);
		check(plan_wants(m), "actual plan eligibility reads full metadata");
		planinfo_t *p = plan_key(m);
		check(m->ps2_planfr == H.frame_no + 1, "actual plan frame identity retains high bits");
		check((i < PLAN_MAX) ? p == &plan_info[i] && p->key == m && m->ps2_pi == i : !p && m->ps2_pi == 0xFFFFu, "actual plan capacity and record ownership");
		m->ps2_vis = 1; m->ps2_want = (UINT8)(i + 1);
		variant.ps2_twin = m; int visible;
		word(plan_wants(m)); word(dc_key(m)); word(tex_want(&variant, &visible)); word((UINT32)visible);
		check(tex_want(&variant, &visible) == (i < PLAN_MAX ? i + 1 : 0), "variant driver lookup reads original plan");
		m->ps2_full_fr = H.frame_no; check(!tex_want(m, &visible), "recent full-size selection stays full size");
		m->ps2_full_fr = 0; m->ps2_vis = 0; check(!tex_want(m, &visible) && (i >= PLAN_MAX || !visible), "actual visibility policy preserved");
		imm_level = 7; check(tex_want(m, &visible) == 7 && visible, "immediate level retains existing override"); imm_level = -1;
		snapshot(m);
	}
	H.frame_no = 0xFFFFFFFFu; plan_n = 0; values[0].ps2_planfr = 123;
	check(plan_key(&values[0]) && values[0].ps2_planfr == 0, "actual frame wrap preserved");
	finish(3, 10, 0);
}

static void failure_case(int stage, int deny)
{
	begin(1, 0, deny); allocation_fail_at = stage; expecting_error = 1;
	if (!setjmp(error_jump)) { HWR_LoadMapTextures(300); check(0, "injected map-root failure must throw"); }
	expecting_error = 0;
	check(strstr(error_text, "HWR_LoadMapTextures") != NULL, "unchanged root allocation error");
	check(!gl_textures && !gl_flats && !gl_numtextures, "failed unpublished map roots cleaned together");
	check(!libc_now && !spill_now && Z_TotalUsage() == 0, "failed roots leave no libc or arena payload");
	allocation_fail_at = 0; HWR_LoadMapTextures(301); check(gl_maptexturesloaded, "reload after root failure"); HWR_FreeMapTextures();
	word((UINT32)stage); finish(4, (size_t)stage, request_bytes);
}

static void variant_failure_case(unsigned mode, int deny)
{
	begin(1, 0, deny);
	texture_t one = {8, 8, false}; texture_t *list[1] = {&one}; INT32 translation[1] = {0};
	textures = list; texturetranslation = translation;
	patch_t *patch = NULL;
	UINT8 map[256]; memset(map, 0x57, sizeof map);
	levelflat_t flat = {LEVELFLAT_TEXTURE, 0};
	if (mode < 2) HWR_LoadMapTextures(1);
	else { patch = Z_Calloc(sizeof *patch, PU_PATCH, NULL); patch->width = 8; patch->height = 8; Patch_AllocateHardwarePatch(patch); }
	allocation_fail_at = allocation_calls + 1; expecting_error = 1;
	if (!setjmp(error_jump))
	{
		if (mode == 0) HWR_GetTexture(0, true);
		else if (mode == 1) HWR_GetLevelFlat(&flat, true);
		else HWR_GetMappedPatch(patch, map);
		check(0, "injected variant allocation must throw");
	}
	expecting_error = 0;
	check(strstr(error_text, "Out of memory") != NULL, "unchanged variant allocation error");
	if (mode == 0) check(!gl_textures[0].mipmap.nextcolormap, "failed map variant never publishes a partial alias");
	else if (mode == 1) check(!gl_flats[0].mipmap.nextcolormap, "failed flat variant never publishes a partial alias");
	else check(!((GLPatch_t *)patch->hardware)->mipmap->nextcolormap, "failed mapped patch never publishes a partial alias");
	allocation_fail_at = 0;
	if (mode == 0) { HWR_GetTexture(0, true); snapshot(current_texture); HWR_FreeMapTextures(); }
	else if (mode == 1) { HWR_GetLevelFlat(&flat, true); snapshot(current_texture); HWR_FreeMapTextures(); }
	else { HWR_GetMappedPatch(patch, map); snapshot(current_texture); HWR_FreeTexture(patch); Z_Free(patch); }
	word(mode); finish(5, mode, request_bytes);
}

int main(int argc, char **argv)
{
#ifdef __mips__
	init_scr();
#endif
	check(argc == 2, "snapshot output path"); snapshot_file = fopen(argv[1], "wb"); check(snapshot_file != NULL, "open snapshot");
	ship_sides = za_twosided; ship_prefer = za_prefer;
	const size_t counts[] = {1, 2, 3, 7, 16, 31, 64, 127, 255, 256, 257, 1023, 65535};
	for (int sides = 0; sides <= 1; sides++) for (int guard = 0; guard <= 1; guard++)
	{
		for (unsigned i = 0; i < sizeof counts / sizeof counts[0]; i++) table_case(counts[i], sides, guard);
		for (int deny = 0; deny <= 1; deny++)
		{
			/* Includes EE and native counts immediately around both layouts' real
			 * 24 KiB spill thresholds, then a full-width large table count. */
			const size_t roots[] = {1, 204, 205, 219, 220, 255, 256, 257, 273, 274, 292, 293, 1023, 65535};
			for (unsigned i = 0; i < sizeof roots / sizeof roots[0]; i++)
			{
				size_t n = roots[i];
				texture_t *storage = malloc(n * sizeof *storage); textures = malloc(n * sizeof *textures); texturetranslation = malloc(n * sizeof *texturetranslation);
				check(storage && textures && texturetranslation, "resource metadata boundary");
				for (size_t j = 0; j < n; j++) { storage[j] = (texture_t){8, 8, false}; textures[j] = &storage[j]; texturetranslation[j] = (INT32)j; }
				for (unsigned j = 0; j < 256; j++) flat_source[j] = (UINT8)(j ^ 0xCA);
				map_case(n, sides, guard, deny);
				free(storage); free(textures); free(texturetranslation);
			}
			patch_case(sides, guard, deny);
		}
		driver_case(sides, guard);
	}
	for (int deny = 0; deny <= 1; deny++) for (int stage = 1; stage <= 2; stage++) failure_case(stage, deny);
	for (int deny = 0; deny <= 1; deny++) for (unsigned mode = 0; mode < 3; mode++) variant_failure_case(mode, deny);
	check(fclose(snapshot_file) == 0, "close literal snapshot"); memory_backing_check();
#ifndef __mips__
	aligned_release(arena_raw);
#endif
	printf("MEMORY9 DONE cases=%u checks=%u sizeof=%zu map=%zu align=%zu\n", cases, checks, sizeof(GLMipmap_t), sizeof(GLMapTexture_t), _Alignof(GLMipmap_t));
	return 0;
}
