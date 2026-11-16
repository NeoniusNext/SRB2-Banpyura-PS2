/* Full actual texture source with host replacements for engine/DMA services.
 * The independent pixel/block bitmap oracles never use a compact record layout. */
#include <math.h>
#include <stdint.h>
#include <stddef.h>
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
static u32 capture_n, locks, unlocks, references, flushes, failures;
static unsigned long long checks, pixels, uploads, snapshots, allocator_ops;
static FILE *output;
static int fail_realloc;
static struct { int chunk_open; } V;
UINT32 hwr_texsig;
int ps2hwd_fx2;
static GLMipmap_t *owners[2048];
static int owner_count;

static void check(int yes, const char *what)
{
	checks++;
	if (!yes) { fprintf(stderr,"Texture record proof failed: %s (check %llu)\n",what,checks);exit(1); }
}
static void *test_realloc(void *p, size_t bytes)
{
	if (fail_realloc) { fail_realloc = 0; return NULL; }
	return realloc(p, bytes);
}
void Z_SetRebuildCost(void **user, unsigned int cost) { (void)user; (void)cost; }
static int HWR_ShouldUsePaletteRendering(void) { return 1; }
static void pk_flush(void)
{
	if (!H.wr) return;
	H.buf_seq[H.cur] = ++H.q_seq;
	H.cur = (H.cur + 1) % NBUF;
	H.wr = 0; flushes++;
}
static qw_t *pk_alloc(u32 n)
{
	qw_t *p;
	if (H.wr + n > BUF_QW) pk_flush();
	check(capture_n + n <= CAP_QW, "bounded packet capture");
	p = capture + capture_n; capture_n += n; H.wr += n;
	return p;
}
static qw_t *ad_alloc(u32 n)
{
	qw_t *p = pk_alloc(n + 1);
	p[0].d[0] = HWGIF_TAG(n, 1, 0, 0, 0, 1); p[0].d[1] = GIF_REGS_AD;
	H.serial++; return p + 1;
}
static void pk_ref(const void *p,u32 n) { memcpy(pk_alloc(n),p,(size_t)n*16); references++; }
static void pk_ref_tail(const void *p,u32 bytes) { qw_t *q=pk_alloc(1); memset(q,0,16); memcpy(q,p,bytes); references++; }
static void ring_wait(int limit) { (void)limit; H.done_seq=H.q_seq; }
static void vu_chunk_flush(void) { V.chunk_open=0; }
#define SyncDCache(a,b) ((void)0)
void HWR_PS2_LockData(void *p) { check(p!=NULL,"lock live source"); locks++; }
void HWR_PS2_UnlockData(void *p) { check(p!=NULL,"unlock live source"); unlocks++; }
void HWR_PS2_FreeData(void *p) { free(p); }
void *HWR_PS2_StealData(GLMipmap_t *m,void **user) { void *p=m->data; m->data=NULL; *user=p; return p; }
void *HWR_PS2_AllocData(size_t n,void **user) { void *p=malloc(n); *user=p; return p; }
const u8 *HWR_PS2_FlatPin(const GLMipmap_t *m) { return m->data; }
boolean HWR_PS2_FlatStreamable(const GLMipmap_t *m) { (void)m; return 0; }
boolean HWR_PS2_FlatRows(const GLMipmap_t *m,UINT32 row,UINT32 n,UINT8 *p) { (void)m;(void)row;(void)n;(void)p;return 0; }
void HWR_PS2_FlatUnpin(const u8 *p,size_t n) { (void)p;(void)n; }
static void ov_flush_all(void) { }
static u32 fb_pages(int stride,int rows,int psm32) { return (u32)(stride/64)*(u32)((rows+(psm32?31:63))/(psm32?32:64)); }
int PS2HWD_ReadVram(void *dst,unsigned int blk,unsigned int tbw,int psm,int x,int y,int w,int h)
{ (void)blk;(void)tbw;(void)x;(void)y;memset(dst,0x5a,(size_t)w*h*(psm==PSM_CT32?4:2));return 0; }
static u32 wanted_level;
static u32 tex_want(const GLMipmap_t *m,int *visible) { (void)m; *visible=1; return wanted_level; }
static void failed(const char *what) { (void)what; failures++; }
#define realloc test_realloc
#include "ps2_hw_pal.inc"
#include "ps2_hw_tex.inc"
#undef realloc
#include "texture-registers.inc"

