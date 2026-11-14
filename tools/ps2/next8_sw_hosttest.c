/* Complete actual reference/current drawers live in a separate external-global
 * TU. This file supplies inputs, compares bytes and measures executed calls. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#ifndef NEXT8_SW_EE
#include <time.h>
#else
#include <kernel.h>
#endif
#include "next8_sw_stub.h"
#define FB_GUARD 64
#define FB_HEIGHT 9
#define FB_MAX (65536 + (640 + 17) * FB_HEIGHT + 2 * FB_GUARD)
#ifdef NEXT8_SW_EE
#define SOURCE_PIXELS (128 * 128)
#else
#define SOURCE_PIXELS (2048 * 2048)
#endif
next8_vid_t vid;
INT32 ds_y, ds_x1, ds_x2, ds_bgofs, ds_waterofs;
UINT8 *topleft, *screens[5];
UINT8 *ds_source, *ds_colormap, *ds_transmap, *ds_translation;
fixed_t ds_xfrac, ds_yfrac, ds_xstep, ds_ystep;
UINT16 ds_flatwidth, ds_flatheight;

typedef struct { const char *name; void (*reference)(void), (*candidate)(void); int sprite, water; } drawer_t;
#define DRAWER(n,s,w) { #n, reference_##n, candidate_##n, s,w }
static const drawer_t drawers[] = {
	DRAWER(R_DrawTranslucentSpan_NPO2_8,0,0),
	DRAWER(R_DrawTranslucentSplat_NPO2_8,0,0),
	DRAWER(R_DrawTranslucentFloorSprite_NPO2_8,1,0),
	DRAWER(R_DrawWaterSpan_NPO2_8,0,1),
};
#define NDRAWERS (sizeof drawers / sizeof drawers[0])
static UINT8 frame_reference[FB_MAX] __attribute__((aligned(16)));
static UINT8 frame_candidate[FB_MAX] __attribute__((aligned(16)));
static UINT8 background[FB_MAX];
static UINT8 source8[2][SOURCE_PIXELS];
static UINT16 source16[2][SOURCE_PIXELS];
static UINT8 cmaps[2][256], transmaps[2][65536], translations[2][256];
static UINT64 cases, compared_bytes, hash = UINT64_C(1469598103934665603);

static void initialize(void)
{
	size_t i, set;
	for (set = 0; set < 2; set++)
	{
		for (i = 0; i < sizeof cmaps[set]; i++) cmaps[set][i] = (UINT8)((i * 29 + set * 37) ^ (i >> 3));
		for (i = 0; i < sizeof transmaps[set]; i++) transmaps[set][i] = (UINT8)((i >> 8) * 23 + (i & 255) * 71 + set * 13);
		for (i = 0; i < sizeof translations[set]; i++) translations[set][i] = (UINT8)(i * 47 + set * 41);
		for (i = 0; i < SOURCE_PIXELS; i++)
		{
			source8[set][i] = i % 7 == 1 ? TRANSPARENTPIXEL : (UINT8)(i * 37 + i / 11 + set * 41);
			source16[set][i] = (UINT16)((i % 5 == 1 ? 0 : 0xFF00) | ((i * 41 + i / 13 + set * 17) & 255));
		}
	}
	for (i = 0; i < sizeof background; i++) background[i] = (UINT8)(i * 53 + (i >> 7));
}

/* Modes cover canonical and out-of-range starts, both signs, fractional and
 * repeated wraps, the 32-pixel gate, and steps at/outside its strict limits. */
static void parameters(int fw, int first, int length, unsigned mode, unsigned tw, unsigned th, int edge)
{
	fixed_t xwidth = (fixed_t)(tw << FRACBITS), yheight = (fixed_t)(th << FRACBITS);
	const fixed_t startsx[] = {0,1,FRACUNIT-1,xwidth-1,xwidth,xwidth+1,-1,-xwidth,-xwidth-1,INT32_MAX,INT32_MIN};
	const fixed_t startsy[] = {0,1,FRACUNIT-1,yheight-1,yheight,yheight+1,-1,-yheight,-yheight-1,INT32_MAX,INT32_MIN};
	const fixed_t stepsx[] = {0,1,-1,FRACUNIT,-FRACUNIT,xwidth-1,1-xwidth,xwidth,-xwidth,xwidth+FRACUNIT,-xwidth-FRACUNIT};
	const fixed_t stepsy[] = {0,1,-1,FRACUNIT,-FRACUNIT,yheight-1,1-yheight,yheight,-yheight,yheight+FRACUNIT,-yheight-FRACUNIT};
	vid.width = fw; vid.rowbytes = fw + (int)(mode % 3) * 7; vid.height = FB_HEIGHT;
	ds_y = edge ? FB_HEIGHT - 1 : 2 + (INT32)(mode % 5);
	ds_x1 = first; ds_x2 = first + length - 1;
	ds_bgofs = edge ? -1 : (INT32)(mode % 3) - 1;
	ds_waterofs = (INT32)(mode % 3) * (FRACUNIT / 4);
	ds_flatwidth = (UINT16)tw; ds_flatheight = (UINT16)th;
	ds_xfrac = startsx[mode % (sizeof startsx / sizeof startsx[0])];
	ds_yfrac = startsy[(mode / 3 + 5) % (sizeof startsy / sizeof startsy[0])];
	ds_xstep = stepsx[(mode / 11) % (sizeof stepsx / sizeof stepsx[0])];
	ds_ystep = stepsy[(mode / 7 + 3) % (sizeof stepsy / sizeof stepsy[0])];
}

