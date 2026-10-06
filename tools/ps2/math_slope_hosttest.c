/* Host test: integer slope-plane setup of r_slopeq.h (PS2_OPT) against the original double code of r_plane.c (HEAD).
 * The reference functions are not copied by hand: math_slope_hosttest.py extracts them from `git show HEAD:src/r_plane.c`
 * into ref_slope.inc, which this file includes.
 *
 * Checks: RQ_SinCos accuracy, RQ_Ang2Rad == ANG2RAD, RQ_Rotate == the double offset formulas of R_FindPlane,
 * RQ_SetSlopePlane == (float) of the original ds_su/ds_sv/ds_sz/ds_slopelight/zeroheight, scaled and unscaled planes.
 * Usage: math_slope_hosttest [--cases=N]
 */
#define _USE_MATH_DEFINES
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "doomtype.h"
#include "m_fixed.h"
#include "m_vector.h"
#include "tables.h"
#include "r_slopeq.h"

#define ANG2RAD(angle) ((float)((angle)*M_PI)/ANGLE_180)

typedef struct
{
	dvector3_t dorigin, dnormdir;
	double dzdelta;
	boolean moved;
} pslope_t;

fixed_t finesine[5*FINEANGLES/4];
fixed_t *finecosine = &finesine[FINEANGLES/4];

/* the globals the reference code reads/writes */
dvector3_t ds_su, ds_sv, ds_sz, ds_slopelight;
double zeroheight;
float focallengthf;
boolean ds_solidcolor, ds_fog, ds_powersoftwo;
int nflatshiftup;
static dvector3_t slope_origin, slope_u, slope_v, slope_lightu, slope_lightv;

void P_CalculateSlopeVectors(pslope_t *slope) { (void)slope; }

void DVector3_Cross(const dvector3_t *a_1, const dvector3_t *a_2, dvector3_t *a_o)
{
	a_o->x = (a_1->y * a_2->z) - (a_1->z * a_2->y);
	a_o->y = (a_1->z * a_2->x) - (a_1->x * a_2->z);
	a_o->z = (a_1->x * a_2->y) - (a_1->y * a_2->x);
}

static void CalcSlopeLightVectors(pslope_t *slope, fixed_t xpos, fixed_t ypos, double height, float ang, angle_t plangle);
static void DoSlopeCrossProducts(void);
static void DoSlopeLightCrossProduct(void);

#include "ref_slope.inc"

/* ---------------------------------------------------------------------------------------------------------- */

static UINT64 rs = 0x9E3779B97F4A7C15ull;
static UINT64 rnd64(void)
{
	rs ^= rs << 13;
	rs ^= rs >> 7;
	rs ^= rs << 17;
	return rs;
}
static UINT32 rnd32(void) { return (UINT32)(rnd64() >> 16); }
static double rndu(void) { return (double)(rnd64() >> 11) / 9007199254740992.0; }
static double rndr(double lo, double hi) { return lo + (hi - lo) * rndu(); }

static float ulp_of(float f)
{
	int e;
	f = fabsf(f);
	if (f == 0.0f)
		return 0.0f;
	frexpf(f, &e);
	return ldexpf(1.0f, e - 24);
}

typedef struct
{
	unsigned long n, exact, ulp1, bad;
	double worst; /* max |a-b| / norm(vector) */
} stat_t;

static void cmp_vec(stat_t *st, const float *a, const dvector3_t *ref, double tol)
{
	const double r[3] = { ref->x, ref->y, ref->z };
	double norm = 0;
	for (int i = 0; i < 3; i++)
		if (fabs(r[i]) > norm)
			norm = fabs(r[i]);
	for (int i = 0; i < 3; i++)
	{
		const float rf = (float)r[i];
		const double d = fabs((double)a[i] - (double)rf);
		st->n++;
		if (a[i] == rf)
			st->exact++;
		else if (d <= ulp_of(rf) * 1.0001)
			st->ulp1++;
		if (norm > 0 && d / norm > st->worst)
			st->worst = d / norm;
		if (norm > 0 ? d / norm > tol : d > 1e-30)
			st->bad++;
	}
}

