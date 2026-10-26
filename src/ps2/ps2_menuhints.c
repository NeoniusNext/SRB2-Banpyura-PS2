// PS2 menu button hints: see ps2_menuhints.h. PS2-338, PS2-339 (OPT11 NETUI).
#include "../doomdef.h"
#include "../doomstat.h"
#include "../i_system.h"
#include "../command.h"
#include "../console.h"
#include "../f_finale.h"
#include "../g_input.h" // gamecontrol: the quick button of the chat (OPT14-CHAT)
#include "../i_video.h"
#include "../m_argv.h"
#include "../m_menu.h"
#include "../r_main.h" // cv_menubgcolor
#include "../screen.h"
#include "../v_video.h"
#include "../w_wad.h"
#include "../z_zone.h"

#include "ps2_menuhints.h"
#include "ps2_osk.h"
#include "ps2_uiicons.h"

consvar_t cv_menuhints = CVAR_INIT ("menuhints", "Off", CV_SAVE, CV_OnOff, NULL); // Off by default: the final build of the hints was handed in without the last runs (opt11-NETUI.md, "handed in without checking")

void PS2MenuHints_RegisterCvars(void)
{
	CV_RegisterVar(&cv_menuhints);
}

// ---- the sets: one row per kind of item ----------------------------------------------------------------------------------------------------------------
// Texts are ps2_uiicons.h tokens and the labels of the menu (English: the console build has no translation tables). Three spaces separate the two halves of a group (the group
// is split there when it has to stand in two lines). What each button does is what M_Responder / the item handlers do with the key that the pad's button becomes:
// Cross = Enter, Circle = Escape (back), Square = Backspace (clears a control, takes the default of a value, deletes a letter of a text), Triangle = the on-screen keyboard on a
// text field, D-pad = the arrows.
typedef struct
{
	const char *left;
	const char *right;
} hintset_t;

static const hintset_t hintsets[PS2MH_NUMKINDS] =
{
	[PS2MH_SELECT]     = {PS2I_DPAD_UD " Select   " PS2I_CROSS " OK", PS2I_CIRCLE " Back"},
	[PS2MH_MAIN]       = {PS2I_DPAD_UD " Select   " PS2I_CROSS " OK", PS2I_CIRCLE " Back"},
	[PS2MH_ARROWS]     = {PS2I_DPAD_UD " Select   " PS2I_DPAD_LR " Change", PS2I_SQUARE " Default   " PS2I_CIRCLE " Back"},
	[PS2MH_TEXT]       = {PS2I_DPAD_UD " Select   " PS2I_TRIANGLE " Keyboard", PS2I_SQUARE " Delete   " PS2I_CIRCLE " Back"},
	[PS2MH_ADDRESS]    = {PS2I_CROSS " Connect   " PS2I_TRIANGLE " Keyboard", PS2I_SQUARE " Delete   " PS2I_CIRCLE " Back"},
	[PS2MH_PLAYERNAME] = {PS2I_DPAD_UD " Select   " PS2I_TRIANGLE " Keyboard", PS2I_SQUARE " Delete   " PS2I_CIRCLE " Back"},
	[PS2MH_CONTROL]    = {PS2I_DPAD_UD " Select   " PS2I_CROSS " Assign", PS2I_SQUARE " Clear   " PS2I_CIRCLE " Back"},
	[PS2MH_CAPTURE]    = {NULL, NULL}, // the message box asks for the button itself ("Press a button for <control>")
	[PS2MH_YESNO]      = {PS2I_CROSS " Yes", PS2I_CIRCLE " No"},
	[PS2MH_MESSAGE]    = {PS2I_CROSS " OK", PS2I_CIRCLE " Close"},
	[PS2MH_SERVER]     = {PS2I_DPAD_UD " Select   " PS2I_CROSS " Join", PS2I_CIRCLE " Back"},
	[PS2MH_PLATTER]    = {PS2I_DPAD_UD PS2I_DPAD_LR " Select   " PS2I_CROSS " OK", PS2I_CIRCLE " Back"},
	[PS2MH_LOADSAVE]   = {PS2I_DPAD_LR " Select   " PS2I_CROSS " OK", PS2I_CIRCLE " Back"},
	[PS2MH_CHOOSEPLAYER] = {PS2I_DPAD_UD " Character   " PS2I_CROSS " OK", PS2I_CIRCLE " Back"},
	[PS2MH_SOUNDTEST]  = {PS2I_DPAD_UD " Select   " PS2I_CROSS " Play", PS2I_SQUARE " Stop   " PS2I_CIRCLE " Back"},
	[PS2MH_SCROLL]     = {PS2I_DPAD_UD " Scroll", PS2I_CIRCLE " Back"},
	[PS2MH_ADDONS]     = {PS2I_DPAD_UD " Select   " PS2I_CROSS " Open", PS2I_CIRCLE " Back"},
	[PS2MH_VIDEOMODE]  = {PS2I_DPAD_UD PS2I_DPAD_LR " Select   " PS2I_CROSS " Set", PS2I_CIRCLE " Back"},
	[PS2MH_CHANGE]     = {PS2I_DPAD_UD " Select   " PS2I_DPAD_LR " Change   " PS2I_CROSS " OK", PS2I_CIRCLE " Back"}, // (the order of every set: move, the main button; then the side ones, Circle last)
	[PS2MH_VMCONFIRM]  = {PS2I_CROSS " Keep this mode", PS2I_CIRCLE " Return"},
	[PS2MH_CHAT]       = {PS2I_DPAD_UD " Select   " PS2I_CROSS " Write", PS2I_CIRCLE " Back"}, // (the quick button is put in by ChatSet)
	[PS2MH_TEAMCHAT]   = {PS2I_DPAD_UD " Select   " PS2I_CROSS " Write", PS2I_CIRCLE " Back"},
	[PS2MH_NONE]       = {NULL, NULL},
};

// OPT14-CHAT: the pause menu's chat items also tell the quick button of the chat: the pad button that Setup Controls has on Talk / Talk (Team only) (Select by default,
// g_input.c), so a changed binding is told as changed. No pad button bound: the Cross / Circle hints alone.
static const hintset_t *ChatSet(boolean team)
{
	static hintset_t set;
	static char right[48];
	const INT32 gc = team ? GC_TEAMKEY : GC_TALKKEY;
	const char *tok = PS2UI_KeyToken(gamecontrol[gc][1]);

	if (!tok[0])
		tok = PS2UI_KeyToken(gamecontrol[gc][0]);
	set = hintsets[team ? PS2MH_TEAMCHAT : PS2MH_CHAT];
	if (tok[0])
	{
		snprintf(right, sizeof right, "%s %s   " PS2I_CIRCLE " Back", tok, team ? "Team chat" : "Quick chat");
		set.right = right;
	}
	return &set;
}

// ---- the map of what the menu has drawn ------------------------------------------------------------------------------------------------------------------
#define ROW0_Y 182      // the top of the 13 px icons of the lowest row: the plate ends 4 px above the bottom edge of the 320x200 picture
#define ROW_PITCH 17    // plate height 15 + 2 px between plates
#define NUMROWS 3       // the lowest row and two above it
#define PLATE_H 15
#define PLATE_PAD 3     // the plate extends this far to the sides of the text
#define GAP 2           // free space all round a plate, in 320x200 pixels
#define EDGE_X 6        // television overscan: the icons start 6 px from the sides
#define FONT_Y 3        // the thin font sits this far below the top of the icon row

boolean ps2mh_recording;
static UINT8 occ[BASEVIDHEIGHT][BASEVIDWIDTH / 8]; // 1 bit per pixel of the 320x200 picture
static UINT8 occ_pre[BASEVIDHEIGHT][BASEVIDWIDTH / 8]; // the map as it was when the groups were placed (the check draws the menu again and notes it again)
static boolean begun;                               // the HUD of a level frame started the note already
static boolean checking;                            // -menuhintscheck: M_Drawer is run again to find what the menu covers
static boolean opt_check, opt_debug, opt_dump;
static UINT64 shown_bits, placing_bits; // the buttons (bit = PS2UI_* icon) of the corner groups that were placed in the last frame / are being placed (one hint, one place)
static boolean crawling, check_armed; // the crawler moved the cursor: the next frame is checked once
static UINT32 check_sig, occ_hash_last;
static INT32 stable_wait;               // -menuhintscheck on its own: checked again when the menu, the item or the plates change

