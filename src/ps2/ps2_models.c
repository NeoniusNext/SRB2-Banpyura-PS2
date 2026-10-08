// SONIC ROBO BLAST 2 (PS2 port)
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps2_models.c
/// \brief OPT11-MODEL (PS2-HW-260..): 3D models from the cooked pack MODELS.PAK. See ps2_models.h and tools/ps2/cook_models.py (formats SRMD / SRMT / SRMB).
///
/// Why not hw_md3load.c: it reads the whole .md3 with malloc (1.0 - 1.6 MB of a 1.6 MB C heap), makes two zone blocks per surface and frame (SONIC: 400) and one more index array
/// per frame (leaked), and the textures it feeds the driver are 32 bit (a GS texture of 256 x 256 CT32 is 256 KB of the 3.2 MB texture pool). Here a model is ONE zone block holding the
/// structures the drawing code reads (model_t, mesh_t, tinyframe_t) followed by the lump of the pack, whose arrays are used in place; a model that was not used this frame is
/// given back when the zone runs out of room (reclaim hook), and a model that does not fit is a sprite, never an error.

#include "../doomdef.h"
#include "../doomstat.h"
#include "../d_main.h"
#include "../z_zone.h"
#include "../w_wad.h"
#include "../w_pack.h"
#include "../m_misc.h"
#include "../m_argv.h"
#include "../i_system.h"
#include "../command.h"
#include "../hardware/hw_glob.h"
#include "ps2_models.h"
#include "hw/ps2_hwd.h" // PS2HWD_ModelWorkReclaim
#include "ps2_mem.h"

#ifdef PS2_PROFILE

#define SRMD_VERSION 1
#define SRMD_HDR 32
#define SRMD_SURF 48
#define MAXLIVE 48

// ---- the pack -----------------------------------------------------------------------------------------------------------------

static struct
{
	int state; // 0 not looked for, 1 open, -1 absent / damaged
	FILE *handle;
	void *iobuf;
	lumpinfo_t *lumps;
	UINT16 n;
	void *pool;
	UINT8 pal[256][3]; // the palette the textures of the pack were made for
	boolean palok;
} PK;

static size_t m_reserve = 1280u * 1024u; // the arena that stays free after a model was loaded (-hwmodelres KiB)
static size_t m_budget = 5u * 1024u * 1024u; // the models alive at once (-hwmodelbudget KiB)
static boolean m_opts;

static void Opts(void)
{
	if (m_opts)
		return;
	m_opts = true;
	if (M_CheckParm("-hwmodelres") && M_IsNextParm())
		m_reserve = (size_t)atoi(M_GetNextParm()) * 1024u;
	if (M_CheckParm("-hwmodelbudget") && M_IsNextParm())
		m_budget = (size_t)atoi(M_GetNextParm()) * 1024u;
}

// fault injection (-hwmodelfail N[,K]): the N-th (and the K - 1 following) allocation of the model code fails as if the zone were full: a model, a texture, a work area
// must then be a sprite / the plain texture for a while, never an error (tools/ps2/md_oom.sh)
static int inj_n, inj_k = 1, inj_count;
static UINT32 st_injected;

void *PS2Models_TryAlloc(size_t size, INT32 tag, void *user, INT32 alignbits)
{
	static boolean inj_init;

	if (!inj_init)
	{
		inj_init = true;
		if (M_CheckParm("-hwmodelfail") && M_IsNextParm())
		{
			const char *a = M_GetNextParm();

			inj_n = atoi(a);
			while (*a && *a != ',')
				a++;
			if (*a == ',')
				inj_k = atoi(a + 1);
			if (inj_k < 1)
				inj_k = 1;
		}
	}
	inj_count++;
	if (inj_n > 0 && inj_count >= inj_n && inj_count < inj_n + inj_k)
	{
		st_injected++;
		return NULL;
	}
	return Z_TryMallocAlign(size, tag, user, alignbits);
}

static const lumpinfo_t *PK_Find(const char *rel)
{
	UINT16 i;

	if (!PK.lumps)
		return NULL;
	for (i = 0; i < PK.n; i++)
		if (!stricmp(PK.lumps[i].fullname, rel))
			return &PK.lumps[i];
	return NULL;
}

