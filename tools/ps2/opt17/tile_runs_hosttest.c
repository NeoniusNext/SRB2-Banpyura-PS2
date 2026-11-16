#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static unsigned long checks, failures, plan_checks, fast_plans, fallback_plans, alpha_fast, alpha_fallback;
static volatile u32 sink;
static u32 rand_state = 0x17002281u;
static u32 rnd(void) { rand_state ^= rand_state << 13; rand_state ^= rand_state >> 17; return rand_state ^= rand_state << 5; }
static double seconds(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec * 1e-9; }

static int coordinate(int p,int n,int wrap){if(wrap){p%=n;return p<0?p+n:p;}return p<0?0:p>=n?n-1:p;}
static void plan_check(const input_t*i,int which){
 u32 columns[64],plan[16],coverage[64]={0};u32 n=fast_plan(i,columns,plan);int bad=n>4;
 const int eligible=!i->r.screen&&i->r.psm==PSM_T8&&(i->m.format==GL_TEXFMT_P_8||i->m.format==GL_TEXFMT_ALPHA_8);
 for(int x=0;x<64;x++){
  int dx=x-1;if(i->region){dx=(x-(i->x&63)+64+16)&63;dx-=16;}
  int expected=coordinate(i->x+dx,i->r.w,i->wrapx);
  if(columns[x]!=(u32)expected)bad=1;
 }
 if(!eligible&&n)bad=1;
 if(eligible&&i->r.w==1&&n!=1)bad=1;
 if(eligible&&i->r.w==2&&i->wrapx&&n!=0)bad=1;
 if(n){
  fast_plans++;if(i->m.format==GL_TEXFMT_ALPHA_8)alpha_fast++;
  for(u32 r=0;r<n;r++){
   u32 sx=plan[r*4],dst=plan[r*4+1],count=plan[r*4+2],same=plan[r*4+3];
   if(!count||dst+count>64||same>1){bad=1;continue;}
   for(u32 x=0;x<count;x++){u32 at=dst+x,value=same?sx:sx+x;if(value>=(u32)i->r.w||columns[at]!=value||coverage[at]++)bad=1;}
  }
  for(int x=0;x<64;x++)if(coverage[x]!=1)bad=1;
 }else if(eligible){fallback_plans++;if(i->m.format==GL_TEXFMT_ALPHA_8)alpha_fallback++;}
 plan_checks++;
 if(bad){if(failures<8)printf("TC FAIL run_plan case=%d fmt=%d width=%d runs=%u wrap=%d region=%d\n",which,i->m.format,i->r.w,n,i->wrapx,i->region);failures++;}
}
static void check(const input_t *i, u8 *a, u8 *b, int which)
{
	memset(a, 0xA5, 64u * 64u * 4u); memset(b, 0xA5, 64u * 64u * 4u);
	base_fill(i, a); fast_fill(i, b); checks++; plan_check(i,which);
	if (memcmp(a, b, 64u * 64u * 4u))
	{
		if (failures < 8) printf("TC FAIL case=%d fmt=%d psm=%d size=%dx%d xy=%d,%d wrap=%d,%d region=%d screen=%d\n", which, i->m.format, i->r.psm, i->r.w, i->r.h, i->x, i->y, i->wrapx, i->wrapy, i->region, i->r.screen);
		failures++;
	}
}

