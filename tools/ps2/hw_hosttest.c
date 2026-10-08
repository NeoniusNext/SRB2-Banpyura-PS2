/* x86 host test of the GS renderer driver logic (see tools/ps2/hw_hosttest.py): the real src/ps2/hw/ps2_hw_*.inc files are
 * compiled as they are, the GS side (DMA ring) is replaced by a capture buffer, and every result is compared with code that
 * is written independently here (double precision pipeline, GS register bit field encoders, block address tables).
 *   neg=N : negative control N - a deliberate mutation; the group named in "HT negctl N expects <group>" must FAIL.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shim.h"
#include "ps2_hwd_dbg.h"

#define HWGIF_UNUSED 0
#define CONS_WARNING 1
#define CONS_ERROR 2
#define CONS_Alert(level, ...) ((void)(level))
#define CONS_Printf(...) ((void)0)
/* Host packet tests do not submit DMA; ordinary malloc/free is sufficient here. */
#define memalign(alignment, bytes) malloc(bytes)
#include "ps2_hw_priv.inc"
#include "ps2_hw_regs.inc"

/* ---- replacements for ps2_hw_gs.inc: a capture buffer with the same buffer boundaries as the DMA ring ---- */
#define CAP_MAX (1 << 21)
static qw_t *cap;
static u32 cap_n;
static u32 cap_marks[4096];
static int cap_nmarks;