static boolean PK_Open(void)
{
	FILE *f = NULL;
	UINT16 n = 0;
	void *pool = NULL;
	boolean nonmusic = false;
	lumpinfo_t *lumps;
	void *iobuf;
	int d;
	const char *dir[2];

	if (PK.state)
		return PK.state > 0;
	PK.state = -1;
	Opts();
	dir[0] = srb2path;
	dir[1] = srb2home;
	for (d = 0; d < 2 && !f; d++)
		if (dir[d][0])
			f = fopen(va("%s" PATHSEP "MODELS.PAK", dir[d]), "rb");
	if (!f)
		return false;
	iobuf = WPack_SetupHandle(f);
	if (!iobuf)
	{
		fclose(f);
		return false;
	}
	if (!WPack_Detect(f) || !(lumps = WPack_GetLumps(f, &n, &pool, &nonmusic)))
	{
		fclose(f);
		free(iobuf);
		CONS_Alert(CONS_WARNING, "MODELS.PAK is not a valid pack; models are not used\n");
		return false;
	}
	PK.handle = f;
	PK.iobuf = iobuf;
	PK.lumps = lumps;
	PK.n = n;
	PK.pool = pool;
	PK.state = 1;
	{
		const lumpinfo_t *pl = PK_Find("PLAYPAL");

		if (pl && pl->size == 768 && WPack_ReadLump(f, pl, PK.pal, 768, 0) == 768)
			PK.palok = true;
	}
	CONS_Printf("MODELS.PAK: %u lumps\n", (unsigned)n);
	return true;
}

boolean PS2Models_PackPresent(void)
{
	return PK_Open();
}

char *PS2Models_Dat(size_t *size)
{
	const lumpinfo_t *l;
	char *buf;

	if (!PK_Open() || !(l = PK_Find("models.dat")) || l->size == 0 || l->size > 65536)
		return NULL;
	buf = malloc(l->size + 1);
	if (!buf)
		return NULL;
	if (WPack_ReadLump(PK.handle, l, buf, l->size, 0) != l->size)
	{
		free(buf);
		return NULL;
	}
	buf[l->size] = '\0';
	*size = l->size;
	return buf;
}

// ---- live models -------------------------------------------------------------------------------------------------------------

typedef struct
{
	model_t *model;
	size_t bytes;
	UINT32 stamp; // Z_FrameCount() of the last use
	md2_t *md2; // its owner (the table entry whose retry time is set when the model is taken back)
	float radius; // of the model, in the units of its vertices (1/64 of an MD3 unit): the farthest vertex over every frame
} live_t;

static live_t live[MAXLIVE];
static size_t live_bytes;
static boolean hook_added;
static UINT32 st_loads, st_frees, st_reclaims, st_fail_mem, st_fail_bad, st_reload_same_frame, st_load_cyc, st_load_max;

static live_t *Live_Find(const model_t *m)
{
	int i;

	for (i = 0; i < MAXLIVE; i++)
		if (live[i].model == m)
			return &live[i];
	return NULL;
}

boolean PS2Models_Is(const model_t *model)
{
	return model && Live_Find(model) != NULL;
}

float PS2Models_Radius(const model_t *model)
{
	const live_t *l = Live_Find(model);

	return l ? l->radius : 0.0f;
}

void PS2Models_Touch(model_t *model)
{
	live_t *l = Live_Find(model);

	if (l)
		l->stamp = Z_FrameCount();
}

void PS2Models_Free(model_t *model)
{
	live_t *l = Live_Find(model);
	int i;

	if (!model)
		return;
	if (l)
	{
		live_bytes -= l->bytes;
		l->model = NULL;
		l->bytes = 0;
	}
	if (model->spr2frames)
		Z_Free(model->spr2frames);
	if (model->superspr2frames)
		Z_Free(model->superspr2frames);
	model->spr2frames = model->superspr2frames = NULL;
	for (i = 0; i < model->numMeshes; i++)
	{
		mesh_t *mesh = &model->meshes[i];

		if (mesh->uvs && mesh->uvs != mesh->originaluvs) // adjustTextureCoords made its own array (a model that draws with a sprite)
			Z_Free(mesh->uvs);
		mesh->uvs = NULL;
	}
	st_frees++;
	Z_Free(model); // the owner (md2_t.model) is cleared
}

