/* x86 host test of the OPT10 HG geometry paths of the GS renderer driver (see tools/ps2/hg_hosttest.py).
 * The real src/ps2/hw/ps2_hw_*.inc files are compiled as they are; the GS side (DMA ring) is a capture buffer.
 *   neg=N : negative control N - a deliberate mutation; the group named in "HG negctl N expects <group>" must FAIL.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shim.h"
#include "ps2_hwd_dbg.h"

typedef int16_t s16;

#define HWGIF_UNUSED 0
#define CONS_WARNING 1
#define CONS_ERROR 2
#define CONS_Alert(level, ...) ((void)(level))
#define CONS_Printf(...) ((void)0)
#define memalign(alignment, bytes) malloc(bytes)
#include "ps2_hw_priv.inc"
#include "ps2_hw_hg.inc"
#include "ps2_hw_vif.inc"
#include "ps2_hw_regs.inc"

/* ---- replacements for ps2_hw_gs.inc: a capture buffer ---- */
#define CAP_MAX (1 << 22)
static qw_t *cap;
static u32 cap_n;

static void cap_reset(void)
{
	cap_n = 0;
	H.wr = 0;
	H.st.qwords = 0;
}
static void pk_flush(void)
{
	H.wr = 0;
}
static qw_t *pk_alloc(u32 n)
{
	qw_t *p;

	if (H.wr + n > BUF_QW)
		pk_flush();
	p = &cap[cap_n];
	cap_n += n;
	H.wr += n;
	if (cap_n > CAP_MAX - 64)
	{
		printf("HG FAIL capture_overflow\n");
		exit(2);
	}
	return p;
}
static qw_t *ad_alloc(u32 n)
{
	qw_t *p = pk_alloc(n + 1);

	H.serial++;
	p[0].d[0] = HWGIF_TAG(n, 1, 0, 0, 0, 1);
	p[0].d[1] = GIF_REGS_AD;
	return p + 1;
}
static void frame_prepare(int clearing)
{
	(void)clearing;
}
static void ring_wait(int limit)
{
	(void)limit;
}
static void pk_ref(const void *data, u32 qwc)
{
	(void)data;
	(void)qwc;
}
#define SyncDCache(a, b) ((void)0)
void HWR_PS2_LockData(void *data) { (void)data; }
void HWR_PS2_UnlockData(void *data) { (void)data; }
void HWR_PS2_FreeData(void *data) { free(data); }
void *HWR_PS2_StealData(GLMipmap_t *m, void **newuser) { void *p = m->data; m->data = NULL; *newuser = p; return p; }
void *HWR_PS2_AllocData(size_t bytes, void **newuser) { void *p = malloc(bytes); *newuser = p; return p; }
const char *HWR_PS2_TexName(const GLMipmap_t *m) { (void)m; return "tex"; }
static u32 tex_want(const GLMipmap_t *m, int *visible) { (void)m; *visible = 1; return 0; }
static void gs_fill_all(int buf, int colour, int depth, u32 rgba) { (void)buf; (void)colour; (void)depth; (void)rgba; }
static u32 fb_pages(int stride, int rows, int psm32)
{
	return (u32)(stride / 64) * (u32)((rows + (psm32 ? 31 : 63)) / (psm32 ? 32 : 64));
}
int PS2HWD_ReadVram(void *dst, unsigned int blk, unsigned int tbw, int psm, int x, int y, int w, int h)
{
	(void)dst; (void)blk; (void)tbw; (void)psm; (void)x; (void)y; (void)w; (void)h;
	return -3;
}

#include "ps2_hw_xform.inc"
#include "ps2_hw_light.inc"
#include "ps2_hw_tex.inc"
static int neg;
#define PLAN_KEY_PCOL(c) (neg == 2 ? 0u : (u32)(c)) /* negative control 2: the plan cache ignores the polygon colour */
#include "ps2_hw_draw.inc"

/* ---- test framework ---- */
static int group_fail, total_fail, groups;
static const char *group_name;
#define NEGCOUNT 2

static void group_begin(const char *name)
{
	group_name = name;
	group_fail = 0;
}
static void group_end(const char *detail)
{
	groups++;
	total_fail += group_fail != 0;
	printf("HG %s %s %s\n", group_fail ? "FAIL" : "PASS", group_name, detail);
}
#define EXPECT(cond, ...) do { if (!(cond)) { if (group_fail < 6) { printf("HG info %s: ", group_name); printf(__VA_ARGS__); printf("\n"); } group_fail++; } } while (0)