static void word(u32 value)
{
	u8 bytes[4]={(u8)value,(u8)(value>>8),(u8)(value>>16),(u8)(value>>24)};
	check(fwrite(bytes,1,4,output)==4,"normalized snapshot write");
}
static void floatword(float f) { union {float f;u32 u;} v;v.f=f;word(v.u); }
static u32 owner_id(const GLMipmap_t *p)
{
	int i;
	if (!p) return 0;
	for (i=0;i<owner_count;i++) if (owners[i]==p) return (u32)i+1;
	check(0,"registered live record owner");return 0;
}
static void remember(GLMipmap_t *p)
{
	check(owner_count<(int)(sizeof owners/sizeof owners[0]),"owner catalog capacity");
	owners[owner_count++]=p;
}
static void snapshot(void)
{
	int i,k;
	snapshots++;
	word(H.used_blocks);word((u32)H.free_n);word((u32)H.rec_n);word((u32)H.rec_cap);
	word((u32)H.rec_free);word((u32)H.lru_head);word((u32)H.lru_tail);word(H.lru_seq);
	word(H.thrash_n);word(H.thrash_blk);word(H.pal_gen);word(H.clut_epoch);word(H.clut_loaded);
	word(H.st.uploads);word(H.st.upload_bytes);word(H.st.evictions);word(H.st.unsupported_mask);
	word(H.screen_bytes);word(H.screen_peak_bytes);word((u32)rel_total);word(rel_bytes);
	word(locks);word(unlocks);word(references);word(flushes);word(failures);word(hwr_texsig);
	for(i=0;i<H.free_n;i++){word(H.free_range[i].start);word(H.free_range[i].len);}
	for(i=0;i<VRAM_BLOCKS;i++)word(blk_owner[i]);
	for(i=0;i<owner_count;i++){word(owners[i]->downloaded);word(owners[i]->flags);word(owners[i]->ps2_drop);}
	for(i=0;i<H.rec_n;i++)
	{
		const texrec_t *r=&H.rec[i];
		word(r->used);word(r->used?owner_id(r->owner):0);
#define SAVE(m) word((u32)r->m)
		SAVE(blk);SAVE(nblk);SAVE(w);SAVE(h);SAVE(tw);SAVE(th);SAVE(tbw);SAVE(psm);SAVE(clut);
		SAVE(wrapx);SAVE(wrapy);SAVE(nearest_only);SAVE(baked);SAVE(screen);SAVE(notcc);SAVE(region);
		SAVE(npot_x);SAVE(npot_y);SAVE(pin);SAVE(stamp);SAVE(seq);SAVE(done);SAVE(ws);SAVE(wgt);
		SAVE(prev);SAVE(next);SAVE(uw);SAVE(vh);floatword(r->sxs);floatword(r->sys);
		SAVE(sw);SAVE(sh);SAVE(dx);SAVE(dy);SAVE(capi);SAVE(pad2);
		for(k=0;k<NCAPS;k++)word(r->fp[k]);
		SAVE(share);SAVE(sec);SAVE(sec_next);SAVE(has255);SAVE(shareable);SAVE(dmapend);SAVE(pad3);
		SAVE(imgws);SAVE(lodfr);SAVE(lod);SAVE(psplit);SAVE(pnext);SAVE(ppar);
		floatword(r->pt0);floatword(r->pt1);floatword(r->ptk);
#undef SAVE
	}
	for(i=0;i<DC_MAX;i++)
	{
		const dcent_t *e=&dc[i];
		word(e->p!=NULL);word(e->key);word(e->bytes);word(e->stamp);word(e->w);word(e->h);word(e->lvl);word(e->busy);
		if(e->p)check(fwrite(e->p,1,e->bytes,output)==e->bytes,"complete cached pixels snapshot");
	}
	word(capture_n);
	check(fwrite(capture,sizeof *capture,capture_n,output)==capture_n,"complete upload/register packets snapshot");
	capture_n=0;
}

