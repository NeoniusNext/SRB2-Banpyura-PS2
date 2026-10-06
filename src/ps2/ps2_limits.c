// PS2-104 (OPT8-F): the free-slot limits of the PC game (mobj types 1024, sprites 1024, states 8192, sounds 1600 + skin sounds, colours 1024, sprite2 1024,
// 256 frames per sprite) on a 32 MB machine. The tables of the old PS2 profile (PS2-09, PS2-11) stay the starting size; the first time an add-on needs a slot
// past them (Lua freeslot(), SOC Freeslot, S_AddSoundFx, a sprite lump with a frame >= 64) PS2Limits_Grow() replaces them with PC-size copies, once. The
// constants (NUMSTATES, NUMMOBJTYPES, NUMSFX, ...) are the PC values at all times, so every number an add-on, a save or a demo can see is the PC number; the
// LIMIT_* macros (info.h, sounds.h, doomdef.h) are the size of the tables that are live.
//
// Pointers into the old tables: the live mobjs (state, info) are re-pointed here; sound channels are stopped first (they hold S_sfx pointers);
// the skin sound numbers move with the skin slots of S_sfx (they follow the free slots, whose number changed); Lua keeps no pointers across the call
// (userdata of states/mobjinfo made before the growth keep pointing at the old, now unused, copy).

#include "../doomdef.h"

#ifdef PS2_DYNLIMITS

#include "../info.h"
#include "../sounds.h"
#include "../s_sound.h"
#include "../z_zone.h"
#include "../m_misc.h"
#include "../p_local.h"
#include "../p_mobj.h"
#include "../r_defs.h"
#include "../r_skins.h"
#include "../r_things.h"
#include "../r_draw.h"
#include "../r_data.h" // Color_cons_t
#include "../lua_script.h"
#include "../deh_tables.h"
#include "../doomstat.h"
#ifdef HWRENDER
#include "../hardware/hw_main.h"
#endif

boolean ps2_fulllimits = false;
boolean ps2_fullsfx = false;

extern spriteframe_t *sprtemp; // r_things.c
extern spritedef_t *sprites;
extern size_t numsprites;
#ifdef HAS_LUA
void LUA_GrowMobjHooks(INT32 oldtypes, INT32 newtypes); // lua_hooklib.c
#endif
void R_GrowTranslationCaches(void); // r_draw.c
#ifdef HWRENDER
void HWR_GrowSpriteTables(void); // hw_md2.c
#endif

#ifdef HAS_LUA
// the userdata of a table element that a script made before the table moved follow it (lua_script.c)
static void RemapTable(const void *oldt, const void *newt, size_t elemsize, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		LUA_RemapUserdata((const UINT8 *)oldt + i * elemsize, (UINT8 *)newt + i * elemsize);
}
#endif

static void *Grown(const void *old, size_t oldsize, size_t newsize)
{
	void *p = Z_Calloc(newsize, PU_STATIC, NULL);

	M_Memcpy(p, old, oldsize);
	return p;
}

