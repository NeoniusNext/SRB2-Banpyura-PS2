// SONIC ROBO BLAST 2 - PS2 port
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps2_loadprof.h
/// \brief PS2-LOAD-1 (OPT12-LOAD): load-time profiler (EE COP0 Count) and level-structure hash, run time switch -loadprof / -loadhash

#ifndef __PS2_LOADPROF__
#define __PS2_LOADPROF__

#include "../doomtype.h"

// Slots. LAP slots are consecutive phases of the start-up (the time since the previous lap goes to the slot); the others are
// inclusive intervals started with LP_BEGIN and stopped with LP_END, they may nest and overlap. Names are printed in the "LP" lines.
#define PS2LP_SLOTS(X) \
	X(B_BOOTINIT, "boot: PS2Boot_Init (IOP modules, argv)") \
	X(B_SYSINIT, "boot: log + I_StartupSystem") \
	X(B_EARLY, "boot: locale, settings, paths -> Z_Init") \
	X(B_ZINIT, "boot: Z_Init, COM_Init, autoload scan, tables") \
	X(B_WADMAIN, "boot: W_InitMultipleFiles (4 main packs + their SOC)") \
	X(B_GFX0, "boot: cht_Init .. before I_StartupGraphics") \
	X(B_GFX1, "boot: I_StartupGraphics") \
	X(B_GFX2, "boot: SCR_Startup, palette remap") \
	X(B_HUINIT, "boot: HU_Init") \
	X(B_CONINIT, "boot: CON_Init") \
	X(B_GFX3, "boot: command registration (server, client, engine, sound, sys)") \
	X(B_WADEXTRA, "boot: extra PWADs (-file / autoload)") \
	X(B_HULOAD, "boot: HU_LoadGraphics") \
	X(B_CONFIG, "boot: M_FirstLoadConfig") \
	X(B_GAMEDATA, "boot: G_LoadGameData .. SCR_CheckDefaultMode") \
	X(B_MINIT, "boot: M_Init") \
	X(B_RINIT, "boot: R_Init") \
	X(B_SOUND, "boot: sound init (IOP audio, mixer threads)") \
	X(B_STINIT, "boot: ST_Init") \
	X(B_NET, "boot: D_CheckNetGame .. autoexec") \
	X(B_START, "boot: start of title / intro / map") \
	X(B_FIRSTFRAME, "boot: main loop -> first frame shown") \
	/* inclusive intervals: files and lumps */ \
	X(W_INITFILE, "W_InitFile (one file, all)") \
	X(W_PACKTABLE, "  WPack_GetLumps (pack header, table, names)") \
	X(W_MD5, "  MD5 of the file") \
	X(W_SOCLOAD, "  SOC / MAINCFG lumps of the file") \
	X(W_LUALOAD, "  Lua lumps of the file") \
	X(W_RESOURCES, "  W_LoadResources of the file (textures, sprites, ...)") \
	X(W_READLUMP, "W_ReadLump* (all lumps read)") \
	X(W_CACHELUMP, "W_CacheLumpNum misses (zone + read)") \
	X(PK_FREAD, "  pack: fread (file I/O)") \
	X(PK_LZ4, "  pack: LZ4 decode") \
	X(W_CHECKNAME, "W_CheckNumForName* (all)") \
	/* level */ \
	X(LV_TOTAL, "P_LoadLevel (all)") \
	X(LV_PRE1, "    pre 1: CON_Drawer / I_FinishUpdate (loading screen)") \
	X(LV_PRE2, "    pre 2: palette, secnodes, SOC / level script, settings") \
	X(LV_PRE3, "    pre 3: cvars, sounds stop, music fade") \
	X(LV_PRE4, "    pre 4: level wipe (fade out)") \
	X(LV_PRE5, "    pre 5: Speeding off text, S_Start (music)") \
	X(LV_FREE, "  old level freed") \
	X(LV_SETUP, "  level setup before the map file (sky, colormaps, ...)") \
	X(LV_MAPFILE, "  P_LoadMapFromFile (all)") \
	X(LV_VRES, "    vres_GetMap (map lump table)") \
	X(LV_MAPDATA, "    P_LoadMapData (vertexes, sectors, lines, sides, things)") \
	X(LV_BSP, "    P_LoadMapBSP (segs, subsectors, nodes)") \
	X(LV_LUT, "    P_LoadMapLUT (blockmap, reject)") \
	X(LV_LINK, "    P_LinkMapData") \
	X(LV_TAGS, "    tags + binary conversion + line args") \
	X(LV_SPAWNSTATE, "    spawn state copy + map MD5") \
	X(LV_AFTERMAP, "  unlockables / save after the map") \
	X(LV_SLOPES, "  P_SpawnSlopes") \
	X(LV_THINGS, "  P_SpawnMapThings") \
	X(LV_SPECIALS, "  P_SpawnSpecials + precipitation") \
	X(LV_HWBUILD, "  hardware level build") \
	X(LV_PRECACHE, "  R_PrecacheLevel") \
	X(LV_POST, "  after the precache (pre-ticker, MapLoad hooks, title card)") \
	/* UDMF */ \
	X(U_OPEN, "UDMF: tokenizer open (copy of the TEXTMAP)") \
	X(U_COUNT, "UDMF: count pass") \
	X(U_VERT, "UDMF: vertexes") \
	X(U_SECT, "UDMF: sectors") \
	X(U_LINE, "UDMF: linedefs") \
	X(U_SIDE, "UDMF: sidedefs") \
	X(U_THING, "UDMF: things") \
	X(U_TEXLOOK, "UDMF: texture/flat name lookups (inside the above)") \
	/* Lua / SOC */ \
	X(LUA_NEWSTATE, "Lua: state creation (LUA_Init)") \
	X(LUA_LOADLUMP, "Lua: LUA_LoadLump (all)") \
	X(LUA_PARSE, "Lua:   luaL_loadbuffer (lexer + parser)") \
	X(LUA_RUN, "Lua:   chunk run") \
	X(LUA_GC, "Lua:   full collections after a script was loaded / run") \
	X(SOC_PARSE, "SOC: DEH_LoadDehackedLump (all)") \
	/* add-ons */ \
	X(AD_TOTAL, "add-on: P_AddWadFile / D_AddFile path (all)") \
	X(AD_ADDFILE, "  W_InitFile") \
	X(AD_POST, "  post-processing (textures, sprites, skins, ...)") \
	/* engine init pieces */ \
	X(R_TEXTURES, "R_LoadTextures") \
	X(R_FLATS, "R_InitFlats / P_InitPicAnims") \
	X(R_SPRITES, "R_InitSprites") \
	X(R_COLORMAPS, "R_InitColormaps / R_InitTranslationTables") \
	X(R_SKINS, "R_InitSkins") \
	X(R_SPRDEFS, "R_InitSprites: R_AddSpriteDefs of every file") \
	X(R_ADDSKINS, "R_InitSprites: R_AddSkins of every file") \
	X(R_PATCHSKINS, "R_InitSprites: R_PatchSkins of every file") \
	X(R_SPRINFO, "R_InitSprites: R_LoadSpriteInfoLumps of every file") \
	X(R_FACEGFX, "R_InitSprites: ST_ReloadSkinFaceGraphics") \
	X(R_OTHER, "R_Init other (tables, view, HUD patches)") \
	X(R_LIGHTGEN, "R_GenerateLightTable (computed, not the cached default)") \
	X(OTHER1, "other 1") \
	X(OTHER2, "other 2") \
	X(OTHER3, "other 3")