static void OccSet(INT32 x0, INT32 y0, INT32 x1, INT32 y1) // [x0, x1) x [y0, y1)
{
	INT32 x, y;

	x0 = max(x0, 0);
	y0 = max(y0, 0);
	x1 = min(x1, BASEVIDWIDTH);
	y1 = min(y1, BASEVIDHEIGHT);
	for (y = y0; y < y1; y++)
		for (x = x0; x < x1; x++)
			occ[y][x >> 3] |= (UINT8)(1u << (x & 7));
}

static INT32 occ_hitx, occ_hity; // the first pixel found by the last OccAny

static boolean OccAny(INT32 x0, INT32 y0, INT32 x1, INT32 y1)
{
	INT32 x, y;

	x0 = max(x0, 0);
	y0 = max(y0, 0);
	x1 = min(x1, BASEVIDWIDTH);
	y1 = min(y1, BASEVIDHEIGHT);
	for (y = y0; y < y1; y++)
		for (x = x0; x < x1; x++)
			if (occ[y][x >> 3] & (1u << (x & 7)))
			{
				occ_hitx = x;
				occ_hity = y;
				return true;
			}
	return false;
}

static INT32 FloorDiv(INT32 a, INT32 b)
{
	return a >= 0 ? a / b : -((-a + b - 1) / b);
}

// A draw of the frame as a rectangle of the screen in real pixels: the way V_DrawFill / V_DrawStretchyFixedPatch / V_DrawCroppedPatch place it (v_video.c: the
// scaling by dup, the centring of the 320x200 picture in a bigger screen, the V_SNAPTO flags, V_NOSCALESTART), then converted to the 320x200 picture that the groups of
// hints are placed in (the centred box: what is outside it, as the HUD that snaps to a corner of a wider screen, cannot meet a group).
// x, y, w, h: fixed point, in the units of the call (base units, or real pixels with V_NOSCALESTART for the position); posdup / sizedup scale position and size.
static INT32 last_note[4]; // the last rectangle noted, in the 320x200 picture (the geometry self test)

// ---- the check of the icons in the lines of text (PS2-340): every icon and every patch (letter, cursor, picture) of a frame as rectangles of the real screen, in two bitmaps ----
#define IC_W 640 // PS2VM_MAXW x PS2VM_MAXH of ps2_vmodes.h
#define IC_H 512
static UINT8 ic_icon[IC_H][IC_W / 8], ic_glyph[IC_H][IC_W / 8];
static INT32 ic_icons, ic_on_glyph, ic_icon_icon, ic_glyph_on_icon; // the icons of the frame; icons that landed on a patch, on another icon (no pixel of air); patches that landed on an icon
static boolean icondrawing; // v_video.c: the patch that is being drawn is an icon

static void BmSet(UINT8 (*bm)[IC_W / 8], const INT32 *r)
{
	INT32 x, y;

	for (y = max(r[1], 0); y < min(r[3], IC_H); y++)
		for (x = max(r[0], 0); x < min(r[2], IC_W); x++)
			bm[y][x >> 3] |= (UINT8)(1u << (x & 7));
}

static void BmClear(UINT8 (*bm)[IC_W / 8], const INT32 *r)
{
	INT32 x, y;

	for (y = max(r[1], 0); y < min(r[3], IC_H); y++)
		for (x = max(r[0], 0); x < min(r[2], IC_W); x++)
			bm[y][x >> 3] &= (UINT8)~(1u << (x & 7));
}

static boolean BmAny(UINT8 (*bm)[IC_W / 8], const INT32 *r, INT32 grow)
{
	INT32 x, y;

	for (y = max(r[1] - grow, 0); y < min(r[3] + grow, IC_H); y++)
		for (x = max(r[0] - grow, 0); x < min(r[2] + grow, IC_W); x++)
			if (bm[y][x >> 3] & (1u << (x & 7)))
				return true;
	return false;
}

static void IcReset(void)
{
	memset(ic_icon, 0, sizeof ic_icon);
	memset(ic_glyph, 0, sizeof ic_glyph);
	ic_icons = ic_on_glyph = ic_icon_icon = ic_glyph_on_icon = 0;
}

// the left and right edge of what the frame drew as text, icons and pictures (columns of the 320x200 picture; the overscan of a television hides the outer 8..12 px)
static void IcExtent(INT32 *minx, INT32 *maxx)
{
	const INT32 dup = vid.dup > 0 ? vid.dup : 1, ox = (vid.width - BASEVIDWIDTH * dup) / 2;
	INT32 x, y, lo = IC_W, hi = -1;

	for (y = 0; y < min((INT32)vid.height, IC_H); y++)
		for (x = 0; x < min((INT32)vid.width, IC_W); x++)
			if ((ic_glyph[y][x >> 3] | ic_icon[y][x >> 3]) & (1u << (x & 7)))
			{
				lo = min(lo, x);
				hi = max(hi, x);
			}
	*minx = hi < 0 ? 0 : FloorDiv(lo - ox, dup);
	*maxx = hi < 0 ? 0 : FloorDiv(hi - ox, dup);
}

void PS2MenuHints_IconDraw(boolean on)
{
	icondrawing = on;
}

// the rectangle of a draw: box = [x0, y0, x1, y1) in the 320x200 picture (rounded outwards, one real pixel more: the column loops round up), real = [x0, y0, x1, y1) in
// the pixels of the screen (exactly what the draw covers). 0 = ok, 1 = nothing to draw, 2 = a background over the whole 320x200 box
static INT32 GeomBox(fixed_t x, fixed_t y, fixed_t w, fixed_t h, INT32 flags, INT32 posdup, INT32 sizedup, boolean center, INT32 *box, INT32 *real)
{
	const INT32 dup = vid.dup > 0 ? vid.dup : 1;
	const INT32 ox = (vid.width - BASEVIDWIDTH * dup) / 2, oy = (vid.height - BASEVIDHEIGHT * dup) / 2; // the box in real pixels
	INT32 rx, ry, rw, rh;

	if (flags & V_NOSCALESTART)
	{
		rx = x >> FRACBITS;
		ry = y >> FRACBITS;
	}
	else
	{
		rx = (INT32)(((INT64)x * posdup) >> FRACBITS);
		ry = (INT32)(((INT64)y * posdup) >> FRACBITS);
		if (center)
		{
			if (vid.width != BASEVIDWIDTH * dup)
			{
				if (flags & V_SNAPTORIGHT)
					rx += 2 * ox;
				else if (!(flags & V_SNAPTOLEFT))
					rx += ox;
			}
			if (vid.height != BASEVIDHEIGHT * dup)
			{
				if (flags & V_SNAPTOBOTTOM)
					ry += 2 * oy;
				else if (!(flags & V_SNAPTOTOP))
					ry += oy;
			}
		}
	}
	rw = (INT32)((((INT64)w * sizedup) + FRACUNIT - 1) >> FRACBITS);
	rh = (INT32)((((INT64)h * sizedup) + FRACUNIT - 1) >> FRACBITS);
	if (rw <= 0 || rh <= 0)
		return 1;
	real[0] = rx;
	real[1] = ry;
	real[2] = rx + rw;
	real[3] = ry + rh;
	rw++; // + 1 real pixel: the column / row loops of the patch drawing round up (found by ps2_mhgeom)
	rh++;
	if (rw >= (BASEVIDWIDTH - 4) * dup && rh >= (BASEVIDHEIGHT - 4) * dup)
		return 2;
	box[0] = FloorDiv(rx - ox, dup);
	box[1] = FloorDiv(ry - oy, dup);
	box[2] = -FloorDiv(-(rx + rw - ox), dup); // rounded up
	box[3] = -FloorDiv(-(ry + rh - oy), dup);
	return 0;
}

static INT32 dup_of_frame(void)
{
	return vid.dup > 0 ? vid.dup : 1;
}