static void reset(void)
{
	int i;
	dma_fence();rel_release_all();dc_flush();
	free(H.rec);
	for(i=0;i<NSCR;i++)free(H.scr_data[i]);
	memset(&H,0,sizeof H);
	if(!blk_owner_p)blk_owner_p=calloc(1,sizeof *blk_owner_p);
	check(blk_owner_p!=NULL,"owner bitmap allocation");memset(blk_owner,0,sizeof blk_owner);
	H.up=1;H.pool_base=3872;H.pool_blocks=VRAM_BLOCKS-H.pool_base;H.clut_base=3360;
	H.free_n=1;H.free_range[0].start=H.pool_base;H.free_range[0].len=H.pool_blocks;
	H.rec_free=H.lru_head=H.lru_tail=H.cur_tex=NOREC;
	for(i=0;i<NSCR;i++)H.scr_rec[i]=NOREC;
	H.fbw=H.vw=320;H.fbh=H.vh=224;H.fb32=1;H.fbpsm=PSM_CT32;
	H.pal_set=1;H.pal_gen=7;H.frame_no=0x81234567u;
	for(i=0;i<256;i++)H.pal[i]=(u32)i|((u32)(255-i)<<8)|((u32)(i^0x5a)<<16);
	pal_build();batch_phase=0;ps2hwd_dbg_flags=HWDBG_NOZC;cfg.tex_max_bytes=0;wanted_level=0;
	resource_error_handler=failed;hwr_texsig=0;
	capture_n=0;references=0;locks=unlocks=flushes=failures=0;owner_count=0;
}

static void consistent(void)
{
	u16 expected[VRAM_BLOCKS]={0};
	u32 total=0,freeblocks=0,b;
	int i,prev=NOREC,seen=0,own=0;
	for(i=0;i<H.rec_n;i++)
	{
		const texrec_t *r=&H.rec[i];
		if(!r->used)continue;
		check(r->blk>=H.pool_base && (u32)r->blk+r->nblk<=VRAM_BLOCKS,"complete live span within GS memory");
		check(r->w && r->h && r->uw==r->w && r->vh==r->h,"used dimensions preserve exact logical size");
		if(r->owner && r->ppar==NOREC)check(r->owner->downloaded==(u32)i+1,"engine handle refers to current record");
		if(r->share!=NOREC)
		{
			check(H.rec[r->share].used && H.rec[r->share].share==NOREC,"view has a live owning image");
			check(r->blk==H.rec[r->share].blk && r->nblk==H.rec[r->share].nblk,"view shares exact owner span");
			continue;
		}
		own++;total+=r->nblk;
		for(b=r->blk;b<(u32)r->blk+r->nblk;b++){check(!expected[b],"live image spans do not overlap");expected[b]=(u16)(i+1);}
	}
	for(i=0;i<H.free_n;i++)
	{
		u32 start=H.free_range[i].start,len=H.free_range[i].len;
		check(len && start>=H.pool_base && start+len<=VRAM_BLOCKS,"free span bounded");
		if(i)check((u32)H.free_range[i-1].start+H.free_range[i-1].len<start,"free spans sorted and coalesced");
		freeblocks+=len;
		for(b=start;b<start+len;b++){check(!expected[b],"free/live spans do not overlap");expected[b]=0xffffu;}
	}
	check(total==H.used_blocks && total+freeblocks==H.pool_blocks,"all GS blocks accounted exactly");
	for(b=H.pool_base;b<VRAM_BLOCKS;b++)check(expected[b] && blk_owner[b]==(expected[b]==0xffffu?0:expected[b]),"all owner bitmap labels exact");
	for(i=H.lru_head;i!=NOREC;i=H.rec[i].next)
	{
		check(++seen<=H.rec_n && H.rec[i].used && H.rec[i].share==NOREC,"LRU holds each live image once");
		check(H.rec[i].prev==prev,"LRU links preserved");prev=i;
	}
	check(seen==own && prev==H.lru_tail,"LRU endpoints and membership exact");
}