// a model of more than SLICE_BYTES is read a slice per frame (the pack read of 1.5 MB was 18 M cycles, 60 ms, in one frame: a hitch of two ticks): the block waits here, unowned
// (the owner is set when the last slice is in), and the model is a sprite meanwhile (PS2Models_Load answers why = 3 until then)
#define SLICE_BYTES (256u * 1024u) // four LZ4 blocks of the pack
static struct
{
	boolean active;
	const lumpinfo_t *l;
	UINT8 *base;
	size_t need;
	UINT32 got, stamp;
} pend;

static void Pend_Cancel(void)
{
	if (pend.active)
	{
		Z_Free(pend.base);
		pend.active = false;
		pend.base = NULL;
	}
}

// the least recently used model not used in this frame; NULL if every one is in use
static live_t *Live_Victim(UINT32 minage)
{
	const UINT32 now = Z_FrameCount();
	live_t *v = NULL;
	int i;

	for (i = 0; i < MAXLIVE; i++)
		if (live[i].model && live[i].stamp != now && now - live[i].stamp >= minage && (!v || (INT32)(live[i].stamp - v->stamp) < 0))
			v = &live[i];
	return v;
}

#define RETRY_FRAMES 280 // a model taken back is not loaded again for this many frames (8 seconds): memory is short, a sprite is drawn meanwhile

static size_t Reclaim(size_t want)
{
	size_t got = PS2HWD_ModelWorkReclaim(); // the work areas of the drawing (cheap to make again) go first
	live_t *v;

	if (pend.active)
	{
		got += pend.need;
		Pend_Cancel(); // a half read model goes next
	}

	while (got < want && (v = Live_Victim(0)) != NULL)
	{
		md2_t *md2 = v->md2;

		got += v->bytes;
		PS2Models_Free(v->model);
		if (md2)
			md2->ps2_retry = Z_FrameCount() + RETRY_FRAMES;
		st_reclaims++;
	}
	return got;
}

// ---- reading a lump ----------------------------------------------------------------------------------------------------------

static size_t ReadLump(const lumpinfo_t *l, void *dst, size_t size, size_t off)
{
	if (off >= l->size)
		return 0;
	if (size > l->size - off)
		size = l->size - off;
	return WPack_ReadLump(PK.handle, l, dst, size, off) == size ? size : 0;
}

// ---- the palette ---------------------------------------------------------------------------------------------------------------

static UINT32 pal_hash_frame = 0xFFFFFFFFu, pal_hash_val;

UINT32 PS2Models_PaletteHash(void)
{
	const UINT32 now = Z_FrameCount();

	if (pal_hash_frame != now)
	{
		const RGBA_t *p = HWR_GetTexturePalette();
		UINT32 h = 2166136261u;
		int i;

		for (i = 0; i < 256; i++)
		{
			h = (h ^ p[i].s.red) * 16777619u;
			h = (h ^ p[i].s.green) * 16777619u;
			h = (h ^ p[i].s.blue) * 16777619u;
		}
		pal_hash_val = h;
		pal_hash_frame = now;
	}
	return pal_hash_val;
}

// nearest colour of the texture palette (r_data.c NearestPaletteColor: squared distance, the first of equals), never index 255 (the key of the holes)
static UINT8 Nearest(const RGBA_t *pal, int r, int g, int b)
{
	int best = 256 * 256 * 4, bi = 0, i;

	for (i = 0; i < 255; i++)
	{
		const int dr = r - pal[i].s.red, dg = g - pal[i].s.green, db = b - pal[i].s.blue;
		const int d = dr * dr + dg * dg + db * db;

		if (d < best)
		{
			if (!d)
				return (UINT8)i;
			best = d;
			bi = i;
		}
	}
	return (UINT8)bi;
}

// the colour a palette rendering texel falls to: the 64 x 64 x 64 lookup table of the OpenGL shader (cell = floor(v / 255 * 63 + 0.5), colour 4 * cell)
static inline int CellOf(int v)
{
	return (v * 126 + 255) / 510;
}

#define QCACHE 2048
static struct
{
	UINT32 key; // 0x80000000 | rgb cell (18 bits); 0 = empty
	UINT8 idx;
} qcache[QCACHE];
static UINT32 qcache_pal;

