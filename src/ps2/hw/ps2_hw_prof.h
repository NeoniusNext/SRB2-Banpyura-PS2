// PS2 GS hardware renderer: COP0 cycle accumulators of the engine side of the renderer (hardware/hw_main.c), printed by
// ps2/i_video.c as "HWPROF" lines every 105 frames (docs/GATES/g1/opt3-H.md). Only built for the PS2 profile.

#ifndef __PS2_HW_PROF_H__
#define __PS2_HW_PROF_H__

#ifdef PS2_PROFILE
enum
{
	HWP_CLEAR, // clear buffers, HWR_ClearView, sky background, setup
	HWP_BSP, // HWR_RenderBSPNode (walls, planes, FOF), includes the polygons the batcher hands to the driver
	HWP_BATCH, // HWR_RenderBatches
	HWP_SPRITES, // sort + draw sprites (models too)
	HWP_NODES, // HWR_CreateDrawNodes: sorted translucent planes and walls
	HWP_POST, // post-processing, screen captures
	HWP_NUM_BASE,
	// OPT9 (PS2-HW-40): finer, partly nested (inclusive) spans; printed as "HWPROF2" lines
	HWP_SETUP = HWP_NUM_BASE, // HWR_RenderPlayerView start .. before the sky: colour clear, SetupView (R_SetupFrame, palette), ClearView
	HWP_SKY, // HWR_DrawSkyBackground
	HWP_SEG, // HWR_ProcessSeg (inclusive: textures, lighting, polygons handed to the batcher), inside HWP_BSP
	HWP_PLANE, // HWR_RenderPlane (inclusive), inside HWP_BSP / HWP_NODES
	HWP_ADDSPR, // HWR_AddSprites (HWR_ProjectSprite for every thing of a sector), inside HWP_BSP
	HWP_PPOLY, // HWR_ProcessPolygon (inclusive; the batcher copy or, outside batching, the driver draw), inside the others
	HWP_SPRSORT, // HWR_SortVisSprites
	HWP_SPRDRAW, // HWR_DrawSprites
	HWP_NODESORT, // sorting in HWR_CreateDrawNodes
	HWP_NODEDRAW, // drawing in HWR_CreateDrawNodes
	HWP_BATCHSORT, // sort of HWR_RenderBatches
	HWP_BATCHDRAW, // driver draws of HWR_RenderBatches
	HWP_SUBSEC, // HWR_Subsector (inclusive: planes, sprites, segs)
	HWP_LIGHT, // HWR_Lighting
	// OPT10 (HG): parts of the setup span (HWPROF6)
	HWP_S_PAL, // ST_doPaletteStuff
	HWP_S_FRAME, // R_SetupFrame
	HWP_S_CLR1, // ClearBuffer(colour)
	HWP_S_NET, // NetUpdate
	HWP_S_CLR2, // HWR_ClearView
	HWP_S_SETSH, // SetTransform / shader state / PS_ perf timing
	// parts of the skybox view (HWR_RenderSkyboxView; HWPROF10)
	HWP_K_SET, // HWR_SetupView
	HWP_K_BG, // HWR_ClearView, HWR_DrawSkyBackground, clipper
	HWP_K_BSP, // HWR_RenderBSPNode
	HWP_K_BAT, // HWR_RenderBatches
	HWP_K_SPR, // sprites
	HWP_K_NODE, // HWR_CreateDrawNodes
	HWP_K_CLR, // HWR_ClearView
	HWP_K_DOME, // HWR_DrawSkyBackground
	HWP_K_CLIP, // clipper set-up, SetTransform, shader state (to the BSP walk)
	HWP_M_SETUP, // main view: HWR_SetupView
	HWP_M_CLIP, // main view: clipper set-up .. before the BSP walk
	HWP_SP_SHADOW, // HWR_DrawSprites: drop shadows (HWR_DrawDropShadow)
	HWP_SP_DRAW, // HWR_DrawSprites: HWR_DrawSprite / precipitation / models
	HWP_SP_FLUSH, // HWR_DrawSprites: HWR_RenderBatches of the collected sprites
	HWP_PL_HIT, // HWR_RenderPlane: served from the plane cache
	HWP_PL_MISS, // ... calculated and stored
	HWP_PL_BYP, // ... not cacheable (slope, horizon line, ...)
	// parts of HWR_RenderBatches (HWPROF21): all views, and the skybox view alone (KB_)
	HWP_B_INIT, // PS2HWD_BatchDraw + the index array
	HWP_B_PLAN, // PS2HWD_PlanBegin .. PlanEnd (the texture planner)
	HWP_B_END, // PS2HWD_BatchEnd
	HWP_KB_INIT,
	HWP_KB_SORT,
	HWP_KB_PLAN,
	HWP_KB_DRAW,
	HWP_KB_END,
	HWP_B_DB, // HWR_DrawBatch (PS2HWD_DrawFans)
	HWP_B_TEX, // HWD.pfnSetTexture between the batches
	HWP_KB_DB,
	HWP_KB_TEX,
	// OPT11 (GEOM): HWR_ProjectSprite by parts (HWPROF33): interpolation, frame and angle, offsets and shadow, position and culling, quad test, vissprite
	HWP_PS_A, HWP_PS_B, HWP_PS_C, HWP_PS_D, HWP_PS_E, HWP_PS_F,
	// HWR_ProcessSeg by parts (HWPROF34): set-up, two sided: top texture / bottom / middle / sky; one sided: wall, (sky walls: E), the rest up to the 3D floors, 3D floors
	HWP_SG_A, HWP_SG_B, HWP_SG_C, HWP_SG_D, HWP_SG_E, HWP_SG_F, HWP_SG_G,
	// HWR_DrawSprite by parts (HWPROF35): quad, patch (HWR_GetMappedPatch), flip / aim, lighting, blend set-up, HWR_ProcessPolygon
	HWP_DS_A, HWP_DS_B, HWP_DS_C, HWP_DS_D, HWP_DS_E, HWP_DS_F,
	// HWR_DrawDropShadow by parts (HWPROF36): interpolation, ground height, patch lookup, HWR_GetPatch, vertices, quad test, set-up, polygon
	HWP_SH_A, HWP_SH_B, HWP_SH_C, HWP_SH_D, HWP_SH_E, HWP_SH_F, HWP_SH_G, HWP_SH_H,
	// HWR_RenderPlane by parts (HWPROF37): set-up and slope, buffer and flat size, vertex loop, slope light, lighting, polygon (HWR_ProcessPolygon)
	HWP_PL_A, HWP_PL_B, HWP_PL_C, HWP_PL_D, HWP_PL_E, HWP_PL_F,
	// OPT11 round 2 (FX2, --hwdetail): the flush of the sprite batch by parts (HWPROF41): collect (HWR_ProcessPolygon of a batched sprite), sort, plan, draw, the rest
	HWP_SF_COLLECT, HWP_SF_SORT, HWP_SF_PLAN, HWP_SF_DRAW, HWP_SF_ALL,
	HWP_NUM
};

