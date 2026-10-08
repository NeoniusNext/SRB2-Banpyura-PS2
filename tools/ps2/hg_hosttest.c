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
int ps2hwp_skyview;
int ps2hwd_fx2; /* OPT11 round 2 (FX2): -hwfx bits (ps2_hw_fx2.h) */
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

static void imm_prepare(const FOutVector *verts, unsigned int n) { (void)verts; (void)n; } /* ps2_hw_plan.inc (HT planner): not part of the host test */
#include "ps2_hw_xform.inc"
#include "ps2_hw_light.inc"
int HWR_ShouldUsePaletteRendering(void) { return 0; } /* OPT11: the host test draws the GLSL (non palette) lighting */
#include "ps2_hw_pal.inc"
#include "ps2_hw_tex.inc"
static int neg;
#define PLAN_KEY_PCOL(c) (neg == 2 ? 0u : (u32)(c)) /* negative control 2: the plan cache ignores the polygon colour */
#include "ps2_hw_draw.inc"

/* ---- test framework ---- */
static int group_fail, total_fail, groups;
static const char *group_name;
#define NEGCOUNT 7

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
	if (!blk_owner_p) /* PS2-171: the driver's big work arrays are zone memory in the engine (hwbig_alloc), here calloc */
	{
		blk_owner_p = calloc(1, sizeof *blk_owner_p);
		ovq_p = calloc(1, sizeof *ovq_p);
		cutbuf_p = calloc(1, sizeof *cutbuf_p);
	}
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
	gv_t v[300]; /* PS2-HW-120: a water strip has up to 2 vertices per depth plane */
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
			if (f->n > 300)
			{
				printf("HG FAIL fan_too_long\n");
				exit(2);
			}
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
			if (f->n > 300)
			{
				printf("HG FAIL fan_too_long\n");
				exit(2);
			}
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
		if ((f->prim & 7) != PRIM_TRISTRIP) /* a strip keeps its order (its triangles are made of consecutive vertices) */
			qsort(f->v, (size_t)f->n, sizeof f->v[0], gv_cmp); /* a piece is a convex fan: compare the vertex sets, not their order */
	}
}