static UINT8 QuantizeTexel(const RGBA_t *pal, int r, int g, int b)
{
	const int cr = CellOf(r), cg = CellOf(g), cb = CellOf(b);
	const UINT32 key = 0x80000000u | ((UINT32)cr << 12) | ((UINT32)cg << 6) | (UINT32)cb;
	const UINT32 slot = (key * 2654435761u) >> 21; // 11 bits

	if (qcache[slot].key != key)
	{
		qcache[slot].key = key;
		qcache[slot].idx = Nearest(pal, cr * 4, cg * 4, cb * 4);
	}
	return qcache[slot].idx;
}

static void QCache_Check(void)
{
	const UINT32 ph = PS2Models_PaletteHash();

	if (qcache_pal != ph)
	{
		memset(qcache, 0, sizeof qcache);
		qcache_pal = ph;
	}
}

UINT8 PS2Models_QuantizeOne(int r, int g, int b)
{
	QCache_Check();
	return QuantizeTexel(HWR_GetTexturePalette(), r, g, b);
}

void PS2Models_FreeAll(void)
{
	int i;

	Pend_Cancel();
	for (i = 0; i < MAXLIVE; i++)
		if (live[i].model)
			PS2Models_Free(live[i].model);
}

void PS2Models_Quantize(const UINT8 *rgba, int w, int h, UINT8 *dst, boolean *holes)
{
	const RGBA_t *pal = HWR_GetTexturePalette();
	int n = w * h, i;
	boolean hole = false;

	QCache_Check();
	for (i = 0; i < n; i++, rgba += 4)
	{
		if (rgba[3] < 128)
		{
			dst[i] = 255;
			hole = true;
		}
		else
		{
			dst[i] = QuantizeTexel(pal, rgba[0], rgba[1], rgba[2]);
		}
	}
	*holes = hole;
}

// index -> index: the colours of the cooker's palette in the current texture palette (identity when the palettes are the same)
static boolean RemapFor(UINT8 remap[256])
{
	const RGBA_t *pal = HWR_GetTexturePalette();
	boolean same = true;
	int i;

	if (!PK.palok)
		return false; // the pack carries no palette: its indices are used as they are
	for (i = 0; i < 256; i++)
		if (pal[i].s.red != PK.pal[i][0] || pal[i].s.green != PK.pal[i][1] || pal[i].s.blue != PK.pal[i][2])
		{
			same = false;
			break;
		}
	if (same)
		return false;
	for (i = 0; i < 255; i++)
		remap[i] = Nearest(pal, PK.pal[i][0], PK.pal[i][1], PK.pal[i][2]);
	remap[255] = 255;
	return true;
}

// ---- textures ------------------------------------------------------------------------------------------------------------------

boolean PS2Models_LoadTexture(const char *rel, GLMipmap_t *mm)
{
	const lumpinfo_t *l;
	UINT8 hdr[16];
	UINT16 w, h;
	UINT32 flags;
	UINT8 *data;
	size_t n;

	if (!PK_Open() || !(l = PK_Find(rel)) || l->size < 16 || ReadLump(l, hdr, 16, 0) != 16)
		return false;
	if (memcmp(hdr, "SRMT", 4) != 0 || LONG(*(UINT32 *)(void *)(hdr + 4)) != 1)
		return false;
	w = SHORT(*(UINT16 *)(void *)(hdr + 8));
	h = SHORT(*(UINT16 *)(void *)(hdr + 10));
	flags = LONG(*(UINT32 *)(void *)(hdr + 12));
	n = (size_t)w * h;
	if (!w || !h || w > 1024 || h > 1024 || l->size != 16 + n)
		return false;
	data = PS2Models_TryAlloc(n, PU_HWRMODELTEXTURE_UNLOCKED, &mm->data, 4);
	if (!data)
	{
		st_fail_mem++;
		return false;
	}
	if (ReadLump(l, data, n, 16) != n)
	{
		Z_Free(data);
		return false;
	}
	{
		UINT8 remap[256];

		if (RemapFor(remap))
		{
			size_t i;

			for (i = 0; i < n; i++)
				data[i] = remap[data[i]];
		}
	}
	mm->width = w;
	mm->height = h;
	mm->format = GL_TEXFMT_P_8;
	mm->flags = (flags & 1) ? TF_CHROMAKEYED : 0;
	return true;
}

// ---- blend maps ----------------------------------------------------------------------------------------------------------------

boolean PS2Models_HasBlend(const char *rel)
{
	return PK_Open() && PK_Find(rel) != NULL;
}