static u64 rng_s = 0x9E3779B97F4A7C15ull;
static u32 rnd(void)
{
	rng_s ^= rng_s << 13;
	rng_s ^= rng_s >> 7;
	rng_s ^= rng_s << 17;
	return (u32)(rng_s >> 16);
}
static double rndf(double lo, double hi)
{
	return lo + (hi - lo) * ((double)(rnd() & 0xFFFFFF) / 16777216.0);
}

static void host_init(void)
{
	int i;

	free(H.rec);
	for (i = 0; i < NSCR; i++)
		free(H.scr_data[i]);
	memset(&H, 0, sizeof H);
	memset(blk_owner, 0, sizeof blk_owner);
	batch_phase = 0;
	OV.n = 0;
	ramp_tex = NOREC;
	memset(planc, 0, sizeof planc);
	planc_active = NULL;
	H.up = 1;
	col_lut_init();
	H.fbw = 640;
	H.fbh = 448;
	H.fb32 = 1;
	H.fbpsm = PSM_CT32;
	H.fb_addr[0] = 0;
	H.fb_addr[1] = 1146880u;
	H.z_addr = 2293760u;
	H.clut_base = (H.z_addr + 1146880u) / 256;
	H.pool_base = H.clut_base + ((CLUT_SLOTS * CLUT_BLOCKS + PAGE_BLOCKS - 1) / PAGE_BLOCKS) * PAGE_BLOCKS;
	H.pool_blocks = VRAM_BLOCKS - H.pool_base;
	H.free_n = 1;
	H.free_start[0] = H.pool_base;
	H.free_len[0] = H.pool_blocks;
	H.rec_free = NOREC;
	H.lru_head = H.lru_tail = NOREC;
	H.cur_tex = NOREC;
	for (i = 0; i < NSCR; i++)
		H.scr_rec[i] = NOREC;
	H.scr_w = 320;
	H.scr_h = 200;
	H.vw = 640;
	H.vh = 448;
	H.sx = 640.0f / 320.0f;
	H.sy = 448.0f / 200.0f;
	H.shader = -1;
	m_identity(H.mv);
	m_scale(H.mv, 1.0f, 1.0f, -1.0f);
	xf_set_2d(0.9f);
	viewport_set(0, 0, 320, 200);
	cap_reset();
}

static GLMipmap_t *mk_flat(int w, int h)
{
	GLMipmap_t *m = calloc(1, sizeof *m);
	u8 *d = malloc((size_t)w * h);
	int i;

	for (i = 0; i < w * h; i++)
		d[i] = (u8)(i * 7);
	m->format = GL_TEXFMT_P_8;
	m->width = (u16)w;
	m->height = (u16)h;
	m->flags = TF_WRAPXY;
	m->data = d;
	return m;
}

/* ---- the GIF stream as fans of GS vertices ---- */
typedef struct
{
	float s, t, q;
	int x, y, z, f;
	u32 col;
} gv_t;
typedef struct
{
	int n;
	u64 prim;
	gv_t v[40];
} gfan_t;

#define MAXFAN 4096
static gfan_t fans[2][MAXFAN];
static int nfans[2];

static int gv_cmp(const void *a, const void *b)
{
	const gv_t *x = a, *y = b;

	if (x->y != y->y)
		return x->y < y->y ? -1 : 1;
	if (x->x != y->x)
		return x->x < y->x ? -1 : 1;
	return x->z < y->z ? -1 : x->z > y->z;
}

