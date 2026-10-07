// Host test of src/ps2/hw/ps2_hw_split.h (OPT10 HT, PS2-HW-70): the polygon cut for textures of more than 1024 rows, stored as two images.
// For random convex polygons with an affine texture mapping and random texture heights (1025..2048 rows), with and without repeat, the parts must
// (a) lie on the polygon's plane with the right s / t / y at every vertex, (b) stay inside their piece's rows of their repeat, (c) add up to the area
// of the polygon, (d) cover every interior point exactly once, and the texture row the GS would sample for that point (piece row + t' * piece rows,
// clamped) must be the row the whole texture would give (frac(t) * rows with repeat, clamp(t) without).
// usage: hw_split_hosttest [neg=1]   exit 0 = pass; with neg=1 a wrong piece scale is injected and the test must fail (negative control)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float x, y, z, s, t; } V;
#define SPLIT_VERT V
#include "../../src/ps2/hw/ps2_hw_split.h"

#define MAXP 16
typedef struct
{
	int piece, cell, n;
	V v[MAXP + 4];
} part_t;

typedef struct
{
	part_t parts[2 * SPLIT_MAXREPEATS];
	int np;
} coll_t;

static void collect(void *ctx, int piece, int cell, const V *v, int n)
{
	coll_t *c = ctx;
	part_t *p = &c->parts[c->np++];

	p->piece = piece;
	p->cell = cell;
	p->n = n;
	memcpy(p->v, v, (size_t)n * sizeof(V));
}

static unsigned long long rs = 88172645463325252ull;
static double rnd(void)
{
	rs ^= rs << 13;
	rs ^= rs >> 7;
	rs ^= rs << 17;
	return (double)(rs >> 11) / 9007199254740992.0;
}

static double area(const V *v, int n)
{
	double a = 0;
	int i;

	for (i = 0; i < n; i++)
	{
		const V *p = &v[i], *q = &v[(i + 1) % n];

		a += (double)p->x * q->z - (double)q->x * p->z;
	}
	return fabs(a) * 0.5;
}

static int inside(const V *v, int n, double x, double z, double eps)
{
	int i, pos = 0, neg = 0;

	for (i = 0; i < n; i++)
	{
		const V *p = &v[i], *q = &v[(i + 1) % n];
		double c = ((double)q->x - p->x) * (z - p->z) - ((double)q->z - p->z) * (x - p->x);

		if (c > eps)
			pos = 1;
		else if (c < -eps)
			neg = 1;
	}
	return !(pos && neg);
}

