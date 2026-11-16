/* Actual allocator, ownership, queued uploads/draws and pressure policy; no GS raster. */
#include <setjmp.h>
#define main capture_fixture_main_unused
#include "opt17/tile_capture_hosttest.c"
#undef main

typedef struct { void *data, *loops; int vertex_count; } test_sky_t;
static unsigned inner_calls, inner_saw_pressure, inner_fault;
static void hw_RenderSkyDome_inner(test_sky_t *sky)
{
    (void)sky; inner_calls++; inner_saw_pressure += tile_cache.sky_pressure == 1;
    if(inner_fault)hw_failure("fixture resource failure inside dome");
}
#include "sky-wrapper.inc"

typedef struct { GLMipmap_t fill,a,b,sky,view; int fi,ai,bi,si,vi; } fixture_t;

static void coverage(void)
{
    u8 seen[VRAM_BLOCKS]={0};u32 owned=0,last=H.pool_base;
    for(int i=0;i<H.free_n;i++)
    {
        u32 start=H.free_range[i].start,end=start+H.free_range[i].len;
        check(start>=last&&(!i||start>last)&&end>start&&end<=H.pool_base+H.pool_blocks,"sorted coalesced free ranges");last=end;
        for(u32 b=start;b<end;b++)check(!seen[b]++&&!blk_owner[b],"free block identity");
    }
    for(int i=0;i<H.rec_n;i++)if(H.rec[i].used&&H.rec[i].nblk&&H.rec[i].share==NOREC)
    {
        const texrec_t *r=&H.rec[i];check(r->blk>=H.pool_base&&(u32)r->blk+r->nblk<=H.pool_base+H.pool_blocks,"record inside pool");
        for(u32 b=r->blk;b<(u32)r->blk+r->nblk;b++){check(!seen[b]++&&blk_owner[b]==i+1,"physical owner identity");owned++;}
    }
    for(u32 b=H.pool_base;b<H.pool_base+H.pool_blocks;b++)check(seen[b]==1,"complete pool coverage");
    check(owned==H.used_blocks,"actual occupancy ledger");
}

static int image(GLMipmap_t *m,int w,int h,int value,int regen)
{
    memset(m,0,sizeof *m);m->width=w;m->height=h;m->format=GL_TEXFMT_P_8;m->regen_kind=regen;
    m->data=malloc((size_t)w*h);check(m->data!=NULL,"source pixels");memset(m->data,value,(size_t)w*h);
    const int ri=tex_upload(m);check(ri!=NOREC,"actual image admission");return ri;
}

static void init_fixture(fixture_t *f,int mixed)
{
    memset(f,0,sizeof *f);f->vi=NOREC;
    native_init(640,512,1);tile_sky_pressure_reset();ps2hwd_dbg_flags=0;sky_weights_calls=sky_cpu_frees=0;
    check(tile_cache_get()!=NOREC&&H.rec[tile_cache_rec].nblk==64&&tex_usable_blocks()==448,"original PAL16KiB policy");
    if(mixed)
    {
        f->fi=image(&f->fill,128,768,31,0);f->ai=image(&f->a,128,64,47,0);f->bi=image(&f->b,128,64,61,0);
        check(H.rec[f->fi].nblk==384&&H.rec[f->ai].nblk==32&&H.rec[f->bi].nblk==32,"mixed prefix dimensions");
        const u32 anchor=H.rec[tile_cache_rec].blk;
        check(H.rec[f->ai].blk==anchor-64&&H.rec[f->bi].blk==anchor-32,"two adjacent prefix owners");
    }
    else
    {
        f->ai=image(&f->a,128,896,0xd3,0);f->fi=f->bi=NOREC;
        check(H.rec[f->ai].nblk==448&&H.rec[f->ai].blk==H.pool_base,"complete ordinary448-block image");
    }
    f->si=image(&f->sky,1024,512,47,1);
    check(H.rec[f->si].tiled==1&&!H.rec[f->si].nblk&&H.used_blocks==512,"full-size sky is zero-block proxy");
    H.cur_tex=f->si;coverage();
}

static void cleanup(fixture_t *f)
{
    tile_sky_pressure_reset();batch_phase=0;H.cur_tex=NOREC;tex_free_all();
    check(H.used_blocks==0,"complete GPU cleanup");
    free(f->fill.data);free(f->a.data);free(f->b.data);free(f->sky.data);
}

