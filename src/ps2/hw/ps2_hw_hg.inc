// PS2 GS hardware renderer driver: geometry-path counters (OPT10 HG, PS2-HW-40..59). Included by ps2_hwd.c right after ps2_hw_priv.inc.
// Printed as "HWPROF3" lines next to HWPROF (ps2/i_video.c, -ps2prof): per frame averages over the window of 105 frames.

static struct
{
	u32 batches, fans; // PS2HWD_DrawFans calls, polygons in them
	u32 single; // hw_DrawPolygon calls (not batched: sprites, HUD, sky wall...)
	u32 p_vu, p_fast, p_lit, p_cut, p_water; // polygons by the way emit_poly drew them
	u32 begin_n, begin_cyc; // begin_draw calls and cycles
	u32 tri_n; // emit_tris calls
	u32 vu_chunks;
} G;

void PS2HWD_ProfExtra(unsigned int frames)
{
	if (!frames)
		frames = 1;
	I_OutputMsg("HWPROF3 batches=%u fans=%u single=%u vu=%u fast=%u lit=%u cut=%u water=%u begin=%u/%u tris=%u vuchunks=%u\n", G.batches / frames, G.fans / frames, G.single / frames,
		G.p_vu / frames, G.p_fast / frames, G.p_lit / frames, G.p_cut / frames, G.p_water / frames, G.begin_n / frames, G.begin_cyc / frames, G.tri_n / frames, G.vu_chunks / frames);
	memset(&G, 0, sizeof G);
}
