// PS2 video modes: the internal render sizes and the GS output formats (docs/VIDEO_MODES.md).
// Pure tables and integer arithmetic only - no engine or PS2 headers - so that the host test builds it too.

#ifndef __PS2_VMODES_H__
#define __PS2_VMODES_H__

// ---- internal render sizes (vid.width x vid.height); index = vid.modenum, never reorder ----
#define PS2VM_NUMMODES 11
#define PS2VM_MAXW 640 // MAXVIDWIDTH / MAXVIDHEIGHT of the PS2 build (screen.h)
#define PS2VM_MAXH 512
#define PS2VM_NUMSCREENS 5 // NUMSCREENS of screen.h

static inline int ps2vm_mode_size(int mode, int *w, int *h)
{
	static const short sizes[PS2VM_NUMMODES][2] = {
		{320, 200}, {320, 224}, {320, 240}, {320, 256}, {400, 300}, {512, 384},
		{512, 448}, {640, 400}, {640, 448}, {640, 480}, {640, 512},
	};
	if (mode < 0 || mode >= PS2VM_NUMMODES)
		return 0;
	*w = sizes[mode][0];
	*h = sizes[mode][1];
	return 1;
}

// exact match only; -1 = no such mode
static inline int ps2vm_mode_for_size(int w, int h)
{
	int i, mw, mh;
	for (i = 0; i < PS2VM_NUMMODES; i++)
		if (ps2vm_mode_size(i, &mw, &mh) && mw == w && mh == h)
			return i;
	return -1;
}

// ---- GS output formats; index = outid, never reorder (0..2 are the old PS2GS_MODE_NTSC/PAL/480P) ----
#define PS2VM_NUMOUT 26
#define PS2VM_AUTO (-1) // NTSC or PAL by the console region

enum
{
	PS2VM_NTSC = 0, PS2VM_PAL = 1, PS2VM_480P = 2, PS2VM_NTSC_FRAME = 3, PS2VM_PAL_FRAME = 4, PS2VM_NTSC_240P = 5,
	PS2VM_PAL_288P = 6, PS2VM_480P_WIDE = 7, PS2VM_576P = 8, PS2VM_720P = 9, PS2VM_1080I = 10, PS2VM_VESA_FIRST = 11,
};

#define PS2VM_NEEDS_BIOS220 1 // SetGsCrt knows the mode only from BIOS 2.20 (romver number >= 220)
#define PS2VM_VGA 2 // VESA output: needs a VGA connection; nothing in the emulator shows it
#define PS2VM_SD 4 // 480i/576i/240p/288p/480p/576p: the OSD "TV screen type" 16:9 makes the picture wide

typedef struct
{
	const char *name; // menu / config text
	const char *arg; // command line word after '-' (and for -output)
	unsigned char gsmode; // SetGsCrt mode
	unsigned char interlace; // 1 = interlaced
	unsigned char ffmd; // 1 = frame, 0 = field (meaningful when interlaced)
	unsigned char flags; // PS2VM_*
	short fbw, fbh; // frame buffer in GS pixels (CT32, double buffered)
	short dw, dh; // gsKit display window of the mode: VCK units, rasters (a whole frame)
	short sx, sy; // gsKit DISPLAY origin of the mode
	unsigned char dar_w, dar_h; // display aspect of the whole frame buffer
} ps2vm_output_t;