/* ---- group: water ---- */
static int wt_w = 64, wt_h = 64; /* the texture of water_setup (litclip: sizes that are not powers of two) */
static void water_setup(float yaw)
{
	FTransform t;
	FSurfaceInfo surf;
	GLMipmap_t *m = mk_flat(wt_w, wt_h);
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
		H.leveltime = (int)(rnd() % 100000); H.lt_frac = 1.0f;
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
			ps2hwd_dbg_flags = pass == 0 ? HWDBG_OLDWATER | HWDBG_WATERPOL : HWDBG_WATERPOL; ps2hwd_water_ab = pass == 0 ? 0 : HWDBG_WATERBANDS; /* PS2-HW-120: the OPT10 sweep is the A/B switch now */
			if (pass == 1 && neg == 1)
				H.leveltime += 60; /* negative control 1: the sweep ripples with another time */
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
		ps2hwd_dbg_flags = 0; ps2hwd_water_ab = 0;
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



/* ---- group: ripple (PS2-HW-120): the water polygon against the exact shader, GS interpolation included ---- */
/* The GLSL water shader: texel = tex(s - sin(a) * 0.025, t - cos(a) * 0.025), a = -pi * (z / 2 * 0.025) + (leveltime - 1 + rendertimefrac) / 35 * 2 (PS2-HW-125: seconds, not tics), z = eye depth of the fragment
 * (zfrag). The test draws random convex polygons with the driver (emit_fan with the water plan), reads the GIF fans back, and for random points
 * inside the polygon interpolates S, T, Q of the piece that contains the point the way the GS does (screen space, then S/Q) and compares the texture
 * coordinate with the shader's: the error in texels, and in pixels (texels * F / z, the size of a texel on the screen at the depth of the point). */
static int rp_cmp_ang(const void *a, const void *b)
{
	const double *x = a, *y = b;

	return x[2] < y[2] ? -1 : x[2] > y[2];
}

typedef struct
{
	double maxtx, maxpx, sumpx;
	long n, miss, over, over2;
	long hist[8]; /* samples with a pixel error in [0, 0.1), [0.1, 0.2), ..., >= 0.7 */
} rp_stat_t;

/* the displacement on the screen (pixels) of a floor point (wx, wz) that moves by (dx, dz) world units */
static double rp_screen_err(double wx, double wz, double dx, double dz)
{
	FOutVector a, b, c;
	cv_t ca, cb, cc;
	double ax, ay, bx, by, cx, cy, j00, j01, j10, j11, ex, ey;

	memset(&a, 0, sizeof a);
	a.x = (float)wx;
	a.z = (float)wz;
	b = a;
	b.x += 1.0f;
	c = a;
	c.z += 1.0f;
	xform_vertex(&a, NULL, &ca);
	xform_vertex(&b, NULL, &cb);
	xform_vertex(&c, NULL, &cc);
	ax = ca.x / ca.w * P.kx;
	ay = ca.y / ca.w * P.ky;
	bx = cb.x / cb.w * P.kx;
	by = cb.y / cb.w * P.ky;
	cx = cc.x / cc.w * P.kx;
	cy = cc.y / cc.w * P.ky;
	j00 = bx - ax; /* d sx / d x */
	j10 = by - ay;
	j01 = cx - ax; /* d sx / d z */
	j11 = cy - ay;
	ex = (j00 * dx + j01 * dz) / 16.0;
	ey = (j10 * dx + j11 * dz) / 16.0;
	return sqrt(ex * ex + ey * ey);
}

static int rp_packdbg, rp_dbg, rp_zero, rp_f, rp_pn, rp_nan, rp_packchk;
static long rp_planes[2], rp_polys[2];
static double rp_l0, rp_l1, rp_l2;
static u64 rp_rng;
static double rp_design;
static void test_ripple(void)
{
	const u32 flags = PF_Translucent | PF_Ripple | PF_Modulated | PF_ColorMapped | PF_Occlude;
	const int sizes[3] = {64, 256, 128};
	rp_stat_t st[2][3];
	int trial, mode, si, polys = 0;
	long pieces[2] = {0, 0};

	memset(st, 0, sizeof st);
	group_begin("ripple");
	for (trial = 0; trial < 300; trial++)
	{
		FOutVector vv[8];
		FSurfaceInfo surf;
		int n = 3 + (int)(rnd() % 4), lt;
		double dist = rndf(40, 1500), cz = rndf(-dist, dist) * 0.6, rx = rndf(30, 300), rz = rndf(30, 300);
		float yaw = (float)rndf(0, 360);
		int lvl = (int)(rnd() % 256);

		si = trial % 3;
		wt_w = wt_h = sizes[si];
		lt = (int)(rnd() % 100000);
		for (mode = 0; mode < 2; mode++)
		{
			double F, ox, oy;
			int s, nf;

			water_setup(yaw);
			H.leveltime = lt; H.lt_frac = 1.0f;
			if (mode == 0 && neg == 7)
				H.leveltime += 20; /* negative control 7: the ripple of another tic */
			{
				/* the polygon of water_poly, with the texture coordinates of this texture size */
				int i;

				if (mode == 0)
					rp_rng = rng_s;
				else
					rng_s = rp_rng; /* the same polygon for both paths */
				water_poly(vv, n, dist, cz, rx, rz);
				for (i = 0; i < n; i++)
				{
					vv[i].s = vv[i].x / (float)wt_w;
					vv[i].t = -vv[i].z / (float)wt_h;
				}
			}
			memset(&surf, 0, sizeof surf);
			surf.PolyColor.rgba = 0x80FFFFFFu;
			surf.LightInfo.light_level = lvl;
			surf.LightInfo.fade_start = 0;
			surf.LightInfo.fade_end = 31;
			H.shaders_on = 1;
			H.shader = 4;
			ps2hwd_dbg_flags = 0; ps2hwd_water_ab = mode == 0 ? (neg == 9 ? HWDBG_WATERNOGAP : 0) : HWDBG_WATERBANDS; /* mode 0: PS2-HW-120, mode 1: the OPT10 sweep */
			cap_reset();
			H.gsr.valid = 0;
			H.clut_loaded = 0;
			if (!begin_draw(neg == 8 ? (flags & ~(u32)PF_Ripple) : flags, &surf))
			{
				EXPECT(0, "begin_draw refused");
				continue;
			}
			EXPECT(neg == 8 || P.water, "plan is not a water plan");
			emit_fan(vv, NULL, n, NULL);
			decode_fans(0);
			nf = nfans[0];
			if (mode == 0 && neg != 8)
			{
				/* the strip packer (ws_pack) against put_vertex (-hwdbg 262144): the same packets, bit for bit */
				static qw_t chk[1 << 16];
				u32 n0 = cap_n;

				if (n0 <= (1u << 16))
				{
					memcpy(chk, cap, (size_t)n0 * sizeof(qw_t));
					ps2hwd_dbg_flags = 0; ps2hwd_water_ab = 4;
					cap_reset();
					H.gsr.valid = 0;
					H.clut_loaded = 0; /* the first run loaded the CLUT: the second would not (TEX0.CLD) */
					if (begin_draw(flags, &surf))
						emit_fan(vv, NULL, n, NULL);
					if (cap_n == n0 && memcmp(chk, cap, (size_t)n0 * sizeof(qw_t)) && rp_packdbg++ < 3)
					{
						u32 k;

						for (k = 0; k < n0; k++)
							if (memcmp(&chk[k], &cap[k], sizeof(qw_t)))
							{
								{
									u32 j;

									for (j = (k > 6 ? k - 6 : 0); j < k + 4 && j < n0; j++)
										printf("HG dbg   qw %u: %08x %08x %08x %08x | %08x %08x %08x %08x\n", (unsigned)j, chk[j].w[0], chk[j].w[1], chk[j].w[2], chk[j].w[3], cap[j].w[0], cap[j].w[1], cap[j].w[2], cap[j].w[3]);
								}
								printf("HG dbg qw %u: ws_pack %08x %08x %08x %08x | put_vertex %08x %08x %08x %08x\n", (unsigned)k, chk[k].w[0], chk[k].w[1], chk[k].w[2], chk[k].w[3], cap[k].w[0], cap[k].w[1], cap[k].w[2], cap[k].w[3]);
								break;
							}
					}
					EXPECT(cap_n == n0 && !memcmp(chk, cap, (size_t)n0 * sizeof(qw_t)), "trial %d: ws_pack and put_vertex make different packets (%u / %u quadwords)", trial, (unsigned)n0, (unsigned)cap_n);
					rp_packchk++;
					memcpy(cap, chk, (size_t)n0 * sizeof(qw_t));
					cap_n = n0;
					ps2hwd_dbg_flags = 0; ps2hwd_water_ab = 0;
				}
			}
			if (neg == 8 && trial == 1 && mode == 0)
			{
				int i, f;
				for (i = 0; i < n; i++)
				{
					FOutVector pt = vv[i];
					cv_t cv;
					xform_vertex(&pt, NULL, &cv);
					printf("HG dbg in %d: world %.1f %.1f s,t %.4f %.4f  clip w %.1f -> sx %.1f sy %.1f\n", i, vv[i].x, vv[i].z, vv[i].s, vv[i].t, cv.w, P.ox + cv.x / cv.w * P.kx, P.oy + cv.y / cv.w * P.ky);
				}
				for (f = 0; f < nf; f++)
					for (i = 0; i < fans[0][f].n; i++)
						printf("HG dbg out fan %d v %d: x %d y %d q %.5f s/q %.4f t/q %.4f\n", f, i, fans[0][f].v[i].x, fans[0][f].v[i].y, fans[0][f].v[i].q, fans[0][f].v[i].s / fans[0][f].v[i].q, fans[0][f].v[i].t / fans[0][f].v[i].q);
			}
			ps2hwd_dbg_flags = 0; ps2hwd_water_ab = 0;
			if (!nf)
				continue;
			if (mode == 0)
				polys++;
			pieces[mode] += nf;
			rp_planes[mode] += (long)G.wf_bands;
			rp_polys[mode] += (long)G.wf_polys;
			G.wf_bands = G.wf_polys = 0;
			F = 0.5 * H.vpw * H.proj[0];
			ox = P.ox;
			oy = P.oy;
			for (s = 0; s < 40; s++)
			{
				/* a random point of the polygon: a triangle of the fan v0, vi, vi+1 and barycentric weights */
				const int k = 1 + (int)(rnd() % (unsigned)(n - 2));
				double w0 = rndf(0, 1), w1 = rndf(0, 1), w2;
				double wx, wz, zeye, ex, ey, expu, expt, dsu, dst_, sxp, syp, bu = 0, bt = 0, bestq = 0;
				FOutVector pt;
				cv_t cv;
				int f, found = 0, i, j;

				if (w0 + w1 > 1.0)
				{
					w0 = 1.0 - w0;
					w1 = 1.0 - w1;
				}
				w2 = 1.0 - w0 - w1;
				wx = w0 * vv[0].x + w1 * vv[k].x + w2 * vv[k + 1].x;
				wz = w0 * vv[0].z + w1 * vv[k].z + w2 * vv[k + 1].z;
				memset(&pt, 0, sizeof pt);
				pt.x = (float)wx;
				pt.y = 0.0f;
				pt.z = (float)wz;
				xform_vertex(&pt, NULL, &cv);
				if (cv.w <= H.near_plane + 25.0f || fabsf(cv.x) > 0.95f * cv.w * (float)H.guard_x / 1024.0f || fabsf(cv.y) > 0.95f * cv.w * (float)H.guard_y / 1024.0f)
					continue; /* behind the near plane or outside the guard band (clipped away) */
				sxp = ox + cv.x / cv.w * P.kx;
				syp = oy + cv.y / cv.w * P.ky;

				zeye = (cv.w - H.near_plane) * (FAR_CLIPPING_PLANE / (FAR_CLIPPING_PLANE - H.near_plane));
				/* the shader's value (the amplitude fades out beyond the depth of RP_PX_FULL pixels: the same law as the driver's, so compare only to that depth) */
				{
					const rtab_t *rt = ripple_table(wt_w);
					double a = -3.14159265358979 * (zeye / 2.0 * 0.025) + ((double)(lt - 1) + 1.0) * (2.0 / 35.0), g = zeye <= rt->zfull ? 1.0 : zeye >= rt->zend ? 0.0 : (rt->zend - zeye) / (rt->zend - rt->zfull);

					const double amp = neg == 8 ? 0.0 : 0.025; /* negative control 8 (a debugging aid): the plan without ripple against the picture without it */

					expu = wx / wt_w - sin(a) * amp; /* the shader */
					expt = -wz / wt_h - cos(a) * amp;
					dsu = wx / wt_w - g * sin(a) * amp; /* the design: the amplitude fades out beyond zfull */
					dst_ = -wz / wt_h - g * cos(a) * amp;
				}
				/* the piece that contains the point: a convex fan (the vertices ordered by angle round the centroid) or a strip (consecutive vertices) */
				for (f = 0; f < nf && !found; f++)
				{
					const gfan_t *g = &fans[0][f];
					double pv[300][4], cx = 0, cy = 0;
					int tri[600][3], nt = 0;

					if (g->n < 3)
						continue;
					if ((g->prim & 7) == PRIM_TRISTRIP)
					{
						for (i = 0; i + 2 < g->n; i++)
						{
							tri[nt][0] = i;
							tri[nt][1] = i + 1;
							tri[nt][2] = i + 2;
							nt++;
						}
					}
					else
					{
						for (i = 0; i < g->n; i++)
						{
							pv[i][0] = g->v[i].x & 0xFFFF; /* decode_fans reads the 16 bit XY as signed: the unsigned value is the 12.4 screen position with the offset */
							pv[i][1] = g->v[i].y & 0xFFFF;
							cx += pv[i][0];
							cy += pv[i][1];
						}
						cx /= g->n;
						cy /= g->n;
						for (i = 0; i < g->n; i++)
						{
							pv[i][2] = atan2(pv[i][1] - cy, pv[i][0] - cx);
							pv[i][3] = (double)i; /* the index of the vertex travels with its angle */
						}
						qsort(pv, (size_t)g->n, sizeof pv[0], rp_cmp_ang);
						for (i = 1; i + 1 < g->n; i++)
						{
							tri[nt][0] = (int)pv[0][3];
							tri[nt][1] = (int)pv[i][3];
							tri[nt][2] = (int)pv[i + 1][3];
							nt++;
						}
					}
					for (i = 0; i < nt && !found; i++)
					{
						const gv_t *a = &g->v[tri[i][0]], *b = &g->v[tri[i][1]], *c = &g->v[tri[i][2]];
						const double ax = a->x & 0xFFFF, ay = a->y & 0xFFFF, bx = b->x & 0xFFFF, by = b->y & 0xFFFF, ccx = c->x & 0xFFFF, ccy = c->y & 0xFFFF;
						const double d = (bx - ax) * (ccy - ay) - (ccx - ax) * (by - ay);
						double l1, l2, l0;

						if (fabs(d) < 1e-9)
							continue;
						l1 = ((sxp - ax) * (ccy - ay) - (ccx - ax) * (syp - ay)) / d;
						l2 = ((bx - ax) * (syp - ay) - (sxp - ax) * (by - ay)) / d;
						l0 = 1.0 - l1 - l2;
						if (l0 < -0.02 || l1 < -0.02 || l2 < -0.02)
							continue;
						{
							const double S = l0 * a->s + l1 * b->s + l2 * c->s, T = l0 * a->t + l1 * b->t + l2 * c->t, Q = l0 * a->q + l1 * b->q + l2 * c->q;

							bu = S / Q;
							bt = T / Q;
							bestq = Q;
							found = 1;
							rp_f = f;
							rp_l0 = l0;
							rp_l1 = l1;
							rp_l2 = l2;
							rp_pn = g->n;
						}
					}
				}
				(void)j;
				(void)bestq;
				if (!found)
				{
					st[mode][si].miss++;
					continue;
				}
				if (zeye > ripple_table(wt_w)->zfull)
					continue; /* the fade-out zone: the amplitude is below 1.5 pixels and goes to 0.8 by design */
				ex = bu - expu;
				ey = bt - expt;
				ex -= floor(ex + 0.5); /* the whole repeats the driver removed */
				ey -= floor(ey + 0.5);
				{
					/* the visible error: the world displacement of the texel (ex, ey repeats = ex * w, -ey * h units on the floor) through the projection of the point */
					double px, tex = fmax(fabs(ex) * wt_w, fabs(ey) * wt_h);

					px = rp_screen_err(wx, wz, ex * wt_w, -ey * wt_h);
					rp_stat_t *r = &st[mode][si];

					if (px > 4.0 && mode == 0 && rp_dbg++ < 12)
						printf("HG dbg trial %d size %d n=%d: world %.1f,%.1f z %.1f exp u,t %.4f,%.4f got %.4f,%.4f err %.3f,%.3f tex px %.2f piece %d of %d (%d verts) bary %.3f %.3f %.3f\n", trial, wt_w, n, wx, wz, zeye, expu, expt, bu, bt, ex * wt_w, ey * wt_h, px, rp_f, nf, rp_pn, rp_l0, rp_l1, rp_l2);
					r->n++;
					r->maxtx = fmax(r->maxtx, tex);
					r->maxpx = fmax(r->maxpx, px);
					r->sumpx += px;
					if (!(px >= 0.0 && px < 1e9))
					{
						rp_nan++;
						px = 1e9;
					}
					r->hist[px >= 0.7 ? 7 : (int)(px * 10.0)]++;
					if (px > 0.6)
						r->over++;
					if (px > 2.0)
						r->over2++;
					if (mode == 0)
					{
						double dx = bu - dsu, dy = bt - dst_;

						dx -= floor(dx + 0.5);
						dy -= floor(dy + 0.5);
						rp_design = fmax(rp_design, rp_screen_err(wx, wz, dx * wt_w, -dy * wt_h));
					}
				}
			}
		}
	}
	{
		char d[400];
		long over_new = 0, n_new = 0, miss = 0;
		double worst = 0;

		for (si = 0; si < 3; si++)
		{
			over_new += st[0][si].over;
			n_new += st[0][si].n;
			miss += st[0][si].miss;
			worst = fmax(worst, st[0][si].maxpx);
			printf("HG info ripple %3d texel flat: new max error %.2f texel (%.2f px, mean %.3f px, %ld samples)   OPT10 bands: max %.2f texel (%.2f px, mean %.3f px)\n", sizes[si], st[0][si].maxtx,
				st[0][si].maxpx, st[0][si].sumpx / fmax(1.0, (double)st[0][si].n), st[0][si].n, st[1][si].maxtx, st[1][si].maxpx, st[1][si].sumpx / fmax(1.0, (double)st[1][si].n));
		}
		for (mode = 0; mode < 2; mode++)
		{
			long h[8] = {0};
			long tot = 0;
			int k;

			for (si = 0; si < 3; si++)
				for (k = 0; k < 8; k++)
				{
					h[k] += st[mode][si].hist[k];
					tot += st[mode][si].hist[k];
				}
			printf("HG info ripple %s: share of the samples by error in pixels: <0.1 %.1f%%  <0.2 %.1f%%  <0.3 %.1f%%  <0.4 %.1f%%  <0.5 %.1f%%  <0.6 %.1f%%  <0.7 %.1f%%  >=0.7 %.1f%%\n", mode ? "OPT10 bands" : "continuous  ",
				100.0 * h[0] / tot, 100.0 * h[1] / tot, 100.0 * h[2] / tot, 100.0 * h[3] / tot, 100.0 * h[4] / tot, 100.0 * h[5] / tot, 100.0 * h[6] / tot, 100.0 * h[7] / tot);
		}
		printf("HG info ripple: ripple planes per polygon: continuous %.1f (%ld polygons), OPT10 bands %.1f (%ld polygons)\n", (double)rp_planes[0] / fmax(1.0, (double)rp_polys[0]), rp_polys[0], (double)rp_planes[1] / fmax(1.0, (double)rp_polys[1]), rp_polys[1]);
		printf("HG info ripple: sample points in no piece: continuous %ld, OPT10 bands %ld\n", st[0][0].miss + st[0][1].miss + st[0][2].miss, st[1][0].miss + st[1][1].miss + st[1][2].miss);
		printf("HG info ripple: %d polygons drawn with ws_pack and with put_vertex: identical packets\n", rp_packchk);
		EXPECT(rp_nan == 0, "%d samples with a NaN / infinite error", rp_nan);
		EXPECT(n_new > 2000, "only %ld samples", n_new);
		{
			long o2 = 0;
			double mean = 0;

			for (si = 0; si < 3; si++)
			{
				o2 += st[0][si].over2;
				mean += st[0][si].sumpx;
			}
			mean /= fmax(1.0, (double)n_new);
			printf("HG info ripple: continuous: mean error %.3f px, %ld of %ld samples (%.2f %%) above 2 px, worst %.2f px\n", mean, o2, n_new, 100.0 * (double)o2 / fmax(1.0, (double)n_new), worst);
			EXPECT(mean < 0.6, "the mean error of the continuous ripple against the shader is %.2f px", mean);
			EXPECT(o2 * 100 < n_new, "%ld of %ld samples are more than 2 px off", o2, n_new);
		}
		EXPECT(miss * 10 < n_new + miss, "%ld of %ld sample points were in no piece", miss, n_new + miss);
		snprintf(d, sizeof d, "%d polygons, %ld/%ld pieces (new/OPT10), %ld sample points, worst error %.2f px against the shader (OPT10 bands: %.2f px)", polys, pieces[0], pieces[1], n_new,
			worst, fmax(st[1][0].maxpx, fmax(st[1][1].maxpx, st[1][2].maxpx)));
		group_end(d);
	}
}

/* ---- group: bands (the light staircase cut of ordinary walls and floors) ---- */
static void test_bands(void)
{
	const u32 flags = PF_Masked | PF_Modulated | PF_ColorMapped | PF_Occlude;
	int trial, total_pieces[2] = {0, 0}, compared = 0, polys = 0, countdiff = 0, cutpolys = 0;
	double maxdxy = 0, maxdst = 0;

	group_begin("bands");
	for (trial = 0; trial < 500; trial++)
	{
		FOutVector vv[8];
		FSurfaceInfo surf;
		int pass, n, k;
		double x0 = rndf(40, 700), z0 = rndf(-700, 700), x1 = rndf(40, 1800), z1 = rndf(-900, 900), y0 = rndf(-20, 60), y1 = rndf(80, 260);
		int lvl = (int)(rnd() % 256), wall = rnd() % 2;

		water_setup((float)rndf(-30, 30));
		H.leveltime = 0; H.lt_frac = 1.0f;
		memset(&surf, 0, sizeof surf);
		surf.PolyColor.rgba = 0xFFFFFFFFu;
		surf.LightInfo.light_level = lvl;
		surf.LightInfo.fade_start = (rnd() % 4 == 0) ? 3 : 0;
		surf.LightInfo.fade_end = (rnd() % 4 == 0) ? 22 : 31;
		H.shaders_on = 1;
		H.shader = wall ? 1 : 0; /* wall / floor light constants */
		if (wall)
		{
			n = 4;
			vv[0].x = (float)x0; vv[0].z = (float)z0; vv[0].y = (float)y0;
			vv[1].x = (float)x1; vv[1].z = (float)z1; vv[1].y = (float)y0;
			vv[2].x = (float)x1; vv[2].z = (float)z1; vv[2].y = (float)y1;
			vv[3].x = (float)x0; vv[3].z = (float)z0; vv[3].y = (float)y1;
		}
		else
		{
			n = 3 + (int)(rnd() % 4);
			water_poly(vv, n, x0 + 200, z0 * 0.5, rndf(60, 400), rndf(60, 600));
		}
		for (k = 0; k < n; k++)
		{
			vv[k].s = (float)(vv[k].x / 128.0);
			vv[k].t = (float)(vv[k].y / 128.0 - vv[k].z / 128.0);
		}
		for (pass = 0; pass < 2; pass++)
		{
			ps2hwd_dbg_flags = pass == 0 ? 0 : HWDBG_BANDSWEEP;
			if (pass == 1 && neg == 3)
				ps2hwd_dbg_flags |= 0; /* (negative control 3 mutates the expected class below) */
			cap_reset();
			H.gsr.valid = 0;
			if (!begin_draw(flags, &surf))
			{
				EXPECT(0, "begin_draw refused");
				continue;
			}
			emit_fan(vv, NULL, n, NULL);
			decode_fans(pass);
			total_pieces[pass] += nfans[pass];
		}
		ps2hwd_dbg_flags = 0;
		polys++;
		if (nfans[0] > 1)
			cutpolys++;
		if (nfans[0] != nfans[1])
		{
			countdiff += abs(nfans[0] - nfans[1]);
			EXPECT(abs(nfans[0] - nfans[1]) <= 2, "trial %d: %d pieces by cut_and_emit, %d by the sweep", trial, nfans[0], nfans[1]);
			continue;
		}
		{
			int i;

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
					maxdst = fmax(maxdst, fmax(fabs(a->v[k].s - b->v[k].s), fabs(a->v[k].t - b->v[k].t)) / fmax(fabs(a->v[k].q), 1e-9));
					EXPECT(dxy <= 2.0, "trial %d piece %d vertex %d: xy differ by %.0f", trial, i, k, dxy);
					EXPECT(a->v[k].f == b->v[k].f + (neg == 3 ? 1 : 0), "trial %d piece %d vertex %d: fog %d vs %d", trial, i, k, a->v[k].f, b->v[k].f);
					EXPECT(abs(a->v[k].z - b->v[k].z) <= 64, "trial %d piece %d vertex %d: z %d vs %d", trial, i, k, a->v[k].z, b->v[k].z);
					EXPECT(fabs(a->v[k].s - b->v[k].s) <= 2e-3 * fmax(1.0, fabs(a->v[k].s)) + 1e-4 && fabs(a->v[k].t - b->v[k].t) <= 2e-3 * fmax(1.0, fabs(a->v[k].t)) + 1e-4,
						"trial %d piece %d vertex %d: st differ", trial, i, k);
				}
				compared++;
			}
		}
	}
	{
		char d[220];

		snprintf(d, sizeof d, "%d polygons (%d cut into several pieces), %d/%d pieces (cut_and_emit / sweep), %d compared (max xy %.0f LSB, st %.2g), %d count differences", polys, cutpolys, total_pieces[0],
			total_pieces[1], compared, maxdxy, maxdst, countdiff);
		group_end(d);
	}
}

