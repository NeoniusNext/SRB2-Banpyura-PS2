// OPT10-HF: host test of the GIF stream validator (src/ps2/hw/ps2_hw_val.inc, PS2-HW-74).
// Feeds hand made GIF streams (the shapes the driver writes: A+D state, texture upload, packed fan, reglist fan, sprite) through val_buffer and checks
// that the clean streams raise nothing and that every seeded fault is reported under the expected class.
//   gcc -O1 -Wall -o /tmp/hf_valtest tools/ps2/hf_valtest.c && /tmp/hf_valtest
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;

typedef union
{
	u64 d[2];
	u32 w[4];
	float f[4];
} qw_t;

static struct
{
	u32 frame_no;
	int fbw, fbh;
} H = {1, 320, 256};
static struct
{
	int on;
} V;
int ps2hwd_dbg_flags;

#define PAGE_BLOCKS 32
#define VRAM_BLOCKS 16384
#define PRIM_POINT 0
#define PRIM_LINE 1
#define PRIM_LINESTRIP 2
#define PRIM_TRI 3
#define PRIM_TRISTRIP 4
#define PRIM_TRIFAN 5
#define PRIM_SPRITE 6
#define PRIM_IIP 8
#define PRIM_TME 16
#define PRIM_ABE 64
#define HWGIF_TAG(nloop, eop, pre, prim, flg, nreg) \
	((u64)(nloop) | ((u64)(eop) << 15) | ((u64)(pre) << 46) | ((u64)(prim) << 47) | ((u64)(flg) << 58) | ((u64)(nreg) << 60))
#define GIF_REGS_AD 0xEull

static void I_OutputMsg(const char *f, ...) __attribute__((format(printf, 1, 2)));
static void I_OutputMsg(const char *f, ...)
{
	va_list ap;

	va_start(ap, f);
	vprintf(f, ap);
	va_end(ap);
}

#include "../../src/ps2/hw/ps2_hw_val.inc"

static qw_t buf[4096];
static u32 n;
static u32 run;

static void begin(void)
{
	memset(&VAL, 0, sizeof VAL);
	VAL.on = 1;
	n = 1;
	run = 0;
	memset(buf, 0, sizeof buf);
}

static qw_t *alloc(u32 k)
{
	qw_t *p = buf + n;

	n += k;
	return p;
}

static void ad(qw_t *p, u64 data, u32 reg)
{
	p->d[0] = data;
	p->d[1] = reg;
}

static void ad_tag(u32 count)
{
	qw_t *p = alloc(1);

	p->d[0] = HWGIF_TAG(count, 1, 0, 0, 0, 1);
	p->d[1] = GIF_REGS_AD;
}

static void ad1(u64 data, u32 reg)
{
	qw_t *p;

	ad_tag(1);
	p = alloc(1);
	ad(p, data, reg);
}

static void finish(void)
{
	buf[run].d[0] = (u64)(n - run - 1) | (7ull << 28);
	val_reset();
	val_buffer(buf, n);
}

static u64 tex0(u32 tbp, u32 tbw, u32 psm, u32 tw, u32 th, u32 cbp, u32 cld)
{
	return (u64)tbp | ((u64)tbw << 14) | ((u64)psm << 20) | ((u64)tw << 26) | ((u64)th << 30) | (1ull << 34) | ((u64)cbp << 37) | ((u64)cld << 61);
}

static float fl(u32 w)
{
	union { u32 w; float f; } u;

	u.w = w;
	return u.f;
}

static u32 fw(float f)
{
	union { u32 w; float f; } u;

	u.f = f;
	return u.w;
}

// packed STQ/RGBAQ/XYZ2 vertex
static void vtx(float s, float t, float q, int x, int y, u32 z)
{
	qw_t *p = alloc(3);

	p[0].d[0] = (u64)fw(s) | ((u64)fw(t) << 32);
	p[0].d[1] = fw(q);
	p[1].d[0] = 0x80 | (0x80ull << 32);
	p[1].d[1] = 0x80 | (0x80ull << 32);
	p[2].d[0] = (u64)(x & 0xFFFF) | ((u64)(y & 0xFFFF) << 32);
	p[2].d[1] = z;
}

static void fan3(u32 prim, float s2, int xv, u32 z)
{
	qw_t *p = alloc(1);

	p->d[0] = HWGIF_TAG(3, 1, 1, prim, 0, 3);
	p->d[1] = 2ull | (1ull << 4) | (5ull << 8);
	vtx(0, 0, 1, 0x8000 + 16 * 10, 0x8000 + 16 * 10, z);
	vtx(s2, 0, 1, xv, 0x8000 + 16 * 10, z);
	vtx(s2, 1, 1, xv, 0x8000 + 16 * 50, z);
}

