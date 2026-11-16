#include <setjmp.h>
#define main base_suite_unused
#include "base_hosttest.c"
#undef main
unsigned int hwr_geo_off;
static unsigned exact_bytes, proxy_cases, missing_regen_cases, policy_cases, record_failures;
static int jump_on_error, require_unlocked_error;
static jmp_buf error_jump;
static void record_error(const char *message)
{
    (void)message;fixture_errors++;
    if(require_unlocked_error)for(unsigned i=0;i<fixture_blocks_n;i++)if(fixture_blocks[i].p)
        fixture_check(!fixture_blocks[i].locked,"explicit budget releases fresh-flat lock before resource error callback");
    if(jump_on_error)longjmp(error_jump,1);
}

static void begin_case(void)
{
    host_init(1);tile_cache_rec=NOREC;tile_invalidate();
    H.fbw=H.vw=640;H.fbh=H.vh=512;H.scr_w=640;H.scr_h=512;
    H.fb_addr[0]=0;H.fb_addr[1]=640u*512u*4u;H.z_addr=2*H.fb_addr[1];
    H.clut_base=(H.z_addr+H.fb_addr[1])/256u;H.pool_base=H.clut_base+512;
    H.pool_blocks=VRAM_BLOCKS-H.pool_base;H.free_n=1;H.free_range[0].start=H.pool_base;H.free_range[0].len=H.pool_blocks;
    H.pal_set=1;H.pal_gen=3;H.frame_no=9;H.cap_adapt=0;H.tex_cap_blocks=0;
    for(unsigned i=0;i<256;i++)H.pal[i]=i*0x010101u;
    pal_build();cfg.tex_max_bytes=0;ps2hwd_dbg_flags=0;batch_phase=2;
    fixture_blocks_n=fixture_locks=fixture_unlocks=fixture_regens=fixture_rows=fixture_errors=0;
    fixture_realloc_fail=fixture_realloc_calls=0;jump_on_error=require_unlocked_error=0;
    fixture_fail_regen=fixture_stream=0;fixture_want=1;resource_error_handler=record_error;
    fixture_check(H.pool_blocks==512,"actual full-resolution PAL pool");
    fixture_check(tile_cache_get()!=NOREC,"permanent16KiB tile exists before pressure");
    unsigned blk=vram_alloc(448,32);fixture_check(blk!=0,"reserve ordinary448 pool blocks");
    int ri=rec_new();fixture_check(ri!=NOREC,"ordinary blocker record");
    H.rec[ri].blk=blk;H.rec[ri].nblk=448;H.rec[ri].pin=1;own_set(ri);lru_push_head(ri);
    fixture_check(H.used_blocks==512,"pool is completely occupied by pinned readers");
    cap_reset();
}

static void end_case(GLMipmap_t *m)
{
    tex_free_all();
    for(unsigned i=0;i<DC_MAX;i++)if(dc[i].p)
    {
        dc[i].busy=0;
        if(fixture_blocks[fixture_block(dc[i].p)].locked)HWR_PS2_UnlockData(dc[i].p);
    }
    dc_flush();
    for(unsigned i=0;i<fixture_blocks_n;i++)if(fixture_blocks[i].p)HWR_PS2_FreeData(fixture_blocks[i].p);
    m->data=NULL;m->downloaded=0;fixture_cases++;
}

static dcent_t *cached_level(GLMipmap_t *m,unsigned level,const u8 *bytes)
{
    unsigned w=m->width>>level,h=m->height>>level;
    dcent_t *e=dc_slot(w*h);fixture_check(e!=NULL,"cache metadata slot");
    fixture_allocate(w*h,(void **)(void *)&e->p);memcpy(e->p,bytes,w*h);
    e->key=dc_key(m);e->w=w;e->h=h;e->lvl=level;e->bytes=w*h;e->busy=0;e->stamp=++dc_seq;dc_bytes+=e->bytes;
    return e;
}

static void inspect_uploaded_tile(const texrec_t *tile,const u8 *full,int ox,int oy)
{
    unsigned copied=0;u32 pos=0;u8 actual[4096];memset(actual,0,sizeof actual);
    while(pos<cap_n)
    {
        const qw_t *q=cap+pos;unsigned n=(unsigned)q[0].d[0]&32767u;
        if(n==4&&q[1].d[1]==GSR_BITBLTBUF)
        {
            unsigned w=(unsigned)q[3].d[0],h=(unsigned)(q[3].d[0]>>32),y=(unsigned)(q[2].d[0]>>48);
            fixture_check(((q[1].d[0]>>32)&16383u)==tile->blk&&w==64&&y+h<=64,"tile transfer destination stays in reserved image");
            fixture_check((q[5].d[0]&32767u)==w*h/16u,"complete original tile bytes copied");
            memcpy(actual+y*w,q+6,w*h);copied+=w*h;pos+=6+w*h/16u;
        }
        else
        {
            fixture_check(n==1&&q[1].d[1]==GSR_TEXFLUSH,"only TEXFLUSH follows copied image");pos+=2;
        }
    }
    fixture_check(copied==4096,"all halo tile bytes uploaded");
    for(int y=0;y<64;y++)for(int x=0;x<64;x++)
    {
        int sx=(ox+x-1)%256,sy=(oy+y-1)%256;if(sx<0)sx+=256;if(sy<0)sy+=256;
        fixture_check(actual[y*64+x]==full[sy*256+sx],"tile bytes match full original source, including holes and halo");exact_bytes++;
    }
}

