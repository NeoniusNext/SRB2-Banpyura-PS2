/* Complete isolated production texture source; host replaces only DMA and engine allocation services. */
#include <math.h>
#include <stdint.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shim.h"
#include "ps2_hwd_dbg.h"
#define CONS_WARNING 1
#define CONS_ERROR 2
#define CONS_Alert(level, ...) ((void)(level))
#define CONS_Printf(...) ((void)0)
#define I_OutputMsg(...) ((void)0)
#include "ps2_hw_priv.inc"
#include "ps2_hw_regs.inc"
#include "ps2_hw_fx2.h"
#include "ps2_hw_xform.inc"

#define CAP_QW (1u << 19)
static qw_t *capture;
static u32 capture_n, flushes, locks, unlocks, references;
static unsigned long long checks, texels, uploads, corpus_cases;
static struct { int chunk_open; } V;
UINT32 hwr_texsig;
int ps2hwd_fx2;
void Z_SetRebuildCost(void **user, unsigned int cost) { (void)user;(void)cost; }
static int HWR_ShouldUsePaletteRendering(void) { return 1; }
static void pk_flush(void) { H.wr = 0; flushes++; }
static qw_t *pk_alloc(u32 n)
{
	qw_t *p;
	if (H.wr + n > BUF_QW) pk_flush();
	if (capture_n + n > CAP_QW) abort();
	p = capture + capture_n;
	capture_n += n;
	H.wr += n;
	return p;
}
static qw_t *ad_alloc(u32 n)
{
	qw_t *p = pk_alloc(n + 1);
	p[0].d[0] = HWGIF_TAG(n, 1, 0, 0, 0, 1);
	p[0].d[1] = GIF_REGS_AD;
	H.serial++;
	return p + 1;
}
static void pk_ref(const void *p, u32 n) { memcpy(pk_alloc(n), p, (size_t)n * 16); references++; }
static void pk_ref_tail(const void *p, u32 bytes) { qw_t *q = pk_alloc(1); memset(q, 0, 16); memcpy(q, p, bytes); references++; }
static void ring_wait(int limit) { (void)limit; H.done_seq = H.q_seq; }
static void vu_chunk_flush(void) { V.chunk_open = 0; }
#define SyncDCache(a,b) ((void)0)
void HWR_PS2_LockData(void *p) { (void)p; locks++; }
void HWR_PS2_UnlockData(void *p) { (void)p; unlocks++; }
void HWR_PS2_FreeData(void *p) { free(p); }
void *HWR_PS2_StealData(GLMipmap_t *m, void **user) { void *p=m->data; m->data=NULL; *user=p; return p; }
void *HWR_PS2_AllocData(size_t n, void **user) { void *p=malloc(n); *user=p; return p; }
const u8 *HWR_PS2_FlatPin(const GLMipmap_t *m) { return m->data; }
boolean HWR_PS2_FlatStreamable(const GLMipmap_t *m) { (void)m; return 0; }
boolean HWR_PS2_FlatRows(const GLMipmap_t *m, UINT32 row, UINT32 n, UINT8 *p) { (void)m;(void)row;(void)n;(void)p;return 0; }
void HWR_PS2_FlatUnpin(const u8 *p, size_t n) { (void)p;(void)n; }
static void ov_flush_all(void) { }
static u32 fb_pages(int stride,int rows,int psm32) { return (u32)(stride/64)*(u32)((rows+(psm32?31:63))/(psm32?32:64)); }
int PS2HWD_ReadVram(void *dst,unsigned int blk,unsigned int tbw,int psm,int x,int y,int w,int h) { (void)dst;(void)blk;(void)tbw;(void)psm;(void)x;(void)y;(void)w;(void)h;return -1; }
static u32 tex_want(const GLMipmap_t *m, int *visible) { (void)m;*visible=1;return 0; }
#include "ps2_hw_pal.inc"
#include "ps2_hw_tex.inc"
#include "texture-registers.inc"
#include "texture-policy.inc"

static void check(int yes, const char *what)
{
	checks++;
	if (!yes) { fprintf(stderr,"P4 proof failed: %s (check %llu)\n",what,checks);exit(1); }
}