/* ---- group: litclip (PS2-HW-59: a clipped polygon of one darkness class takes the lit fast path instead of cut_and_emit) ---- */
static void test_litclip(void)
{
	const u32 flags = PF_Masked | PF_Modulated | PF_ColorMapped | PF_Occlude;
	int trial, polys = 0, lit = 0, litclipped = 0, compared = 0, cutpolys = 0, diffs = 0;

	group_begin("litclip");
	for (trial = 0; trial < 800; trial++)
	{
		FOutVector vv[8];
		FSurfaceInfo surf;
		int pass, n, k, wall = rnd() % 3 == 0;
		double cx = rndf(-900, 900), cz = rndf(-300, 1500), rx = rndf(20, 900), rz = rndf(20, 900);
		double x0 = rndf(-900, 900), z0 = rndf(-300, 1500), x1 = rndf(-900, 900), z1 = rndf(-300, 1500), y0 = rndf(-60, 60), y1 = rndf(80, 400);
		int lvl = (int)(rnd() % 256);
		u32 lit0, clip0;

		wt_w = trial % 3 == 0 ? 48 : 64; /* 48: not a power of two: the polygon is cut at the repeats (stage 0) */
		wt_h = trial % 4 == 0 ? 40 : 64;
		water_setup((float)rndf(-180, 180));
		H.leveltime = 0; H.lt_frac = 1.0f;
		memset(&surf, 0, sizeof surf);
		surf.PolyColor.rgba = 0xFFFFFFFFu;
		surf.LightInfo.light_level = lvl;
		surf.LightInfo.fade_start = (rnd() % 4 == 0) ? 3 : 0;
		surf.LightInfo.fade_end = (rnd() % 4 == 0) ? 22 : 31;
		H.shaders_on = 1;
		H.shader = wall ? 1 : 0;
		if (wall)
		{
			n = 4;
			vv[0].x = (float)x0; vv[0].z = (float)z0; vv[0].y = (float)y0;
			vv[1].x = (float)x1; vv[1].z = (float)z1; vv[1].y = (float)y0;
			vv[2].x = (float)x1; vv[2].z = (float)z1; vv[2].y = (float)y1;
			vv[3].x = (float)x0; vv[3].z = (float)z0; vv[3].y = (float)y1;
		}
		else
		{
			n = 3 + (int)(rnd() % 5);
			water_poly(vv, n, cx, cz, rx, rz);
		}
		{
			const double sc = trial % 5 == 0 ? 16.0 : 128.0; /* 16: more than UV_EXTENT texels: the polygon is cut at whole repeats even for a power of two texture */

			for (k = 0; k < n; k++)
			{
				vv[k].s = (float)(vv[k].x / sc);
				vv[k].t = (float)(vv[k].y / sc - vv[k].z / sc);
			}
		}
		lit0 = G.p_lit;
		clip0 = H.st.clipped;
		for (pass = 0; pass < 2; pass++)
		{
			ps2hwd_dbg_flags = pass == 0 ? 0 : 8192; /* 8192: the general path only */
			cap_reset();
			H.gsr.valid = 0;
			if (!begin_draw(flags, &surf))
			{
				EXPECT(0, "begin_draw refused");
				continue;
			}
			emit_fan(vv, NULL, n, NULL);
			decode_fans(pass);
			if (pass == 0)
			{
				const int l = (int)(G.p_lit - lit0), c = (int)(H.st.clipped - clip0);

				lit += l > 0;
				litclipped += l > 0 && c > 0;
			}
		}
		ps2hwd_dbg_flags = 0;
		polys++;
		if (nfans[0] > 1)
			cutpolys++;
		if (nfans[0] != nfans[1])
		{
			EXPECT(0, "trial %d: %d pieces by the lit path / cut_and_emit, %d by cut_and_emit", trial, nfans[0], nfans[1]);
			if (getenv("HGDBG"))
			{
				int q;

				printf("DBG trial %d wall %d n %d lvl %d litdelta %d clipdelta %d\n", trial, wall, n, lvl, (int)(G.p_lit - lit0), (int)(H.st.clipped - clip0));
				for (q = 0; q < n; q++)
					printf("  v %g %g %g\n", (double)vv[q].x, (double)vv[q].y, (double)vv[q].z);
				for (q = 0; q < nfans[0]; q++)
					printf("  new fan %d: %d verts w/ q %g\n", q, fans[0][q].n, (double)fans[0][q].v[0].q);
				for (q = 0; q < nfans[1]; q++)
					printf("  old fan %d: %d verts\n", q, fans[1][q].n);
			}
			diffs++;
			continue;
		}
		for (k = 0; k < nfans[0]; k++)
		{
			const gfan_t *a = &fans[0][k], *b = &fans[1][k];
			int i;

			if (a->n != b->n)
			{
				EXPECT(0, "trial %d piece %d: %d vertices vs %d", trial, k, a->n, b->n);
				diffs++;
				continue;
			}
			for (i = 0; i < a->n; i++)
			{
				const int same = a->v[i].x == b->v[i].x && a->v[i].y == b->v[i].y && a->v[i].z == b->v[i].z && a->v[i].f == b->v[i].f + (neg == 5 ? 1 : 0) && a->v[i].col == b->v[i].col
					&& memcmp(&a->v[i].s, &b->v[i].s, sizeof(float)) == 0 && memcmp(&a->v[i].t, &b->v[i].t, sizeof(float)) == 0 && memcmp(&a->v[i].q, &b->v[i].q, sizeof(float)) == 0;

				EXPECT(same, "trial %d piece %d vertex %d: xy %d,%d vs %d,%d z %d vs %d fog %d vs %d st %g,%g vs %g,%g q %g vs %g", trial, k, i, a->v[i].x, a->v[i].y, b->v[i].x, b->v[i].y, a->v[i].z, b->v[i].z,
					a->v[i].f, b->v[i].f, (double)a->v[i].s, (double)a->v[i].t, (double)b->v[i].s, (double)b->v[i].t, (double)a->v[i].q, (double)b->v[i].q);
				diffs += !same;
			}
			compared++;
		}
	}
	wt_w = wt_h = 64;
	EXPECT(litclipped >= 30, "only %d polygons were clipped and drawn by the lit path: the test does not reach it", litclipped);
	{
		char d[220];

		snprintf(d, sizeof d, "%d polygons (%d by the lit path, %d of them clipped, %d cut into several pieces), %d pieces compared: bit-identical to cut_and_emit (%d differences)", polys, lit, litclipped, cutpolys,
			compared, diffs);
		group_end(d);
	}
}

