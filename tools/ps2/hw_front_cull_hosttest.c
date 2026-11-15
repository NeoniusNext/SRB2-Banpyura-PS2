// Execute source-extracted published and current predicates, including the real
// HWR_FrPlaneCulled gate/horizon path and the current subsector cache reset.
// Host layout and FP are not claimed to be the target ABI or EE timing.
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef FRONT_CULL_NATIVE
#include <kernel.h>
#endif

typedef int boolean;
typedef int32_t fixed_t;
typedef struct { float x, y, z; } polyvertex_t;
typedef struct { float a, b, c, d; } fr_plane_t;
typedef struct { int numpts; polyvertex_t pts[64]; } poly_t;
typedef struct { poly_t *planepoly; } extrasubsector_t;
typedef struct { int horizon; } subsector_t;
typedef struct { int ffloors; } sector_t;

#define FR_MARGIN 1.0f
#define HWFR_NOCULL 1u
#define HWFR_NOPLANECULL 4u
#define HWFR_NOHEIGHTCACHE 0x1000u
#define FR_GATE() (frv.on && !(hwr_fr_off & HWFR_NOCULL))
#define FR_DBG() 0
#define true 1
#define false 0
static struct { fr_plane_t p[5]; int on; } frv;
static unsigned int hwr_fr_off;
static struct { unsigned int ptest, phit; } frs;
static extrasubsector_t extrasubsectors[2];
static sector_t sector, *gl_frontsector = &sector;
static unsigned int horizon_calls;
static unsigned long long reference_vertices, candidate_vertices, queries;

static float FixedToFloat(fixed_t x) { return x / (float)65536; }
static int HWR_PlaneHasHorizon(const subsector_t *sub)
{
	horizon_calls++;
	return sub->horizon;
}
#define I_OutputMsg(...) ((void)0)

#include "hw_front_cull_under_test.h"

static uint64_t random_state = UINT64_C(0x3af12b4c9876de01);
static uint32_t random32(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 7;
	random_state ^= random_state << 17;
	return (uint32_t)(random_state >> 16);
}
static float randomf(float limit)
{
	return ((float)(random32() & 0xffffffu) / 8388608.0f - 1.0f) * limit;
}

static void clear_cache(fr_heightcache_t *cache)
{
	cache->inside = cache->outside = 0;
}

static int compare(const char *label, const polyvertex_t *p, int n, float h, fr_heightcache_t *cache)
{
	const int a = ReferencePlaneHidden(p, n, h);
	const int b = HWR_FrPlaneHiddenCached(p, n, h, cache);

	queries++;
	if (a != b)
	{
		printf("FAIL %s query=%llu n=%d h=%a reference=%d candidate=%d masks=%x/%x\n", label, queries, n, (double)h, a, b, cache->inside, cache->outside);
		return 0;
	}
	return 1;
}

// Camera side planes for yaw, pitch, roll, aspect/FOV and mirrored views. The
// cache consumes these actual plane coefficients; no view/azimuth shortcut is
// substituted. X,Y,Z below are world x,height,y as in hw_front.inc.
static void view(float yaw, float pitch, float roll, float tanx, float tany,
	float eyex, float eyey, float eyez, int mirror)
{
	const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
	const float cr = cosf(roll), sr = sinf(roll);
	const float f[3] = {sy * cp, sp, cy * cp};
	const float r0[3] = {cy, 0, -sy};
	const float u0[3] = {-sy * sp, cp, -cy * sp};
	float r[3], u[3];
	int k, j;

	for (j = 0; j < 3; j++)
	{
		r[j] = (r0[j] * cr + u0[j] * sr) * (mirror ? -1.0f : 1.0f);
		u[j] = u0[j] * cr - r0[j] * sr;
	}
	for (k = 0; k < 5; k++)
	{
		float normal[3];
		for (j = 0; j < 3; j++)
		{
			if (k < 2)
				normal[j] = (k ? -r[j] : r[j]) - f[j] * tanx;
			else if (k < 4)
				normal[j] = (k == 3 ? -u[j] : u[j]) - f[j] * tany;
			else
				normal[j] = -f[j];
		}
		{
			const float inv = 1.0f / sqrtf(normal[0]*normal[0] + normal[1]*normal[1] + normal[2]*normal[2]);
			fr_plane_t *p = &frv.p[k];
			p->a = normal[0] * inv;
			p->b = normal[1] * inv;
			p->c = normal[2] * inv;
			p->d = -(p->a * eyex + p->b * eyey + p->c * eyez) + (k == 4 ? 0.1f : 0.0f);
		}
	}
	frv.on = 1;
}