// S_sfx: [builtin][free slots][skin slots]. The skin slots follow the free slots, so they move; the sounds that are in them go with them.
static void GrowSounds(void)
{
	const INT32 smallskin0 = sfx_freeslot0 + PS2_SMALL_SFXFREESLOTS;
	const INT32 fullskin0 = sfx_freeslot0 + NUMSFXFREESLOTS;
	const INT32 oldnum = smallskin0 + PS2_SMALL_SKINSFXSLOTS;
	sfxinfo_t *newsfx = Z_Calloc(sizeof (sfxinfo_t) * NUMSFX, PU_STATIC, NULL);
	char (*newnames)[7] = Z_Calloc(sizeof (char[7]) * (sfx_freeslot0 + NUMSFXFREESLOTS + NUMSKINSFXSLOTS), PU_STATIC, NULL);
	INT32 i, k;
	skin_t *pending = ps2_skin_pending;

	// builtin sounds and the free slots in use: unchanged numbers
	M_Memcpy(newsfx, S_sfx, sizeof (sfxinfo_t) * smallskin0);
	M_Memcpy(newnames, freeslotnames, sizeof (char[7]) * (smallskin0 - sfx_freeslot0 + 1)); // names are indexed by number - sfx_freeslot0
	// skin sounds: the old skin slots to the end of the new table
	for (k = 0; k < PS2_SMALL_SKINSFXSLOTS; k++)
	{
		newsfx[fullskin0 + k] = S_sfx[smallskin0 + k];
		M_Memcpy(newnames[fullskin0 + k - sfx_freeslot0], freeslotnames[smallskin0 + k - sfx_freeslot0], sizeof (char[7]));
	}
#ifdef HAS_LUA
	RemapTable(S_sfx, newsfx, sizeof (sfxinfo_t), (size_t)smallskin0);
	for (k = 0; k < PS2_SMALL_SKINSFXSLOTS; k++)
		LUA_RemapUserdata(&S_sfx[smallskin0 + k], &newsfx[fullskin0 + k]);
#endif
	S_sfx = newsfx;
	freeslotnames = newnames;
	// every slot of the free and skin ranges that was not carried over is empty, with its generated name
	{
		const INT32 emptyfrom[2] = { smallskin0, fullskin0 + PS2_SMALL_SKINSFXSLOTS };
		const INT32 emptyto[2] = { fullskin0, NUMSFX };
		INT32 r;

		for (r = 0; r < 2; r++)
			for (i = emptyfrom[r]; i < emptyto[r]; i++)
			{
				INT32 value = (i + 1) - sfx_freeslot0;

				if (value < 10)
					sprintf(freeslotnames[value - 1], "fre00%d", value);
				else if (value < 100)
					sprintf(freeslotnames[value - 1], "fre0%d", value);
				else if (value < 1000)
					sprintf(freeslotnames[value - 1], "fre%d", value);
				else
					sprintf(freeslotnames[value - 1], "fr%d", value);
				S_sfx[i].singularity = false;
				S_sfx[i].priority = 0;
				S_sfx[i].pitch = 0;
				S_sfx[i].volume = -1;
				S_sfx[i].data = NULL;
				S_sfx[i].length = 0;
				S_sfx[i].skinsound = -1;
				S_sfx[i].usefulness = -1;
				S_sfx[i].lumpnum = LUMPERROR;
				S_sfx[i].caption[0] = '\0';
			}
	}
	for (i = sfx_freeslot0; i < NUMSFX; i++)
		S_sfx[i].name = freeslotnames[i - sfx_freeslot0];

	// the numbers of the skin sounds (the skin R_AddSkins is building is not counted in numskins yet)
	for (i = 0; i < numskins + (pending ? 1 : 0); i++)
	{
		skin_t *skin = i < numskins ? skins[i] : pending;

		for (k = 0; k < NUMSKINSOUNDS; k++)
			if ((INT32)skin->soundsid[k] >= smallskin0 && (INT32)skin->soundsid[k] < oldnum)
				skin->soundsid[k] += fullskin0 - smallskin0;
	}
	// sfxfree (the next free slot) stays: S_AddSoundFx grows the tables when it ran past the last small free slot, which is now a free slot
	ps2_fullsfx = true; // from here the LIMIT_*SFX* sizes are the PC ones
}

// The sound tables grow on their own (a skin with sounds of its own, more than 256 free sounds): 'sfx numbers' of skins are PC numbers from then on
void PS2Limits_GrowSounds(void)
{
	if (ps2_fullsfx)
		return;
	CONS_Printf("PS2 limits: the sound tables grow to the PC size (sounds %d)\n", (int)NUMSFX);
	S_StopSounds(); // the channels hold pointers into S_sfx
	GrowSounds();
}