static void decode_fans(int which)
{
	u32 at = 0;

	nfans[which] = 0;
	while (at < cap_n)
	{
		const u64 d0 = cap[at].d[0], d1 = cap[at].d[1];
		const u32 nloop = (u32)(d0 & 0x7FFF), flg = (u32)((d0 >> 58) & 3), nreg = (u32)((d0 >> 60) & 15) ? (u32)((d0 >> 60) & 15) : 16;
		gfan_t *f;

		if (flg == 0 && nreg == 1 && d1 == GIF_REGS_AD)
		{
			at += 1 + nloop;
			continue;
		}
		if (nfans[which] >= MAXFAN)
		{
			printf("HG FAIL fan_overflow\n");
			exit(2);
		}
		f = &fans[which][nfans[which]++];
		memset(f, 0, sizeof *f);
		if (flg == 1)
		{
			/* REGLIST: PRIM, then per vertex (ST, RGBAQ, XYZF), (ST, XYZ) or (XYZ); the descriptor nibbles say which (0 = PRIM, 2 = ST, 1 = RGBAQ, 4/5 = XYZF2/XYZ2) */
			const u64 *o = (const u64 *)&cap[at + 1];
			const u32 n1 = (u32)((d1 >> 4) & 15), n2 = (u32)((d1 >> 8) & 15);
			const u32 pervert = (n1 == 2 && n2 == 1) ? 3 : (n1 == 2 ? 2 : 1);
			u32 k = 0, i;

			f->prim = o[k++];
			f->n = (int)((nreg - 1) / pervert);
			for (i = 0; i < (u32)f->n; i++)
			{
				gv_t *v = &f->v[i];
				u64 xy;

				v->q = 1.0f;
				if (pervert >= 2)
				{
					const u64 st = o[k++];
					union { u32 u; float f; } a, b;

					a.u = (u32)st;
					b.u = (u32)(st >> 32);
					v->s = a.f;
					v->t = b.f;
				}
				if (pervert == 3)
				{
					const u64 rg = o[k++];
					union { u32 u; float f; } c;

					c.u = (u32)(rg >> 32);
					v->q = c.f;
					v->col = (u32)rg;
				}
				xy = o[k++];
				v->x = (int)(s16)(xy & 0xFFFF);
				v->y = (int)(s16)((xy >> 16) & 0xFFFF);
				v->z = (int)((xy >> 32) & 0xFFFFFF);
				v->f = (int)(xy >> 56);
			}
			at += 1 + (nreg + 1) / 2;
		}
		else
		{
			/* PACKED: ST/Q, RGBA, XYZF per vertex (3 regs) or ST/Q, XYZ... */
			u32 i;

			f->prim = (d0 >> 47) & 0x7FF;
			f->n = (int)nloop;
			for (i = 0; i < nloop; i++)
			{
				const qw_t *q = &cap[at + 1 + i * nreg];
				gv_t *v = &f->v[i];

				if (nreg == 3)
				{
					v->s = q[0].f[0];
					v->t = q[0].f[1];
					v->q = q[0].f[2];
					v->col = q[1].w[0] | (q[1].w[1] << 8) | (q[1].w[2] << 16) | (q[1].w[3] << 24);
					v->x = (int)(s16)q[2].w[0];
					v->y = (int)(s16)q[2].w[1];
					v->z = (int)(q[2].w[2] >> 4);
					v->f = (int)(q[2].w[3] >> 4);
				}
			}
			at += 1 + nloop * nreg;
		}
		qsort(f->v, (size_t)f->n, sizeof f->v[0], gv_cmp); /* a piece is a convex fan: compare the vertex sets, not their order */
	}
}

/* ---- group: water ---- */
static void water_setup(float yaw)
{
	FTransform t;
	FSurfaceInfo surf;
	GLMipmap_t *m = mk_flat(64, 64);
	int ri;

	host_init();
	memset(&t, 0, sizeof t);
	t.x = 0.0f;
	t.y = 0.0f;
	t.z = 41.0f;
	t.anglex = 0.0f;
	t.angley = yaw;
	t.scalex = t.scalez = 1.0f;
	t.scaley = 1.6f;
	t.fovxangle = 90.0f;
	xf_set_transform(&t);
	xf_update();
	H.pal_set = 0;
	cap_reset();
	ri = tex_upload(m);
	H.cur_tex = ri;
	(void)surf;
}

static int water_poly(FOutVector *vv, int n, double cx, double cz, double rx, double rz)
{
	double ang[8];
	int i, j;

	for (i = 0; i < n; i++)
		ang[i] = rndf(0, 6.2831853);
	for (i = 0; i < n; i++)
		for (j = i + 1; j < n; j++)
			if (ang[j] < ang[i])
			{
				double t = ang[i];

				ang[i] = ang[j];
				ang[j] = t;
			}
	for (i = 0; i < n; i++)
	{
		const double x = cx + rx * cos(ang[i]), z = cz + rz * sin(ang[i]);

		vv[i].x = (float)x;
		vv[i].y = 0.0f;
		vv[i].z = (float)z;
		vv[i].s = (float)(x / 64.0);
		vv[i].t = (float)(-z / 64.0);
	}
	return n;
}