static u32 sample(const GLMipmap_t *m,const u8 *source,u32 x,u32 y,int psm)
{
	size_t at=(size_t)y*m->width+x;
	if(m->format==GL_TEXFMT_P_8 || m->format==GL_TEXFMT_ALPHA_8)return source[at];
	if(m->format==GL_TEXFMT_AP_88)
	{
		u32 index=source[at*2],a=source[at*2+1];
		if(psm==PSM_T8)return (!(m->flags&TF_CHROMAKEYED) && !a)?255:index;
		return H.pal[index]|(((a*128u+127u)/255u)<<24);
	}
	if(m->format==GL_TEXFMT_RGBA)
	{
		u32 pixel;memcpy(&pixel,source+at*4,4);
		if(psm==PSM_T8)
		{
			u32 i;if(!(pixel>>24))return 255;
			for(i=0;i<256;i++)if(H.pal[i]==(pixel&0xffffffu))return i;
			check(0,"independent RGBA palette lookup");
		}
		return (pixel&0xffffffu)|(((((pixel>>24)*128u+127u)/255u))<<24);
	}
	return (u32)source[at*2]*0x010101u|((((u32)source[at*2+1]*128u+127u)/255u)<<24);
}

static void pixels_exact(const GLMipmap_t *m,const u8 *source,int ri)
{
	const texrec_t *a=&H.rec[ri];
	u32 pos=0,rows=0;
	while(pos<capture_n)
	{
		const qw_t *q=&capture[pos];
		u32 n=(u32)q[0].d[0]&0x7fffu;
		if(n==4 && q[0].d[1]==GIF_REGS_AD && q[1].d[1]==GSR_BITBLTBUF)
		{
			u32 blk=(u32)(q[1].d[0]>>32)&0x3fffu,psm=(u32)(q[1].d[0]>>56)&63u;
			u32 w=(u32)q[3].d[0],h=(u32)(q[3].d[0]>>32),r0=(u32)(q[2].d[0]>>48);
			u32 nq=(u32)q[5].d[0]&0x7fffu,bpp=psm==PSM_T8?1u:4u,x,y,offset=0;
			const u8 *data=(const u8 *)(q+6);
			const texrec_t *r=a;
			if(a->pnext!=NOREC && blk==H.rec[a->pnext].blk){r=&H.rec[a->pnext];offset=a->h;}
			check(blk==r->blk && psm==r->psm && w==r->w,"transfer selects exact admitted record");
			check(q[1].d[1]==GSR_BITBLTBUF && q[2].d[1]==GSR_TRXPOS && q[3].d[1]==GSR_TRXREG && q[4].d[1]==GSR_TRXDIR,"ordered transfer setup");
			check(r0+h<=r->h && nq==(w*h*bpp+15u)/16u,"all transfer rows/qwords bounded");
			for(y=0;y<h;y++)for(x=0;x<w;x++)
			{
				u32 originalx=(x<<a->dx)+((1u<<a->dx)>>1);
				u32 originaly=((r0+y)<<a->dy)+((1u<<a->dy)>>1)+offset;
				u32 want=sample(m,source,originalx,originaly,(int)psm),got;
				if(bpp==1)got=data[y*w+x];else memcpy(&got,data+((size_t)y*w+x)*4,4);
				check(got==want,"every uploaded texel preserves source RGB/alpha/index");pixels++;
			}
			for(x=w*h*bpp;x<nq*16u;x++)check(!data[x],"qword tail zero initialized");
			rows+=h;pos+=6+nq;
		}
		else
		{
			check(n==1 && q[0].d[1]==GIF_REGS_AD && q[1].d[1]==GSR_TEXFLUSH,"TEXFLUSH follows upload data");pos+=2;
		}
	}
	check(rows==(u32)a->h+(a->pnext==NOREC?0u:H.rec[a->pnext].h),"each stored row uploaded once");uploads++;
}

