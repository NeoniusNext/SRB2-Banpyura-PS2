/* VU0 transform diagnostic. Includes the real driver so that the probe exercises
 * its static helpers; hw_test.c supplies engine stand-ins in another object.
 * The no-load phase deliberately reproduces a missing vu0_need(), then the
 * loaded phase checks the same seeded inputs and an independent clip oracle.
 * Build/run with tools/ps2/vu0_diagnostic.py. No rendered pixels are changed. */
#include "../../src/ps2/hw/ps2_hwd.c"

/* Extra engine stand-ins absent from older hw_test.c revisions. Weak linkage
 * lets an up-to-date shared fixture provide them without duplicate symbols. */
INT32 ps2hwt_patchtag __attribute__((weak));
boolean ps2hwt_comp_old __attribute__((weak));
void __attribute__((weak)) PS2_SleepUs(UINT32 us) { DelayThread(us ? us : 1); }
void __attribute__((weak)) Z_SetHWCacheLRU(boolean on, size_t freemin, size_t cap)
{
	(void)on; (void)freemin; (void)cap;
}
void __attribute__((weak)) Z_SetRebuildCost(void **user, UINT32 cycles)
{
	(void)user; (void)cycles;
}

typedef struct
{
	u32 n, oc_diff, scalar_visible, vu_visible, common_visible, zero_w;
	u32 scalar_oracle_diff, vu_oracle_diff, pair_diff, constants_bad;
	int max_dx, max_dy, max_dz;
	float max_q_rel, max_clip_abs;
} diag_t;

typedef struct
{
	unsigned seed, index;
	FOutVector s;
	cv_t scalar, vu;
	pv_t pv;
	float q, w_terms[4];
	int xi, yi, zi;
} worst_t;

static void save_worst(worst_t *w, unsigned seed, unsigned index, const FOutVector *s,
	const cv_t *scalar, const cv_t *vu, const pv_t *pv, int xi, int yi, int zi, float q)
{
	w->seed = seed; w->index = index; w->s = *s;
	w->scalar = *scalar; w->vu = *vu; w->pv = *pv;
	w->xi = xi; w->yi = yi; w->zi = zi; w->q = q;
	w->w_terms[0] = H.mvp[3] * s->x; w->w_terms[1] = H.mvp[7] * s->y;
	w->w_terms[2] = H.mvp[11] * s->z; w->w_terms[3] = H.mvp[15];
}

static void print_worst(const char *kind, const worst_t *w)
{
	printf("EETEST VU0 WORST kind=%s seed=%u index=%u input=%.9g,%.9g,%.9g scalar_clip=%.9g,%.9g,%.9g,%.9g vu_clip=%.9g,%.9g,%.9g,%.9g\n",
		kind, w->seed, w->index, (double)w->s.x, (double)w->s.y, (double)w->s.z,
		(double)w->scalar.x, (double)w->scalar.y, (double)w->scalar.z, (double)w->scalar.w,
		(double)w->vu.x, (double)w->vu.y, (double)w->vu.z, (double)w->vu.w);
	printf("EETEST VU0 WORST_PROJECTION kind=%s scalar_xyz=%d,%d,%d vu_xyz=%d,%d,%d scalar_q=%.9g vu_q=%.9g w_terms=%.9g,%.9g,%.9g,%.9g\n",
		kind, w->xi, w->yi, w->zi, w->pv.xi, w->pv.yi, w->pv.zi, (double)w->q, (double)w->pv.q,
		(double)w->w_terms[0], (double)w->w_terms[1], (double)w->w_terms[2], (double)w->w_terms[3]);
}

static u32 drng;
static float drnd(void)
{
	drng = drng * 1664525u + 1013904223u;
	return (float)((drng >> 8) & 0xFFFF) * (1.0f / 65536.0f);
}

/* Direct comparisons avoid the scalar implementation's sum-sign-bit trick. */
static int clip_oracle(const cv_t *d)
{
	const float gx = (float)H.guard_x * (1.0f / 1024.0f);
	const float gy = (float)H.guard_y * (1.0f / 1024.0f);
	if (d->w <= 0.0f)
		return 63;
	return (d->x < -gx * d->w) | ((d->x > gx * d->w) << 1)
		| ((d->y < -gy * d->w) << 2) | ((d->y > gy * d->w) << 3)
		| ((d->z < -d->w) << 4) | ((d->z > d->w) << 5);
}