static void test_water(void)
{
	const u32 flags = PF_Translucent | PF_Ripple | PF_Modulated | PF_ColorMapped | PF_Occlude;
	int trial, total_pieces[2] = {0, 0}, compared = 0, mismatches = 0, polys = 0, fallbacks = 0;
	double maxdxy = 0, maxdq = 0, maxdst = 0;

	group_begin("water");
	for (trial = 0; trial < 400; trial++)
	{
		FOutVector vv[8];
		FSurfaceInfo surf;
		int pass, n = 3 + (int)(rnd() % 4);
		double dist = rndf(60, 1300), cz = rndf(-dist, dist) * 0.6, rx = rndf(30, 260), rz = rndf(30, 260);
		int lvl = (int)(rnd() % 256);

		water_setup((float)rndf(0, 360));
		H.leveltime = (int)(rnd() % 100000);
		water_poly(vv, n, dist, cz, rx, rz);
		memset(&surf, 0, sizeof surf);
		surf.PolyColor.rgba = 0x80FFFFFFu;
		surf.LightInfo.light_level = lvl;
		surf.LightInfo.fade_start = 0;
		surf.LightInfo.fade_end = 31;
		H.shaders_on = 1;
		H.shader = 4;
		for (pass = 0; pass < 2; pass++)
		{
			ps2hwd_dbg_flags = pass == 0 ? HWDBG_OLDWATER | HWDBG_WATERPOL : HWDBG_WATERPOL;
			ps2hwd_dbg_flags |= HWDBG_NOPLANC;
			if (pass == 1 && neg == 1)
				H.leveltime += 7; /* negative control 1: the sweep ripples with another time */
			cap_reset();
			H.gsr.valid = 0;
			if (!begin_draw(flags, &surf))
			{
				EXPECT(0, "begin_draw refused");
				continue;
			}
			EXPECT(P.water, "plan is not a water plan");
			emit_fan(vv, NULL, n, NULL);
			decode_fans(pass);
			total_pieces[pass] += nfans[pass];
		}
		ps2hwd_dbg_flags = 0;
		polys++;
		if (nfans[0] == 0 && nfans[1] == 0)
			continue;
		if (nfans[0] != nfans[1])
		{
			/* degenerate slivers may be dropped by one path only: count them, the tolerance below decides */
			mismatches += abs(nfans[0] - nfans[1]);
			if (abs(nfans[0] - nfans[1]) > 2)
				EXPECT(0, "trial %d: %d pieces by the old path, %d by the sweep", trial, nfans[0], nfans[1]);
			fallbacks++;
			continue;
		}
		{
			int i, k;

			for (i = 0; i < nfans[0]; i++)
			{
				const gfan_t *a = &fans[0][i], *b = &fans[1][i];

				if (a->n != b->n)
				{
					EXPECT(0, "trial %d piece %d: %d vertices old, %d new", trial, i, a->n, b->n);
					continue;
				}
				for (k = 0; k < a->n; k++)
				{
					double dxy = fmax(abs(a->v[k].x - b->v[k].x), abs(a->v[k].y - b->v[k].y));

					maxdxy = fmax(maxdxy, dxy);
					maxdq = fmax(maxdq, fabs(a->v[k].q - b->v[k].q) / fmax(fabs(a->v[k].q), 1e-9));
					maxdst = fmax(maxdst, fmax(fabs(a->v[k].s - b->v[k].s), fabs(a->v[k].t - b->v[k].t)) / fmax(fabs(a->v[k].q), 1e-9));
					EXPECT(dxy <= 2.0, "trial %d piece %d vertex %d: xy differ by %.0f", trial, i, k, dxy);
					EXPECT(a->v[k].f == b->v[k].f, "trial %d piece %d vertex %d: fog %d vs %d", trial, i, k, a->v[k].f, b->v[k].f);
					EXPECT(abs(a->v[k].z - b->v[k].z) <= 64, "trial %d piece %d vertex %d: z %d vs %d", trial, i, k, a->v[k].z, b->v[k].z);
					EXPECT(fabs(a->v[k].s - b->v[k].s) <= 2e-3 * fmax(1.0, fabs(a->v[k].s)) + 1e-4 && fabs(a->v[k].t - b->v[k].t) <= 2e-3 * fmax(1.0, fabs(a->v[k].t)) + 1e-4,
						"trial %d piece %d vertex %d: st %.5f,%.5f vs %.5f,%.5f", trial, i, k, a->v[k].s, a->v[k].t, b->v[k].s, b->v[k].t);
				}
				compared++;
			}
		}
	}
	{
		char d[200];

		snprintf(d, sizeof d, "%d polygons, %d/%d pieces (old/sweep), %d compared (max xy %.0f LSB, st %.2g), %d count differences", polys, total_pieces[0], total_pieces[1], compared, maxdxy, maxdst,
			mismatches);
		group_end(d);
	}
	(void)fallbacks;
	(void)maxdq;
}