static boolean NoteGeom(fixed_t x, fixed_t y, fixed_t w, fixed_t h, INT32 flags, INT32 posdup, INT32 sizedup, boolean center, boolean fill)
{
	INT32 box[4], real[4];
	const INT32 r = GeomBox(x, y, w, h, flags, posdup, sizedup, center, box, real);

	if (r == 1)
		return false;
	if (r == 2)
		return checking; // a background over the whole screen: the hints stand on it by design; the check leaves it out of the picture of the menu (true: do not draw it)
	if (checking)
		return false; // the check draws the menu again: nothing is recorded
	last_note[0] = box[0];
	last_note[1] = box[1];
	last_note[2] = box[2];
	last_note[3] = box[3];
	OccSet(box[0], box[1], box[2], box[3]);
	if (icondrawing)
	{
		ic_icons++;
		if (BmAny(ic_glyph, real, 0))
			ic_on_glyph++;
		if (BmAny(ic_icon, real, 1))
			ic_icon_icon++;
		BmSet(ic_icon, real);
	}
	else if (!fill)
	{
		// the letters, the cursor, the small pictures: not the backgrounds and the big pictures (an icon is never put on one by design: the pixel check looks at those)
		if (real[2] - real[0] <= 24 * dup_of_frame() && real[3] - real[1] <= 24 * dup_of_frame())
		{
			if (BmAny(ic_icon, real, 0))
				ic_glyph_on_icon++;
			BmSet(ic_glyph, real);
		}
	}
	else if (!(flags & V_ALPHAMASK))
	{
		// an opaque fill (the panel of the on-screen keyboard, a plate) hides what was drawn under it: that is no longer in the way of an icon drawn on top
		BmClear(ic_glyph, real);
		BmClear(ic_icon, real);
	}
	return false;
}

boolean PS2MenuHints_NoteRect(INT32 x, INT32 y, INT32 w, INT32 h, INT32 flags)
{
	const INT32 dup = vid.dup > 0 ? vid.dup : 1;

	return NoteGeom((fixed_t)x << FRACBITS, (fixed_t)y << FRACBITS, (fixed_t)w << FRACBITS, (fixed_t)h << FRACBITS, flags, dup, (flags & V_NOSCALESTART) ? 1 : dup, true, true);
}

boolean PS2MenuHints_NotePatch(fixed_t x, fixed_t y, fixed_t pscale, fixed_t vscale, INT32 scrn, const patch_t *patch)
{
	INT32 dup = vid.dup > 0 ? vid.dup : 1;
	fixed_t pw = FixedMul((fixed_t)patch->width << FRACBITS, pscale), ph = FixedMul((fixed_t)patch->height << FRACBITS, vscale);
	fixed_t ox = FixedMul((fixed_t)patch->leftoffset << FRACBITS, pscale), oy = FixedMul((fixed_t)patch->topoffset << FRACBITS, vscale);

	if (scrn & V_FLIP)
		ox = FixedMul((fixed_t)(patch->width - patch->leftoffset) << FRACBITS, pscale) + 1;
	switch (scrn & V_SCALEPATCHMASK)
	{
		case V_NOSCALEPATCH: dup = 1; break;
		case V_SMALLSCALEPATCH: dup = vid.smalldup; break;
		case V_MEDSCALEPATCH: dup = vid.meddup; break;
		default: break;
	}
	return NoteGeom(x - ox, y - oy, pw, ph, scrn, dup, dup, !(scrn & V_SCALEPATCHMASK), false);
}

boolean PS2MenuHints_NoteCropped(fixed_t x, fixed_t y, fixed_t pscale, fixed_t vscale, INT32 scrn, const patch_t *patch, fixed_t w, fixed_t h)
{
	INT32 dup = vid.dup > 0 ? vid.dup : 1;
	fixed_t ox = FixedMul((fixed_t)patch->leftoffset << FRACBITS, pscale), oy = FixedMul((fixed_t)patch->topoffset << FRACBITS, vscale);

	switch (scrn & V_SCALEPATCHMASK)
	{
		case V_NOSCALEPATCH: dup = 1; break;
		case V_SMALLSCALEPATCH: dup = vid.smalldup; break;
		case V_MEDSCALEPATCH: dup = vid.meddup; break;
		default: break;
	}
	return NoteGeom(x - ox, y - oy, FixedMul(w, pscale), FixedMul(h, vscale), scrn, dup, dup, !(scrn & V_SCALEPATCHMASK), false);
}

// ---- one hint, one place (PS2-341): the log of the hints of a frame, the buttons that the corner hints show ---------------------------------------------------
// A text with button icons ("<Cross> Change  <Square> Clear") is split into (button, label) entries. The frame must not have the same button twice, nor the same label twice:
// the corner hints and a line of the menu itself that says the same would be a duplicate.
typedef struct { UINT8 token; char label[20]; char source[12]; } hintent_t;
static hintent_t hlog[48];
static INT32 hlog_n;
static boolean hlog_open;
static const menu_t *shown_menu;

static void HintLogReset(void)
{
	hlog_n = 0;
	hlog_open = true;
}

void PS2MenuHints_Log(const char *source, const char *text)
{
	const char *p = text;

	if (!hlog_open || checking || !opt_check)
		return;
	while (*p)
	{
		if ((UINT8)*p < 0x16 && PS2UI_TokenIcon((UINT8)*p) >= 0)
		{
			hintent_t *e;
			const UINT8 token = (UINT8)*p;
			size_t n = 0;

			p++;
			while (*p == ' ')
				p++;
			if (hlog_n >= (INT32)(sizeof hlog / sizeof hlog[0]))
				return;
			e = &hlog[hlog_n++];
			e->token = token;
			while (*p && !((UINT8)*p < 0x16 && PS2UI_TokenIcon((UINT8)*p) >= 0) && n + 1 < sizeof e->label)
			{
				if ((UINT8)*p >= 0x80) // a colour code
				{
					p++;
					continue;
				}
				if (!(*p == ' ' && p[1] == ' '))
					e->label[n++] = (char)tolower((UINT8)*p);
				else // two spaces end the label ("<Cross> OK   <Circle> Back")
					break;
				p++;
			}
			while (n && e->label[n - 1] == ' ')
				n--;
			e->label[n] = '\0';
			strlcpy(e->source, source, sizeof e->source);
			continue;
		}
		p++;
	}
}

static INT32 HintDuplicates(char *out, size_t outn)
{
	INT32 i, j, dups = 0;

	out[0] = '\0';
	for (i = 0; i < hlog_n; i++)
		for (j = i + 1; j < hlog_n; j++)
			if (hlog[i].token == hlog[j].token || (hlog[i].label[0] && !strcmp(hlog[i].label, hlog[j].label)))
			{
				dups++;
				if (strlen(out) + 40 < outn)
					snprintf(out + strlen(out), outn - strlen(out), " dup(%s:%s|%s:%s)", hlog[i].source, hlog[i].label, hlog[j].source, hlog[j].label);
			}
	return dups;
}

boolean PS2MenuHints_Shows(INT32 icon)
{
	return cv_menuhints.value && shown_menu == currentMenu && icon >= 0 && icon < 64 && (shown_bits & ((UINT64)1 << icon));
}

static boolean opt_cmdline; // -menuhintscheck / -iconcheck on the command line

static void Command_Crawl_f(void);
static void Command_Go_f(void);
static void Command_Geom_f(void);
static void Command_Place_f(void);

static void Opts(void)
{
	static boolean parsed;

	if (parsed)
		return;
	parsed = true;
	COM_AddCommand("ps2_menucrawl", Command_Crawl_f, 0);
	COM_AddCommand("ps2_menugo", Command_Go_f, 0);
	COM_AddCommand("ps2_mhgeom", Command_Geom_f, 0);
	COM_AddCommand("ps2_mhplace", Command_Place_f, 0);
	opt_cmdline = M_CheckParm("-menuhintscheck") || M_CheckParm("-iconcheck"); // both switch on the check of the frame: the hints against the menu, the icons against the text
	opt_check = opt_cmdline;
	opt_debug = M_CheckParm("-menuhintsdebug") != 0;
	opt_dump = M_CheckParm("-menuhintsdump") != 0;
}

void PS2MenuHints_Begin(void)
{
	Opts();
	if (checking || !(cv_menuhints.value || opt_check) || !menuactive)
		return;
	memset(occ, 0, sizeof occ);
	IcReset();
	HintLogReset();
	ps2mh_recording = true;
	begun = true;
}

void PS2MenuHints_MenuStart(void)
{
	if (checking || begun)
		return;
	PS2MenuHints_Begin();
}

