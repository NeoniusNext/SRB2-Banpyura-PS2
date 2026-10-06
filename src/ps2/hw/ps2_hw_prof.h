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
	HWP_NUM
};

extern unsigned long long ps2hwp_cyc[HWP_NUM];

static inline unsigned int ps2hwp_now(void)
{
	unsigned int v;
	__asm__ volatile("mfc0 %0,$9" : "=r"(v));
	return v;
}

// HWP_LOCAL starts a lap timer in a function, HWP_LAP(idx) adds the cycles since the previous lap to the accumulator idx
#define HWP_LOCAL unsigned int hwp_t0 = ps2hwp_now(), hwp_t1
#define HWP_LAP(idx) do { hwp_t1 = ps2hwp_now(); ps2hwp_cyc[idx] += (unsigned int)(hwp_t1 - hwp_t0); hwp_t0 = hwp_t1; } while (0)
#else
#define HWP_LOCAL ((void)0)
#define HWP_LAP(idx) ((void)0)
#endif

#endif
