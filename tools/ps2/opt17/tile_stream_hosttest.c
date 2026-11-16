#define main base_suite_main
#include "base_hosttest.c"
#undef main
unsigned int hwr_geo_off;

static unsigned long tile_checks;
static int tile_failures;
#define TC(cond, ...) do { tile_checks++; if (!(cond)) { if (tile_failures < 12) { printf("TILE FAIL "); printf(__VA_ARGS__); printf("\n"); } tile_failures++; } } while (0)

static void tile_test_init(void)
{
	if (H.rec) tex_free_all();
	host_init(1);
	memset(&cfg, 0, sizeof cfg);
	H.vw = H.fbw = 640; H.vh = H.fbh = 480;
	H.fb_addr[0] = 0; H.fb_addr[1] = 1228800u; H.z_addr = 2457600u;
	H.clut_base = 3686400u / 256u;
	H.pool_base = H.clut_base + 512u; H.pool_blocks = VRAM_BLOCKS - H.pool_base;
	H.free_n = 1; H.free_range[0].start = H.pool_base; H.free_range[0].len = H.pool_blocks;
	H.cur_tex = NOREC;
	H.pal_set = 1;
	for (int i = 0; i < 256; i++) H.pal[i] = (u32)i | ((u32)(255 - i) << 8) | ((u32)(i ^ 117) << 16);
	pal_build();
	tile_invalidate();
	ps2hwd_dbg_flags = HWDBG_NOPLAN;
}

static int ref_coord(int at, int size, int wrap)
{
	if (!wrap) return at < 0 ? 0 : at >= size ? size - 1 : at;
	while (at < 0) at += size;
	while (at >= size) at -= size;
	return at;
}

static u32 ref_pixel(const texrec_t *r, const u8 *data, int x, int y)
{
	const GLMipmap_t *m = r->owner;
	size_t p = (size_t)y * (m ? m->width : H.fbw) + x;
	if (r->screen) return H.fb32 ? ((const u32 *)(const void *)data)[p] : ((const u16 *)(const void *)data)[p];
	if (m->format == GL_TEXFMT_P_8 || m->format == GL_TEXFMT_ALPHA_8) return data[p];
	if (m->format == GL_TEXFMT_AP_88)
	{
		u16 v = ((const u16 *)(const void *)data)[p];
		return r->psm == PSM_T8 ? ((m->flags & TF_CHROMAKEYED) || (v >> 8) ? v & 255u : 255u) : H.pal[v & 255u] | (((u32)(v >> 8) + 1u) / 2u << 24);
	}
	if (m->format == GL_TEXFMT_ALPHA_INTENSITY_88) return (u32)data[p * 2] * 0x010101u | (((u32)data[p * 2 + 1] + 1u) / 2u << 24);
	{
		u32 v = ((const u32 *)(const void *)data)[p];
		if (r->psm != PSM_T8) return (v & 0xFFFFFFu) | (((v >> 24) + 1u) / 2u << 24);
		if (!(v >> 24)) return 255;
		for (int i = 0; i < 256; i++) if (r->tile_palette[i] == (v & 0xFFFFFFu)) return (u32)i;
		return 256;
	}
}