// A screen that is not a menu (the network screen, the connection screens of the client): the check of the icons of its frame
static boolean ext_frame;
static char ext_last[8][24]; // the screens already reported, and the counters of the last report (a line is printed again only when they change)
static INT32 ext_count[8][4];

void PS2MenuHints_FrameBegin(void)
{
	Opts();
	if (checking || !opt_check || ext_frame)
		return;
	IcReset();
	HintLogReset();
	ps2mh_recording = true;
	ext_frame = true;
}

void PS2MenuHints_FrameEnd(const char *screen)
{
	INT32 i;

	if (!ext_frame)
		return;
	ext_frame = false;
	ps2mh_recording = false;
	hlog_open = false;
	if (hlog_n)
	{
		char dupbuf[200];
		const INT32 dups = HintDuplicates(dupbuf, sizeof dupbuf);
		static INT32 last_hints[8], last_n[8];
		static char last_name[8][24];
		INT32 k;

		for (k = 0; k < 8 && last_name[k][0] && strcmp(last_name[k], screen); k++)
			;
		if (k == 8)
			k = 7;
		if (strcmp(last_name[k], screen) || last_n[k] != hlog_n || last_hints[k] != dups)
		{
			strlcpy(last_name[k], screen, sizeof last_name[k]);
			last_n[k] = hlog_n;
			last_hints[k] = dups;
			CONS_Printf("HINTCHK screen=%s hints=%d dup=%d%s\n", screen, (int)hlog_n, (int)dups, dupbuf);
		}
	}
	if (!ic_icons)
		return;
	for (i = 0; i < 8 && ext_last[i][0]; i++)
		if (!strcmp(ext_last[i], screen))
			break;
	if (i == 8)
		i = 7;
	if (ext_last[i][0] && !strcmp(ext_last[i], screen) && ext_count[i][0] == ic_icons && ext_count[i][1] == ic_on_glyph && ext_count[i][2] == ic_icon_icon && ext_count[i][3] == ic_glyph_on_icon)
		return;
	strlcpy(ext_last[i], screen, sizeof ext_last[i]);
	ext_count[i][0] = ic_icons;
	ext_count[i][1] = ic_on_glyph;
	ext_count[i][2] = ic_icon_icon;
	ext_count[i][3] = ic_glyph_on_icon;
	{
		INT32 xl, xr;

		IcExtent(&xl, &xr);
		CONS_Printf("ICONCHK screen=%s %dx%d icons=%d on_text=%d icon_icon=%d text_on_icon=%d %s x=%d..%d\n", screen, (int)vid.width, (int)vid.height, (int)ic_icons, (int)ic_on_glyph,
			(int)ic_icon_icon, (int)ic_glyph_on_icon, (ic_on_glyph | ic_icon_icon | ic_glyph_on_icon) ? "OVERLAP" : "ok", (int)xl, (int)xr);
	}
}

boolean PS2MenuHints_Checking(void)
{
	return checking;
}

// ---- placing the groups --------------------------------------------------------------------------------------------------------------------------------
typedef struct { INT32 x, y, w, h; } rect_t;
static rect_t plates[6]; // the plates drawn this frame (for the check)
static INT32 numplates;
static char how[3] = "  "; // how the left and the right group were placed (PlaceGroup)

static boolean PlateFree(INT32 x, INT32 y, INT32 w, INT32 h)
{
	if (x < EDGE_X - PLATE_PAD || x + w > BASEVIDWIDTH - (EDGE_X - PLATE_PAD) || y < 0 || y + h > BASEVIDHEIGHT - 4)
	{
		if (opt_debug)
			CONS_Printf("MHPLACE [%d,%d,%dx%d] outside the safe area\n", (int)x, (int)y, (int)w, (int)h);
		return false; // outside the safe area
	}
	if (OccAny(x - GAP, y - GAP, x + w + GAP, y + h + GAP))
	{
		if (opt_debug)
			CONS_Printf("MHPLACE [%d,%d,%dx%d] blocked at %d,%d\n", (int)x, (int)y, (int)w, (int)h, (int)occ_hitx, (int)occ_hity);
		return false;
	}
	return true;
}

static void Commit(const char *s, INT32 x0, INT32 y, INT32 w)
{
	// a plate of the menu's own colour under the text: it stays readable over the logo and the level
	V_DrawFill(x0 - PLATE_PAD, y - 1, w + 2 * PLATE_PAD, PLATE_H, cv_menubgcolor.value|V_TRANSLUCENT);
	PS2MenuHints_Log("corner", s);
	for (const char *q = s; *q; q++)
		if ((UINT8)*q < 0x16 && PS2UI_TokenIcon((UINT8)*q) >= 0)
			placing_bits |= (UINT64)1 << PS2UI_TokenIcon((UINT8)*q);
	ps2ui_bigicons = true; // the plate is 15 px tall: the large icons fit, and read better from the sofa
	V_DrawThinString(x0, y + FONT_Y, V_ALLOWLOWERCASE, s);
	ps2ui_bigicons = false;
	OccSet(x0 - PLATE_PAD, y - 1, x0 + w + PLATE_PAD, y - 1 + PLATE_H);
	if (numplates < (INT32)(sizeof plates / sizeof plates[0]))
	{
		plates[numplates].x = x0 - PLATE_PAD;
		plates[numplates].y = y - 1;
		plates[numplates].w = w + 2 * PLATE_PAD;
		plates[numplates].h = PLATE_H;
		numplates++;
	}
}

// the width of a group of hints: with the large icons that it is drawn with
static INT32 GroupWidth(const char *s)
{
	INT32 w;

	ps2ui_bigicons = true;
	w = V_ThinStringWidth(s, V_ALLOWLOWERCASE);
	ps2ui_bigicons = false;
	return w;
}

static boolean Fits(const char *s, boolean right, INT32 row, INT32 *x0, INT32 *y, INT32 *w)
{
	*w = GroupWidth(s);
	*x0 = right ? BASEVIDWIDTH - EDGE_X - *w : EDGE_X;
	*y = ROW0_Y - row * ROW_PITCH;
	return PlateFree(*x0 - PLATE_PAD, *y - 1, *w + 2 * PLATE_PAD, PLATE_H);
}

// the nearest free place for the text s along a row, from the corner on towards the middle of the screen (2 px steps); false: none
static boolean Slide(const char *s, boolean right, INT32 row, INT32 *x0, INT32 *y, INT32 *w)
{
	INT32 x;

	*w = GroupWidth(s);
	*y = ROW0_Y - row * ROW_PITCH;
	for (x = EDGE_X; x + *w <= BASEVIDWIDTH - EDGE_X; x += 2)
	{
		*x0 = right ? BASEVIDWIDTH - EDGE_X - *w - (x - EDGE_X) : x;
		if (*x0 < EDGE_X || (right ? *x0 < BASEVIDWIDTH / 2 : *x0 + *w > BASEVIDWIDTH / 2))
			break; // each group keeps to its own half of the screen
		if (PlateFree(*x0 - PLATE_PAD, *y - 1, *w + 2 * PLATE_PAD, PLATE_H))
			return true;
	}
	return false;
}

// splits a group at its first three spaces; false: it has no halves
static boolean Halves(const char *s, char *a, char *b)
{
	const char *p = strstr(s, "   ");

	if (!p)
		return false;
	memcpy(a, s, (size_t)(p - s));
	a[p - s] = '\0';
	strcpy(b, p + 3);
	return true;
}

// the icons of a group and nothing else
static boolean IconsOnly(const char *s, char *out)
{
	size_t n = 0;

	for (; *s; s++)
		if ((UINT8)*s < 0x16 && PS2UI_TokenIcon((UINT8)*s) >= 0)
		{
			if (n)
				out[n++] = ' ';
			out[n++] = *s;
		}
	out[n] = '\0';
	return n > 0;
}

