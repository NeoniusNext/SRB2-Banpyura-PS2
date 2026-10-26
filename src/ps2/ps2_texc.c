// SONIC ROBO BLAST 2 (PS2 port)
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps2_texc.c
/// \brief OPT13 IZ (PS2-602, R2): composite textures prebuilt by the cooker. See ps2_texc.h and docs/PACK_FORMAT.md ("TEXC.PAK").
///
/// A wall texture of the hardware renderer is made of patches (HWR_GenerateTexture): for a 256x256 texture of 81 placements 4 M EE cycles with the patches in the zone, 260..320 M when the
/// zone dropped them (a lump read from the pack costs 4 M cycles whatever its size), the sky (151 patches) 26 M, and these builds fall in the middle of a frame whenever a texture is
/// asked for for the first time or after the zone dropped its data. The cooker (the host engine with -texcdump, tools/ps2/cook.py --texc) makes the same pixels once and packs them LZ4HC
/// into TEXC.PAK (5.5% of their size). Here a texture whose definition (and the packs its patches come from) is the one the pack was made for is read from it: one lump read, or none
/// when the level's prefetch has the stored form in memory, and a decode of 0.1..1.5 M cycles. Any other texture (an add-on's, a patch that is not from the cooked packs) is composed as before.

#include "../doomdef.h"
#include "../doomstat.h"
#include "../d_main.h"
#include "../z_zone.h"
#include "../w_wad.h"
#include "../w_pack.h"
#include "../m_argv.h"
#include "../i_system.h"
#include "../r_data.h"
#include "../r_textures.h"
#include "../r_state.h"
#include "../r_sky.h"
#include "../r_patch.h"
#include "../r_picformats.h"
#include "../p_spec.h"
#include "ps2_texc.h"

#ifdef PS2_PROFILE

#ifndef PS2
#define Z_TryMallocAlign(s, t, u, a) Z_MallocAlign(s, t, u, a) // host engine: no zone arena, no failure (as in w_pack.c)
#endif

#define TEXC_HOLE 255 // HWR_PATCHES_CHROMAKEY_COLORINDEX: what a texel no patch covers is
#define TEXC_MAXBYTES (1u << 20) // the largest texture with a stored composite
#define TEXC_FORMAT 1 // part of every key: a change of the composition rules makes every old pack miss

#ifdef _EE
static inline UINT32 TC_Cyc(void)
{
	UINT32 v;

	__asm__ volatile("mfc0 %0,$9" : "=r"(v));
	return v;
}
#else
static inline UINT32 TC_Cyc(void)
{
	return 0;
}
#endif

static struct
{
	int state; // 0 not looked for, 1 open, -1 absent / damaged / switched off
	FILE *handle;
	void *iobuf;
	lumpinfo_t *lumps;
	UINT16 n;
	void *pool;
	struct tc_key_s { UINT64 key; UINT16 lump; } *keys; // sorted by key
	UINT32 nkeys;
	INT32 ntex; // the texture list the per-texture tables below belong to
	UINT16 *tlump; // per texture: 0 not looked up, 0xFFFF the pack has none, else lump + 1
	UINT8 *arena; // the stored form of the textures of the level (PU_LEVEL while it is filled, then PU_CACHE: the zone clears the pointer when it takes the block back)
	UINT32 *toff; // per texture: offset in arena + 1, or 0
	size_t budget; // -texcmem KiB: the most the prefetch keeps
	UINT32 s_hit, s_miss, s_fail, s_resident, s_read; // window counters: stored composites used / textures the pack has none of / damaged / from the prefetch / lumps read in the frame
	UINT32 s_cyc, s_readcyc;
	UINT32 pre_n, pre_cyc;
	size_t pre_bytes; // of the level
} TC = { 0 };

// ---- the key --------------------------------------------------------------------------------------------------------------