static void test_formats(void)
{
	static const int fmts[] = {GL_TEXFMT_P_8, GL_TEXFMT_ALPHA_8, GL_TEXFMT_AP_88, GL_TEXFMT_AP_88, GL_TEXFMT_AP_88, GL_TEXFMT_RGBA, GL_TEXFMT_RGBA, GL_TEXFMT_ALPHA_INTENSITY_88};
	for (int f = 0; f < 8; f++)
	{
		GLMipmap_t m;
		u32 history[256];
		texrec_t r;
		const int bpp = fmts[f] == GL_TEXFMT_RGBA ? 4 : fmts[f] == GL_TEXFMT_AP_88 || fmts[f] == GL_TEXFMT_ALPHA_INTENSITY_88 ? 2 : 1;
		u8 *data = malloc(129u * 131u * (u32)bpp);
		memset(&m, 0, sizeof m); memset(&r, 0, sizeof r);
		m.width = r.w = r.sw = 129; m.height = r.h = r.sh = 131; m.data = data; m.format = fmts[f];
		m.flags = f == 3 ? TF_CHROMAKEYED : 0;
		r.owner = &m; r.psm = f < 4 || f == 5 ? PSM_T8 : PSM_CT32;
		r.tile_palette = history;
		memcpy(history, H.pal, sizeof history);
		for (size_t p = 0; p < 129u * 131u; p++)
		{
			u32 index = (u32)(p * 17u + p / 129u) & 255u;
			if (bpp == 1) data[p] = (u8)index;
			else if (bpp == 2) ((u16 *)(void *)data)[p] = (u16)(index | ((p % 9u ? 255u : f == 4 ? 73u : 0u) << 8));
			else ((u32 *)(void *)data)[p] = f == 5 ? H.pal[index] | 0xFF000000u : (u32)(p * 758347u);
		}
		for (int wrap = 0; wrap < 4; wrap++) for (int origin = 0; origin < 3; origin++)
		{
			tile_fill_t c = {0};
			c.source=r; c.data=data; c.x=c.y=origin == 0 ? 0 : origin == 1 ? 62 : 124; c.wrapx=wrap&1; c.wrapy=wrap&2;
#ifdef TILE_COLUMNS_TEST
			tile_fill_columns(&c);
#endif
			u8 row[TILE_SIZE * 4];
			for (u32 y = 0; y < TILE_SIZE; y++)
			{
				tile_fill(&c, y, row);
				for (int x = 0; x < TILE_SIZE; x++)
				{
					int sx = ref_coord(c.x + x - 1, r.w, c.wrapx), sy = ref_coord(c.y + (int)y - 1, r.h, c.wrapy);
					u32 got = r.psm == PSM_T8 ? row[x] : ((const u32 *)(const void *)row)[x];
					TC(got == ref_pixel(&r, data, sx, sy), "halo format%d wrap%d origin%d at%d,%u", f, wrap, origin, x, y);
				}
			}
		}
		free(data);
	}
}

static void test_capacity_and_packets(void)
{
	GLMipmap_t m;
	texrec_t tile;
	int ri;
	u32 before;
	memset(&m, 0, sizeof m); m.width = 1024; m.height = 512; m.format = GL_TEXFMT_P_8; m.flags = TF_WRAPXY; m.regen_kind = 1;
	m.data = malloc((size_t)m.width * m.height); memset(m.data, 47, (size_t)m.width * m.height);
	before = H.used_blocks;
	ri = tex_upload(&m);
	TC(ri != NOREC && H.rec[ri].tiled == 1 && !H.rec[ri].nblk && H.used_blocks == before, "SKY1 full-size512KiB must stream in368KiB pool");
	if (ri == NOREC) { free(m.data); return; }
	TC(H.rec[ri].sw == 1024 && H.rec[ri].sh == 512 && !H.rec[ri].dx && !H.rec[ri].dy, "proxy changed source resolution");
	cap_reset();
	TC(tile_load(&H.rec[ri], 0, 0, 1, 1, &tile), "tile_load failed");
	TC(H.used_blocks - before == TILE_SIZE * TILE_SIZE * 4u / 256u, "scratch not bounded to16KiB");
	TC(tile.tbw == 2 && tile.tw == 6 && tile.th == 6 && tile.psm == PSM_T8, "indexed tile GS layout");
	memset(m.data, 91, (size_t)m.width * m.height);
	{
		pkt_t p; int images = 0;
		for (u32 at = 0; at < cap_n && pkt_at(at, &p); at += 1 + p.size)
			if (p.flg == 2)
			{
				const u8 *bytes = (const u8 *)&cap[at + 1]; images++;
				for (u32 b = 0; b < p.size * 16u; b++) TC(bytes[b] == 47, "queued DMA payload aliases EE source");
			}
		TC(images == 1, "tile image count");
	}
	cap_reset();
	TC(tile_load(&H.rec[ri], 0, 0, 1, 1, &tile) && cap_n == 0, "identical tile not reused");
	{
		texrec_t source = H.rec[ri];
		tile_invalidate(); cap_reset();
		TC(tile_load(&source, 0, 0, 1, 1, &tile) && cap_n != 0, "invalidated tile reused stale pixels");
	}
	before = H.used_blocks; tex_drop(ri, 0);
	TC(H.used_blocks == before && H.free_range[0].start >= H.pool_base, "metadata teardown freed block0/created zero free range");
	free(m.data);
}

