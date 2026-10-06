/* Native REGLIST register values must equal the unchanged PACKED encoder bit for bit. */
#define main legacy_hw_hosttest_main
#include "hw_hosttest.c"
#undef main

int main(int argc, char **argv)
{
	static const int formats[] = {0, 1, 2, 3, 5};
	unsigned packets = 0, vertices = 0, old_qw = 0, new_qw = 0;
	int f, it, mutated = 0;
	cap = (qw_t *)calloc(CAP_MAX, sizeof(qw_t));
	if (argc > 1) neg = atoi(argv[1]);
	host_init(1);
	group_begin("native_reglist");
	P.ox = P.oy = 32768.0f; P.kx = 2400.0f; P.ky = -1800.0f;
	P.zo = 8000000.0f; P.zk = -7500000.0f; P.zbias = 0.5f;
	P.zmax = PS2HWD_ZMAX; P.tsx = 0.75f; P.tsy = 0.625f;
	for (f = 0; f < 5; f++)
		for (it = 0; it < 3000; it++)
		{
			cv_t v[15]; qw_t expected[45], *e = expected;
			pack_t pk; const u64 *raw; u64 desc;
			u32 prim = (u32)(PRIM_TRIFAN | ((it & 31) << 3));
			int i, j, n, nr;
			memset(&pk, 0, sizeof pk);
			pk.fmt = formats[f]; pk.nreg = pack_regs(pk.fmt);
			n = 3 + (int)(rnd() % (unsigned)(15 / pk.nreg - 2));
			nr = 1 + n * pk.nreg;
			pk.col = rnd(); pk.fog = rnd() & 255;
			pk.sb = (float)rndf(-4, 4); pk.tb = (float)rndf(-4, 4);
			for (i = 0; i < n; i++)
			{
				v[i].w = pk.fmt == 5 ? 1.0f : (float)rndf(0.01, 32768.0);
				v[i].x = (float)rndf(-1, 1) * v[i].w;
				v[i].y = (float)rndf(-1, 1) * v[i].w;
				v[i].z = (float)rndf(-1.1, 1.1) * v[i].w;
				v[i].a[0] = (float)rndf(-100, 100); v[i].a[1] = (float)rndf(-100, 100);
				e = put_vertex(e, &v[i], &pk);
			}
			cap_reset(); H.gsr.valid = RV_RGBAQ;
			/* Also reserve at the end of a producer buffer: a packet must flush as a whole. */
			if ((it & 1) != 0) H.wr = BUF_QW - 1;
			emit_reglist_fan(v, n, &pk, prim);
			raw = (const u64 *)&cap[1]; desc = cap[0].d[1];
			if (!mutated && neg == 1 && (nr & 1)) { ((u64 *)raw)[nr] = 1; mutated = 1; }
			if (!mutated && neg == 2 && pk.fmt == 0) { ((u64 *)raw)[2] ^= 1ull << 32; mutated = 1; }
			if (!mutated && neg == 3) { ((u64 *)raw)[pk.nreg] ^= 1; mutated = 1; }
			EXPECT((cap[0].d[0] & 0x7FFF) == 1 && ((cap[0].d[0] >> 58) & 3) == 1, "wrong REGLIST mode/count");
			EXPECT(((cap[0].d[0] >> 46) & 1) == 0 && ((cap[0].d[0] >> 60) & 15) == (u32)(nr & 15), "PRE/NREG (16 encodes as 0)");
			EXPECT((desc & 15) == 0 && raw[0] == prim, "explicit PRIM missing");
			EXPECT(cap_n == (u32)(1 + (nr + 1) / 2) && H.wr == cap_n, "DMA extent/ring boundary");
			if (nr & 1) EXPECT(raw[nr] == 0, "padding must be initialized and ignored");
			for (i = 0; i < n; i++)
				for (j = 0; j < pk.nreg; j++)
				{
					int k = i * pk.nreg + j; u64 want; u32 reg = (u32)((desc >> ((k + 1) * 4)) & 15);
					const qw_t *z = &expected[k];
					EXPECT(reg == ((pack_regdesc(pk.fmt) >> (j * 4)) & 15), "descriptor mismatch");
					if (reg == 2) want = z->d[0];
					else if (reg == 1)
					{
						u32 qb = expected[k - 1].w[2];
						want = (u64)pk.col | ((u64)qb << 32);
					}
					else if (reg == 4)
						want = (u64)(z->w[0] & 65535) | ((u64)(z->w[1] & 65535) << 16) | ((u64)(z->w[2] >> 4) << 32) | ((u64)(z->w[3] >> 4) << 56);
					else want = (u64)(z->w[0] & 65535) | ((u64)(z->w[1] & 65535) << 16) | ((u64)z->w[2] << 32);
					EXPECT(raw[k + 1] == want, "format %d vertex %d register %u not bit-identical",pk.fmt,i,reg);
				}
			EXPECT((H.gsr.valid & RV_RGBAQ) == ((pk.fmt == 0 || pk.fmt == 1) ? 0 : RV_RGBAQ), "RGBAQ cache invalidation");
			packets++; vertices += (unsigned)n; old_qw += (unsigned)(1 + n * pk.nreg); new_qw += cap_n;
		}
	group_end("PACKED vs native ST/RGBAQ/XYZ2/XYZF2, explicit PRIM, odd padding, NREG=16, ring end and colour/Q cache");
	printf("O4GS packets=%u vertices=%u packed_qw=%u reglist_qw=%u mutated=%d negative=%d failures=%d\n",packets,vertices,old_qw,new_qw,mutated,neg,total_fail);
	return total_fail ? 1 : 0;
}
