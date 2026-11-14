/* Exact GS-event equivalence for production indexed REGLIST and model encoders.
 * production-*.h are extracted verbatim from the driver by gs_list_hosttest.py. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef float FLOAT;
#include "production-types.h"

#define BUF_QW 8192u
#define CAP_QW 131072u
#define RV_RGBAQ 1u
#define PRIM_TRI 3u
#define GSR_PRIM 0u
#define PRIM_IIP (1u << 3)
#define PRIM_TME (1u << 4)
#define PRIM_FGE (1u << 5)
#define PRIM_ABE (1u << 6)
#define PS2HWD_MAXPOLY 192
#define PS2HWD_CLIPEXTRA 12
enum { LM_NONE = 0, LM_BANDS, LM_RAMP };

static struct {
	float ox, oy, zo, zk, zbias, kx, ky, zmax, ra, rb, tsx, tsy;
	pass_t pass[4];
	int vlight, vcol, textured, npass, lmode, cutx, cuty, water, cull, wrapx, wrapy;
} P;
static struct {
	u32 wr;
	float mvp[16];
	int guard_x, guard_y;
	struct { u32 valid; } gsr;
	struct { u32 verts_out, verts_in, polys, rejected, clipped, cyc_draw; } st;
} H;
static qw_t cap[CAP_QW];
static u32 cap_n, marks[128], nmarks;
static int failures, negative, mutated;
static u64 vertices, model_vertices, list_triangles, old_qwords, new_qwords;
static u64 integrated_triangles;
static cv_t vbuf_a[PS2HWD_MAXPOLY + PS2HWD_CLIPEXTRA], vbuf_b[PS2HWD_MAXPOLY + PS2HWD_CLIPEXTRA];

#define CHECK(cond, message) do { if (!(cond)) { if (failures < 8) printf("FAIL %s line=%d\n", message, __LINE__); failures++; } } while (0)

static void pk_flush(void)
{
	CHECK(nmarks < 128, "capture mark overflow");
	marks[nmarks++] = cap_n;
	H.wr = 1; /* the next real producer buffer starts with its DMA chain tag */
}

static qw_t *pk_alloc(u32 n)
{
	qw_t *out;
	CHECK(n <= BUF_QW - 2, "oversized allocation");
	if (H.wr + n > BUF_QW) pk_flush();
	CHECK(cap_n + n <= CAP_QW, "capture overflow");
	if (cap_n + n > CAP_QW) exit(2);
	out = cap + cap_n;
	cap_n += n;
	H.wr += n;
	return out;
}

static u32 cyc(void) { return 0; }
static void pass_setup(int pass, int texture) { (void)pass; (void)texture; }
static int ov_room(int n) { (void)n; return 1; }
static void ov_flush(void) { CHECK(0, "unexpected overlay flush"); }
static void emit_overlay(const cv_t *v, int n) { (void)v; (void)n; CHECK(0, "unexpected overlay"); }
static void cut_and_emit(const cv_t *v, int n, int stage, piece_t pc, int na)
{
	(void)v; (void)n; (void)stage; (void)pc; (void)na;
	CHECK(0, "unexpected general polygon path");
}

#include "production-code.h"

static u32 random_state = 0x59F273B1u;
static u32 random_u32(void)
{
	u32 x = random_state;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	return random_state = x;
}

static float random_float(float lo, float hi)
{
	return lo + (hi - lo) * (float)(random_u32() & 0xFFFFFFu) * (1.0f / 16777216.0f);
}

static void reset(u32 wr)
{
	memset(cap, 0xA5, sizeof cap);
	cap_n = nmarks = 0;
	H.wr = wr;
	H.st.verts_out = 0;
	H.gsr.valid = ~0u;
	memset(&LS, 0, sizeof LS);
}

/* GIF's PACKED -> native conversion, independently from the native writers. */
static u64 packed_value(const qw_t *p, u32 reg, u32 q)
{
	if (reg == 2) return p->d[0];
	if (reg == 1) return (u64)(p->w[0] & 255) | ((u64)(p->w[1] & 255) << 8)
		| ((u64)(p->w[2] & 255) << 16) | ((u64)(p->w[3] & 255) << 24) | ((u64)q << 32);
	if (reg == 4) return (u64)(p->w[0] & 65535) | ((u64)(p->w[1] & 65535) << 16)
		| ((u64)((p->w[2] >> 4) & 0xFFFFFF) << 32) | ((u64)((p->w[3] >> 4) & 255) << 56);
	return (u64)(p->w[0] & 65535) | ((u64)(p->w[1] & 65535) << 16) | ((u64)p->w[2] << 32);
}