boolean PS2Models_LoadBlend(const char *rel, ps2_blend_t *out)
{
	const lumpinfo_t *l;
	UINT8 *b;
	UINT32 nruns, npix;
	size_t need;

	memset(out, 0, sizeof *out);
	if (!PK_Open() || !(l = PK_Find(rel)) || l->size < 20)
		return false;
	b = PS2Models_TryAlloc(l->size, PU_STATIC, NULL, 4);
	if (!b)
	{
		st_fail_mem++;
		return false;
	}
	if (ReadLump(l, b, l->size, 0) != l->size || memcmp(b, "SRMB", 4) != 0)
	{
		Z_Free(b);
		return false;
	}
	out->w = SHORT(*(UINT16 *)(void *)(b + 8));
	out->h = SHORT(*(UINT16 *)(void *)(b + 10));
	nruns = LONG(*(UINT32 *)(void *)(b + 12));
	npix = LONG(*(UINT32 *)(void *)(b + 16));
	need = 20 + (size_t)nruns * 8 + (size_t)npix * 4;
	if (need != l->size || npix > (UINT32)out->w * out->h)
	{
		Z_Free(b);
		return false;
	}
	out->nruns = nruns;
	out->npix = npix;
	out->run = (const UINT32 *)(const void *)(b + 20);
	out->px = b + 20 + (size_t)nruns * 8;
	out->block = b;
	return true;
}

void PS2Models_FreeBlend(ps2_blend_t *b)
{
	if (b->block)
		Z_Free(b->block);
	memset(b, 0, sizeof *b);
}

// ---- models --------------------------------------------------------------------------------------------------------------------

#define ALIGN16(x) (((x) + 15u) & ~15u)

static inline UINT32 U32At(const UINT8 *p) { return LONG(*(const UINT32 *)(const void *)p); }
static inline UINT16 U16At(const UINT8 *p) { return SHORT(*(const UINT16 *)(const void *)p); }

static int Slot(void)
{
	int i;

	for (i = 0; i < MAXLIVE; i++)
		if (!live[i].model)
			return i;
	return -1;
}

#define MAX_LOADS_PER_FRAME 2
static UINT32 ld_frame = 0xFFFFFFFFu, ld_count;

