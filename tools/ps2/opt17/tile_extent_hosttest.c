#include "common.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned checks,failures,cases64,cases128,regression64_failures,plans_fast,plans_fallback,loader_checks;
static unsigned long long compared_bytes;
static u32 random_state=0x170813;
static u32 rnd(void){random_state^=random_state<<13;random_state^=random_state>>17;return random_state^=random_state<<5;}
static int coord(int x,int n,int wrap){if(wrap){x%=n;return x<0?x+n:x;}return x<0?0:x>=n?n-1:x;}
static void fail(const input_t*i,const char*why){if(failures<8)printf("DYN FAIL %s fmt=%d psm=%d screen=%d region=%d width=%d xy=%d,%d\n",why,i->m.format,i->r.psm,i->r.screen,i->region,i->r.w,i->x,i->y);failures++;}
static void plan(const input_t*i,int side){
 u32 sx[128],p[16],covered[128]={0},n=candidate_plan(i,sx,p);int bad=n>4;
 for(int x=0;x<side;x++){
  int dx=x-1;if(i->region){dx=(x-(i->x&(side-1))+2*side+side/4)&(side-1);dx-=side/4;}
  if(sx[x]!=(u32)coord(i->x+dx,i->r.w,i->wrapx))bad=1;
 }
 int eligible=!i->r.screen&&i->r.psm==PSM_T8&&(i->m.format==GL_TEXFMT_P_8||i->m.format==GL_TEXFMT_ALPHA_8);
 if(!eligible&&n)bad=1;
 if(eligible&&i->r.w==1&&n!=1)bad=1;
 if(eligible&&i->r.w==2&&i->wrapx&&n)bad=1;
 if(n){plans_fast++;for(u32 k=0;k<n;k++){
  u32 source=p[k*4],dst=p[k*4+1],count=p[k*4+2],same=p[k*4+3];
  if(!count||dst+count>(u32)side||same>1){bad=1;continue;}
  for(u32 x=0;x<count;x++){u32 value=source+(same?0:x);if(value>=(u32)i->r.w||sx[dst+x]!=value||covered[dst+x]++)bad=1;}
 }for(int x=0;x<side;x++)if(covered[x]!=1)bad=1;}else if(eligible)plans_fallback++;
 if(bad)fail(i,"plan");
}
static void check(input_t*i,u8*a,u8*b,u8*old){
 int side=i->region&&i->r.psm==PSM_T8&&!i->r.screen?128:64;
 memset(a,0xA5,65536);memset(b,0xA5,65536);memset(old,0xA5,65536);
 reference_fill(i,a);candidate_fill(i,b);checks++;compared_bytes+=(unsigned long long)side*side*4;
 if(candidate_extent(i)!=side)fail(i,"extent");
 if(memcmp(a,b,(size_t)side*side*4))fail(i,"scalar bytes");
 if(side==64){cases64++;frozen_fill(i,old);if(memcmp(old,b,16384)){regression64_failures++;fail(i,"frozen64 bytes");}}
 else cases128++;
 plan(i,side);
}
int main(void){
 static const int widths[]={1,2,31,32,63,64,65,127,128,129,1024};
 u8*raw=malloc(1031u*137u*4u),*a=malloc(65536),*b=malloc(65536),*old=malloc(65536);
 u32 history[256];input_t i;memset(&i,0,sizeof i);
 if(!raw||!a||!b||!old)return 2;
 for(size_t x=0;x<1031u*137u*4u;x++)raw[x]=(u8)rnd();
 for(int x=0;x<256;x++){history[x]=(u32)x|((u32)(255-x)<<8)|((u32)(x^117)<<16);i.pal[x]=history[x]^0x00123400u;}
 for(int mode=0;mode<18;mode++)for(size_t w=0;w<sizeof widths/sizeof *widths;w++){
  i.m.format=mode==0?GL_TEXFMT_P_8:mode==1?GL_TEXFMT_ALPHA_8:mode<8?GL_TEXFMT_AP_88:mode<12?GL_TEXFMT_RGBA:GL_TEXFMT_ALPHA_INTENSITY_88;
  i.m.flags=(mode&1)?TF_CHROMAKEYED:0;i.m.packed=mode==6||mode==7;i.m.regen_kind=0;i.m.ps2_cpu=i.m.packed?1:0;
  i.r.w=i.m.width=widths[w];i.r.h=i.m.height=131;i.fbw=i.r.w+5;
  i.r.psm=(mode&1)?PSM_CT32:PSM_T8;i.r.screen=mode>=14;i.fb32=mode&1;
  if(i.r.screen)i.r.psm=i.fb32?PSM_CT32:PSM_CT16S;
  // ALPHA_8's indexed path must be covered at both extents.
  if(mode==1)i.r.psm=PSM_T8;
  i.r.tile_palette=history;i.r.tile_source=NULL;
  i.m.ps2_uw=(u16)(i.r.w>3?i.r.w-3:i.r.w);i.m.ps2_uh=126;
  size_t pixels=(size_t)i.m.ps2_uw*i.m.ps2_uh,packed_bytes=pixels+(pixels+7)/8;
  u8*packed=i.m.packed?malloc(packed_bytes):NULL;
  if(i.m.packed&&!packed)return 2;
  if(packed)for(size_t p=0;p<packed_bytes;p++)packed[p]=(u8)rnd();
  if(i.m.format==GL_TEXFMT_RGBA&&i.r.psm==PSM_T8)
   for(size_t p=0;p<1031u*137u;p++)((u32*)(void*)raw)[p]=history[(p*17u+p/131u)&255u]|(p%7u?0xff000000u:0);
  i.data=i.m.data=packed?packed:raw;i.r.owner=&i.m;
  for(int region=0;region<2;region++)for(int wrap=0;wrap<4;wrap++)for(int origin=0;origin<8;origin++){
   i.region=region;i.wrapx=wrap&1;i.wrapy=wrap&2;
   i.x=origin<3?origin*32-17:origin<6?i.r.w+origin*17:origin==6?-1000000:1000000;
   i.y=origin<3?origin*33-17:origin<6?i.r.h+origin*17:origin==6?-1000000:1000000;
   check(&i,a,b,old);
  }
  i.x=0;i.y=0;i.wrapx=i.wrapy=1;
  if(!layout_probe(&i))fail(&i,"actual loader layout/cache/bytes");
  loader_checks++;
  free(packed);
 }
 printf("DYN COMPLETE checks=%u cases64=%u cases128=%u failures=%u regression64_failures=%u compared_bytes=%llu fast_plans=%u fallback_plans=%u loader_checks=%u real_packed_accessor=1\n",checks,cases64,cases128,failures,regression64_failures,compared_bytes,plans_fast,plans_fallback,loader_checks);
 free(old);free(b);free(a);free(raw);return failures?1:0;
}