static void state(void)
{
	ad_tag(8);
	ad(alloc(1), 0 | (5ull << 16) | (0ull << 24), 0x4C);
	ad(alloc(1), 100 | (1ull << 24), 0x4E);
	ad(alloc(1), 0x8000ull | (0x8000ull << 32), 0x18);
	ad(alloc(1), 0ull | (319ull << 16) | (0ull << 32) | (223ull << 48), 0x40);
	ad(alloc(1), 1 | (6ull << 4) | (1ull << 16) | (2ull << 17), 0x47);
	ad(alloc(1), 0 | (1ull << 2) | (0ull << 4) | (1ull << 6) | (0x80ull << 32), 0x42);
	ad(alloc(1), tex0(2000, 2, 0x13, 7, 6, 4000, 1), 0x06);
	ad(alloc(1), 0, 0x08);
}

static int check(const char *name, int cls, int want)
{
	int ok = want ? VAL.err[cls] > 0 : VAL.err[cls] == 0;
	u32 tot = 0;
	int i;

	for (i = 0; i < VE_NERR; i++)
		tot += VAL.err[i];
	if (!want && tot)
		ok = 0;
	printf("%-34s %s (errors total %u, class %s = %u)\n", name, ok ? "ok" : "FAIL", tot, ve_name[cls], VAL.err[cls]);
	return ok ? 0 : 1;
}