static void only_height_plane(float a, float b, float d)
{
	int k;
	for (k = 0; k < 5; k++)
		frv.p[k] = (fr_plane_t){0, 0, 0, -100};
	frv.p[0] = (fr_plane_t){a, b, 0, d};
	frv.on = 1;
}

static int boundaries(void)
{
	polyvertex_t p[4] = {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}};
	fr_heightcache_t cache;
	const float heights[] = {1.0f, 1.0f, 2.0f, 0.0f, 3.0f, -2.0f,
		FLT_MAX, -FLT_MAX, INFINITY, -INFINITY, NAN, 1.0f};
	unsigned int i;
	int sign;

	for (sign = -1; sign <= 1; sign++)
	{
		only_height_plane(0, (float)sign, 0);
		clear_cache(&cache);
		for (i = 0; i < sizeof heights / sizeof heights[0]; i++)
			if (!compare("height-boundary", p, 4, heights[i], &cache))
				return 0;
		clear_cache(&cache);
		for (i = 0; i < 128; i++)
		{
			const float h = i % 3 == 0 ? nextafterf(1.0f, INFINITY) : i % 3 == 1 ? 1.0f : nextafterf(1.0f, -INFINITY);
			if (!compare("one-ulp-margin", p, 4, h, &cache))
				return 0;
		}
	}
	// Exercise indeterminate sums followed by ordinary finite thresholds.
	for (i = 0; i < 4; i++)
		p[i].x = -INFINITY;
	only_height_plane(1, 1, 0);
	clear_cache(&cache);
	if (!compare("infinite-sum", p, 4, INFINITY, &cache)
		|| !compare("infinite-to-finite", p, 4, 0, &cache))
		return 0;
	return 1;
}

static int scoped_calls(void)
{
	poly_t poly = {4, {{-16, 100, 0}, {16, 100, 0}, {16, 132, 0}, {-16, 132, 0}}};
	polyvertex_t *p = poly.pts;
	subsector_t sub = {0};
	fr_heightcache_t storage;
	unsigned int context, gate;

	// Poisoning represents reused stack bytes. The extracted production reset
	// must overwrite both masks on every subsector before the first query.
	for (context = 0; context < 256; context++)
	{
		fr_heightcache_t *cache;
		int k;
		memset(&storage, 0, sizeof storage);
		storage.inside = storage.outside = 31;
		for (k = 0; k < 5; k++)
		{
			storage.in[k] = FLT_MAX;
			storage.out[k] = -FLT_MAX;
		}
		view((float)context * 0.09f, (float)(context % 7) * 0.2f - 0.6f,
			(float)(context % 3) * 1.57f, context & 1 ? 1.0f : 1.75f,
			context & 2 ? 0.375f : 0.75f, (float)context * 13.0f,
			context & 4 ? 8000.0f : 0.0f, context & 8 ? -32000.0f : 0.0f, context & 16);
		sector.ffloors = 1;
		hwr_fr_off = 0;
		cache = BeginSubsector(&storage, 0);
		if (!compare("subsector/view-reset", p, 4, 0, cache))
			return 0;
		// Actual wrapper gates, including the existing horizon/slope exemptions,
		// invalid/missing polygon, disabled culling and the A/B cache switch.
		for (gate = 0; gate < 64; gate++)
		{
			unsigned int ah, bh, apt, aph, bpt, bph;
			const fixed_t height = (fixed_t)((int)gate - 32) * 65536;
			const int slope = (gate >> 2) & 1;
			int a, b;
			sub.horizon = gate & 1;
			poly.numpts = gate & 2 ? 2 : 4;
			extrasubsectors[0].planepoly = gate & 8 ? NULL : &poly;
			frv.on = !(gate & 16);
			hwr_fr_off = gate & 32 ? HWFR_NOPLANECULL : 0;
			frs.ptest = frs.phit = horizon_calls = 0;
			a = ReferencePlaneCulled(&sub, 0, height, slope);
			ah = horizon_calls; apt = frs.ptest; aph = frs.phit;
			frs.ptest = frs.phit = horizon_calls = 0;
			b = HWR_FrPlaneCulled(&sub, 0, height, slope, cache);
			bh = horizon_calls; bpt = frs.ptest; bph = frs.phit;
			queries++;
			if (a != b || ah != bh || apt != bpt || aph != bph)
			{
				printf("FAIL wrapper-gate context=%u gate=%u returns=%d/%d horizon=%u/%u tests=%u/%u hits=%u/%u\n", context, gate, a, b, ah, bh, apt, bpt, aph, bph);
				return 0;
			}
		}
		hwr_fr_off = HWFR_NOHEIGHTCACHE;
		if (BeginSubsector(&storage, 0) != NULL)
			return 0;
		hwr_fr_off = 0;
		if (BeginSubsector(&storage, 1) != NULL)
			return 0;
		sector.ffloors = 0;
		if (BeginSubsector(&storage, 0) != NULL)
			return 0;
	}
	return 1;
}