static void proxy_case(int kind,int keyed,int source_kind,int fresh,int fail_regen)
{
    static u8 full[65536],coarse[16384];GLMipmap_t m;dcent_t *l0=NULL;
    memset(full,41,sizeof full);full[0]=255;memset(coarse,41,sizeof coarse);fixture_source=full;
    begin_case();memset(&m,0,sizeof m);m.width=m.height=256;m.format=GL_TEXFMT_P_8;
    m.regen_kind=kind;m.regen_id=(int)fixture_cases+1;m.flags=TF_WRAPXY|(keyed?TF_CHROMAKEYED:0);
    fixture_fail_regen=fail_regen;fixture_stream=fresh;
    if(source_kind==0){fixture_allocate(sizeof full,&m.data);memcpy(m.data,full,sizeof full);}
    if(source_kind==1){l0=cached_level(&m,0,full);l0->busy=1;HWR_PS2_LockData(l0->p);}
    if(!fresh)cached_level(&m,1,coarse);
    if(source_kind==3)fixture_want=2; /* Only cached level1 exists; requested level2 uses dsrc. */
    fixture_check(!scan_has_ff(coarse,sizeof coarse),"cached decimated image has no index255");
    int ri=tex_upload(&m);
    if(fail_regen)
    {
        fixture_check(ri==NOREC&&!m.downloaded&&!m.data,"failed full-source regeneration returns cleanly");
        fixture_check(tex_fail_why&&strcmp(tex_fail_why,"no full-size texture source")==0,"missing canonical source reason is precise");
        missing_regen_cases++;
    }
    else
    {
        fixture_check(ri!=NOREC,"planned failure falls back to full size");
        const texrec_t proxy=H.rec[ri];
        fixture_check(proxy.tiled==1&&!proxy.nblk&&proxy.w==256&&proxy.h==256&&!proxy.dx&&!proxy.dy,"metadata proxy retains exact full source dimensions");
        fixture_check(proxy.has255&&m.ps2_h255==2&&proxy.clut==(keyed?2:1),"canonical index255 classification replaces cached mip classification");
        fixture_check(proxy.shareable&&proxy.owner==&m&&!proxy.tile_source,"regenerable proxy owns no permanent CPU pixels");
        if(keyed)fixture_check((m.flags&TF_TRANSPARENT)!=0,"full-source keyed hole preserves transparency policy");
        fixture_check(H.used_blocks==512,"fallback does not steal GS blocks from pinned readers");
        if(l0)fixture_check(l0->busy==1&&fixture_blocks[fixture_block(l0->p)].locked,"full cache source keeps its older queued lease");
        for(int y=0;y<256;y+=62)for(int x=0;x<256;x+=62)
        {
            texrec_t tile;cap_reset();fixture_check(tile_load(&proxy,x,y,1,1,&tile),"full proxy remains drawable under the same pool pressure");
            inspect_uploaded_tile(&tile,full,x,y);
        }
        proxy_cases++;
    }
    for(unsigned i=0;i<DC_MAX;i++)if(dc[i].p&&dc[i].lvl>0)
        fixture_check(!fixture_blocks[fixture_block(dc[i].p)].locked,"failed fresh-flat mip releases its temporary CPU lock");
    if(fresh)fixture_check(fixture_rows==4,"new flat mip was actually built from four raw bands");
    end_case(&m);
}