static cv_t random_vertex(int fmt)
{
	cv_t v;
	int i;
	v.w = random_float(0.01f, 32768.0f);
	v.x = random_float(-2.0f, 2.0f) * v.w;
	v.y = random_float(-2.0f, 2.0f) * v.w;
	v.z = random_float(-1.5f, 1.5f) * v.w;
	for (i = 0; i < 6; i++) v.a[i] = random_float(-100.0f, 500.0f);
	if (fmt == 6) v.a[2] = random_float(0.0f, 3.0f);
	return v;
}

static pack_t random_pack(int fmt)
{
	pack_t pk;
	memset(&pk, 0, sizeof pk);
	pk.fmt = fmt;
	pk.nreg = pack_regs(fmt);
	pk.col = random_u32();
	pk.fog = random_u32() & 255;
	pk.sb = random_float(-4.0f, 4.0f);
	pk.tb = random_float(-4.0f, 4.0f);
	pk.ramp = (int)(random_u32() & 1);
	return pk;
}

static void test_vertices(void)
{
	static const int formats[] = {0, 1, 4, 6};
	int i, f;
	for (f = 0; f < 4; f++)
		for (i = 0; i < 50000; i++)
		{
			pack_t pk = random_pack(formats[f]);
			cv_t v = random_vertex(pk.fmt);
			qw_t packed[3]; u64 native[4];
			u32 old_valid;
			int k;
			H.gsr.valid = ~0u;
			CHECK(put_vertex(packed, &v, &pk) == packed + 3, "PACKED vertex extent");
			old_valid = H.gsr.valid;
			H.gsr.valid = ~0u;
			native[3] = 0xA5A5A5A5A5A5A5A5ull;
			CHECK(put_vertex_reglist(native, &v, &pk) == native + 3, "native vertex extent");
			CHECK(native[3] == 0xA5A5A5A5A5A5A5A5ull, "native overwrote a fourth register");
			CHECK(H.gsr.valid == old_valid, "RGBAQ cache invalidation");
			for (k = 0; k < 3; k++)
			{
				u32 reg = (u32)((pack_regdesc(pk.fmt) >> (4 * k)) & 15);
				CHECK(native[k] == packed_value(&packed[k], reg, packed[0].w[2]), "scalar native register differs from PACKED");
			}
			vertices++;
		}
}

/* The unchanged pre-optimization model writer, retained as the reference. */
static void old_mdl_put(qw_t *o, const mdl_v_t *m, const qw_t *col, u32 fog4, int fmt)
{
	o[0] = m->stq;
	o[1] = fmt == 6 ? m->col4 : *col;
	o[2].d[0] = (u64)(u32)m->xi | ((u64)(u32)m->yi << 32);
	o[2].d[1] = (u64)(u32)m->zi | ((u64)fog4 << 32);
}

static mdl_v_t random_model(int fmt)
{
	mdl_v_t m;
	int j;
	memset(&m, 0, sizeof m);
	m.xi = (s32)random_u32(); m.yi = (s32)random_u32();
	m.zi = (s32)((random_u32() & 0xFFFFFFu) << (fmt == 0 ? 0 : 4));
	for (j = 0; j < 4; j++) { m.stq.w[j] = random_u32(); m.col4.w[j] = random_u32(); }
	return m;
}

/* Former per-vertex lighting stage: retain its four PACKED colour words. */
static void old_model_colour(mdl_v_t *m, u32 col, float lf)
{
	m->col4.w[0] = (u32)((float)(col & 255) * lf + 0.5f);
	m->col4.w[1] = (u32)((float)((col >> 8) & 255) * lf + 0.5f);
	m->col4.w[2] = (u32)((float)((col >> 16) & 255) * lf + 0.5f);
	m->col4.w[3] = col >> 24;
}