int main(void)
{
	static const int widths[] = {1, 2, 31, 32, 63, 64, 65, 127, 129, 1024};
	u8 *data = malloc(1031u * 137u * 4u), *a = malloc(64u * 64u * 4u), *b = malloc(64u * 64u * 4u);
	u32 palette[256]; input_t i; memset(&i, 0, sizeof i);
	if (!data || !a || !b) return 2;
	for (size_t p = 0; p < 1031u * 137u * 4u; p++) data[p] = (u8)rnd();
	for (int p = 0; p < 256; p++) palette[p] = i.pal[p] = (u32)p | ((u32)(255-p)<<8) | ((u32)(p^117)<<16);
	i.data = data; i.m.data = data; i.r.owner = &i.m; i.r.tile_palette = palette;
	int which = 0;
	for (int format = 0; format < 16; format++) for (size_t w = 0; w < sizeof widths / sizeof *widths; w++)
	{
		i.m.format = format < 2 ? (format == 0 ? GL_TEXFMT_P_8 : GL_TEXFMT_ALPHA_8) : format < 8 ? GL_TEXFMT_AP_88 : format < 12 ? GL_TEXFMT_RGBA : GL_TEXFMT_ALPHA_INTENSITY_88;
		i.m.flags = (format & 1) ? TF_CHROMAKEYED : 0; i.m.packed = format == 6 || format == 7;
		i.r.w = i.m.width = widths[w]; i.r.h = i.m.height = 131; i.fbw = i.r.w + 5;
		i.r.psm = (format & 1) ? PSM_CT32 : PSM_T8; i.r.screen = format >= 14; i.fb32 = format & 1;
		/* Indexed RGBA must exercise actual palette hits and transparent holes. */
		if (i.m.format == GL_TEXFMT_RGBA && i.r.psm == PSM_T8)
			for (size_t p = 0; p < 1031u * 137u; p++)
				((u32 *)(void *)data)[p] = palette[(p * 17u + p / 131u) & 255u] | (p % 7u ? 0xFF000000u : 0u);
		for (int region = 0; region < 2; region++) for (int wrap = 0; wrap < 4; wrap++) for (int origin = 0; origin < 8; origin++)
		{
			i.region = region; i.wrapx = wrap & 1; i.wrapy = wrap & 2;
			i.x = origin < 3 ? origin * 32 - 17 : origin < 6 ? i.r.w + origin * 17 : origin == 6 ? -1000000 : 1000000;
			i.y = origin < 3 ? origin * 33 - 17 : origin < 6 ? i.r.h + origin * 17 : origin == 6 ? -1000000 : 1000000;
			check(&i, a, b, which++);
		}
	}

 printf("TC BASE_MATRIX checks=%lu compared_bytes=%lu\n",checks,checks*64ul*64ul*4ul);
 // The original matrix's ALPHA_8 branch used CT32. Exercise actual PSMT8 here.
 for(size_t w=0;w<sizeof widths/sizeof *widths;w++){
  i.m.format=GL_TEXFMT_ALPHA_8;i.m.flags=0;i.m.packed=0;i.r.psm=PSM_T8;i.r.screen=0;i.fb32=0;
  i.r.w=i.m.width=widths[w];i.r.h=i.m.height=131;i.fbw=i.r.w+5;
  unsigned long oldfast=alpha_fast,oldfallback=alpha_fallback;
  for(int region=0;region<2;region++)for(int wrap=0;wrap<4;wrap++)for(int origin=0;origin<8;origin++){
   i.region=region;i.wrapx=wrap&1;i.wrapy=wrap&2;
   i.x=origin<3?origin*32-17:origin<6?i.r.w+origin*17:origin==6?-1000000:1000000;
   i.y=origin<3?origin*33-17:origin<6?i.r.h+origin*17:origin==6?-1000000:1000000;
   check(&i,a,b,which++);
  }
  printf("TC ALPHA8 width=%d fast=%lu fallback=%lu\n",i.r.w,alpha_fast-oldfast,alpha_fallback-oldfallback);
 }
	/* Include preparation/zeroing on every benchmark upload; no cached rows. */
	i.r.w = i.m.width = 1024; i.r.h = i.m.height = 131; i.fbw = 1029; i.x = -87; i.y = 48; i.wrapx = i.wrapy = 1; i.region = 1;
	for (int mode = 0; mode < 3; mode++)
	{
		i.m.format = mode == 0 ? GL_TEXFMT_P_8 : GL_TEXFMT_AP_88; i.m.packed = 0; i.r.psm = mode == 0 ? PSM_T8 : PSM_CT32;
		i.r.screen = mode == 2; i.fb32 = 1;
		const int count = 6000; double tb = seconds();
		for (int n = 0; n < count; n++) { base_fill(&i, a); sink += a[n & 16383]; }
		tb = seconds() - tb; double tf = seconds();
		for (int n = 0; n < count; n++) { fast_fill(&i, b); sink += b[n & 16383]; }
		tf = seconds() - tf;
		printf("TC TIMING mode=%d uploads=%d base_ns=%.0f fast_ns=%.0f speedup=%.3f\n", mode, count, tb*1e9/count, tf*1e9/count, tb/tf);
	}
	printf("TC PLANS checks=%lu fast=%lu fallback=%lu alpha_fast=%lu alpha_fallback=%lu context_base=%u context_fast=%u delta=%u\n",plan_checks,fast_plans,fallback_plans,alpha_fast,alpha_fallback,base_context_size(),fast_context_size(),fast_context_size()-base_context_size());
	printf("TC COMPLETE checks=%lu failures=%lu compared_bytes=%lu sink=%u\n", checks, failures, checks*64ul*64ul*4ul, sink);
	free(b); free(a); free(data); return failures ? 1 : 0;
}