static void reset(void)
{
	int i;
	rel_release_all();
	dc_flush();
	free(H.rec);
	memset(&H,0,sizeof H);
	if (!blk_owner_p) blk_owner_p=calloc(1,sizeof *blk_owner_p);
	memset(blk_owner,0,sizeof blk_owner);
	H.up=1;H.pool_base=2048;H.pool_blocks=VRAM_BLOCKS-H.pool_base;
	H.clut_base=1024;H.free_n=1;H.free_range[0].start=H.pool_base;H.free_range[0].len=H.pool_blocks;
	H.rec_free=H.lru_head=H.lru_tail=H.cur_tex=NOREC;
	H.pal_set=1;H.pal_gen=7;
	for(i=0;i<256;i++) H.pal[i]=(u32)i|((u32)(255-i)<<8)|((u32)(i^0x5a)<<16);
	ps2hwd_dbg_flags=0;cfg.tex_max_bytes=0;
	capture_n=0;references=0;locks=unlocks=0;
}

static u32 expected_blocks(u32 width,u32 height,u32 tbw,int p4)
{
	/* Independent rectangle walk using the documented GS block order. */
	static const u8 t4[8][4]={{0,2,8,10},{1,3,9,11},{4,6,12,14},{5,7,13,15},{16,18,24,26},{17,19,25,27},{20,22,28,30},{21,23,29,31}};
	static const u8 t8[4][8]={{0,1,4,5,16,17,20,21},{2,3,6,7,18,19,22,23},{8,9,12,13,24,25,28,29},{10,11,14,15,26,27,30,31}};
	u32 x,y,end=0,bw=p4?32u:16u,ph=p4?128u:64u;
	for(y=0;y<height;y+=16) for(x=0;x<width;x+=bw)
	{
		u32 block=((y/ph)*(tbw/2u)+x/128u)*32u+(p4?t4[(y%128u)/16u][(x%128u)/32u]:t8[(y%64u)/16u][(x%128u)/16u])+1u;
		if(block>end)end=block;
	}
	return end;
}

static void compare_upload(const u8 *source,u32 width,u32 height,u32 band,int p4)
{
	u32 at=0,row=0;
	while(at<capture_n)
	{
		qw_t *q=&capture[at];
		if((q[0].d[0]&0x7fffu)==4 && q[0].d[1]==GIF_REGS_AD && q[1].d[1]==GSR_BITBLTBUF)
		{
			u32 psm=(u32)(q[1].d[0]>>56)&63u,rrw=(u32)q[3].d[0],rrh=(u32)(q[3].d[0]>>32),nqw=(u32)(q[5].d[0]&0x7fffu);
			u32 bytes=rrw*rrh/(p4?2u:1u),i;
			u8 *data=(u8 *)(q+6);
			check(psm==(u32)(p4?PSM_T4:PSM_T8),"upload PSM");
			check(rrw==width && (u32)(q[2].d[0]>>48)==row,"transfer row position");
			check(!p4 || (!(rrw&7u) && !((q[2].d[0]>>32)&3u)),"legal P4 transfer alignment");
			check(nqw==(bytes+15u)/16u,"exact image qword count");
			for(i=0;i<rrw*rrh;i++)
			{
				u32 index=p4?band*16u+((data[i/2u]>>((i&1u)*4u))&15u):data[i];
				check(index==source[(size_t)row*width+i],"every original index preserved");texels++;
			}
			for(i=bytes;i<nqw*16u;i++) check(data[i]==0,"qword tail padding initialized");
			row+=rrh;at+=6u+nqw;
		}
		else
		{
			u32 n=(u32)(q[0].d[0]&0x7fffu);
			check(q[0].d[1]==GIF_REGS_AD && n==1u && q[1].d[1]==GSR_TEXFLUSH,"ordered upload TEXFLUSH");
			at+=2;
		}
	}
	check(row==height,"all image rows uploaded exactly once");uploads++;
}