// How a group was placed: 'L' one line at the bottom, 'R' one line a row or two higher, 'S' two stacked lines, 'I' icons only, '-' nowhere. A group goes in one line at the
// lowest row that is free (two rows higher at most), else in two stacked lines, else as icons, else it is not drawn.
static char PlaceGroup(const char *s, boolean right)
{
	INT32 x0, y, w, x1, y1, w1, row;
	char a[160], b[160], icons[80];

	icons[0] = '\0';
	for (row = 0; row < NUMROWS; row++)
		if (Fits(s, right, row, &x0, &y, &w))
		{
			Commit(s, x0, y, w);
			return row ? 'R' : 'L';
		}
	if (Halves(s, a, b))
		for (row = 0; row < NUMROWS - 1; row++)
			if (Fits(b, right, row, &x0, &y, &w) && Fits(a, right, row + 1, &x1, &y1, &w1))
			{
				Commit(b, x0, y, w);
				Commit(a, x1, y1, w1);
				return 'S';
			}
	if (IconsOnly(s, icons))
		for (row = 0; row < NUMROWS; row++)
			if (Fits(icons, right, row, &x0, &y, &w))
			{
				Commit(icons, x0, y, w);
				return 'I';
			}
	// the corner is taken in every row: the nearest free place along the bottom rows, towards the middle of the screen (the text first, then the icons)
	for (row = 0; row < NUMROWS; row++)
		if (Slide(s, right, row, &x0, &y, &w))
		{
			Commit(s, x0, y, w);
			return 'M';
		}
	if (icons[0])
		for (row = 0; row < NUMROWS; row++)
			if (Slide(icons, right, row, &x0, &y, &w))
			{
				Commit(icons, x0, y, w);
				return 'm';
			}
	return '-';
}

// ---- the check (-menuhintscheck): the pixels of the frame, software renderer ------------------------------------------------------------------------------
static UINT8 *chk_save, *chk_mask;
static size_t chk_size;

static void ClearScreen(UINT8 c)
{
	memset(screens[0], c, chk_size);
}

// fills `mask` with the pixels that M_Drawer changes on a flat colour `bg`, apart from what the fade of the menu does: the frame of an empty menu is the reference.
// content / ink (when given): the pixels of the menu WITHOUT its icons (the icons take their room but are not drawn), and the pixels that the icons cover
static void MenuMask(UINT8 bg, UINT8 *mask, UINT8 *content, UINT8 *ink)
{
	menu_t dummy;
	menu_t *real = currentMenu;
	UINT8 *ref = chk_save + chk_size; // the second to sixth part of the scratch block
	UINT8 *n1 = chk_save + 2 * chk_size, *t1 = chk_save + 3 * chk_size, *n2 = chk_save + 4 * chk_size, *t2 = chk_save + 5 * chk_size;
	size_t i;

	memset(&dummy, 0, sizeof dummy);
	dummy.menuitems = real->menuitems;
	dummy.numitems = 0;
	ClearScreen(bg);
	currentMenu = &dummy;
	ps2ui_hideicons = content != NULL; // (what the frame has apart from the menu, the on-screen keyboard, has icons of its own: they must not count as the menu's icons)
	M_Drawer();
	ps2ui_hideicons = false;
	currentMenu = real;
	memcpy(ref, screens[0], chk_size);
	if (content)
	{
		// the pixels of the text: the menu drawn without its icons (n), and without its icons and letters (t); the difference is what the letters cover, on whatever background
		// the menu has of its own. Some menus animate their background in every call of the drawing routine (the Marathon menu): both pictures are drawn twice, and a pixel
		// that differs between the two draws of the same picture is not looked at.
		ps2ui_hideicons = true;
		ClearScreen(bg);
		M_Drawer();
		memcpy(n1, screens[0], chk_size);
		ps2ui_hidetext = true;
		ClearScreen(bg);
		M_Drawer();
		memcpy(t1, screens[0], chk_size);
		ps2ui_hidetext = false;
		ClearScreen(bg);
		M_Drawer();
		memcpy(n2, screens[0], chk_size);
		ps2ui_hidetext = true;
		ClearScreen(bg);
		M_Drawer();
		memcpy(t2, screens[0], chk_size);
		ps2ui_hidetext = ps2ui_hideicons = false;
		for (i = 0; i < chk_size; i++)
			content[i] |= (UINT8)(n1[i] == n2[i] && t1[i] == t2[i] && n1[i] != t1[i]);
	}
	ClearScreen(bg);
	M_Drawer();
	for (i = 0; i < chk_size; i++)
	{
		mask[i] |= (UINT8)(screens[0][i] != ref[i]);
		if (ink)
			ink[i] |= (UINT8)(screens[0][i] != n1[i] && n1[i] == n2[i] && t1[i] == t2[i]);
	}
}

