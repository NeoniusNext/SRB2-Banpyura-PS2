/* The formatting, composition, refresh, placement, draw and Off paths below
 * come from the actual isolated ps2_memhud.c. Only engine/GS services are fake. */
#include <stddef.h>
typedef uint8_t UINT8; typedef int16_t INT16; typedef int32_t INT32; typedef uint32_t UINT32;
typedef unsigned tic_t;
#define FONTSTART 33
#define FONTSIZE 95
#define BASEVIDWIDTH 320
#define BASEVIDHEIGHT 200
#define PU_STATIC 1
#define MEM_DUMMY 0
#define V_ALLOWLOWERCASE MEM_DUMMY
#define V_REDMAP MEM_DUMMY
#define V_YELLOWMAP MEM_DUMMY
#define V_GREENMAP MEM_DUMMY
#define V_GRAYMAP MEM_DUMMY
#define MENUCOLOR MEM_DUMMY
#define V_SNAPTORIGHT MEM_DUMMY
#define V_SNAPTOBOTTOM MEM_DUMMY
#define V_USERHUDTRANS MEM_DUMMY
#define V_40TRANS MEM_DUMMY
#define SAMPLE_TICS 9
#define GS_NULL 0
typedef struct { unsigned topdelta, length, data_offset; } post_t;
typedef struct { unsigned num_posts; post_t *posts; UINT8 *pixels; } column_t;
typedef struct fixture_patch_s { int width, height, leftoffset, topoffset; column_t *columns; void **owner; } patch_t;
typedef struct { INT16 width, height, leftoffset, topoffset; } softwarepatch_t;
static struct { patch_t *chars[FONTSIZE]; unsigned spacewidth; int kerning; } tny_font;
typedef struct { int value; } fixture_cvar_t;
static fixture_cvar_t cv_showmem, cv_ticrate, cv_showping;
static int netgame, consoleplayer, serverplayer, simulated_lag, gamestate = 1;
static UINT32 servermaxping, playerpingtable[1];
static int Playing(void) { return 1; }
static int GetThreadId(void) { return 7; }
static unsigned DIntr(void) { return 1; }
static void EIntr(void) {}
static unsigned PS2Mem_Cycles(void) { return 100; }
static unsigned fixture_tic;
static tic_t I_GetTime(void) { return fixture_tic; }
static int M_CheckParm(const char *s) { (void)s; return 0; }
static unsigned const font_widths[FONTSIZE] = {
#include "font-widths.inc"
};
#include "hud-types.inc"
#include "hud-format.inc"
#include "hud-globals.inc"
#include "hud-workspace.inc"

static int fail_workspace, fail_patch, logical_scale = 1;
static unsigned workspace_allocs, workspace_frees, patch_allocs, patch_frees, draws, glyph_draws;
static unsigned char *workspace_raw;
static ps2memhud_t fixture_sample;
static int ZA_Ready(void) { return 1; }
static void *Z_TryMallocAlign(size_t n, int tag, void *owner, int align)
{
	(void)tag; (void)align;
	workspace_allocs++;
	if (fail_workspace) return NULL;
	workspace_raw = malloc(n + 64);
	if (!workspace_raw) abort();
	memset(workspace_raw, 0xa5, n + 64);
	void *p = workspace_raw + 32;
	*(void **)owner = p;
	return p;
}
static void check_workspace(void)
{
	if (!workspace_raw) return;
	for (unsigned i = 0; i < 32; i++)
		CHECK(workspace_raw[i] == 0xa5 && workspace_raw[32 + sizeof *work + i] == 0xa5);
}
static void Z_Free(void *p)
{
	if (!p) return;
	CHECK(p == work);
	check_workspace(); free(workspace_raw); workspace_raw = NULL; work = NULL; workspace_frees++;
}
static void Patch_Free(patch_t *p)
{
	if (p->owner) *p->owner = NULL;
	free(p); patch_frees++;
}
static patch_t *Patch_TryCreateFromDoomPatch(const softwarepatch_t *p)
{
	check_workspace();
	const unsigned char *data = (const unsigned char *)p;
	const INT32 *offsets = (const INT32 *)(const void *)(data + 8);
	CHECK(p->width > 0 && p->width <= BMP_W && (p->height == 8 || p->height == 16));
	for (int x = 0; x < p->width; x++)
	{
		unsigned at = (unsigned)offsets[x];
		CHECK(at >= 8u + 4u * p->width && at < sizeof doc);
		while (at < sizeof doc && data[at] != 255)
		{
			unsigned y = data[at], n = data[at + 1];
			CHECK(n && y + n <= (unsigned)p->height && at + 4u + n < sizeof doc);
			at += 4 + n;
		}
		CHECK(at < sizeof doc);
	}
	if (fail_patch) return NULL;
	patch_t *out = calloc(1, sizeof *out);
	if (!out) abort();
	out->width = p->width; out->height = p->height; patch_allocs++;
	return out;
}
static void Z_SetUser(patch_t *p, void **owner) { p->owner = owner; *owner = p; }
static const UINT8 *V_GetStringColormap(int color) { (void)color; return NULL; }
static void PS2MemHud_Sample(ps2memhud_t *out) { *out = fixture_sample; }
static void rect(int x, int y, int w, int h)
{
	CHECK(x >= 0 && y >= 0 && x + w <= 320 && y + h <= 200);
	CHECK(x * logical_scale + w * logical_scale <= 320 * logical_scale);
	CHECK(y * logical_scale + h * logical_scale <= 200 * logical_scale);
	draws++;
}
static void V_DrawFill(int x, int y, int w, int h, int flags) { (void)flags; rect(x, y, w, h); }
static void V_DrawScaledPatch(int x, int y, int flags, patch_t *p) { (void)flags; rect(x, y, p->width, p->height); }
static void V_DrawThinString(int x, int y, int flags, const char *s) { (void)flags; rect(x, y, str_w(s), 7); glyph_draws++; }
#include "hud-compose.inc"
#include "hud-refresh.inc"
#include "hud-bottom.inc"
#include "hud-onchange.inc"
#include "hud-draw.inc"