/* ---- group: sort (PS2-HW-59: HWR_GroupSort32 against the stable radix sort) ---- */
#include "hw_sort.h"
static void test_sort(void)
{
	static unsigned int keys[8192], idx[8192], k2[8192], i2[8192], tk[8192], ti[8192], rk[8192], ri[8192];
	int trial, groupsorts = 0, fallbacks = 0, bad = 0;

	group_begin("sort");
	for (trial = 0; trial < 400; trial++)
	{
		const unsigned int n = trial % 11 == 0 ? (unsigned int)(rnd() % 4) : trial % 7 == 0 ? 8192 : 1 + (rnd() % 3000);
		const unsigned int distinct = trial % 13 == 0 ? 1 : trial % 5 == 0 ? 900 + (rnd() % 3000) : 1 + (rnd() % 400);
		unsigned int pool[4096], i;
		int r;

		for (i = 0; i < distinct && i < 4096; i++)
			pool[i] = (trial % 3 == 0) ? (rnd() & 0x7FFFFFFFu) ^ 0x80000000u : (((rnd() % 200) << 16) | (rnd() & 0xFFFFu)) ^ 0x80000000u;
		for (i = 0; i < n; i++)
		{
			keys[i] = pool[rnd() % (distinct < 4096 ? distinct : 4096)];
			idx[i] = i;
			k2[i] = keys[i];
			i2[i] = i;
		}
		r = HWR_GroupSort32(keys, idx, tk, ti, n);
		if (r < 0)
		{
			fallbacks++;
			continue;
		}
		groupsorts++;
		/* reference: the radix sort of the same input (its result is in k2/i2, or in rk/ri) */
		{
			const unsigned int *ref;

			if (HWR_RadixSort32(k2, i2, rk, ri, n))
				ref = i2;
			else
				ref = ri;
			if (neg == 6 && n > 3)
			{
				const unsigned int t = ti[0];

				ti[0] = ti[n - 1];
				ti[n - 1] = t; /* negative control 6: a wrong order */
			}
			for (i = 0; i < n; i++)
				if (ti[i] != ref[i])
				{
					bad++;
					EXPECT(0, "trial %d (n %u, %u distinct): position %u: group sort %u, radix %u", trial, n, distinct, i, ti[i], ref[i]);
					break;
				}
		}
	}
	EXPECT(groupsorts >= 250 && fallbacks >= 20, "%d sorts by groups, %d fallbacks: the test does not reach both paths", groupsorts, fallbacks);
	{
		char d[200];

		snprintf(d, sizeof d, "%d random inputs sorted by groups (%d with too many distinct keys: left to the radix sort): the order is the stable radix sort's (%d differences)", groupsorts, fallbacks, bad);
		group_end(d);
	}
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
	ps2hwd_dbg_flags = cache_on ? HWDBG_PLANC : 0;
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
	EXPECT(hits_off == 0, "the cache hit without -hwdbg PLANC");
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

/* ---- group: clip (clip_poly with the per-plane distance arrays against the reference of before PS2-HW-54) ---- */
static int clip_poly_ref(cv_t *a, cv_t *b, int n, int na, int mask, cv_t **res)
{
	cv_t *in = a, *out = b, *t;
	float gx = (float)H.guard_x * (1.0f / 1024.0f), gy = (float)H.guard_y * (1.0f / 1024.0f);
	int p, i, k;

	for (p = 0; p < 6 && n > 0; p++)
	{
		int m = 0;

		if (!(mask & (1 << p)))
			continue;
		for (i = 0; i < n; i++)
		{
			const cv_t *v0 = &in[i], *v1 = &in[i + 1 == n ? 0 : i + 1];
			float d0 = plane_dist(v0, p, gx, gy), d1 = plane_dist(v1, p, gx, gy);

			if (d0 >= 0.0f)
				out[m++] = *v0;
			if ((d0 >= 0.0f) != (d1 >= 0.0f))
			{
				float tt = d0 / (d0 - d1);
				cv_t *o = &out[m++];

				o->x = v0->x + (v1->x - v0->x) * tt;
				o->y = v0->y + (v1->y - v0->y) * tt;
				o->z = v0->z + (v1->z - v0->z) * tt;
				o->w = v0->w + (v1->w - v0->w) * tt;
				for (k = 0; k < na; k++)
					o->a[k] = v0->a[k] + (v1->a[k] - v0->a[k]) * tt;
			}
		}
		n = m;
		t = in;
		in = out;
		out = t;
	}
	*res = in;
	return n;
}

static void test_clip(void)
{
	cv_t a1[PS2HWD_MAXPOLY + PS2HWD_CLIPEXTRA], b1[PS2HWD_MAXPOLY + PS2HWD_CLIPEXTRA], a2[PS2HWD_MAXPOLY + PS2HWD_CLIPEXTRA], b2[PS2HWD_MAXPOLY + PS2HWD_CLIPEXTRA];
	int it, nclip = 0, nout = 0, verts = 0;

	group_begin("clip");
	host_init();
	for (it = 0; it < 6000; it++)
	{
		const int n = 3 + (int)(rnd() % 10), na = (rnd() & 1) ? 6 : 2, mask = (int)(rnd() & 63);
		cv_t *r1, *r2;
		int i, k, c1, c2, bad = 0;

		for (i = 0; i < n; i++)
		{
			a1[i].x = (float)rndf(-4.0, 4.0);
			a1[i].y = (float)rndf(-4.0, 4.0);
			a1[i].z = (float)rndf(-3.0, 3.0);
			a1[i].w = (float)rndf(-1.0, 4.0);
			for (k = 0; k < 6; k++)
				a1[i].a[k] = (float)rndf(-2.0, 2.0);
		}
		memcpy(a2, a1, sizeof(cv_t) * (size_t)n);
		c1 = clip_poly_ref(a1, b1, n, na, mask, &r1);
		c2 = clip_poly(a2, b2, n, na, neg == 4 ? (mask | 1) : mask, &r2);
		if (c1 != c2)
			bad++;
		else
			for (i = 0; i < c1; i++)
			{
				if (r1[i].x != r2[i].x || r1[i].y != r2[i].y || r1[i].z != r2[i].z || r1[i].w != r2[i].w)
					bad++;
				for (k = 0; k < na; k++)
					if (r1[i].a[k] != r2[i].a[k])
						bad++;
			}
		EXPECT(!bad, "polygon %d (n %d, na %d, mask %02x): %d vertices against %d, %d values differ", it, n, na, mask, c2, c1, bad);
		nclip += mask != 0 && c1 != n;
		nout += c1 == 0;
		verts += c1;
	}
	{
		/* hw_floorf (PS2-HW-53) against the library floor: fractions, negatives, exact integers, huge values, the edges of the int range */
		int fl_bad = 0;
		long fl_n;
		float edge[] = {0.0f, -0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 4.0f, -4.0f, 4.0001f, -4.0001f, 2147482880.0f, -2147482880.0f, 2147483648.0f, -2147483648.0f, 3e9f, -3e9f, 1e30f, -1e30f, 8388607.5f, -8388607.5f, 16777216.0f};

		for (fl_n = 0; fl_n < (long)(sizeof edge / sizeof edge[0]); fl_n++)
			fl_bad += hw_floorf(edge[fl_n]) != __builtin_floorf(edge[fl_n]);
		for (fl_n = 0; fl_n < 2000000; fl_n++)
		{
			const float x = (float)rndf(-70000.0, 70000.0) * ((fl_n & 3) == 0 ? 0.001f : (fl_n & 3) == 1 ? 1.0f : (fl_n & 3) == 2 ? 30000.0f : 1.0f);
			const float xi = (float)(int)(x * 0.5f); /* integers too */

			fl_bad += hw_floorf(x) != __builtin_floorf(x);
			fl_bad += hw_floorf(xi) != __builtin_floorf(xi);
		}
		EXPECT(!fl_bad, "hw_floorf differs from floorf in %d values", fl_bad);
	}	{
		/* lf_class_w (PS2-HW-43b: the light class with a memory of proven depth ranges) against lf_class(zfrag_of_w()) for random walks and jumps of w */
		static const int lights[] = {0, 40, 96, 128, 160, 200, 255};
		int li, k, cl_bad = 0, th_bad = 0;
		long cl_n = 0, cl_slow = 0, th_n = 0;

		for (li = 0; li < 7; li++)
		{
			float w = 8.0f;

			light_setup(&P.rs.lp, lights[li], li & 1, 0, 31);
			lit_fast_plan();
			for (k = 0; k < 60000; k++)
			{
				const u32 r = rnd();

				if ((r & 7) == 0)
					w = (float)rndf(1.0, 4000.0);
				else
					w += (float)rndf(-6.0, 6.0);
				if (w < 0.5f)
					w = 0.5f;
				if (lf_class_w(w) != lf_class(zfrag_of_w(w)))
					cl_bad++;
				cl_n++;
			}
			cl_slow += __builtin_popcountll(LF.known);
			/* lf_thresholds (memory of the step depths) against doom_thresholds for random depth pairs */
			for (k = 0; k < 20000; k++)
			{
				const float wa = (float)rndf(0.5, 4000.0), wb = wa + (float)rndf(0.0, k % 4 == 0 ? 3000.0 : 60.0);
				const float za = zfrag_of_w(wa), zb = zfrag_of_w(wb);
				const int c0 = lf_class_w(wa), c1 = lf_class_w(wb);
				float th1[MAXCUTS], th2[MAXCUTS];
				int cls1[MAXCUTS], cls2[MAXCUTS], cls0, n1, n2, q;

				n1 = doom_thresholds(&P.rs.lp, za, zb, th1, cls1, MAXCUTS, &cls0);
				th_n++;
				if (cls0 != c0 || (c0 == c1 && n1 != 0))
				{
					th_bad++;
					continue;
				}
				if (c0 == c1)
					continue;
				n2 = lf_thresholds(c0, c1, za, zb, th2, cls2, MAXCUTS);
				if (n1 != n2 + (neg == 4 ? 1 : 0))
					th_bad++;
				else
					for (q = 0; q < n1; q++)
						th_bad += (th1[q] != th2[q]) || cls1[q] != cls2[q];
			}
		}
		EXPECT(!th_bad, "lf_thresholds differs from doom_thresholds in %d of %ld depth pairs", th_bad, th_n);
		EXPECT(!cl_bad, "lf_class_w differs from lf_class in %d of %ld depths", cl_bad, cl_n);
		(void)cl_slow;
	}
	{
		char d[200];

		snprintf(d, sizeof d, "6000 random polygons x plane masks (%d changed by clipping, %d clipped away, %d vertices out): bit-identical to the one-plane-at-a-time reference", nclip, nout, verts);
		group_end(d);
	}
}

int main(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++)
		if (!strncmp(argv[i], "neg=", 4))
			neg = atoi(argv[i] + 4);
	cap = malloc(sizeof(qw_t) * CAP_MAX);
	if (neg)
		printf("HG negctl %d expects %s\n", neg, neg == 1 ? "water" : neg == 2 ? "plancache" : neg == 4 ? "clip" : neg == 5 ? "litclip" : neg == 6 ? "sort" : neg == 7 || neg == 8 ? "ripple" : "bands");
	test_water();
	test_ripple();
	test_bands();
	test_litclip();
	test_sort();
	test_plancache();
	test_clip();
	printf("HG negctl-count %d\n", NEGCOUNT);
	printf("HG RESULT %s: %d groups, %d failed\n", total_fail ? "FAILED" : "PASSED", groups, total_fail);
	printf("HG COMPLETE\n");
	return total_fail ? 1 : 0;
}