static void test_perspective_geometry(void)
{
	GLMipmap_t m;
	cv_t tri[3];
	piece_t pc = {0, 0, -1, 0};
	int ri;
	memset(&m, 0, sizeof m); m.width = 1024; m.height = 512; m.format = GL_TEXFMT_RGBA; m.flags = TF_WRAPXY;
	m.data = malloc((size_t)m.width * m.height * 4u);
	for (u32 y = 0; y < m.height; y++) for (u32 x = 0; x < m.width; x++) ((u32 *)m.data)[(size_t)y * m.width + x] = 0xFF000000u | x | (y << 12);
	H.pal_set = 0;
	ri = tex_upload(&m); TC(ri != NOREC && H.rec[ri].tiled == 1, "RGBA geometry source not tiled");
	if (ri == NOREC) { free(m.data); return; }
	H.cur_tex = ri; TC(begin_draw(PF_NoDepthTest, NULL), "geometry plan failed");
	TC(!P.fast && !P.litfast && !P.vuok, "direct sampling bypasses tiled residency");
	memset(tri, 0, sizeof tri);
	for (int test = 0; test < 3; test++)
	{
		const float xy[3][2] = {{-.8f, -.7f}, {.75f, -.6f}, {-.15f,.85f}};
		const float uv[3][2] = {{-.12f,.06f},{.49f,-.09f},{.12f,.72f}};
		pt2 ref[3]; double iq[3], uq[3], vq[3], area;
		u32 tile_origin_x = 0, tile_origin_y = 0; int have_tile = 0, packets = 0;
		pkt_t p;
		for (int i = 0; i < 3; i++)
		{
			tri[i].w = 1.0f + (float)i * (.3f + .07f * test);
			tri[i].x = xy[i][0] * tri[i].w; tri[i].y = xy[i][1] * tri[i].w;
			tri[i].a[0] = uv[i][0] + .03f * test; tri[i].a[1] = uv[i][1] + .01f * test;
			ref[i].x = (P.ox + tri[i].x / tri[i].w * P.kx) / 16.0;
			ref[i].y = (P.oy + tri[i].y / tri[i].w * P.ky) / 16.0;
			iq[i] = 1.0 / tri[i].w; uq[i] = tri[i].a[0] * iq[i]; vq[i] = tri[i].a[1] * iq[i];
		}
		P.vcol = test % 3 != 0; P.vlight = test % 3 == 2;
		for (int i = 0; i < 3; i++)
		{
			tri[i].a[2] = P.vlight ? .15f + .3f * i : 15.0f + 95.0f * i;
			tri[i].a[3] = 235.0f - 87.0f * i; tri[i].a[4] = 32.0f + 63.0f * i; tri[i].a[5] = 255.0f - 81.0f * i;
		}
		area = (ref[1].x-ref[0].x)*(ref[2].y-ref[0].y)-(ref[2].x-ref[0].x)*(ref[1].y-ref[0].y);
		tile_invalidate(); cap_reset(); tile_emit_triangle(tri, &pc, ri);
		for (u32 at = 0; at < cap_n && pkt_at(at, &p); at += 1 + p.size)
		{
			if (p.flg == 2)
			{
				const u32 value = ((const u32 *)(const void *)&cap[at+1])[TILE_SIZE+1];
				tile_origin_x = value & 4095u; tile_origin_y = (value >> 12) & 4095u; have_tile = 1;
			}
			if (p.pre && p.flg == 0 && (p.prim & 7) == PRIM_TRIFAN)
			{
				pack_t pk; qw_t expected[9]; qw_t *next = expected;
				memset(&pk,0,sizeof pk); pk.fmt = P.vlight ? 6 : P.vcol ? 4 : 0; pk.col = P.pass[0].col; pk.fog = 255;
				TC(have_tile && p.nloop == 3 && p.nreg == 3, "stream changed original triangle shape"); packets++;
				for (int e = 0; e < 3; e++) next = put_vertex(next,&tri[e],&pk);
				for (u32 i = 0; i < p.nloop; i++)
				{
					const qw_t *v = p.reglist ? &p.decoded[i*p.nreg] : &cap[p.at+1+i*p.nreg];
					for (int lane = 0; lane < 2; lane++) TC(memcmp(&v[lane],&expected[i*3+lane],sizeof(qw_t)) == 0, "original GS STQ/colour changed test%d v%u lane%d", test,i,lane);
					const qw_t *zv = &v[2], *ze = &expected[i*3+2];
					TC(zv->w[0] == ze->w[0] && zv->w[1] == ze->w[1], "stencil changed original XY");
					const u32 expected_z = P.vlight ? ze->w[2] >> 4 : ze->w[2];
					TC((zv->w[2] & 0xFFFFFFu) == expected_z && (zv->w[2] >> 24) == 1, "stencil changed lower24/NoDepth marker");
				}
			}
		}
		TC(packets > 5 && P.r == &H.rec[ri], "geometry not streamed/restored");
	}
	tex_drop(ri, 0); free(m.data);
}