static void fixed_policy_case(int cap)
{
    static u8 full[65536],coarse[16384];GLMipmap_t m;
    memset(full,41,sizeof full);memset(coarse,41,sizeof coarse);fixture_source=full;begin_case();
    memset(&m,0,sizeof m);m.width=m.height=256;m.format=GL_TEXFMT_P_8;m.regen_kind=1;m.regen_id=1000+fixture_cases;m.flags=TF_WRAPXY;
    fixture_allocate(sizeof full,&m.data);memcpy(m.data,full,sizeof full);cached_level(&m,1,coarse);
    if(cap)H.tex_cap_blocks=64;else cfg.tex_max_bytes=8192;
    int ri=tex_upload(&m);
    fixture_check(ri==NOREC&&!m.downloaded,"explicit cap or byte budget preserves failure policy");
    fixture_check(!fixture_regens,"explicit cap or byte budget never regenerates full source for fallback");
    if(!cap)fixture_check(fixture_errors==1&&!strcmp(tex_fail_why,"footprint over budget"),"explicit byte budget retains resource error");
    else fixture_check(!fixture_errors&&!strcmp(tex_fail_why,"no VRAM block (pool)"),"explicit fixed cap retains residency failure");
    policy_cases++;end_case(&m);
}

static void fresh_budget_case(int jump)
{
    static u8 full[65536];GLMipmap_t m;
    memset(full,41,sizeof full);fixture_source=full;begin_case();
    memset(&m,0,sizeof m);m.width=m.height=256;m.format=GL_TEXFMT_P_8;m.regen_kind=2;m.regen_id=2000+fixture_cases;m.flags=TF_WRAPXY;
    fixture_stream=1;cfg.tex_max_bytes=8192;jump_on_error=jump;require_unlocked_error=1;
    if(!setjmp(error_jump))
    {
        int ri=tex_upload(&m);
        fixture_check(!jump&&ri==NOREC,"explicit budget returns original failure when callback returns");
    }
    else fixture_check(jump,"budget callback performs expected nonlocal recovery");
    fixture_check(fixture_rows==4&&fixture_locks==1&&fixture_unlocks==1,"budget failure balances newly streamed level lease");
    fixture_check(fixture_errors==1&&!strcmp(tex_fail_why,"footprint over budget")&&!m.data&&!m.downloaded,
        "fresh-flat explicit budget preserves error and engine object state");
    policy_cases++;end_case(&m);
}

static void record_failure_case(int referenced)
{
    static u8 full[65536];GLMipmap_t m;
    memset(full,41,sizeof full);fixture_source=full;begin_case();
    memset(&m,0,sizeof m);m.width=m.height=256;m.format=GL_TEXFMT_P_8;m.regen_kind=2;m.regen_id=3000+fixture_cases;m.flags=TF_WRAPXY;
    fixture_stream=1;ps2hwd_dbg_flags=referenced?0:HWDBG_NOZC;
    /* Leave GS room but fail exactly the following texture-record growth. */
    tex_drop(1,0);H.rec_free=NOREC;H.rec_cap=H.rec_n;fixture_realloc_calls=0;fixture_realloc_fail=1;
    int ri=tex_upload(&m);fixture_realloc_fail=0;
    fixture_check(ri==NOREC&&!m.downloaded&&!strcmp(tex_fail_why,"record table")&&fixture_realloc_calls==1,
        "real record-allocation failure returns cleanly");
    fixture_check(H.used_blocks==64,"failed record allocation returns only its ordinary GS image");
    dcent_t *e=dc_find(dc_key(&m),128,128,1);fixture_check(e&&e->p,"newly streamed level remains a valid CPU cache entry");
    int locked=fixture_blocks[fixture_block(e->p)].locked;
    if(referenced)
    {
        fixture_check(e->busy>0&&locked,"failed record still protects queued zero-copy reader");
        dma_fence();fixture_check(!e->busy&&!fixture_blocks[fixture_block(e->p)].locked,
            "DMA retirement releases failed-record source after its reader finishes");
    }
    else fixture_check(!e->busy&&!locked,"common failure exit releases fresh-flat lock after copied upload");
    record_failures++;end_case(&m);
}

int main(void)
{
    cap=calloc(CAP_MAX,sizeof *cap);fixture_check(cap!=NULL,"packet sink allocation");
    for(int keyed=0;keyed<2;keyed++)for(int kind=1;kind<=2;kind++)
    {
        for(int source=0;source<4;source++)proxy_case(kind,keyed,source,0,0);
        proxy_case(kind,keyed,2,0,1);
    }
    for(int keyed=0;keyed<2;keyed++)
    {
        proxy_case(2,keyed,2,1,0);proxy_case(2,keyed,2,1,1);
    }
    fixed_policy_case(0);fixed_policy_case(1);
    fresh_budget_case(0);fresh_budget_case(1);
    record_failure_case(0);record_failure_case(1);
    free(H.rec);free(blk_owner_p);free(ovq_p);free(cutbuf_p);free(cap);free(scratch);
    printf("VRAM FALLBACK COMPLETE cases=%u proxies=%u missing_regen=%u explicit_policies=%u record_failures=%u exact_source_bytes=%u checks=%llu\n",fixture_cases,proxy_cases,missing_regen_cases,policy_cases,record_failures,exact_bytes,fallback_checks);
    return 0;
}