/* Seven aliases exercise live source, colormap, translation, blend and shifted
 * background contents. Framebuffer never aliases the storage of globals. */
static void bind_frame(UINT8 *frame, size_t d, unsigned mode, unsigned alias)
{
	unsigned set = mode & 1;
	screens[0] = topleft = frame + FB_GUARD;
	screens[1] = alias == 6 ? topleft + 1 : (alias == 7 ? topleft - 1 : (alias == 5 ? topleft : background + FB_GUARD));
	ds_source = alias == 1 || alias == 5 ? topleft : (drawers[d].sprite ? (UINT8 *)source16[set] : source8[set]);
	ds_colormap = alias == 2 || alias == 5 ? topleft : cmaps[set];
	ds_translation = alias == 3 || alias == 5 ? topleft : translations[set];
	ds_transmap = alias == 4 || alias == 5 ? frame : transmaps[set];
}

static int compare(size_t d, int fw, int first, int length, unsigned mode, unsigned tw, unsigned th, unsigned alias, int edge, int broken)
{
	size_t i;
	INT32 expected_x;
	for (i = 0; i < FB_MAX; i++) frame_reference[i] = (UINT8)(i * 31 + (i >> 4) + mode);
	memcpy(frame_candidate, frame_reference, FB_MAX);
	parameters(fw,first,length,mode,tw,th,edge);
	if (alias) { ds_y = edge ? FB_HEIGHT-1 : 0; if (!edge) ds_bgofs = 0; }
	bind_frame(frame_reference,d,mode,alias);
	drawers[d].reference(); expected_x = ds_x1;
	parameters(fw,first,length,mode,tw,th,edge);
	if (alias) { ds_y = edge ? FB_HEIGHT-1 : 0; if (!edge) ds_bgofs = 0; }
	bind_frame(frame_candidate,d,mode,alias);
	drawers[d].candidate();
	if (broken && length > 0) topleft[ds_y * fw + first] ^= 1;
	if (memcmp(frame_reference,frame_candidate,FB_MAX) || ds_x1 != expected_x)
	{
		for (i=0;i<FB_MAX;i++) if(frame_reference[i]!=frame_candidate[i]) { printf("DIFF offset=%lu before=%u after=%u\n",(unsigned long)i,frame_reference[i],frame_candidate[i]);break; }
		printf("AFFINE FAIL drawer=%s frame=%d x=%d n=%d mode=%u flat=%ux%u alias=%u edge=%d\n",drawers[d].name,fw,first,length,mode,tw,th,alias,edge);
		return 1;
	}
	cases++; compared_bytes += FB_MAX;
	for (i = 0; i < 32; i++) hash = (hash ^ frame_candidate[(FB_MAX / 33) * i]) * UINT64_C(1099511628211);
	return 0;
}

/* With constant texel zero, entry64 changes 65->64. The next pixel must read
 * the new byte. Copying the blend table before the loop fails immediately. */
static int adversarial_blend(size_t d)
{
	unsigned v;
	for (v = 0; v < 2; v++)
	{
		UINT8 *frame = v ? frame_candidate : frame_reference;
		memset(frame,0,FB_MAX);
		parameters(320,0,100,0,63,45,0);
		ds_y = 0; ds_xfrac = ds_yfrac = ds_xstep = ds_ystep = ds_waterofs = ds_bgofs = 0;
		bind_frame(frame,d,0,4);
		screens[1] = frame + FB_GUARD;
		memset(topleft,64,320); topleft[0] = 65;
		(v ? drawers[d].candidate : drawers[d].reference)();
	}
	if (memcmp(frame_reference,frame_candidate,FB_MAX))
	{
		printf("AFFINE FAIL adversarial-live-blend drawer=%s\n",drawers[d].name);
		return 1;
	}
	cases++; compared_bytes += FB_MAX;
	return 0;
}

