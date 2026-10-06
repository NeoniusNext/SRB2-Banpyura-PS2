// PS2 GS hardware renderer driver: register value builders that need no hardware (included by ps2_hwd.c, and by the
// x86 host test tools/ps2/hw_hosttest.c).

static inline void ad_set(qw_t *p, u64 data, u32 reg)
{
	p->d[0] = data;
	p->d[1] = reg;
}

// FRAME_1 / ZBUF_1 values for a frame buffer
static u64 frame_reg(int buf, u32 fbmsk)
{
	return (u64)(H.fb_addr[buf] / 8192) | ((u64)(H.fbw / 64) << 16) | ((u64)H.fbpsm << 24) | ((u64)fbmsk << 32);
}

static u64 zbuf_reg(int zmsk)
{
	return (u64)(H.z_addr / 8192) | ((u64)ZPSM_24 << 24) | ((u64)(zmsk ? 1 : 0) << 32);
}

static u64 scissor_reg(int x0, int y0, int x1, int y1)
{
	return (u64)x0 | ((u64)x1 << 16) | ((u64)y0 << 32) | ((u64)y1 << 48);
}
