// PS2-110 (OPT6-F): test hooks of the content systems (ZIP/PNG, add-ons, Lua, UDMF, limits). Diagnostics only: every
// hook is switched on by a -ftest-* parameter and costs nothing without it. Output goes to the engine log ("FT_" lines),
// tools/ps2/ftest_check.py compares them with what the host-side reference (zipfile/PIL, the host profile) says.
//
//   -ftest-lumps W        after the start-up add-ons are loaded: "FT_LUMP wad lump fullname size crc32" for every lump of the files W..
//   -ftest-patches A,B    "FT_PATCH name w h left top crc32 posts" for W_CachePatchName(): the PNG converter result
//   -ftest-textures A,B   "FT_TEX name num w h crc32" for the composed columns of the texture
//   -ftest-level          after every level load: "FT_LEVEL map udmf nverts nsectors nlines nsides nthings" and "FT_LCRC" checksums of the
//                         level structure (vertices, sectors, lines, sides, things; canonical little-endian records, texture names go to a separate FT_LNAM checksum, uppercase 8 bytes)
//                         that tools/ps2/ftest_check.py udmf recomputes from the TEXTMAP lump with an independent Python parser
//   -ftest-quit           I_Quit() after the hooks ran ("FT_DONE")

#include "../doomdef.h"
#include "../m_argv.h"
#include "../i_system.h"
#include "../w_wad.h"
#include "../z_zone.h"
#include "../r_defs.h"
#include "../r_data.h"
#include "../r_textures.h"
#include "../r_patch.h"
#include "../r_picformats.h"
#include "../r_state.h"
#include "../doomstat.h"
#include "../p_setup.h"
#include "ps2_ftest.h"

static UINT32 crc_table[256];

static void CRCInit(void)
{
	UINT32 i, j, c;

	if (crc_table[1])
		return;
	for (i = 0; i < 256; i++)
	{
		c = i;
		for (j = 0; j < 8; j++)
			c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crc_table[i] = c;
	}
}