static inline const ps2vm_output_t *ps2vm_output(int id)
{
#define VGA(n, a, m, w, h, dw_, sx_, sy_, dh_, fw, fh, dw2, dh2) \
	{n, a, m, 0, 1, PS2VM_VGA, fw, fh, dw_, dh_, sx_, sy_, dw2, dh2}
	static const ps2vm_output_t t[PS2VM_NUMOUT] = {
		{"NTSC 480i", "ntsc", 0x02, 1, 0, PS2VM_SD, 640, 448, 2880, 480, 492, 34, 4, 3},
		{"PAL 576i", "pal", 0x03, 1, 0, PS2VM_SD, 640, 512, 2880, 576, 520, 40, 4, 3},
		{"480p", "480p", 0x50, 0, 1, PS2VM_SD, 640, 480, 1440, 480, 232, 35, 4, 3},
		{"NTSC 480i Frame", "ntscframe", 0x02, 1, 1, PS2VM_SD, 640, 224, 2880, 480, 492, 34, 4, 3},
		{"PAL 576i Frame", "palframe", 0x03, 1, 1, PS2VM_SD, 640, 256, 2880, 576, 520, 40, 4, 3},
		{"NTSC 240p", "240p", 0x02, 0, 1, PS2VM_SD, 640, 224, 2880, 480, 492, 34, 4, 3},
		{"PAL 288p", "288p", 0x03, 0, 1, PS2VM_SD, 640, 256, 2880, 576, 520, 40, 4, 3},
		{"480p Wide", "480pw", 0x50, 0, 1, PS2VM_SD, 704, 480, 1440, 480, 232, 35, 4, 3},
		{"576p", "576p", 0x53, 0, 1, PS2VM_SD | PS2VM_NEEDS_BIOS220, 704, 576, 1440, 576, 255, 44, 4, 3},
		{"720p", "720p", 0x52, 0, 1, 0, 640, 360, 1280, 720, 306, 24, 16, 9},
		{"1080i", "1080i", 0x51, 1, 1, 0, 640, 540, 1920, 1080, 236, 38, 16, 9},
		// VESA: the frame buffer is the screen divided by an integer so that two CT32 buffers fit into the 4 MB
		VGA("VESA 640x480 60", "vesa640x480@60", 0x1A, 640, 480, 1280, 280, 18, 480, 640, 480, 4, 3),
		VGA("VESA 640x480 72", "vesa640x480@72", 0x1B, 640, 480, 1280, 330, 18, 480, 640, 480, 4, 3),
		VGA("VESA 640x480 75", "vesa640x480@75", 0x1C, 640, 480, 1280, 360, 18, 480, 640, 480, 4, 3),
		VGA("VESA 640x480 85", "vesa640x480@85", 0x1D, 640, 480, 1280, 260, 18, 480, 640, 480, 4, 3),
		VGA("VESA 800x600 56", "vesa800x600@56", 0x2A, 800, 600, 1600, 450, 25, 600, 400, 300, 4, 3),
		VGA("VESA 800x600 60", "vesa800x600@60", 0x2B, 800, 600, 1600, 465, 25, 600, 400, 300, 4, 3),
		VGA("VESA 800x600 72", "vesa800x600@72", 0x2C, 800, 600, 1600, 465, 25, 600, 400, 300, 4, 3),
		VGA("VESA 800x600 75", "vesa800x600@75", 0x2D, 800, 600, 1600, 510, 25, 600, 400, 300, 4, 3),
		VGA("VESA 800x600 85", "vesa800x600@85", 0x2E, 800, 600, 1600, 500, 25, 600, 400, 300, 4, 3),
		VGA("VESA 1024x768 60", "vesa1024x768@60", 0x3B, 1024, 768, 2048, 580, 30, 768, 512, 384, 4, 3),
		VGA("VESA 1024x768 70", "vesa1024x768@70", 0x3C, 1024, 768, 1024, 266, 30, 768, 512, 384, 4, 3),
		VGA("VESA 1024x768 75", "vesa1024x768@75", 0x3D, 1024, 768, 1024, 260, 30, 768, 512, 384, 4, 3),
		VGA("VESA 1024x768 85", "vesa1024x768@85", 0x3E, 1024, 768, 1024, 290, 30, 768, 512, 384, 4, 3),
		VGA("VESA 1280x1024 60", "vesa1280x1024@60", 0x4A, 1280, 1024, 1280, 350, 40, 1024, 640, 512, 5, 4),
		VGA("VESA 1280x1024 75", "vesa1280x1024@75", 0x4B, 1280, 1024, 1280, 350, 40, 1024, 640, 512, 5, 4),
	};
#undef VGA
	return (id >= 0 && id < PS2VM_NUMOUT) ? &t[id] : 0;
}

// ---- VRAM bookkeeping (pages of 8 KB; the GS has 512) ----
#define PS2VM_VRAM_PAGES 512

static inline int ps2vm_fb_stride(int fbw)
{
	return (fbw + 63) & ~63;
}

static inline int ps2vm_fb_pages(int fbw, int fbh)
{
	return (ps2vm_fb_stride(fbw) / 64) * ((fbh + 31) / 32); // CT32 page: 64 x 32 px
}

static inline int ps2vm_tex_tbw(int w)
{
	return 2 * ((w + 127) / 128); // PSMT8 page: 128 x 64 px; TBW counts 64 px
}

static inline int ps2vm_tex_pages(int w, int h)
{
	return ((w + 127) / 128) * ((h + 63) / 64);
}

// 2 frame buffers + the source texture + 1 page of CLUT
static inline int ps2vm_vram_pages(const ps2vm_output_t *o, int w, int h)
{
	return 2 * ps2vm_fb_pages(o->fbw, o->fbh) + ps2vm_tex_pages(w, h) + 1;
}