model_t *PS2Models_Load(const char *rel, void **owner, int *why)
{
	const lumpinfo_t *l;
	UINT8 hdr[SRMD_HDR];
	UINT32 total, namesOff, surfOff, nsurf, nframes, ovh, i;
	UINT8 *base, *blob;
	model_t *model;
	mesh_t *meshes;
	tinyframe_t *tf;
	material_t *mat;
	size_t need;
	int slot;
	boolean resume;

	const unsigned c0 = PS2Mem_Cycles();

	*why = 0;
	if (pend.active && (INT32)(Z_FrameCount() - pend.stamp) > 120)
		Pend_Cancel(); // nobody asked for it any more (the object is gone, the level changed)
	if (!PK_Open() || !(l = PK_Find(rel)))
		return NULL;
	{
		// at most MAX_LOADS_PER_FRAME models are read in one frame (a load is up to 7 M cycles: a level start with ten new models in view would be a hitch of 70 ms):
		// the others are sprites for a frame or two (why = 3: ask again next frame)
		const UINT32 now = Z_FrameCount();

		if (ld_frame != now)
		{
			ld_frame = now;
			ld_count = 0;
		}
		if (ld_count >= MAX_LOADS_PER_FRAME)
		{
			*why = 3;
			return NULL;
		}
		ld_count++;
	}
	if (l->size < SRMD_HDR || ReadLump(l, hdr, SRMD_HDR, 0) != SRMD_HDR || memcmp(hdr, "SRMD", 4) != 0 || U32At(hdr + 4) != SRMD_VERSION)
	{
		*why = 2;
		return NULL;
	}
	total = U32At(hdr + 8);
	nsurf = U16At(hdr + 12);
	nframes = U16At(hdr + 14);
	namesOff = U32At(hdr + 16);
	surfOff = U32At(hdr + 20);
	if (total != l->size || nsurf < 1 || nsurf > 32 || nframes < 1 || nframes > 1024
		|| (size_t)namesOff + (size_t)nframes * 16 > total || (size_t)surfOff + (size_t)nsurf * SRMD_SURF > total)
	{
		*why = 2;
		return NULL;
	}
	ovh = ALIGN16((UINT32)sizeof(model_t)) + ALIGN16(nsurf * (UINT32)sizeof(mesh_t)) + ALIGN16(nsurf * nframes * (UINT32)sizeof(tinyframe_t)) + ALIGN16(nsurf * (UINT32)sizeof(material_t));
	need = ovh + total;
	Opts();
	// the models alive at once: give back those not used in this frame before the budget is passed
	while (live_bytes + need > m_budget)
	{
		live_t *v = Live_Victim(105); // over the budget: a model that was not used for three seconds goes (one in use is never thrown out to be loaded again)

		if (!v)
			break;
		PS2Models_Free(v->model);
	}
	resume = pend.active && pend.l == l;
	if (!resume && pend.active && total > SLICE_BYTES)
	{
		*why = 3; // another big model is being read: one at a time
		return NULL;
	}
	if (resume)
	{
		base = pend.base;
	}
	else
	{
		if (live_bytes + need > m_budget + m_budget / 2 || Slot() < 0)
		{
			*why = 1;
			st_fail_mem++;
			return NULL;
		}
		if (!hook_added)
		{
			Z_AddReclaimHook(Reclaim);
			hook_added = true;
		}
		base = PS2Models_TryAlloc(need, PU_STATIC, total > SLICE_BYTES ? NULL : owner, 4); // alignbits: log2 (16 bytes); a model read in slices has no owner until it is complete
		if (!base)
		{
			*why = 1;
			st_fail_mem++;
			return NULL;
		}
	}
	blob = base + ovh;
	if (total > SLICE_BYTES)
	{
		const UINT32 got = resume ? pend.got : 0, n = total - got < SLICE_BYTES ? total - got : SLICE_BYTES;

		if (ReadLump(l, blob + got, n, got) != n)
		{
			pend.active = false;
			Z_Free(base);
			*why = 2;
			st_fail_bad++;
			return NULL;
		}
		if (got + n < total)
		{
			pend.active = true;
			pend.l = l;
			pend.base = base;
			pend.need = need;
			pend.got = got + n;
			pend.stamp = Z_FrameCount();
			*why = 3; // the next slice in the next frame
			return NULL;
		}
		pend.active = false;
		pend.base = NULL;
		Z_SetUser(base, owner); // complete: *owner = base
		if (live_bytes + need > m_budget + m_budget / 2 || (slot = Slot()) < 0)
		{
			Z_Free(base);
			*why = 1;
			st_fail_mem++;
			return NULL;
		}
	}
	else
	{
		slot = Slot();
		if (slot < 0 || ReadLump(l, blob, total, 0) != total)
		{
			Z_Free(base); // owner cleared
			*why = slot < 0 ? 1 : 2;
			if (slot < 0)
				st_fail_mem++;
			else
				st_fail_bad++;
			return NULL;
		}
	}
	if (Z_ArenaFree() < m_reserve) // the frame needs working memory too: a model that would eat it is a sprite
	{
		Z_Free(base);
		*why = 1;
		st_fail_mem++;
		return NULL;
	}
	memset(base, 0, ovh);
	{
		UINT32 o = 0;

		model = (model_t *)(void *)(base + o);
		o += ALIGN16((UINT32)sizeof(model_t));
		meshes = (mesh_t *)(void *)(base + o);
		o += ALIGN16(nsurf * (UINT32)sizeof(mesh_t));
		tf = (tinyframe_t *)(void *)(base + o);
		o += ALIGN16(nsurf * nframes * (UINT32)sizeof(tinyframe_t));
		mat = (material_t *)(void *)(base + o);
	}
	model->numMeshes = (int)nsurf;
	model->meshes = meshes;
	model->numMaterials = (int)nsurf;
	model->materials = mat;
	model->frameNames = (char *)(blob + namesOff);
	model->maxNumFrames = (int)nframes;
	{
		UINT32 ti = 0;

		for (i = 0; i < nsurf; i++)
		{
			const UINT8 *s = blob + surfOff + i * SRMD_SURF;
			const UINT32 nv = U32At(s), nt = U32At(s + 4), nf = U32At(s + 8), uvOff = U32At(s + 12), idxOff = U32At(s + 16), posOff = U32At(s + 20), nrmOff = U32At(s + 24);
			mesh_t *mesh = &meshes[i];
			UINT32 j;

			if (!nv || nv > 65535 || !nt || nt > 0x3FFFF || !nf || nf > nframes
				|| (size_t)uvOff + (size_t)nv * 8 > total || (size_t)idxOff + (size_t)nt * 6 > total
				|| (size_t)posOff + (size_t)nf * nv * 6 > total || (size_t)nrmOff + (size_t)nf * nv > total
				|| (uvOff & 3) || (idxOff & 1) || (posOff & 1))
			{
				Z_Free(base);
				*why = 2;
				st_fail_bad++;
				return NULL;
			}
			{
				const unsigned short *ix = (const unsigned short *)(const void *)(blob + idxOff);
				UINT32 k;

				for (k = 0; k < nt * 3; k++)
					if (ix[k] >= nv)
					{
						Z_Free(base);
						*why = 2;
						st_fail_bad++;
						return NULL;
					}
			}
			mesh->numVertices = (int)nv;
			mesh->numTriangles = (int)nt;
			mesh->numFrames = (int)nf;
			mesh->uvs = (float *)(void *)(blob + uvOff);
			mesh->originaluvs = mesh->uvs;
			mesh->indices = (unsigned short *)(void *)(blob + idxOff);
			mesh->tinyframes = &tf[ti];
			for (j = 0; j < nf; j++, ti++)
			{
				tf[ti].material = &mat[i];
				tf[ti].vertices = (short *)(void *)(blob + posOff + (size_t)j * nv * 6);
				tf[ti].normals = (char *)(blob + nrmOff + (size_t)j * nv);
			}
		}
	}
	model->max_s = model->max_t = model->vbo_max_s = model->vbo_max_t = 1.0f;
	for (i = 0; i < (UINT32)model->numMaterials; i++)
	{
		material_t *m = &model->materials[i];

		m->ambient[0] = m->ambient[1] = m->ambient[2] = 0.7686f;
		m->ambient[3] = 1.0f;
		m->diffuse[0] = m->diffuse[1] = m->diffuse[2] = 0.5863f;
		m->diffuse[3] = 1.0f;
	}
	LoadModelSprite2(model);
	if (!model->spr2frames)
		LoadModelInterpolationSettings(model);
	live[slot].model = model;
	live[slot].md2 = (md2_t *)(void *)((char *)owner - offsetof(md2_t, model)); // owner = &md2->model
	live[slot].bytes = need;
	live[slot].stamp = Z_FrameCount();
	{
		UINT32 rb = U32At(hdr + 28);
		float rf;

		memcpy(&rf, &rb, sizeof rf);
		live[slot].radius = rf > 0.0f && rf < 1.0e7f ? rf : 0.0f;
	}
	live_bytes += need;
	st_loads++;
	{
		const unsigned dt = PS2Mem_Cycles() - c0;

		st_load_cyc += dt;
		if (dt > st_load_max)
			st_load_max = dt;
	}
	return model;
}