static void TC_Mix(UINT64 *h, UINT32 v)
{
	int i;

	for (i = 0; i < 4; i++)
	{
		*h ^= (v >> (8 * i)) & 0xFF;
		*h *= 0x100000001B3ull;
	}
}

// The translucency tables (TRANS10..TRANS90) a translucent patch is blended with: which lump of which pack they are (0: not all of them are in version 2 packs). The key of a texture with a translucent
// patch holds it, so that an add-on with its own tables never meets a composite made with the game's.
static UINT64 TC_TransIdent(void)
{
	static UINT64 id;
	static INT32 listsize = -1;
	INT32 level;
	UINT64 h = 0xCBF29CE484222325ull;

	if (listsize == numwadfiles)
		return id;
	listsize = numwadfiles;
	id = 0;
	for (level = 1; level <= 9; level++)
	{
		char name[16];
		lumpnum_t l;
		UINT32 pid[3];

		snprintf(name, sizeof name, "TRANS%d0", (int)level);
		l = W_CheckNumForName(name);
		if (l == LUMPERROR || WADFILENUM(l) >= numwadfiles || !WPack_Identity((const wpack_t *)wadfiles[WADFILENUM(l)]->pack, pid))
			return 0;
		TC_Mix(&h, (UINT32)l);
		TC_Mix(&h, pid[0]);
		TC_Mix(&h, pid[1]);
		TC_Mix(&h, pid[2]);
	}
	id = h ? h : 1;
	return id;
}

static UINT64 TC_KeyWhy(INT32 texnum, const char **why)
{
	const texture_t *t;
	UINT64 h = 0xCBF29CE484222325ull, transid = 0;
	boolean usedtrans = false;
	INT32 i;

	*why = "";
	if (texnum < 0 || texnum >= numtextures || !textures || !(t = textures[texnum]))
	{
		*why = "no texture";
		return 0;
	}
	// a flat is a floor (the engine's own flat, no composition) unless it cannot be one: floors are square. The skies that are flats (SKY4: 512x768, 25 M cycles to make) are walls to the renderer.
	if (t->type != TEXTURETYPE_SINGLEPATCH && t->type != TEXTURETYPE_COMPOSITE && !(t->type == TEXTURETYPE_FLAT && t->width != t->height && t->patchcount == 1))
	{
		*why = t->type == TEXTURETYPE_FLAT ? "flat" : "unknown type";
		return 0;
	}
	if (t->width <= 0 || t->height <= 0 || t->patchcount <= 0 || (size_t)t->width * (size_t)t->height > TEXC_MAXBYTES)
	{
		*why = "size";
		return 0;
	}
	TC_Mix(&h, TEXC_FORMAT);
	TC_Mix(&h, (UINT32)(UINT16)t->width | ((UINT32)(UINT16)t->height << 16));
	TC_Mix(&h, (UINT32)t->type | ((UINT32)(UINT16)t->patchcount << 8));
	for (i = 0; i < t->patchcount; i++)
	{
		const texpatch_t *p = &t->patches[i];
		UINT32 id[3];

		if (p->style != AST_COPY)
		{
			// translucent: the blend is a lookup in a table of the game packs (above); the other styles blend in RGB and find the nearest colour of the palette of the moment
			if (p->style != AST_TRANSLUCENT || !(transid = TC_TransIdent()))
			{
				static char buf[40];

				snprintf(buf, sizeof buf, "blend style %d alpha %d", (int)p->style, (int)p->alpha);
				*why = buf;
				return 0;
			}
			if (!usedtrans)
			{
				usedtrans = true;
				TC_Mix(&h, (UINT32)transid);
				TC_Mix(&h, (UINT32)(transid >> 32));
			}
		}
		if (p->wad >= numwadfiles || !wadfiles[p->wad] || !WPack_Identity((const wpack_t *)wadfiles[p->wad]->pack, id))
		{
			*why = "patch not from a version 2 pack";
			return 0;
		}
		TC_Mix(&h, (UINT32)(UINT16)p->originx | ((UINT32)(UINT16)p->originy << 16));
		TC_Mix(&h, (UINT32)p->wad | ((UINT32)p->lump << 16));
		TC_Mix(&h, (UINT32)p->flip | ((UINT32)p->alpha << 8));
		TC_Mix(&h, id[0]);
		TC_Mix(&h, id[1]);
		TC_Mix(&h, id[2]);
	}
	return h ? h : 1;
}

