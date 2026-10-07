// PS2 GS hardware renderer driver: vertex transform on VU0 in macro mode (included by ps2_hwd.c after ps2_hw_xform.inc).
//
// One engine vertex: clip = M * (x, y, z, 1), the six clip flags against the guard band, 1/w, and the GS integers of the vertex
// (X, Y in 12.4 fixed point with the 2048 offset, Z24) - the arithmetic of xform_vertex + outcode + put_vertex, in about a fifth of the
// EE cycles. The matrix and the batch constants are loaded into VU0 registers once per plan (vu0_batch_load, called by begin_draw);
// no other code of the engine uses COP2. VU0 arithmetic is not IEEE (results truncate, no NaN/inf): the GS integers can differ from the
// scalar path by one LSB (1/16 pixel); tools/ps2/hw_test.c measures that (group vu0_transform).
// The host test build has no VU0: the scalar functions of ps2_hw_xform.inc / ps2_hw_draw.inc are the only path there.

#if defined(__mips__) && !defined(PS2HWD_NO_VU0)
#define HW_VU0 1

typedef struct
{
	s32 xi, yi, zi; // GS X, Y (x16 + 2048 offset), Z24
	float q; // 1 / w
} __attribute__((aligned(16))) pv_t;

static struct
{
	float guard[4]; // 1 / guard_x, 1 / guard_y, 1, 1
	float scale[4]; // kx, ky, zk, 1
	float offset[4]; // ox, oy, zo + bias, 0
	float zmax[4]; // , , Z24 maximum,
} vu0k __attribute__((aligned(16)));

// The plan's constants into VU0: vf1..vf4 = columns of the model-view-projection matrix (16-byte aligned), vf8 guard band,
// vf11 scale, vf12 offset, vf13 Z limit.
static void vu0_batch_load(const float *mvp, float guard_x, float guard_y, float kx, float ky, float zk, float ox, float oy, float zoff, float zmax)
{
	vu0k.guard[0] = 1.0f / guard_x;
	vu0k.guard[1] = 1.0f / guard_y;
	vu0k.guard[2] = 1.0f;
	vu0k.guard[3] = 1.0f;
	vu0k.scale[0] = kx;
	vu0k.scale[1] = ky;
	vu0k.scale[2] = zk;
	vu0k.scale[3] = 1.0f;
	vu0k.offset[0] = ox;
	vu0k.offset[1] = oy;
	vu0k.offset[2] = zoff;
	vu0k.offset[3] = 0.0f;
	vu0k.zmax[0] = 0.0f;
	vu0k.zmax[1] = 0.0f;
	vu0k.zmax[2] = zmax;
	vu0k.zmax[3] = 0.0f;
	__asm__ volatile(
		"lqc2 $vf1, 0x00(%0)\n"
		"lqc2 $vf2, 0x10(%0)\n"
		"lqc2 $vf3, 0x20(%0)\n"
		"lqc2 $vf4, 0x30(%0)\n"
		"lqc2 $vf8, 0x00(%1)\n"
		"lqc2 $vf11, 0x10(%1)\n"
		"lqc2 $vf12, 0x20(%1)\n"
		"lqc2 $vf13, 0x30(%1)\n"
		: : "r"(mvp), "r"(&vu0k) : "memory");
}