void PS2Models_Stats(unsigned int *out)
{
	int i, n = 0;

	for (i = 0; i < MAXLIVE; i++)
		if (live[i].model)
			n++;
	out[0] = (unsigned)n;
	out[1] = (unsigned)(live_bytes / 1024);
	out[2] = st_loads;
	out[3] = st_frees;
	out[4] = st_reclaims;
	out[5] = st_fail_mem;
	out[6] = st_fail_bad;
	out[9] = st_injected;
	out[7] = st_load_cyc;
	out[8] = st_load_max;
}

void PS2Models_Report(void)
{
	int i, n = 0;

	for (i = 0; i < MAXLIVE; i++)
		if (live[i].model)
			n++;
	CONS_Printf("models: pack %s, %d alive (%u KiB), loads %u, frees %u, reclaims %u, no memory %u, damaged %u, budget %u KiB, reserve %u KiB, injected failures %u\n",
		PK.state > 0 ? "open" : PK.state < 0 ? "absent" : "unopened", n, (unsigned)(live_bytes / 1024), st_loads, st_frees, st_reclaims, st_fail_mem, st_fail_bad,
		(unsigned)(m_budget / 1024), (unsigned)(m_reserve / 1024), st_injected);
	(void)st_reload_same_frame;
}

#endif // PS2_PROFILE