static void test_models(void)
{
	static const int formats[] = {0, 1, 6};
	int i, f, k;
	for (f = 0; f < 3; f++)
		for (i = 0; i < 50000; i++)
		{
			int fmt = formats[f];
			mdl_v_t m = random_model(fmt), prepared;
			qw_t col, packed[3]; u64 native[4];
			u32 fog4 = random_u32(), colour = random_u32();
			float lf = random_float(0.0f, 3.0f);
			for (k = 0; k < 4; k++) col.w[k] = (colour >> (k * 8)) & 255;
			if (fmt == 6) old_model_colour(&m, colour, lf);
			old_mdl_put(packed, &m, &col, fog4, fmt);
			prepared = m;
			mdl_prepare_v(&prepared, colour, lf, fmt);
			native[3] = ~0ull;
			CHECK(mdl_put_v(native, &prepared, fog4, fmt) == native + 3, "model native extent");
			CHECK(native[3] == ~0ull, "model fourth register overwrite");
			for (k = 0; k < 3; k++)
			{
				u32 reg = (u32)((pack_regdesc(fmt) >> (4 * k)) & 15);
				CHECK(native[k] == packed_value(&packed[k], reg, packed[0].w[2]), "model native register differs from PACKED");
			}
			model_vertices++;
		}
}

typedef struct { u32 reg; u64 value; } event_t;
static event_t expected[100000];
static u32 expected_n;
static void expect_vertex(const qw_t *packed, int fmt)
{
	int k, nr = pack_regs(fmt);
	for (k = 0; k < nr; k++)
	{
		u32 reg = (u32)((pack_regdesc(fmt) >> (4 * k)) & 15);
		expected[expected_n].reg = reg;
		expected[expected_n++].value = packed_value(&packed[k], reg, packed[0].w[2]);
	}
}

/* Decode as GIF/GS do; verify all packet extents against real ring boundaries. */
static void decode(u32 prim, u32 want_vertices)
{
	u32 pos = 0, ev = 0, nv = 0, active_prim = ~0u, mark = 0;
	while (pos < cap_n)
	{
		qw_t *p = &cap[pos];
		u32 nr = (u32)(p->d[0] >> 60), flg = (u32)(p->d[0] >> 58) & 3;
		u32 loops = (u32)p->d[0] & 32767, total, qw, k;
		u64 desc = p->d[1];
		u32 q = 0x3F800000u;
		if (!nr) nr = 16;
		total = loops * nr;
		qw = 1 + (flg == 1 ? (total + 1) / 2 : total);
		CHECK(flg <= 1 && loops > 0, "GIF mode or loop count");
		CHECK(pos + qw <= cap_n, "GIF payload beyond capture");
		if (pos + qw > cap_n) return;
		while (mark < nmarks && marks[mark] <= pos) mark++;
		CHECK(mark >= nmarks || pos + qw <= marks[mark], "GIF packet straddles producer buffers");
		if (!flg && ((p->d[0] >> 46) & 1)) active_prim = (u32)(p->d[0] >> 47) & 2047;
		if (!mutated && negative && flg == 1 && nr == 10)
		{
			u64 *data = (u64 *)(p + 1);
			if (negative == 1) data[1] ^= 1ull << 32;
			if (negative == 2) data[2] ^= 1ull << 32;
			if (negative == 3) active_prim ^= PRIM_ABE;
			if (negative == 4) desc = (desc & ~(15ull << 36)) | (5ull << 36);
			if (negative == 5) { marks[nmarks++] = pos + 2; mark = nmarks - 1; }
			mutated = 1;
			CHECK(mark >= nmarks || pos + qw <= marks[mark], "mutated producer boundary");
		}
		for (k = 0; k < total; k++)
		{
			u32 reg = (u32)(desc >> (4 * (k % nr))) & 15;
			u64 value;
			if (flg == 1) value = ((const u64 *)(p + 1))[k];
			else { if (reg == 2) q = p[k + 1].w[2]; value = packed_value(&p[k + 1], reg, q); }
			if (reg == 15) { CHECK(value == 0, "NOP must be initialized"); continue; }
			if (reg == 0) { CHECK(nv % 3 == 0, "PRIM reset inside triangle"); active_prim = (u32)value; continue; }
			CHECK(active_prim == prim, "wrong active PRIM");
			CHECK(ev < expected_n, "extra GS register write");
			if (ev < expected_n) CHECK(reg == expected[ev].reg && value == expected[ev].value, "list GS register event differs");
			ev++;
			if (reg == 4 || reg == 5) nv++;
		}
		pos += qw;
	}
	CHECK(pos == cap_n && ev == expected_n, "list event count");
	CHECK(nv == want_vertices && H.st.verts_out == want_vertices, "list vertex count and stats");
}