static void texture_pixels(u32 width,u32 height,u32 band,int policy,const u8 *input)
{
	GLMipmap_t m;
	u32 n=width*height,i,base_locks;
	u8 *source=malloc(n),*saved=malloc(n);
	int rec,p4;
	reset();memset(&m,0,sizeof m);
	for(i=0;i<n;i++)source[i]=(u8)(band*16u+(i%15u)); /* reserve index255 for the fallback case */
	if(input)memcpy(source,input,n);
	else
	{
		if(policy==1)source[n-1]=(u8)(((band+1u)&15u)*16u);
		if(policy==2)source[n-1]=255;
	}
	memcpy(saved,source,n);
	m.width=(UINT16)width;m.height=(UINT16)height;m.format=GL_TEXFMT_P_8;m.flags=TF_WRAPXY;m.regen_kind=2;m.regen_id=17;m.data=source;
	if(policy==3)ps2hwd_dbg_flags=1;
	if(policy==4)cfg.tex_max_bytes=4*1024*1024;
	if(policy==5)H.pal_set=0;
	if(policy==6)m.flags=TF_WRAPX;
	if(policy==7)H.cap_adapt=1;
	base_locks=locks;
	rec=tex_upload(&m);
	check(rec!=NOREC,"complete production tex_upload succeeds");
	p4=H.rec[rec].psm==PSM_T4;
	check(p4==(policy==0 && n>=2048 && !(width&7u) && expected_blocks(width,height,((width+127)/128)*2,1)<expected_blocks(width,height,((width+127)/128)*2,0)),"eligibility and exact T8 fallback");
	check(H.rec[rec].csa==(p4?band:0u),"stored original palette band");
	check(H.rec[rec].w==width && H.rec[rec].h==height,"logical dimensions unchanged");
	check(H.rec[rec].nblk==expected_blocks(width,height,H.rec[rec].tbw,p4),"exact native footprint blocks");
	compare_upload(saved,width,height,band,p4);
	check(!p4 || !references,"P4 DMA reads packed ring, never original byte stream");
	check(!p4 || (locks-base_locks==unlocks),"synchronous source lock is balanced");
	if(m.data)free(m.data);
	free(saved);
}

static void texture_case(u32 width,u32 height,u32 band,int policy) { texture_pixels(width,height,band,policy,NULL); }

static void stock_corpus(const char *path)
{
	FILE *file=fopen(path,"rb");
	u8 magic[8],*data;
	u32 count,i,header[4],k;
	check(file!=NULL,"corpus opened");
	check(fread(magic,1,8,file)==8 && memcmp(magic,"P4CORP12",8)==0,"corpus format");
	check(fread(&count,4,1,file)==1,"corpus count");
	for(i=0;i<count;i++)
	{
		int policy;
		check(fread(header,4,4,file)==4 && header[0]*header[1]==header[3],"corpus dimensions");
		data=malloc(header[3]);check(data!=NULL,"corpus allocation");
		check(fread(data,1,header[3],file)==header[3],"all corpus texels loaded");
		policy=header[2]==0xffffffffu?1:0;
		for(k=0;k<header[3];k++)if(data[k]==255){policy=2;break;}
		texture_pixels(header[0],header[1],header[2]==0xffffffffu?0:header[2],policy,data);
		free(data);corpus_cases++;
	}
	check(fgetc(file)==EOF,"corpus consumed exactly");fclose(file);
}

