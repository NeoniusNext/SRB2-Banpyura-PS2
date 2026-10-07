// PS2 GS hardware renderer driver: palette rendering (included by ps2_hwd.c in front of ps2_hw_tex.inc). PS2-HW-71 (OPT10, HF).
//
// The OpenGL driver of SRB2 renders by default with "palette rendering" (gr_paletterendering On): every texel is looked up in the master palette
// and lit through the sector's colormap table, colour = lighttable[index][row], row = clamp(floor(R_DoomColormap(light, depth)), 0, 31), the
// same colours the software renderer produces (the final picture is then reduced to the screen palette, which also applies the palette flashes
// of damage, pick-ups and water). The GLSL equation of the non palette path (mix(colour, fade colour, darkness)) is a different picture: no hue
// shifts of the dark colormap rows, no tint of the colormap.
//
// The GS has no 3D lookup, but a PSMT8 texture is a palette lookup by construction: the CLUT of a polygon piece is the row of the light table.
//   * CreateLightTable / UpdateLightTable keep the engine's table (32 rows x 256 colours) as 32 x 256 palette indices (8 KB);
//   * a lit polygon with a light table id and a palette texture is cut at the depths where the row changes (the staircase of ps2_hw_light.inc, rows
//     instead of fog classes) and every piece is drawn with the CLUT of its row (CK_LIGHT, 256 entries built from the table and the screen palette);
//   * the screen palette (SetScreenPalette: the flash palette, 16 bit crushed when gr_palettedepth is 16) is applied when a CLUT is built:
//     out_pal() replaces the texture palette in every CLUT of palette textures, so flashes and the 565 look need no post process.
// Anything the CLUT cannot do (direct colour textures, flat colours, fog blocks) keeps the GLSL equations of the non palette path.

#define LT_MAX 1024 // light table ids (the engine makes one per colormap of the map plus the default one)

static u8 *lt_idx[LT_MAX]; // id -> 32 rows x 256 palette indices; NULL = unused
static u8 lt_gen[LT_MAX]; // bumped by every update: part of the CLUT key
static u32 spal[256]; // screen palette, 0x00BBGGRR
static int spal_set;
static int pal_last_mode = -1;

static int pal_lookup(u32 rgb); // ps2_hw_tex.inc

static inline int palette_mode(void)
{
	return HWR_ShouldUsePaletteRendering() != 0;
}

// the colours palette textures show: the screen palette in palette rendering mode, else the texture palette
static inline const u32 *out_pal(void)
{
	return (spal_set && palette_mode()) ? spal : H.pal;
}

static inline int lt_ok(u32 id)
{
	return id && id < LT_MAX && lt_idx[id];
}

// colormap row of a darkness class (ps2_hw_light.inc): floor(colormap) clamped to 0..31; classes 0 and 1 are row 0 (palette rendering)
static inline int pal_row_of(int cls)
{
	if (ps2hwd_dbg_flags & 16384)
		return 0; // diagnostics: always the brightest row
	return cls >= 32 ? 31 : (cls <= 1 ? 0 : cls - 1);
}

// CLUT parameter: light table id (10 bits), its generation (8 bits), row (5 bits)
#define pal_param(row) ((u32)P.pal_tbl | ((u32)lt_gen[P.pal_tbl] << 10) | ((u32)(row) << 18))

static u32 lt_store(u32 id, const RGBA_t *t)
{
	u32 i;
	u8 *ix;

	if (!H.pal_set)
		return 0; // the colours of the table cannot be turned into indices before the palette is known: the engine asks again
	if (!id)
	{
		for (id = 1; id < LT_MAX && lt_idx[id]; id++)
			;
		if (id >= LT_MAX)
		{
			hw_limit(HW_LIGHT_TABLE, "more than 1023 colormap light tables: the polygon is lit by the GLSL equations");
			return 0;
		}
	}
	ix = lt_idx[id] ? lt_idx[id] : (u8 *)malloc(256 * 32);
	if (!ix)
	{
		hw_limit(HW_LIGHT_TABLE, "no memory for a colormap light table: the polygon is lit by the GLSL equations");
		return 0;
	}
	{
		u32 miss = 0;

		for (i = 0; i < 256 * 32; i++)
		{
			int k = pal_lookup((u32)t[i].s.red | ((u32)t[i].s.green << 8) | ((u32)t[i].s.blue << 16));

			miss += k < 0;
			ix[i] = (u8)(k < 0 ? 0 : k);
		}
		I_OutputMsg("HWLT table %u stored: %u of 8192 colours not in the palette; row 0 [0,1,2]=%02x%02x%02x %02x%02x%02x row 31 [128]=%02x%02x%02x\n", (unsigned)id, (unsigned)miss,
			t[0].s.red, t[0].s.green, t[0].s.blue, t[1].s.red, t[1].s.green, t[1].s.blue, t[31 * 256 + 128].s.red, t[31 * 256 + 128].s.green, t[31 * 256 + 128].s.blue);
	}
	lt_idx[id] = ix;
	lt_gen[id]++;
	return id;
}

static void lt_clear(void)
{
	u32 i;

	for (i = 1; i < LT_MAX; i++)
	{
		free(lt_idx[i]);
		lt_idx[i] = NULL;
		lt_gen[i] = 0;
	}
}

// the CLUT entries of row `row` of table `tbl` (alpha 0x80; the keyed variant has the hole at index 255)
static void pal_light_entries(u32 param, u32 keyed, u32 *e)
{
	const u8 *ix = lt_idx[param & 1023];
	const u32 *op = out_pal();
	const u32 row = (param >> 18) & 31;
	int i;

	for (i = 0; i < 256; i++)
		e[i] = (op[ix ? ix[row * 256 + (u32)i] : (u8)i] & 0x00FFFFFFu) | (0x80u << 24);
	if (keyed)
		e[255] &= 0x00FFFFFFu;
}