#ifndef NEXT8_SW_EE
static int equality(int broken, int aliases_only)
{
	static const int widths[] = {1,7,16,31,319,320,512,640};
	static const int lengths[] = {-17,-1,0,1,2,3,7,8,15,16,17,31,32,33,63,127,319,320,321,512,639,640};
	static const unsigned dimensions[][2] = {{1,1},{3,7},{13,19},{31,7},{63,45},{127,93},{255,129},{2047,2047}};
	size_t f,n,t,d;
	unsigned mode,alias;
	if (!aliases_only)
	for (f=0;f<sizeof widths/sizeof widths[0];f++)
	for (n=0;n<sizeof lengths/sizeof lengths[0];n++)
	for (t=0;t<sizeof dimensions/sizeof dimensions[0];t++)
	for (mode=0;mode<121;mode++)
	for (d=0;d<NDRAWERS;d++)
	{
		int fw=widths[f], length=lengths[n], first;
		if (length>fw) continue;
		first = length<1 || mode%3==0 ? 0 : (mode%3==1 ? fw-length : (fw-length)/2);
		if (compare(d,fw,first,length,mode,dimensions[t][0],dimensions[t][1],0,0,broken)) return 1;
	}
	for (d=0;d<NDRAWERS;d++) if (adversarial_blend(d)) return 1;
	for (alias=1;alias<=7;alias++)
	for (mode=0;mode<121;mode++)
	for (n=0;n<sizeof lengths/sizeof lengths[0];n++)
	for (d=0;d<NDRAWERS;d++)
	{
		int length=lengths[n];
		if (length>320) continue;
		if (compare(d,320,length<=0?0:(INT32)(mode%3)*((320-length)/2),length,mode,63,45,alias,0,broken)) return 1;
	}
	/* Inclusive destination endpoint and reversed size_t counts, exactly as
	 * present in the original bounded/fallback loops. */
	for (d=0;d<NDRAWERS;d++)
	for (mode=0;mode<22;mode++)
	for (n=0;n<sizeof lengths/sizeof lengths[0];n++)
	for (alias=0;alias<=7;alias++)
		if (compare(d,320,318,lengths[n],mode,63,45,alias,1,broken)) return 1;
	printf("AFFINE PASS cases=%llu compared_bytes=%llu hash=%016llx aliases=7\n",(unsigned long long)cases,(unsigned long long)compared_bytes,(unsigned long long)hash);
	return 0;
}
#endif

static UINT64 now(void)
{
#ifdef NEXT8_SW_EE
	UINT32 value;
	__asm__ volatile("mfc0 %0,$9" : "=r"(value));
	return value;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC,&ts);
	return (UINT64)ts.tv_sec*1000000000+ts.tv_nsec;
#endif
}

static int benchmark(void)
{
	static const int lengths[] = {0,1,2,3,7,8,16,31,32,33,100,320};
	size_t d,n,i;
	unsigned mode;
	const unsigned calls=2048;
	for(d=0;d<NDRAWERS;d++) if(adversarial_blend(d)) return 1;
	for(d=0;d<NDRAWERS;d++)
	for(mode=0;mode<6;mode++)
	for(n=0;n<sizeof lengths/sizeof lengths[0];n++)
	{
		UINT64 best[2]={UINT64_MAX,UINT64_MAX};
		unsigned rep,order;
		/* Modes include positive/negative bounded steps and both larger-step
		 * fallback signs, with canonical starts and nontrivial texels. */
		unsigned input=mode==0?0:(mode==1?66:(mode==2?88:(mode==3?99:66)));
		initialize();
		if(mode==4) { memset(source8,255,sizeof source8); memset(source16,0,sizeof source16); }
		if(mode==5)
		{
			memset(source8,0,sizeof source8);
			for(i=0;i<sizeof source16/sizeof source16[0][0];i++) ((UINT16 *)source16)[i]=0xFF00;
		}
		if(compare(d,320,0,lengths[n],input,63,45,0,0,0)) return 1;
		for(rep=0;rep<5;rep++)
		for(order=0;order<2;order++)
		{
			unsigned variant=(order+rep)&1;
			void (*draw)(void)=variant?drawers[d].candidate:drawers[d].reference;
			UINT64 start,elapsed;
			parameters(320,0,lengths[n],input,63,45,0);
			bind_frame(frame_candidate,d,input,0);
			draw();
#ifdef NEXT8_SW_EE
			/* Exclude asynchronous SDK interrupt service from kernel cycles.
			 * DIntr returns the prior EIE state; restore it after the clock. */
			const int interrupts_enabled=DIntr();
#endif
			start=now();
			for(i=0;i<calls;i++) draw();
#ifdef NEXT8_SW_EE
			elapsed=(UINT32)((UINT32)now()-(UINT32)start);
			if(interrupts_enabled) EIntr();
#else
			elapsed=now()-start;
#endif
			if(elapsed<best[variant]) best[variant]=elapsed;
		}
		printf("AFFINEBENCH drawer=%s n=%d mode=%u calls=%u before=%llu after=%llu\n",drawers[d].name,lengths[n],mode,calls,(unsigned long long)best[0],(unsigned long long)best[1]);
		fflush(stdout);
	}
	puts("AFFINE DONE");return 0;
}

int main(int argc,char **argv)
{
	initialize();
#ifdef NEXT8_SW_EE
	(void)argc;(void)argv;return benchmark();
#else
	if(argc>1&&!strcmp(argv[1],"bench")) return benchmark();
	return equality(argc>1&&!strcmp(argv[1],"broken-pixel"),argc>1&&!strcmp(argv[1],"aliases"));
#endif
}