extern unsigned long long ps2hwp_cyc[HWP_NUM];

// OPT10 (HG): event counters of the engine side (HWPROF3 line, per frame)
enum { HWC_SEGS, HWC_SUBSECS, HWC_PLANES, HWC_SPRITES, HWC_PROC, HWC_PROC_BATCH, HWC_SPR_ON, HWC_SPR_FLUSH, HWC_SPR_SOLO, HWC_SPR_SHADOW, HWC_PLANE_HIT, HWC_PLANE_MISS, HWC_PLANE_KEYMISS, HWC_PLANE_BYPASS, HWC_PLANE_BAD, HWC_SEG_SIMPLE1, HWC_SEG_SIMPLE2, HWC_SEG_COMPLEX, HWC_PKM_H, HWC_PKM_L, HWC_PKM_F, HWC_PKM_T, HWC_PKM_O, HWC_PKM_E, HWC_PKM_P,
	// OPT11 (GEOM): why a seg is not simple (first reason that applies), HWPROF30
	HWC_SR_POLY, HWC_SR_FFLOORS, HWC_SR_SLOPE, HWC_SR_HEIGHTSEC, HWC_SR_LIGHTS, HWC_SR_MID, HWC_SR_BACK,
	HWC_AL_CALLS, HWC_AL_BACK, HWC_AL_CLIP, HWC_AL_EMPTY, HWC_AL_BOX, HWC_AL_BOXREJ, HWC_AL_VHIT,
	// OPT11 round 2 (FX2): the sprites of a frame (HWPROF40, --hwdetail): things of the sectors, ProjectSprite calls, behind the view, quad hidden, vissprites, shadows, shadows hidden
	HWC_FX_THINGS, HWC_FX_PROJ, HWC_FX_BEHIND, HWC_FX_QHID, HWC_FX_VIS, HWC_FX_SHADOW, HWC_FX_SHQHID, HWC_FX_PRE, HWC_FX_SPRHID, HWC_FX_SPRSH, HWC_FX_ROLL, HWC_FX_NOEXT, HWC_NUM };