int main(int argc, char **argv)
{
	const int neg_control = argc > 1 && !strcmp(argv[1], "neg=1");
	const int heights[] = {1100, 1280, 1536, 1792, 2048, 1025, 1026};
	long polys = 0, parts = 0, samples = 0, bad = 0;
	int it;

	for (it = 0; it < 20000; it++)
	{
		const int sh = heights[it % 7], hb = sh - 1024, wrap = (it / 7) & 1, n = 3 + (int)(rnd() * 6);
		const float lo_t[2] = {0.0f, 1024.0f / (float)sh}, hi_t[2] = {1024.0f / (float)sh, 1.0f};
		float tk[2] = {(float)sh / 1024.0f, (float)sh / (float)hb};
		const double a = (rnd() - 0.5) * 0.02, b = (rnd() - 0.5) * 0.02, c0 = (rnd() - 0.5) * 4.0; // t = a x + b z + c0
		const double as = (rnd() - 0.5) * 0.02, bs = (rnd() - 0.5) * 0.02, cs = rnd();
		const double ky = (rnd() - 0.5) * 0.3, kz = (rnd() - 0.5) * 0.3;
		const double cx = (rnd() - 0.5) * 400.0, cz = (rnd() - 0.5) * 400.0, rad = 20.0 + rnd() * 300.0;
		double ang[MAXP];
		V poly[MAXP];
		V b0[MAXP + 8], b1[MAXP + 8];
		coll_t col;
		double tmin = 1e30, tmax = -1e30, area0, areasum = 0.0;
		int i, j, k;

		if (neg_control)
			tk[1] *= 1.02f; // the injected fault: the second piece's scale is wrong
		for (i = 0; i < n; i++)
			ang[i] = rnd() * 6.2831853;
		for (i = 0; i < n; i++) // sorted angles on a circle: a convex polygon
			for (j = i + 1; j < n; j++)
				if (ang[j] < ang[i])
				{
					double t = ang[i];

					ang[i] = ang[j];
					ang[j] = t;
				}
		for (i = 0; i < n; i++)
		{
			poly[i].x = (float)(cx + rad * cos(ang[i]));
			poly[i].z = (float)(cz + rad * sin(ang[i]));
			poly[i].y = (float)(ky * poly[i].x + kz * poly[i].z);
			poly[i].t = (float)(a * poly[i].x + b * poly[i].z + c0);
			poly[i].s = (float)(as * poly[i].x + bs * poly[i].z + cs);
			if (poly[i].t < tmin)
				tmin = poly[i].t;
			if (poly[i].t > tmax)
				tmax = poly[i].t;
		}
		if (tmax - tmin < 1e-3 || area(poly, n) < 100.0)
			continue; // degenerate
		if (tmax - tmin > 12.0) // not more repeats than the test wants to look at
			continue;
		polys++;
		memset(&col, 0, sizeof col);
		split_polygon(poly, n, wrap, lo_t, hi_t, tk, b0, b1, collect, &col);
		parts += col.np;
		area0 = area(poly, n);
		for (k = 0; k < col.np; k++)
		{
			const part_t *p = &col.parts[k];
			const double lo = p->cell + lo_t[p->piece], hi = p->cell + hi_t[p->piece];

			if (p->n < 3 || p->n > n + 2)
			{
				printf("part %d has %d vertices (polygon %d)\n", k, p->n, n);
				bad++;
			}
			areasum += area(p->v, p->n);
			for (i = 0; i < p->n; i++)
			{
				const V *v = &p->v[i];
				const double t_orig = (double)v->t / tk[p->piece] + lo; // undo the remap
				const double t_ref = a * v->x + b * v->z + c0, s_ref = as * v->x + bs * v->z + cs, y_ref = ky * v->x + kz * v->z;

				if (fabs(t_orig - t_ref) > 1e-3 || fabs(v->s - s_ref) > 1e-3 || fabs(v->y - y_ref) > 1e-2)
				{
					printf("vertex off the mapping: part %d piece %d cell %d dt=%g ds=%g dy=%g\n", k, p->piece, p->cell, t_orig - t_ref, v->s - s_ref, v->y - y_ref);
					bad++;
				}
				if ((wrap || p->piece) && t_ref < lo - 1e-3)
				{
					printf("part %d below its piece start\n", k);
					bad++;
				}
				if ((wrap || !p->piece) && t_ref > hi + 1e-3)
				{
					printf("part %d above its piece end\n", k);
					bad++;
				}
			}
		}
		if (fabs(areasum - area0) > area0 * 2e-4)
		{
			printf("area %g of the parts, %g of the polygon (wrap %d, sh %d, t %g..%g)\n", areasum, area0, wrap, sh, tmin, tmax);
			bad++;
		}
		for (j = 0; j < 40; j++) // interior points
		{
			double x, z, t_p, row_exp, row_got = -1;
			int cnt = 0, tri = 1 + (int)(rnd() * (n - 2));
			double u = rnd(), v2 = rnd();

			if (u + v2 > 1.0)
			{
				u = 1.0 - u;
				v2 = 1.0 - v2;
			}
			x = poly[0].x * (1.0 - u - v2) + poly[tri].x * u + poly[tri + 1].x * v2;
			z = poly[0].z * (1.0 - u - v2) + poly[tri].z * u + poly[tri + 1].z * v2;
			t_p = a * x + b * z + c0;
			{
				// not on a cut: the parts share the line, which one gets the point is not defined there
				double f = t_p - floor(t_p), edge = fabs(f - 1024.0 / sh);

				if (edge < 2e-3 || f < 2e-3 || f > 1.0 - 2e-3 || (!wrap && (fabs(t_p) < 2e-3 || fabs(t_p - 1.0) < 2e-3)))
					continue;
			}
			samples++;
			for (k = 0; k < col.np; k++)
			{
				const part_t *p = &col.parts[k];

				if (inside(p->v, p->n, x, z, 1e-7 * rad * rad))
				{
					// the t' the GS interpolates at the point: affine in (x, z) over the part, from the mapping (exact: the vertices were checked above)
					double tq = (t_p - (p->cell + lo_t[p->piece])) * tk[p->piece], rows = p->piece ? hb : 1024.0, row0 = p->piece ? 1024.0 : 0.0;

					if (tq < 0.0)
						tq = 0.0;
					if (tq > 1.0)
						tq = 1.0; // the GS clamps at the piece's edge
					row_got = row0 + tq * rows;
					cnt++;
				}
			}
			if (wrap)
				row_exp = (t_p - floor(t_p)) * sh;
			else
				row_exp = (t_p < 0.0 ? 0.0 : t_p > 1.0 ? 1.0 : t_p) * sh;
			if (cnt != 1 || fabs(row_got - row_exp) > 0.05)
			{
				printf("point (%g, %g): in %d parts, row %g, expected %g (wrap %d sh %d t %g)\n", x, z, cnt, row_got, row_exp, wrap, sh, t_p);
				bad++;
			}
		}
		if (bad > 20)
			break;
	}
	printf("%ld polygons, %ld parts, %ld sample points, %ld problems%s\n", polys, parts, samples, bad, neg_control ? " (negative control)" : "");
	return bad ? 1 : 0;
}