static void texture_cases(void)
{
	static const u16 dims[][2]={{1,1},{3,5},{17,65},{127,63},{129,65},{257,129},{64,1024},{64,1536},{1024,512},{16384,1},{1,16384}};
	u32 kind,di,cap;
	for(kind=0;kind<9;kind++)for(di=0;di<sizeof dims/sizeof dims[0];di++)for(cap=0;cap<NCAPS;cap++)
	{
		GLMipmap_t m;u32 width=dims[di][0],height=dims[di][1],n=width*height,bpp,i;int ri;
		u8 *source;
		reset();memset(&m,0,sizeof m);remember(&m);
		m.width=(UINT16)width;m.height=(UINT16)height;m.flags=TF_WRAPXY;m.regen_id=37;
		m.format=kind==0?GL_TEXFMT_P_8:kind<=4?GL_TEXFMT_AP_88:kind<=6?GL_TEXFMT_RGBA:kind==7?GL_TEXFMT_ALPHA_8:GL_TEXFMT_ALPHA_INTENSITY_88;
		bpp=m.format==GL_TEXFMT_RGBA?4u:(m.format==GL_TEXFMT_AP_88 || m.format==GL_TEXFMT_ALPHA_INTENSITY_88)?2u:1u;
		source=malloc((size_t)n*bpp);check(source!=NULL,"pixel source allocation");m.data=source;
		for(i=0;i<n;i++)
		{
			u8 index=(u8)(i*17u+i/width);
			if(bpp==1)source[i]=index;
			else if(bpp==2)
			{
				source[i*2]=index;source[i*2+1]=(u8)(kind==8 || kind==2?i:kind==3 && (i&3u)==0?0:255);
				if(kind==4){m.flags|=TF_CHROMAKEYED;source[i*2+1]=(u8)i;}
			}
			else
			{
				u32 color=H.pal[index]|((kind==6?(i&255u):255u)<<24);memcpy(source+i*4,&color,4);
			}
		}
		if(kind==0){m.regen_kind=1;m.flags|=TF_CHROMAKEYED;}
		if(kind==1 && width>17 && height<1024){m.ps2_uw=(UINT16)(width-3);m.ps2_uh=(UINT16)(height-1);}
		H.tex_cap_blocks=CAPS[cap];H.cap_idx=(int)cap;
		ri=tex_upload(&m);check(ri!=NOREC,"actual texture conversion/admission succeeds");
		check(H.rec[ri].stamp==0x81234567u,"frame IDs retain high bits");
		pixels_exact(&m,source,ri);consistent();snapshot();
		tex_drop(ri,1);check(!m.downloaded,"eviction clears external handle");consistent();snapshot();free(source);
	}
}