static void Check(void)
{
	const INT32 dup = vid.dup > 0 ? vid.dup : 1;
	size_t i, covered = 0, near = 0, inside = 0;
	INT32 p;
	UINT8 *pmask = NULL, *pnear = NULL, *content = NULL, *ink = NULL;
	size_t touch = 0, inkpix = 0;
	INT32 oy = (vid.height - BASEVIDHEIGHT * dup) / 2, ox = (vid.width - BASEVIDWIDTH * dup) / 2; // the 320x200 picture is centred in a bigger one
	const boolean pixels = rendermode == render_soft && screens[0] && currentMenu;
	INT32 first[3][2], nfirst = 0, xl = 0, xr = 0;
	fixed_t saved_rdt;

	if (!currentMenu)
		return;
	if (pixels)
	{
		chk_size = (size_t)vid.rowbytes * (size_t)vid.height;
		chk_save = malloc(chk_size * 6);
		chk_mask = calloc(chk_size, 1);
		pmask = calloc(chk_size, 1);
		pnear = calloc(chk_size, 1);
		content = calloc(chk_size, 1);
		ink = calloc(chk_size, 1);
		if (!chk_save || !chk_mask || !pmask || !pnear || !content || !ink)
		{
			free(chk_save);
			free(chk_mask);
			free(pmask);
			free(pnear);
			free(content);
			free(ink);
			chk_save = chk_mask = NULL;
			return;
		}
		memcpy(chk_save, screens[0], chk_size);
		checking = true;
		ps2mh_recording = true; // the draws are looked at (backgrounds over the whole screen are left out)
		saved_rdt = renderdeltatics;
		renderdeltatics = 0; // the animations of the menus (the save slots that scroll, the character select) stand still while it is drawn again
		MenuMask(0xFE, chk_mask, content, ink);
		MenuMask(0x01, chk_mask, content, ink); // twice, on two colours: a menu pixel that happens to have the colour of the background is not lost
		renderdeltatics = saved_rdt;
		if (ic_icons)
		{
			// the icons against the text: a pixel of the menu (without its icons) that touches a pixel of an icon, also corner to corner, or lies under it
			const INT32 rb = (INT32)vid.rowbytes;
			INT32 x, y, dx, dy;

			for (y = 0; y < vid.height; y++)
				for (x = 0; x < vid.width; x++)
				{
					if (ink[(size_t)y * rb + x])
						inkpix++;
					if (!content[(size_t)y * rb + x])
						continue;
					for (dy = -1; dy <= 1; dy++)
						for (dx = -1; dx <= 1; dx++)
							if (x + dx >= 0 && x + dx < vid.width && y + dy >= 0 && y + dy < vid.height && ink[(size_t)(y + dy) * rb + x + dx])
							{
								touch++;
								dy = dx = 2; // (once per pixel of the menu)
							}
				}
			if (touch && opt_dump) // a picture of the failure: the icons white, the text grey, the text that touches an icon red
			{
				static INT32 icdumps;
				char path[256];
				FILE *f;

				snprintf(path, sizeof path, "%s/icfail-%d-%u-%d.ppm", I_GetEnv("HOME") ? I_GetEnv("HOME") : ".", (int)icdumps++, (unsigned)currentMenu->menuid, (int)M_PS2MenuCursor(-1));
				f = fopen(path, "wb");
				if (f)
				{
					fprintf(f, "P6\n%d %d\n255\n", (int)vid.width, (int)vid.height);
					for (y = 0; y < vid.height; y++)
						for (x = 0; x < vid.width; x++)
						{
							boolean near_ink = false;
							UINT8 px[3];

							for (dy = -1; dy <= 1 && !near_ink; dy++)
								for (dx = -1; dx <= 1; dx++)
									if (x + dx >= 0 && x + dx < vid.width && y + dy >= 0 && y + dy < vid.height && ink[(size_t)(y + dy) * rb + x + dx])
										near_ink = true;
							px[0] = (UINT8)(ink[(size_t)y * rb + x] ? 255 : (content[(size_t)y * rb + x] ? (near_ink ? 255 : 110) : 0));
							px[1] = (UINT8)(ink[(size_t)y * rb + x] ? 255 : (content[(size_t)y * rb + x] && !near_ink ? 110 : 0));
							px[2] = px[1];
							fwrite(px, 1, 3, f);
						}
					fclose(f);
				}
			}
		}
		// the plates and their margin as pixel masks
		for (p = 0; p < numplates; p++)
		{
			INT32 x, y;

			for (y = (plates[p].y - GAP) * dup + oy; y < (plates[p].y + plates[p].h + GAP) * dup + oy; y++)
				for (x = (plates[p].x - GAP) * dup + ox; x < (plates[p].x + plates[p].w + GAP) * dup + ox; x++)
					if (x >= 0 && x < vid.width && y >= 0 && y < vid.height)
					{
						const INT32 inner = x >= (plates[p].x) * dup + ox && x < (plates[p].x + plates[p].w) * dup + ox && y >= (plates[p].y) * dup + oy && y < (plates[p].y + plates[p].h) * dup + oy;

						pnear[(size_t)y * vid.rowbytes + x] = 1;
						if (inner)
							pmask[(size_t)y * vid.rowbytes + x] = 1;
					}
		}
		for (i = 0; i < chk_size; i++)
		{
			covered += chk_mask[i];
			near += (size_t)(chk_mask[i] & pnear[i]);
			inside += (size_t)(chk_mask[i] & pmask[i]);
			if (chk_mask[i] & pnear[i] && nfirst < 3) // where: the first pixels, in the 320x200 picture
			{
				first[nfirst][0] = (INT32)((i % vid.rowbytes - ox) / dup);
				first[nfirst][1] = (INT32)((i / vid.rowbytes - oy) / dup);
				nfirst++;
			}
		}
		checking = false;
		ps2mh_recording = false;
		memcpy(screens[0], chk_save, chk_size);
		if (near && opt_dump) // a picture of the failure: the menu pixels white, the plates and their margin red, both yellow
		{
			static INT32 dumps;
			char path[256];
			FILE *f;
			INT32 x, y;

			snprintf(path, sizeof path, "%s/mhfail-%d-%u-%d.ppm", I_GetEnv("HOME") ? I_GetEnv("HOME") : ".", (int)dumps++, (unsigned)currentMenu->menuid, (int)M_PS2MenuCursor(-1));
			f = fopen(path, "wb");
			if (f)
			{
				fprintf(f, "P6\n%d %d\n255\n", (int)vid.width, (int)vid.height);
				for (y = 0; y < vid.height; y++)
					for (x = 0; x < vid.width; x++)
					{
						const size_t k = (size_t)y * vid.rowbytes + x;
						const UINT8 px[3] = {(UINT8)((chk_mask[k] || pnear[k]) ? 255 : 0), (UINT8)(chk_mask[k] ? 255 : 0), (UINT8)(chk_mask[k] && !pnear[k] ? 255 : 0)};

						fwrite(px, 1, 3, f);
					}
				fclose(f);
			}
			snprintf(path, sizeof path, "%s/mhocc-%d.ppm", I_GetEnv("HOME") ? I_GetEnv("HOME") : ".", (int)dumps - 1); // the map of what the hooks noted, 320x200: occupied = white, plates red
			f = fopen(path, "wb");
			if (f)
			{
				fprintf(f, "P6\n%d %d\n255\n", (int)BASEVIDWIDTH, (int)BASEVIDHEIGHT);
				for (y = 0; y < BASEVIDHEIGHT; y++)
					for (x = 0; x < BASEVIDWIDTH; x++)
					{
						INT32 q, inplate = 0;
						UINT8 px[3];

						for (q = 0; q < numplates; q++)
							inplate |= x >= plates[q].x && x < plates[q].x + plates[q].w && y >= plates[q].y && y < plates[q].y + plates[q].h;
						px[0] = (UINT8)((occ_pre[y][x >> 3] & (1u << (x & 7))) || inplate ? 255 : 0);
						px[1] = (UINT8)((occ_pre[y][x >> 3] & (1u << (x & 7))) && !inplate ? 255 : 0);
						px[2] = px[1];
						fwrite(px, 1, 3, f);
					}
				fclose(f);
			}
		}
	}
	IcExtent(&xl, &xr);
	CONS_Printf("MHCHECK menu=%u item=%d/%d kind=%d how=%c%c plates=%d", (unsigned)currentMenu->menuid, (int)M_PS2MenuCursor(-1), (int)currentMenu->numitems, (int)M_PS2MenuKind(), how[0], how[1], (int)numplates);
	for (p = 0; p < numplates; p++)
		CONS_Printf(" [%d,%d,%dx%d]", (int)plates[p].x, (int)plates[p].y, (int)plates[p].w, (int)plates[p].h);
	if (pixels)
	{
		CONS_Printf(" %dx%d menu_pixels=%u under=%u within2=%u %s", (int)vid.width, (int)vid.height, (unsigned)covered, (unsigned)inside, (unsigned)near, near ? "OVERLAP" : "ok");
		for (p = 0; p < nfirst; p++)
			CONS_Printf(" at(%d,%d)", (int)first[p][0], (int)first[p][1]);
		CONS_Printf(" icons=%d on_text=%d icon_icon=%d text_on_icon=%d ink=%u touch=%u %s x=%d..%d\n", (int)ic_icons, (int)ic_on_glyph, (int)ic_icon_icon, (int)ic_glyph_on_icon, (unsigned)inkpix, (unsigned)touch,
			(ic_on_glyph | ic_icon_icon | ic_glyph_on_icon || touch) ? "ICONOVERLAP" : "iconok", (int)xl, (int)xr);
	}
	else
		CONS_Printf(" %dx%d (hardware renderer: the pixels are not checked here) icons=%d on_text=%d icon_icon=%d text_on_icon=%d %s x=%d..%d\n", (int)vid.width, (int)vid.height, (int)ic_icons, (int)ic_on_glyph,
			(int)ic_icon_icon, (int)ic_glyph_on_icon, (ic_on_glyph | ic_icon_icon | ic_glyph_on_icon) ? "ICONOVERLAP" : "iconok", (int)xl, (int)xr);
	{
		char dupbuf[200];
		const INT32 dups = HintDuplicates(dupbuf, sizeof dupbuf);

		CONS_Printf("HINTCHK menu=%u item=%d/%d hints=%d dup=%d%s\n", (unsigned)currentMenu->menuid, (int)M_PS2MenuCursor(-1), (int)currentMenu->numitems, (int)hlog_n, (int)dups, dupbuf);
	}
	free(chk_save);
	free(chk_mask);
	free(pmask);
	free(pnear);
	free(content);
	free(ink);
	chk_save = chk_mask = NULL;
}