// Transforms one vertex: d = clip coordinates (and s, t), o = GS integers and 1/w; returns the outcode in the encoding of outcode()
// (1 x < -gx*w, 2 x > gx*w, 4 y <, 8 y >, 16 z < -w, 32 z > w).
static inline int vu0_xform(const FOutVector *s, cv_t *d, pv_t *o)
{
	float in[4] __attribute__((aligned(16)));
	u32 f;

	in[0] = s->x;
	in[1] = s->y;
	in[2] = s->z;
	in[3] = 1.0f;
	__asm__ volatile(
		"lqc2 $vf5, 0(%1)\n"
		"vmulaw $ACC, $vf4, $vf0\n"
		"vmaddax $ACC, $vf1, $vf5\n"
		"vmadday $ACC, $vf2, $vf5\n"
		"vmaddz $vf6, $vf3, $vf5\n"
		"vmul $vf7, $vf6, $vf8\n"
		"vclipw.xyz $vf7, $vf7\n"
		"vdiv $Q, $vf0w, $vf6w\n"
		"sqc2 $vf6, 0(%2)\n"
		"cfc2 %0, $vi18\n"
		"vwaitq\n"
		"vmulq.xyz $vf9, $vf6, $Q\n"
		"vmulaw $ACC, $vf12, $vf0\n"
		"vmadd $vf10, $vf9, $vf11\n"
		"vmax.z $vf10, $vf10, $vf0\n"
		"vmini.z $vf10, $vf10, $vf13\n"
		"vftoi0.xyz $vf10, $vf10\n"
		"vmulq.w $vf10, $vf0, $Q\n"
		"sqc2 $vf10, 0(%3)\n"
		: "=&r"(f) : "r"(in), "r"(d), "r"(o) : "memory"); // early clobber: cfc2 writes %0 before the last store reads %3 (PS2-HW-44)
	d->a[0] = s->s;
	d->a[1] = s->t;
	f &= 0x3F; // VU0: bit 0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z (|w| is the limit)
	f = ((f & 0x15u) << 1) | ((f >> 1) & 0x15u);
	return d->w <= 0.0f ? 0x3F : (int)f; // behind the eye: the polygon is clipped, whatever |w| says
}
// Two vertices at once (PS2-HW-62). The multiply chain of the second vertex runs while the first one waits for the divide, the guard band tests of both
// precede the divide, and the clip flags of both are read by one cfc2 (the 24 bit history: the second vertex in bits 0..5, the first in 6..11); the divide of
// the second vertex runs while the first one is projected. The arithmetic of every vertex is the one of vu0_xform, operation for operation (the only
// difference: the projection writes xyz and the 1/w into w with a mask instead of the other way round), so the results are bit-identical.
static inline void vu0_xform2(const FOutVector *s0, const FOutVector *s1, cv_t *d0, cv_t *d1, pv_t *o0, pv_t *o1, int *oc0, int *oc1)
{
	float in[8] __attribute__((aligned(16)));
	u32 f, fa, fb;

	in[0] = s0->x;
	in[1] = s0->y;
	in[2] = s0->z;
	in[3] = 1.0f;
	in[4] = s1->x;
	in[5] = s1->y;
	in[6] = s1->z;
	in[7] = 1.0f;
	__asm__ volatile(
		"lqc2 $vf5, 0(%1)\n"
		"lqc2 $vf14, 16(%1)\n"
		"vmulaw $ACC, $vf4, $vf0\n"
		"vmaddax $ACC, $vf1, $vf5\n"
		"vmadday $ACC, $vf2, $vf5\n"
		"vmaddz $vf6, $vf3, $vf5\n"
		"vmulaw $ACC, $vf4, $vf0\n"
		"vmaddax $ACC, $vf1, $vf14\n"
		"vmadday $ACC, $vf2, $vf14\n"
		"vmaddz $vf15, $vf3, $vf14\n"
		"vmul $vf7, $vf6, $vf8\n"
		"vmul $vf16, $vf15, $vf8\n"
		"vclipw.xyz $vf7, $vf7\n"
		"vclipw.xyz $vf16, $vf16\n"
		"vdiv $Q, $vf0w, $vf6w\n"
		"sqc2 $vf6, 0(%2)\n"
		"sqc2 $vf15, 0(%3)\n"
		"cfc2 %0, $vi18\n"
		"vwaitq\n"
		"vmulq.xyz $vf9, $vf6, $Q\n"
		"vmulq.w $vf10, $vf0, $Q\n"
		"vdiv $Q, $vf0w, $vf15w\n"
		"vmulaw $ACC, $vf12, $vf0\n"
		"vmadd.xyz $vf10, $vf9, $vf11\n"
		"vmax.z $vf10, $vf10, $vf0\n"
		"vmini.z $vf10, $vf10, $vf13\n"
		"vftoi0.xyz $vf10, $vf10\n"
		"sqc2 $vf10, 0(%4)\n"
		"vwaitq\n"
		"vmulq.xyz $vf17, $vf15, $Q\n"
		"vmulq.w $vf18, $vf0, $Q\n"
		"vmulaw $ACC, $vf12, $vf0\n"
		"vmadd.xyz $vf18, $vf17, $vf11\n"
		"vmax.z $vf18, $vf18, $vf0\n"
		"vmini.z $vf18, $vf18, $vf13\n"
		"vftoi0.xyz $vf18, $vf18\n"
		"sqc2 $vf18, 0(%5)\n"
		: "=&r"(f) : "r"(in), "r"(d0), "r"(d1), "r"(o0), "r"(o1) : "memory");
	d0->a[0] = s0->s;
	d0->a[1] = s0->t;
	d1->a[0] = s1->s;
	d1->a[1] = s1->t;
	fa = (f >> 6) & 0x3F;
	fb = f & 0x3F;
	fa = ((fa & 0x15u) << 1) | ((fa >> 1) & 0x15u);
	fb = ((fb & 0x15u) << 1) | ((fb >> 1) & 0x15u);
	*oc0 = d0->w <= 0.0f ? 0x3F : (int)fa;
	*oc1 = d1->w <= 0.0f ? 0x3F : (int)fb;
}
#endif