static void constants_snapshot(float *v)
{
	__asm__ volatile(
		"sqc2 $vf1, 0x00(%0)\n sqc2 $vf2, 0x10(%0)\n"
		"sqc2 $vf3, 0x20(%0)\n sqc2 $vf4, 0x30(%0)\n"
		"sqc2 $vf8, 0x40(%0)\n sqc2 $vf11, 0x50(%0)\n"
		"sqc2 $vf12, 0x60(%0)\n sqc2 $vf13, 0x70(%0)\n"
		: : "r"(v) : "memory");
}

static void constants_zero(void)
{
	static const float z[4] __attribute__((aligned(16))) = {0, 0, 0, 0};
	__asm__ volatile(
		"lqc2 $vf1, 0(%0)\n lqc2 $vf2, 0(%0)\n"
		"lqc2 $vf3, 0(%0)\n lqc2 $vf4, 0(%0)\n"
		"lqc2 $vf8, 0(%0)\n lqc2 $vf11, 0(%0)\n"
		"lqc2 $vf12, 0(%0)\n lqc2 $vf13, 0(%0)\n"
		: : "r"(z) : "memory");
}

static int constants_match(void)
{
	float got[32] __attribute__((aligned(16))), want[32];
	const float gx = (float)H.guard_x * (1.0f / 1024.0f);
	const float gy = (float)H.guard_y * (1.0f / 1024.0f);
	constants_snapshot(got);
	memcpy(want, H.mvp, 16 * sizeof(float));
	want[16] = 1.0f / gx; want[17] = 1.0f / gy; want[18] = want[19] = 1.0f;
	want[20] = P.kx; want[21] = P.ky; want[22] = P.zk; want[23] = 1.0f;
	want[24] = P.ox; want[25] = P.oy; want[26] = P.zo + P.zbias; want[27] = 0.0f;
	want[28] = want[29] = want[31] = 0.0f; want[30] = P.zmax;
	return !memcmp(want, got, sizeof want);
}

static void setup_seed(unsigned seed, float *camera)
{
	FTransform t;
	FSurfaceInfo surf;
	drng = seed * 2654435761u + 12345u;
	memset(&t, 0, sizeof t);
	t.x = (drnd() - 0.5f) * 8192.0f;
	t.y = (drnd() - 0.5f) * 8192.0f;
	t.z = drnd() * 2048.0f;
	t.anglex = (drnd() - 0.5f) * 80.0f;
	t.angley = drnd() * 360.0f;
	t.fovxangle = 70.0f + drnd() * 40.0f;
	t.scalex = t.scaley = t.scalez = 1.0f;
	xf_set_transform(&t);
	memset(&surf, 0, sizeof surf);
	surf.PolyColor.rgba = 0xFFFFFFFFu;
	if (!begin_draw(PF_NoTexture | PF_Modulated | PF_Occlude, &surf))
		printf("EETEST VU0 SETUP_FAILED seed=%u\n", seed);
	camera[0] = t.x; camera[1] = t.y; camera[2] = t.z;
}