static void triangle(cv_t tri[3],int sky)
{
    const float xy[3][2]={{-.8f,-.7f},{.75f,-.6f},{-.15f,.85f}};
    memset(tri,0,3*sizeof *tri);
    for(int i=0;i<3;i++)
    {
        tri[i].w=1.0f+.5f*i;tri[i].x=xy[i][0]*tri[i].w;tri[i].y=xy[i][1]*tri[i].w;tri[i].z=.125f*tri[i].w;
        tri[i].a[0]=sky?(i==1?0.0f:-16.0f/1024.0f):(i==1?.8f:.1f);
        tri[i].a[1]=sky?(i==0?.5f:.75f):(i==2?.8f:.1f);
    }
}

static void queued_pressure(void)
{
    fixture_t f;init_fixture(&f,0);piece_t pc={0,0,-1,0};cv_t t[3];
    const u32 anchor=H.rec[tile_cache_rec].blk,alloc_fail=TX.alloc_fail,evictions=H.st.evictions;
    H.cur_tex=f.ai;check(begin_draw(PF_NoDepthTest,NULL),"ordinary queued plan");triangle(t,0);emit_piece(t,3,&pc);
    const u32 old_end=cap_n;qw_t *old=malloc((size_t)old_end*sizeof *old);check(old!=NULL,"old queue snapshot");memcpy(old,cap,(size_t)old_end*sizeof *old);
    H.cur_tex=f.si;
    // Isolate this explicit context-off witness even when a deliberately
    // unsafe negative candidate already touched its memo during setup.
    tile_sky_pressure_reset();
    check(!tile_cache_try_borrow()&&H.rec[f.ai].used&&H.used_blocks==512&&!sky_weights_calls,"world borrow cannot pressure-evict");
    check(begin_draw(PF_NoDepthTest,NULL),"sky queued plan");triangle(t,1);
    const u32 before_vertices=H.st.verts_out;
    const unsigned saved=tile_sky_pressure_enter();
    tile_emit_triangle(t,&pc,f.si);
    check(tile_cache_borrowed_blocks()==64&&H.rec[tile_cache_rec].blk==anchor-64&&H.used_blocks==128,"SKY reclaims exact prefix under pressure");
    check(!H.rec[f.ai].used&&!f.a.downloaded&&H.st.evictions==evictions+1&&sky_weights_calls==1,"one approved physical owner dropped");
    check(!sky_cpu_frees&&f.a.data&&((u8 *)f.a.data)[0]==0xd3&&((u8 *)f.a.data)[114687]==0xd3,"pressure retains canonical CPU pixels");
    check(H.st.verts_out==before_vertices+3&&!memcmp(old,cap,(size_t)old_end*sizeof *old),"old queued upload and draw immutable");
    const u32 wide_end=cap_n;qw_t *wide=malloc((size_t)wide_end*sizeof *wide);check(wide!=NULL,"wide queue snapshot");memcpy(wide,cap,(size_t)wide_end*sizeof *wide);
    coverage();tile_sky_pressure_leave(saved);
    check(!tile_cache.sky_pressure&&!tile_cache.sky_failed&&!tile_cache_borrowed_blocks()
        &&H.rec[tile_cache_rec].blk==anchor&&H.used_blocks==64&&tex_usable_blocks()==448,"end dome restores permanent16KiB before ordinary allocation");
    GLMipmap_t ordinary={0};const int normal=image(&ordinary,128,896,0xa7,0);
    check(H.rec[normal].nblk==448&&H.rec[normal].blk==H.pool_base&&H.used_blocks==512,"ordinary448 reuses returned prefix");
    check(!memcmp(wide,cap,(size_t)wide_end*sizeof *wide)&&TX.alloc_fail==alloc_fail,"queued wide packets survive actual ordinary overwrite");
    pkt_t p;u64 blit=0;unsigned sky_images=0,new_images=0;u32 sky_image_at=0,sky_primitive_at=0;
    for(u32 at=0;at<cap_n&&pkt_at(at,&p);at+=1+p.size)
    {
        if(p.flg==0&&p.nreg==1&&(p.regs&15)==GIF_REGS_AD)
            for(u32 k=0;k<p.nloop;k++)if(cap[at+1+k].d[1]==GSR_BITBLTBUF)blit=cap[at+1+k].d[0];
        if(p.flg==2&&at>=old_end&&at<wide_end)
        {
            check(((blit>>32)&16383u)==anchor-64&&p.size*16u==32768,"borrowed sky IMAGE follows old draw");
            for(u32 b=0;b<p.size*16u;b++)check(((u8 *)&cap[at+1])[b]==47,"copied sky source bytes");
            sky_images++;sky_image_at=at;
        }
        if(at>=old_end&&at<wide_end)
        {
            const u32 prim=p.pre?p.prim:(p.flg==1&&(p.regs&15)==0?(u32)cap[at+1].d[0]:0);
            if(prim&PRIM_TME)sky_primitive_at=at;
        }
        if(p.flg==2&&at>=wide_end)
        {
            for(u32 b=0;b<p.size*16u;b++)check(((u8 *)&cap[at+1])[b]==0xa7,"ordinary overwrite copies follow wide reader");
            new_images++;
        }
    }
    check(sky_images==1&&sky_image_at>=old_end&&sky_primitive_at>sky_image_at&&new_images==2,"old reader then wide IMAGE/draw then overwrite ordering");
    for(u32 b=anchor-64;b<anchor;b++)check(blk_owner[b]==normal+1,"ordinary owns every returned prefix block");
    check(!tile_cache_try_borrow()&&H.rec[normal].used,"world draw still cannot pressure-evict after SKY");
    coverage();free(old);free(wide);cleanup(&f);free(ordinary.data);
    puts("SKYPRESS QUEUED old-reader->pressure-wide->return->ordinary-overwrite PASS CPU-source-retained=1");
}