static int dense_fofs(void)
{
	polyvertex_t p[32];
	fr_heightcache_t cache;
	const unsigned long long a0 = reference_vertices, b0 = candidate_vertices;
	int i, j;

	view(0, 0.25f, 0.2f, 1, 0.75f, 0, 0, 0, 0);
	for (i = 0; i < 32; i++)
	{
		p[i].x = cosf((float)i * 6.2831853f / 32.0f) * 30.0f;
		p[i].y = 100.0f + sinf((float)i * 6.2831853f / 32.0f) * 30.0f;
	}
	clear_cache(&cache);
	for (j = 0; j < 64; j++)
		for (i = -80; i <= 80; i += 5)
			if (!compare("dense-3D-floors", p, 32, (float)i, &cache))
				return 0;
	{
		const unsigned long long a = reference_vertices - a0, b = candidate_vertices - b0;
		printf("dense FOF vertex tests: reference=%llu candidate=%llu\n", a, b);
		if (b >= a / 4
#ifdef FRONT_CULL_NATIVE
			&& 0
#endif
		)
		{
			puts("FAIL cache does not reduce repeated vertex work");
			return 0;
		}
	}
	// Visible distant geometry remains visible; no distance threshold exists.
	view(0, 0, 0, 1, 0.75f, 0, 0, 0, 0);
	for (i = 0; i < 32; i++)
		p[i].y += 31800.0f;
	clear_cache(&cache);
	if (!compare("far-visible", p, 32, 0, &cache) || ReferencePlaneHidden(p, 32, 0))
	{
		puts("FAIL visible distant plane");
		return 0;
	}
	return 1;
}

static int random_sequences(void)
{
	polyvertex_t p[64];
	fr_heightcache_t cache;
	unsigned int context;

	for (context = 0; context <
#ifdef FRONT_CULL_NATIVE
		4096
#else
		65536
#endif
		; context++)
	{
		const int n = 3 + (int)(random32() % 62);
		const float cx = randomf(30000), cz = randomf(30000), radius = randomf(2000);
		float heights[32];
		int i;
		view(randomf(3.14159265f), randomf(1.57f), randomf(3.14159265f),
			0.25f + fabsf(randomf(2.0f)), 0.125f + fabsf(randomf(2.0f)),
			randomf(30000), randomf(30000), randomf(30000), context & 1);
		for (i = 0; i < n; i++)
		{
			const float angle = (float)i * 6.2831853f / (float)n;
			p[i].x = cx + cosf(angle) * radius;
			p[i].y = cz + sinf(angle) * radius;
		}
		for (i = 0; i < 32; i++)
			heights[i] = randomf(32767);
		clear_cache(&cache);
		for (i = 0; i < 64; i++)
			if (!compare("random-camera/height", p, n, heights[(i * 13) & 31], &cache))
				return 0;
	}
	return 1;
}