UINT64 PS2TexC_Key(INT32 texnum)
{
	const char *why;

	return TC_KeyWhy(texnum, &why);
}

// ---- the pack ----------------------------------------------------------------------------------------------------------

static int TC_CmpKey(const void *a, const void *b)
{
	const UINT64 ka = ((const struct tc_key_s *)a)->key, kb = ((const struct tc_key_s *)b)->key;

	return ka < kb ? -1 : ka > kb ? 1 : 0;
}

static boolean TC_ParseKey(const char *s, UINT64 *key)
{
	UINT64 v = 0;
	int i;

	for (i = 0; i < 16; i++)
	{
		char c = s[i];

		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= (UINT64)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v |= (UINT64)(c - 'a' + 10);
		else
			return false;
	}
	if (s[16])
		return false;
	*key = v;
	return true;
}

boolean PS2TexC_Present(void)
{
	FILE *f = NULL;
	UINT16 n = 0, i;
	void *pool = NULL;
	boolean nonmusic = false;
	lumpinfo_t *lumps;
	void *iobuf;
	int d;
	const char *dir[2];
	UINT32 nk = 0;

	if (TC.state)
		return TC.state > 0;
	TC.state = -1;
	if (M_CheckParm("-notexc"))
		return false;
	TC.budget = 1024u << 10;
	if (M_CheckParm("-texcmem") && M_IsNextParm())
		TC.budget = (size_t)atoi(M_GetNextParm()) << 10;
	dir[0] = srb2path;
	dir[1] = srb2home;
	for (d = 0; d < 2 && !f; d++)
		if (dir[d] && dir[d][0])
			f = fopen(va("%s" PATHSEP "TEXC.PAK", dir[d]), "rb");
	if (!f)
		return false;
	iobuf = WPack_SetupHandle(f);
	if (!iobuf)
	{
		fclose(f);
		return false;
	}
	if (!WPack_Detect(f) || !(lumps = WPack_GetLumps(f, "TEXC.PAK", &n, &pool, &nonmusic, NULL)))
	{
		fclose(f);
		free(iobuf);
		CONS_Alert(CONS_WARNING, "TEXC.PAK is not a valid pack; the textures are made from their patches\n");
		return false;
	}
	TC.keys = Z_TryMallocAlign((size_t)n * sizeof *TC.keys, PU_STATIC, &TC.keys, 3);
	if (!TC.keys)
	{
		Z_Free(lumps);
		Z_Free(pool);
		fclose(f);
		free(iobuf);
		return false;
	}
	for (i = 0; i < n; i++)
	{
		UINT64 key;

		if (lumps[i].size && TC_ParseKey(lumps[i].longname, &key))
		{
			TC.keys[nk].key = key;
			TC.keys[nk].lump = i;
			nk++;
		}
	}
	qsort(TC.keys, nk, sizeof *TC.keys, TC_CmpKey);
	TC.handle = f;
	TC.iobuf = iobuf;
	TC.lumps = lumps;
	TC.n = n;
	TC.pool = pool;
	TC.nkeys = nk;
	TC.state = nk ? 1 : -1;
	CONS_Printf("TEXC.PAK: %u prebuilt composite textures\n", (unsigned)nk);
	return TC.state > 0;
}

void PS2TexC_Reset(void)
{
	if (TC.tlump)
		Z_Free(TC.tlump);
	if (TC.toff)
		Z_Free(TC.toff);
	TC.tlump = NULL;
	TC.toff = NULL;
	TC.ntex = 0;
}