/* ---- group: plan cache ---- */
typedef struct
{
	u32 flags;
	FSurfaceInfo surf;
	int tex; /* index into the textures, -1 = none */
	int shader, shaders_on;
	int nv;
	FOutVector v[6];
	int world; /* 1 = a polygon in the 3D view, 0 = screen space (HUD) */
} drawcase_t;

#define NCASES 600
static drawcase_t cases[NCASES];

static void make_cases(void)
{
	static const u32 blends[] = {PF_Masked, PF_Translucent, PF_Additive, PF_Subtractive, PF_ReverseSubtract, PF_Environment, PF_Multiplicative, PF_Fog, PF_Translucent | PF_Ripple};
	static const u32 extras[] = {0, PF_Occlude, PF_NoDepthTest, PF_Decal, PF_Occlude | PF_ColorMapped, PF_ColorMapped, PF_Occlude | PF_ColorMapped | PF_Decal, PF_ForceWrapX | PF_RemoveYWrap};
	static const u32 pcols[] = {0xFFFFFFFFu, 0xA0FFFFFFu, 0x80FF8080u};
	drawcase_t tpl[6];
	int i, k, cur = 0;

	/* six plans the way the engine's sprite/shadow loop alternates between them, plus random one-off draws */
	for (i = 0; i < 6; i++)
	{
		drawcase_t *c = &tpl[i];

		memset(c, 0, sizeof *c);
		c->flags = blends[i % 3] | extras[(i / 3) * 4 + (i % 2)] | PF_Modulated;
		c->tex = i % 3 == 2 ? -1 : i % 2;
		if (c->tex < 0)
			c->flags |= PF_NoTexture;
		c->surf.PolyColor.rgba = pcols[i % 3];
		c->surf.LightInfo.light_level = 120 + 25 * i;
		c->surf.LightInfo.fade_end = 31;
		c->shaders_on = 1;
		c->shader = 1;
		c->world = 1;
	}
	tpl[3] = tpl[0]; /* the same plan in another colour: a cache that ignores the colour gets this wrong */
	tpl[3].surf.PolyColor.rgba = 0x80FF8080u;
	for (i = 0; i < NCASES; i++)
	{
		drawcase_t *c = &cases[i];

		if (rnd() % 100 < 22)
		{
			memset(c, 0, sizeof *c);
			c->flags = blends[rnd() % 9] | extras[rnd() % 8] | PF_Modulated;
			c->tex = (int)(rnd() % 5) - (int)(rnd() % 2 ? 0 : 1);
			if (c->tex < 0 || (c->flags & PF_Fog))
			{
				c->tex = -1;
				c->flags |= PF_NoTexture;
			}
			c->surf.PolyColor.rgba = rnd() | 0x01000000u;
			c->surf.TintColor.rgba = (rnd() % 4 == 0) ? (rnd() & 0xFFFFFFu) | ((rnd() % 200) << 24) : 0;
			c->surf.FadeColor.rgba = (rnd() % 3 == 0) ? (rnd() & 0xFFFFFFu) : 0;
			c->surf.LightInfo.light_level = (int)(rnd() % 256);
			c->surf.LightInfo.fade_start = (rnd() % 5 == 0) ? 3 : 0;
			c->surf.LightInfo.fade_end = (rnd() % 5 == 0) ? 20 : 31;
			c->shaders_on = (int)(rnd() % 8) != 0;
			c->shader = (int)(rnd() % 6);
			c->world = (int)(rnd() % 60) != 0;
		}
		else
		{
			if (rnd() % 100 < 45)
				cur = (int)(rnd() % 6);
			*c = tpl[cur];
		}
		c->nv = 3 + (int)(rnd() % 4);
		for (k = 0; k < c->nv; k++)
		{
			double a = 6.2831853 * k / c->nv;

			if (c->world)
			{
				c->v[k].x = (float)(300 + 80 * cos(a) + rndf(-5, 5));
				c->v[k].z = (float)(-60 + 80 * sin(a) + rndf(-5, 5));
				c->v[k].y = (float)(rndf(-20, 90));
			}
			else
			{
				c->v[k].x = (float)(0.2 + 0.1 * cos(a));
				c->v[k].y = (float)(-0.2 + 0.1 * sin(a));
				c->v[k].z = 0.0f;
			}
			c->v[k].s = (float)rndf(0, 3);
			c->v[k].t = (float)rndf(0, 3);
		}
	}
}