static void test_owned_source_and_views(void)
{
	for (int order = 0; order < 3; order++)
	{
		GLMipmap_t m, view;
		texrec_t tile;
		memset(&m, 0, sizeof m); memset(&view, 0, sizeof view);
		m.width = 1024; m.height = 512; m.format = GL_TEXFMT_RGBA;
		m.data = malloc(1024u*512u*4u);
		for (u32 i = 0; i < 1024u*512u; i++) ((u32 *)m.data)[i] = H.pal[i&255u] | 0xFF000000u;
		tile_spy_source = m.data; tile_spy_locks = tile_spy_unlocks = 0; tile_spy_poison = 0;
		int ri = tile_source_record(&m, PSM_T8, 1, 0, 0, 0, 0, 1024, 512);
		TC(ri != NOREC && H.rec[ri].tile_palette && H.rec[ri].tile_source == m.data && H.rec[ri].tile_source_bytes == 2097152u, "external source lease/history absent");
		TC(tile_spy_locks == 1 && tile_spy_unlocks == 0, "external source unprotected before record admission");
		if (ri == NOREC) { free(m.data); continue; }
		TC(tile_load(&H.rec[ri],0,0,0,0,&tile) && tile_spy_unlocks == 0, "tile upload released persistent source lease");
		view.width = m.width; view.height = m.height; view.format = m.format;
		int vi = tex_view(&view,ri); TC(vi != NOREC,"cannot make history sharing view");
		if (order == 0) { tex_drop(vi,0); TC(tile_spy_unlocks == 0,"view released owning source lease"); tex_drop(ri,0); }
		else if (order == 1) { tex_drop(ri,0); TC(!H.rec[vi].used && !view.downloaded,"owner deletion left history view dangling"); }
		else { void *old = m.data; m.data = NULL; free(old); tex_drop(ri,0); TC(tile_spy_unlocks == 0,"explicitly freed source passed to zone unlock"); }
		if (order != 2) { TC(tile_spy_unlocks == 1,"owning source lease release count%d",tile_spy_unlocks); free(m.data); }
		tile_spy_source = NULL;
	}
}

static void tile_retire_pending(void)
{
	tile_spy_poll = NULL;
	rel_release_buf(H.cur);
}