static void run_phase(const char *name, int load)
{
	diag_t r = {0};
	worst_t worst_q = {0}, worst_z = {0};
	unsigned seed, i;
	for (seed = 1; seed <= 12; seed++)
	{
		float camera[3];
		constants_zero();
		setup_seed(seed, camera);
		if (load)
			vu0_need();
		r.constants_bad += !constants_match();
		for (i = 0; i < 4096; i += 2)
		{
			FOutVector s[2];
			cv_t ds[2], dv[2], dp[2];
			pv_t pv[2], pp[2];
			int oc[2], op[2], j, k;
			for (j = 0; j < 2; j++)
			{
				s[j].x = camera[0] + (drnd() - 0.5f) * 6000.0f;
				s[j].z = camera[1] + (drnd() - 0.5f) * 6000.0f;
				s[j].y = camera[2] + (drnd() - 0.5f) * 3000.0f;
				s[j].s = s[j].t = 0.0f;
				xform_vertex(&s[j], NULL, &ds[j]);
				oc[j] = vu0_xform(&s[j], &dv[j], &pv[j]);
			}
			vu0_xform2(&s[0], &s[1], &dp[0], &dp[1], &pp[0], &pp[1], &op[0], &op[1]);
			for (j = 0; j < 2; j++)
			{
				int so, xi, yi, zi, dd;
				float q, qd;
				scalar_xform(&s[j], &so, &xi, &yi, &zi, &q);
				r.n++;
				r.oc_diff += so != oc[j];
				r.scalar_visible += so == 0;
				r.vu_visible += oc[j] == 0;
				r.zero_w += dv[j].w == 0.0f;
				r.scalar_oracle_diff += so != clip_oracle(&ds[j]);
				r.vu_oracle_diff += oc[j] != clip_oracle(&dv[j]);
				r.pair_diff += oc[j] != op[j] || memcmp(&dv[j], &dp[j], 6 * sizeof(float)) || memcmp(&pv[j], &pp[j], sizeof(pv_t));
				for (k = 0; k < 4; k++)
				{
					float delta = fabsf(((float *)&ds[j])[k] - ((float *)&dv[j])[k]);
					if (delta > r.max_clip_abs) r.max_clip_abs = delta;
				}
				if (so || oc[j]) continue;
				r.common_visible++;
				dd = abs(pv[j].xi - xi); if (dd > r.max_dx) r.max_dx = dd;
				dd = abs(pv[j].yi - yi); if (dd > r.max_dy) r.max_dy = dd;
				dd = abs(pv[j].zi - zi);
				if (dd > r.max_dz)
				{
					r.max_dz = dd;
					save_worst(&worst_z, seed, i + j, &s[j], &ds[j], &dv[j], &pv[j], xi, yi, zi, q);
				}
				qd = fabsf((pv[j].q - q) / q);
				if (qd > r.max_q_rel)
				{
					r.max_q_rel = qd;
					save_worst(&worst_q, seed, i + j, &s[j], &ds[j], &dv[j], &pv[j], xi, yi, zi, q);
				}
			}
		}
	}
	printf("EETEST VU0 PHASE name=%s n=%u outcode_diff=%u scalar_visible=%u vu_visible=%u common_visible=%u zero_w=%u constants_bad=%u scalar_oracle_diff=%u vu_oracle_diff=%u pair_diff=%u max_dx=%d max_dy=%d max_dz=%d max_q_rel=%g max_clip_abs=%g\n",
		name, r.n, r.oc_diff, r.scalar_visible, r.vu_visible, r.common_visible, r.zero_w, r.constants_bad,
		r.scalar_oracle_diff, r.vu_oracle_diff, r.pair_diff, r.max_dx, r.max_dy, r.max_dz, (double)r.max_q_rel, (double)r.max_clip_abs);
	if (load)
	{
		print_worst("q", &worst_q);
		print_worst("z", &worst_z);
	}
}

static void standard(const char *name, int flags)
{
	ps2hwd_vu0test_t r;
	u32 seed, n = 0, oc = 0, pairs = 0;
	int dx = 0, dy = 0, dz = 0;
	float q = 0;
	ps2hwd_dbg_flags = flags;
	constants_zero();
	for (seed = 1; seed <= 12; seed++)
	{
		PS2HWD_TestVU0(4096, seed, &r);
		n += r.n; oc += r.oc_diff; pairs += r.pair_diff;
		if (r.max_dx > dx) dx = r.max_dx;
		if (r.max_dy > dy) dy = r.max_dy;
		if (r.max_dz > dz) dz = r.max_dz;
		if (r.max_q_rel > q) q = r.max_q_rel;
	}
	printf("EETEST VU0 STANDARD name=%s n=%u outcode_diff=%u pair_diff=%u max_dx=%d max_dy=%d max_dz=%d max_q_rel=%g\n",
		name, n, oc, pairs, dx, dy, dz, (double)q);
	ps2hwd_dbg_flags = 0;
}

int main(int argc, char **argv)
{
	ps2hwd_config_t c;
	(void)argc; (void)argv;
	PS2HWD_GetConfig(&c);
	c.video = PS2HWD_VIDEO_NTSC; c.fb32 = 1; c.dither = 0;
	PS2HWD_Configure(&c);
	if (!PS2HWD_Init())
	{
		printf("EETEST VU0 INIT_FAILED\nEETEST DONE\n");
		SleepThread();
		return 1;
	}
	PS2HWD_SetScreenSize(320, 200);
	standard("normal", 0);
	standard("scale256", 256);
	run_phase("no_load", 0);
	run_phase("need", 1);
	PS2HWD_Shutdown();
	printf("EETEST DONE\n");
	SleepThread();
	return 0;
}