/* Frozen minimal V7 converter reference; provenance is recorded by the runner. */
#if 0
/* TILE_EXTENT_FROZEN_V7_BEGIN */
static inline u32 a8_to_gs(u32 a)
{
	// For a byte a = 2*k or 2*k+1, the rounded 128/255 scale is k or k+1, respectively.
	return (a + 1u) >> 1;
}
static inline boolean HWR_PS2_IsPackedPatch(const GLMipmap_t *m)
{
	return (m->ps2_cpu & HWR_PS2_CPU_PATCH) && m->format == GL_TEXFMT_AP_88
		&& !m->regen_kind && m->ps2_uw && m->ps2_uh
		&& m->ps2_uw <= m->width && m->ps2_uh <= m->height;
}
static inline UINT16 HWR_PS2_ReadAP88(const GLMipmap_t *m, UINT32 x, UINT32 y)
{
	const UINT8 *data = m->data;
	size_t p;

	if (HWR_PS2_IsPackedPatch(m))
	{
		if (x >= m->ps2_uw || y >= m->ps2_uh)
			return 0x00ffu;
		p = (size_t)y * m->ps2_uw + x;
		return (UINT16)(data[p] | ((data[(size_t)m->ps2_uw * m->ps2_uh + (p >> 3)] & (1u << (p & 7u))) ? 0xff00u : 0u));
	}
	p = ((size_t)y * m->width + x) * 2u;
	return (UINT16)(data[p] | ((UINT16)data[p + 1u] << 8));
}
static int tile_coord(int p, int size, int wrap)
{
	if (wrap)
	{
		p %= size;
		return p < 0 ? p + size : p;
	}
	return p < 0 ? 0 : p >= size ? size - 1 : p;
}
typedef struct
{
	texrec_t source;
	const u8 *data;
	int x, y, wrapx, wrapy, region;
	int sx[TILE_SIZE];
	struct { u16 sx; u8 dst, count, same; } runs[4];
	u32 run_count;
} tile_fill_t;
static void tile_fill_columns(tile_fill_t *c)
{
	for (int x = 0; x < TILE_SIZE; x++)
	{
		int dx = x - 1;
		if (c->region)
		{
			dx = (x - (c->x & (TILE_SIZE - 1)) + TILE_SIZE + TILE_SIZE / 4) & (TILE_SIZE - 1);
			dx -= TILE_SIZE / 4;
		}
		c->sx[x] = tile_coord(c->x + dx, c->source.w, c->wrapx);
	}
	c->run_count = 0;
	if (c->source.screen || c->source.psm != PSM_T8 || !c->source.owner
		|| (c->source.owner->format != GL_TEXFMT_P_8 && c->source.owner->format != GL_TEXFMT_ALPHA_8))
		return;
	for (int x = 0; x < TILE_SIZE;)
	{
		const int start = x, sx = c->sx[x];
		const int same = x + 1 == TILE_SIZE || c->sx[x + 1] != sx + 1;
		const u32 r = c->run_count;

		if (r == 4) { c->run_count = 0; return; }
		x++;
		while (x < TILE_SIZE && (same ? c->sx[x] == sx : c->sx[x] == sx + x - start)) x++;
		c->runs[r].sx = (u16)sx;
		c->runs[r].dst = (u8)start;
		c->runs[r].count = (u8)(x - start);
		c->runs[r].same = (u8)same;
		c->run_count++;
	}
}
static void tile_fill(void *ctx, u32 row, u8 *dst)
{
	const tile_fill_t *c = ctx;
	const texrec_t *r = &c->source;
	const GLMipmap_t *m = r->owner;
	int dy = (int)row - 1;
	if (c->region)
	{
		dy = ((int)row - (c->y & (TILE_SIZE - 1)) + TILE_SIZE + TILE_SIZE / 4) & (TILE_SIZE - 1);
		dy -= TILE_SIZE / 4;
	}
	const int sy = tile_coord(c->y + dy, r->h, c->wrapy);
	int x;

	if (c->run_count)
	{
		const u8 *src = c->data + (size_t)sy * m->width;
		for (u32 i = 0; i < c->run_count; i++)
			if (c->runs[i].same)
				memset(dst + c->runs[i].dst, src[c->runs[i].sx], c->runs[i].count);
			else
				memcpy(dst + c->runs[i].dst, src + c->runs[i].sx, c->runs[i].count);
		return;
	}
	for (x = 0; x < TILE_SIZE; x++)
	{
		const int sx = c->sx[x];
		u32 value = 0;
		if (r->screen)
		{
			const size_t at = (size_t)sy * H.fbw + sx;
			if (H.fb32)
				((u32 *)(void *)dst)[x] = ((const u32 *)(const void *)c->data)[at];
			else
				((u16 *)(void *)dst)[x] = ((const u16 *)(const void *)c->data)[at];
			continue;
		}
		switch (m->format)
		{
			case GL_TEXFMT_P_8:
			case GL_TEXFMT_ALPHA_8:
				value = c->data[(size_t)sy * m->width + sx];
				break;
			case GL_TEXFMT_AP_88:
			{
				u16 pixel;
#ifdef HWR_PS2_PACKED_PATCHES
				if (HWR_PS2_IsPackedPatch(m))
					pixel = HWR_PS2_ReadAP88(m, (u32)sx, (u32)sy);
				else
#endif
					pixel = ((const u16 *)(const void *)c->data)[(size_t)sy * m->width + sx];
				if (r->psm == PSM_T8)
					value = (!(m->flags & TF_CHROMAKEYED) && !(pixel >> 8)) ? 255u : pixel & 255u;
				else
					value = H.pal[pixel & 255u] | (a8_to_gs(pixel >> 8) << 24);
				break;
			}
			case GL_TEXFMT_RGBA:
			{
				const u32 pixel = ((const u32 *)(const void *)c->data)[(size_t)sy * m->width + sx];
				if (r->psm == PSM_T8)
				{
					int index;
					value = 255;
					if (pixel >> 24)
						for (index = 0; index < 256; index++)
							if (r->tile_palette[index] == (pixel & 0xFFFFFFu))
							{
								value = (u32)index;
								break;
							}
				}
				else
					value = (pixel & 0xFFFFFFu) | (a8_to_gs(pixel >> 24) << 24);
				break;
			}
			case GL_TEXFMT_ALPHA_INTENSITY_88:
			{
				const u8 *p = c->data + ((size_t)sy * m->width + sx) * 2u;
				value = (u32)p[0] * 0x010101u | (a8_to_gs(p[1]) << 24);
				break;
			}
			default:
				break;
		}
		if (r->psm == PSM_T8)
			dst[x] = (u8)value;
		else
			((u32 *)(void *)dst)[x] = value;
	}
}
/* TILE_EXTENT_FROZEN_V7_END */
#endif