// ps2_mhgeom: does the rectangle that the hooks note contain every pixel that the draw really changes? (software renderer; the noted rectangle is the one the groups of
// hints keep clear of.) Draws a few patches / fills with and without scaling and snapping on a flat colour, finds the changed pixels and compares.
static void Command_Geom_f(void)
{
	static const char *const pnames[] = {"BLANKLVL", "BLACXLVL", "CHARBG", "SAVEBACK", "SAVENONE", "ULTIMATE", "GAMEDONE", "STLIVEX"};
	static const struct { fixed_t scale; INT32 flags; const char *name; } modes[] =
	{
		{FRACUNIT, 0, "scaled"}, {FRACUNIT / 2, 0, "half"}, {FRACUNIT, V_SNAPTOLEFT | V_SNAPTOTOP, "snap left top"}, {FRACUNIT, V_SNAPTORIGHT | V_SNAPTOBOTTOM, "snap right bottom"},
		{FRACUNIT, V_NOSCALEPATCH, "noscale patch"}, {FRACUNIT, V_SMALLSCALEPATCH, "small scale patch"}, {FRACUNIT * 3 / 4, 0, "3/4"},
	};
	static const INT32 xs[] = {0, 13, 150, 230}, ys[] = {0, 7, 60, 150};
	const INT32 dup = vid.dup > 0 ? vid.dup : 1;
	const INT32 ox = (vid.width - BASEVIDWIDTH * dup) / 2, oy = (vid.height - BASEVIDHEIGHT * dup) / 2;
	INT32 bad = 0, total = 0, pi, mi, xi, yi, k;

	if (rendermode != render_soft || !screens[0])
	{
		CONS_Printf("MHGEOM: software renderer only\n");
		return;
	}
	chk_size = (size_t)vid.rowbytes * (size_t)vid.height;
	chk_save = malloc(chk_size);
	if (!chk_save)
		return;
	memcpy(chk_save, screens[0], chk_size);
	for (pi = 0; pi < (INT32)(sizeof pnames / sizeof pnames[0]); pi++)
	{
		patch_t *patch = W_CachePatchName(pnames[pi], PU_PATCH);

		for (mi = 0; mi < (INT32)(sizeof modes / sizeof modes[0]); mi++)
			for (xi = 0; xi < 4; xi++)
				for (yi = 0; yi < 4; yi++)
				{
					INT32 x0 = vid.width, y0 = vid.height, x1 = -1, y1 = -1, px, py;
					const INT32 x = xs[xi], y = ys[yi];

					ClearScreen(0x01);
					last_note[0] = last_note[1] = last_note[2] = last_note[3] = -999;
					checking = false;
					ps2mh_recording = true;
					V_DrawStretchyFixedPatch((fixed_t)x << FRACBITS, (fixed_t)y << FRACBITS, modes[mi].scale, modes[mi].scale, modes[mi].flags, patch, NULL);
					ps2mh_recording = false;
					for (py = 0; py < vid.height; py++)
						for (px = 0; px < vid.width; px++)
							if (screens[0][(size_t)py * vid.rowbytes + px] != 0x01)
							{
								x0 = min(x0, px);
								y0 = min(y0, py);
								x1 = max(x1, px);
								y1 = max(y1, py);
							}
					if (x1 < 0)
						continue; // off the screen
					total++;
					{
						const INT32 ax0 = FloorDiv(x0 - ox, dup), ay0 = FloorDiv(y0 - oy, dup), ax1 = FloorDiv(x1 - ox, dup) + 1, ay1 = FloorDiv(y1 - oy, dup) + 1;

						if (last_note[0] > ax0 || last_note[1] > ay0 || last_note[2] < ax1 || last_note[3] < ay1)
						{
							bad++;
							if (bad <= 12)
								CONS_Printf("MHGEOM MISMATCH %s %s at %d,%d: drawn [%d,%d)-[%d,%d) noted [%d,%d)-[%d,%d)\n", pnames[pi], modes[mi].name, (int)x, (int)y, (int)ax0, (int)ay0, (int)ax1, (int)ay1,
									(int)last_note[0], (int)last_note[1], (int)last_note[2], (int)last_note[3]);
						}
					}
				}
	}
	// fills
	for (mi = 0; mi < 4; mi++)
		for (xi = 0; xi < 4; xi++)
			for (yi = 0; yi < 4; yi++)
			{
				static const INT32 fl[4] = {0, V_SNAPTOLEFT | V_SNAPTOTOP, V_SNAPTORIGHT | V_SNAPTOBOTTOM, V_SNAPTOLEFT | V_SNAPTOBOTTOM};
				INT32 x0 = vid.width, y0 = vid.height, x1 = -1, y1 = -1, px, py;

				ClearScreen(0x01);
				last_note[0] = last_note[1] = last_note[2] = last_note[3] = -999;
				ps2mh_recording = true;
				V_DrawFill(xs[xi], ys[yi], 37, 21, fl[mi] | 0xFE);
				ps2mh_recording = false;
				for (py = 0; py < vid.height; py++)
					for (px = 0; px < vid.width; px++)
						if (screens[0][(size_t)py * vid.rowbytes + px] != 0x01)
						{
							x0 = min(x0, px);
							y0 = min(y0, py);
							x1 = max(x1, px);
							y1 = max(y1, py);
						}
				if (x1 < 0)
					continue;
				total++;
				{
					const INT32 ax0 = FloorDiv(x0 - ox, dup), ay0 = FloorDiv(y0 - oy, dup), ax1 = FloorDiv(x1 - ox, dup) + 1, ay1 = FloorDiv(y1 - oy, dup) + 1;

					if (last_note[0] > ax0 || last_note[1] > ay0 || last_note[2] < ax1 || last_note[3] < ay1)
					{
						bad++;
						if (bad <= 12)
							CONS_Printf("MHGEOM MISMATCH fill flags %x at %d,%d: drawn [%d,%d)-[%d,%d) noted [%d,%d)-[%d,%d)\n", (unsigned)fl[mi], (int)xs[xi], (int)ys[yi], (int)ax0, (int)ay0, (int)ax1, (int)ay1,
								(int)last_note[0], (int)last_note[1], (int)last_note[2], (int)last_note[3]);
					}
				}
			}
	(void)k;
	memcpy(screens[0], chk_save, chk_size);
	free(chk_save);
	chk_save = NULL;
	CONS_Printf("MHGEOM %dx%d: %d draws, %d noted too small\n", (int)vid.width, (int)vid.height, (int)total, (int)bad);
}

// ---- the end of the frame ------------------------------------------------------------------------------------------------------------------------------
// ps2_mhplace N (1..7): the placement of the left group of the "select" hints against a made-up obstacle in the bottom rows, to see (and for the log to say) every way
// that a group can be placed: 1 nothing in the way (L), 2 the corner of the lowest row blocked (R), 3 the corner blocked in every row (M), 4 the middle of the left
// half blocked so that the line is too long for the room (S), 5 less room still (I), 6 the whole left half blocked (-), 7 the safe area of the screen is taken
// all along the bottom. 0 = off. Each draws the obstacle as a red block under the hints.
static INT32 place_test;
static const struct { INT32 x0, y0, x1, y1; char want; } place_cases[] =
{
	{0, 0, 0, 0, 'L'}, {0, 176, 40, 200, 'R'}, {0, 140, 30, 200, 'M'}, {75, 140, 160, 200, 'S'}, {45, 140, 160, 200, 'I'}, {0, 140, 160, 200, '-'},
};

static void Command_Place_f(void)
{
	place_test = COM_Argc() > 1 ? atoi(COM_Argv(1)) : 0;
	if (place_test > (INT32)(sizeof place_cases / sizeof place_cases[0]))
		place_test = 0;
	menuactive = true;
}

static void PlaceTest(void)
{
	const hintset_t *set = &hintsets[PS2MH_SELECT];
	const INT32 c = place_test - 1;
	static INT32 last_logged;

	memset(occ, 0, sizeof occ);
	numplates = 0;
	if (place_cases[c].x1 > place_cases[c].x0)
	{
		V_DrawFill(place_cases[c].x0, place_cases[c].y0, place_cases[c].x1 - place_cases[c].x0, place_cases[c].y1 - place_cases[c].y0, 35);
		OccSet(place_cases[c].x0, place_cases[c].y0, place_cases[c].x1, place_cases[c].y1);
	}
	how[0] = PlaceGroup(set->left, false);
	how[1] = PlaceGroup(set->right, true);
	if (last_logged != place_test)
	{
		last_logged = place_test;
		CONS_Printf("MHPLACE-TEST %d: left group placed '%c' (expected '%c'), right group '%c' %s\n", (int)place_test, how[0], place_cases[c].want, how[1], how[0] == place_cases[c].want ? "ok" : "WRONG");
	}
}

