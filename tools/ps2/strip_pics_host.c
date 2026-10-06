/* Host tool for the PS2 profile's PNG replacement (PS2-20): the real src/r_picformats.c with engine stubs.
 *
 *   oracle  (cl /DHAVE_PNG, libpng+zlib, NOT PS2_PROFILE): the unmodified Picture_PNGConvert/PNG_Read path.
 *       strip_png2pic PLAYPAL OUTDIR in1.png in2.png ...
 *       Converts every PNG in order with the engine's own persistent nearest-colour memo, exactly like the game
 *       does, once as PICFMT_PATCH and once as PICFMT_FLAT. Writes OUTDIR/NNN.matrix and OUTDIR/NNN.flat.
 *   check   (cl /DPS2_PROFILE, no libpng/zlib): the cooked-picture path the PS2 build runs.
 *       strip_cooked_check PLAYPAL OUTDIR cooked1.lmp cooked2.lmp ...
 *       Runs Picture_PNGDimensions/Picture_PNGConvert (macros for the cooked functions) over lumps taken out of a pack
 *       and writes the same two files.
 *
 * NNN.matrix: INT32 width, height, leftoffset, topoffset, then width*height UINT16 in row order: palette index, or
 * 0x100 for a transparent pixel (read back with Picture_GetPatchPixel from the converted patch_t).
 * NNN.flat: width*height bytes of the PICFMT_FLAT conversion (transparent = TRANSPARENTPIXEL).
 * tools/ps2/strip_pics_test.py compares the two sets byte for byte.
 * Stub source: NearestPaletteColor (r_data.c), InitColorLUT/GetColorLUT (v_video.c) and Patch_Create* (r_patch.c) are
 * included verbatim (sliced by the test script into .inc files).
 */
#include "doomdef.h"
#include "doomstat.h"
#include "doomtype.h"
#include "z_zone.h"
#include "r_defs.h"
#include "r_patch.h"
#include "r_picformats.h"
#include "r_skins.h"
#include "r_things.h"
#include "r_textures.h"
#include "v_video.h"
#include "w_wad.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---- engine services r_picformats.c links to ------------------------------------------------------------------ */
RGBA_t *pMasterPalette = NULL;
RGBA_t *pLocalPalette = NULL;
spriteinfo_t spriteinfo[NUMSPRITES];
char spr2names[NUMPLAYERSPRITES][MAXSPRITENAME + 1];
INT32 numskins = 0;
skin_t **skins = NULL;
INT32 numtextures = 0;
texture_t **textures = NULL;
wadfile_t **wadfiles = NULL;

void *Z_MallocAlign(size_t size, INT32 tag, void *user, INT32 alignbits)
{
	void *p = malloc(size ? size : 1);
	(void)tag; (void)alignbits;
	if (!p)
		I_Error("host tool: out of memory");
	if (user)
		*(void **)user = p;
	return p;
}

void *Z_CallocAlign(size_t size, INT32 tag, void *user, INT32 alignbits)
{
	void *p = Z_MallocAlign(size, tag, user, alignbits);
	memset(p, 0, size);
	return p;
}

void *Z_ReallocAlign(void *ptr, size_t size, INT32 tag, void *user, INT32 alignbits)
{
	void *p;
	(void)tag; (void)alignbits;
	p = realloc(ptr, size ? size : 1);
	if (!p)
		I_Error("host tool: out of memory");
	if (user)
		*(void **)user = p;
	return p;
}

void Z_Free(void *p) { free(p); }

