/* Exact baseline/current source replay, plus a separate paired EE Count probe.
 * The full production driver state, ABI and pixel-centre rule are included. */
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hardware/hw_drv.h"
#include "ps2/hw/ps2_hwd.h"
#include "ps2/hw/ps2_hwd_dbg.h"
#ifdef PARA_CULL_NATIVE
#include <tamtypes.h>
#include <kernel.h>
#include <gsKit.h>
#else
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef struct GSGLOBAL GSGLOBAL;
#endif
#include "ps2/hw/ps2_hw_priv.inc"
#include "ps2/hw/ps2_hw_fx2.h"
#include "ps2/hw/ps2_hw_quad.h"
#include "hw_para_cull_under_test.h"

static uint32_t random_state = UINT32_C(0xd5137a19);
static unsigned long queries, hidden, retained;
static uint32_t random32(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}
static float randomf(float scale)
{
	return ((float)(random32() & 0xffffffu) / 8388608.0f - 1.0f) * scale;
}
static void setup_case(ps2cull_t *cs, float *p, float *r, float *u)
{
	memset(cs, 0, sizeof *cs);
	cs->r[0][0] = cs->r[1][1] = cs->r[3][2] = 1.0f;
	cs->w2 = 8.0f;
	cs->valid = 1;
	p[0] = p[1] = -10;
	p[2] = 100;
	r[0] = u[1] = 20;
	r[1] = r[2] = u[0] = u[2] = 0;
	H.vpx = H.vpy = 0;
	H.vpw = 320;
	H.vph = 200;
}
static int compare(const char *label, const ps2cull_t *cs, const float *p, const float *r, const float *u, int expected)
{
	const int a = ReferenceParaHidden(cs, p, r, u);
	const int b = PS2HWD_ParaHidden(cs, p, r, u);
	queries++;
	hidden += b != 0;
	retained += b == 0;
	if (a != b || (expected >= 0 && a != expected))
	{
		printf("PARACULL FAIL query=%lu label=%s returns=%d/%d expected=%d p=%a,%a,%a\n",
			queries, label, a, b, expected, (double)p[0], (double)p[1], (double)p[2]);
		return 0;
	}
	return 1;
}
static int correctness(void)
{
	ps2cull_t cs;
	float p[3], r[3], u[3];
	unsigned i, j, k, l;
	static const float special[] = {0.0f, -0.0f, 1.0f, -1.0f, FLT_MIN, -FLT_MIN,
		FLT_MAX, -FLT_MAX, INFINITY, -INFINITY, NAN, 0x1p-149f, -0x1p-149f};
	setup_case(&cs, p, r, u);
	if (!compare("visible-large", &cs, p, r, u, 0)) return 0;
	// A quad spanning multiple planes has no common outcode and stays visible.
	p[0] = p[1] = -200;
	r[0] = u[1] = 400;
	if (!compare("mixed-outcodes", &cs, p, r, u, 0)) return 0;
	for (i = 0; i < 6; i++)
	{
		setup_case(&cs, p, r, u);
		if (i < 4) p[i / 2] = i & 1 ? 200 : -220;
		else cs.r[2][3] = i & 1 ? 200 : -200;
		if (!compare("shared-outside-plane", &cs, p, r, u, 1)) return 0;
	}
	// Even with every x outside, any later near/eye corner must retain it.
	setup_case(&cs, p, r, u);
	p[0] = 1000;
	r[2] = 100;
	u[2] = -96;
	if (!compare("last-corner-near-eye", &cs, p, r, u, 0)) return 0;
	for (i = 0; i < 4; i++)
		for (j = 0; j < 3; j++)
		{
			float near = cs.w2;
			setup_case(&cs, p, r, u);
			p[0] = 1000;
			if (j != 1) near = nextafterf(near, j == 0 ? -INFINITY : INFINITY);
			p[2] = near;
			if (i == 1) { p[2] += 32; r[2] = -32; }
			if (i == 2) { p[2] += 32; r[2] = u[2] = -16; }
			if (i == 3) { p[2] += 32; u[2] = -32; }
			if (!compare("near-float-boundary", &cs, p, r, u, -1)) return 0;
		}
	for (i = 0; i < 6; i++)
		for (j = 0; j < 3; j++)
		{
			float edge = 100;
			setup_case(&cs, p, r, u);
			if (j != 1) edge = nextafterf(edge, j == 0 ? -INFINITY : INFINITY);
			if (i < 4)
			{
				p[i / 2] = i & 1 ? edge : -edge;
				if (i / 2) u[1] = 0;
				else r[0] = 0;
			}
			else cs.r[2][3] = i & 1 ? edge : -edge;
			if (!compare("side-float-boundary", &cs, p, r, u, -1)) return 0;
		}
	// Small pixel boxes, both sides of the 8-pixel gate, shifted split viewports.
	for (i = 0; i < 4; i++)
		for (j = 0; j < 64; j++)
			for (k = 0; k < 16; k++)
			{
				setup_case(&cs, p, r, u);
				H.vpx = i & 1 ? 160 : 0;
				H.vpy = i & 2 ? 100 : 0;
				H.vpw = i & 1 ? 160 : 320;
				H.vph = i & 2 ? 100 : 200;
				p[0] = -0.5f + (float)j / 64.0f;
				p[1] = -0.5f + (float)k / 16.0f;
				r[0] = (float)(j % 9) * 100.0f / (H.vpw * 0.5f);
				u[1] = (float)(k % 9) * 100.0f / (H.vph * 0.5f);
				if (!compare("pixel-box-split-view", &cs, p, r, u, -1)) return 0;
			}
	// The native EE's non-IEEE cases are compared locally with its own baseline.
	for (i = 0; i < sizeof special / sizeof special[0]; i++)
		for (j = 0; j < sizeof special / sizeof special[0]; j++)
			for (k = 0; k < 16; k++)
			{
				setup_case(&cs, p, r, u);
				cs.r[k / 4][k % 4] = special[i];
				r[j % 3] = special[j];
				if (!compare("nonfinite-row-edge", &cs, p, r, u, -1)) return 0;
			}
	for (i = 0; i <
#ifdef PARA_CULL_NATIVE
		32768
#else
		262144
#endif
		; i++)
	{
		setup_case(&cs, p, r, u);
		for (j = 0; j < 3; j++)
		{
			p[j] = randomf(32767);
			r[j] = randomf(4096);
			u[j] = randomf(4096);
		}
		for (j = 0; j < 4; j++)
			for (k = 0; k < 4; k++)
				cs.r[j][k] = randomf(k == 3 ? 32767.0f : 2.0f);
		H.vpw = (i & 1) ? 160 : 320;
		H.vph = (i & 2) ? 100 : 200;
		H.vpx = (i & 4) ? 160 : 0;
		H.vpy = (i & 8) ? 100 : 0;
		if (!compare("random-transform-geometry", &cs, p, r, u, -1)) return 0;
	}
	// Actual sprite geometry: rotated right/up axes, pitch, eye crossings, tiny
	// far sprites and display offsets, under many independent current views.
	for (i = 0; i < 8192; i++)
	{
		const float yaw = randomf(3.14159265f), pitch = randomf(1.57079632f);
		const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
		setup_case(&cs, p, r, u);
		for (j = 0; j < 3; j++)
		{
			p[j] = randomf(1000);
			r[j] = randomf(16);
			u[j] = randomf(32);
		}
		cs.r[0][0] = cy;
		cs.r[0][2] = -sy;
		cs.r[1][0] = sp * sy;
		cs.r[1][1] = cp;
		cs.r[1][2] = sp * cy;
		cs.r[3][0] = cp * sy;
		cs.r[3][1] = -sp;
		cs.r[3][2] = cp * cy;
		for (l = 0; l < 4; l++)
		{
			cs.r[0][3] = randomf(400);
			cs.r[1][3] = randomf(400);
			cs.r[3][3] = randomf(400);
			if (!compare("sprite-rotated-current-view", &cs, p, r, u, -1)) return 0;
		}
	}
	printf("PARACULL PASS queries=%lu hidden=%lu retained=%lu state=%u cull=%u\n",
		queries, hidden, retained, (unsigned)sizeof H, (unsigned)sizeof(ps2cull_t));
	return 1;
}
#ifdef PARA_CULL_NATIVE
static volatile unsigned sink;
static unsigned count_now(void)
{
	unsigned n;
	__asm__ volatile("sync\n\tmfc0 %0,$9" : "=r"(n) : : "memory");
	return n;
}
#define BENCH(name, fn) \
static __attribute__((noinline, noipa)) unsigned name(const ps2cull_t *cs, const float *p, const float *r, const float *u) \
{ \
	unsigned i, sum = 0; \
	for (i = 0; i < 8192; i++) { __asm__ volatile("" : : : "memory"); sum += (unsigned)fn(cs,p,r,u); } \
	sink = sum; return sum; \
}
BENCH(bench_reference, ReferenceParaHidden)
BENCH(bench_current, PS2HWD_ParaHidden)
static int benchmark(void)
{
	static const char *const labels[] = {"large-visible", "left", "right", "bottom", "top", "near-plane", "far-plane", "eye-first", "eye-last", "small-centre", "small-no-centre", "varying-w-visible"};
	ps2cull_t cs;
	float p[3], r[3], u[3];
	unsigned pattern, rep, order;
	for (pattern = 0; pattern < sizeof labels / sizeof labels[0]; pattern++)
	{
		unsigned best[2] = {UINT32_MAX, UINT32_MAX};
		setup_case(&cs, p, r, u);
		if (pattern > 0 && pattern < 5) p[(pattern - 1) / 2] = pattern & 1 ? -220 : 200;
		if (pattern == 5 || pattern == 6) cs.r[2][3] = pattern == 5 ? -200 : 200;
		if (pattern == 7) p[2] = 4;
		if (pattern == 8) { p[0] = 1000; r[2] = 100; u[2] = -96; }
		if (pattern == 9 || pattern == 10)
		{
			p[0] = pattern == 9 ? 0.3125f : 0;
			p[1] = pattern == 9 ? -0.5f : 0;
			r[0] = u[1] = 0.02f;
		}
		if (pattern == 11) { r[2] = 10; u[2] = 20; }
		for (rep = 0; rep < 7; rep++)
			for (order = 0; order < 2; order++)
			{
				const unsigned variant = (rep + order) & 1;
				const unsigned wanted = (unsigned)ReferenceParaHidden(&cs, p, r, u) * 8192;
				unsigned start, elapsed, value;
				start = count_now();
				value = variant ? bench_current(&cs, p, r, u) : bench_reference(&cs, p, r, u);
				elapsed = count_now() - start;
				if (elapsed < best[variant]) best[variant] = elapsed;
				if (value != wanted)
				{
					printf("PARACULL FAIL benchmark=%u variant=%u value=%u expected=%u\n", pattern, variant, value, wanted);
					return 0;
				}
			}
		printf("PARACULL BENCH pattern=%s queries=8192 reference=%u current=%u\n", labels[pattern], best[0], best[1]);
	}
	return 1;
}
#endif
int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	if (!correctness()) return 1;
#ifdef PARA_CULL_NATIVE
	if (!benchmark()) return 1;
	puts("PARACULL DONE");
	SleepThread();
#endif
	return 0;
}
