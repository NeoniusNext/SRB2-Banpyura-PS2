/* Isolated varying-Z check, appended after the frozen stencil helper. */
void TN_StencilGradient(int z32, int nodepth, int zwrite, u32 rgba)
{
	const u32 depth[3] = {0x00ffffffu, 0x00654321u, 0x00123457u};
	const u64 test = (1ull << 16) | ((u64)(nodepth && !z32 ? 1 : 2) << 17);
	const u64 zbuf = (u64)(H.z_addr / 8192u) | ((u64)(!z32) << 24) | ((u64)(!zwrite) << 32);
	qw_t *p; int i;
	frame_prepare(0);
	tn_stencil_state(frame_reg(H.target, 0), test, zbuf, rgba, H.vh);
	p = pk_alloc(4);
	p[0].d[0] = HWGIF_TAG(3, 1, 1, PRIM_TRI | ((u32)tn_stencil_context << 9), 0, 1); p[0].d[1] = GIF_REGS_XYZ;
	for (i = 0; i < 3; i++) {
		p[1 + i].w[0] = (u32)(2048 + (i == 1 ? H.vw : 0)) * 16u;
		p[1 + i].w[1] = (u32)(2048 + (i == 2 ? H.vh : 0)) * 16u;
		p[1 + i].w[2] = depth[i] | (z32 && nodepth ? 0x01000000u : 0);
		p[1 + i].w[3] = 0;
	}
	H.gsr.valid = 0;
}

void TN_StencilFog(int xyzf2, u32 fog, u32 rgba)
{
	const u64 test = (1ull << 16) | ((u64)(xyzf2 ? 1 : 2) << 17);
	const u64 zbuf = (u64)(H.z_addr / 8192u) | ((u64)xyzf2 << 24) | (1ull << 32);
	qw_t *p; int i;
	frame_prepare(0); tn_stencil_state(frame_reg(H.target, 0), test, zbuf, rgba, H.vh);
	p = ad_alloc(1); ad_set(p, 0x00675432u, GSR_FOGCOL);
	/* Poison the shared fog attribute with a culled XYZF2 point first, so
	 * the candidate cannot inherit the preceding reference's fog value. */
	p = pk_alloc(2); p[0].d[0] = HWGIF_TAG(1, 1, 1, PRIM_POINT | PRIM_FGE | ((u32)tn_stencil_context << 9), 0, 1); p[0].d[1] = GIF_REGS_XYZF;
	p[1].w[0] = 2038u * 16u; p[1].w[1] = 2038u * 16u; p[1].w[2] = 0; p[1].w[3] = (255u - fog) << 4;
	p = ad_alloc(1); ad_set(p, (u64)fog << 56, GSR_FOG);
	p = pk_alloc(4); p[0].d[0] = HWGIF_TAG(3, 1, 1, PRIM_TRI | PRIM_FGE | ((u32)tn_stencil_context << 9), 0, 1); p[0].d[1] = xyzf2 ? GIF_REGS_XYZF : GIF_REGS_XYZ;
	for (i = 0; i < 3; i++) {
		p[1 + i].w[0] = (u32)(2048 + (i == 1 ? H.vw : 0)) * 16u;
		p[1 + i].w[1] = (u32)(2048 + (i == 2 ? H.vh : 0)) * 16u;
		p[1 + i].w[2] = xyzf2 ? 0x00654321u << 4 : 0x01654321u;
		p[1 + i].w[3] = xyzf2 ? fog << 4 : 0;
	}
	H.gsr.valid = 0;
}