void I_Error(const char *error, ...)
{
	va_list ap;
	va_start(ap, error);
	vfprintf(stderr, error, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(2);
}

void CONS_Alert(alerttype_t level, const char *fmt, ...)
{
	va_list ap;
	(void)level;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

void CONS_Debug(INT32 flags, const char *fmt, ...)
{
	(void)flags; (void)fmt;
}

void *M_Memcpy(void *dest, const void *src, size_t n) { return memcpy(dest, src, n); }
char *M_GetToken(const char *s) { (void)s; return NULL; }
size_t W_LumpLengthPwad(UINT16 w, UINT16 l) { (void)w; (void)l; return 0; }
void *W_CacheLumpNumPwad(UINT16 w, UINT16 l, INT32 tag) { (void)w; (void)l; (void)tag; return NULL; }
INT32 R_SkinAvailable(const char *name) { (void)name; return -1; }
spritenum_t R_GetSpriteNumByName(const char *name) { (void)name; return SPR_NULL; }
void R_CheckTextureCache(INT32 tex) { (void)tex; }
column_t *R_GetColumn(fixed_t tex, INT32 col) { (void)tex; (void)col; return NULL; }

/* ---- verbatim engine code ------------------------------------------------------------------------------------- */
#include "strip_nearest.inc"
#include "strip_clut.inc"
#include "strip_patch.inc"

/* ---- tool ----------------------------------------------------------------------------------------------------- */
static UINT8 *slurp(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	UINT8 *buf;
	long n;

	if (!f)
		I_Error("cannot open %s", path);
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)n + 1);
	if (fread(buf, 1, (size_t)n, f) != (size_t)n)
		I_Error("cannot read %s", path);
	fclose(f);
	*size = (size_t)n;
	return buf;
}

static void dump(const char *dir, int n, patch_t *patch, const UINT8 *flat, INT32 w, INT32 h, INT16 left, INT16 top)
{
	char path[1024];
	FILE *f;
	INT32 x, y;
	INT32 hdr[4];

	snprintf(path, sizeof path, "%s/%03d.matrix", dir, n);
	f = fopen(path, "wb");
	if (!f)
		I_Error("cannot write %s", path);
	hdr[0] = w; hdr[1] = h; hdr[2] = left; hdr[3] = top;
	fwrite(hdr, sizeof hdr, 1, f);
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
		{
			UINT8 *px = Picture_GetPatchPixel(patch, PICFMT_PATCH, x, y, 0);
			UINT16 v = px ? *px : 0x100;
			fwrite(&v, sizeof v, 1, f);
		}
	fclose(f);

	snprintf(path, sizeof path, "%s/%03d.flat", dir, n);
	f = fopen(path, "wb");
	if (!f)
		I_Error("cannot write %s", path);
	fwrite(flat, 1, (size_t)(w * h), f);
	fclose(f);
}

int main(int argc, char **argv)
{
	size_t palsize;
	UINT8 *pal;
	int i;

	if (argc < 4)
	{
		fprintf(stderr, "usage: %s PLAYPAL OUTDIR input...\n", argv[0]);
		return 1;
	}
	pal = slurp(argv[1], &palsize);
	if (palsize < 768)
		I_Error("PLAYPAL too small");
	pMasterPalette = calloc(256, sizeof *pMasterPalette);
	pLocalPalette = calloc(256, sizeof *pLocalPalette);
	for (i = 0; i < 256; i++)
	{
		pMasterPalette[i].s.red = pLocalPalette[i].s.red = pal[i * 3];
		pMasterPalette[i].s.green = pLocalPalette[i].s.green = pal[i * 3 + 1];
		pMasterPalette[i].s.blue = pLocalPalette[i].s.blue = pal[i * 3 + 2];
		pMasterPalette[i].s.alpha = pLocalPalette[i].s.alpha = 0xFF;
	}

	for (i = 3; i < argc; i++)
	{
		size_t size;
		UINT8 *data = slurp(argv[i], &size);
		INT32 w = 0, h = 0, w2 = 0, h2 = 0;
		INT16 left = 0, top = 0;
		size_t outsize = 0;
		patch_t *patch;
		UINT8 *flat;

		if (!Picture_IsLumpPNG(data, size))
			I_Error("%s is not recognised as a picture lump by Picture_IsLumpPNG", argv[i]);
		if (!Picture_PNGDimensions(data, &w2, &h2, &top, &left, size))
			I_Error("%s: Picture_PNGDimensions failed", argv[i]);

		patch = Picture_PNGConvert(data, PICFMT_PATCH, &w, &h, &top, &left, size, &outsize, 0);
		if (w != w2 || h != h2)
			I_Error("%s: dimensions %dx%d differ from Picture_PNGDimensions %dx%d", argv[i], w, h, w2, h2);
		flat = Picture_PNGConvert(data, PICFMT_FLAT, NULL, NULL, NULL, NULL, size, NULL, 0);
		dump(argv[2], i - 3, patch, flat, w, h, left, top);
		printf("%03d %s %dx%d offs %d,%d\n", i - 3, argv[i], w, h, left, top);
		free(data);
	}
	return 0;
}