static void pc_run(int cache_on, u32 *hits)
{
	GLMipmap_t *tex[5];
	int rec[5], i;
	FTransform t;

	host_init();
	memset(&t, 0, sizeof t);
	t.z = 41.0f;
	t.scalex = t.scalez = 1.0f;
	t.scaley = 1.6f;
	t.fovxangle = 90.0f;
	for (i = 0; i < 5; i++)
	{
		tex[i] = mk_flat(i == 3 ? 128 : 64, i == 4 ? 32 : 64);
		if (i == 1)
		{
			int j;

			tex[i]->flags |= TF_CHROMAKEYED;
			for (j = 0; j < 64 * 64; j += 5)
				((u8 *)tex[i]->data)[j] = 255;
		}
		if (i == 2)
			tex[i]->flags = 0;
		rec[i] = tex_upload(tex[i]);
	}
	ps2hwd_dbg_flags = cache_on ? 0 : HWDBG_NOPLANC;
	G.plan_hit = G.plan_miss = 0;
	cap_reset();
	H.gsr.valid = 0;
	for (i = 0; i < NCASES; i++)
	{
		drawcase_t *c = &cases[i];

		if (i == 0 || c->world != cases[i - 1].world)
		{
			if (c->world)
				xf_set_transform(&t);
			else
				xf_set_2d(0.9f);
			viewport_set(0, 0, 320, 200);
		}
		H.shaders_on = c->shaders_on;
		H.shader = c->shader;
		H.cur_tex = c->tex >= 0 ? rec[c->tex] : NOREC;
		H.cur_missing = 0;
		if (i % 97 == 50)
			H.mat_dirty = 1; /* the view changes now and then */
		if (begin_draw(c->flags, &c->surf))
			emit_fan(c->v, NULL, c->nv, NULL);
	}
	*hits = G.plan_hit;
	ps2hwd_dbg_flags = 0;
}

static void test_plancache(void)
{
	static qw_t *ref;
	u32 hits_off = 0, hits_on = 0, refn;
	int i, diff = 0;

	group_begin("plancache");
	make_cases();
	pc_run(0, &hits_off);
	refn = cap_n;
	ref = malloc(sizeof(qw_t) * (cap_n + 1));
	memcpy(ref, cap, sizeof(qw_t) * cap_n);
	pc_run(1, &hits_on);
	EXPECT(hits_off == 0, "the cache hit with -hwdbg NOPLANC");
	EXPECT(hits_on > 50, "the cache hit only %u times in %d draws", hits_on, NCASES);
	EXPECT(cap_n == refn, "%u quadwords with the cache, %u without", cap_n, refn);
	for (i = 0; i < (int)(cap_n < refn ? cap_n : refn); i++)
		if (memcmp(&cap[i], &ref[i], sizeof(qw_t)))
		{
			if (diff++ < 3)
				printf("HG info plancache: quadword %d differs\n", i);
		}
	EXPECT(diff == 0, "%d quadwords differ", diff);
	{
		char d[200];

		snprintf(d, sizeof d, "%d random draws (%u cache hits): the GIF stream (state writes and fans, %u quadwords) is bit-identical with and without the plan cache", NCASES, hits_on, refn);
		group_end(d);
	}
	free(ref);
}

int main(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++)
		if (!strncmp(argv[i], "neg=", 4))
			neg = atoi(argv[i] + 4);
	cap = malloc(sizeof(qw_t) * CAP_MAX);
	if (neg)
		printf("HG negctl %d expects %s\n", neg, neg == 1 ? "water" : "plancache");
	test_water();
	test_plancache();
	printf("HG negctl-count %d\n", NEGCOUNT);
	printf("HG RESULT %s: %d groups, %d failed\n", total_fail ? "FAILED" : "PASSED", groups, total_fail);
	printf("HG COMPLETE\n");
	return total_fail ? 1 : 0;
}