void PS2MenuHints_Draw(void)
{
	const hintset_t *set;
	INT32 kind;

	if (checking) // the check draws the menu again: the plates of the real frame stay
		return;
	ps2mh_recording = false;
	begun = false;
	numplates = 0;
	how[0] = how[1] = ' ';
	placing_bits = 0;
	if (place_test)
	{
		PlaceTest();
		return;
	}
	if (!menuactive)
		return;
	if (cv_menuhints.value && !PS2OSK_Active()) // (the keyboard has its own line of hints)
	{
		kind = M_PS2MenuKind();
		if (kind >= 0 && kind < PS2MH_NUMKINDS)
		{
			set = (kind == PS2MH_CHAT || kind == PS2MH_TEAMCHAT) ? ChatSet(kind == PS2MH_TEAMCHAT) : &hintsets[kind];
			if (set->left)
				how[0] = PlaceGroup(set->left, false);
			if (set->right)
				how[1] = PlaceGroup(set->right, true);
		}
	}
	shown_bits = placing_bits; // the next frame of this menu: its own lines for these buttons are left out (PS2MenuHints_Shows)
	shown_menu = (cv_menuhints.value && placing_bits) ? currentMenu : NULL;
	if (opt_check)
	{
		UINT32 sig = (UINT32)(size_t)currentMenu * 2654435761u + (UINT32)M_PS2MenuCursor(-1) * 40503u + (UINT32)numplates + (PS2OSK_Active() ? 977u : 0u);
		INT32 p;

		for (p = 0; p < numplates; p++)
			sig = sig * 31u + (UINT32)(plates[p].x + plates[p].y * 400 + plates[p].w * 7);
		// a menu that is still moving (the save slots sliding in, a list scrolling) is drawn differently by every frame, and some draw routines change their state
		// even when the time stands still: the check, which draws the frame again, waits for two frames with the same picture of the menu (40 frames at most)
		UINT32 hash = 2166136261u;
		size_t q;
		boolean stable;

		for (q = 0; q < sizeof occ; q++)
			hash = (hash ^ ((const UINT8 *)occ)[q]) * 16777619u;
		stable = hash == occ_hash_last || stable_wait >= 40;
		occ_hash_last = hash;
		if (!stable)
			stable_wait++;
		else if (crawling ? check_armed : sig != check_sig)
		{
			memcpy(occ_pre, occ, sizeof occ);
			check_armed = false;
			check_sig = sig;
			stable_wait = 0;
			Check();
		}
	}
	hlog_open = false;
}

// ---- the crawler: every menu, every item ----------------------------------------------------------------------------------------------------------------
static INT32 crawl_n, crawl_m, crawl_i, crawl_wait, crawl_last;
static menu_t *crawl_saved;
static INT32 crawl_saved_item;

static void Command_Crawl_f(void)
{
	if (crawling)
	{
		crawling = false;
		return;
	}
	crawl_n = M_PS2MenuList(-1, NULL, NULL);
	crawl_m = COM_Argc() > 1 ? atoi(COM_Argv(1)) : 0; // ps2_menucrawl [first menu]: after a menu that cannot be drawn, go on with the next
	crawl_last = COM_Argc() > 2 ? atoi(COM_Argv(2)) : crawl_n - 1; // ps2_menucrawl first last
	crawl_i = -1;
	crawl_wait = 0;
	crawl_saved = currentMenu;
	crawl_saved_item = M_PS2MenuCursor(-1);
	opt_check = true;
	crawling = true;
	menuactive = true;
	CONS_Printf("MHCRAWL start: %d menus\n", (int)crawl_n);
}

// The menus of the record attack and the like put the game into another state (GS_TIMEATTACK) when they are entered, and the next menu would be drawn in it: every menu
// of the crawler and of -menuseq starts from the state of the first call (the title screen), without a screen wipe.
static void BaseState(void)
{
	static boolean have;
	static gamestate_t base;

	if (!have)
	{
		have = true;
		base = gamestate;
		return;
	}
	gamestate = wipegamestate = base;
}

// ps2_menugo MENU [ITEM]: bring up menu number MENU of the crawler's list with the cursor on ITEM (for pictures: -netcmd 60:ps2_menugo~51~3 -vidshot f200)
static void Command_Go_f(void)
{
	menu_t *m = NULL;
	const char *name = "";
	const INT32 n = M_PS2MenuList(-1, NULL, NULL), idx = COM_Argc() > 1 ? atoi(COM_Argv(1)) : 0;

	if (idx < 0 || idx >= n)
		return;
	M_PS2MenuList(idx, &m, &name);
	BaseState();
	M_PS2MenuEnter(m, idx, n);
	menuactive = true;
	M_PS2MenuCursor(COM_Argc() > 2 ? atoi(COM_Argv(2)) : 0);
	if (COM_Argc() > 3) // ps2_menugo MENU ITEM HINTS: the cvar menuhints on / off (pictures of the same menu with and without the hints)
		CV_StealthSetValue(&cv_menuhints, atoi(COM_Argv(3)) ? 1 : 0);
	CONS_Printf("MHGO %d %s item %d hints %d\n", (int)idx, name, (int)M_PS2MenuCursor(-1), (int)cv_menuhints.value);
}

// -menuseq SPACING,M:I[:HINTS],P<N>,...: (P<N> = ps2_mhplace N) a picture series for -vidshot: from the 150th displayed frame on, every SPACING frames the next menu M of the crawler's list is brought up
// with the cursor on item I (Command_Go_f); -vidshot m<N> takes the picture at the N-th frame of the series (the pace is that of displayed frames: a screen wipe or a slow
// game tic does not move it)
static const char *seqspec;
static INT32 seqframe, seqcalls;

void PS2MenuHints_SeqTick(void)
{
	static boolean parsed;

	if (!parsed)
	{
		parsed = true;
		if (M_CheckParm("-menuseq") && M_IsNextParm())
			seqspec = M_GetNextParm();
	}
	if (!seqspec || ++seqcalls < 150) // the title screen is up after about 100 displayed frames
		return;
	seqframe++;
	{
		const INT32 spacing = atoi(seqspec) > 0 ? atoi(seqspec) : 30;
		const INT32 want = (seqframe - 1) / spacing;
		const char *p = strchr(seqspec, ','), *colon, *next;
		INT32 k;

		if ((seqframe - 1) % spacing)
			return;
		for (k = 0; p && k < want; k++)
			p = strchr(p + 1, ',');
		if (!p)
			return;
		colon = strchr(p + 1, ':');
		next = strchr(p + 1, ',');
		if (p[1] == 'P') // P3: ps2_mhplace 3 (the placement test patterns)
		{
			char cmd[32];

			snprintf(cmd, sizeof cmd, "ps2_mhplace %d\n", atoi(p + 2));
			COM_BufAddText(cmd);
			return;
		}
		{
			char cmd[48];

			const char *colon2 = (colon && (!next || colon < next)) ? strchr(colon + 1, ':') : NULL;

			snprintf(cmd, sizeof cmd, "ps2_menugo %d %d %d\n", atoi(p + 1), (colon && (!next || colon < next)) ? atoi(colon + 1) : 0, (colon2 && (!next || colon2 < next)) ? atoi(colon2 + 1) : (int)cv_menuhints.value);
			COM_BufAddText(cmd);
		}
	}
}

INT32 PS2MenuHints_SeqFrame(void)
{
	return seqframe;
}

static boolean Selectable(const menuitem_t *it)
{
	const UINT16 st = it->status;

	if (st == IT_DISABLED || (st & IT_DISPLAY) == IT_HEADERTEXT || (st & IT_TYPE) == IT_SPACE)
		return false;
	return true;
}

void PS2MenuHints_Frame(void)
{
	Opts();
	if (!crawling)
		return;
	if (check_armed) // the frame with the new cursor is not drawn and checked yet (140 game frames at most)
	{
		if (++crawl_wait < 140)
			return;
		check_armed = false;
	}
	for (;;)
	{
		menu_t *m = NULL;
		const char *name = "";

		if (crawl_m >= crawl_n || crawl_m > crawl_last)
		{
			CONS_Printf("MHCRAWL done: %d menus\n", (int)crawl_n);
			crawling = false;
			opt_check = opt_cmdline;
			currentMenu = crawl_saved;
			M_PS2MenuCursor(crawl_saved_item);
			return;
		}
		M_PS2MenuList(crawl_m, &m, &name);
		if (crawl_i < 0)
		{
			CONS_Printf("MHCRAWL menu %d/%d %s id=%u items=%d title=%s\n", (int)crawl_m, (int)crawl_n, name, (unsigned)m->menuid, (int)m->numitems, m->menutitlepic ? m->menutitlepic : "-");
			BaseState();
			M_PS2MenuEnter(m, crawl_m, crawl_n);
			menuactive = true;
			crawl_i = 0;
		}
		while (crawl_i < m->numitems && !Selectable(&m->menuitems[crawl_i]))
			crawl_i++;
		if (crawl_i >= m->numitems)
		{
			crawl_m++;
			crawl_i = -1;
			if (m->numitems <= 0)
				continue;
			continue;
		}
		M_PS2MenuCursor(crawl_i);
		crawl_i++;
		crawl_wait = 0;
		check_armed = true;
		return;
	}
}