static void references_and_views(void)
{
	GLMipmap_t m,twin;
	u8 *saved;int ri,vi;
	reset();memset(&m,0,sizeof m);memset(&twin,0,sizeof twin);remember(&m);remember(&twin);
	m.width=64;m.height=1536;m.format=GL_TEXFMT_P_8;m.flags=TF_WRAPXY;m.regen_kind=2;m.regen_id=13;
	m.data=malloc((size_t)m.width*m.height);check(m.data!=NULL,"aligned source allocation");
	memset(m.data,255,(size_t)m.width*m.height);saved=m.data;ps2hwd_dbg_flags=0;
	ri=tex_upload(&m);check(ri!=NOREC && m.data==NULL,"map source transferred to cache owner");
	check(H.rec[ri].pnext!=NOREC && rel_total==2,"split images queue two source references");
	twin=m;twin.downloaded=0;twin.flags|=TF_CHROMAKEYED;
	vi=tex_view(&twin,ri);check(vi!=NOREC,"keyed twin record created");
	check(H.rec[vi].clut==2 && H.rec[vi].share==ri,"keyed view preserves owner and CLUT semantics");
	consistent();snapshot();
	check(dc_find(dc_key(&m),64,1536,0)!=NULL,"source cache still owns referenced source");
	tex_drop(ri,1);check(!m.downloaded && !twin.downloaded,"owner eviction invalidates all split pieces and views");
	check(rel_total==2 && dc_find(dc_key(&m),64,1536,0)->p==saved,"evicted image source remains live during pending DMA");
	check(dc_slot(DC_BUDGET)==NULL,"busy source cannot be evicted to satisfy cache budget");
	consistent();snapshot();dma_fence();
	check(!rel_total && locks==unlocks,"source references released after completion");
	ri=tex_upload(&m);check(ri!=NOREC,"cache reconstructs evicted split image without engine source");
	check(dc_find(dc_key(&m),64,1536,0)->p==saved,"reconstruction uses same live cache root");
	consistent();snapshot();dma_fence();tex_drop(ri,0);dc_flush();consistent();snapshot();
}

static void oversized_legacy_crop(void)
{
	GLMipmap_t m;u8 *source;u32 i;int ri;
	reset();memset(&m,0,sizeof m);remember(&m);
	m.width=2048;m.height=8;m.ps2_uw=1536;m.ps2_uh=7;m.format=GL_TEXFMT_AP_88;
	source=malloc((size_t)m.width*m.height*2);check(source!=NULL,"oversized crop source allocation");m.data=source;
	for(i=0;i<(u32)m.width*m.height;i++){source[i*2]=(u8)(i+i/m.width);source[i*2+1]=255;}
	/* The legacy crop overwrites W2 after tex_plan selected dx=1. A spare
	 * source row keeps every existing fill read within the original allocation.
	 * Preserve that accepted >1024 record exactly; this is no new GS-size claim. */
	ri=tex_upload(&m);check(ri!=NOREC,"legacy oversized crop retains its original admission");
	check(H.rec[ri].w==1536 && H.rec[ri].uw==1536 && H.rec[ri].h==7 && H.rec[ri].vh==7 && H.rec[ri].dx==1,"crop dimensions and preexisting selected shift preserved");
	pixels_exact(&m,source,ri);consistent();snapshot();tex_drop(ri,0);consistent();snapshot();free(source);
}

static void record_growth(void)
{
	GLMipmap_t *m=calloc(1200,sizeof *m);u8 source=11;int i;
	reset();check(m!=NULL,"growth owner allocation");
	for(i=0;i<1200;i++)
	{
		int ri;remember(&m[i]);m[i].width=m[i].height=1;m[i].format=GL_TEXFMT_P_8;m[i].data=&source;
		ri=tex_upload(&m[i]);check(ri==i,"record growth retains stable full-width handles");
		H.rec[ri].done=H.rec[ri].ws=H.rec[ri].imgws=H.rec[ri].lodfr=0xf1234567u;
		check(H.rec[ri].done==0xf1234567u,"record frame fields retain full width");
		if(i==510 || i==511 || i==512 || i==1024 || i==1199){consistent();snapshot();}
		else capture_n=0;
	}
	check(H.rec_cap==2048,"unchanged record growth capacities");
	for(i=1;i<1200;i+=2)tex_drop(i,0);
	consistent();snapshot();
	for(i=1;i<1200;i+=2)check(tex_upload(&m[i])!=NOREC,"freed record slots reused after realloc");
	consistent();snapshot();tex_free_all();
	check(H.rec && H.rec_n==0 && H.rec_cap==2048 && !H.used_blocks,"flush clears records while retaining table capacity");
	for(i=0;i<1200;i++)check(!m[i].downloaded,"flush clears every engine handle");
	consistent();snapshot();free(m);owner_count=0;
	reset();
	for(i=0;i<512;i++)check(rec_new()==i,"initial slab slots available");
	{
		texrec_t *old=H.rec;
		fail_realloc=1;check(rec_new()==NOREC,"record allocation failure reported");
		check(H.rec==old && H.rec_cap==512 && H.rec_n==512,"failed growth preserves old table and capacity");
		check(rec_new()==512 && H.rec_cap==1024,"growth retries without losing live records");
	}
}