static inline int ps2vm_log2ceil(int n)
{
	int l = 0;
	while ((1 << l) < n)
		l++;
	return l;
}

// ---- CRTC (DISPLAY) values; the same arithmetic as gsKit_set_buffer_attributes of gsKit 1.5.1 ----
typedef struct
{
	int dx, dy; // DISPLAY.DX / DY
	int magh, magv; // DISPLAY.MAGH / MAGV (the value written: factor - 1)
	int dw, dh; // DISPLAY.DW / DH (the value written: size - 1 is done by the caller)
} ps2vm_crtc_t;

// ddx/ddy: the board specific offsets of _GetGsDxDyOffset (only used for the modes other than NTSC/PAL, 0 otherwise)
static inline void ps2vm_crtc(const ps2vm_output_t *o, int ddx, int ddy, ps2vm_crtc_t *c)
{
	int sx = o->sx, sy = o->sy, dw = o->dw, dh = o->dh, magh, magv;

	if ((o->gsmode == 0x02 || o->gsmode == 0x03) && !o->interlace)
	{
		sy /= 2; // 240p / 288p instead of 480i / 576i
		dh /= 2;
	}
	magh = dw / o->fbw - 1;
	magv = dh / o->fbh - 1;
	if (o->gsmode != 0x02 && o->gsmode != 0x03)
	{
		sx += ddx;
		sy += ddy;
	}
	sx += (dw - (magh + 1) * o->fbw) / 2;
	sy += (dh - (magv + 1) * o->fbh) / 2;
	if (o->interlace)
		sy &= ~1;
	c->dw = (magh + 1) * o->fbw;
	c->dh = (magv + 1) * o->fbh;
	if (o->interlace && o->ffmd)
		magv--;
	c->dx = sx;
	c->dy = sy;
	c->magh = magh;
	c->magv = magv;
}

// ---- where the internal frame goes inside the frame buffer ----
enum
{
	PS2VM_FIT_43 = 0, // the frame as a 4:3 picture on the display: whole buffer on a 4:3 screen, centred column on 16:9
	PS2VM_FIT_STRETCH = 1, // the whole buffer
	PS2VM_FIT_SQUARE = 2, // square pixels: the aspect of the internal frame itself
	PS2VM_FIT_INTEGER = 3, // largest whole multiple of the internal frame
	PS2VM_FIT_COUNT
};

// A picture of aspect pw:ph, shown on a frame buffer of size fbw x fbh whose whole area has the display aspect dw_:dh_,
// centred; sizes rounded down to even.
static inline void ps2vm_fit_aspect(int pw, int ph, int dw_, int dh_, int fbw, int fbh, int *x, int *y, int *w, int *h)
{
	// width fraction of the buffer: (pw/ph) / (dw_/dh_); the whole buffer when equal
	long long num = (long long)pw * dh_, den = (long long)ph * dw_; // picture aspect / display aspect = num / den
	if (num == den)
	{
		*x = 0; *y = 0; *w = fbw; *h = fbh;
	}
	else if (num < den) // picture narrower than the display: bars left and right
	{
		*w = (int)(((long long)fbw * num / den) & ~1LL);
		*h = fbh;
		*x = (fbw - *w) / 2;
		*y = 0;
	}
	else // wider: bars above and below
	{
		*w = fbw;
		*h = (int)(((long long)fbh * den / num) & ~1LL);
		*x = 0;
		*y = (fbh - *h) / 2;
	}
}

static inline void ps2vm_dest(int fit, int iw, int ih, int fbw, int fbh, int dar_w, int dar_h, int *x, int *y, int *w, int *h)
{
	switch (fit)
	{
		case PS2VM_FIT_STRETCH:
			*x = 0; *y = 0; *w = fbw; *h = fbh;
			return;
		case PS2VM_FIT_SQUARE:
			ps2vm_fit_aspect(iw, ih, dar_w, dar_h, fbw, fbh, x, y, w, h);
			return;
		case PS2VM_FIT_INTEGER:
		{
			int s = fbw / iw < fbh / ih ? fbw / iw : fbh / ih;
			if (s >= 1)
			{
				*w = iw * s; *h = ih * s;
				*x = (fbw - *w) / 2; *y = (fbh - *h) / 2;
				return;
			}
			break; // does not fit whole: fall back to the 4:3 picture
		}
		default:
			break;
	}
	ps2vm_fit_aspect(4, 3, dar_w, dar_h, fbw, fbh, x, y, w, h);
}

#endif
