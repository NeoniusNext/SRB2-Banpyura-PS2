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
	u32 tri_n; // emit_tris calls
	u32 vu_chunks;
	u32 sing_by[8]; // hw_DrawPolygon calls by (PF_NoTexture 1 | PF_NoDepthTest 2 | PF_Occlude 4)
	u32 sing_cyc[8]; // their cycles
	u32 c_fast, c_lit, c_cut, c_water; // cycles of emit_poly by branch
	u32 plan_hit, plan_miss; // plan cache (PS2-HW-46)
	u32 p_cutold, c_cutold; // polygons / cycles of the old cut_and_emit (repeat cuts, wide polygons)
	u32 pc_key, pc_copy, pc_touch, pc_misc, pc_miss; // cycles: key + lookup, P/LF copy, texture touch, rest of a hit, a miss
} G;

#ifdef __mips__ // (the x86 host tests have no log)
void PS2HWD_ProfExtra(unsigned int frames)
{
	if (!frames)
		frames = 1;
	I_OutputMsg("HWPROF3 batches=%u fans=%u single=%u vu=%u fast=%u lit=%u cut=%u water=%u begin=%u/%u tris=%u vuchunks=%u planhit=%u planmiss=%u\n", G.batches / frames, G.fans / frames, G.single / frames,
		G.p_vu / frames, G.p_fast / frames, G.p_lit / frames, G.p_cut / frames, G.p_water / frames, G.begin_n / frames, G.begin_cyc / frames, G.tri_n / frames, G.vu_chunks / frames, G.plan_hit / frames, G.plan_miss / frames);
	I_OutputMsg("HWPROF9 cut_and_emit (repeats etc.) polys=%u cyc=%u\n", G.p_cutold / frames, G.c_cutold / frames);
	I_OutputMsg("HWPROF8 plan cache cycles: key=%u copy=%u touch=%u misc=%u miss=%u\n", G.pc_key / frames, G.pc_copy / frames, G.pc_touch / frames, G.pc_misc / frames, G.pc_miss / frames);
	I_OutputMsg("HWPROF4 segs=%u subsecs=%u planes=%u sprites=%u proc=%u procbatch=%u\n", ps2hwp_cnt[HWC_SEGS] / frames, ps2hwp_cnt[HWC_SUBSECS] / frames, ps2hwp_cnt[HWC_PLANES] / frames,
		ps2hwp_cnt[HWC_SPRITES] / frames, ps2hwp_cnt[HWC_PROC] / frames, ps2hwp_cnt[HWC_PROC_BATCH] / frames);
	I_OutputMsg("HWPROF7 single_cyc t0d0o0=%u nt=%u nd=%u nt+nd=%u o=%u o+nt=%u o+nd=%u o+nt+nd=%u | emit cyc fast=%u lit=%u cut=%u water=%u\n", G.sing_cyc[0] / frames, G.sing_cyc[1] / frames, G.sing_cyc[2] / frames, G.sing_cyc[3] / frames,
		G.sing_cyc[4] / frames, G.sing_cyc[5] / frames, G.sing_cyc[6] / frames, G.sing_cyc[7] / frames, G.c_fast / frames, G.c_lit / frames, G.c_cut / frames, G.c_water / frames);
	I_OutputMsg("HWPROF5 single_by(tex/depth/occl) t0d0o0=%u nt=%u nd=%u nt+nd=%u o=%u o+nt=%u o+nd=%u o+nt+nd=%u\n", G.sing_by[0] / frames, G.sing_by[1] / frames, G.sing_by[2] / frames, G.sing_by[3] / frames,
		G.sing_by[4] / frames, G.sing_by[5] / frames, G.sing_by[6] / frames, G.sing_by[7] / frames);
	I_OutputMsg("HWPROF6 pal=%u frame=%u clr1=%u net=%u clr2=%u\n", (unsigned)(ps2hwp_cyc[HWP_S_PAL] / frames), (unsigned)(ps2hwp_cyc[HWP_S_FRAME] / frames), (unsigned)(ps2hwp_cyc[HWP_S_CLR1] / frames),
		(unsigned)(ps2hwp_cyc[HWP_S_NET] / frames), (unsigned)(ps2hwp_cyc[HWP_S_CLR2] / frames));
	I_OutputMsg("HWPROF10 skybox view: set=%u bg=%u bsp=%u batch=%u sprites=%u nodes=%u\n", (unsigned)(ps2hwp_cyc[HWP_K_SET] / frames), (unsigned)(ps2hwp_cyc[HWP_K_BG] / frames),
		(unsigned)(ps2hwp_cyc[HWP_K_BSP] / frames), (unsigned)(ps2hwp_cyc[HWP_K_BAT] / frames), (unsigned)(ps2hwp_cyc[HWP_K_SPR] / frames), (unsigned)(ps2hwp_cyc[HWP_K_NODE] / frames));
	memset(ps2hwp_cnt, 0, sizeof ps2hwp_cnt);
	memset(&G, 0, sizeof G);
}
#endif
#endif
