// Host test of src/ps2/hw/ps2_hw_quad.h (OPT10 HT, PS2-HW-72): a quad whose screen box the test calls empty must hold no pixel centre, whatever the GS
// does with its vertices: random convex quads (sprites are rectangles, rotated ones are tested too) of 0..4 pixels, at random positions, every vertex moved
// by up to 1/16 pixel in each axis (the 12.4 fixed point snapping, worst case). The brute force counts the integer points inside the moved quad (edges
// included). Also: the test must call something empty (it is not 'never hidden'), and a quad that does hold a centre is never called empty.
// usage: hw_quad_hosttest [neg=1]   exit 0 = pass; with neg=1 the margin is taken away and the snapping made larger: the test must fail (negative control)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/ps2/hw/ps2_hw_quad.h"

static unsigned long long rs = 0x9E3779B97F4A7C15ull;
static double rnd(void)
{
	rs ^= rs << 13;
	rs ^= rs >> 7;
	rs ^= rs << 17;
	return (double)(rs >> 11) / 9007199254740992.0;
}

// is the point inside the convex polygon (counter-clockwise or clockwise), edges included
static int inside(const double *x, const double *y, int n, double px, double py)
{
	int i, pos = 0, neg = 0;

	for (i = 0; i < n; i++)
	{
		const int j = (i + 1) % n;
		const double c = (x[j] - x[i]) * (py - y[i]) - (y[j] - y[i]) * (px - x[i]);

		if (c > 1e-12)
			pos = 1;
		else if (c < -1e-12)
			neg = 1;
	}
	return !(pos && neg);
}

int main(int argc, char **argv)
{
	const int neg_control = argc > 1 && !strcmp(argv[1], "neg=1");
	const double snap = neg_control ? 0.5 : 1.0 / 16.0; // the injected fault: vertices move by half a pixel
	long quads = 0, empty = 0, bad = 0, withcentre = 0;
	int it;

	for (it = 0; it < 400000; it++)
	{
		double x[4], y[4], vx[4], vy[4];
		const double ox = (rnd() - 0.5) * 20.0, oy = (rnd() - 0.5) * 20.0, w = rnd() * rnd() * 4.0, h = rnd() * rnd() * 4.0;
		const int rot = it & 1;
		float bx0 = 1e30f, bx1 = -1e30f, by0 = 1e30f, by1 = -1e30f;
		int i, found = 0;

		x[0] = ox;     y[0] = oy;
		x[1] = ox + w; y[1] = oy;
		x[2] = ox + w; y[2] = oy + h;
		x[3] = ox;     y[3] = oy + h;
		if (rot) // a quad turned by a random angle around its middle
		{
			const double a = rnd() * 6.2831853, c = cos(a), s = sin(a), mx = ox + w * 0.5, my = oy + h * 0.5;

			for (i = 0; i < 4; i++)
			{
				const double dx = x[i] - mx, dy = y[i] - my;

				x[i] = mx + dx * c - dy * s;
				y[i] = my + dx * s + dy * c;
			}
		}
		for (i = 0; i < 4; i++)
		{
			if ((float)x[i] < bx0)
				bx0 = (float)x[i];
			if ((float)x[i] > bx1)
				bx1 = (float)x[i];
			if ((float)y[i] < by0)
				by0 = (float)y[i];
			if ((float)y[i] > by1)
				by1 = (float)y[i];
		}
		quads++;
		if (neg_control)
		{
			// the fault: no margin (the header's test with the margin taken away)
			const int e = ceilf(bx0) > floorf(bx1) || ceilf(by0) > floorf(by1);

			for (i = 0; i < 4; i++)
			{
				vx[i] = x[i] + (rnd() * 2.0 - 1.0) * snap;
				vy[i] = y[i] + (rnd() * 2.0 - 1.0) * snap;
			}
			if (e)
			{
				int px, py;

				empty++;
				for (px = -30; px <= 30 && !found; px++)
					for (py = -30; py <= 30; py++)
						if (inside(vx, vy, 4, (double)px, (double)py))
						{
							found = 1;
							break;
						}
				if (found)
					bad++;
			}
			continue;
		}
		if (quad_no_centre(bx0, bx1, by0, by1))
		{
			int px, py;

			empty++;
			for (i = 0; i < 4; i++) // the worst case of the snapping, also tried at the corners of the allowed move
			{
				vx[i] = x[i] + (rnd() * 2.0 - 1.0) * snap;
				vy[i] = y[i] + (rnd() * 2.0 - 1.0) * snap;
			}
			for (px = -30; px <= 30 && !found; px++)
				for (py = -30; py <= 30; py++)
					if (inside(vx, vy, 4, (double)px, (double)py))
					{
						found = 1;
						break;
					}
			if (found)
			{
				printf("a quad called empty holds a pixel centre: box %g..%g x %g..%g\n", bx0, bx1, by0, by1);
				bad++;
			}
		}
		else
		{
			for (i = 0; i < 4; i++)
			{
				vx[i] = x[i];
				vy[i] = y[i];
			}
			for (int px = -30; px <= 30 && !found; px++)
				for (int py = -30; py <= 30; py++)
					if (inside(vx, vy, 4, (double)px, (double)py))
					{
						found = 1;
						break;
					}
			withcentre += found;
		}
		if (bad > 20)
			break;
	}
	printf("%ld quads, %ld called empty, %ld not called empty hold a centre, %ld problems%s\n", quads, empty, withcentre, bad, neg_control ? " (negative control)" : "");
	if (!neg_control && (empty < quads / 10))
	{
		printf("the test calls too few quads empty to mean anything\n");
		return 1;
	}
	return bad ? 1 : 0;
}