static void protected_case(int which)
{
    fixture_t f;init_fixture(&f,1);const u32 used=H.used_blocks,evictions=H.st.evictions;
    if(which==0)H.rec[f.bi].pin=1;
    if(which==1){H.rec[f.bi].screen=3;H.rec[f.bi].stamp=H.frame_no;}
    if(which==2)H.cur_tex=f.bi;
    if(which==3){batch_phase=1;H.rec[f.ai].stamp=H.frame_no-1;H.rec[f.bi].stamp=H.frame_no;}
    if(which==4||which==5)
    {
        f.view.width=128;f.view.height=64;f.view.format=GL_TEXFMT_P_8;
        f.vi=tex_view(&f.view,f.bi);check(f.vi!=NOREC,"actual shared view creation");
        if(which==4)H.cur_tex=f.vi;
        else{batch_phase=1;H.rec[f.ai].stamp=H.rec[f.bi].stamp=H.frame_no-1;H.rec[f.vi].stamp=H.frame_no;}
    }
    if(which==6||which==7)
    {
        H.rec[f.ai].pnext=f.fi;H.rec[f.fi].ppar=f.ai;
        if(which==6)H.rec[f.fi].pin=1;else H.cur_tex=f.fi;
    }
    const unsigned saved=tile_sky_pressure_enter();const unsigned calls=sky_weights_calls;
    check(!tile_cache_try_borrow(),"protected closure refuses pressure borrow");
    check(H.rec[f.ai].used&&H.rec[f.bi].used&&H.rec[f.fi].used&&H.used_blocks==used&&H.st.evictions==evictions,
        "protected mixed-prefix preflight is atomic");
    if(which==4)check(H.rec[f.vi].used&&f.view.downloaded,"shared-current view and physical owner survive");
    if(which==6)check(H.rec[f.fi].pin&&H.rec[f.fi].used,"paired protected piece survives");
    check(sky_weights_calls==calls+1,"one pressure weights scan");
    for(int i=0;i<106;i++)check(!tile_cache_try_borrow(),"failed SKY prefix remains rejected");
    check(sky_weights_calls==calls+1,"failed prefix memo avoids repeated weight scans");
    tile_sky_pressure_leave(saved);check(!tile_cache.sky_pressure&&!tile_cache.sky_failed,"context leaves every protected early return");
    coverage();cleanup(&f);printf("SKYPRESS PROTECTED case=%d atomic=PASS memo106=PASS\n",which);
}

