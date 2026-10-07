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
	HWP_NUM
};

extern unsigned long long ps2hwp_cyc[HWP_NUM];

// OPT10 (HG): event counters of the engine side (HWPROF3 line, per frame)
enum { HWC_SEGS, HWC_SUBSECS, HWC_PLANES, HWC_SPRITES, HWC_PROC, HWC_PROC_BATCH, HWC_SPR_ON, HWC_SPR_FLUSH, HWC_SPR_SOLO, HWC_SPR_SHADOW, HWC_PLANE_HIT, HWC_PLANE_MISS, HWC_PLANE_KEYMISS, HWC_PLANE_BYPASS, HWC_PLANE_BAD, HWC_SEG_SIMPLE1, HWC_SEG_SIMPLE2, HWC_SEG_COMPLEX, HWC_NUM };
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
#else
#define HWP_LOCAL ((void)0)
#define HWP_LAP(idx) ((void)0)
#define HWP_SPAN_BEGIN(name) ((void)0)
#define HWP_SPAN_END(name, idx) ((void)0)
#define HWC_ADD(i) ((void)0)
#define ps2hwp_skyview 0
#endif

#endif