static void make_slope(pslope_t *s)
{
	double hyp;
	const int kind = (int)(rnd32() % 10);
	s->moved = false;
	s->dorigin.x = rndr(-32768, 32768);
	s->dorigin.y = rndr(-32768, 32768);
	s->dorigin.z = rndr(-4096, 8192);
	if (kind == 0)
	{ /* flat "slope" */
		s->dnormdir.x = s->dnormdir.y = 0;
		s->dzdelta = 0;
	}
	else
	{
		const double zang = rndr(0.01, kind == 9 ? 1.5 : 0.9), dir = rndr(0, 2 * M_PI);
		double nx = sin(zang) * cos(dir), ny = sin(zang) * sin(dir), nz = cos(zang);
		/* the game stores the normal as fixed_t (16.16) */
		nx = floor(nx * 65536) / 65536;
		ny = floor(ny * 65536) / 65536;
		nz = floor(nz * 65536) / 65536;
		hyp = hypot(nx, ny);
		s->dnormdir.x = -nx / hyp;
		s->dnormdir.y = -ny / hyp;
		s->dzdelta = hyp / nz;
	}
	s->dnormdir.z = 0;
}

int main(int argc, char **argv)
{
	unsigned long cases = 2000000, fails = 0;
	int strict = 0;
	stat_t su = {0}, sv = {0}, sz = {0}, sl = {0};
	unsigned long zh_n = 0, zh_bad = 0, ls_bad = 0, scaled_n = 0;
	double zh_worst = 0, ls_worst = 0;
	for (int i = 1; i < argc; i++)
		if (!strncmp(argv[i], "--cases=", 8))
			cases = strtoul(argv[i] + 8, NULL, 10);
		else if (!strcmp(argv[i], "--strict-equivalence"))
			strict = 1;

	for (int i = 0; i < 5 * FINEANGLES / 4; i++)
		finesine[i] = (fixed_t)floor(sin((i + 0.5) * 2 * M_PI / FINEANGLES) * 65536.0 + 0.5);

	/* --- sin/cos + ANG2RAD ------------------------------------------------------------------------------ */
	{
		double worst = 0;
		unsigned long angbad = 0, n = 2000000;
		for (unsigned long i = 0; i < n; i++)
		{
			UINT32 a = i < 1000 ? (UINT32)(i * 4294967ull) : rnd32();
			const float f = ANG2RAD(a);
			INT64 sq, cq;
			double sr, cr;
			if (RQ_Ang2Rad(a) != f)
			{
				if (angbad++ < 5)
					printf("ANG2RAD mismatch a=%u %.9g vs %.9g\n", a, RQ_Ang2Rad(a), f);
			}
			RQ_SinCos(f, &sq, &cq);
			sr = sin((double)f);
			cr = cos((double)f);
			if (fabs(ldexp((double)sq, -62) - sr) > 1e-9 && angbad < 8 && i < 100000)
				printf("sin mismatch f=%.9g got %.12g want %.12g | cos got %.12g want %.12g\n", f, ldexp((double)sq, -62), sr, ldexp((double)cq, -62), cr);
			if (fabs(ldexp((double)sq, -62) - sr) > worst)
				worst = fabs(ldexp((double)sq, -62) - sr);
			if (fabs(ldexp((double)cq, -62) - cr) > worst)
				worst = fabs(ldexp((double)cq, -62) - cr);
		}
		printf("sincos: %lu angles, ANG2RAD mismatches %lu, max |error| vs libm %.3g\n", n, angbad, worst);
		if (angbad || worst > 4e-16)
			fails++;
	}

	/* --- rotation of the plane offset (R_FindPlane) ----------------------------------------------------- */
	{
		unsigned long n = 4000000, diff0 = 0, diff1 = 0, big = 0;
		for (unsigned long i = 0; i < n; i++)
		{
			const int mode = (int)(i & 3);
			float x = (float)rndr(-32768, 32768), y = (float)rndr(-32768, 32768);
			UINT32 a = rnd32();
			float ang = ANG2RAD(a);
			INT64 want, got, base = (INT64)(rnd32() >> 4) - (1 << 27);
			if (i & 4) { x = (float)rndr(-8, 8); y = (float)rndr(-8, 8); }
			switch (mode)
			{
				case 0: want = (INT64)((x * cos(ang) + y * sin(ang)) * FRACUNIT); got = RQ_QToFixed(RQ_Rotate(x, y, ang, false)); break;
				case 1: want = (INT64)((-x * sin(ang) + y * cos(ang)) * FRACUNIT); got = RQ_QToFixed(RQ_Rotate(y, -x, ang, false)); break;
				case 2: { INT64 o = base; o -= (x * cos(ang) + y * sin(ang)) * FRACUNIT; want = o; got = RQ_QToFixed(RQ_FixedToQ(base) - RQ_Rotate(x, y, ang, false)); break; }
				default: { INT64 o = base; o -= (x * sin(ang) - y * cos(ang)) * FRACUNIT; want = o; got = RQ_QToFixed(RQ_FixedToQ(base) - RQ_Rotate(x, -y, ang, true)); break; }
			}
			if (got == want)
				diff0++;
			else if (got - want == 1 || want - got == 1)
				diff1++;
			else if (big++ < 5)
				printf("rotate mismatch mode %d x=%g y=%g ang=%.9g want %lld got %lld\n", mode, x, y, ang, (long long)want, (long long)got);
		}
		printf("rotate: %lu cases, equal %lu, off by one %lu, other %lu\n", n, diff0, diff1, big);
		if (big || (strict && diff1))
			fails++;
	}

	/* --- seg / view ray intersection (R_StoreWallRange) and thick-side yscale (R_RenderThickSideRange) ----- */
	{
		unsigned long n = 4000000, eq = 0, off1 = 0, bad = 0, ill = 0;
		for (unsigned long i = 0; i < n; i++)
		{
			INT32 x1 = (INT32)rndr(-30000 * 65536.0, 30000 * 65536.0), y1 = (INT32)rndr(-30000 * 65536.0, 30000 * 65536.0);
			const double len = rndr(8, (i & 1) ? 2000 : 128) * 65536.0, sa = rndr(0, 2 * M_PI);
			INT32 x2 = (INT32)((UINT32)x1 + (UINT32)(INT32)(len * cos(sa))), y2 = (INT32)((UINT32)y1 + (UINT32)(INT32)(len * sin(sa)));
			INT32 vx = (INT32)((UINT32)x1 + (UINT32)(INT32)rndr(-1500 * 65536.0, 1500 * 65536.0));
			INT32 vy = (INT32)((UINT32)y1 + (UINT32)(INT32)rndr(-1500 * 65536.0, 1500 * 65536.0));
			const UINT32 idx = rnd32() % FINEANGLES;
			const INT32 S = FINESINE(idx), C = FINECOSINE(idx);
			INT32 rx, ry, nx, ny;
			double a1, b1, c1, a2, b2, c2, det;
			a1 = FixedToDouble((fixed_t)((UINT32)y2 - (UINT32)y1));
			b1 = FixedToDouble((fixed_t)((UINT32)x1 - (UINT32)x2));
			c1 = a1 * FixedToDouble(x1) + b1 * FixedToDouble(y1);
			a2 = -FixedToDouble(S);
			b2 = FixedToDouble(C);
			c2 = a2 * FixedToDouble(vx) + b2 * FixedToDouble(vy);
			det = a1 * b2 - a2 * b1;
			/* ill-conditioned (view ray nearly parallel to the seg, |sin(angle)| < 1e-3): both results are noise */
			if (!strict && fabs(det) < 1e-3 * hypot(a1, b1))
			{
				ill++;
				continue;
			}
			rx = DoubleToFixed((b2 * c1 - b1 * c2) / det);
			ry = DoubleToFixed((a1 * c2 - a2 * c1) / det);
			RQ_RayHitsSeg(x1, y1, x2, y2, vx, vy, S, C, &nx, &ny);
			if (nx == rx && ny == ry)
				eq++;
			else if (llabs((INT64)nx - rx) <= 1 && llabs((INT64)ny - ry) <= 1)
				off1++;
			else if (bad++ < 5)
				printf("raycross mismatch: seg (%d,%d)-(%d,%d) view (%d,%d) S=%d C=%d want (%d,%d) got (%d,%d)\n", x1, y1, x2, y2, vx, vy, S, C, rx, ry, nx, ny);
		}
		printf("seg/ray intersection: %lu cases (+%lu skipped ill-conditioned): equal %lu, off by 1 LSB %lu, worse %lu\n", n - ill, ill, eq, off1, bad);
		if (bad || (strict && off1))
			fails++;
	}
	{
		unsigned long n = 20000000, bad = 0;
		for (unsigned long i = 0; i < n; i++)
		{
			const INT32 scale1 = (INT32)(rnd32() >> (rnd32() % 12)), scalestep = (INT32)rnd32() >> (8 + rnd32() % 20);
			const INT32 x1 = (INT32)(rnd32() % 320), dsx1 = (INT32)(rnd32() % 320), cols = (INT32)(rnd32() % 320);
			double scalestep_d = FixedToDouble(scalestep) / 1.0;
			double yscale = (FixedToDouble(scale1) + (x1 - dsx1) * scalestep_d) / 1.0;
			INT64 ysfix = (INT64)scale1 + (INT64)(x1 - dsx1) * scalestep;
			for (INT32 k = 0; k < cols && k < 8; k++)
			{
				const double ys = yscale;
				if (fabs(ys) < 30000.0 && DoubleToFixed(ys) != (fixed_t)ysfix)
				{
					if (bad++ < 5)
						printf("yscale mismatch %d %d %d %d\n", scale1, scalestep, x1, dsx1);
				}
				yscale += scalestep_d;
				ysfix += scalestep;
			}
		}
		printf("thick side yscale: %lu cases, mismatches %lu\n", n, bad);
		if (bad)
			fails++;
	}

	/* --- slope plane vectors ------------------------------------------------------------------------------ */
	for (unsigned long c = 0; c < cases; c++)
	{
		pslope_t sl_;
		rq_slope_t q;
		rq_planein_t in;
		rq_planeout_t out;
		fixed_t xpos, ypos, zpos, xs, ys;
		INT64 xoff, yoff;
		angle_t angle, plangle;
		int scaled = (int)(rnd32() % 4 == 0);
		const int pick = (int)(rnd32() % 8);
		make_slope(&sl_);
		xpos = (fixed_t)rndr(-32768 * 65536.0, 32767 * 65536.0);
		ypos = (fixed_t)rndr(-32768 * 65536.0, 32767 * 65536.0);
		zpos = (fixed_t)rndr(-1024 * 65536.0, 4096 * 65536.0);
		angle = rnd32();
		plangle = (pick < 5) ? 0 : (pick == 5 ? ANGLE_90 : rnd32());
		xoff = (INT64)(rndr(-512, 512) * 65536.0);
		yoff = (INT64)(rndr(-512, 512) * 65536.0);
		if (pick == 7) { xoff = (INT64)rnd32() * 8 - 17179869184ll; yoff = (INT64)rnd32() * 8 - 17179869184ll; }
		xs = (fixed_t)rndr(0.1 * 65536, 8 * 65536);
		ys = (fixed_t)rndr(0.1 * 65536, 8 * 65536);
		ds_solidcolor = ds_fog = false;
		ds_powersoftwo = (rnd32() & 1);
		nflatshiftup = 10 - (int)(rnd32() % 5);
		focallengthf = (float)rndr(80, 400);

		if (scaled)
		{
			scaled_n++;
			R_SetScaledSlopePlane(&sl_, xpos, ypos, zpos, xs, ys, xoff, yoff, angle, plangle);
		}
		else
			R_SetSlopePlane(&sl_, xpos, ypos, zpos, (fixed_t)xoff, (fixed_t)yoff, angle, plangle);
		/* the unscaled original takes fixed_t offsets */
		if (!scaled)
			xoff = (fixed_t)xoff, yoff = (fixed_t)yoff;

		RQ_LoadSlope(&q, &sl_.dorigin, &sl_.dnormdir, &sl_.dzdelta);
		in.slope = &q;
		in.xpos = xpos;
		in.ypos = ypos;
		in.zpos = zpos;
		in.xoff = xoff;
		in.yoff = yoff;
		in.angle = angle;
		in.plangle = plangle;
		in.plsin = FINESINE(plangle >> ANGLETOFINESHIFT);
		in.plcos = FINECOSINE(plangle >> ANGLETOFINESHIFT);
		in.scaled = scaled;
		in.xscale = scaled ? FixedToFloat(xs) : 1.0f;
		in.yscale = scaled ? FixedToFloat(ys) : 1.0f;
		in.lightonly = false;
		in.sfshift = 16 + (ds_powersoftwo ? nflatshiftup : 0);
		in.focallength = focallengthf;
		RQ_SetSlopePlane(&in, &out);

		{
			const double tol = 6e-7;
			cmp_vec(&su, out.su, &ds_su, tol);
			cmp_vec(&sv, out.sv, &ds_sv, tol);
			cmp_vec(&sz, out.sz, &ds_sz, tol);
			cmp_vec(&sl, out.light, &ds_slopelight, tol);
		}
		{
			const double want = zeroheight, got = ldexp((double)out.zeroheight, -40);
			const double d = fabs(want - got);
			zh_n++;
			if (d > zh_worst)
				zh_worst = d;
			if (d > 1e-9)
				zh_bad++;
			if (want != 0)
			{
				const float s1 = RQ_LightScale(320 * 320 / 320, out.zeroheight, 10 * 65536 + 123);
				const double s0 = (double)(320 * 320 / 320) / zeroheight / 21.0f * FIXED_TO_FLOAT(10 * 65536 + 123);
				const double r = fabs(s1 - s0) / fabs(s0);
				if (r > ls_worst)
					ls_worst = r;
				if (r > 5e-7)
					ls_bad++;
			}
		}
	}
	printf("slope planes: %lu cases (%lu scaled)\n", cases, scaled_n);
#define REP(name, st) printf("  %-12s comps %lu exact(float of ref) %.4f%%  within 1ulp %.4f%%  worst |d|/norm %.3g  beyond tol %lu\n", name, (st).n, 100.0 * (st).exact / (st).n, 100.0 * ((st).exact + (st).ulp1) / (st).n, (st).worst, (st).bad)
	REP("su", su);
	REP("sv", sv);
	REP("sz", sz);
	REP("slopelight", sl);
	printf("  zeroheight  worst |d| %.3g (units), beyond 1e-9: %lu / %lu\n", zh_worst, zh_bad, zh_n);
	printf("  lightscale  worst rel %.3g, beyond 5e-7: %lu\n", ls_worst, ls_bad);
	fails += (su.bad + sv.bad + sz.bad + sl.bad + zh_bad + ls_bad) != 0;
	if (strict)
	{
		const unsigned long different = su.n - su.exact + sv.n - sv.exact + sz.n - sz.exact + sl.n - sl.exact;
		printf("STRICT vector components differing from float(ref): %lu (not a pixel test)\n", different);
		fails += different != 0;
	}
	printf("RESULT %s\n", fails ? "FAIL" : "PASS");
	return fails ? 1 : 0;
}