#ifdef FRONT_CULL_NATIVE
static volatile unsigned int bench_sink;
static unsigned int count_now(void)
{
	unsigned int n;
	__asm__ volatile("sync\n\tmfc0 %0,$9" : "=r"(n) : : "memory");
	return n;
}

// Both loops reset once per subsector and consume exactly the same points,
// view coefficients, height order and query count. No vertex counters are
// present in these source-extracted native functions.
static __attribute__((noinline)) unsigned int bench_reference(const polyvertex_t *p, int n, const float *heights, int nh, int repeats)
{
	unsigned int result = 0;
	int repeat, i;
	for (repeat = 0; repeat < repeats; repeat++)
	{
		__asm__ volatile("" : : : "memory");
		for (i = 0; i < nh; i++)
			result += (unsigned int)ReferencePlaneHidden(p, n, heights[i]);
	}
	bench_sink = result;
	return result;
}
static __attribute__((noinline)) unsigned int bench_cached(const polyvertex_t *p, int n, const float *heights, int nh, int repeats)
{
	unsigned int result = 0;
	int repeat, i;
	for (repeat = 0; repeat < repeats; repeat++)
	{
		fr_heightcache_t cache;
		__asm__ volatile("" : : : "memory");
		clear_cache(&cache);
		for (i = 0; i < nh; i++)
			result += (unsigned int)HWR_FrPlaneHiddenCached(p, n, heights[i], &cache);
	}
	bench_sink = result;
	return result;
}
static int native_benchmark(void)
{
	static const int counts[] = {1, 2, 4, 8, 32, 512};
	static const int sizes[] = {4, 16, 64};
	polyvertex_t p[64];
	float heights[512];
	int pattern, size, count, i, sample;

	for (pattern = 0; pattern < 3; pattern++)
		for (size = 0; size < 3; size++)
		{
			const int n = sizes[size];
			view(0, pattern == 2 ? 0.25f : 0, pattern == 2 ? 0.2f : 0, 1, 0.75f, 0, 0, 0, 0);
			for (i = 0; i < n; i++)
			{
				p[i].x = cosf((float)i * 6.2831853f / (float)n) * 30.0f + (pattern == 1 ? 1000.0f : 0);
				p[i].y = 100.0f + sinf((float)i * 6.2831853f / (float)n) * 30.0f;
			}
			for (count = 0; count < 6; count++)
			{
				const int nh = counts[count], repeats = 8192 / nh;
				unsigned int best_a = UINT32_MAX, best_b = UINT32_MAX;
				for (i = 0; i < nh; i++)
					heights[i] = ((float)((i * 13) % nh) - (float)nh * 0.5f) * (pattern == 2 ? 320.0f : 60.0f) / (float)nh;
				for (sample = 0; sample < 5; sample++)
				{
					unsigned int t, ca, cb, a, b;
					t = count_now(); a = bench_reference(p, n, heights, nh, repeats); ca = count_now() - t;
					t = count_now(); b = bench_cached(p, n, heights, nh, repeats); cb = count_now() - t;
					if (a != b)
					{
						printf("EETEST FAIL benchmark parity pattern=%d n=%d heights=%d %u/%u\n", pattern, n, nh, a, b);
						return 0;
					}
					if (ca < best_a) best_a = ca;
					if (cb < best_b) best_b = cb;
				}
				printf("EETEST FRONT pattern=%d vertices=%d heights=%d queries=%d reference=%u cache=%u\n", pattern, n, nh, repeats * nh, best_a, best_b);
			}
		}
	return 1;
}
#endif

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	if (!boundaries() || !scoped_calls() || !dense_fofs() || !random_sequences())
		return 1;
	printf("PASS queries=%llu vertex-tests=%llu/%llu cache-bytes=%zu\n", queries, reference_vertices, candidate_vertices, sizeof(fr_heightcache_t));
#ifdef FRONT_CULL_NATIVE
	if (!native_benchmark())
		return 1;
	printf("EETEST DONE\n");
	SleepThread();
#endif
	return 0;
}