static void test_hud(void)
{
	static patch_t glyphs[FONTSIZE];
	static column_t columns[16];
	static post_t posts[4] = {{0,1,0},{2,1,1},{4,1,2},{6,1,3}};
	static UINT8 pixels[4] = {3,4,5,6};
	for (unsigned c = 0; c < 16; c++) columns[c] = (column_t){4,posts,pixels};
	for (unsigned c = 0; c < FONTSIZE; c++) if (font_widths[c])
	{
		glyphs[c] = (patch_t){(int)font_widths[c],7,0,0,columns,NULL};
		tny_font.chars[c] = &glyphs[c];
	}
	tny_font.spacewidth = 2; tny_font.kerning = 0;
	for (unsigned cpu = 0; cpu <= 100; cpu++) for (unsigned gpu = 0; gpu <= 100; gpu++)
	{
		struct { unsigned char pre[32]; memline_t line; unsigned char post[32]; } canary;
		memset(&canary, 0xa5, sizeof canary);
		fixture_sample.cpu_percent = cpu; fixture_sample.gpu_percent = gpu; fixture_sample.gpu_valid = 1;
		line_activity(&canary.line, &fixture_sample);
		for (unsigned i = 0; i < 32; i++) CHECK(canary.pre[i] == 0xa5 && canary.post[i] == 0xa5);
		CHECK(strlen(canary.line.label) < sizeof canary.line.label && strlen(canary.line.main) < sizeof canary.line.main && strlen(canary.line.detail) < sizeof canary.line.detail);
		measure(&canary.line); CHECK(line_w(&canary.line) < 288);
	}
	fixture_sample = (ps2memhud_t){0};
	fixture_sample.ram_total = fixture_sample.ram_used = fixture_sample.ram_free = fixture_sample.zone_free = fixture_sample.zone_big = fixture_sample.zone_peak = fixture_sample.libc_free = 131072;
	fixture_sample.vram_total = fixture_sample.vram_used = fixture_sample.vram_fb = fixture_sample.vram_z = fixture_sample.vram_clut = fixture_sample.vram_tex_total = fixture_sample.vram_tex_used = fixture_sample.vram_ws = 4096;
	fixture_sample.cpu_percent = fixture_sample.gpu_percent = 100; fixture_sample.gpu_valid = 1;
	{
		memline_t l;
		memset(&l, 0xa5, sizeof l);
		line_ram2(&l, &fixture_sample);
		CHECK(strlen(l.detail) < sizeof l.detail);
		fixture_sample.hw = 1; line_vram2(&l, &fixture_sample); CHECK(strlen(l.detail) < sizeof l.detail);
		fixture_sample.gpu_valid = 0; line_activity(&l, &fixture_sample); CHECK(strstr(l.detail, "N/A") && strlen(l.detail) < sizeof l.detail);
		fixture_sample.gpu_valid = 1;
	}
	for (unsigned mode = 1; mode <= 3; mode++) for (unsigned hw = 0; hw <= 1; hw++) for (unsigned fps = 0; fps <= 1; fps++) for (unsigned ping = 0; ping <= 1; ping++) for (unsigned scale = 1; scale <= 2; scale++)
	{
		cv_showmem.value = mode; cv_ticrate.value = fps; cv_showping.value = ping;
		netgame = 1; consoleplayer = 0; serverplayer = 1; fixture_sample.hw = hw; logical_scale = scale;
		showmem_onchange(); fixture_tic += 10; prof_on = 0;
		PS2MemHud_Draw();
		CHECK(nlines == 3 && tpatch && apatch && work);
		for (unsigned i = 0; i < 3; i++) CHECK(line_w(&mem_lines[i]) <= BMP_W);
		unsigned allocs = workspace_allocs + patch_allocs;
		for (unsigned i = 0; i < 20; i++) { fixture_tic++; PS2MemHud_Draw(); }
		CHECK(workspace_allocs + patch_allocs == allocs); // stable frames make no allocations
		cv_showmem.value = 0; showmem_onchange(); CHECK(!work && !tpatch && !apatch && !ps2_activity_enabled);
	}
	fail_workspace = 1; cv_showmem.value = 1; showmem_onchange(); fixture_tic += 10;
	unsigned before = glyph_draws; PS2MemHud_Draw(); CHECK(!work && !tpatch && !apatch && glyph_draws > before);
	fail_workspace = 0; fail_patch = 1; fixture_tic += 10; PS2MemHud_Draw(); CHECK(work && !tpatch && !apatch);
	fail_patch = 0; fixture_tic += 10; PS2MemHud_Draw(); CHECK(tpatch && apatch);
	cv_showmem.value = 0; showmem_onchange(); CHECK(!work && workspace_frees && patch_frees == patch_allocs);
	printf("ACTIVITY HUD workspace=%zu bytes optional/off-free; rows=3 draws=%u font-asset-widths=95\n", sizeof(memworkspace_t), draws);
}
