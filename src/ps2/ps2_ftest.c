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
//   -ftest-exec CMDS      at the first level load: the console commands CMDS ("addfile+ZF.pk3;map+2": '+' is a space, ';' separates commands),
//                         i.e. add-ons added while a level (and its mobjs) exists
//   -ftest-mc NAME        copies <data>/NAME to the memory card (mc0:/SRB2/NAME), reads it back, lists the folder: "FT_MC ..." lines (the card driver
//                         is loaded on the way, ps2_addons.c); "-ftest-mcdev mass" does the same on mass:/SRB2 (only where a stick exists)
//   -ftest-sprites A,B    at the first level load: "FT_SPR name sprnum numframes frames-with-a-patch" of the sprite definitions (sprites[]) of the sprite names
//   -ftest-quit           I_Quit() after the hooks ran ("FT_DONE")

#ifndef PS2_NO_ADDONS
#include <libmc.h> // before the engine headers: newer ps2sdk sifrpc headers have a struct member named "client" (d_clisrv.h #defines it)
#endif
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
#include "../r_things.h"
#include "../doomstat.h"
#include "../p_setup.h"
#include "../command.h"
#include "ps2_ftest.h"
#include "ps2_boot.h"
#ifdef HAS_ADDONS
#include "ps2_addons.h"
#endif
#include <dirent.h>
#include <stdio.h>

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

// -ftest-sprites: how many frames the sprite definition of a (long) sprite name has, and how many of them have a patch
static void TestSprites(const char *list)
{
	char name[MAXSPRITENAME + 1];

	while (NextName(&list, name, sizeof name))
	{
		spritenum_t num = R_GetSpriteNumByName(name);

		if (num == NUMSPRITES || (size_t)num >= numsprites)
			I_OutputMsg("FT_SPR %s MISSING\n", name);
		else
		{
			const spritedef_t *def = &sprites[num];
			unsigned f, withpatch = 0;

			for (f = 0; f < def->numframes; f++)
				if (def->spriteframes[f].rotate != SRF_NONE)
					withpatch++;
			I_OutputMsg("FT_SPR %s %d %u %u\n", name, (int)num, (unsigned)def->numframes, withpatch);
		}
	}
}

void PS2FTest_Level(void)
{
	UINT32 cv = 0, cs = 0, cl = 0, cd = 0, ct = 0, cn = 0;
	size_t i;
	int j;

	static boolean execdone;

	if (!execdone && M_CheckParm("-ftest-exec") && M_IsNextParm())
	{
		char cmds[256], *p;

		execdone = true;
		strlcpy(cmds, M_GetNextParm(), sizeof cmds);
		for (p = cmds; *p; p++)
			if (*p == '+')
				*p = ' ';
		I_OutputMsg("FT_EXEC %s\n", cmds);
		COM_BufAddText(va("%s\n", cmds));
	}
	{
		static boolean sprdone;

		if (!sprdone && M_CheckParm("-ftest-sprites") && M_IsNextParm())
		{
			sprdone = true;
			TestSprites(M_GetNextParm());
		}
	}
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

#ifdef HAS_ADDONS
// A card that was never formatted (an empty PCSX2 card file is all 0xFF; a PS2 game does not format cards, the BIOS does) is formatted through libmc, so that the test can write to it
static void FormatCard(void)
{
	int type = 0, freeclusters = 0, format = 0, cmd = 0, ret = 0;

	mcInit(MC_TYPE_XMC);
	mcGetInfo(0, 0, &type, &freeclusters, &format);
	mcSync(0, &cmd, &ret);
	I_OutputMsg("FT_MC card type %d free %d format %d ret %d\n", type, freeclusters, format, ret);
	if (format != 1)
	{
		mcFormat(0, 0);
		mcSync(0, &cmd, &ret);
		I_OutputMsg("FT_MC formatted ret %d\n", ret);
		mcGetInfo(0, 0, &type, &freeclusters, &format);
		mcSync(0, &cmd, &ret);
		I_OutputMsg("FT_MC card type %d free %d format %d ret %d\n", type, freeclusters, format, ret);
	}
}

// -ftest-mc: the add-on storage path (memory card): write, read back, list
static void TestMC(const char *name)
{
	char src[PS2BOOT_PATHMAX + 64], dir[64], dst[PS2BOOT_PATHMAX + 64];
	const char *dev = "mc0:";
	FILE *f;
	UINT8 *data;
	long size;
	UINT32 crc, crc2 = 0;
	size_t got = 0;
	DIR *d;
	struct dirent *e;
	int n = 0;

	if (M_CheckParm("-ftest-mcdev") && M_IsNextParm())
		dev = M_GetNextParm();
	if (!strcmp(dev, "mass"))
		dev = "mass:";
	snprintf(src, sizeof src, "%s/%s", ps2boot.datadir, name);
	snprintf(dir, sizeof dir, "%s/SRB2", dev);
	snprintf(dst, sizeof dst, "%s/%s", dir, name);
	I_OutputMsg("FT_MC prepare %s %d\n", dev, (int)PS2Addons_Prepare(dir));
	if (!strcmp(dev, "mc0:") && M_CheckParm("-ftest-mcformat"))
		FormatCard();
	f = fopen(src, "rb");
	if (!f)
	{
		I_OutputMsg("FT_MC FAILED cannot read %s\n", src);
		return;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = Z_Malloc((size_t)size, PU_STATIC, NULL);
	if (fread(data, 1, (size_t)size, f) != (size_t)size)
		I_OutputMsg("FT_MC FAILED short read of %s\n", src);
	fclose(f);
	crc = PS2FTest_CRC32(0, data, (size_t)size);
	I_OutputMsg("FT_MC source %s %ld %08x\n", name, size, (unsigned)crc);
	I_mkdir(dir, 0755);
	f = fopen(dst, "wb");
	if (!f)
	{
		I_OutputMsg("FT_MC FAILED cannot create %s\n", dst);
		return;
	}
	got = fwrite(data, 1, (size_t)size, f);
	fclose(f);
	I_OutputMsg("FT_MC wrote %s %lu\n", dst, (unsigned long)got);
	memset(data, 0, (size_t)size);
	f = fopen(dst, "rb");
	if (!f)
	{
		I_OutputMsg("FT_MC FAILED cannot reopen %s\n", dst);
		return;
	}
	got = fread(data, 1, (size_t)size, f);
	fclose(f);
	crc2 = PS2FTest_CRC32(0, data, got);
	I_OutputMsg("FT_MC readback %lu %08x %s\n", (unsigned long)got, (unsigned)crc2, (got == (size_t)size && crc == crc2) ? "SAME" : "DIFFERENT");
	d = opendir(dir);
	while (d && (e = readdir(d)))
	{
		I_OutputMsg("FT_MC entry %s\n", e->d_name);
		n++;
	}
	if (d)
		closedir(d);
	I_OutputMsg("FT_MC listed %d\n", n);
	Z_Free(data);
}
#endif

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
#ifdef HAS_ADDONS
	if (M_CheckParm("-ftest-mc") && M_IsNextParm())
		TestMC(M_GetNextParm());
#endif
	if (M_CheckParm("-ftest-quit"))
	{
		I_OutputMsg("FT_DONE\n");
		I_Quit();
	}
}