static void captures_and_palette(void)
{
	GLMipmap_t m;RGBA_t colors[256];u8 source[64*64*2];int ri,i;
	reset();
	for(i=0;i<NSCR;i++){check(scr_tex_get(i)!=NOREC,"full capture record admitted");consistent();}
	snapshot();
	for(i=0;i<NSCR;i++)
	{
		ri=H.scr_rec[i];check(H.rec[ri].blk && H.rec[ri].nblk==1120,"CT32 screen retains full padded GS footprint");
		tex_drop(ri,0);check(H.scr_rec[i]==NOREC,"capture handle clears on drop");consistent();
	}
	snapshot();memset(&m,0,sizeof m);remember(&m);m.width=m.height=64;m.format=GL_TEXFMT_AP_88;m.data=source;
	for(i=0;i<64*64;i++){source[i*2]=(u8)i;source[i*2+1]=128;}
	ri=tex_upload(&m);check(ri!=NOREC && H.rec[ri].baked && H.rec[ri].psm==PSM_CT32,"partial-alpha patch keeps direct color");
	pixels_exact(&m,source,ri);consistent();snapshot();
	for(i=0;i<256;i++){colors[i].s.red=(UINT8)(255-i);colors[i].s.green=(UINT8)i;colors[i].s.blue=(UINT8)(i^0xa5);colors[i].s.alpha=255;}
	palette_set(colors);check(!m.downloaded,"palette update invalidates baked direct-color image");consistent();snapshot();
}