static void test_cached_source_lifetime(void)
{
	for (int mode = 0; mode < 3; mode++)
	{
		GLMipmap_t m;
		texrec_t source, tile;
		dcent_t *e = &dc[0];
		const size_t bytes = 1024u * 512u;
		memset(&m, 0, sizeof m); memset(&source, 0, sizeof source);
		m.width = 1024; m.height = 512; m.format = GL_TEXFMT_P_8; m.regen_kind = 1; m.regen_id = 31;
		e->p = malloc(bytes); memset(e->p, 73, bytes);
		e->key = dc_key(&m); e->w = 1024; e->h = 512; e->bytes = (u32)bytes; e->lvl = 0; e->busy = 1;
		dc_bytes += (u32)bytes;
		source.owner = &m; source.w = source.sw = 1024; source.h = source.sh = 512; source.psm = PSM_T8; source.tiled = 1;
		rel_buf[H.cur].n = 1; rel_buf[H.cur].dc[0] = 0; rel_total = 1; rel_bytes = (u32)bytes;
		tile_spy_source = e->p; tile_spy_locks = tile_spy_unlocks = 0; tile_spy_poison = 1; tile_spy_bytes = bytes;
		tile_invalidate(); cap_reset();
		if (mode == 1) tile_spy_poll = tile_retire_pending;
		if (mode == 2) { tex_free_all(); H.pool_blocks = 0; H.free_n = 0; }
		int ok = tile_load(&source, 0, 0, 1, 1, &tile);
		TC(ok == (mode != 2), "cached source failure result mode%d", mode);
		if (mode != 1)
		{
			TC(e->busy == 1 && tile_spy_unlocks == 0, "pending DMA source unlocked on tile exit mode%d busy%u unlocks%d", mode,e->busy,tile_spy_unlocks);
			rel_release_buf(H.cur);
		}
		TC(e->busy == 0 && tile_spy_unlocks == 1, "temporary source reference leaked/early unlocked mode%d busy%u unlocks%d",mode,e->busy,tile_spy_unlocks);
		if (ok)
		{
			pkt_t p; int images = 0;
			for (u32 at = 0; at < cap_n && pkt_at(at, &p); at += 1 + p.size)
				if (p.flg == 2)
				{
					const u8 *data = (const u8 *)&cap[at+1]; images++;
					for (u32 b = 0; b < p.size*16u; b++) TC(data[b] == 73, "pending DMA retirement purged synchronous tile source mode%d",mode);
				}
			TC(images == 1, "pending source tile payload absent");
		}
		tile_spy_poll = NULL; tile_spy_source = NULL; tile_spy_poison = 0;
		free(e->p); memset(e, 0, sizeof *e); dc_bytes -= (u32)bytes;
	}
}

#ifdef TILE_CORE_FAST_TEST
static void test_one_core(void)
{
	GLMipmap_t m = {0};
	cv_t tri[3] = {0};
	piece_t pc = {0, 0, -1, 0};
	int ox, oy;

	tile_test_init(); H.pal_set = 0;
	m.width = 1024; m.height = 512; m.format = GL_TEXFMT_RGBA; m.flags = TF_WRAPXY;
	m.data = malloc(1024u * 512u * 4u); memset(m.data, 0x37, 1024u * 512u * 4u);
	const int ri = tex_upload(&m);
	TC(ri != NOREC && H.rec[ri].tiled == 1, "one-core source must be virtual");
	if (ri == NOREC) { free(m.data); return; }
	H.cur_tex = ri; TC(begin_draw(PF_NoDepthTest, NULL), "one-core draw setup");
	for (int i = 0; i < 3; i++)
	{
		const float xy[3][2] = {{-.8f,-.7f},{.75f,-.6f},{-.15f,.85f}};
		const float uv[3][2] = {{80,70},{95,90},{87,76}};
		tri[i].w = 1.0f + .5f * i;
		tri[i].x = xy[i][0] * tri[i].w; tri[i].y = xy[i][1] * tri[i].w;
		tri[i].a[0] = uv[i][0] / m.width; tri[i].a[1] = uv[i][1] / m.height;
	}
	tile_run_peak_bytes = 0;
	const u32 fast_before = tile_core_fast_count(), masks_before = tile_stencil_mask_sprites;
	TC(tile_one_core(tri, &pc, &H.rec[ri], &ox, &oy) && ox >= 95-46 && ox <= 80+15 && oy >= 90-46 && oy <= 70+15, "positive-Q guarded raw window rejected");
	cap_reset(); tile_emit_triangle(tri, &pc, ri);
	TC(tile_core_fast_count() == fast_before + 1 && tile_workspace_peak() == 0, "one-core must bypass classifier/VLA");
	TC(!tile_stencil_active && tile_stencil_mask_sprites == masks_before && P.r == &H.rec[ri], "one-core changed stencil/restoration state");
	cv_t saved[3]; memcpy(saved, tri, sizeof saved);
	tri[0].a[0]=tri[0].a[1]=0.0f; tri[1].a[0]=32.0f/m.width;tri[1].a[1]=32.0f/m.height;tri[2].a[0]=16.0f/m.width;tri[2].a[1]=16.0f/m.height;
	TC(tile_one_core(tri,&pc,&H.rec[ri],&ox,&oy), "inclusive 0..32 core rejected");
	tri[0].a[0]=80.0f/m.width;tri[0].a[1]=100.0f/m.height;tri[1].a[0]=141.0f/m.width;tri[1].a[1]=161.0f/m.height;tri[2].a[0]=110.0f/m.width;tri[2].a[1]=130.0f/m.height;
	TC(tile_one_core(tri,&pc,&H.rec[ri],&ox,&oy) && ox==95 && oy==115, "guarded61texel window rejected");
	tri[1].a[0]=143.0f/m.width;
	TC(!tile_one_core(tri,&pc,&H.rec[ri],&ox,&oy), "window missing required original neighbors accepted");
	memcpy(tri,saved,sizeof saved);
	tri[1].a[0] += 1.0f; // Same wrapped key; different unwrapped cell.
	TC(!tile_one_core(tri, &pc, &H.rec[ri], &ox, &oy), "equal wrapped keys accepted across full repeat");
	tri[1].a[0] -= 1.0f; tri[1].w = -1.0f;
	TC(!tile_one_core(tri, &pc, &H.rec[ri], &ox, &oy), "non-positive Q entered one-core path");
	tri[1].w = 1.5f; tri[1].a[0] = INFINITY;
	TC(!tile_one_core(tri, &pc, &H.rec[ri], &ox, &oy), "non-finite UV entered one-core path");
	tex_drop(ri, 0); free(m.data);
}
#endif