int main(void)
{
	int bad = 0;

	VAL.on = 1;
	// 1. clean: state, a 128x64 PSMT8 texture upload (inline data), TEXFLUSH, a fan, a sprite
	begin();
	state();
	{
		qw_t *p;
		u32 qw = 128 * 64 / 16, i;

		ad_tag(4);
		ad(alloc(1), (u64)2000 << 32 | (u64)2 << 48 | (u64)0x13 << 56, 0x50);
		ad(alloc(1), 0, 0x51);
		ad(alloc(1), 128ull | (64ull << 32), 0x52);
		ad(alloc(1), 0, 0x53);
		p = alloc(1);
		p->d[0] = HWGIF_TAG(qw, 1, 0, 0, 2, 0);
		for (i = 0; i < qw; i++)
			alloc(1)->d[0] = i;
		ad1(0, 0x3F);
	}
	fan3(PRIM_TRIFAN | PRIM_IIP | PRIM_TME | PRIM_ABE, 1.0f, 0x8000 + 16 * 60, 5000);
	{
		qw_t *p = alloc(1);

		p->d[0] = HWGIF_TAG(2, 1, 1, PRIM_SPRITE | PRIM_TME | (1u << 8), 0, 3);
		p->d[1] = 3ull | (1ull << 4) | (5ull << 8);
		p = alloc(6);
		p[0].d[0] = 0;
		p[1].d[0] = p[1].d[1] = 0x80;
		p[2].d[0] = 0x8000 | (0x8000ull << 32);
		p[3].d[0] = 128ull * 16 | ((u64)64 * 16 << 32);
		p[4].d[0] = p[4].d[1] = 0x80;
		p[5].d[0] = (0x8000ull + 128 * 16) | ((0x8000ull + 64 * 16) << 32);
		p[5].d[1] = 100;
	}
	finish();
	bad += check("clean stream", VE_PRIM, 0);

	// 2. REGLIST fan as emit_reglist_fan writes it: PRIM, then ST, RGBAQ(+Q), XYZ2 per vertex (descriptor table fan_reglist_desc[0][3])
	begin();
	state();
	{
		qw_t *p = alloc(1);
		u64 *o;
		int i;

		p->d[0] = HWGIF_TAG(1, 1, 0, 0, 1, 10);
		p->d[1] = 0x0000005125125120ull;
		o = (u64 *)alloc(5);
		*o++ = PRIM_TRIFAN | PRIM_TME;
		for (i = 0; i < 3; i++)
		{
			*o++ = (u64)fw(i == 1 ? 1.0f : 0.0f) | ((u64)fw(i == 2 ? 1.0f : 0.0f) << 32);
			*o++ = 0x80808080ull | ((u64)fw(1.0f) << 32);
			*o++ = (0x8000ull + 160 * (u64)(i + 1)) | ((0x8000ull + 160) << 16) | (5000ull << 32);
		}
	}
	finish();
	bad += check("clean reglist fan", VE_PRIM, 0);

	// the screen texture of the driver: 320 x 200 CT32 at the top of VRAM, TW=9 TH=8, region clamp 0..319 / 0..199 written BEFORE TEX0: clean
	begin();
	state();
	ad1(2ull | (2ull << 2) | (319ull << 14) | (199ull << 34), 0x08);
	ad1(tex0(15264, 5, 0x00, 9, 8, 4000, 1), 0x06);
	fan3(PRIM_TRIFAN | PRIM_TME, 0.5f, 0x8000 + 16 * 60, 5000);
	finish();
	bad += check("clean screen texture at the VRAM end", VE_TEX0VRAM, 0);
	// a 64 x 64 PSMT8 upload that ends exactly at the end of VRAM (blocks 16368..16383): clean; one block further: reported
	begin();
	state();
	ad_tag(4);
	ad(alloc(1), (u64)16368 << 32 | (u64)2 << 48 | (u64)0x13 << 56, 0x50);
	ad(alloc(1), 0, 0x51);
	ad(alloc(1), 64ull | (64ull << 32), 0x52);
	ad(alloc(1), 0, 0x53);
	{
		qw_t *p = alloc(1);
		u32 i;

		p->d[0] = HWGIF_TAG(256, 1, 0, 0, 2, 0);
		for (i = 0; i < 256; i++)
			alloc(1)->d[0] = i;
	}
	finish();
	bad += check("clean upload to the VRAM end", VE_BITBLT, 0);
	begin();
	state();
	ad_tag(4);
	ad(alloc(1), (u64)16369 << 32 | (u64)2 << 48 | (u64)0x13 << 56, 0x50);
	ad(alloc(1), 0, 0x51);
	ad(alloc(1), 64ull | (64ull << 32), 0x52);
	ad(alloc(1), 0, 0x53);
	{
		qw_t *p = alloc(1);
		u32 i;

		p->d[0] = HWGIF_TAG(256, 1, 0, 0, 2, 0);
		for (i = 0; i < 256; i++)
			alloc(1)->d[0] = i;
	}
	finish();
	bad += check("upload one block past VRAM end", VE_BITBLT, 1);

	// seeded faults
#define FAULT(NAME, CLS, BODY) \
	begin(); \
	state(); \
	BODY; \
	finish(); \
	bad += check(NAME, CLS, 1)

	FAULT("TEX0 TW=11", VE_TEX0, ad1(tex0(2000, 2, 0x13, 11, 6, 4000, 1), 0x06));
	FAULT("TEX0 TBW odd for T8", VE_TEX0TBW, ad1(tex0(2000, 3, 0x13, 7, 6, 4000, 1), 0x06));
	FAULT("TEX0 TBW narrower than texture", VE_TEX0TBW, { ad1(tex0(2000, 1, 0x00, 7, 6, 4000, 1), 0x06); fan3(PRIM_TRIFAN | PRIM_TME, 1.0f, 0x8000 + 16 * 60, 5000); });
	FAULT("TEX0 beyond VRAM", VE_TEX0VRAM, { ad1(tex0(16000, 16, 0x00, 10, 10, 4000, 1), 0x06); fan3(PRIM_TRIFAN | PRIM_TME, 1.0f, 0x8000 + 16 * 60, 5000); });
	FAULT("CLAMP region beyond texture", VE_CLAMP, { ad1(2ull | (2ull << 2) | (0ull << 4) | (200ull << 14) | (0ull << 24) | (10ull << 34), 0x08); fan3(PRIM_TRIFAN | PRIM_TME, 1.0f, 0x8000 + 16 * 60, 5000); });
	FAULT("CLAMP overflow into reserved bits", VE_CLAMP, ad1(2ull | (1ull << 50), 0x08));
	FAULT("SCISSOR x1 = 2048", VE_SCISSOR, ad1(0ull | (2048ull << 16) | (223ull << 48), 0x40));
	FAULT("ALPHA selector 3", VE_ALPHA, ad1(3ull, 0x42));
	FAULT("TEST ZTE=0", VE_TESTZTE, ad1(1ull, 0x47));
	FAULT("FRAME FBW overflow", VE_FRAME, ad1((u64)64 << 16, 0x4C));
	FAULT("context 2 register", VE_CTX2, ad1(0, 0x4D));
	FAULT("unknown register", VE_REGUNK, ad1(0, 0x70));
	FAULT("TRXREG too large", VE_TRX, ad1(5000ull | (10ull << 32), 0x52));
	FAULT("BITBLTBUF DBW odd for T8", VE_BITBLT, ad1((u64)2000 << 32 | (u64)3 << 48 | (u64)0x13 << 56, 0x50));
	FAULT("image without transfer", VE_IMGSIZE, { qw_t *p = alloc(2); p[0].d[0] = HWGIF_TAG(1, 1, 0, 0, 2, 0); });
	FAULT("TEXFLUSH missing after upload", VE_TEXFLUSH, {
		ad_tag(4);
		ad(alloc(1), (u64)2000 << 32 | (u64)2 << 48 | (u64)0x13 << 56, 0x50);
		ad(alloc(1), 0, 0x51);
		ad(alloc(1), 128ull | (64ull << 32), 0x52);
		ad(alloc(1), 0, 0x53);
		{
			qw_t *p = alloc(1);
			u32 i;

			p->d[0] = HWGIF_TAG(512, 1, 0, 0, 2, 0);
			for (i = 0; i < 512; i++)
				alloc(1)->d[0] = i;
		}
		fan3(PRIM_TRIFAN | PRIM_TME, 1.0f, 0x8000 + 16 * 60, 5000);
	});
	FAULT("image data too long", VE_IMGSIZE, {
		ad_tag(4);
		ad(alloc(1), (u64)2000 << 32 | (u64)2 << 48 | (u64)0x13 << 56, 0x50);
		ad(alloc(1), 0, 0x51);
		ad(alloc(1), 128ull | (64ull << 32), 0x52);
		ad(alloc(1), 0, 0x53);
		{
			qw_t *p = alloc(1);
			u32 i;

			p->d[0] = HWGIF_TAG(520, 1, 0, 0, 2, 0);
			for (i = 0; i < 520; i++)
				alloc(1)->d[0] = i;
		}
	});
	FAULT("register write inside a transfer", VE_IMGORDER, {
		ad_tag(4);
		ad(alloc(1), (u64)2000 << 32 | (u64)2 << 48 | (u64)0x13 << 56, 0x50);
		ad(alloc(1), 0, 0x51);
		ad(alloc(1), 128ull | (64ull << 32), 0x52);
		ad(alloc(1), 0, 0x53);
		ad1(0, 0x3F);
	});
	FAULT("vertex outside the guard band", VE_VTXRANGE, fan3(PRIM_TRIFAN, 1.0f, 0x8000 + 16 * 2030, 5000));
	FAULT("Z above Z24", VE_VTXZ, fan3(PRIM_TRIFAN, 1.0f, 0x8000 + 16 * 60, 0x1000000u));
	FAULT("texel coordinate beyond 4096", VE_STRANGE, fan3(PRIM_TRIFAN | PRIM_TME, 40.0f, 0x8000 + 16 * 60, 5000));
	FAULT("negative Q", VE_STQ, {
		qw_t *p = alloc(1);

		p->d[0] = HWGIF_TAG(3, 1, 1, PRIM_TRIFAN | PRIM_TME, 0, 3);
		p->d[1] = 2ull | (1ull << 4) | (5ull << 8);
		vtx(0, 0, -1.0f, 0x8000, 0x8000, 100);
		vtx(1, 0, -1.0f, 0x8000 + 160, 0x8000, 100);
		vtx(1, 1, -1.0f, 0x8000 + 160, 0x8000 + 160, 100);
	});
	FAULT("vertex count not a primitive", VE_PRIMCOUNT, {
		qw_t *p = alloc(1);

		p->d[0] = HWGIF_TAG(2, 1, 1, PRIM_TRI, 0, 3);
		p->d[1] = 2ull | (1ull << 4) | (5ull << 8);
		vtx(0, 0, 1, 0x8000, 0x8000, 100);
		vtx(1, 0, 1, 0x8000 + 160, 0x8000, 100);
		fan3(PRIM_TRIFAN, 1.0f, 0x8000 + 16 * 60, 5000);
	});
	FAULT("packed UV overflows 14 bits", VE_UVFIELD, {
		qw_t *p = alloc(1);

		p->d[0] = HWGIF_TAG(1, 1, 1, PRIM_SPRITE | PRIM_TME | (1u << 8), 0, 2);
		p->d[1] = 3ull | (5ull << 4);
		p = alloc(2);
		p[0].d[0] = (u64)20000 | ((u64)5 << 32);
		p[1].d[0] = 0x8000 | (0x8000ull << 32);
		p[1].d[1] = 100;
	});
	FAULT("RGBA component above 255", VE_RGBA, {
		qw_t *p = alloc(1);

		p->d[0] = HWGIF_TAG(1, 1, 1, PRIM_POINT, 0, 1);
		p->d[1] = 1ull;
		p = alloc(1);
		p->d[0] = 300;
	});
	// REF with an address that is not 16-byte aligned: the chain is CNT (state), REF, END
	begin();
	state();
	buf[run].d[0] = (u64)(n - run - 1) | (1ull << 28);
	{
		qw_t *t = alloc(1);

		t->d[0] = 4ull | (3ull << 28) | ((u64)0x100008 << 32);
		run = n;
		alloc(1);
	}
	val_reset();
	buf[run].d[0] = 7ull << 28;
	val_buffer(buf, n);
	bad += check("REF address unaligned", VE_DMATAG, 1);
	(void)fl;
	printf("%s\n", bad ? "SOME TESTS FAILED" : "all validator tests passed");
	return bad ? 1 : 0;
}