static void test_lists(void)
{
	static const int formats[] = {0, 1, 2, 4, 5, 6};
	static const u32 counts[] = {1, 2, 3, 7, 100, 1637, 1638, 9000};
	int f, c, edge;
	for (f = 0; f < 6; f++)
		for (c = 0; c < 8; c++)
			for (edge = 0; edge < 10; edge++)
			{
				u32 t, count = counts[c];
				pack_t pk = random_pack(formats[f]);
				u32 prim;
				reset(edge == 0 ? 1 : BUF_QW - (u32)edge);
				expected_n = 0;
				P.pass[0].tex = 1; P.pass[0].abe = (u8)(c & 1); P.pass[0].fog = (u8)(f & 1);
				prim = PRIM_TRI | pass_prim(&P.pass[0]);
				LS.nreg = pk.nreg; LS.fmt = pk.fmt;
				for (t = 0; t < count; t++)
				{
					qw_t *o = ls_reserve(pk.nreg == 3 ? 5 : 3 * (u32)pk.nreg);
					u64 *native = (u64 *)o;
					int k;
					for (k = 0; k < 3; k++)
					{
						cv_t v = random_vertex(pk.fmt); qw_t packed[3];
						put_vertex(packed, &v, &pk); expect_vertex(packed, pk.fmt);
						if (pk.nreg == 3) native = put_vertex_reglist(native, &v, &pk);
						else o = put_vertex(o, &v, &pk);
					}
					if (pk.nreg == 3) *native = 0;
					LS.n += 3;
				}
				ls_close();
				decode(prim, count * 3);
				list_triangles += count;
				old_qwords += count * (u32)pk.nreg * 3u + 1;
				new_qwords += cap_n;
			}
}

static void test_model_lists(void)
{
	static mdl_v_t mv[400], old_mv[400];
	static unsigned short indices[9000];
	static u16 order[3000];
	static const int formats[] = {0, 1, 6};
	int f, mode;
	for (f = 0; f < 3; f++)
		for (mode = 0; mode < 4; mode++)
		{
			int fmt = formats[f], k;
			u32 i, n = mode == 0 ? 1 : mode == 1 ? 3 : 3000, fog4 = (random_u32() & 255) << 4;
			qw_t col;
			u32 colour = random_u32();
			reset(mode == 3 ? BUF_QW - 7 : 1); expected_n = 0;
			LS.nreg = 3; LS.fmt = fmt;
			P.pass[0].tex = 1; P.pass[0].abe = 1; P.pass[0].fog = (u8)(fmt != 0);
			for (k = 0; k < 4; k++) col.w[k] = (colour >> (k * 8)) & 255;
			for (k = 0; k < 400; k++)
			{
				float lf = random_float(0.0f, 3.0f);
				old_mv[k] = random_model(fmt);
				if (fmt == 6) old_model_colour(&old_mv[k], colour, lf);
				mv[k] = old_mv[k];
				mdl_prepare_v(&mv[k], colour, lf, fmt);
			}
			for (i = 0; i < 9000; i++) indices[i] = (unsigned short)(random_u32() % 400);
			for (i = 0; i < 3000; i++) order[i] = (u16)((i * 997) % 3000);
			mdl_emit_fmt(mv, indices, order, n, fog4, fmt);
			ls_close();
			for (i = 0; i < n; i++)
				for (k = 0; k < 3; k++)
				{
					qw_t packed[3];
					old_mdl_put(packed, &old_mv[indices[order[i] * 3 + (u32)k]], &col, fog4, fmt);
					expect_vertex(packed, fmt);
				}
			decode(PRIM_TRI | pass_prim(&P.pass[0]), n * 3);
			list_triangles += n;
		}
}

/* Exercise the actual emit_tris function, including clipping that expands one
 * triangle into several output triangles and cache-index aliases 0/256/512. */