void PS2Limits_Grow(void)
{
	const INT32 s0 = PS2_SMALL_MOBJFREESLOTS, c0 = PS2_SMALL_COLORFREESLOTS;
	state_t *oldstates = states;
	mobjinfo_t *oldinfo = mobjinfo;
	skincolor_t *oldcolors = skincolors;
	thinker_t *th;
	INT32 i;

	size_t usedbefore;

	if (ps2_fulllimits)
		return;
	usedbefore = Z_TagUsage(PU_STATIC);
	CONS_Printf("PS2 limits: the slot tables grow to the PC size (states %d, mobj types %d, sprites %d, sounds %d, colours %d, sprite2 %d)\n",
		(int)NUMSTATES, (int)NUMMOBJTYPES, (int)NUMSPRITES, (int)NUMSFX, (int)MAXSKINCOLORS, (int)NUMPLAYERSPRITES);

	S_StopSounds(); // the channels hold pointers into S_sfx

	sprnames = Grown(sprnames, (size_t)(SPR_FIRSTFREESLOT + s0 + 1) * (MAXSPRITENAME + 1), (size_t)(NUMSPRITES + 1) * (MAXSPRITENAME + 1));
	spr2names = Grown(spr2names, (size_t)(SPR2_FIRSTFREESLOT + PS2_SMALL_SPR2FREESLOTS) * (MAXSPRITENAME + 1), (size_t)NUMPLAYERSPRITES * (MAXSPRITENAME + 1));
	spr2defaults = Grown(spr2defaults, sizeof (playersprite_t) * (SPR2_FIRSTFREESLOT + PS2_SMALL_SPR2FREESLOTS), sizeof (playersprite_t) * NUMPLAYERSPRITES);
	states = Grown(states, sizeof (state_t) * (S_FIRSTFREESLOT + s0 * 8), sizeof (state_t) * NUMSTATES);
	mobjinfo = Grown(mobjinfo, sizeof (mobjinfo_t) * (MT_FIRSTFREESLOT + s0), sizeof (mobjinfo_t) * NUMMOBJTYPES);
	skincolors = Grown(skincolors, sizeof (skincolor_t) * (SKINCOLOR_FIRSTFREESLOT + c0), sizeof (skincolor_t) * MAXSKINCOLORS);
	FREE_STATES = Grown(FREE_STATES, sizeof (char *) * s0 * 8, sizeof (char *) * NUMSTATEFREESLOTS);
	FREE_MOBJS = Grown(FREE_MOBJS, sizeof (char *) * s0, sizeof (char *) * NUMMOBJFREESLOTS);
	FREE_SKINCOLORS = Grown(FREE_SKINCOLORS, sizeof (char *) * c0, sizeof (char *) * NUMCOLORFREESLOTS);

#ifdef HAS_LUA
	RemapTable(oldstates, states, sizeof (state_t), (size_t)(S_FIRSTFREESLOT + s0 * 8));
	RemapTable(oldinfo, mobjinfo, sizeof (mobjinfo_t), (size_t)(MT_FIRSTFREESLOT + s0));
	RemapTable(oldcolors, skincolors, sizeof (skincolor_t), (size_t)(SKINCOLOR_FIRSTFREESLOT + c0));
#endif

	PS2Limits_GrowSounds();

	// the sprite scratch table of R_AddSingleSpriteDef: 0xFF = "no frame", as R_AddSingleSpriteDef clears it
	{
		spriteframe_t *grown = Z_Malloc(sizeof (spriteframe_t) * MAXFRAMENUM, PU_STATIC, NULL);

		memset(grown, 0xFF, sizeof (spriteframe_t) * MAXFRAMENUM);
		M_Memcpy(grown, sprtemp, sizeof (spriteframe_t) * PS2_SMALL_MAXFRAMENUM);
		sprtemp = grown;
	}

	ps2_fulllimits = true; // from here the LIMIT_* sizes are the PC ones

	P_PatchInfoRange(s0, NUMMOBJFREESLOTS, c0, NUMCOLORFREESLOTS, 15); // empty slots with their initial values

	// the list of colours the colour cvars choose from names the entries of the (moved) colour table
	for (i = 0; i < MAXSKINCOLORS; i++)
	{
		Color_cons_t[i].value = i;
		Color_cons_t[i].strvalue = skincolors[i].name;
	}
	Color_cons_t[MAXSKINCOLORS].value = 0;
	Color_cons_t[MAXSKINCOLORS].strvalue = NULL;

	if (sprites) // the sprite definitions: one per sprite name (the table was allocated at its full size by R_InitSprites)
		numsprites = NUMSPRITES;

#ifdef HAS_LUA
	LUA_GrowMobjHooks(MT_FIRSTFREESLOT + s0, NUMMOBJTYPES);
#endif
	R_GrowTranslationCaches();
#ifdef HWRENDER
	HWR_GrowSpriteTables();
#endif

	// live objects point into the old state and mobjinfo tables
	for (i = THINK_MOBJ; i <= THINK_PRECIP; i += THINK_PRECIP - THINK_MOBJ)
		for (th = thlist[i].next; th && th != &thlist[i]; th = th->next) // (the lists are empty and unlinked before the first level: next == NULL)
		{
			state_t **statep = (i == THINK_MOBJ) ? &((mobj_t *)th)->state : &((precipmobj_t *)th)->state;

			if (*statep >= oldstates && *statep < oldstates + (S_FIRSTFREESLOT + s0 * 8))
				*statep = states + (*statep - oldstates);
			if (i == THINK_MOBJ)
			{
				mobj_t *mo = (mobj_t *)th;

				if (mo->info >= oldinfo && mo->info < oldinfo + (MT_FIRSTFREESLOT + s0))
					mo->info = mobjinfo + (mo->info - oldinfo);
			}
		}
	CONS_Printf("PS2 limits: the PC-size tables took %u KiB of the zone (PU_STATIC %u -> %u KiB)\n", (unsigned)((Z_TagUsage(PU_STATIC) - usedbefore) >> 10), (unsigned)(usedbefore >> 10), (unsigned)(Z_TagUsage(PU_STATIC) >> 10));
}

#endif // PS2_DYNLIMITS