// the lump of the stored composite of texture `texnum`, -1 if the pack has none
static int TC_Lump(INT32 texnum)
{
	UINT64 key;
	UINT32 lo, hi;

	if (!PS2TexC_Present() || texnum < 0 || texnum >= numtextures)
		return -1;
	if (TC.ntex != numtextures || !TC.tlump)
	{
		PS2TexC_Reset();
		TC.tlump = Z_Calloc((size_t)numtextures * sizeof *TC.tlump, PU_STATIC, &TC.tlump);
		TC.ntex = numtextures;
	}
	if (TC.tlump[texnum])
		return TC.tlump[texnum] == 0xFFFF ? -1 : (int)TC.tlump[texnum] - 1;
	TC.tlump[texnum] = 0xFFFF;
	key = PS2TexC_Key(texnum);
	if (!key)
		return -1;
	for (lo = 0, hi = TC.nkeys; lo < hi;)
	{
		const UINT32 mid = (lo + hi) >> 1;

		if (TC.keys[mid].key < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < TC.nkeys && TC.keys[lo].key == key)
	{
		const lumpinfo_t *l = &TC.lumps[TC.keys[lo].lump];

		if ((size_t)l->size == (size_t)textures[texnum]->width * (size_t)textures[texnum]->height)
		{
			TC.tlump[texnum] = (UINT16)(TC.keys[lo].lump + 1);
			return TC.keys[lo].lump;
		}
	}
	return -1;
}

boolean PS2TexC_Fetch(INT32 texnum, UINT8 *dest, size_t bytes)
{
	const int li = TC_Lump(texnum);
	const lumpinfo_t *l;
	const UINT8 *blob;
	UINT8 *tmp = NULL;
	UINT32 t0 = TC_Cyc(), t1;
	boolean ok;

	if (li < 0)
	{
		TC.s_miss++;
		return false;
	}
	l = &TC.lumps[li];
	if ((size_t)l->size != bytes)
	{
		TC.s_fail++;
		return false;
	}
	if (TC.arena && TC.toff && TC.toff[texnum])
	{
		Z_Touch(TC.arena); // (in use: the zone takes the stored forms back only when nothing has asked for them for a frame)
		blob = TC.arena + TC.toff[texnum] - 1;
		TC.s_resident++;
	}
	else
	{
		tmp = Z_TryMallocAlign(l->disksize, PU_RENDERWORK, NULL, 0);
		if (!tmp || WPack_ReadRaw(TC.handle, l, tmp) != l->disksize)
		{
			Z_Free(tmp);
			TC.s_fail++;
			return false;
		}
		blob = tmp;
		TC.s_read++;
		TC.s_readcyc += TC_Cyc() - t0;
	}
	t1 = TC_Cyc();
	ok = WPack_DecodeMem(l->compression, blob, l->disksize, dest, l->size);
	Z_Free(tmp);
	if (ok)
		TC.s_hit++;
	else
		TC.s_fail++;
	TC.s_cyc += TC_Cyc() - t0;
	(void)t1;
	return ok;
}

// ---- the prefetch of a level --------------------------------------------------------------------------------------------

typedef struct { UINT32 pos, disk, off; INT32 tex; UINT16 lump; UINT8 sky; } tc_item_t;

static int TC_CmpItem(const void *a, const void *b)
{
	const tc_item_t *x = a, *y = b;

	return x->pos < y->pos ? -1 : x->pos > y->pos ? 1 : 0;
}

static void TC_Mark(UINT8 *mark, INT32 tex)
{
	INT32 first, last, k;

	if (tex < 0 || tex >= numtextures)
		return;
	mark[tex] = 1;
	if (P_PS2_AnimRange(tex, &first, &last)) // the pictures of the animation the texture belongs to are shown in turn
		for (k = first; k <= last && k < numtextures; k++)
			if (k >= 0)
				mark[k] = 1;
}

void PS2TexC_PrefetchLevel(void)
{
	UINT8 *mark;
	tc_item_t *items;
	INT32 i, nitems = 0, a, nu;
	size_t j, total = 0, at = 0;
	UINT32 t0 = TC_Cyc();
	UINT8 *run = NULL;
	size_t runcap = 0;

	TC.pre_n = TC.pre_cyc = 0;
	TC.pre_bytes = 0;
	if (TC.arena)
		Z_Free(TC.arena);
	TC.arena = NULL;
	if (!PS2TexC_Present() || !TC.budget || numtextures <= 0 || !sides)
		return;
	if (TC_Lump(0) < -1 || !TC.tlump)
		return; // (makes the per-texture tables)
	if (!TC.toff)
		TC.toff = Z_Calloc((size_t)numtextures * sizeof *TC.toff, PU_STATIC, &TC.toff);
	else
		memset(TC.toff, 0, (size_t)numtextures * sizeof *TC.toff);
	mark = Z_TryMallocAlign((size_t)numtextures, PU_RENDERWORK, NULL, 0);
	items = Z_TryMallocAlign((size_t)numtextures * sizeof *items, PU_RENDERWORK, NULL, 0);
	if (!mark || !items)
	{
		Z_Free(mark);
		Z_Free(items);
		return;
	}
	memset(mark, 0, (size_t)numtextures);
	for (j = 0; j < numsides; j++)
	{
		TC_Mark(mark, sides[j].toptexture);
		TC_Mark(mark, sides[j].midtexture);
		TC_Mark(mark, sides[j].bottomtexture);
	}
	TC_Mark(mark, skytexture);
	for (i = 0; i < numtextures; i++)
	{
		int li;

		if (!mark[i] || (li = TC_Lump(i)) < 0)
			continue;
		items[nitems].pos = TC.lumps[li].position;
		items[nitems].disk = TC.lumps[li].disksize;
		items[nitems].tex = i;
		items[nitems].lump = (UINT16)li;
		items[nitems].sky = (i == skytexture);
		nitems++;
	}
	// what fits the budget: the sky first when the room is short, then the textures in the order of the list (a zone's textures lie together in the pack)
	{
		INT32 kept = 0;
		size_t sum = 0;

		for (i = 0; i < nitems; i++)
			if (items[i].sky)
			{
				const tc_item_t tmp = items[i];

				memmove(&items[1], &items[0], (size_t)i * sizeof tmp);
				items[0] = tmp;
				break;
			}
		for (i = 0; i < nitems; i++)
		{
			const size_t sz = ((size_t)items[i].disk + 15u) & ~(size_t)15u;

			if (sum + sz > TC.budget)
				continue;
			sum += sz;
			items[kept++] = items[i];
		}
		nitems = kept;
	}
	if (!nitems)
	{
		Z_Free(mark);
		Z_Free(items);
		return;
	}
	// position order; textures with one stored form (identical definitions, identical pixels) share it
	qsort(items, (size_t)nitems, sizeof *items, TC_CmpItem);
	for (a = 0, nu = 0; a < nitems; a++)
	{
		const tc_item_t it = items[a];

		if (nu && items[nu - 1].pos == it.pos)
		{
			TC.toff[it.tex] = items[nu - 1].off + 1u;
			continue;
		}
		items[nu] = it;
		items[nu].off = (UINT32)at;
		TC.toff[it.tex] = (UINT32)at + 1u;
		at += ((size_t)it.disk + 15u) & ~(size_t)15u;
		nu++;
	}
	total = at;
#ifdef PS2
	{
		const size_t room = Z_ArenaFree();

		if (room < total + Z_RenderHeadroom() + (1u << 20)) // not at the edge of the arena: the stored form is a luxury, the same read happens in the frame instead
		{
			CONS_Printf("TEXC prefetch: no room for %u KiB (arena free %u KiB)\n", (unsigned)(total >> 10), (unsigned)(room >> 10));
			memset(TC.toff, 0, (size_t)numtextures * sizeof *TC.toff);
			Z_Free(mark);
			Z_Free(items);
			return;
		}
	}
#endif
	TC.arena = Z_TryMallocAlign(total, PU_LEVEL, &TC.arena, 4);
	if (!TC.arena)
	{
		memset(TC.toff, 0, (size_t)numtextures * sizeof *TC.toff);
		Z_Free(mark);
		Z_Free(items);
		return;
	}
	// one read for every run of stored forms that lie close together
	for (a = 0; a < nu;)
	{
		INT32 b = a + 1, k;
		UINT32 end = items[a].pos + items[a].disk;
		size_t span;
		lumpinfo_t where = TC.lumps[items[a].lump];

		while (b < nu && items[b].pos >= end && items[b].pos - end <= (16u << 10) && items[b].pos + items[b].disk - items[a].pos <= (256u << 10))
		{
			end = items[b].pos + items[b].disk;
			b++;
		}
		span = (size_t)end - items[a].pos;
		if (span > runcap)
		{
			Z_Free(run);
			runcap = (span + 4095u) & ~(size_t)4095u;
			run = Z_TryMallocAlign(runcap, PU_RENDERWORK, NULL, 0);
			if (!run)
				break;
		}
		where.position = items[a].pos;
		where.disksize = (UINT32)span;
		if (WPack_ReadRaw(TC.handle, &where, run) != span)
			break;
		for (k = a; k < b; k++)
			memcpy(TC.arena + items[k].off, run + (items[k].pos - items[a].pos), items[k].disk);
		a = b;
	}
	if (a < nu) // a read failed: nothing of this prefetch is trusted
	{
		memset(TC.toff, 0, (size_t)numtextures * sizeof *TC.toff);
		Z_Free(TC.arena);
		TC.arena = NULL;
	}
	else
	{
		TC.pre_n = (UINT32)nu;
		TC.pre_bytes = total;
		Z_ChangeTag(TC.arena, PU_CACHE); // a cache: the stored forms are a luxury, the zone takes the block back (TC.arena becomes NULL, the lumps are read one by one) when a geometry cache or a frame needs the room
	}
	Z_Free(run);
	Z_Free(mark);
	Z_Free(items);
	TC.pre_cyc = TC_Cyc() - t0;
	CONS_Printf("TEXC prefetch: %u stored composites, %u KiB, %u cycles\n", (unsigned)TC.pre_n, (unsigned)(TC.pre_bytes >> 10), (unsigned)TC.pre_cyc);
}

// ---- diagnostics -------------------------------------------------------------------------------------------------------------

void PS2TexC_Prof(unsigned int frames)
{
	if (!TC.state && !frames)
		return;
	if (TC.state > 0)
		I_OutputMsg("HWTEXC pack=%u textures: composites used %u (stored form in memory %u, read from the pack %u) cycles %u (reads %u) | not in the pack %u, damaged %u | level prefetch %u KiB, %u cycles\n",
			(unsigned)TC.nkeys, TC.s_hit, TC.s_resident, TC.s_read, TC.s_cyc, TC.s_readcyc, TC.s_miss, TC.s_fail, (unsigned)(TC.pre_bytes >> 10), TC.pre_cyc);
	TC.s_hit = TC.s_miss = TC.s_fail = TC.s_resident = TC.s_read = TC.s_cyc = TC.s_readcyc = 0;
}

void PS2TexC_Check(boolean (*compose)(INT32 texnum, UINT8 *dest, size_t bytes))
{
	INT32 i, checked = 0, bad = 0, notfound = 0;
	UINT8 *a, *b;
	size_t cap = 0;

	if (!PS2TexC_Present())
	{
		CONS_Printf("TEXC check: no TEXC.PAK\n");
		return;
	}
	a = Z_TryMallocAlign(TEXC_MAXBYTES, PU_RENDERWORK, NULL, 0);
	b = Z_TryMallocAlign(TEXC_MAXBYTES, PU_RENDERWORK, NULL, 0);
	if (!a || !b)
	{
		Z_Free(a);
		Z_Free(b);
		CONS_Printf("TEXC check: no room for the buffers\n");
		return;
	}
	(void)cap;
	for (i = 0; i < numtextures; i++)
	{
		const size_t bytes = textures[i] ? (size_t)textures[i]->width * (size_t)textures[i]->height : 0;

		if (TC_Lump(i) < 0)
		{
			if (PS2TexC_Key(i))
				notfound++;
			continue;
		}
		if (!PS2TexC_Fetch(i, a, bytes) || !compose(i, b, bytes))
		{
			CONS_Printf("TEXC check: texture %d %.8s: cannot fetch or compose\n", (int)i, textures[i]->name);
			bad++;
		}
		else if (memcmp(a, b, bytes))
		{
			if (bad < 10)
				CONS_Printf("TEXC MISMATCH texture %d %.8s %dx%d\n", (int)i, textures[i]->name, (int)textures[i]->width, (int)textures[i]->height);
			bad++;
		}
		checked++;
	}
	CONS_Printf("TEXC check: %d textures checked, %d differ; %d eligible textures have no stored composite\n", (int)checked, (int)bad, (int)notfound);
	Z_Free(a);
	Z_Free(b);
}

// ---- the host engine of the cooker -----------------------------------------------------------------------------------------

#ifndef _EE
// The pixels of texture `texnum` as HWR_GenerateTexture (the hardware renderer's fast composition, hw_cache.c: HWR_DrawTexturePatchInCache with 1:1 scale, no blend, no vertical flip) makes them.
static boolean TC_Compose(INT32 texnum, UINT8 *block)
{
	const texture_t *t = textures[texnum];
	const INT32 bw = t->width, bh = t->height;
	INT32 pi;

	memset(block, TEXC_HOLE, (size_t)bw * (size_t)bh);
	for (pi = 0; pi < t->patchcount; pi++)
	{
		const texpatch_t *patch = &t->patches[pi];
		patch_t *rp;
		boolean freeit = false;
		INT32 x, x1, x2, col, ncols, j;
		INT32 width, height;

		if (t->type == TEXTURETYPE_FLAT)
		{
			// HWR_GenerateTexture: the raw flat becomes a patch of the size of the texture
			UINT8 *pdata = W_CacheLumpNumPwad(patch->wad, patch->lump, PU_CACHE);

			rp = (patch_t *)Picture_Convert(PICFMT_FLAT, pdata, PICFMT_PATCH, 0, NULL, t->width, t->height, 0, 0, 0);
			freeit = true;
		}
		else
			rp = W_CachePatchNumPwad(patch->wad, patch->lump, PU_PATCH);
		if (!rp)
			return false;
		width = rp->width;
		height = rp->height;
		x1 = patch->originx;
		x2 = x1 + width;
		if (x1 > bw || x2 < 0 || patch->originy > bh || (patch->originy + height) < 0)
		{
			if (freeit)
				Patch_Free(rp);
			continue;
		}
		x = x1 < 0 ? 0 : x1;
		if (x2 > bw)
			x2 = bw;
		col = x;
		ncols = x2 - x;
		{
			const INT32 sx0 = x1 < 0 ? -x1 : 0;
			const INT32 originy = patch->originy;

			for (j = 0; j < ncols; j++)
			{
				const column_t *pc = (patch->flip & 1) ? &rp->columns[(width - 1) - (sx0 + j)] : &rp->columns[sx0 + j];
				UINT8 *dcol = block + col + j;
				unsigned k;

				for (k = 0; k < pc->num_posts; k++)
				{
					const post_t *post = &pc->posts[k];
					const UINT8 *source = pc->pixels + post->data_offset;
					INT32 position, count = (INT32)post->length, y;
					UINT8 *d;

					const boolean blend = patch->style != AST_COPY;

					if (!(patch->flip & 2))
					{
						INT32 srcoff = 0;

						position = originy + (INT32)post->topdelta;
						if (position < 0)
						{
							srcoff = -position;
							count += position;
							position = 0;
						}
						if (position + count > bh)
							count = bh - position;
						if (count <= 0)
							continue;
						d = dcol + (size_t)position * (size_t)bw;
						for (y = 0; y < count; y++, d += bw)
							*d = blend ? ASTBlendPaletteIndexes(*d, source[srcoff + y], patch->style, patch->alpha) : source[srcoff + y];
					}
					else
					{
						// HWR_DrawFlippedColumnInCache at scale 1: the post is read from its last pixel up
						INT32 yfrac = (INT32)post->length - 1;

						position = originy + (height - (INT32)post->length - (INT32)post->topdelta);
						if (position < 0)
						{
							yfrac += position;
							count += position;
							position = 0;
						}
						if (position + count >= bh)
							count = bh - position;
						if (count <= 0)
							continue;
						d = dcol + (size_t)position * (size_t)bw;
						for (y = 0; y < count; y++, d += bw, yfrac--)
							*d = blend ? ASTBlendPaletteIndexes(*d, source[yfrac], patch->style, patch->alpha) : source[yfrac];
					}
				}
			}
		}
		if (freeit)
			Patch_Free(rp);
	}
	return true;
}

void PS2TexC_AfterTextures(void)
{
	FILE *f;
	UINT8 *buf;
	INT32 i, n = 0, skipped = 0, failed = 0;
	const char *path;
	const UINT32 magic = 0x44435854u, version = 1; // "TXCD"

	if (!M_CheckParm("-texcdump") || !M_IsNextParm())
		return;
	path = M_GetNextParm();
	f = fopen(path, "wb");
	buf = malloc(TEXC_MAXBYTES);
	if (!f || !buf)
		I_Error("-texcdump: cannot open %s", path);
	fwrite(&magic, 4, 1, f);
	fwrite(&version, 4, 1, f);
	for (i = 0; i < numtextures; i++)
	{
		const UINT64 key = PS2TexC_Key(i);
		const texture_t *t = textures[i];
		UINT32 num = (UINT32)i, size;
		UINT16 w, h;

		if (!key)
		{
			const char *why;

			TC_KeyWhy(i, &why);
			if (why[0] && strcmp(why, "flat"))
				printf("TEXCDUMP skipped %.8s %dx%d type %d patches %d: %s\n", t ? t->name : "?", t ? (int)t->width : 0, t ? (int)t->height : 0, t ? (int)t->type : 0, t ? (int)t->patchcount : 0, why);
			skipped++;
			continue;
		}
		if (!TC_Compose(i, buf))
		{
			failed++;
			continue;
		}
		size = (UINT32)((size_t)t->width * (size_t)t->height);
		w = (UINT16)t->width;
		h = (UINT16)t->height;
		printf("TEXCDUMP made %d %.8s %dx%d patches %d key %016llx\n", (int)i, t->name, (int)t->width, (int)t->height, (int)t->patchcount, (unsigned long long)key);
		fwrite(&key, 8, 1, f);
		fwrite(&num, 4, 1, f);
		fwrite(&w, 2, 1, f);
		fwrite(&h, 2, 1, f);
		fwrite(&size, 4, 1, f);
		fwrite(buf, 1, size, f);
		n++;
	}
	fclose(f);
	printf("TEXCDUMP %d composites written to %s (%d textures not eligible, %d failed)\n", (int)n, path, (int)skipped, (int)failed);
	fflush(stdout);
	exit(failed ? 1 : 0);
}
#else
void PS2TexC_AfterTextures(void)
{
}
#endif // !_EE

#endif // PS2_PROFILE