static void test_emit_tris(void)
{
	static FOutVector base[777];
	static float colour[777 * 4];
	static u32 indices[2000 * 3];
	static const int formats[] = {0, 2, 4, 6};
	int f, edge, clipped_seen = 0;
	H.guard_x = H.guard_y = 1024;
	memset(H.mvp, 0, sizeof H.mvp);
	H.mvp[0] = H.mvp[5] = H.mvp[10] = 1.0f;
	H.mvp[11] = 0.3f; H.mvp[15] = 1.1f;
	for (f = 0; f < 4; f++)
		for (edge = 0; edge < 12; edge++)
		{
			int k, fmt = formats[f];
			u32 i, count = 2000, want = 0;
			pack_t pk;
			reset(edge == 0 ? 1 : BUF_QW - (u32)edge);
			expected_n = 0;
			P.npass = 1; P.lmode = LM_NONE;
			P.vcol = fmt == 4; P.vlight = fmt == 6; P.textured = fmt != 2;
			P.wrapx = P.wrapy = 1; P.cull = 0;
			P.pass[0].tex = (u8)P.textured; P.pass[0].fog = 0;
			P.pass[0].abe = 1; P.pass[0].col = random_u32();
			memset(&pk, 0, sizeof pk);
			pk.fmt = fmt; pk.nreg = pack_regs(fmt); pk.col = P.pass[0].col;
			for (k = 0; k < 777; k++)
			{
				base[k].x = random_float(-2.0f, 2.0f); base[k].y = random_float(-2.0f, 2.0f);
				base[k].z = random_float(-1.3f, 1.3f);
				base[k].s = random_float(-10.0f, 10.0f); base[k].t = random_float(-10.0f, 10.0f);
				for (i = 0; i < 4; i++) colour[k * 4 + (int)i] = random_float(0.0f, fmt == 6 ? 1.0f : 255.0f);
			}
			for (i = 0; i < count * 3; i++) indices[i] = random_u32() % 777;
			indices[0] = 0; indices[1] = 256; indices[2] = 512;
			for (i = 0; i < count; i++)
			{
				cv_t a[16], b[16], *cl = a;
				int oc[3], n = 3;
				for (k = 0; k < 3; k++)
				{
					u32 vi = indices[i * 3 + (u32)k];
					const float *vc = fmt == 4 ? &colour[vi * 4] : fmt == 6 ? &colour[vi] : NULL;
					xform_vertex(&base[vi], vc, &a[k]);
					oc[k] = outcode(&a[k], 1.0f, 1.0f);
				}
				if (oc[0] & oc[1] & oc[2]) continue;
				if (oc[0] | oc[1] | oc[2])
					n = clip_poly(a, b, n, fmt == 6 ? 3 : fmt == 4 ? 6 : 2, oc[0] | oc[1] | oc[2], &cl);
				if (n > 3) clipped_seen = 1;
				if (n < 3) continue;
				pk.sb = pk.tb = 0.0f;
				if (P.textured)
				{
					float smin = cl[0].a[0], tmin = cl[0].a[1];
					for (k = 1; k < n; k++) { if (cl[k].a[0] < smin) smin = cl[k].a[0]; if (cl[k].a[1] < tmin) tmin = cl[k].a[1]; }
					pk.sb = fabsf(smin) > 4.0f ? floorf(smin) : 0.0f;
					pk.tb = fabsf(tmin) > 4.0f ? floorf(tmin) : 0.0f;
				}
				for (k = 1; k + 1 < n; k++)
				{
					qw_t packed[3];
					put_vertex(packed, &cl[0], &pk); expect_vertex(packed, fmt);
					put_vertex(packed, &cl[k], &pk); expect_vertex(packed, fmt);
					put_vertex(packed, &cl[k + 1], &pk); expect_vertex(packed, fmt);
					want += 3;
				}
			}
			emit_tris(base, indices, count * 3, fmt == 4 || fmt == 6 ? colour : NULL);
			decode(PRIM_TRI | pass_prim(&P.pass[0]), want);
			integrated_triangles += want / 3;
		}
	CHECK(clipped_seen, "emit_tris clipped expansion not exercised");
}

int main(int argc, char **argv)
{
	if (argc > 1) negative = atoi(argv[1]);
	P.ox = P.oy = 32768.0f; P.kx = 2400.0f; P.ky = -1800.0f;
	P.zo = 8000000.0f; P.zk = -7500000.0f; P.zbias = 0.5f;
	P.zmax = 16777215.0f; P.tsx = 0.75f; P.tsy = 0.625f; P.ra = 0.3f; P.rb = 91.0f;
	col_lut_init();
	test_vertices(); test_models(); test_lists(); test_model_lists(); test_emit_tris();
	if (negative) CHECK(mutated, "negative control was not exercised");
	printf("GS-LIST %s scalar=%llu model=%llu triangles=%llu emit_tris=%llu old_qw=%llu native_qw=%llu mutated=%d failures=%d\n",
		failures ? "FAIL" : "PASS", (unsigned long long)vertices, (unsigned long long)model_vertices,
		(unsigned long long)list_triangles, (unsigned long long)integrated_triangles,
		(unsigned long long)old_qwords, (unsigned long long)new_qwords, mutated, failures);
	return failures ? 1 : 0;
}