#ifdef TILE_ALL_SOURCES_TEST
static void test_noncanonical_ap88(void)
{
	for(int packed=0;packed<2;packed++)
	{
		GLMipmap_t m={0};texrec_t tile;
		tile_test_init();m.width=1024;m.height=512;m.format=GL_TEXFMT_AP_88;
		const size_t pixels=1024u*512u, bytes=packed?pixels+(pixels+7)/8:pixels*2;
		m.data=malloc(bytes);
		if(packed)
		{
			m.ps2_cpu=HWR_PS2_CPU_PATCH;m.ps2_uw=1024;m.ps2_uh=512;
			memset(m.data,47,pixels);memset((u8*)m.data+pixels,255,(pixels+7)/8);
		}
		else for(size_t i=0;i<pixels;i++)((u16*)m.data)[i]=0xff2f;
		tile_spy_source=m.data;tile_spy_locks=tile_spy_unlocks=0;tile_spy_poison=0;
		const int ri=tex_upload(&m);
		TC(ri!=NOREC&&H.rec[ri].tiled==1&&H.rec[ri].tile_source==m.data&&H.rec[ri].tile_source_bytes==bytes,"noncanonical AP88 source lease/actual byte count missing packed%d",packed);
		if(ri!=NOREC)
		{
			TC(tile_spy_locks==1&&!tile_spy_unlocks&&(m.ps2_cpu&HWR_PS2_CPU_TILED_SOURCE),"AP88 admission released source packed%d",packed);
			TC(tile_load_region(&H.rec[ri],0,0,0,0,&tile)&&!tile_spy_unlocks,"AP88 tile copy released persistent lease packed%d",packed);
			if(packed){m.ps2_uw=129;m.ps2_uh=131;const size_t n=129u*131u;TC(tile_cpu_source_bytes(&m)==n+(n+7)/8,"packed AP88 counted logical padding");}
			tex_drop(ri,0);TC(tile_spy_unlocks==1&&!(m.ps2_cpu&HWR_PS2_CPU_TILED_SOURCE),"AP88 owner deletion leaked source lease packed%d",packed);
		}
		free(m.data);tile_spy_source=NULL;
	}
}
#endif

int main(void)
{
	cap = calloc(CAP_MAX, sizeof *cap);
	tile_test_init(); test_formats(); test_capacity_and_packets(); test_perspective_geometry(); H.pal_set = 1; test_owned_source_and_views(); test_cached_source_lifetime();
#ifdef TILE_CORE_FAST_TEST
	test_one_core();
#endif
#ifdef TILE_ALL_SOURCES_TEST
	test_noncanonical_ap88();
#endif
	if (H.rec) tex_free_all(); free(H.rec); free(cap);
	printf("TILE COMPLETE checks=%lu failed=%d\n", tile_checks, tile_failures);
	return tile_failures != 0;
}