static void wrapper_cases(void)
{
    native_init(640,512,1);tile_sky_pressure_reset();check(tile_cache_get()!=NOREC&&tile_cache_try_borrow(),"prior ordinary free borrow");
    test_sky_t sky={NULL,(void *)1,3};const unsigned calls=inner_calls;
    hw_RenderSkyDome(NULL);hw_RenderSkyDome(&sky);
    sky.data=(void *)1;sky.vertex_count=0;hw_RenderSkyDome(&sky);
    check(inner_calls==calls&&tile_cache_borrowed_blocks()==64&&!tile_cache.sky_pressure,"invalid and empty dome keep prior borrow unchanged");
    sky.vertex_count=3;hw_RenderSkyDome(&sky);
    check(inner_calls==calls+1&&inner_saw_pressure&&!tile_cache.sky_pressure&&!tile_cache_borrowed_blocks(),"actual wrapper restores state after inner early return");
    unsigned outer=tile_sky_pressure_enter(),inner=tile_sky_pressure_enter();tile_cache.sky_failed=1;
    tile_sky_pressure_leave(inner);check(tile_cache.sky_pressure&&tile_cache.sky_failed,"nested failure memo propagates");
    tile_sky_pressure_leave(outer);check(!tile_cache.sky_pressure&&!tile_cache.sky_failed,"outer context restores original flags");
    inner=tile_sky_pressure_enter();H.up=0;tile_sky_pressure_reset();tile_sky_pressure_leave(inner);
    check(!tile_cache.sky_pressure&&!tile_cache.sky_failed,"shutdown/error reset cannot resurrect context");H.up=1;tex_free_all();
    puts("SKYPRESS WRAPPER invalid/empty/noop/early/nested/error PASS");
}

static jmp_buf recovery_jump;
static int recovery_nonlocal,recovery_calls,recovery_revoked;
static void recovery_callback(const char *message)
{
    (void)message;recovery_calls++;recovery_revoked+=H.up&&tile_cache.sky_pressure==2;
    if(recovery_nonlocal)longjmp(recovery_jump,1);
}

static void nonlocal_error(void)
{
    fixture_t f;init_fixture(&f,0);test_sky_t sky={(void *)1,(void *)1,3};
    recovery_calls=recovery_revoked=0;recovery_nonlocal=1;resource_error_handler=recovery_callback;inner_fault=1;
    if(setjmp(recovery_jump)==0)
    {
        hw_RenderSkyDome(&sky);
        check(0,"actual callback must leave through longjmp");
    }
    inner_fault=0;
    check(recovery_calls==1&&recovery_revoked==1&&H.up,"nonlocal callback sees revoked permission with active renderer");
    check(tile_cache.sky_pressure==2&&!tile_cache_try_borrow()&&H.rec[f.ai].used&&H.used_blocks==512,
        "world cannot pressure-evict after skipped wrapper cleanup");
    tile_sky_pressure_reset();recovery_nonlocal=0;
    const unsigned outer=tile_sky_pressure_enter(),inner=tile_sky_pressure_enter();
    hw_failure("fixture returning recovery callback");
    tile_sky_pressure_leave(inner);tile_sky_pressure_leave(outer);
    check(tile_cache.sky_pressure==2&&!tile_cache_try_borrow()&&H.rec[f.ai].used,
        "nested leave cannot restore aborted permission");
    // A reset during a returning callback must not let an old saved nested
    // scope re-enable permission in a still-active/reinitialized renderer.
    tile_sky_pressure_reset();const unsigned saved=tile_sky_pressure_enter();
    tile_sky_pressure_reset();tile_sky_pressure_leave(saved);
    check(!tile_cache.sky_pressure,"old scope cannot resurrect permission after reset");
    tile_sky_pressure_revoke();tex_drop(f.ai,0);
    check(tile_cache_try_borrow()&&tile_cache_borrowed_blocks()==64&&tile_cache.sky_pressure==2,
        "non-evicting free-prefix borrow survives revocation");
    check(tile_cache_return_borrow(),"ordinary return remains available after recovery");
    resource_error_handler=NULL;coverage();cleanup(&f);
    puts("SKYPRESS NONLOCAL actual-hw_failure->callback->longjmp active-H.up=1 world-denied/nested-safe/free-borrow PASS");
}

int main(void)
{
    cap=calloc(CAP_MAX,sizeof *cap);check(cap!=NULL,"packet recorder");
    struct {int valid,x,y,wrapx,wrapy,region;const void *data;const GLMipmap_t *owner;u32 frame,palette;u8 screen,psm;} old_cache;
    check(sizeof old_cache==sizeof tile_cache,"context fields consume existing cache padding");
    queued_pressure();for(int i=0;i<8;i++)protected_case(i);wrapper_cases();nonlocal_error();
    free(cap);printf("SKYPRESS COMPLETE checks=%llu failures=0\n",capture_checks);return 0;
}
