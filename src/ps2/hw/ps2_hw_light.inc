// PS2 GS hardware renderer driver: SRB2 shader lighting maths (included by ps2_hwd.c and by the x86 host test).
//
// The OpenGL driver lights a fragment in GLSL (src/hardware/hw_shaders.h): R_DoomColormap(light, z) -> floor -> darkness in
// 1/32 steps, optional remap by the colormap's fade start/end, then final = mix(colour, fade colour, darkness). The GS has no
// fragment programs. Darkness is a staircase of the eye depth z, so it is reproduced exactly as
//   * GS fog on pieces of constant darkness (a polygon is cut at the depths where the staircase steps), or
//   * for solid polygons: a second pass that blends the fade colour with a per-pixel alpha looked up in a 256-entry
//     CLUT by the interpolated eye depth (ramp), which needs no cutting.
// All functions here are pure.

#define LIGHT_ZMAX 2032.0f // lightz = clamp(z / 16, 0, 127): the colormap no longer changes beyond z = 2032

typedef struct
{
	float light; // `lighting` uniform, 0..255
	float sfudge, cfudge; // STARTMAP_FUDGE, SCALE_FUDGE: floor 1.06 / 1.15, wall 1.05 / 2.2
	int fade_start, fade_end; // colormap fade range, 0..31 (0 / 31 = no remap)
	int pal; // PS2-HW-71 palette rendering: the colormap row is floor(R_DoomColormap) clamped to 0..31, so the classes 0 and 1 are the same row (no cut between them)
} lightp_t;

static inline void light_setup(lightp_t *lp, int light, int wall, int fs, int fe)
{
	lp->light = (float)light;
	lp->sfudge = wall ? 1.05f : 1.06f;
	lp->cfudge = wall ? 2.2f : 1.15f;
	lp->fade_start = fs;
	lp->fade_end = fe;
	lp->pal = 0;
}

static inline float lmaxf(float a, float b)
{
	return a > b ? a : b;
}

// R_DoomColormap (non-dithered) for a fragment z
static inline float doom_colormap(const lightp_t *lp, float z)
{
	float lightnum = lp->light / 17.0f, lightz = z / 16.0f, startmap, scale, cap;

	lightnum = lightnum < 0.0f ? 0.0f : lightnum > 15.0f ? 15.0f : lightnum;
	lightz = lightz < 0.0f ? 0.0f : lightz > 127.0f ? 127.0f : lightz;
	startmap = (15.0f - lightnum) * 4.0f;
	scale = 160.0f / (lightz + 1.0f);
	cap = (155.0f - lp->light) * 0.26f;
	return lmaxf(startmap * lp->sfudge - scale * 0.5f * lp->cfudge, cap);
}

// darkness class 0..32: 0 = none, 1..31 = (class - 0.5) / 32, 32 = 31 / 32 (R_DoomLightingEquation's clamp)
static inline int doom_class(const lightp_t *lp, float z)
{
	float cm = floorf(doom_colormap(lp, z));

	if (cm < 0.0f)
		return lp->pal ? 1 : 0;
	if (cm >= 31.0f)
		return 32;
	return (int)cm + 1;
}

// darkness of a class after the fade remap, in 1/128 (the GS alpha scale), 0..128
static inline int doom_darkness128(const lightp_t *lp, int cls)
{
	float d = cls == 0 ? 0.0f : cls >= 32 ? 31.0f / 32.0f : ((float)cls - 0.5f) / 32.0f;

	if (lp->fade_start != 0 || lp->fade_end != 31)
	{
		float fs = (float)lp->fade_start / 31.0f, fe = (float)lp->fade_end / 31.0f, fd = fe - fs;

		d = (d - fs) * (1.0f / fd);
		d = d < 0.0f ? 0.0f : d > 1.0f ? 1.0f : d;
	}
	return (int)(d * 128.0f + 0.5f);
}

// eye depth at which the colormap first reaches `m` (floor(cm) >= m); LIGHT_ZMAX * 2 when it never does, 0 when it always does
static inline float doom_depth_of_level(const lightp_t *lp, int m)
{
	float lightnum = lp->light / 17.0f, startmap, cap, base, lz;

	lightnum = lightnum < 0.0f ? 0.0f : lightnum > 15.0f ? 15.0f : lightnum;
	startmap = (15.0f - lightnum) * 4.0f;
	cap = (155.0f - lp->light) * 0.26f;
	base = startmap * lp->sfudge;
	if (cap >= (float)m)
		return 0.0f; // the cap already is at or above this level
	if (base <= (float)m)
		return LIGHT_ZMAX * 2.0f; // base - scale * c < base <= m: never
	// base - 160 / (lz + 1) * 0.5 * cfudge = m
	lz = 160.0f * 0.5f * lp->cfudge / (base - (float)m) - 1.0f;
	if (lz >= 127.0f)
		return LIGHT_ZMAX * 2.0f;
	return lz < 0.0f ? 0.0f : lz * 16.0f;
}

// Depths (ascending) where the darkness class steps, inside (z0, z1); returns the count (at most `max`). cls[i] is the class
// from th[i] on; *cls0 receives the class at z0.
static int doom_thresholds(const lightp_t *lp, float z0, float z1, float *th, int *cls, int max, int *cls0)
{
	int c0 = doom_class(lp, z0), c1 = doom_class(lp, z1), c, n = 0;

	*cls0 = c0;
	for (c = c0 + 1; c <= c1; c++)
	{
		float z = doom_depth_of_level(lp, c - 1); // class c starts where floor(colormap) reaches c - 1

		if (z <= z0)
			continue;
		if (z >= z1)
			break;
		if (n && z <= th[n - 1])
		{
			cls[n - 1] = c; // two classes begin at the same depth: the later one wins
			continue;
		}
		if (n == max)
			break;
		th[n] = z;
		cls[n] = c;
		n++;
	}
	return n;
}