typedef enum
{
#define X(id, name) LP_##id,
	PS2LP_SLOTS(X)
#undef X
	LP_COUNT
} ps2lp_slot_t;

#if defined(PS2_PROFILE)

#ifdef PS2
extern boolean ps2lp_on;
#else
#define ps2lp_on 0 /* host profile build (tools/ps2/host_variant.sh): no EE counter, the macros below compile to nothing */
#endif

static inline UINT32 PS2LP_Now(void)
{
	UINT32 v;

#ifdef PS2
	__asm__ volatile("mfc0 %0,$9" : "=r"(v));
#else
	v = 0;
#endif
	return v;
}

void PS2LP_Init(UINT32 t0); // -loadprof (main); t0 = the Count value at the start of main()
void PS2LP_Add(int slot, UINT32 t0); // adds the time since t0 (PS2LP_Now()) to the slot
void PS2LP_Lap(int slot); // adds the time since the previous lap to the slot (start-up phases)
void PS2LP_Report(const char *label); // prints and clears the slots ("LP label slot cycles calls")
void PS2LP_SampleEvent(int ev); // -ps2sample -lpsamp N: the PC sampler (build.py --sample) over the start-up (1) or the first level (2)

#define LP_BEGIN(v) UINT32 v = ps2lp_on ? PS2LP_Now() : 0
#define LP_RESTART(v) ((v) = ps2lp_on ? PS2LP_Now() : 0)
#define LP_END(slot, v) do { if (ps2lp_on) PS2LP_Add(LP_##slot, v); } while (0)
#define LP_LAP(slot) do { if (ps2lp_on) PS2LP_Lap(LP_##slot); } while (0)
#define LP_REPORT(label) do { if (ps2lp_on) PS2LP_Report(label); } while (0)
#define LP_SAMPLE(ev) do { if (ps2lp_on) PS2LP_SampleEvent(ev); } while (0)

// FNV-1a over 32-bit words (little endian: the same on the host), for the level-structure hash of -loadhash
typedef struct { UINT32 h[2]; } ps2lp_hash_t; // two independent 32-bit FNV lanes
static inline void PS2LP_H32(ps2lp_hash_t *hs, UINT32 v)
{
	int i;

	for (i = 0; i < 4; i++)
	{
		UINT32 b = (v >> (8 * i)) & 0xFF;

		hs->h[0] = (hs->h[0] ^ b) * 16777619u;
		hs->h[1] = (hs->h[1] ^ (b + 0x9Eu)) * 2246822519u;
	}
}
void PS2LP_HashPrint(const char *label, const ps2lp_hash_t *hs);

#else

#define LP_BEGIN(v) UINT32 v = 0; (void)v
#define LP_RESTART(v) ((void)(v))
#define LP_END(slot, v) ((void)(v))
#define LP_LAP(slot) ((void)0)
#define LP_REPORT(label) ((void)0)
#define LP_SAMPLE(ev) ((void)0)

#endif

#endif // __PS2_LOADPROF__