UINT32 PS2FTest_CRC32(UINT32 crc, const void *data, size_t size)
{
	const UINT8 *p = data;

	CRCInit();
	crc = ~crc;
	while (size--)
		crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

static const char *NextName(const char **list, char *out, size_t outsize)
{
	const char *p = *list;
	size_t n = 0;

	while (*p == ',')
		p++;
	if (!*p)
		return NULL;
	while (p[n] && p[n] != ',' && n < outsize - 1)
	{
		out[n] = p[n];
		n++;
	}
	out[n] = 0;
	p += n;
	while (*p && *p != ',')
		p++;
	*list = p;
	return out;
}

static void TestLumps(INT32 first)
{
	UINT16 w, l;

	for (w = (UINT16)first; w < numwadfiles; w++)
		for (l = 0; l < wadfiles[w]->numlumps; l++)
		{
			const lumpinfo_t *li = &wadfiles[w]->lumpinfo[l];
			UINT32 crc = 0;

			if (li->size)
			{
				UINT8 *data = Z_Malloc(li->size, PU_STATIC, NULL);

				W_ReadLumpHeaderPwad(w, l, data, 0, 0);
				crc = PS2FTest_CRC32(0, data, li->size);
				Z_Free(data);
			}
			I_OutputMsg("FT_LUMP %u %u %s %lu %08x\n", (unsigned)w, (unsigned)l, li->fullname, (unsigned long)li->size, (unsigned)crc);
		}
	I_OutputMsg("FT_LUMPS DONE %u\n", (unsigned)numwadfiles);
}

static void TestPatches(const char *list)
{
	char name[32];

	while (NextName(&list, name, sizeof name))
	{
		lumpnum_t num = W_CheckNumForName(name);
		patch_t *patch;
		UINT32 crc = 0, posts = 0;
		INT32 x;

		if (num == LUMPERROR)
		{
			I_OutputMsg("FT_PATCH %s MISSING\n", name);
			continue;
		}
		patch = W_CachePatchNum(num, PU_STATIC);
		for (x = 0; x < patch->width; x++)
		{
			const column_t *col = &patch->columns[x];
			unsigned p;

			for (p = 0; p < col->num_posts; p++)
			{
				const post_t *post = &col->posts[p];
				UINT32 hdr[2];

				hdr[0] = post->topdelta;
				hdr[1] = post->length;
				crc = PS2FTest_CRC32(crc, hdr, sizeof hdr);
				crc = PS2FTest_CRC32(crc, col->pixels + post->data_offset, post->length);
				posts++;
			}
		}
		I_OutputMsg("FT_PATCH %s %d %d %d %d %08x %u\n", name, (int)patch->width, (int)patch->height,
			(int)patch->leftoffset, (int)patch->topoffset, (unsigned)crc, (unsigned)posts);
		Z_Free(patch);
	}
}

static void TestTextures(const char *list)
{
	char name[32];

	while (NextName(&list, name, sizeof name))
	{
		INT32 num = R_CheckTextureNumForName(name);
		UINT32 crc = 0;
		INT32 x;

		if (num < 0)
		{
			I_OutputMsg("FT_TEX %s MISSING\n", name);
			continue;
		}
		for (x = 0; x < textures[num]->width; x++)
		{
			const column_t *c = R_GetColumn(num, x);
			unsigned p;

			for (p = 0; p < c->num_posts; p++)
				crc = PS2FTest_CRC32(crc, c->pixels + c->posts[p].data_offset, c->posts[p].length);
		}
		I_OutputMsg("FT_TEX %s %d %d %d %08x\n", name, (int)num, (int)textures[num]->width, (int)textures[num]->height, (unsigned)crc);
	}
}

// -ftest-level: checksum of the level structures, the records of tools/ps2/ftest_check.py (rec_*)
static UINT32 crc_i32(UINT32 crc, INT32 v)
{
	UINT8 b[4] = { (UINT8)v, (UINT8)(v >> 8), (UINT8)(v >> 16), (UINT8)(v >> 24) };

	return PS2FTest_CRC32(crc, b, 4);
}

static UINT32 crc_name(UINT32 crc, const char *name)
{
	UINT8 b[8] = { 0 };
	int i;

	for (i = 0; i < 8 && name[i]; i++)
		b[i] = (UINT8)((name[i] >= 'a' && name[i] <= 'z') ? name[i] - 32 : name[i]);
	return PS2FTest_CRC32(crc, b, 8);
}

void PS2FTest_Level(void)
{
	UINT32 cv = 0, cs = 0, cl = 0, cd = 0, ct = 0, cn = 0;
	size_t i;
	int j;

	if (!M_CheckParm("-ftest-level"))
		return;
	for (i = 0; i < numvertexes; i++)
	{
		cv = crc_i32(cv, vertexes[i].x);
		cv = crc_i32(cv, vertexes[i].y);
	}
	for (i = 0; i < numsectors; i++)
	{
		const sector_t *s = &sectors[i];

		cs = crc_i32(cs, s->floorheight);
		cs = crc_i32(cs, s->ceilingheight);
		cs = crc_i32(cs, s->lightlevel);
		cs = crc_i32(cs, s->special);
		cs = crc_i32(cs, s->tags.count ? s->tags.tags[0] : 0);
		cn = crc_name(cn, levelflats[s->floorpic].name);
		cn = crc_name(cn, levelflats[s->ceilingpic].name);
	}
	for (i = 0; i < numlines; i++)
	{
		const line_t *l = &lines[i];

		cl = crc_i32(cl, (INT32)(l->v1 - vertexes));
		cl = crc_i32(cl, (INT32)(l->v2 - vertexes));
		cl = crc_i32(cl, l->special);
		for (j = 0; j < 5; j++)
			cl = crc_i32(cl, l->args[j]);
		cl = crc_i32(cl, l->tags.count ? l->tags.tags[0] : 0);
		cl = crc_i32(cl, l->sidenum[0] == NO_SIDEDEF ? -1 : (INT32)l->sidenum[0]);
		cl = crc_i32(cl, l->sidenum[1] == NO_SIDEDEF ? -1 : (INT32)l->sidenum[1]);
	}
	for (i = 0; i < numsides; i++)
	{
		const side_t *s = &sides[i];

		cd = crc_i32(cd, s->textureoffset);
		cd = crc_i32(cd, s->rowoffset);
		cd = crc_i32(cd, SIDE_OFFSETX_TOP(s));
		cd = crc_i32(cd, SIDE_OFFSETX_MID(s));
		cd = crc_i32(cd, SIDE_OFFSETX_BOTTOM(s));
		cd = crc_i32(cd, SIDE_OFFSETY_TOP(s));
		cd = crc_i32(cd, SIDE_OFFSETY_MID(s));
		cd = crc_i32(cd, SIDE_OFFSETY_BOTTOM(s));
		cd = crc_i32(cd, SIDE_SCALEX_TOP(s));
		cd = crc_i32(cd, SIDE_SCALEX_MID(s));
		cd = crc_i32(cd, SIDE_SCALEX_BOTTOM(s));
		cd = crc_i32(cd, SIDE_SCALEY_TOP(s));
		cd = crc_i32(cd, SIDE_SCALEY_MID(s));
		cd = crc_i32(cd, SIDE_SCALEY_BOTTOM(s));
		cd = crc_i32(cd, (INT32)(s->sector - sectors));
		cn = crc_name(cn, s->toptexture ? textures[s->toptexture]->name : "-"); // texture 0 is "no texture"
		cn = crc_name(cn, s->midtexture ? textures[s->midtexture]->name : "-");
		cn = crc_name(cn, s->bottomtexture ? textures[s->bottomtexture]->name : "-");
	}
	for (i = 0; i < nummapthings; i++)
	{
		const mapthing_t *t = &mapthings[i];

		ct = crc_i32(ct, t->x);
		ct = crc_i32(ct, t->y);
		ct = crc_i32(ct, t->z);
		ct = crc_i32(ct, t->angle);
		ct = crc_i32(ct, t->type);
	}
	I_OutputMsg("FT_LEVEL %d %d %lu %lu %lu %lu %lu\n", (int)gamemap, (int)udmf, (unsigned long)numvertexes, (unsigned long)numsectors,
		(unsigned long)numlines, (unsigned long)numsides, (unsigned long)nummapthings);
	I_OutputMsg("FT_LCRC %08x %08x %08x %08x %08x\n", (unsigned)cv, (unsigned)cs, (unsigned)cl, (unsigned)cd, (unsigned)ct);
	I_OutputMsg("FT_LNAM %08x flats %lu first %.8s last %.8s\n", (unsigned)cn, (unsigned long)numlevelflats, levelflats[0].name, levelflats[numlevelflats - 1].name);
}

void PS2FTest_Startup(void)
{
	static boolean ran;

	if (ran)
		return;
	ran = true;
	if (M_CheckParm("-ftest-lumps") && M_IsNextParm())
		TestLumps(atoi(M_GetNextParm()));
	if (M_CheckParm("-ftest-patches") && M_IsNextParm())
		TestPatches(M_GetNextParm());
	if (M_CheckParm("-ftest-textures") && M_IsNextParm())
		TestTextures(M_GetNextParm());
	if (M_CheckParm("-ftest-quit"))
	{
		I_OutputMsg("FT_DONE\n");
		I_Quit();
	}
}