static u32 random_state=0x5912deadu;
static u32 random32(void) { random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state; }
static void bitmap_check(const u8 *busy)
{
	u8 freebits[VRAM_BLOCKS]={0};u32 used=0,b;int i;
	for(i=0;i<H.free_n;i++)for(b=H.free_range[i].start;b<(u32)H.free_range[i].start+H.free_range[i].len;b++){check(b<VRAM_BLOCKS && !freebits[b],"free bitmap bounded/nonoverlapping");freebits[b]=1;}
	for(b=H.pool_base;b<VRAM_BLOCKS;b++){check(freebits[b]==!busy[b],"free list equals independent occupied-block bitmap");used+=busy[b]!=0;}
	check(used==H.used_blocks,"used-block total equals independent bitmap");
}
static u32 oracle_allocate(u8 *busy,u32 n,u32 align,int high)
{
	u32 s,b;
	if(high)
	{
		for(s=(VRAM_BLOCKS-n)&~(align-1);s>=H.pool_base;s-=align)
		{
			for(b=0;b<n && !busy[s+b];b++);
			if(b==n)return s;
			if(s<align)break;
		}
	}
	else for(s=(H.pool_base+align-1)&~(align-1);s+n<=VRAM_BLOCKS;s+=align)
	{
		for(b=0;b<n && !busy[s+b];b++);
		if(b==n)return s;
	}
	return 0;
}
static void allocator_stress(void)
{
	u8 busy[VRAM_BLOCKS]={0};
	struct {u32 start,n;} held[128]={{0,0}};
	u32 step,b;int k;
	reset();
	/* Exercise the complete representable block domain independently of FB/Z. */
	H.pool_base=1;H.pool_blocks=VRAM_BLOCKS-1;H.free_range[0].start=1;H.free_range[0].len=H.pool_blocks;
	for(step=0;step<12000;step++)
	{
		k=(int)(random32()%128u);
		if(held[k].start)
		{
			vram_free(held[k].start,held[k].n);
			for(b=0;b<held[k].n;b++)busy[held[k].start+b]=0;
			held[k].start=0;
		}
		else
		{
			u32 n=1+(random32()%1024),align=1u<<(random32()%6),high=random32()&1u;
			u32 want=oracle_allocate(busy,n,align,(int)high),got=high?vram_alloc_hi(n,align):vram_alloc(n,align);
			check(got==want,"low/high aligned allocation equals independent bitmap search");
			if(got){held[k].start=got;held[k].n=n;for(b=0;b<n;b++)busy[got+b]=1;}
		}
		allocator_ops++;
		if(!(step&63u))bitmap_check(busy);
	}
	bitmap_check(busy);reset();
	H.pool_base=1;H.pool_blocks=VRAM_BLOCKS-1;H.free_range[0].start=1;H.free_range[0].len=H.pool_blocks;
	/* Worst-case one-block holes retain the original maximum range capacity. */
	memset(busy,1,sizeof busy);H.free_n=0;H.used_blocks=H.pool_blocks;
	for(b=H.pool_base;b<VRAM_BLOCKS;b+=2){vram_free(b,1);busy[b]=0;}
	check(H.free_n==8192,"maximum alternating free runs retained");bitmap_check(busy);
	for(b=H.pool_base+1;b<VRAM_BLOCKS;b+=2){vram_free(b,1);busy[b]=0;}
	check(H.free_n==1 && H.free_range[0].len==VRAM_BLOCKS-1,"all holes coalesce at the upper GS boundary");bitmap_check(busy);snapshot();
}

static void registers_and_bounds(void)
{
	texrec_t r;u32 w,h,psm;
	reset();memset(&r,0,sizeof r);
	r.blk=VRAM_BLOCKS-1;r.nblk=VRAM_BLOCKS;
	check(r.blk==16383 && r.nblk==16384,"full hardware block address/length domains retained");
	for(w=1;w<=1024;w++)for(h=1;h<=1024;h=h<8?h*2:h*2+1)for(psm=0;psm<2;psm++)
	{
		u64 t0,t1,clamp,expected;
		r.w=r.uw=(u16)w;r.h=r.vh=(u16)h;r.psm=(u8)(psm?PSM_T8:PSM_CT32);
		r.tw=(u8)log2_u32(w);r.th=(u8)log2_u32(h);r.tbw=(u8)(psm?((w+127)/128)*2:(w+63)/64);
		r.region=3;tex_regs(&r,1,1,1,1200,0,&t0,&t1,&clamp);
		expected=2ull|(2ull<<2)|((u64)(w-1)<<14)|((u64)(h-1)<<34);
		check(clamp==expected && (t0&0x3fffu)==16383 && t1==97,"all valid dimensions encode exact GS clamp and texture state");
		word((u32)t0);word((u32)(t0>>32));word((u32)clamp);word((u32)(clamp>>32));
	}
}

int main(int argc,char **argv)
{
	check(argc==2,"snapshot destination supplied");output=fopen(argv[1],"wb");check(output!=NULL,"snapshot destination opened");
	capture=calloc(CAP_QW,sizeof *capture);check(capture!=NULL,"capture allocated");
	texture_cases();oversized_legacy_crop();references_and_views();record_growth();captures_and_palette();allocator_stress();registers_and_bounds();
	reset();free(H.rec);free(blk_owner_p);free(capture);free(scratch);
	check(fclose(output)==0,"normalized snapshot closed");
	printf("{\"checks\":%llu,\"pixels\":%llu,\"uploads\":%llu,\"snapshots\":%llu,\"allocator_operations\":%llu}\n",checks,pixels,uploads,snapshots,allocator_ops);
	return 0;
}