extern unsigned int ps2hwp_cnt[HWC_NUM];
extern int ps2hwp_skyview; // 1 while HWR_RenderSkyboxView runs (the counters of the driver are split by view)
#define HWC_ADD(i) (ps2hwp_cnt[i]++)

static inline unsigned int ps2hwp_now(void)
{
	unsigned int v;
	__asm__ volatile("mfc0 %0,$9" : "=r"(v));
	return v;
}

// HWP_LOCAL starts a lap timer in a function, HWP_LAP(idx) adds the cycles since the previous lap to the accumulator idx
#define HWP_LOCAL unsigned int hwp_t0 = ps2hwp_now(), hwp_t1
#define HWP_LAP(idx) do { hwp_t1 = ps2hwp_now(); ps2hwp_cyc[idx] += (unsigned int)(hwp_t1 - hwp_t0); hwp_t0 = hwp_t1; } while (0)
// nested (inclusive) span: HWP_SPAN_BEGIN(name) ... HWP_SPAN_END(name, idx)
#define HWP_SPAN_BEGIN(name) unsigned int name = ps2hwp_now()
#define HWP_SPAN_END(name, idx) do { ps2hwp_cyc[idx] += (unsigned int)(ps2hwp_now() - (name)); } while (0)
// the same, also added to idxk while the skybox view runs
#define HWP_SPAN_END2(name, idx, idxk) do { const unsigned int d_ = (unsigned int)(ps2hwp_now() - (name)); ps2hwp_cyc[idx] += d_; if (ps2hwp_skyview) ps2hwp_cyc[idxk] += d_; } while (0)
#ifdef PS2_HWDETAIL
// OPT11 (GEOM): the fine laps inside the engine functions (HWPROF33..37) and the counters of AddLine (HWPROF31) cost 10 cycles each: only in a build.py --hwdetail build
#define HWD_LOCAL HWP_LOCAL
#define HWD_LAP(idx) HWP_LAP(idx)
#define HWD_ADD(i) HWC_ADD(i)
#define HWD_ADDC(acc) do { hwp_t1 = ps2hwp_now(); (acc) += (unsigned int)(hwp_t1 - hwp_t0); hwp_t0 = hwp_t1; } while (0) // OPT11 round 2: a lap into a plain counter
#else
#define HWD_LOCAL ((void)0)
#define HWD_LAP(idx) ((void)0)
#define HWD_ADD(i) ((void)0)
#define HWD_ADDC(acc) ((void)0)
#endif
#else
#define HWD_LOCAL ((void)0)
#define HWD_LAP(idx) ((void)0)
#define HWD_ADD(i) ((void)0)
#define HWD_ADDC(acc) ((void)0)
#define HWP_LOCAL ((void)0)
#define HWP_LAP(idx) ((void)0)
#define HWP_SPAN_BEGIN(name) ((void)0)
#define HWP_SPAN_END(name, idx) ((void)0)
#define HWP_SPAN_END2(name, idx, idxk) ((void)0)
#define HWC_ADD(i) ((void)0)
#define ps2hwp_skyview 0
#endif

#endif