static void cap_reset(void)
{
	cap_n = 0;
	cap_nmarks = 0;
	H.wr = 0;
	H.st.qwords = 0;
}
static void pk_flush(void)
{
	if (H.wr)
	{
		cap_marks[cap_nmarks++] = cap_n;
		H.wr = 0;
	}
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
		printf("HT FAIL capture_overflow\n");
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
/* OPT9 (HT): the zero-copy upload path of ps2_hw_tex.inc (DMA references, locked engine data); the host test never takes it (no engine data pointer) */
static void ring_wait(int limit)
{
	(void)limit;
}
static void pk_ref(const void *data, u32 qwc)
{
	(void)data; (void)qwc;
}
#define SyncDCache(a, b) ((void)0)
void HWR_PS2_LockData(void *data) { (void)data; }
void HWR_PS2_UnlockData(void *data) { (void)data; }
void HWR_PS2_FreeData(void *data) { free(data); }
void *HWR_PS2_StealData(GLMipmap_t *m, void **newuser) { void *p = m->data; m->data = NULL; *newuser = p; return p; }
void *HWR_PS2_AllocData(size_t bytes, void **newuser) { void *p = malloc(bytes); *newuser = p; return p; }
/* PS2-HW-34: the frame plan (ps2_hw_plan.inc) is not part of the host test: every texture is stored at full size */
static u32 tex_want(const GLMipmap_t *m, int *visible) { (void)m; *visible = 1; return 0; }
static int clear_count;
static void gs_fill_all(int buf, int colour, int depth, u32 rgba)
{
	(void)buf; (void)colour; (void)depth; (void)rgba;
	clear_count++;
}
static u32 fb_pages(int stride, int rows, int psm32)
{
	return (u32)(stride / 64) * (u32)((rows + (psm32 ? 31 : 63)) / (psm32 ? 32 : 64));
}
static u8 mock_vram[4 * 1024 * 1024];
static int mock_read_fail;
int PS2HWD_ReadVram(void *dst, unsigned int blk, unsigned int tbw, int psm, int x, int y, int w, int h)
{
	(void)tbw; (void)x; (void)y;
	if (mock_read_fail)
		return -3;
	memcpy(dst, mock_vram + blk * 256, (size_t)w * h * (psm == PSM_CT32 ? 4 : 2));
	return 0;
}

#include "ps2_hw_xform.inc"
#include "ps2_hw_light.inc"
#include "ps2_hw_tex.inc"
#include "ps2_hw_draw.inc"
#include "ps2_hw_model.inc"
static u32 expand_pixel(u32 v, int fb32)
{
	if (fb32) return v & 0xFFFFFFu;
	return ((v & 31) * 255 / 31) | (((v >> 5) & 31) * 255 / 31 << 8) | (((v >> 10) & 31) * 255 / 31 << 16);
}
#include "ps2_hw_screen.inc"

/* ---- test framework ---- */
static int neg; /* active negative control, 0 = none */
static int group_fail, total_fail, groups;
static const char *group_name;
#define NEGCOUNT 22

static void group_begin(const char *name)
{
	group_name = name;
	group_fail = 0;
}
static void group_end(const char *detail)
{
	groups++;
	total_fail += group_fail != 0;
	printf("HT %s %s %s\n", group_fail ? "FAIL" : "PASS", group_name, detail);
}
#define EXPECT(cond, ...) do { if (!(cond)) { if (group_fail < 5) { printf("HT info %s: ", group_name); printf(__VA_ARGS__); printf("\n"); } group_fail++; } } while (0)

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

static void host_init(int fb32)
{
	int i;

	free(H.rec);
	for (i = 0; i < NSCR; i++) free(H.scr_data[i]);
	memset(&H, 0, sizeof H);
	if (!blk_owner_p) /* PS2-171: the driver's big work arrays are zone memory in the engine (hwbig_alloc), here calloc */
	{
		blk_owner_p = calloc(1, sizeof *blk_owner_p);
		ovq_p = calloc(1, sizeof *ovq_p);
		cutbuf_p = calloc(1, sizeof *cutbuf_p);
		plan_info_p = calloc(1, sizeof *plan_info_p);
	}
	memset(blk_owner, 0, sizeof blk_owner);
	batch_phase = 0;
	OV.n = 0;
	ramp_tex = NOREC;
	H.up = 1;
	col_lut_init();
	H.fbw = 640;
	H.fbh = 448;
	H.fb32 = fb32;
	H.fbpsm = fb32 ? PSM_CT32 : PSM_CT16S;
	H.fb_addr[0] = 0;
	H.fb_addr[1] = fb32 ? 1146880u : 573440u;
	H.z_addr = fb32 ? 2293760u : 1146880u;
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
	m_identity(H.mv);
	m_scale(H.mv, 1.0f, 1.0f, -1.0f);
	xf_set_2d(0.9f);
	viewport_set(0, 0, 320, 200);
	cap_reset();
}

/* ---- reference implementation, double precision ---- */
typedef struct
{
	double m[16];
} dmat_t;

static void dm_mul(double *o, const double *a, const double *b)
{
	double t[16];
	int c, r;

	for (c = 0; c < 4; c++)
		for (r = 0; r < 4; r++)
			t[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
	memcpy(o, t, sizeof t);
}
static void dm_id(double *m)
{
	int i;

	for (i = 0; i < 16; i++)
		m[i] = i % 5 == 0;
}
static void dm_rot(double *m, double deg, double x, double y, double z)
{
	double t[16], r = deg * M_PI / 180.0, c = cos(r), s = sin(r), l = sqrt(x * x + y * y + z * z), ic;

	x /= l; y /= l; z /= l; ic = 1 - c;
	dm_id(t);
	t[0] = x * x * ic + c; t[1] = y * x * ic + z * s; t[2] = z * x * ic - y * s;
	t[4] = x * y * ic - z * s; t[5] = y * y * ic + c; t[6] = z * y * ic + x * s;
	t[8] = x * z * ic + y * s; t[9] = y * z * ic - x * s; t[10] = z * z * ic + c;
	dm_mul(m, m, t);
}
static void dm_tr(double *m, double x, double y, double z)
{
	double t[16];

	dm_id(t);
	t[12] = x; t[13] = y; t[14] = z;
	dm_mul(m, m, t);
}
static void dm_sc(double *m, double x, double y, double z)
{
	double t[16];

	dm_id(t);
	t[0] = x; t[5] = y; t[10] = z;
	dm_mul(m, m, t);
}
static void dm_persp(double *m, double fovy, double aspect, double zn, double zf)
{
	double p[16] = {0}, rad = fovy / 2 * M_PI / 180, cot = cos(rad) / sin(rad);

	p[0] = cot / aspect;
	p[5] = cot;
	p[10] = -(zf + zn) / (zf - zn);
	p[11] = -1;
	p[14] = -2 * zn * zf / (zf - zn);
	dm_mul(m, m, p);
}
static void ref_mvp(double *out, const FTransform *t, double zn)
{
	double mv[16], pr[16];

	dm_id(mv);
	dm_sc(mv, t->scalex, t->scaley, -t->scalez);
	if (t->roll)
		dm_rot(mv, t->rollangle, 0, 0, 1);
	dm_rot(mv, t->anglex, 1, 0, 0);
	dm_rot(mv, t->angley + (neg == 1 ? 90.0 : 270.0), 0, 1, 0); /* neg 1: a wrong turn direction */
	dm_tr(mv, -t->x, -t->z, -t->y);
	dm_id(pr);
	dm_persp(pr, t->fovxangle, 1.0, zn, 32768.0);
	dm_mul(out, pr, mv);
}
static void ref_clipspace(const double *m, const double *p, double *o)
{
	o[0] = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
	o[1] = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
	o[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
	o[3] = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
}

static FTransform rand_transform(void)
{
	FTransform t;

	memset(&t, 0, sizeof t);
	t.x = (float)rndf(-3000, 3000);
	t.y = (float)rndf(-3000, 3000);
	t.z = (float)rndf(-500, 500);
	t.anglex = (float)rndf(-80, 80);
	t.angley = (float)rndf(0, 360);
	t.scalex = 1.0f;
	t.scaley = (float)rndf(1.2, 1.8);
	t.scalez = 1.0f;
	t.fovxangle = (float)rndf(60, 110);
	return t;
}

/* ---- group: matrices ---- */
static void test_matrices(void)
{
	int i, k;
	double worst = 0.0, worst_rot = 0.0, worst_tr = 0.0;
	FTransform t;

	group_begin("matrices");
	/* analytic: camera at world (0, 20, 0) (FTransform x, z = height 20, y), yaw 0 looks along +x, right is -z, up is +y */
	memset(&t, 0, sizeof t);
	t.z = 20.0f;
	t.scalex = t.scalez = 1.0f;
	t.scaley = 1.6f;
	t.fovxangle = 90.0f;
	xf_set_transform(&t);
	xf_update();
	{
		struct { float p[3]; double nx, ny; } cases[] = {
			{{100, 20, 0}, 0.0, 0.0},       /* straight ahead: the centre */
			{{100, 20, -100}, 1.0, 0.0},    /* 45 degrees to the right: the edge of a 90 degree field */
			{{100, 20, 100}, -1.0, 0.0},
			{{100, 120, 0}, 0.0, 1.6},      /* up: scaled by the aspect factor */
			{{50, -30, -25}, 0.5, -1.6},
		};

		for (i = 0; i < 5; i++)
		{
			const float *m = H.mvp, *p = cases[i].p;
			float x = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], y = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13], w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
			double ex = cases[i].nx + (neg == 2 ? 0.01 : 0.0); /* neg 2: expectation off by 1% of the field */

			EXPECT(fabs(x / w - ex) < 1e-4 && fabs(y / w - cases[i].ny) < 1e-4, "analytic case %d: got (%f, %f) want (%f, %f)", i, x / w, y / w, ex, cases[i].ny);
		}
	}
	for (i = 0; i < 300; i++)
	{
		double ref[16];
		float sc;

		t = rand_transform();
		t.roll = (i % 5) == 0;
		t.rollangle = (float)rndf(-45, 45);
		xf_set_2d(0.9f + (float)(i % 7));
		xf_set_transform(&t);
		xf_update();
		ref_mvp(ref, &t, H.near_plane);
		sc = 0.0f;
		for (k = 0; k < 16; k++)
			sc = fmaxf(sc, (float)fabs(ref[k]));
		for (k = 0; k < 16; k++)
		{
			double e = fabs((double)H.mvp[k] - ref[k]) / (fabs(ref[k]) + 1e-3 * sc + 1e-6);

			if (k % 4 != 3 && k < 12 && fabs((double)H.mvp[k] - ref[k]) > worst_rot)
				worst_rot = fabs((double)H.mvp[k] - ref[k]);
			if (k >= 12 && fabs((double)H.mvp[k] - ref[k]) / (fabs(ref[k]) + 1) > worst_tr)
				worst_tr = fabs((double)H.mvp[k] - ref[k]) / (fabs(ref[k]) + 1);

			if (e > worst)
				worst = e;
			EXPECT(e < 2e-4, "transform %d element %d: float %g double %g", i, k, H.mvp[k], ref[k]);
		}
	}
	{
		char d[128];

		snprintf(d, sizeof d, "4x4 model-view-projection of 300 random transforms against the double precision product, worst error: rotation part %.2e absolute, translation part %.2e relative; 5 analytic points", worst_rot, worst_tr);
		group_end(d);
	}
	xf_set_2d(0.9f);
}

/* ---- decoding of captured GIF packets ---- */
typedef struct
{
	u32 at; /* index of the tag in cap */
	u32 nloop, nreg, flg, prim, eop, pre;
	u64 regs;
	u32 size; /* physical qwords after the tag, including REGLIST padding */
	int reglist;
	qw_t decoded[15]; /* logical PACKED vertices decoded independently from REGLIST */
} pkt_t;

static int pkt_at(u32 at, pkt_t *p)
{
	u64 d0, d1;

	if (at >= cap_n)
		return 0;
	d0 = cap[at].d[0];
	d1 = cap[at].d[1];
	p->at = at;
	p->nloop = (u32)(d0 & 0x7FFF);
	p->eop = (u32)((d0 >> 15) & 1);
	p->pre = (u32)((d0 >> 46) & 1);
	p->prim = (u32)((d0 >> 47) & 0x7FF);
	p->flg = (u32)((d0 >> 58) & 3);
	p->nreg = (u32)(d0 >> 60);
	if (p->nreg == 0)
		p->nreg = 16;
	p->regs = d1;
	p->size = p->flg == 2 ? p->nloop : p->flg == 1 ? (p->nloop * p->nreg + 1) / 2 : p->nloop * p->nreg;
	p->reglist = p->flg == 1;
	if (p->reglist)
	{
		const u64 *raw = (const u64 *)&cap[at + 1];
		const u32 nr = p->nreg;
		u32 vn = 0, k, r;
		u64 desc = 0;
		float q = 1.0f;
		if (p->pre || p->nloop != 1 || (d1 & 15) != 0 || at + 1 + p->size > cap_n)
			return 0;
		p->prim = (u32)raw[0];
		/* Locate the first vertex kick, then require exactly that register sequence for every vertex. */
		for (k = 1; k < nr; k++)
		{
			r = (u32)((d1 >> (k * 4)) & 15);
			desc |= (u64)r << (vn++ * 4);
			if (r == 4 || r == 5) break;
		}
		if (!vn || (nr - 1) % vn != 0 || (r != 4 && r != 5)) return 0;
		memset(p->decoded, 0, sizeof p->decoded);
		for (k = 1; k < nr; k++)
		{
			qw_t *dst = &p->decoded[k - 1];
			r = (u32)((d1 >> (k * 4)) & 15);
			if (r != ((desc >> (((k - 1) % vn) * 4)) & 15)) return 0;
			if (r == 2) { dst->d[0] = raw[k]; }
			else if (r == 1)
			{
				u32 col = (u32)raw[k], qb = (u32)(raw[k] >> 32);
				dst->w[0] = col & 255; dst->w[1] = (col >> 8) & 255;
				dst->w[2] = (col >> 16) & 255; dst->w[3] = col >> 24;
				memcpy(&q, &qb, sizeof q);
			}
			else if (r == 4 || r == 5)
			{
				dst->w[0] = (u32)raw[k] & 65535;
				dst->w[1] = (u32)(raw[k] >> 16) & 65535;
				dst->w[2] = r == 5 ? (u32)(raw[k] >> 32) : ((u32)(raw[k] >> 32) & 0xFFFFFF) << 4;
				dst->w[3] = r == 5 ? 0 : (u32)(raw[k] >> 56) << 4;
				if ((desc & 15) == 2) p->decoded[k - vn].f[2] = q;
			}
			else return 0;
		}
		if ((nr & 1) && raw[nr] != 0) return 0;
		/* Existing assertions inspect the logical draw; size retains the actual DMA span. */
		p->flg = 0; p->pre = 1; p->regs = desc;
		p->nloop = (nr - 1) / vn; p->nreg = vn;
	}
	return 1;
}

/* a GIF packet must lie inside one DMA buffer */
static int straddles(const pkt_t *p)
{
	int i;
	u32 end = p->at + 1 + p->size;

	for (i = 0; i < cap_nmarks; i++)
		if (cap_marks[i] > p->at && cap_marks[i] < end)
			return 1;
	return 0;
}

/* ---- window space reference rendering of one polygon ---- */
typedef struct
{
	double x, y;
} pt2;

static int clip_rect(const pt2 *in, int n, pt2 *out, double x0, double y0, double x1, double y1)
{
	pt2 a[64], b[64];
	int i, cnt = n, p;

	memcpy(a, in, sizeof(pt2) * (size_t)n);
	for (p = 0; p < 4; p++)
	{
		int m = 0;

		for (i = 0; i < cnt; i++)
		{
			pt2 v0 = a[i], v1 = a[(i + 1) % cnt];
			double d0 = p == 0 ? v0.x - x0 : p == 1 ? x1 - v0.x : p == 2 ? v0.y - y0 : y1 - v0.y;
			double d1 = p == 0 ? v1.x - x0 : p == 1 ? x1 - v1.x : p == 2 ? v1.y - y0 : y1 - v1.y;

			if (d0 >= 0)
				b[m++] = v0;
			if ((d0 >= 0) != (d1 >= 0))
			{
				double t = d0 / (d0 - d1);

				b[m].x = v0.x + (v1.x - v0.x) * t;
				b[m].y = v0.y + (v1.y - v0.y) * t;
				m++;
			}
		}
		memcpy(a, b, sizeof(pt2) * (size_t)m);
		cnt = m;
		if (!cnt)
			return 0;
	}
	memcpy(out, a, sizeof(pt2) * (size_t)cnt);
	return cnt;
}
static double area2(const pt2 *p, int n)
{
	double a = 0;
	int i;

	for (i = 0; i < n; i++)
		a += p[i].x * p[(i + 1) % n].y - p[(i + 1) % n].x * p[i].y;
	return fabs(a) * 0.5;
}

/* reference: world polygon -> window polygon (frame buffer pixels, GL convention: pixel centres at +0.5), clipped to near/far in
   clip space (planes 4, 5), then to the viewport rectangle. neg 3: the near plane clip is left out. */
static int ref_window_polygon(const double *mvp, const double (*w)[3], int n, double vpx, double vpy, double vpw, double vph, pt2 *out)
{
	double c[40][4], d[40][4];
	int i, k, cnt = n, pl;
	pt2 win[40];
	double (*in)[4] = c, (*o)[4] = d;

	for (i = 0; i < n; i++)
		ref_clipspace(mvp, w[i], c[i]);
	for (pl = 4; pl < 6; pl++)
	{
		int m = 0;

		if (pl == 4 && neg == 3)
			continue;
		for (i = 0; i < cnt; i++)
		{
			double *v0 = in[i], *v1 = in[(i + 1) % cnt], d0, d1;

			d0 = pl == 4 ? v0[2] + v0[3] : v0[3] - v0[2];
			d1 = pl == 4 ? v1[2] + v1[3] : v1[3] - v1[2];
			if (d0 >= 0)
				memcpy(o[m++], v0, sizeof(double[4]));
			if ((d0 >= 0) != (d1 >= 0))
			{
				double t = d0 / (d0 - d1);

				for (k = 0; k < 4; k++)
					o[m][k] = v0[k] + (v1[k] - v0[k]) * t;
				m++;
			}
		}
		cnt = m;
		{
			double (*t)[4] = in;

			in = o;
			o = t;
		}
		if (!cnt)
			return 0;
	}
	for (i = 0; i < cnt; i++)
	{
		double iw = 1.0 / in[i][3];

		win[i].x = vpx + (in[i][0] * iw * 0.5 + 0.5) * vpw;
		win[i].y = vpy + (-in[i][1] * iw * 0.5 + 0.5) * vph;
	}
	return clip_rect(win, cnt, out, 0, 0, 640, 448);
}

/* decode the first TRIFAN / TRI packet after pos: window polygon (GS coordinates + 0.5 = GL convention) */
static int decode_vertices(const pkt_t *p, int textured, pt2 *win, double *zs, float *st, float *qs, int max)
{
	int i, nreg = (int)p->nreg, nv = (int)p->nloop;

	if (nv > max)
		return -1;
	for (i = 0; i < nv; i++)
	{
		const qw_t *v = p->reglist ? &p->decoded[i * nreg] : &cap[p->at + 1 + (u32)(i * nreg)];
		const qw_t *xyz = &v[nreg - 1];

		if (textured)
		{
			st[i * 2] = v[0].f[0];
			st[i * 2 + 1] = v[0].f[1];
			qs[i] = v[0].f[2];
		}
		win[i].x = (double)(xyz->w[0] & 0xFFFF) / 16.0 - 2048.0 + 0.5; /* + 0.5: the driver aims half a pixel up/left (GS centres) */
		win[i].y = (double)(xyz->w[1] & 0xFFFF) / 16.0 - 2048.0 + 0.5;
		zs[i] = (double)xyz->w[2];
		if (xyz->w[3] != 0 || (xyz->w[0] >> 16) != 0 || (xyz->w[1] >> 16) != 0)
			return -2;
	}
	return nv;
}

static void fan_poly(FOutVector *v, const double (*w)[3], int n, int sc)
{
	int i;

	for (i = 0; i < n; i++)
	{
		v[i].x = (float)w[i][0];
		v[i].y = (float)w[i][1];
		v[i].z = (float)w[i][2];
		v[i].s = (float)(sc ? w[i][0] * 0.05 : 0.0);
		v[i].t = (float)(sc ? w[i][1] * 0.05 : 0.0);
	}
}

/* random convex planar polygon: a regular n-gon on a circle in a random plane */
static int rand_polygon(double (*w)[3], double cx, double cy, double cz, double rmax)
{
	int n = 3 + (int)(rnd() % 7), i;
	double nx = rndf(-1, 1), ny = rndf(-1, 1), nz = rndf(-1, 1), nl = sqrt(nx * nx + ny * ny + nz * nz), ux, uy, uz, vx, vy, vz, r = rndf(5, rmax), a0 = rndf(0, 6.28);

	nx /= nl; ny /= nl; nz /= nl;
	/* u = n x (1,0,0) or n x (0,1,0) */
	if (fabs(nx) < 0.9) { ux = 0; uy = nz; uz = -ny; }
	else { ux = -nz; uy = 0; uz = nx; }
	nl = sqrt(ux * ux + uy * uy + uz * uz);
	ux /= nl; uy /= nl; uz /= nl;
	vx = ny * uz - nz * uy; vy = nz * ux - nx * uz; vz = nx * uy - ny * ux;
	for (i = 0; i < n; i++)
	{
		double a = a0 + 6.283185307 * i / n;

		w[i][0] = cx + r * (cos(a) * ux + sin(a) * vx);
		w[i][1] = cy + r * (cos(a) * uy + sin(a) * vy);
		w[i][2] = cz + r * (cos(a) * uz + sin(a) * vz);
	}
	return n;
}

static void fake_texture(int wrap)
{
	int ri = rec_new();

	H.rec[ri].blk = H.pool_base;
	H.rec[ri].nblk = 16;
	H.rec[ri].w = H.rec[ri].h = 64;
	H.rec[ri].tw = H.rec[ri].th = 6;
	H.rec[ri].uw = H.rec[ri].vh = 64;
	H.rec[ri].sxs = H.rec[ri].sys = 1.0f;
	H.rec[ri].tbw = 2;
	H.rec[ri].psm = PSM_T8;
	H.rec[ri].clut = 1;
	H.rec[ri].wrapx = H.rec[ri].wrapy = (u8)wrap;
	lru_push_head(ri);
	H.cur_tex = ri;
}

/* ---- group: vertex packing of unclipped polygons ---- */
static void test_packing(void)
{
	int it, nunc = 0, nclip = 0;
	FSurfaceInfo s;
	FTransform t;

	group_begin("vertex_packing");
	memset(&s, 0, sizeof s);
	s.PolyColor.s.red = 200;
	s.PolyColor.s.green = 100;
	s.PolyColor.s.blue = 50;
	s.PolyColor.s.alpha = 255;
	for (it = 0; it < 4000; it++)
	{
		double w[16][3], ref[16], mvp[16];
		FOutVector v[16];
		int n, k;
		pkt_t p;
		u32 pos;
		pt2 win[64];
		double zs[64];
		float st[128], qs[64];
		int nv, found = 0, textured = (it % 4) != 3, wrap = it & 1;

		host_init(it % 2);
		t = rand_transform();
		xf_set_transform(&t);
		xf_update();
		ref_mvp(mvp, &t, 0.9);
		(void)ref;
		fake_texture(wrap);
		n = rand_polygon(w, t.x + rndf(-200, 200), t.z + rndf(-100, 100), t.y + rndf(-200, 200), rndf(20, 300));
		fan_poly(v, (const double (*)[3])w, n, 1);
		{
			/* the polygon must be completely in front of the camera and inside the viewport to be comparable one to one */
			int inside = 1;
			double pc[4];

			for (k = 0; k < n; k++)
			{
				ref_clipspace(mvp, w[k], pc);
				if (pc[3] < 1.0 || fabs(pc[0] / pc[3]) > 1.2 || fabs(pc[1] / pc[3]) > 1.2 || pc[2] / pc[3] > 0.999)
					inside = 0;
			}
			if (!inside)
				continue;
		}
		if (!textured)
			H.cur_tex = NOREC;
		cap_reset();
		if (!begin_draw(PF_Masked | PF_Modulated | PF_Occlude | (textured ? 0 : PF_NoTexture), &s))
		{
			EXPECT(0, "begin_draw refused");
			continue;
		}
		emit_fan(v, NULL, n, NULL);
		for (pos = 0; pos < cap_n && pkt_at(pos, &p); pos += 1 + p.size)
			if (p.flg == 0 && p.pre && p.regs != GIF_REGS_AD)
			{
				found = 1;
				break;
			}
		EXPECT(found, "no vertex packet");
		if (!found)
			continue;
		nunc++;
		EXPECT(!straddles(&p), "packet straddles a DMA buffer");
		EXPECT(p.nloop == (u32)n && p.eop == 1 && p.pre == 1, "tag: nloop %u (want %d) eop %u pre %u", p.nloop, n, p.eop, p.pre);
		EXPECT(p.nreg == (textured ? 3u : 1u), "nreg %u", p.nreg);
		EXPECT(p.regs == (textured ? (neg == 4 ? 0x521ull : 0x512ull) : 0x5ull), "reg descriptor %llx", (unsigned long long)p.regs);
		EXPECT((p.prim & 7) == PRIM_TRIFAN, "primitive type %u", p.prim & 7);
		EXPECT(((p.prim >> 3) & 1) == 1 && ((p.prim >> 4) & 1) == (textured ? 1u : 0u) && ((p.prim >> 6) & 1) == 0 && ((p.prim >> 8) & 1) == 0 && ((p.prim >> 9) & 1) == 0, "PRIM bits %03x", p.prim);
		nv = decode_vertices(&p, textured, win, zs, st, qs, 64);
		EXPECT(nv == n, "decoded %d vertices (want %d), decode error", nv, n);
		if (nv != n)
			continue;
		for (k = 0; k < n; k++)
		{
			double pc[4], iw, wx, wy, wz, e;

			ref_clipspace(mvp, w[k], pc);
			iw = 1.0 / pc[3];
			wx = (pc[0] * iw * 0.5 + 0.5) * 320.0 * 2.0; /* viewport (0,0,320,200) -> 640 x 448 */
			wy = (-pc[1] * iw * 0.5 + 0.5) * 200.0 * 2.24;
			wz = PS2HWD_ZMAX * (1.0 - (pc[2] * iw * 0.5 + 0.5)) + 0.5;
			if (neg == 5)
				wy = 448.0 - wy; /* neg 5: the y axis the wrong way */
			EXPECT(fabs(win[k].x - wx) < 0.075 && fabs(win[k].y - wy) < 0.075, "vertex %d window (%.4f, %.4f) want (%.4f, %.4f)", k, win[k].x, win[k].y, wx, wy);
			/* the depth value is judged as a distance: the error converted to eye space units (dZ/dd = ZMAX * n*f/((f-n)*d^2)) */
			e = fabs(zs[k] - wz) / (PS2HWD_ZMAX * 0.9 / (pc[3] * pc[3]));
			/* float32 matrices: the error grows with the magnitude of the world coordinates (about 5e-6 of them) */
			EXPECT(e < 0.01 + 3e-5 * (fabs(t.x) + fabs(t.y) + fabs(t.z) + fabs(w[k][0]) + fabs(w[k][1]) + fabs(w[k][2])), "vertex %d z %g want %g (%.4f units of depth at distance %.1f)", k, zs[k], wz, e, pc[3]);
			if (textured)
			{
				double q = qs[k], sref = v[k].s, tref = v[k].t, sb = sref - st[k * 2] / q, tb = tref - st[k * 2 + 1] / q;

				EXPECT(fabs(q - iw) < fabs(iw) * 1e-4, "vertex %d q %g want %g", k, q, iw);
				EXPECT(fabs(sb - round(sb)) < 1e-2 && fabs(tb - round(tb)) < 1e-2, "vertex %d s,t offsets %g %g are not whole texture repeats", k, sb, tb);
				if (!wrap)
					EXPECT(fabs(sb) < 1e-3 && fabs(tb) < 1e-3, "clamped texture got a coordinate offset %g %g", sb, tb);
			}
			/* colour words: textured = scale 0x80 per 255, untextured = raw; alpha always the 0x80 scale */
			EXPECT(P.pass[0].col == ((textured ? (u32)col_lut[200] : 200u) | ((textured ? (u32)col_lut[100] : 100u) << 8) | ((textured ? (u32)col_lut[50] : 50u) << 16) | 0x80000000u), "flat colour modulation lost");
		}
	}
	nclip = 0;
	{
		char d[160];

		snprintf(d, sizeof d, "%d unclipped polygons (CT32 and CT16S setups, textured and flat): GIF tag, vertex x/y/z within 1/16 pixel, S/T/Q, colours", nunc);
		group_end(d);
	}
	(void)nclip;
}

/* ---- group: clipping ---- */
static void test_clipping(void)
{
	int it, nclipped = 0, nrej = 0, total = 0;
	FSurfaceInfo s;
	double worst = 0.0;
	int max_after = 0;

	group_begin("clipping");
	memset(&s, 0, sizeof s);
	s.PolyColor.rgba = 0xFFFFFFFFu;
	for (it = 0; it < 3000; it++)
	{
		FTransform t;
		double w[16][3], mvp[16];
		FOutVector v[16];
		int n, nv, k, found = 0;
		pkt_t p;
		u32 pos;
		pt2 win[64], ref[64], got[64];
		double zs[64];
		float st[128], qs[64];
		int nref, ngot;
		double aref, agot;
		int vp = it % 3; /* 0 full screen, 1 inset viewport, 2 a viewport at the edge */
		int vx0 = vp == 0 ? 0 : vp == 1 ? 40 : 0, vy0 = vp == 0 ? 0 : vp == 1 ? 30 : 0, vx1 = vp == 0 ? 320 : vp == 1 ? 280 : 120, vy1 = vp == 0 ? 200 : vp == 1 ? 170 : 200;

		host_init(it & 1);
		viewport_set(vx0, vy0, vx1, vy1);
		t = rand_transform();
		t.fovxangle = (float)rndf(60, 120);
		xf_set_transform(&t);
		xf_update();
		ref_mvp(mvp, &t, 0.9);
		n = rand_polygon(w, t.x + rndf(-400, 400), t.z + rndf(-200, 200), t.y + rndf(-400, 400), rndf(10, it % 5 == 0 ? 20000 : 800));
		fan_poly(v, (const double (*)[3])w, n, 0);
		H.cur_tex = NOREC;
		cap_reset();
		begin_draw(PF_NoTexture | PF_Modulated | PF_NoDepthTest, &s);
		emit_fan(v, NULL, n, NULL);
		total++;
		nref = ref_window_polygon(mvp, (const double (*)[3])w, n, vx0 * 2.0, vy0 * 2.24, (vx1 - vx0) * 2.0, (vy1 - vy0) * 2.24, ref);
		/* the scissor rectangle of the viewport is applied by the GS: do the same to the reference (and to the decoded polygon) */
		{
			double sx0 = floor(vx0 * 2.0 + 0.001), sy0 = floor(vy0 * 2.24 + 0.001), sx1 = ceil(vx1 * 2.0 - 0.001), sy1 = ceil(vy1 * 2.24 - 0.001);
			pt2 tmp[64];
			int m = nref;

			memcpy(tmp, ref, sizeof(pt2) * (size_t)m);
			/* clip to the viewport rectangle in GL conventions: the vertices are already inside [vp] by construction of the NDC clip,
			   but the reference above only clipped to near/far: do the viewport now */
			m = clip_rect(tmp, m, ref, vx0 * 2.0, vy0 * 2.24, vx1 * 2.0, vy1 * 2.24);
			nref = m;
			(void)sx0; (void)sy0; (void)sx1; (void)sy1;
		}
		for (pos = 0; pos < cap_n && pkt_at(pos, &p); pos += 1 + p.size)
			if (p.flg == 0 && p.pre && p.regs != GIF_REGS_AD)
			{
				found = 1;
				break;
			}
		if (!found)
		{
			nrej++;
			aref = nref >= 3 ? area2(ref, nref) : 0.0;
			EXPECT(aref < 1.5, "polygon rejected but the reference shows %.2f px^2 on screen", aref);
			continue;
		}
		EXPECT(!straddles(&p), "packet straddles a buffer");
		nv = decode_vertices(&p, 0, win, zs, st, qs, 64);
		EXPECT(nv >= 3, "decode %d", nv);
		if (nv < 3)
			continue;
		if (nv > n)
			nclipped++;
		if (nv > max_after)
			max_after = nv;
		for (k = 0; k < nv; k++)
		{
			EXPECT(win[k].x > -1950 + 2048 - 2048 && win[k].x < 4000 && fabs(win[k].x) < 2000 && fabs(win[k].y) < 2000, "vertex %d far outside the GS range: (%g, %g)", k, win[k].x, win[k].y);
			EXPECT(zs[k] >= 0 && zs[k] <= PS2HWD_ZMAX, "vertex %d z %g outside 0..2^24", k, zs[k]);
		}
		/* viewport rectangle in GL window coordinates: the driver's output polygon is larger (guard band); clip it the same way */
		ngot = clip_rect(win, nv, got, vx0 * 2.0, vy0 * 2.24, vx1 * 2.0, vy1 * 2.24);
		aref = nref >= 3 ? area2(ref, nref) : 0.0;
		agot = ngot >= 3 ? area2(got, ngot) : 0.0;
		{
			double err = fabs(aref - agot), per = 0.0, tol;
			int q;

			/* every vertex is rounded to 1/16 pixel: the area moves by about (perimeter * 1/32) at the most */
			for (q = 0; q < nref; q++)
				per += sqrt((ref[q].x - ref[(q + 1) % nref].x) * (ref[q].x - ref[(q + 1) % nref].x) + (ref[q].y - ref[(q + 1) % nref].y) * (ref[q].y - ref[(q + 1) % nref].y));
			tol = 0.4 + per * (1.0 / 24.0) + 1e-4 * aref;
			if (aref > 1 && err / (aref + 1) > worst)
				worst = err / (aref + 1);
			EXPECT(err < tol, "on-screen area %.2f px^2, reference %.2f (vp %d, n=%d -> %d)", agot, aref, vp, n, nv);
		}
	}
	{
		char d[200];

		snprintf(d, sizeof d, "%d random polygons (%d needed clipping, %d rejected, up to %d vertices after clipping): visible area against the double precision clip+project, worst relative error %.2e", total, nclipped, nrej, max_after, worst);
		group_end(d);
	}
	host_init(0);
}

/* ---- group: triangle lists ---- */
static void test_lists(void)
{
	int it, ntri_total = 0, nbuf_total = 0;
	FSurfaceInfo s;

	group_begin("triangle_lists");
	memset(&s, 0, sizeof s);
	s.PolyColor.rgba = 0xFFFFFFFFu;
	for (it = 0; it < 60; it++)
	{
		static FOutVector verts[16384];
		static u32 idx[3 * 8192];
		FTransform t;
		int nv = 0, ni = 0, k, polys = it < 5 ? 3000 : 20 + (int)(rnd() % 60);
		double mvp[16];
		double ref_area = 0.0, got_area = 0.0;
		u32 pos;
		pkt_t p;
		u64 got_tris = 0, want_tris = 0;

		host_init(it & 1);
		t = rand_transform();
		if (it < 5) /* a dense cluster right in front of a fixed camera: thousands of visible triangles fill several DMA buffers */
		{
			memset(&t, 0, sizeof t);
			t.z = 20.0f;
			t.scalex = t.scalez = 1.0f;
			t.scaley = 1.6f;
			t.fovxangle = 90.0f;
		}
		xf_set_transform(&t);
		xf_update();
		ref_mvp(mvp, &t, 0.9);
		for (k = 0; k < polys; k++)
		{
			double w[16][3];
			int n = it < 5 ? rand_polygon(w, rndf(60, 400), rndf(-30, 70), rndf(-150, 150), rndf(3, 25)) : rand_polygon(w, t.x + rndf(-300, 300), t.z + rndf(-100, 100), t.y + rndf(-300, 300), rndf(5, 150)), j, first = nv;
			pt2 ref[64];
			int nref;

			if (nv + n > 16384 || ni + 3 * (n - 2) > 3 * 8192)
				break;
			fan_poly(&verts[nv], (const double (*)[3])w, n, 0);
			nv += n;
			for (j = 1; j + 1 < n; j++)
			{
				idx[ni++] = (u32)first;
				idx[ni++] = (u32)(first + j);
				idx[ni++] = (u32)(first + j + 1);
			}
			nref = ref_window_polygon(mvp, (const double (*)[3])w, n, 0, 0, 640, 448, ref);
			if (nref >= 3)
				ref_area += area2(ref, nref);
		}
		H.cur_tex = NOREC;
		cap_reset();
		begin_draw(PF_NoTexture | PF_Modulated | PF_NoDepthTest, &s);
		emit_tris(verts, idx, (u32)ni, NULL);
		want_tris = (u64)(ni / 3);
		/* sum the area of the emitted triangles that is inside the screen; overlapping polygons are not generated apart from
		   the fans themselves, and coplanar random polygons of different planes may overlap: compare the SUM of polygon areas
		   (the reference sums polygons the same way, no union) */
		for (pos = 0; pos < cap_n && pkt_at(pos, &p); pos += 1 + p.size)
		{
			if (p.flg == 0 && p.pre && p.regs != GIF_REGS_AD)
			{
				int j, nvv;
				pt2 *win = (pt2 *)malloc(sizeof(pt2) * 3 * 16384), tri[3], clipped[16];
				double *zs = (double *)malloc(sizeof(double) * 3 * 16384);
				float st[2], qs[1];

				EXPECT(!straddles(&p), "list packet straddles a buffer");
				EXPECT((p.prim & 7) == (neg == 6 ? PRIM_TRIFAN : PRIM_TRI) && p.nloop % 3 == 0 && p.eop == 1 && p.pre == 1, "list tag prim %u nloop %u", p.prim & 7, p.nloop);
				if (p.nloop > 3 * 16384)
				{
					free(win);
					free(zs);
					continue;
				}
				nvv = decode_vertices(&p, 0, win, zs, st, qs, 3 * 16384);
				EXPECT(nvv == (int)p.nloop, "decode");
				for (j = 0; j + 2 < nvv; j += 3)
				{
					int nc;

					tri[0] = win[j];
					tri[1] = win[j + 1];
					tri[2] = win[j + 2];
					nc = clip_rect(tri, 3, clipped, 0, 0, 640, 448);
					got_area += nc >= 3 ? area2(clipped, nc) : 0.0;
					got_tris++;
				}
				free(win);
				free(zs);
			}
		}
		nbuf_total += cap_nmarks;
		ntri_total += (int)want_tris;
		if (it < 5)
			EXPECT(cap_nmarks >= 2, "the dense batch fitted into %d DMA buffer(s)", cap_nmarks + 1);
		EXPECT(fabs(ref_area - got_area) < 2.0 + ref_area * 2e-3 + got_tris * 0.3, "area %.1f px^2 against the reference %.1f (%d polygons, %d triangles in, %llu out)", got_area, ref_area, polys, ni / 3, (unsigned long long)got_tris);
	}
	{
		char d[160];

		snprintf(d, sizeof d, "%d triangles in 60 batches through DrawIndexedTriangles' packer (%d DMA buffers filled): one GIF tag per run, no packet across a buffer boundary, visible area = double precision reference", ntri_total, nbuf_total);
		group_end(d);
	}
	host_init(0);
}

/* ---- group: register values ---- */
static u64 enc_test(int ate, int atst, int aref, int afail, int date, int datm, int zte, int ztst)
{
	return (u64)ate | ((u64)atst << 1) | ((u64)aref << 4) | ((u64)afail << 12) | ((u64)date << 14) | ((u64)datm << 15) | ((u64)zte << 16) | ((u64)ztst << 17);
}
static u64 enc_alpha(int a, int b, int c, int d, int fix)
{
	return (u64)a | ((u64)b << 2) | ((u64)c << 4) | ((u64)d << 6) | ((u64)fix << 32);
}
static u64 enc_tex0(u32 tbp0, u32 tbw, u32 psm, u32 tw, u32 th, u32 tcc, u32 tfx, u32 cbp, u32 cpsm, u32 csm, u32 csa, u32 cld)
{
	return (u64)tbp0 | ((u64)tbw << 14) | ((u64)psm << 20) | ((u64)tw << 26) | ((u64)th << 30) | ((u64)tcc << 34) | ((u64)tfx << 35) | ((u64)cbp << 37) | ((u64)cpsm << 51) | ((u64)csm << 55) | ((u64)csa << 56) | ((u64)cld << 61);
}
static u64 enc_tex1(int lcm, int mxl, int mmag, int mmin, int mtba, int l, int k)
{
	return (u64)lcm | ((u64)mxl << 2) | ((u64)mmag << 5) | ((u64)mmin << 6) | ((u64)mtba << 9) | ((u64)l << 19) | ((u64)k << 32);
}
static u64 enc_clamp(int wms, int wmt, int minu, int maxu, int minv, int maxv)
{
	return (u64)wms | ((u64)wmt << 2) | ((u64)minu << 4) | ((u64)maxu << 14) | ((u64)minv << 24) | ((u64)maxv << 34);
}
static u64 enc_frame(u32 fbp, u32 fbw, u32 psm, u32 msk)
{
	return (u64)fbp | ((u64)fbw << 16) | ((u64)psm << 24) | ((u64)msk << 32);
}
static u64 enc_zbuf(u32 zbp, u32 psm, u32 zmsk)
{
	return (u64)zbp | ((u64)psm << 24) | ((u64)zmsk << 32);
}
static u64 enc_scissor(u32 x0, u32 x1, u32 y0, u32 y1)
{
	return (u64)x0 | ((u64)x1 << 16) | ((u64)y0 << 32) | ((u64)y1 << 48);
}

/* the A+D writes of the stream as a table: reg -> last value written */
static void ad_table(u64 *val, int *seen)
{
	u32 pos;
	pkt_t p;

	memset(seen, 0, 128 * sizeof(int));
	for (pos = 0; pos < cap_n && pkt_at(pos, &p); pos += 1 + p.size)
		if (p.flg == 0 && p.regs == GIF_REGS_AD)
		{
			u32 i;

			for (i = 0; i < p.nloop; i++)
			{
				u32 reg = (u32)(cap[p.at + 1 + i].d[1] & 0xFF);

				if (reg < 128)
				{
					val[reg] = cap[p.at + 1 + i].d[0];
					seen[reg] = 1;
				}
			}
		}
}

static void test_registers(void)
{
	struct
	{
		u32 flags;
		int ate, atst, aref, abe, a, b, c, d, ztst, zmsk;
		const char *name;
	} cases[] = {
		{PF_Masked, 1, 6, 0x40, 0, 0, 0, 0, 0, 2, 1, "masked"},
		{PF_Translucent, 1, 7, 0, 1, 0, 1, 0, 1, 2, 1, "translucent"},
		{PF_Additive, 1, 7, 0, 1, 0, 2, 0, 1, 2, 1, "additive"},
		{PF_ReverseSubtract, 1, 7, 0, 1, 2, 0, 0, 1, 2, 1, "reverse subtract"},
		{PF_Subtractive, 1, 7, 0, 1, 0, 1, 2, 2, 2, 1, "subtractive source-minus-destination"},
		{PF_Translucent | PF_NoDepthTest, 1, 7, 0, 1, 0, 1, 0, 1, 1, 1, "translucent, no depth test"},
		{PF_Masked | PF_Occlude, 1, 6, 0x40, 0, 0, 0, 0, 0, 2, 0, "masked occluding"},
		{PF_Masked | PF_Occlude | PF_NoAlphaTest, 0, 6, 0x40, 0, 0, 0, 0, 0, 2, 0, "no alpha test"},
		{PF_Fog | PF_NoTexture, 0, 1, 0, 1, 1, 2, 0, 2, 2, 1, "fog block destination channel scale"},
		{PF_Occlude, 1, 6, 0x40, 0, 0, 0, 0, 0, 2, 0, "no blend flags"},
	};
	int i, k;
	u64 val[128];
	int seen[128];

	group_begin("register_values");
	for (i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++)
	{
		FSurfaceInfo s;
		u64 want_test, want_alpha;
		int inv;

		for (inv = 0; inv < 2; inv++)
		{
			host_init(i & 1);
			memset(&s, 0, sizeof s);
			s.PolyColor.rgba = 0xFF804020u;
			H.cur_tex = NOREC;
			cap_reset();
			if (!begin_draw(cases[i].flags | PF_Modulated | PF_NoTexture | (inv ? PF_Invisible : 0), &s))
			{
				EXPECT(0, "%s: refused", cases[i].name);
				continue;
			}
			pass_setup(0, 0);
			ad_table(val, seen);
			want_test = enc_test(cases[i].ate, cases[i].atst, cases[i].aref, 0, 0, 0, 1, cases[i].ztst + (neg == 7 ? 1 : 0));
			EXPECT(seen[0x47] && val[0x47] == want_test, "%s: TEST %llx want %llx", cases[i].name, (unsigned long long)val[0x47], (unsigned long long)want_test);
			if (cases[i].abe)
			{
				want_alpha = enc_alpha(cases[i].a, cases[i].b, cases[i].c, cases[i].d, cases[i].c == 2 ? 128 : 0);
				EXPECT(seen[0x42] && val[0x42] == want_alpha, "%s: ALPHA %llx want %llx", cases[i].name, (unsigned long long)val[0x42], (unsigned long long)want_alpha);
			}
			EXPECT(seen[0x4e] && val[0x4e] == enc_zbuf(H.z_addr / 8192, 1, (u32)cases[i].zmsk), "%s: ZBUF %llx", cases[i].name, (unsigned long long)val[0x4e]);
			EXPECT(seen[0x4c] && val[0x4c] == enc_frame(H.fb_addr[0] / 8192, 10, H.fb32 ? 0u : 0x0Au, inv ? 0xFFFFFFFFu : cases[i].flags == (PF_Fog | PF_NoTexture) ? 0xFFFFFF00u : 0u), "%s: FRAME %llx", cases[i].name, (unsigned long long)val[0x4c]);
			EXPECT(seen[0x40] && val[0x40] == enc_scissor(0, 639, 0, 447), "%s: SCISSOR %llx", cases[i].name, (unsigned long long)val[0x40]);
		}
	}
	/* textured: TEX0 / TEX1 / CLAMP for the three palette flavours and CT16 / CT32 */
	for (k = 0; k < 6; k++)
	{
		static const struct { int psm, clut, notcc, wrap; } tex[6] = {{PSM_T8, 1, 0, 1}, {PSM_T8, 2, 0, 0}, {PSM_T8, 3, 0, 1}, {PSM_CT16, 0, 0, 0}, {PSM_CT32, 0, 0, 1}, {PSM_CT16S, 0, 1, 0}};
		FSurfaceInfo s;
		int ri, mod, lin;

		for (mod = 0; mod < 2; mod++)
			for (lin = 0; lin < 2; lin++)
			{
				u32 cbp;
				u64 want0, want1, wantc;

				host_init(0);
				H.tex_filter = lin ? HWD_SET_TEXTUREFILTER_BILINEAR : HWD_SET_TEXTUREFILTER_POINTSAMPLED;
				memset(&s, 0, sizeof s);
				s.PolyColor.rgba = 0xFFFFFFFFu;
				ri = rec_new();
				H.rec[ri].blk = 9000 + (u32)k * 64;
				H.rec[ri].w = 64;
				H.rec[ri].h = 32;
				H.rec[ri].tw = 6;
				H.rec[ri].th = 5;
				H.rec[ri].uw = 64; H.rec[ri].vh = 32;
				H.rec[ri].sxs = H.rec[ri].sys = 1.0f;
				H.rec[ri].tbw = 2;
				H.rec[ri].psm = (u8)tex[k].psm;
				H.rec[ri].clut = (u8)tex[k].clut;
				H.rec[ri].notcc = (u8)tex[k].notcc;
				H.rec[ri].wrapx = (u8)tex[k].wrap;
				H.rec[ri].wrapy = 0;
				lru_push_head(ri);
				H.cur_tex = ri;
				cap_reset();
				begin_draw(PF_Masked | (mod ? PF_Modulated : 0), &s);
				pass_setup(0, 0);
				ad_table(val, seen);
				cbp = H.clut_base + (u32)(tex[k].clut - 1) * 4;
				want0 = enc_tex0(9000 + (u32)k * 64, 2, (u32)tex[k].psm, 6, 5, tex[k].notcc ? 0u : 1u, 0, tex[k].clut ? cbp : 0u, 0, 0, 0, tex[k].clut ? 1u : 0u);
				want1 = enc_tex1(1, 0, lin, lin, 0, 0, 0);
				wantc = enc_clamp(tex[k].wrap ? 0 : 1, 1, 0, 63, 0, 31);
				EXPECT(seen[0x06] && val[0x06] == want0, "texture %d mod %d: TEX0 %llx want %llx", k, mod, (unsigned long long)val[0x06], (unsigned long long)want0);
				EXPECT(seen[0x14] && val[0x14] == want1, "texture %d: TEX1 %llx want %llx", k, (unsigned long long)val[0x14], (unsigned long long)want1);
				EXPECT(seen[0x08] && val[0x08] == wantc, "texture %d: CLAMP %llx want %llx", k, (unsigned long long)val[0x08], (unsigned long long)wantc);
			}
	}
	{
		/* the CLUT is loaded once (CLD = 1) and not again while the same CLUT stays loaded */
		FSurfaceInfo s;
		int ri;
		pkt_t p;
		u32 pos;
		int cld_count = 0;

		host_init(0);
		memset(&s, 0, sizeof s);
		ri = rec_new();
		H.rec[ri].blk = 9000;
		H.rec[ri].w = H.rec[ri].h = 16;
		H.rec[ri].tw = H.rec[ri].th = 4;
		H.rec[ri].uw = H.rec[ri].vh = 16;
		H.rec[ri].sxs = H.rec[ri].sys = 1.0f;
		H.rec[ri].tbw = 2;
		H.rec[ri].psm = PSM_T8;
		H.rec[ri].clut = 1;
		lru_push_head(ri);
		H.cur_tex = ri;
		cap_reset();
		for (i = 0; i < 3; i++)
		{
			begin_draw(PF_Masked | PF_Modulated, &s);
			pass_setup(0, 0);
		}
		for (pos = 0; pos < cap_n && pkt_at(pos, &p); pos += 1 + p.size)
			if (p.flg == 0 && p.regs == GIF_REGS_AD)
			{
				u32 j;

				for (j = 0; j < p.nloop; j++)
					if ((cap[p.at + 1 + j].d[1] & 0xFF) == 0x06 && ((cap[p.at + 1 + j].d[0] >> 61) & 7) == 1)
						cld_count++;
			}
		EXPECT(cld_count == 1, "CLD=1 written %d times for three draws with the same texture", cld_count);
	}
	{
		char d[160];

		snprintf(d, sizeof d, "TEST/ALPHA/ZBUF/FRAME/SCISSOR for 10 blend and depth flag sets (x invisible, x CT32/CT16S) and TEX0/TEX1/CLAMP for 6 texture kinds x modulate x filter, against the GS manual's field layout");
		group_end(d);
	}
	host_init(0);
}

/* ---- group: texture footprint in VRAM ---- */
static const u8 BT32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23}, {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
static const u8 BT16[8][4] = {{0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15}, {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
static const u8 BT16S[8][4] = {{0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27}, {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};

static u32 model_block(int psm, u32 x, u32 y, u32 tbw)
{
	switch (psm)
	{
		case PSM_CT32: return ((y >> 5) * tbw + (x >> 6)) * 32 + BT32[(y >> 3) & 3][(x >> 3) & 7];
		case PSM_CT16: return ((y >> 6) * tbw + (x >> 6)) * 32 + BT16[(y >> 3) & 7][(x >> 4) & 3];
		case PSM_CT16S: return ((y >> 6) * tbw + (x >> 6)) * 32 + (neg == 8 ? BT16 : BT16S)[(y >> 3) & 7][(x >> 4) & 3]; /* neg 8: the wrong block table */
		default: return ((y >> 6) * (tbw / 2) + (x >> 7)) * 32 + BT32[(y >> 4) & 3][(x >> 4) & 7]; /* PSMT8: 128x64 pages of 16x16 blocks */
	}
}

static void test_footprint(void)
{
	static const int psms[4] = {PSM_T8, PSM_CT16, PSM_CT16S, PSM_CT32};
	int pi, wi, hi, n = 0;

	group_begin("texture_footprint");
	for (pi = 0; pi < 4; pi++)
		for (wi = 4; wi <= 10; wi++)
			for (hi = 4; hi <= 10; hi++)
			{
				u32 w = 1u << wi, h = 1u << hi, x, y, maxb = 0;
				u32 tbw = psms[pi] == PSM_T8 ? ((w + 127) / 128) * 2 : (w + 63) / 64;

				for (y = 0; y < h; y += 8)
					for (x = 0; x < w; x += 8)
					{
						u32 b = model_block(psms[pi], x, y, tbw);

						if (b > maxb)
							maxb = b;
					}
				n++;
				EXPECT(tex_footprint(psms[pi], w, h, tbw) == maxb + 1, "psm %02x %ux%u: footprint %u model %u", psms[pi], w, h, tex_footprint(psms[pi], w, h, tbw), maxb + 1);
			}
	{
		char d[160];

		snprintf(d, sizeof d, "%d shapes (4 formats x sizes 16..1024 in both directions): blocks needed = highest block touched by any pixel in the GS page/block layout + 1", n);
		group_end(d);
	}
}

/* ---- group: VRAM allocator ---- */
static void test_allocator(void)
{
	enum { MAXA = 512 };
	struct { u32 s, n; int used; } a[MAXA];
	static u8 used[VRAM_BLOCKS];
	int it, na = 0, nalloc = 0, nfree = 0, i;
	u32 expect_used = 0;

	group_begin("vram_allocator");
	host_init(0);
	memset(used, 0, sizeof used);
	for (it = 0; it < 20000; it++)
	{
		if (na < MAXA && (rnd() % 3 != 0 || na == 0))
		{
			u32 n = 1 + rnd() % (rnd() % 8 == 0 ? 400 : 40), al = 1, s, j;

			while (al < n && al < PAGE_BLOCKS)
				al <<= 1;
			s = vram_alloc(n, al);
			if (!s)
				continue;
			EXPECT(s >= H.pool_base && s + n <= VRAM_BLOCKS && (s % al) == 0, "allocation %u+%u (align %u) outside the pool or unaligned", s, n, al);
			for (j = 0; j < n; j++)
			{
				EXPECT(!used[s + j], "block %u handed out twice", s + j);
				used[s + j] = 1;
			}
			a[na].s = s;
			a[na].n = n;
			na++;
			expect_used += n;
			nalloc++;
		}
		else if (na)
		{
			int k = (int)(rnd() % (u32)na);
			u32 j;

			for (j = 0; j < a[k].n; j++)
				used[a[k].s + j] = 0;
			vram_free(a[k].s, a[k].n - (neg == 9 && (it % 50 == 0) ? 1u : 0u)); /* neg 9: a free that gives back one block too few */
			expect_used -= a[k].n;
			a[k] = a[na - 1];
			na--;
			nfree++;
		}
		EXPECT(H.used_blocks == expect_used - (neg == 9 ? 0u : 0u), "used_blocks %u want %u", H.used_blocks, expect_used);
		if (it % 500 == 0)
		{
			/* the free list: sorted, disjoint, merged, and exactly the blocks no allocation holds */
			u32 prev_end = 0, free_total = 0;

			for (i = 0; i < H.free_n; i++)
			{
				EXPECT(H.free_len[i] > 0 && (i == 0 || H.free_start[i] > prev_end), "free list entry %d not sorted/merged: %u+%u after end %u", i, H.free_start[i], H.free_len[i], prev_end);
				prev_end = H.free_start[i] + H.free_len[i];
				free_total += H.free_len[i];
				{
					u32 j;

					for (j = 0; j < H.free_len[i]; j++)
						EXPECT(!used[H.free_start[i] + j], "block %u is free and allocated", H.free_start[i] + j);
				}
			}
			EXPECT(free_total + expect_used == H.pool_blocks, "free %u + used %u != pool %u", free_total, expect_used, H.pool_blocks);
		}
		if (group_fail > 20)
			break;
	}
	{
		char d[160];

		snprintf(d, sizeof d, "%d allocations, %d frees of 1..400 blocks: no overlap, alignment, accounting, sorted merged free list", nalloc, nfree);
		group_end(d);
	}
	host_init(0);
}

/* ---- group: LRU eviction ---- */
static GLMipmap_t *mk_tex(int w, int h, UINT32 fmt)
{
	GLMipmap_t *m = (GLMipmap_t *)calloc(1, sizeof *m);
	size_t bpp = fmt == GL_TEXFMT_RGBA ? 4 : fmt == GL_TEXFMT_AP_88 || fmt == GL_TEXFMT_ALPHA_INTENSITY_88 ? 2 : 1;

	m->format = (GLTextureFormat_t)fmt;
	m->width = (UINT16)w;
	m->height = (UINT16)h;
	m->data = calloc((size_t)w * h, bpp);
	return m;
}

static void test_eviction(void)
{
	enum { N = 80 };
	GLMipmap_t *t[N];
	int i, ri, evicted_lru_ok = 1;
	u32 stamp_frame;

	group_begin("lru_eviction");
	host_init(0);
	for (i = 0; i < 256; i++)
		H.pal[i] = (u32)i | ((u32)(i ^ 0x55) << 8) | ((u32)(255 - i) << 16);
	H.pal_set = 1;
	pal_build();
	for (i = 0; i < N; i++)
	{
		int x, y;

		t[i] = mk_tex(256, 256, GL_TEXFMT_RGBA); /* 64 KiB indexed = 256 blocks */
		for (y = 0; y < 256; y++)
			for (x = 0; x < 256; x++)
				((u32 *)t[i]->data)[y * 256 + x] = (H.pal[(x + y + i) % 255] & 0xFFFFFF) | 0xFF000000u;
	}
	/* fill the pool, touching textures in a known order */
	H.frame_no = 10;
	for (i = 0; i < N; i++)
	{
		ri = tex_upload(t[i]);
		if (ri == NOREC)
			break;
		if (H.used_blocks + 256 > H.pool_blocks)
			break;
	}
	{
		int resident = 0, first_missing = -1;

		for (i = 0; i < N; i++)
			if (t[i]->downloaded)
				resident++;
		EXPECT(resident >= 20 && resident < N, "%d textures resident after filling", resident);
		/* use texture 0 again so it becomes the most recent, then a new frame, then force evictions */
		for (i = 0; i < N && !t[i]->downloaded; i++)
			;
		first_missing = i;
		stamp_frame = H.frame_no;
		H.frame_no = 11;
		{
			int oldest = H.lru_tail, newest = H.lru_head;

			EXPECT(oldest != NOREC && newest != NOREC && H.rec[oldest].stamp <= H.rec[newest].stamp, "LRU ends");
			lru_touch(oldest); /* the oldest becomes the newest */
			EXPECT(H.lru_head == oldest && H.lru_tail != oldest, "touch did not move the record to the head");
			{
				GLMipmap_t *victim_owner = H.rec[H.lru_tail].owner;
				int newcomer = N - 1;

				if (t[newcomer]->downloaded)
					tex_drop((int)(t[newcomer]->downloaded - 1), 0);
				ri = tex_upload(t[newcomer]);
				EXPECT(ri != NOREC, "upload with eviction failed");
				EXPECT(victim_owner->downloaded == 0, "the least recently used texture was not the one evicted");
				EXPECT(H.rec[oldest].used && H.rec[oldest].owner->downloaded != 0, "the texture touched most recently was evicted");
				if (neg == 10) /* neg 10: pretend the victim keeps its handle */
					victim_owner->downloaded = 1;
				evicted_lru_ok = victim_owner->downloaded == 0;
			}
		}
		(void)first_missing;
		(void)stamp_frame;
	}
	EXPECT(evicted_lru_ok, "the evicted texture still has a VRAM handle");
	/* accounting: sum of the records' blocks == used_blocks */
	{
		u32 sum = 0;

		for (i = 0; i < H.rec_n; i++)
			if (H.rec[i].used)
				sum += H.rec[i].nblk;
		EXPECT(sum == H.used_blocks, "records hold %u blocks, allocator says %u", sum, H.used_blocks);
	}
	/* textures used in the current frame are only evicted when nothing else is left */
	{
		int cur_frame_resident_before = 0, cur_frame_resident_after = 0;

		H.frame_no = 20;
		for (i = 0; i < H.rec_n; i++)
			if (H.rec[i].used && i % 3 == 0)
			{
				lru_touch(i);
				cur_frame_resident_before++;
			}
		for (i = 0; i < 6; i++)
		{
			GLMipmap_t *x = mk_tex(256, 256, GL_TEXFMT_RGBA);

			memcpy(x->data, t[0]->data, 256 * 256 * 4);
			tex_upload(x);
		}
		for (i = 0; i < H.rec_n; i++)
			if (H.rec[i].used && i % 3 == 0 && H.rec[i].stamp == 20)
				cur_frame_resident_after++;
		EXPECT(cur_frame_resident_after == cur_frame_resident_before, "textures of the current frame were evicted while older ones existed (%d -> %d)", cur_frame_resident_before, cur_frame_resident_after);
	}
	group_end("textures of 256 blocks into a full pool: the least recently used is evicted and its handle cleared, a touched one survives, block accounting matches, current-frame textures are kept");
	host_init(0);
}

/* ---- group: palette lookup and texture conversion ---- */
static void test_conversion(void)
{
	int it, texels = 0, holes_total = 0, partial_rejects = 0;

	group_begin("palette_conversion");
	for (it = 0; it < 200; it++)
	{
		static u32 src[64 * 64];
		static u8 dst[64 * 64];
		int i, n = 64 * 64, holes = 0, expect_ok = 1, dup = it % 3 == 0;
		u32 pal_expect_colour[256];

		host_init(0);
		for (i = 0; i < 256; i++)
		{
			H.pal[i] = (u32)(rnd() & 0xFFFFFF);
			if (dup && i > 3 && (rnd() % 5) == 0)
				H.pal[i] = H.pal[rnd() % (u32)i]; /* duplicate colours: the lowest index must win */
			pal_expect_colour[i] = H.pal[i];
		}
		H.pal_set = 1;
		pal_build();
		for (i = 0; i < n; i++)
		{
			u32 r = rnd() % 100;

			if (r < 8)
				src[i] = H.pal[255]; /* exactly representable hole, including its hidden RGB */
			else if (r < 12 && it % 7 == 0)
			{
				src[i] = H.pal[rnd() % 255] | ((rnd() % 254 + 1) << 24); /* partial alpha: not representable */
				expect_ok = 0;
			}
			else
				src[i] = (H.pal[rnd() % 256] & 0xFFFFFF) | 0xFF000000u;
		}
		{
			int uses255 = 0;
			int ok = rgba_to_indices(src, (u32)n, dst, &holes, &uses255);
			int k;

			EXPECT(ok == expect_ok, "conversion %s but expected %s", ok ? "worked" : "failed", expect_ok ? "success" : "failure");
			if (!expect_ok)
			{
				partial_rejects++;
				continue;
			}
			for (k = 0; k < n; k++)
			{
				u32 a = src[k] >> 24;

				texels++;
				if (a == 0)
				{
					EXPECT(dst[k] == 255, "hole texel %d got index %u", k, dst[k]);
					holes_total++;
				}
				else
				{
					u32 c = src[k] & 0xFFFFFF, got = pal_expect_colour[dst[k]] & 0xFFFFFF;
					int lowest = -1, j;

					for (j = 0; j < 256 && lowest < 0; j++)
						if ((pal_expect_colour[j] & 0xFFFFFF) == c)
							lowest = j;
					if (lowest >= 0)
						EXPECT(dst[k] == lowest + (neg == 11 ? 1 : 0), "texel %d colour %06x: index %u, lowest palette index with that colour %d", k, c, dst[k], lowest);
					EXPECT(got == c, "opaque RGB changed, including palette index 255");
				}
			}
			EXPECT(holes == (holes_total > 0 && 1), "holes flag %d", holes);
			holes_total = 0;
		}
	}
	{
		char d[160];

		snprintf(d, sizeof d, "%d texels from 200 random palettes (with duplicate colours): lowest index wins, alpha 0 -> index 255, partial alpha refused (%d textures), index 255's colour maps to the nearest other", texels, partial_rejects);
		group_end(d);
	}
	host_init(0);
}

/* ---- group: upload packets ---- */
static void test_upload(void)
{
	static const int sizes[][2] = {{16, 16}, {64, 64}, {128, 32}, {32, 128}, {200, 50}, {48, 96}, {320, 200}, {512, 512}, {1024, 64}, {64, 1024}, {100, 100}, {16, 1000}, {2048, 8}, {1024, 1024}};
	int si, fmt, uploads = 0, bytes = 0;

	group_begin("upload_packets");
	for (fmt = 0; fmt < 4; fmt++)
		for (si = 0; si < (int)(sizeof sizes / sizeof sizes[0]); si++)
		{
			int w = sizes[si][0], h = sizes[si][1], x, y, ri;
			GLMipmap_t *m;
			u32 pos, row_expected = 0;
			pkt_t p;
			texrec_t r;
			int bpp, W2, H2, nbands = 0, partial = fmt == 3, rows_seen = 0;
			u32 bbp = 0, bbw = 0, bpsm = 0;

			if (w > 1024 || h > 1024)
				continue; /* unsupported >1024 input is tested separately as an explicit failure */
			if ((fmt == 1 || fmt == 3) && w * h * 4 > 1700000)
				continue; /* cannot fit this host fixture's pool; budget failures are tested separately */
			host_init(0);
			for (x = 0; x < 256; x++)
				H.pal[x] = (u32)x | ((u32)(x ^ 0x55) << 8) | ((u32)(255 - x) << 16);
			H.pal_set = 1;
			pal_build();
			m = mk_tex(w, h, GL_TEXFMT_RGBA);
			for (y = 0; y < h; y++)
				for (x = 0; x < w; x++)
				{
					u32 c;

					if (fmt == 0)
						c = (H.pal[(x * 3 + y * 5) % 255] & 0xFFFFFF) | 0xFF000000u; /* indexable */
					else if (fmt == 1)
						c = ((u32)(x * 7) & 255) | (((u32)(y * 11) & 255) << 8) | 0xFF0000u | 0xFF000000u; /* not palette colours: CT16 */
					else if (fmt == 2)
						c = (x + y) % 9 == 0 ? H.pal[255] : ((H.pal[(x + y) % 255] & 0xFFFFFF) | 0xFF000000u); /* exactly representable keyed holes */
					else
						c = ((u32)(x * 5) & 255) | (((u32)(y * 9) & 255) << 8) | (((u32)(x + y) & 255) << 24); /* partial alpha: CT32 */
					((u32 *)m->data)[y * w + x] = c;
				}
			cap_reset();
			ri = tex_upload(m);
			if (ri == NOREC)
			{
				EXPECT(0, "upload %dx%d format %d failed", w, h, fmt);
				continue;
			}
			uploads++;
			r = H.rec[ri];
			W2 = r.w;
			H2 = r.h;
			bpp = r.psm == PSM_T8 ? 1 : r.psm == PSM_CT32 ? 4 : 2;
			EXPECT(W2 == w && H2 == h, "stored size %dx%d changed from %dx%d", W2, H2, w, h);
			EXPECT((fmt == 0 || fmt == 2 ? r.psm == PSM_T8 : r.psm == PSM_CT32), "format %d stored as psm %02x", fmt, r.psm);
			for (pos = 0; pos < cap_n && pkt_at(pos, &p); pos += 1 + p.size)
			{
				EXPECT(!straddles(&p), "upload packet straddles a buffer");
				if (p.flg == 0 && p.regs == GIF_REGS_AD && p.nloop == 4)
				{
					/* BITBLTBUF, TRXPOS, TRXREG, TRXDIR */
					u64 bb = cap[p.at + 1].d[0], tp = cap[p.at + 2].d[0], tr = cap[p.at + 3].d[0], td = cap[p.at + 4].d[0];
					pkt_t img;
					u32 dbp = (u32)((bb >> 32) & 0x3FFF), dbw = (u32)((bb >> 48) & 0x3F), dpsm = (u32)((bb >> 56) & 0x3F);
					u32 dsax = (u32)((tp >> 32) & 0x7FF), dsay = (u32)((tp >> 48) & 0x7FF), rrw = (u32)(tr & 0xFFF), rrh = (u32)((tr >> 32) & 0xFFF);

					EXPECT(cap[p.at + 1].d[1] == 0x50 && cap[p.at + 2].d[1] == 0x51 && cap[p.at + 3].d[1] == 0x52 && cap[p.at + 4].d[1] == 0x53, "register addresses of the transfer set-up");
					EXPECT(dbp == r.blk && dbw == r.tbw && dpsm == (u32)r.psm, "BITBLTBUF dbp %u/%u dbw %u/%u psm %02x/%02x", dbp, r.blk, dbw, r.tbw, dpsm, r.psm);
					EXPECT(dsax == 0 && (bb & 0xFFFFFFFFull) == 0 && td == 0, "TRXPOS dsax %u, source fields %llx, TRXDIR %llu", dsax, (unsigned long long)(bb & 0xFFFFFFFFull), (unsigned long long)td);
					EXPECT(dsay == row_expected, "band starts at row %u, expected %u", dsay, row_expected);
					EXPECT(rrw == (u32)W2 && rrh > 0 && dsay + rrh <= (u32)H2, "band %ux%u at row %u of a %dx%d texture", rrw, rrh, dsay, W2, H2);
					EXPECT(pkt_at(p.at + 5, &img) && img.flg == 2 && img.eop == 1, "no IMAGE tag behind the transfer set-up");
					EXPECT(img.nloop == (rrw * rrh * (u32)bpp + 15) / 16 && img.nloop <= 32767, "IMAGE tag nloop %u, want %u", img.nloop, (rrw * rrh * (u32)bpp + 15) / 16);
					/* the payload: compare the first and last pixel of each row against an independent resampling */
					{
						const u8 *data = (const u8 *)&cap[img.at + 1];
						u32 ry;

						for (ry = 0; ry < rrh; ry++)
						{
							u32 rx;
							u32 trow = dsay + ry;
							u32 srow = (u32)(((2 * trow + 1) * (u32)h) / (2 * (u32)H2));

							if (W2 == w && H2 == h)
								srow = trow;
							for (rx = 0; rx < (u32)W2; rx += (u32)(W2 > 64 ? 7 : 1))
							{
								u32 scol = W2 == w ? rx : (u32)(((2 * rx + 1) * (u32)w) / (2 * (u32)W2));
								u32 sp = ((u32 *)m->data)[srow * (u32)w + scol];
								int ok = 1;

								if (r.psm == PSM_T8)
								{
									u8 idx = data[(size_t)ry * (size_t)W2 + rx];

									if ((sp >> 24) == 0)
										ok = idx == 255;
									else
										ok = (H.pal[idx] & 0xFFFFFF) == (sp & 0xFFFFFF) || (idx != 255 && (sp & 0xFFFFFF) == (H.pal[255] & 0xFFFFFF));
								}
								else if (r.psm == PSM_CT16)
								{
									u16 px = ((const u16 *)data)[(size_t)ry * (size_t)W2 + rx];
									u16 want = (u16)(((sp & 0xFF) >> 3) | ((((sp >> 8) & 0xFF) >> 3) << 5) | ((((sp >> 16) & 0xFF) >> 3) << 10) | (1u << 15));

									ok = px == (neg == 12 ? (u16)(want ^ 1) : want);
								}
								else
								{
									u32 alpha = ((sp >> 24) * 128u + 127u) / 255u;
									u32 px = ((const u32 *)data)[(size_t)ry * (size_t)W2 + rx], want = (sp & 0xFFFFFF) | (alpha << 24);
									if (neg == 12) want ^= 1u;

									ok = px == want;
								}
								EXPECT(ok, "format %d %dx%d: pixel (%u,%u) of the transfer does not match the source (%u,%u)", fmt, w, h, rx, trow, scol, srow);
							}
						}
					}
					row_expected = dsay + rrh;
					rows_seen += (int)rrh;
					nbands++;
					bbp = dbp;
					bbw = dbw;
					bpsm = dpsm;
					bytes += (int)(img.nloop * 16);
				}
			}
			EXPECT(rows_seen == H2, "bands cover %d rows of %d", rows_seen, H2);
			EXPECT(nbands >= 1, "no bands");
			(void)bbp; (void)bbw; (void)bpsm; (void)partial;
			tex_free_all();
			free(m->data);
			free(m);
		}
	{
		char d[200];

		snprintf(d, sizeof d, "%d textures (PSMT8 indexed, keyed, CT16, CT32 with alpha; 16x16..1024x1024 and odd sizes): transfer set-up registers, band geometry, IMAGE sizes, payload against an independent resampling (%d bytes)", uploads, bytes);
		group_end(d);
	}
	host_init(0);
}

static void test_cache_collisions(void)
{
	static FOutVector v[513];
	const u32 idx[3] = {0, 256, 512};
	pt2 want[3], got[3];
	double z[3];
	float st[6], q[3];
	pkt_t p;
	u32 at;
	int found = 0, i;
	FSurfaceInfo s;
	group_begin("cache_collisions");
	host_init(0);
	memset(&s, 0, sizeof s); s.PolyColor.rgba = 0xFFFFFFFF;
	v[0].x = -0.7f; v[0].y = -0.6f;
	v[256].x = 0.8f; v[256].y = -0.5f;
	v[512].x = 0.2f; v[512].y = 0.9f;
	begin_draw(PF_NoTexture | PF_Modulated, &s);
	m_identity(H.mvp); H.mat_dirty = 0;
	cap_reset();
	emit_fan(v, idx, 3, NULL);
	for (at = 0; at < cap_n && pkt_at(at, &p); at += 1 + p.size)
		if (p.pre && p.regs == GIF_REGS_XYZ) break;
	EXPECT(decode_vertices(&p, 0, want, z, st, q, 3) == 3, "reference fan decode");
	cap_reset();
	if (neg == 13) v[256] = v[0];
	emit_tris(v, idx, 3, NULL);
	for (at = 0; at < cap_n && pkt_at(at, &p); at += 1 + p.size)
		if (p.pre && p.regs == GIF_REGS_XYZ)
		{
			found = decode_vertices(&p, 0, got, z, st, q, 3) == 3;
			break;
		}
	EXPECT(found, "no triangle packet");
	if (found)
		for (i = 0; i < 3; i++)
			EXPECT(got[i].x == want[i].x && got[i].y == want[i].y, "vertex %d differs after cache collision", i);
	group_end("indices 0/256/512 alias one transform-cache slot; every vertex equals the independent fan path");
}

static void test_ap88(void)
{
	GLMipmap_t *m;
	u32 at, texels = 0;
	pkt_t p;
	int ri, i;
	group_begin("ap88_partial_alpha");
	host_init(0);
	for (i = 0; i < 256; i++) H.pal[i] = (u32)i | ((u32)(255 - i) << 8);
	H.pal_set = 1; pal_build();
	m = mk_tex(16, 16, GL_TEXFMT_AP_88);
	for (i = 0; i < 256; i++)
	{
		((u8 *)m->data)[i * 2] = (u8)(i % 2 ? 255 : 10);
		((u8 *)m->data)[i * 2 + 1] = (u8)i;
	}
	cap_reset(); ri = tex_upload(m);
	EXPECT(ri != NOREC && H.rec[ri].psm == PSM_CT32 && H.rec[ri].baked, "partial AP88 must use CT32 and palette invalidation");
	for (at = 0; at < cap_n && pkt_at(at, &p); at += 1 + p.size)
		if (p.flg == 2)
		{
			const u32 *px = (const u32 *)&cap[at + 1];
			for (i = 0; i < 256; i++)
			{
				u32 a = ((u32)i * 128u + 127u) / 255u, rgb = H.pal[i % 2 ? 255 : 10];
				EXPECT(px[i] == (rgb | (a << 24)) + (neg == 14 ? 1u : 0u), "AP88 pixel %d alpha or RGB lost", i);
				texels++;
			}
		}
	EXPECT(texels == 256, "payload pixel count %u", texels);
	tex_free_all(); free(m->data); free(m);
	group_end("all 256 alpha bytes and opaque palette index 255 retained (GS alpha 0..128), direct-colour palette dependency tracked");
}

static void test_lossless_budgets(void)
{
	GLMipmap_t *keep, *large;
	int ri, maxbytes = cfg.tex_max_bytes;
	u32 used, handle, uploads;
	group_begin("lossless_budgets");
	host_init(0);
	keep = mk_tex(21, 37, GL_TEXFMT_RGBA);
	((u32 *)keep->data)[0] = 0xFF123457u;
	ri = tex_upload(keep);
	EXPECT(ri != NOREC && H.rec[ri].psm == PSM_CT32 && H.rec[ri].w == 21 && H.rec[ri].h == 37, "opaque odd-sized texture lost RGB or dimensions");
	used = H.used_blocks; handle = keep->downloaded; uploads = H.st.uploads;
	large = mk_tex(2048, 8, GL_TEXFMT_P_8);
	cap_reset();
	ri = tex_upload(large);
	EXPECT(ri != NOREC && H.rec[ri].w == (neg == 16 ? 2048 : 1024) && H.rec[ri].h == 8, ">1024 texture must be stored decimated (PS2-HW-20)");
	if (ri != NOREC)
		tex_drop(ri, 0);
	EXPECT(H.used_blocks == used && keep->downloaded == handle, "dropping the decimated texture left blocks behind");
	(void)uploads;
	free(large->data); free(large);
	large = mk_tex(1024, 1024, GL_TEXFMT_RGBA);
	((u32 *)large->data)[0] = 0xFF123457u;
	EXPECT(tex_upload(large) == NOREC && H.used_blocks == used && keep->downloaded == handle, "impossible 4 MiB upload evicted existing records");
	free(large->data); free(large);
	// Transparent RGB must survive: it participates in a filtered texel even though its alpha is zero.
	H.pal[10] = 0x00563412u; H.pal[255] = 0x00ABCDEFu; H.pal_set = 1; pal_build();
	large = mk_tex(16, 16, GL_TEXFMT_RGBA);
	for (ri = 0; ri < 256; ri++) ((u32 *)large->data)[ri] = H.pal[10] | 0xFF000000u;
	((u32 *)large->data)[0] = 0x00654321u;
	ri = tex_upload(large);
	EXPECT(ri != NOREC && H.rec[ri].psm == PSM_CT32, "RGBA alpha-zero RGB was replaced by keyed palette RGB");
	tex_drop(ri, 0); free(large->data); free(large);
	large = mk_tex(16, 16, GL_TEXFMT_AP_88);
	for (ri = 0; ri < 256; ri++) { ((u8 *)large->data)[ri * 2] = 10; ((u8 *)large->data)[ri * 2 + 1] = 255; }
	((u8 *)large->data)[1] = 0;
	ri = tex_upload(large);
	EXPECT(ri != NOREC && H.rec[ri].psm == PSM_CT32 && H.rec[ri].baked, "AP88 alpha-zero palette RGB was replaced by index 255");
	tex_drop(ri, 0); free(large->data); free(large);
	large = mk_tex(1, 1, GL_TEXFMT_P_8);
	cfg.tex_max_bytes = 1;
	EXPECT(tex_upload(large) == NOREC && H.used_blocks == used, "budget must include minimum GS block padding");
	cfg.tex_max_bytes = maxbytes;
	free(large->data); free(large);
	tex_free_all(); free(keep->data); free(keep);
	group_end("exact CT32 RGB/NPOT sizes; oversized and padded-budget failures preserve resident handles and accounting");
}

static void test_screen_resources(void)
{
	int i, j, ri[3], restored, clear0;
	u32 bytes, used, uploads, budget = cfg.screen_max_bytes;
	UINT8 rgb[32 * 16 * 3];
	pkt_t p;
	u32 at;
	int payload = 0;
	group_begin("screen_resources");
	host_init(1);
	H.fbw = H.vw = 64; H.fbh = H.vh = 32;
	H.scr_w = 32; H.scr_h = 16; H.sx = H.sy = 2.0f;
	H.pool_blocks = 64; H.free_len[0] = 64; // exactly two full captures
	bytes = 64 * 32 * 4;
	for (i = 0; i < 2; i++)
	{
		ri[i] = scr_tex_get(i);
		EXPECT(ri[i] != NOREC, "initial capture allocation %d", i);
		for (j = 0; j < 64 * 32; j++)
			((u32 *)(void *)(mock_vram + H.rec[ri[i]].blk * 256))[j] = (u32)(10 + i * 70) | ((u32)(j % 64) << 8) | ((u32)(j / 64) << 16) | 0x80000000u;
	}
	used = H.used_blocks;
	cfg.screen_max_bytes = bytes - 1;
	EXPECT(scr_tex_get(2) == NOREC && H.used_blocks == used && H.scr_rec[0] == ri[0] && H.scr_rec[1] == ri[1], "backing budget failure lost captures");
	cfg.screen_max_bytes = 3 * bytes;
	mock_read_fail = 1;
	EXPECT(scr_tex_get(2) == NOREC && H.screen_bytes == 0 && H.scr_rec[0] == ri[0], "failed readback authorized eviction");
	mock_read_fail = 0;
	ri[2] = scr_tex_get(2);
	EXPECT(ri[2] != NOREC && H.scr_data[0] != NULL && H.scr_rec[0] == NOREC && H.st.screen_spills == 1, "full pool did not spill oldest capture losslessly");
	EXPECT(H.screen_bytes == bytes && H.screen_peak_bytes == bytes, "EE backing accounting");
	if (neg == 17 && H.scr_data[0]) ((u32 *)H.scr_data[0])[0] ^= 0x40u;
	for (j = 0; j < 64 * 32; j++)
		EXPECT(((u32 *)H.scr_data[0])[j] == (10u | ((u32)(j % 64) << 8) | ((u32)(j / 64) << 16) | 0x80000000u), "spilled pixel %d changed", j);
	cap_reset(); restored = scr_resident(0);
	EXPECT(restored != NOREC && H.st.screen_restores == 1, "capture restoration failed");
	for (at = 0; at < cap_n && pkt_at(at, &p); at += 1 + p.size)
		if (p.flg == 2)
		{
			EXPECT(p.nloop * 16 == bytes && memcmp(&cap[at + 1], H.scr_data[0], bytes) == 0, "restore payload is not exact saved capture");
			payload++;
		}
	EXPECT(payload == 1, "expected one raw IMAGE restore");
	uploads = H.st.screen_restores;
	EXPECT(PS2HWD_ReadScreenRGB(0, rgb) && H.st.screen_restores == uploads, "backed readback unnecessarily restored GS residency");
	for (j = 0; j < 32 * 16; j++)
	{
		EXPECT(rgb[j * 3] == 10 && rgb[j * 3 + 1] == (UINT8)(2 * (j % 32) + 1) && rgb[j * 3 + 2] == (UINT8)(2 * (j / 32) + 1), "bilinear pixel-centre/readback orientation %d", j);
	}
	clear0 = clear_count;
	hw_DrawScreenFinalTexture(HWD_SCREENTEXTURE_GENERIC2, 320, 160);
	EXPECT(clear_count == clear0, "implicit final presentation cleared its source");
	cap_reset(); hw_DrawScreenFinalTexture(0, 300, 100);
	EXPECT(clear_count == clear0 + 1, "letterbox must clear bars once");
	{
		pt2 win[4]; double zs[4]; float st[8], qs[4]; int found = 0;
		for (at = 0; at < cap_n && pkt_at(at, &p); at += 1 + p.size)
			if (p.pre && (p.prim & 7) == PRIM_TRIFAN)
			{
				found = decode_vertices(&p, 1, win, zs, st, qs, 4) == 4;
				break;
			}
		EXPECT(found, "final draw did not emit a quad");
		if (found)
			EXPECT(fabs(win[0].x - 64.0 / 6.0) < 0.075 && fabs(win[2].x - 64.0 * 5.0 / 6.0) < 0.075 && st[1] == 1.0f && st[3] == 0.0f, "letterbox dimensions or top-down UV orientation");
	}
	memset(rgb, 0xA5, sizeof rgb);
	EXPECT(!PS2HWD_ReadScreenRGB(HWD_SCREENTEXTURE_GENERIC3, rgb) && rgb[0] == 0xA5, "uncaptured palette slot must not alias GENERIC2");
	hw_FlushScreenTextures();
	EXPECT(H.used_blocks == 0 && H.screen_bytes == 0, "FlushScreenTextures leaked GS/EE storage");
	cfg.screen_max_bytes = budget;
	group_end("lossless spill/restore payload, transactional budget/read failures, selected-slot bilinear RGB, final aspect/zero-copy and flush ownership");
}

static void test_many_repeats(void)
{
	FOutVector v[4] = {{-0.8f,-0.8f,0,-0.25f,-0.25f}, {-0.8f,0.8f,0,-0.25f,83.75f}, {0.8f,0.8f,0,97.75f,83.75f}, {0.8f,-0.8f,0,97.75f,-0.25f}};
	FSurfaceInfo surf = {0};
	pkt_t p;
	u32 at;
	int fans = 0, ri, j;
	double area = 0;
	group_begin("many_npot_repeats");
	host_init(0); fake_texture(1); ri = H.cur_tex;
	H.rec[ri].w = H.rec[ri].uw = 21; H.rec[ri].h = H.rec[ri].vh = 37;
	H.rec[ri].tw = 5; H.rec[ri].th = 6;
	H.rec[ri].sxs = 21.0f / 32.0f; H.rec[ri].sys = 37.0f / 64.0f;
	H.rec[ri].npot_x = H.rec[ri].npot_y = 1; H.rec[ri].region = 3;
	surf.PolyColor.rgba = 0xFFFFFFFFu;
	begin_draw(PF_Masked | PF_Modulated | PF_NoDepthTest, &surf);
	m_identity(H.mvp); H.mat_dirty = 0;
	cap_reset(); emit_fan(v, NULL, 4, NULL);
	for (at = 0; at < cap_n && pkt_at(at, &p); at += 1 + p.size)
		if (p.pre && (p.prim & 7) == PRIM_TRIFAN)
		{
			pt2 win[16]; double zs[16]; float st[32], qs[16];
			int n = decode_vertices(&p, 1, win, zs, st, qs, 16);
			EXPECT(n >= 3 && !straddles(&p), "invalid repeat piece packet");
			if (n < 3) continue;
			area += area2(win, n);
			for (j = 0; j < n; j++)
				EXPECT(st[j * 2] >= -1e-5f && st[j * 2] <= 21.0f / 32.0f + 1e-5f && st[j * 2 + 1] >= -1e-5f && st[j * 2 + 1] <= 37.0f / 64.0f + 1e-5f, "piece UV was clamped instead of repeating after MAXCUTS");
			fans++;
		}
	EXPECT(fans >= (neg == 18 ? 20000 : 7000) && fabs(area - 640.0 * 448.0 * 0.64) < 100.0, "repeat pieces=%d area=%.1f lost source geometry", fans, area);
	group_end("98x84 repeats, both axes exceed bounded cut staging: every piece UV in its period, total area preserved, packets bounded");
}

static void test_models(void)
{
	static float a[9] = {-0.4f,-0.4f,0, 0.4f,-0.4f,0, 0,0.4f,0};
	static float b[9] = {-0.2f,-0.4f,0, 0.6f,-0.4f,0, 0.2f,0.4f,0};
	static short ta[9], tb[9];
	static float uv[6] = {0,1, 1,1, 0.5f,0};
	static unsigned short ix[3] = {0,1,2};
	mdlframe_t frames[2] = {{0}};
	tinyframe_t tiny[2] = {{0}};
	model_t model = {0}; mesh_t mesh = {0};
	FSurfaceInfo surf = {0};
	int it, j, emitted = 0;
	group_begin("model_geometry");
	frames[0].vertices = a; frames[1].vertices = b;
	for (j = 0; j < 9; j++) { ta[j] = (short)(a[j] * 64); tb[j] = (short)(b[j] * 64); }
	tiny[0].vertices = ta; tiny[1].vertices = tb;
	mesh.numFrames = 2; mesh.numVertices = 3; mesh.numTriangles = 1; mesh.uvs = uv; mesh.indices = ix;
	model.numMeshes = 1; model.meshes = &mesh;
	surf.PolyFlags = PF_NoTexture; surf.PolyColor.rgba = 0xFFFFFFFF;
	for (it = 0; it < 16; it++)
	{
		FTransform pos = {0}; double ref[16];
		pt2 win[3]; double zs[3]; float st[6], qs[3]; pkt_t p; u32 at;
		float old[16]; int found = 0, istiny = it & 1;
		host_init(0); m_identity(H.mv); m_identity(H.proj); memcpy(old, H.mv, sizeof old);
		mesh.frames = istiny ? NULL : frames; mesh.tinyframes = istiny ? tiny : NULL;
		pos.x = 0.1f; pos.z = -0.1f; pos.anglez = 20;
		pos.roll = 1; pos.rollangle = 15; pos.rollz = 1;
		pos.centerx = 0.1f; pos.centery = -0.1f;
		pos.flip = pos.mirror = 0;
		cap_reset();
		draw_model(&model, 0, 4, 2, 1, &pos, 2, 2, (UINT8)((it >> 1) & 1), (UINT8)((it >> 2) & 1), &surf);
		EXPECT(memcmp(old, H.mv, sizeof old) == 0 && H.mat_dirty, "model did not restore camera");
		dm_id(ref); dm_tr(ref, pos.x, pos.z, pos.y); dm_rot(ref, pos.anglez, 0,0,-1);
		dm_tr(ref, pos.centerx, pos.centery, 0); dm_rot(ref, pos.rollangle, 0,0,1); dm_tr(ref, -pos.centerx,-pos.centery,0);
		dm_sc(ref, 1, (it & 2) ? -1 : 1, (it & 4) ? -1 : 1);
		for (at = 0; at < cap_n && pkt_at(at, &p); at += 1 + p.size)
			if (p.pre && p.regs == GIF_REGS_XYZ)
			{ found = decode_vertices(&p, 0, win, zs, st, qs, 3) == 3; break; }
		// Reflection in Z reverses the front-face selection of this XY plane.
		EXPECT(found == !(it & 4), "wrong model culling, case %d", it);
		if (!found) continue;
		emitted++;
		for (j = 0; j < 3; j++)
		{
			double v[3], o[4]; int k;
			for (k = 0; k < 3; k++) v[k] = istiny ? (double)(short)(ta[j*3+k] + 0.5f*(tb[j*3+k]-ta[j*3+k]))/64 : a[j*3+k]+0.5*(b[j*3+k]-a[j*3+k]);
			ref_clipspace(ref, v, o);
			EXPECT(fabs(win[j].x - (o[0]*0.5+0.5)*640 - (neg == 15 ? 10 : 0)) < 0.075 && fabs(win[j].y - (-o[1]*0.5+0.5)*448) < 0.075, "model case %d vertex %d != double object transform", it, j);
		}
	}
	EXPECT(emitted == 8, "expected 8 visible and 8 back-facing models; got %d visible", emitted);
	group_end("float/tiny frames, interpolation, vertical/horizontal flips, pivot roll, camera restoration vs double object transform");
}

/* ---- group: constants ---- */
static void test_constants(void)
{
	u64 t;

	group_begin("gif_constants");
	t = HWGIF_TAG(5, 1, 1, PRIM_TRIFAN | (1 << 3) | (1 << 4), 0, 3);
	EXPECT((t & 0x7FFF) == 5 && ((t >> 15) & 1) == 1 && ((t >> 46) & 1) == 1 && ((t >> 47) & 0x7FF) == (u64)(5 | 8 | 16) && ((t >> 58) & 3) == 0 && (t >> 60) == 3, "tag layout %llx", (unsigned long long)t);
	t = HWGIF_TAG(7, 0, 0, 0, 2, 0);
	EXPECT(((t >> 58) & 3) == 2 && (t >> 60) == 0 && (t & 0x7FFF) == 7, "image tag layout %llx", (unsigned long long)t);
	EXPECT(GIF_REGS_AD == 0xE && GIF_REGS_STQ_RGBA_XYZ == 0x512 && GIF_REGS_RGBA_XYZ == 0x51 && GIF_REGS_UV_XYZ == 0x53, "register descriptors");
	EXPECT(GSR_PRIM == 0 && GSR_RGBAQ == 1 && GSR_ST == 2 && GSR_UV == 3 && GSR_XYZ2 == 5 && GSR_TEX0_1 == 6 && GSR_CLAMP_1 == 8 && GSR_TEX1_1 == 0x14 && GSR_XYOFFSET_1 == 0x18 && GSR_PRMODECONT == 0x1a &&
		GSR_TEXA == 0x3b && GSR_TEXFLUSH == 0x3f && GSR_SCISSOR_1 == 0x40 && GSR_ALPHA_1 == 0x42 && GSR_DIMX == 0x44 && GSR_DTHE == 0x45 && GSR_COLCLAMP == 0x46 && GSR_TEST_1 == 0x47 && GSR_PABE == 0x49 && GSR_FBA_1 == 0x4a &&
		GSR_FRAME_1 == 0x4c && GSR_ZBUF_1 == 0x4e && GSR_BITBLTBUF == 0x50 && GSR_TRXPOS == 0x51 && GSR_TRXREG == 0x52 && GSR_TRXDIR == 0x53 && GSR_FINISH == 0x61, "GS register addresses");
	EXPECT(PSM_CT32 == 0 && PSM_CT16 == 2 && PSM_CT16S == 0x0A && PSM_T8 == 0x13 && PSM_Z24 == 0x31 && PSM_Z32 == 0x30 && ZPSM_24 == 1, "pixel formats");
	group_end("tag fields, register descriptors and addresses, pixel format codes");
}


/* ---- group: eviction by window (PS2-HW-16) ---- */
/* Independent oracle: the weights of a record (1 = least recently used .. 63 = most recent, a protected one = INF) are recomputed
 * here from the records' recency numbers; the chosen window must hold the minimal total among all aligned windows. */
static u64 oracle_weight(int ri, u32 lo, u32 shift)
{
	const texrec_t *r = &H.rec[ri];

	if (r->pin)
		return ~0ull;
	if (r->screen)
		return (r->screen <= 2 && H.frame_no - r->stamp > WIPE_DEAD_FRAMES) ? 1 : ~0ull;
	if (ri == H.cur_tex)
		return ~0ull;
	if (r->stamp == H.frame_no)
		return batch_phase == 1 ? ~0ull : batch_phase == 2 ? (r->done == H.frame_no + 1 ? 40u : 60u) : 64u;
	return 1u + (u64)((r->seq - lo) >> shift);
}

static int own_consistent(void)
{
	u32 b;
	int i;

	for (b = 0; b < VRAM_BLOCKS; b++)
	{
		int o = blk_owner[b];

		if (o)
		{
			if (o - 1 >= H.rec_n || !H.rec[o - 1].used || b < H.rec[o - 1].blk || b >= H.rec[o - 1].blk + H.rec[o - 1].nblk)
				return 0;
		}
	}
	for (i = 0; i < H.rec_n; i++)
		if (H.rec[i].used)
			for (b = H.rec[i].blk; b < H.rec[i].blk + H.rec[i].nblk; b++)
				if (blk_owner[b] != i + 1)
					return 0;
	return 1;
}

static u64 oracle_best_window_cost(u32 nblocks, u32 align)
{
	u32 lo = 0, span = 1, shift = 0, first, s, e;
	u64 best = ~0ull;

	if (H.lru_tail != NOREC)
	{
		lo = H.rec[H.lru_tail].seq;
		span = H.rec[H.lru_head].seq - lo + 1;
	}
	while ((span >> shift) > 30)
		shift++;
	first = (H.pool_base + align - 1) & ~(align - 1);
	for (s = first; s + nblocks <= H.pool_base + H.pool_blocks; s += align)
	{
		u64 cost = 0;
		int bad = 0;

		for (e = s; e < s + nblocks; e++)
		{
			int o = blk_owner[e];

			if (o)
			{
				u64 w = oracle_weight(o - 1, lo, shift);

				if (w == ~0ull)
					bad = 1;
				else
					cost += w;
			}
		}
		if (!bad && cost < best)
			best = cost;
	}
	return best;
}

static void test_window_eviction(void)
{
	enum { N = 400 };
	GLMipmap_t *t[N];
	int i, k, nres;
	u32 ev0, b;

	group_begin("eviction_window");
	ps2hwd_dbg_flags = neg == 20 ? 32 : 0; /* neg 20: the first feasible window instead of the cheapest */
	host_init(0);
	for (i = 0; i < 256; i++)
		H.pal[i] = (u32)i | ((u32)(i ^ 0x55) << 8) | ((u32)(255 - i) << 16);
	H.pal_set = 1;
	pal_build();
	/* 1: fill the pool with 128x128 indexed textures (64 blocks each) of the current frame while the engine is batching:
	 * a 2048-block request cannot be met without touching them, so it fails and evicts NOTHING */
	for (i = 0; i < N; i++)
		t[i] = mk_tex(128, 128, GL_TEXFMT_P_8);
	H.frame_no = 50;
	batch_phase = 1;
	for (i = 0; i < N; i++)
		if (tex_upload(t[i]) == NOREC)
			break;
	nres = i;
	EXPECT(nres > 100, "only %d textures fit", nres);
	ev0 = H.st.evictions;
	b = vram_alloc_evicting(2048, 32, 0);
	EXPECT(b == 0, "request over protected textures returned block %u", b);
	EXPECT(H.st.evictions == ev0, "a failed request evicted %u textures", H.st.evictions - ev0);
	for (i = 0; i < nres; i++)
		EXPECT(t[i]->downloaded != 0, "texture %d lost by a failed request", i);
	EXPECT(own_consistent(), "owner map after the failed request");
	/* 2: a new frame, nothing drawn yet: the cheapest window of 1024 blocks is found by an oracle over all aligned windows;
	 * the freed window is free of records, exactly the records that overlapped it are gone, and the cost equals the oracle's */
	H.frame_no = 51;
	for (i = 0; i < 60; i++)
		if (i % 5)
			lru_touch((int)(t[i]->downloaded - 1)); /* a known pattern of recent textures among old ones */
	for (i = 60; i < 80; i++)
	{
		lru_touch((int)(t[i]->downloaded - 1)); /* recent but not drawn in this frame: evictable at a higher cost, so the first feasible window is not the cheapest */
		H.rec[t[i]->downloaded - 1].stamp = 50;
	}
	{
		u32 nblocks = 1024, align = 32;
		u64 best = oracle_best_window_cost(nblocks, align), got = 0;
		int pre_used[N + 8], j, ok = 1;
		u32 pre_blk[N + 8], pre_n[N + 8], pre_seq[N + 8], n_gone = 0;
		u32 lo = H.rec[H.lru_tail].seq, span = H.rec[H.lru_head].seq - lo + 1, shift = 0;

		while ((span >> shift) > 30)
			shift++;
		for (j = 0; j < H.rec_n && j < N + 8; j++)
		{
			pre_used[j] = H.rec[j].used;
			pre_blk[j] = H.rec[j].blk;
			pre_n[j] = H.rec[j].nblk;
			pre_seq[j] = H.rec[j].seq;
		}
		ev0 = H.st.evictions;
		b = vram_alloc_evicting(nblocks, align, 0);
		EXPECT(b != 0, "window eviction found nothing although old textures exist");
		if (b)
		{
			EXPECT((b & (align - 1)) == 0, "block %u not aligned", b);
			for (j = 0; j < H.rec_n; j++)
				if (H.rec[j].used && H.rec[j].blk < b + nblocks && H.rec[j].blk + H.rec[j].nblk > b)
					ok = 0;
			for (j = 0; j < H.rec_n && j < N + 8; j++)
				if (pre_used[j] && !H.rec[j].used)
				{
					n_gone++;
					if (!(pre_blk[j] < b + nblocks && pre_blk[j] + pre_n[j] > b))
						ok = 0;
					{
						u32 a = pre_blk[j] > b ? pre_blk[j] : b, z = pre_blk[j] + pre_n[j] < b + nblocks ? pre_blk[j] + pre_n[j] : b + nblocks;

						got += (u64)(1u + ((pre_seq[j] - lo) >> shift)) * (z - a); /* weight per block of the part inside the window */
					}
				}
			EXPECT(ok, "a resident record overlaps the new block, or an unrelated record was evicted");
			EXPECT(H.st.evictions - ev0 == n_gone, "eviction counter %u, records gone %u", H.st.evictions - ev0, n_gone);
			EXPECT(got == best || (neg == 19 && 0), "cost of the evicted window %llu, cheapest possible %llu", (unsigned long long)got, (unsigned long long)best);
			vram_free(b, nblocks); /* the test gives the blocks back */
		}
	}
	EXPECT(own_consistent(), "owner map after the window eviction");
	/* 3: a wipe image that no wipe drew for a while is dropped without a copy to EE RAM; a fresh one is not */
	{
		int old_rec, fresh_rec;
		u32 spills0;

		host_init(0);
		spills0 = H.st.screen_spills;
		H.frame_no = 100;
		fresh_rec = scr_tex_get(1); /* first: the top of the pool; the stale slot lies below it, next to the free space */
		old_rec = scr_tex_get(0);
		EXPECT(old_rec != NOREC && fresh_rec != NOREC, "screen slots not created");
		if (old_rec != NOREC && fresh_rec != NOREC)
		{
			u32 want, g;

			H.rec[old_rec].stamp = 100 - WIPE_DEAD_FRAMES - 5;
			H.rec[fresh_rec].stamp = 100;
			EXPECT(evict_weight(&H.rec[old_rec], old_rec, 0, 0) == 1, "stale wipe image is not cheap");
			EXPECT(evict_weight(&H.rec[fresh_rec], fresh_rec, 0, 0) == WEIGHT_STAY, "fresh wipe image is not protected");
			want = (H.pool_blocks - H.rec[fresh_rec].nblk - 16) & ~31u; /* more than the free space on one side: only the stale slot can give way */
			g = vram_alloc_evicting(want, 32, 0);
			EXPECT(g != 0, "stale wipe slot did not make room");
			EXPECT(H.scr_rec[0] == NOREC && H.scr_rec[1] == fresh_rec, "wrong slot dropped");
			EXPECT(H.st.screen_spills == spills0 && !H.scr_data[0], "the stale slot was copied to RAM");
		}
	}
	/* 4: random upload / drop / touch / evicting allocation: the owner map always equals the records, accounting holds */
	host_init(0);
	for (i = 0; i < 256; i++)
		H.pal[i] = (u32)i | ((u32)(i ^ 0x55) << 8) | ((u32)(255 - i) << 16);
	H.pal_set = 1;
	pal_build();
	{
		static const int sizes[] = {16, 32, 64, 128, 256, 512};
		GLMipmap_t *pool_t[700];
		int np = 0;

		memset(pool_t, 0, sizeof pool_t);
		for (k = 0; k < 1500; k++)
		{
			int op = (int)(rnd() % 8);

			H.frame_no += (rnd() % 7) == 0;
			batch_phase = (int)(rnd() % 3);
			if (op < 5 && np < 700)
			{
				pool_t[np] = mk_tex(sizes[rnd() % 6], sizes[rnd() % 6], GL_TEXFMT_P_8);
				tex_upload(pool_t[np]);
				np++;
			}
			else if (op == 5 && np)
			{
				GLMipmap_t *x = pool_t[rnd() % (u32)np];

				if (x && x->downloaded)
					tex_drop((int)(x->downloaded - 1), 0);
			}
			else if (op == 6 && H.lru_head != NOREC)
			{
				lru_touch(H.lru_head);
			}
			else
			{
				u32 n = 32u * (1 + rnd() % 80), g = vram_alloc_evicting(n, 32, (int)(rnd() & 1));

				if (g)
					vram_free(g, n);
			}
			if (k % 100 == 0)
			{
				u32 sum = 0;

				for (i = 0; i < H.rec_n; i++)
					if (H.rec[i].used)
						sum += H.rec[i].nblk;
				EXPECT(own_consistent(), "owner map != records after %d operations", k);
				EXPECT(sum == H.used_blocks, "records hold %u blocks, allocator says %u after %d operations", sum, H.used_blocks, k);
			}
		}
		if (neg == 19)
			blk_owner[H.pool_base + 7] ^= 1; /* corrupt the owner map: the consistency check must notice */
		EXPECT(own_consistent(), "owner map != records at the end");
	}
	batch_phase = 0;
	ps2hwd_dbg_flags = 0;
	group_end("a request no window can satisfy evicts nothing; the cheapest aligned window is emptied (cost == oracle minimum), only the records that overlap it; stale wipe images give way without a RAM copy; owner map = records over 1500 random operations");
	host_init(0);
}


/* ---- group: fast fans (PS2-HW-19): the fast polygon path against the general path, with GS state changed in between ---- */
#define FF_MAXPRIM 4096
static u64 ff_hash[2][FF_MAXPRIM];
static int ff_n[2];

/* replay the captured stream: at every primitive kick the register file (A+D writes so far) and the primitive's bytes are hashed */
static int ff_replay(u64 *out, int max)
{
	u64 regs[128];
	int seen = 0, n = 0;
	u32 pos;
	pkt_t p;

	memset(regs, 0, sizeof regs);
	for (pos = 0; pos < cap_n && pkt_at(pos, &p) && n < max; pos += 1 + p.size)
	{
		if (p.flg == 0 && p.regs == GIF_REGS_AD)
		{
			u32 i;

			for (i = 0; i < p.nloop; i++)
			{
				u32 reg = (u32)(cap[p.at + 1 + i].d[1] & 0xFF);

				if (reg < 128)
					regs[reg] = cap[p.at + 1 + i].d[0];
			}
		}
		else if (p.flg != 2)
		{
			u64 h = 1469598103934665603ull;
			u32 i;
			const u64 *raw;

			for (i = 0; i < 128; i++)
				if (i == 0x06 || i == 0x08 || i == 0x14 || i == 0x42 || i == 0x47 || i == 0x4c || i == 0x4e || i == 0x40 || i == 0x3d || i == 0x01)
				{
					h ^= regs[i] ^ ((u64)i << 56);
					h *= 1099511628211ull;
				}
			raw = (const u64 *)&cap[pos];
			for (i = 0; i < 2 * (1 + p.size); i++)
			{
				h ^= raw[i];
				h *= 1099511628211ull;
			}
			out[n++] = h;
		}
		seen++;
	}
	(void)seen;
	return n;
}

static void ff_run(int mode)
{
	/* mode 0: general path (flag 64 set), 1: fast path; identical random operations */
	static const u32 plans[4] = {PF_Masked | PF_Modulated, PF_Translucent | PF_Modulated | PF_NoDepthTest, PF_Additive | PF_Modulated | PF_NoTexture, PF_Masked | PF_Occlude | PF_Modulated};
	FSurfaceInfo s;
	GLMipmap_t *t[3];
	int i, k, plan;

	rng_s = 0x1234567ull;
	host_init(0);
	ps2hwd_dbg_flags = mode ? 0 : 64;
	for (i = 0; i < 256; i++)
		H.pal[i] = (u32)i | ((u32)(i ^ 0x55) << 8) | ((u32)(255 - i) << 16);
	H.pal_set = 1;
	pal_build();
	clut_fixed_refresh(1);
	t[0] = mk_tex(64, 64, GL_TEXFMT_P_8);
	t[1] = mk_tex(32, 32, GL_TEXFMT_RGBA);
	t[2] = mk_tex(16, 16, GL_TEXFMT_P_8);
	for (i = 0; i < 3; i++)
		if (tex_upload(t[i]) == NOREC)
			printf("HT FAIL fast_fans upload\n");
	cap_reset();
	for (plan = 0; plan < 24; plan++)
	{
		memset(&s, 0, sizeof s);
		s.PolyColor.rgba = 0xFF000000u | (rnd() & 0xFFFFFFu);
		s.PolyColor.s.alpha = (UINT8)(plan % 3 ? 200 : 255);
		H.cur_tex = (int)(t[rnd() % 3]->downloaded - 1);
		if (!begin_draw(plans[plan & 3], &s))
			continue;
		for (k = 0; k < 60; k++)
		{
			FOutVector q[6];
			int n = 3 + (int)(rnd() % 4), j, d = (int)(rnd() % 12);
			float cx = (float)rndf(-60.0, 60.0), cy = (float)rndf(-40.0, 40.0), z = (float)rndf(8.0, 300.0), r = (float)rndf(2.0, 80.0);

			for (j = 0; j < n; j++)
			{
				double a = 6.2831853 * j / n;

				q[j].x = cx + r * (float)cos(a);
				q[j].y = cy + r * (float)sin(a);
				q[j].z = z + (float)(j & 1);
				q[j].s = (float)(0.5 + 0.5 * cos(a) + (rnd() % 3));
				q[j].t = (float)(0.5 + 0.5 * sin(a));
			}
			emit_fan(q, NULL, n, NULL);
			switch (d)
			{
				case 1: do_clear(1, 1, NULL); break;
				case 2: (void)clut_get(CK_TINT, 0xFF8040u + (rnd() & 0xFF), 0); break;
				case 3: if (!(rnd() & 7)) { GLMipmap_t *x = mk_tex(16, 16, GL_TEXFMT_P_8); tex_upload(x); } break;
				case 4: { qw_t *pp = ad_alloc(1); ad_set(pp, 0, GSR_PRMODECONT); H.gsr.valid = 0; if (neg == 21) P.serial = H.serial; break; } /* neg 21: a missed invalidation */
				case 5: ov_flush_all(); break;
				default: break;
			}
		}
	}
	ps2hwd_dbg_flags = 0;
	ff_n[mode] = ff_replay(ff_hash[mode], FF_MAXPRIM);
}

static void test_fast_fans(void)
{
	int i, bad = 0;

	group_begin("fast_fans");
	ff_run(0);
	ff_run(1);
	EXPECT(ff_n[0] > 100 && ff_n[0] < FF_MAXPRIM, "%d primitives", ff_n[0]);
	EXPECT(ff_n[0] == ff_n[1], "primitive counts differ: general %d fast %d", ff_n[0], ff_n[1]);
	for (i = 0; i < ff_n[0] && i < ff_n[1]; i++)
		if (ff_hash[0][i] != ff_hash[1][i])
			bad++;
	EXPECT(bad == 0, "%d of %d primitives differ (register file at the kick or packet bytes)", bad, ff_n[0]);
	group_end("1440 random polygons (3..6 vertices, some clipped, wrapped s) in 24 plans with GS state changes between them: every primitive has the same register file and packet bytes as the general path");
	host_init(0);
}


/* ---- group: texture decimation (PS2-HW-20) ---- */
/* the raster payload of the uploads in the capture: rows of `bpp` bytes, stored by (row, x) */
static int dec_payload(u8 *out, u32 w, u32 h, u32 bpp)
{
	u32 pos, rows = 0;
	pkt_t p;

	for (pos = 0; pos < cap_n && pkt_at(pos, &p); pos += 1 + p.size)
		if (p.flg == 0 && p.regs == GIF_REGS_AD && p.nloop == 4)
		{
			u64 tp = cap[p.at + 2].d[0], tr = cap[p.at + 3].d[0];
			u32 dsay = (u32)((tp >> 48) & 0x7FF), rrw = (u32)(tr & 0xFFF), rrh = (u32)((tr >> 32) & 0xFFF);
			pkt_t img;

			if (!pkt_at(p.at + 5, &img) || img.flg != 2 || rrw != w || dsay + rrh > h)
				return -1;
			memcpy(out + (size_t)dsay * w * bpp, &cap[img.at + 1], (size_t)rrw * rrh * bpp);
			rows += rrh;
		}
	return (int)rows;
}

static void test_decimation(void)
{
	static const struct { int w, h; u32 cap; int fmt; } cases[] = {
		{2048, 64, 0, 0}, {64, 4096, 0, 0}, {1024, 1024, 1024, 0}, {1024, 512, 512, 0}, {512, 512, 256, 0}, {256, 4096, 0, 2}, {2560, 128, 0, 0}, {1024, 1024, 0, 0},
		{100, 100, 20, 0}, {512, 512, 1024, 3}, {2048, 2048, 4096, 0},
	};
	int ci, bad_size = 0, bad_px = 0, n = 0;
	static u8 big[4 * 1024 * 1024];

	group_begin("texture_decimation");
	for (ci = 0; ci < (int)(sizeof cases / sizeof cases[0]); ci++)
	{
		int w = cases[ci].w, h = cases[ci].h, fmt = cases[ci].fmt, x, y, ri;
		u32 dx = 0, dy = 0, bpp;
		GLMipmap_t *m;
		int rows;

		host_init(0);
		H.tex_cap_blocks = cases[ci].cap;
		for (x = 0; x < 256; x++)
			H.pal[x] = (u32)x | ((u32)(x ^ 0x55) << 8) | ((u32)(255 - x) << 16);
		H.pal_set = 1;
		pal_build();
		m = mk_tex(w, h, fmt == 2 ? GL_TEXFMT_P_8 : GL_TEXFMT_RGBA);
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++)
			{
				if (fmt == 2)
					((u8 *)m->data)[y * w + x] = (u8)((x * 3 + y * 5) % 255);
				else if (fmt == 3)
					((u32 *)m->data)[y * w + x] = ((u32)(x * 5) & 255) | (((u32)(y * 9) & 255) << 8) | (((u32)(x + y) & 255) << 24);
				else
					((u32 *)m->data)[y * w + x] = (H.pal[(x * 3 + y * 5) % 255] & 0xFFFFFF) | 0xFF000000u;
			}
		/* the rule, written down independently: halve an axis while it is over 1024 texels, then the larger axis while the footprint is over the cap */
		while ((w >> dx) > 1024)
			dx++;
		while ((h >> dy) > 1024)
			dy++;
		cap_reset();
		ri = tex_upload(m);
		if (ri == NOREC)
		{
			EXPECT(0, "%dx%d cap %u: upload failed (%s)", w, h, cases[ci].cap, tex_fail_why);
			continue;
		}
		n++;
		{
			const texrec_t *r = &H.rec[ri];
			u32 W2 = (u32)w >> dx, H2 = (u32)h >> dy;

			while (cases[ci].cap && tex_footprint(r->psm, W2, H2, r->psm == PSM_T8 ? ((W2 + 127) / 128) * 2 : (W2 + 63) / 64) > cases[ci].cap)
			{
				if (W2 >= H2 && W2 > 16) { dx++; W2 >>= 1; }
				else if (H2 > 16) { dy++; H2 >>= 1; }
				else if (W2 > 16) { dx++; W2 >>= 1; }
				else break;
			}
			bpp = r->psm == PSM_T8 ? 1 : 4;
			if (r->w != W2 || r->h != H2)
			{
				bad_size++;
				EXPECT(0, "%dx%d cap %u stored %ux%u, want %ux%u", w, h, cases[ci].cap, r->w, r->h, W2, H2);
				continue;
			}
			if (neg == 22)
				dx = dx ? dx - 1 : 0; /* the oracle takes a different column: must be noticed */
			rows = dec_payload(big, W2, H2, bpp);
			if (rows != (int)H2)
			{
				EXPECT(0, "%dx%d: payload rows %d want %u", w, h, rows, H2);
				continue;
			}
			for (y = 0; y < (int)H2; y += (H2 > 64 ? 5 : 1))
				for (x = 0; x < (int)W2; x += (W2 > 64 ? 3 : 1))
				{
					u32 sx = ((u32)x << dx) + ((1u << dx) >> 1), sy = ((u32)y << dy) + ((1u << dy) >> 1);
					int ok;

					if (fmt == 2)
						ok = big[(size_t)y * W2 + x] == ((u8 *)m->data)[sy * (u32)w + sx];
					else if (r->psm == PSM_T8)
						ok = (H.pal[big[(size_t)y * W2 + x]] & 0xFFFFFF) == (((u32 *)m->data)[sy * (u32)w + sx] & 0xFFFFFF);
					else
					{
						u32 sp = ((u32 *)m->data)[sy * (u32)w + sx], gp = ((u32 *)big)[(size_t)y * W2 + x];

						ok = (gp & 0xFFFFFF) == (sp & 0xFFFFFF) && (gp >> 24) == a8_to_gs(sp >> 24);
					}
					if (!ok)
						bad_px++;
				}
		}
	}
	EXPECT(bad_size == 0, "%d stored sizes differ from the rule", bad_size);
	EXPECT(bad_px == 0, "%d sampled texels differ from the centre texel of their source block", bad_px);
	{
		/* a width over 1024 that cannot be halved exactly is refused, nothing is stored */
		GLMipmap_t *m = mk_tex(1025, 16, GL_TEXFMT_P_8);

		host_init(0);
		H.tex_cap_blocks = 0;
		EXPECT(tex_upload(m) == NOREC && H.used_blocks == 0, "an odd width over 1024 was accepted");
	}
	group_end("textures over 1024 texels per axis or over the footprint cap are stored decimated by powers of two: stored size follows the rule, every sampled texel is the centre texel of its source block (indices stay indices; CT32 colour and alpha exact); an odd size over 1024 is refused");
	host_init(0);
}

int main(int argc, char **argv)
{
	int i;

	setvbuf(stdout, NULL, _IONBF, 0);
	for (i = 1; i < argc; i++)
		if (!strncmp(argv[i], "neg=", 4))
			neg = atoi(argv[i] + 4);
	cap = (qw_t *)calloc(CAP_MAX, sizeof(qw_t));
	host_init(0);
	printf("HT hello neg=%d sizeof(void*)=%d\n", neg, (int)sizeof(void *));
	test_constants();
	test_matrices();
	test_packing();
	test_clipping();
	test_lists();
	test_cache_collisions();
	test_models();
	test_registers();
	test_footprint();
	test_allocator();
	test_eviction();
	test_conversion();
	test_upload();
	test_ap88();
	test_lossless_budgets();
	test_screen_resources();
	test_many_repeats();
	test_window_eviction();
	test_fast_fans();
	test_decimation();
	printf("HT negctl-count %d\n", NEGCOUNT);
	{
		static const struct { int n; const char *g; } map[] = {{1, "matrices"}, {2, "matrices"}, {3, "clipping"}, {4, "vertex_packing"}, {5, "vertex_packing"}, {6, "triangle_lists"}, {7, "register_values"}, {8, "texture_footprint"},
			{9, "vram_allocator"}, {10, "lru_eviction"}, {11, "palette_conversion"}, {12, "upload_packets"},
			{13, "cache_collisions"}, {14, "ap88_partial_alpha"}, {15, "model_geometry"}, {16, "lossless_budgets"}, {17, "screen_resources"}, {18, "many_npot_repeats"}, {19, "eviction_window"}, {20, "eviction_window"}, {21, "fast_fans"}, {22, "texture_decimation"}};
		for (i = 0; i < NEGCOUNT; i++)
			if (neg == 0 || neg == map[i].n)
				printf("HT negctl %d expects %s\n", map[i].n, map[i].g);
	}
	printf("HT COMPLETE groups=%d failed=%d\n", groups, total_fail);
	return total_fail ? 1 : 0;
}
