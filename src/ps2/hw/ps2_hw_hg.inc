// PS2 GS hardware renderer driver: geometry-path counters (OPT10 HG, PS2-HW-40..59). Included by ps2_hwd.c right after ps2_hw_priv.inc.
// Printed as "HWPROF3" lines next to HWPROF (ps2/i_video.c, -ps2prof): per frame averages over the window of 105 frames.

#ifndef PS2_HW_HG_INC
#define PS2_HW_HG_INC
#include "ps2_hw_prof.h"

static struct
{
	u32 batches, fans; // PS2HWD_DrawFans calls, polygons in them
	u32 single; // hw_DrawPolygon calls (not batched: sprites, HUD, sky wall...)
	u32 p_vu, p_fast, p_lit, p_cut, p_water; // polygons by the way emit_poly drew them
	u32 begin_n, begin_cyc; // begin_draw calls and cycles
	u32 vc_n, vc_pieces, vc_cyc, vc_ee; // PS2-HW-236: polygons cut for the program (vu_cut_blk), pieces written, cycles, polygons left to the EE
	u32 bb_n, bb_cyc, bb_same, bb_diff[8]; // begin_draw of a batch (DrawBlocks): calls, cycles, plans the key of the last draw equalled (how = 2), first difference of the key (flags / colour / tint / fade / light table, ..)
	u32 tri_n; // emit_tris calls
	u32 vu_chunks;
	u32 sing_by[8]; // hw_DrawPolygon calls by (PF_NoTexture 1 | PF_NoDepthTest 2 | PF_Occlude 4)
	u32 sing_cyc[8]; // their cycles
	u32 c_fast, c_lit, c_cut, c_water; // cycles of emit_poly by branch
	u32 plan_hit, plan_miss; // plan cache (PS2-HW-46)
	u32 p_cutold, c_cutold; // polygons / cycles of the old cut_and_emit (repeat cuts, wide polygons)
	u32 pc_key, pc_copy, pc_touch, pc_misc, pc_miss; // cycles: key + lookup, P/LF copy, texture touch, rest of a hit, a miss
	u32 sk_n[5], sk_c[5]; // emit_poly in the skybox view by branch (vu, fast, lit, cut, water): polygons, cycles
	u32 wf_polys, wf_pieces, wf_bands; // band_sweep: polygons, pieces emitted, ripple planes inside the polygons
	u32 dome_n, dome_begin, dome_xf, dome_emit, dome_tris; // sky_dome_fast: calls, cycles of begin_draw / vu0 transform of the mesh / strips and clipped triangles, clipped triangles
	u32 cw_n[7], cw_c[7]; // the polygons of cut_and_emit by reason (HWPROF20): polygons, cycles
	u32 sk_single, sk_begin, sk_batches, sk_fans; // hw_DrawPolygon / begin_draw / PS2HWD_DrawFans calls in the skybox view
	u32 drv_cyc; // OPT11 (VU): EE cycles inside the hwdriver_s entry points (outermost call, waits for the GS / the DMA / the flip taken out): the cost of the driver
	u32 s_imm, s_begin, s_emit, s_set_n, s_set_cyc, c_fans; // hw_DrawPolygon by part (imm_prepare, begin_draw, emit_fan), hw_SetTexture calls / cycles, the polygons of PS2HWD_DrawFans (all)
	u32 vr_calls, vr_slow, vr_rows, vr_blk_slow, vr_same; // prow_ensure: calls, calls that walked the rows, rows walked, rows that needed clut_get
	u32 vs_n, vs_c, vs_rows, vs_pass, vs_consts, vs_send, vs_light; // vu_plan_setup: calls, cycles, and by part (rows, pass_setup, constants build, constants send, light)
	u32 md_draws, md_meshes, md_tris, md_verts, md_cyc, md_slow, md_lod, md_over, md_c1, md_c2, md_c3, md_ctx; // OPT11 MODEL (HWPROF40): draw_model calls, meshes, triangles, vertices interpolated, cycles inside draw_model, emit_tris calls of the corner path
	u32 s_vu, s_same, s_retarget, vs_reuse, b_retarget, s_rec; // single polygons drawn through the VU1 program, of them: with the plan (key) of the previous single draw
} G;