static void band_registers(void)
{
	u32 band,palette,row,index;
	texrec_t r;
	reset();memset(&r,0,sizeof r);r.psm=PSM_T4;r.w=r.h=r.uw=r.vh=64;r.tw=r.th=6;r.tbw=2;
	for(palette=0;palette<3;palette++)for(row=0;row<32;row++)for(band=0;band<16;band++)
	{
		u32 clut[256],internal[256],i;
		u64 t0,t1,cl;
		r.csa=(u8)band;
		for(i=0;i<256;i++)clut[i]=((i*0x10203u)^(row*0x30201u)^(palette*0x890123u))|0x80000000u;
		/* CSM1 full T8 preload reads all logical entries, then CSA selects its internal bank. */
		clut_swizzle(clut);
		for(i=0;i<256;i++)internal[i]=clut[(i&~24u)|((i&8u)<<1)|((i&16u)>>1)];
		H.clut_loaded=0;capture_n=0;
		tex_regs(&r,0,0,1,1200+row*4,1,&t0,&t1,&cl);
		check(capture_n==2,"unknown palette emits full preload packet");
		check(capture[1].d[1]==GSR_TEX2_1 && ((capture[1].d[0]>>20)&63u)==PSM_T8 && (capture[1].d[0]>>61)==1 && ((capture[1].d[0]>>56)&31u)==0,"full T8 CLD1 CSA0, never partial T4 load");
		check(((t0>>20)&63u)==PSM_T4 && ((t0>>56)&31u)==band && (t0>>61)==0,"P4 CLD0 selects original bank");
		for(index=0;index<16;index++)check(internal[band*16+index]==((((band*16+index)*0x10203u)^(row*0x30201u)^(palette*0x890123u))|0x80000000u),"all bands/rows/palettes preserve CLUT colour");
		tex_regs(&r,0,0,1,1200+row*4,1,&t0,&t1,&cl);
		check(capture_n==2,"same complete CLUT identity reuses full contents");
	}
}

static void policy_counterexample(void)
{
	GLMipmap_t m[3];
	const float areas[3]={2000.0f,8000.0f,100.0f};
	const int bin[3]={11,19,15}; /* exact upper edges 1/4, 1, 1/2 */
	u32 level[2][3],pass,i;
	reset();memset(m,0,sizeof m);
	plan_info_p=calloc(1,sizeof *plan_info_p);
	for(i=0;i<3;i++)
	{
		m[i].width=(UINT16)(i==2?512:1024);m[i].height=(UINT16)(i==2?256:1024);
		m[i].format=GL_TEXFMT_P_8;m[i].regen_kind=1;
		plan_info[i].key=&m[i];plan_info[i].total=areas[i];plan_info[i].hist[bin[i]]=areas[i];
	}
	H.pool_base=4352;H.pool_blocks=12032;H.free_range[0].start=H.pool_base;H.free_range[0].len=H.pool_blocks;
	H.plan_pct=69;plan_n=3;imm_nprev=0;plan_nofid=0;
	for(pass=0;pass<2;pass++)
	{
		H.plan_reserve=pass?3468u:3980u;plan_active=1;
		PS2HWD_PlanEnd();
		for(i=0;i<3;i++)level[pass][i]=m[i].ps2_want;
	}
	check(level[0][0]==1 && level[0][1]==1 && level[0][2]==0,"literal production lower-budget counterexample");
	check(level[1][0]==2 && level[1][1]==0 && level[1][2]==1,"literal production larger-budget counterexample");
	check(2587u+14u*64u+(2587u+14u*64u)/7u==3980u,"baseline unplanned reserve is reachable");
	check(2587u+14u*32u+(2587u+14u*32u)/7u==3468u,"P4 unplanned reserve is reachable");
	free(plan_info_p);plan_info_p=NULL;
}

int main(int argc,char **argv)
{
	u32 band,w,h,policy;
	capture=calloc(CAP_QW,sizeof *capture);if(!capture)return 2;
	for(band=0;band<16;band++)for(w=8;w<=1024;w*=2)for(h=1;h<=1024;h*=2)
		if(w*h>=2048)texture_case(w,h,band,0);
	for(policy=1;policy<=7;policy++)texture_case(64,64,8,(int)policy);
	texture_case(17,128,8,0);texture_case(24,97,8,0);texture_case(40,53,8,0);texture_case(8,257,8,0);
	if(argc>1)stock_corpus(argv[1]);
	band_registers();policy_counterexample();reset();free(H.rec);H.rec=NULL;free(blk_owner_p);free(capture);
	printf("{\"checks\":%llu,\"texels\":%llu,\"uploads\":%llu,\"corpus_cases\":%llu,\"bands\":16,\"CLUT_rows\":32,\"palette_epochs\":3,\"ring_flushes\":%u,\"policy_counterexample\":{\"budgets\":[4300,4812],\"levels\":[[1,1,0],[2,0,1]],\"reject_naive_no_quality_loss_claim\":true}}\n",checks,texels,uploads,corpus_cases,flushes);
	return 0;
}