// the outermost driver call: cycles minus the waits
static u32 drv_depth, drv_t0, drv_w0;
static inline void drv_in(void)
{
	if (drv_depth++ == 0)
	{
		drv_t0 = cyc();
		drv_w0 = H.st.cyc_wait + H.st.cyc_flipwait;
	}
}

static inline void drv_out(void)
{
	if (--drv_depth == 0)
	{
		const u32 w = H.st.cyc_wait + H.st.cyc_flipwait - drv_w0, d = cyc() - drv_t0;

		if (d > w && w < 0x80000000u)
			G.drv_cyc += d - w;
	}
}

#define HG_SKY(br, dt) do { if (ps2hwp_skyview) { G.sk_n[br]++; G.sk_c[br] += (dt); } } while (0)

#ifdef __mips__ // (the x86 host tests have no log)
extern void PS2Models_Stats(unsigned int *out); // OPT11 MODEL (ps2_models.c)
extern void HWR_PBProf(unsigned int frames); // OPT11 round 2 VU2: HWPROF50, the collection of the batch (hardware/hw_pbatch.inc)
extern void HWR_GCacheProf(unsigned int frames); // OPT11 GEOM: HWPROF32, the geometry cache (hardware/hw_gcache.inc)
void PS2HWD_ProfExtra(unsigned int frames)
{
	if (!frames)
		frames = 1;
	I_OutputMsg("HWPROF3 batches=%u fans=%u single=%u vu=%u fast=%u lit=%u cut=%u water=%u begin=%u/%u tris=%u vuchunks=%u planhit=%u planmiss=%u\n", G.batches / frames, G.fans / frames, G.single / frames,
		G.p_vu / frames, G.p_fast / frames, G.p_lit / frames, G.p_cut / frames, G.p_water / frames, G.begin_n / frames, G.begin_cyc / frames, G.tri_n / frames, G.vu_chunks / frames, G.plan_hit / frames, G.plan_miss / frames);
	I_OutputMsg("HWPROF51 batches that needed a plan: %u of %u (%u cycles) | key differs in: flags=%u colour=%u tint=%u fade=%u ltable=%u texkind=%u rest(light, texture, view)=%u plan-invalid=%u\n", G.bb_n / frames, G.batches / frames, G.bb_cyc / frames,
		G.bb_diff[0] / frames, G.bb_diff[1] / frames, G.bb_diff[2] / frames, G.bb_diff[3] / frames, G.bb_diff[4] / frames, G.bb_diff[5] / frames, G.bb_diff[6] / frames, G.bb_diff[7] / frames);
	I_OutputMsg("HWPROF52 cut for the VU1 program (PS2-HW-236): polygons=%u pieces=%u cycles=%u | left to the EE: %u\n", G.vc_n / frames, G.vc_pieces / frames, G.vc_cyc / frames, G.vc_ee / frames);
	I_OutputMsg("HWPROF9 cut_and_emit (repeats etc.) polys=%u cyc=%u\n", G.p_cutold / frames, G.c_cutold / frames);
	I_OutputMsg("HWPROF23 driver total=%u (+ plan %u: the texture planner HWR_RenderBatches calls) | singles: imm=%u begin=%u emit=%u vu=%u same=%u retarget=%u | settex n=%u cyc=%u | DrawFans polys=%u batch retargets=%u state records=%u\n", G.drv_cyc / frames, (unsigned)(ps2hwp_cyc[HWP_B_PLAN] / frames), G.s_imm / frames, G.s_begin / frames, G.s_emit / frames,
		G.s_vu / frames, G.s_same / frames, G.s_retarget / frames, G.s_set_n / frames, G.s_set_cyc / frames, G.c_fans / frames, G.b_retarget / frames, G.s_rec / frames);
	I_OutputMsg("HWPROF24 vu_plan_setup n=%u cyc=%u: light=%u rows=%u pass=%u build=%u send=%u | reuse=%u chunks=%u | prow_ensure calls=%u same=%u slow=%u rows=%u clut_get=%u\n", G.vs_n / frames, G.vs_c / frames, G.vs_light / frames, G.vs_rows / frames, G.vs_pass / frames, G.vs_consts / frames,
		G.vs_send / frames, G.vs_reuse / frames, G.vu_chunks / frames, G.vr_calls / frames, G.vr_same / frames, G.vr_slow / frames, G.vr_rows / frames, G.vr_blk_slow / frames);
	I_OutputMsg("HWPROF8 plan cache cycles: key=%u copy=%u touch=%u misc=%u miss=%u\n", G.pc_key / frames, G.pc_copy / frames, G.pc_touch / frames, G.pc_misc / frames, G.pc_miss / frames);
	I_OutputMsg("HWPROF4 segs=%u subsecs=%u planes=%u sprites=%u proc=%u procbatch=%u\n", ps2hwp_cnt[HWC_SEGS] / frames, ps2hwp_cnt[HWC_SUBSECS] / frames, ps2hwp_cnt[HWC_PLANES] / frames,
		ps2hwp_cnt[HWC_SPRITES] / frames, ps2hwp_cnt[HWC_PROC] / frames, ps2hwp_cnt[HWC_PROC_BATCH] / frames);
	I_OutputMsg("HWPROF7 single_cyc t0d0o0=%u nt=%u nd=%u nt+nd=%u o=%u o+nt=%u o+nd=%u o+nt+nd=%u | emit cyc fast=%u lit=%u cut=%u water=%u\n", G.sing_cyc[0] / frames, G.sing_cyc[1] / frames, G.sing_cyc[2] / frames, G.sing_cyc[3] / frames,
		G.sing_cyc[4] / frames, G.sing_cyc[5] / frames, G.sing_cyc[6] / frames, G.sing_cyc[7] / frames, G.c_fast / frames, G.c_lit / frames, G.c_cut / frames, G.c_water / frames);
	I_OutputMsg("HWPROF5 single_by(tex/depth/occl) t0d0o0=%u nt=%u nd=%u nt+nd=%u o=%u o+nt=%u o+nd=%u o+nt+nd=%u\n", G.sing_by[0] / frames, G.sing_by[1] / frames, G.sing_by[2] / frames, G.sing_by[3] / frames,
		G.sing_by[4] / frames, G.sing_by[5] / frames, G.sing_by[6] / frames, G.sing_by[7] / frames);
	I_OutputMsg("HWPROF6 pal=%u frame=%u clr1=%u net=%u clr2=%u\n", (unsigned)(ps2hwp_cyc[HWP_S_PAL] / frames), (unsigned)(ps2hwp_cyc[HWP_S_FRAME] / frames), (unsigned)(ps2hwp_cyc[HWP_S_CLR1] / frames),
		(unsigned)(ps2hwp_cyc[HWP_S_NET] / frames), (unsigned)(ps2hwp_cyc[HWP_S_CLR2] / frames));
	I_OutputMsg("HWPROF13 skybox view emit_poly n/cyc: vu=%u fast=%u/%u lit=%u/%u cut=%u/%u water=%u/%u | single=%u begin=%u batches=%u fans=%u\n", G.sk_n[0] / frames, G.sk_n[1] / frames, G.sk_c[1] / frames,
		G.sk_n[2] / frames, G.sk_c[2] / frames, G.sk_n[3] / frames, G.sk_c[3] / frames, G.sk_n[4] / frames, G.sk_c[4] / frames, G.sk_single / frames, G.sk_begin / frames, G.sk_batches / frames, G.sk_fans / frames);
	I_OutputMsg("HWPROF19 plane cycles: hit=%u miss=%u bypass=%u\n", (unsigned)(ps2hwp_cyc[HWP_PL_HIT] / frames), (unsigned)(ps2hwp_cyc[HWP_PL_MISS] / frames), (unsigned)(ps2hwp_cyc[HWP_PL_BYP] / frames));
	I_OutputMsg("HWPROF21 RenderBatches parts (all views): init=%u sort=%u plan=%u draw=%u end=%u | skybox view: init=%u sort=%u plan=%u draw=%u end=%u\n", (unsigned)(ps2hwp_cyc[HWP_B_INIT] / frames),
		(unsigned)(ps2hwp_cyc[HWP_BATCHSORT] / frames), (unsigned)(ps2hwp_cyc[HWP_B_PLAN] / frames), (unsigned)(ps2hwp_cyc[HWP_BATCHDRAW] / frames), (unsigned)(ps2hwp_cyc[HWP_B_END] / frames),
		(unsigned)(ps2hwp_cyc[HWP_KB_INIT] / frames), (unsigned)(ps2hwp_cyc[HWP_KB_SORT] / frames), (unsigned)(ps2hwp_cyc[HWP_KB_PLAN] / frames), (unsigned)(ps2hwp_cyc[HWP_KB_DRAW] / frames),
		(unsigned)(ps2hwp_cyc[HWP_KB_END] / frames));
	I_OutputMsg("HWPROF22 RenderBatches loop: DrawBatch=%u SetTexture=%u | skybox view: DrawBatch=%u SetTexture=%u\n", (unsigned)(ps2hwp_cyc[HWP_B_DB] / frames), (unsigned)(ps2hwp_cyc[HWP_B_TEX] / frames),
		(unsigned)(ps2hwp_cyc[HWP_KB_DB] / frames), (unsigned)(ps2hwp_cyc[HWP_KB_TEX] / frames));
	I_OutputMsg("HWPROF20 cut_and_emit by reason n/cyc: pow2=%u/%u plan=%u/%u big=%u/%u vcol=%u/%u clipped=%u/%u classes=%u/%u other=%u/%u\n", G.cw_n[0] / frames, G.cw_c[0] / frames, G.cw_n[1] / frames, G.cw_c[1] / frames,
		G.cw_n[2] / frames, G.cw_c[2] / frames, G.cw_n[3] / frames, G.cw_c[3] / frames, G.cw_n[4] / frames, G.cw_c[4] / frames, G.cw_n[5] / frames, G.cw_c[5] / frames, G.cw_n[6] / frames, G.cw_c[6] / frames);
	I_OutputMsg("HWPROF18 plane cache key misses: height=%u light=%u flags=%u texture=%u offsets=%u epoch=%u poly=%u\n", ps2hwp_cnt[HWC_PKM_H] / frames, ps2hwp_cnt[HWC_PKM_L] / frames, ps2hwp_cnt[HWC_PKM_F] / frames,
		ps2hwp_cnt[HWC_PKM_T] / frames, ps2hwp_cnt[HWC_PKM_O] / frames, ps2hwp_cnt[HWC_PKM_E] / frames, ps2hwp_cnt[HWC_PKM_P] / frames);
	I_OutputMsg("HWPROF17 band_sweep: polys=%u pieces=%u ripple_planes=%u\n", G.wf_polys / frames, G.wf_pieces / frames, G.wf_bands / frames);
	I_OutputMsg("HWPROF40 models: draws=%u meshes=%u tris=%u verts=%u slow=%u lod=%u over=%u cyc=%u c1=%u c2=%u c3=%u ctx=%u\n", G.md_draws / frames, G.md_meshes / frames, G.md_tris / frames, G.md_verts / frames, G.md_slow / frames, G.md_lod / frames, G.md_over / frames, G.md_cyc / frames, G.md_c1 / frames, G.md_c2 / frames, G.md_c3 / frames, G.md_ctx / frames);
	{
		unsigned int ms[10];

		PS2Models_Stats(ms);
		I_OutputMsg("HWPROF41 model memory: alive=%u (%u KiB) loads=%u frees=%u reclaims=%u refused(memory)=%u damaged=%u load cycles total=%u max=%u injected=%u\n", ms[0], ms[1], ms[2], ms[3], ms[4], ms[5], ms[6], ms[7], ms[8], ms[9]);
	}
	HWR_GCacheProf(frames);
	HWR_PBProf(frames);
	I_OutputMsg("HWPROF37 HWR_RenderPlane parts: setup=%u buffer+flat=%u vertices=%u lightlevel=%u lighting=%u flags+shader=%u polygon=%u (the last, the sum of the two before it, is what HWR_ProcessPolygon takes)\n", (unsigned)(ps2hwp_cyc[HWP_PL_A] / frames),
		(unsigned)(ps2hwp_cyc[HWP_PL_B] / frames), (unsigned)(ps2hwp_cyc[HWP_PL_C] / frames), 0u, (unsigned)(ps2hwp_cyc[HWP_PL_D] / frames), (unsigned)(ps2hwp_cyc[HWP_PL_E] / frames), (unsigned)(ps2hwp_cyc[HWP_PL_F] / frames));
	I_OutputMsg("HWPROF36 HWR_DrawDropShadow parts: interp=%u groundz=%u checks=%u getpatch=%u verts=%u quadtest=%u setup=%u polygon=%u\n", (unsigned)(ps2hwp_cyc[HWP_SH_A] / frames), (unsigned)(ps2hwp_cyc[HWP_SH_B] / frames), (unsigned)(ps2hwp_cyc[HWP_SH_C] / frames),
		(unsigned)(ps2hwp_cyc[HWP_SH_D] / frames), (unsigned)(ps2hwp_cyc[HWP_SH_E] / frames), (unsigned)(ps2hwp_cyc[HWP_SH_F] / frames), (unsigned)(ps2hwp_cyc[HWP_SH_G] / frames), (unsigned)(ps2hwp_cyc[HWP_SH_H] / frames));
	I_OutputMsg("HWPROF35 HWR_DrawSprite parts: quad=%u patch=%u aim=%u light=%u blend=%u polygon=%u\n", (unsigned)(ps2hwp_cyc[HWP_DS_A] / frames), (unsigned)(ps2hwp_cyc[HWP_DS_B] / frames), (unsigned)(ps2hwp_cyc[HWP_DS_C] / frames),
		(unsigned)(ps2hwp_cyc[HWP_DS_D] / frames), (unsigned)(ps2hwp_cyc[HWP_DS_E] / frames), (unsigned)(ps2hwp_cyc[HWP_DS_F] / frames));
	I_OutputMsg("HWPROF34 HWR_ProcessSeg parts: setup=%u top/single=%u bottom=%u mid=%u sky=%u remaining=%u 3dfloors=%u\n", (unsigned)(ps2hwp_cyc[HWP_SG_A] / frames), (unsigned)(ps2hwp_cyc[HWP_SG_B] / frames),
		(unsigned)(ps2hwp_cyc[HWP_SG_C] / frames), (unsigned)(ps2hwp_cyc[HWP_SG_D] / frames), (unsigned)(ps2hwp_cyc[HWP_SG_E] / frames), (unsigned)(ps2hwp_cyc[HWP_SG_F] / frames), (unsigned)(ps2hwp_cyc[HWP_SG_G] / frames));
	I_OutputMsg("HWPROF33 HWR_ProjectSprite parts: interp=%u frame=%u offsets=%u position=%u quadtest=%u vissprite=%u | AddSprites total=%u\n", (unsigned)(ps2hwp_cyc[HWP_PS_A] / frames), (unsigned)(ps2hwp_cyc[HWP_PS_B] / frames),
		(unsigned)(ps2hwp_cyc[HWP_PS_C] / frames), (unsigned)(ps2hwp_cyc[HWP_PS_D] / frames), (unsigned)(ps2hwp_cyc[HWP_PS_E] / frames), (unsigned)(ps2hwp_cyc[HWP_PS_F] / frames), (unsigned)(ps2hwp_cyc[HWP_ADDSPR] / frames));
	I_OutputMsg("HWPROF41 sprite batch (FX2): collect=%u | flush: sort=%u plan=%u draw=%u all=%u\n", (unsigned)(ps2hwp_cyc[HWP_SF_COLLECT] / frames), (unsigned)(ps2hwp_cyc[HWP_SF_SORT] / frames), (unsigned)(ps2hwp_cyc[HWP_SF_PLAN] / frames), (unsigned)(ps2hwp_cyc[HWP_SF_DRAW] / frames), (unsigned)(ps2hwp_cyc[HWP_SF_ALL] / frames));
	I_OutputMsg("HWPROF40 sprites (FX2): things=%u projected=%u behind=%u quadhidden=%u vissprites=%u shadows=%u shadowhidden=%u prefiltered=%u drawn_hidden=%u drawn_with_shadow=%u rolled=%u noext=%u\n", ps2hwp_cnt[HWC_FX_THINGS] / frames, ps2hwp_cnt[HWC_FX_PROJ] / frames, ps2hwp_cnt[HWC_FX_BEHIND] / frames,
		ps2hwp_cnt[HWC_FX_QHID] / frames, ps2hwp_cnt[HWC_FX_VIS] / frames, ps2hwp_cnt[HWC_FX_SHADOW] / frames, ps2hwp_cnt[HWC_FX_SHQHID] / frames, ps2hwp_cnt[HWC_FX_PRE] / frames, ps2hwp_cnt[HWC_FX_SPRHID] / frames, ps2hwp_cnt[HWC_FX_SPRSH] / frames, ps2hwp_cnt[HWC_FX_ROLL] / frames, ps2hwp_cnt[HWC_FX_NOEXT] / frames);
	I_OutputMsg("HWPROF31 AddLine: calls=%u backface=%u clipped=%u empty=%u | boxes=%u rejected=%u\n", ps2hwp_cnt[HWC_AL_CALLS] / frames, ps2hwp_cnt[HWC_AL_BACK] / frames, ps2hwp_cnt[HWC_AL_CLIP] / frames,
		ps2hwp_cnt[HWC_AL_EMPTY] / frames, ps2hwp_cnt[HWC_AL_BOX] / frames, ps2hwp_cnt[HWC_AL_BOXREJ] / frames);
	I_OutputMsg("HWPROF30 complex segs by first reason: polyseg=%u ffloors=%u slope=%u heightsec=%u lights=%u midtex=%u back=%u\n", ps2hwp_cnt[HWC_SR_POLY] / frames, ps2hwp_cnt[HWC_SR_FFLOORS] / frames, ps2hwp_cnt[HWC_SR_SLOPE] / frames,
		ps2hwp_cnt[HWC_SR_HEIGHTSEC] / frames, ps2hwp_cnt[HWC_SR_LIGHTS] / frames, ps2hwp_cnt[HWC_SR_MID] / frames, ps2hwp_cnt[HWC_SR_BACK] / frames);
	I_OutputMsg("HWPROF16 segs: one-sided simple=%u two-sided simple=%u complex=%u\n", ps2hwp_cnt[HWC_SEG_SIMPLE1] / frames, ps2hwp_cnt[HWC_SEG_SIMPLE2] / frames, ps2hwp_cnt[HWC_SEG_COMPLEX] / frames);
	I_OutputMsg("HWPROF15 sky dome (driver): calls=%u begin=%u xform=%u emit=%u clipped_tris=%u\n", G.dome_n / frames, G.dome_begin / frames, G.dome_xf / frames, G.dome_emit / frames, G.dome_tris / frames);
	I_OutputMsg("HWPROF12 view set-up: skybox clear=%u dome=%u clip=%u | main setupview=%u clip=%u\n", (unsigned)(ps2hwp_cyc[HWP_K_CLR] / frames), (unsigned)(ps2hwp_cyc[HWP_K_DOME] / frames),
		(unsigned)(ps2hwp_cyc[HWP_K_CLIP] / frames), (unsigned)(ps2hwp_cyc[HWP_M_SETUP] / frames), (unsigned)(ps2hwp_cyc[HWP_M_CLIP] / frames));
	I_OutputMsg("HWPROF14 sprites: shadow=%u draw=%u flush=%u\n", (unsigned)(ps2hwp_cyc[HWP_SP_SHADOW] / frames), (unsigned)(ps2hwp_cyc[HWP_SP_DRAW] / frames), (unsigned)(ps2hwp_cyc[HWP_SP_FLUSH] / frames));
	I_OutputMsg("HWPROF11 sprite batching: calls=%u flush_nonopaque=%u solo=%u shadows=%u | plane cache hit=%u miss=%u (tag) keymiss=%u bypass=%u bad=%u\n", ps2hwp_cnt[HWC_SPR_ON] / frames, ps2hwp_cnt[HWC_SPR_FLUSH] / frames, ps2hwp_cnt[HWC_SPR_SOLO] / frames,
		ps2hwp_cnt[HWC_SPR_SHADOW] / frames, ps2hwp_cnt[HWC_PLANE_HIT] / frames, ps2hwp_cnt[HWC_PLANE_MISS] / frames, ps2hwp_cnt[HWC_PLANE_KEYMISS] / frames, ps2hwp_cnt[HWC_PLANE_BYPASS] / frames, ps2hwp_cnt[HWC_PLANE_BAD]);
	I_OutputMsg("HWPROF10 skybox view: set=%u bg=%u bsp=%u batch=%u sprites=%u nodes=%u\n", (unsigned)(ps2hwp_cyc[HWP_K_SET] / frames), (unsigned)(ps2hwp_cyc[HWP_K_BG] / frames),
		(unsigned)(ps2hwp_cyc[HWP_K_BSP] / frames), (unsigned)(ps2hwp_cyc[HWP_K_BAT] / frames), (unsigned)(ps2hwp_cyc[HWP_K_SPR] / frames), (unsigned)(ps2hwp_cyc[HWP_K_NODE] / frames));
	memset(ps2hwp_cnt, 0, sizeof ps2hwp_cnt);
	memset(&G, 0, sizeof G);
}
#endif
#endif
