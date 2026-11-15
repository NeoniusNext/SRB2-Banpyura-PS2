/* Complete production PB collection, planner and sorted walk, with the actual
 * C driver texture planner and synchronous output capture. Target VU/DMA/GS
 * execution is a separate full-engine requirement, not emulated here. */
#include <assert.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include "driver-context.inc"

static FILE *events, *packets, *memory_log;
static unsigned case_no, frame_no, event_count, draw_count, plan_count;
static unsigned fail_soft, saw_soft_failure, fail_required, caught_required;
static unsigned fail_draw, fail_plan_end, unwind_count;
static unsigned fixture_errors, hard_errors, fixture_mode;
static uint64_t rng_state;
static jmp_buf required_jump;
static void check_pb_capacity(void);
static GLMipmap_t textures[320];
static GLMipmap_t *current_texture;
static int currently_batching, hwr_grec_on, hwr_sprite_batch, hwr_sprite_shadow;
UINT32 hwr_geo_off;
static UINT32 hwr_gv_off, hwr_scan_dir;
static int hwr_ph_on, hwr_ph_vis, leveltime;
static struct { int value; } cv_glshaders;
static int gl_shadersavailable;
typedef struct { union { int i; } value; } fixture_stat_t;
static fixture_stat_t ps_hw_numpolys, ps_hw_numcalls, ps_hw_numverts,
    ps_hw_numshaders, ps_hw_numtextures, ps_hw_numpolyflags, ps_hw_numcolors;
UINT32 ps2hwp_cnt[HWC_NUM];
unsigned long long ps2hwp_cyc[HWP_NUM];
static UINT32 hwr_plc[4], hwr_plcyc[4], hwr_plkey, hwr_plhit, hwr_plhitn;
UINT32 hwr_texsig;
#define SHADER_NONE (-1)
#define PS_START_TIMING(name) ((void)0)
#define PS_STOP_TIMING(name) ((void)0)
#define PU_HWRBATCH 25
#define PU_HWRCACHE_UNLOCKED 108
#define HWR_BatchAlloc(bytes) HWR_BatchResize(NULL, (bytes))

typedef struct { unsigned char *raw, *body; size_t bytes; unsigned active; } allocation_t;
static allocation_t allocations[32];
static size_t alloc_live, alloc_peak;
static unsigned alloc_calls, resize_calls, free_calls, guard_checks;
static const unsigned char guard_pattern = 0xA7;

static void ensure(int condition, const char *what)
{
    if (!condition) { fprintf(stderr, "PB ASSERT case=%u frame=%u %s\n", case_no, frame_no, what); exit(3); }
}
static void u32out(UINT32 value) { ensure(fwrite(&value,4,1,events)==1,"event write"); }
static void bytesout(const void *data,size_t bytes) { ensure(fwrite(data,1,bytes,events)==bytes,"event payload"); }
static void event(UINT32 type) { u32out(type); event_count++; }
static unsigned texture_id(const GLMipmap_t *m)
{
    if (!m) return 0;
    ensure(m>=textures && m<textures+320,"known texture identity");
    return (unsigned)(m-textures)+1;
}
static allocation_t *allocation(void *body)
{
    unsigned i;
    for(i=0;i<32;i++) if(allocations[i].active && allocations[i].body==body) return &allocations[i];
    ensure(0,"known allocation root"); return NULL;
}
static void check_guards(void)
{
    unsigned i,j;
    for(i=0;i<32;i++) if(allocations[i].active) {
        allocation_t *a=&allocations[i];
        for(j=0;j<32;j++) ensure(a->body[-32+(int)j]==guard_pattern && a->body[a->bytes+j]==guard_pattern,"allocation exterior guard");
        ensure(((uintptr_t)a->body&15u)==0,"allocation alignment");
    }
    guard_checks++;
}
static void fixture_free(void *body)
{
    allocation_t *a;
    if(!body) return;
    check_guards(); a=allocation(body); alloc_live-=a->bytes; free_calls++;
    memset(body,0xDD,a->bytes); free(a->raw); memset(a,0,sizeof *a);
}
static void *fixture_resize(void *old,size_t bytes,int soft)
{
    allocation_t *a=NULL; unsigned i; unsigned char *raw,*body;
    check_guards();
    if(soft && fail_soft) { fail_soft=0; saw_soft_failure++; return NULL; }
    if(!soft && fail_required && --fail_required==0) {
        hard_errors++; longjmp(required_jump,1);
    }
    if(old) a=allocation(old);
    if(!a) { for(i=0;i<32;i++) if(!allocations[i].active) { a=&allocations[i]; break; } }
    ensure(a!=NULL,"allocation slot");
    ensure(posix_memalign((void **)&raw,16,bytes+64)==0,"fixture host memory"); body=raw+32;
    memset(raw,guard_pattern,32); memset(body,0xCD,bytes); memset(body+bytes,guard_pattern,32);
    if(old) { size_t n=a->bytes<bytes?a->bytes:bytes; memcpy(body,old,n); }
    alloc_live+=bytes; if(alloc_live>alloc_peak) alloc_peak=alloc_live;
    if(old) { alloc_live-=a->bytes; memset(old,0xDD,a->bytes); free(a->raw); resize_calls++; }
    else alloc_calls++;
    a->raw=raw; a->body=body; a->bytes=bytes; a->active=1;
    return body;
}
static void *Z_TryReallocAlign(void *old,size_t bytes,int tag,void *owner,int alignment)
{
    ensure(tag==PU_HWRBATCH && !owner && alignment==4,"soft resize contract");
    return fixture_resize(old,bytes,1);
}
static void Z_Free(void *p) { fixture_free(p); }
static void *HWR_BatchResize(void *old,size_t bytes) { return fixture_resize(old,bytes,0); }
static void Z_ChangeTag(void *p,int tag) { (void)p; ensure(tag==PU_HWRCACHE_UNLOCKED,"cache tag"); }
static void I_Error(const char *format,...)
{
    (void)format; hard_errors++; longjmp(required_jump,1);
}
static void I_OutputMsg(const char *format,...)
{
    char buffer[4096]; va_list ap; int count;
    va_start(ap,format); count=vsnprintf(buffer,sizeof buffer,format,ap); va_end(ap);
    ensure(count>=0 && (size_t)count<sizeof buffer,"debug output bounds");
    event(0x44424700);u32out((UINT32)count);bytesout(buffer,(size_t)count);
}
static int HWR_ShaderOfTarget(int target) { return target==SHADER_NONE?target:target+16; }
static UINT32 HWR_PS2_TextureOrder(const GLMipmap_t *m)
{
    UINT32 id;
    if(!m) return 0x3FFEu;
    id=m->regen_kind==1?(UINT32)m->regen_id&0x1FFFu:m->regen_kind==2?0x2000u|((UINT32)m->regen_id&0x1FFFu):0x3FF0u;
    return hwr_scan_dir?0x3FFFu-id:id;
}

static void fixture_texture(GLMipmap_t *m)
{
    event(0x54455800); u32out(texture_id(m)); H.cur_tex=m?(int)texture_id(m)-1:NOREC;
    H.cur_missing=0; H.serial++;
}
static void fixture_shader(int shader)
{
    event(0x53484400);u32out((UINT32)shader);H.shader=shader;H.serial++;
}
static struct { void (*pfnSetTexture)(GLMipmap_t *); void (*pfnSetShader)(int); } HWD={fixture_texture,fixture_shader};

void PS2HWD_PlanBegin(void)
{
    check_guards();check_pb_capacity();event(0x504C4200); driver_PlanBegin();
}
void PS2HWD_PlanBlock(GLMipmap_t *m,const void *block)
{
    const qw_t *b=block; unsigned i,n=b[0].w[0];
    check_guards();ensure(n>=3 && n<=PS2HWD_MAXPOLY,"planned polygon size");
    event(0x504C5000);u32out(texture_id(m));u32out(n);
    for(i=0;i<n;i++) { bytesout(b[2+2*i].w,12);bytesout(b[3+2*i].w,8); }
    driver_PlanBlock(m,block); plan_count++;
}
void PS2HWD_PlanEnd(void)
{
    unsigned i; driver_PlanEnd(); event(0x504C4500);u32out((UINT32)plan_n);
    for(i=0;i<(unsigned)plan_n;i++) {
        u32out(texture_id(plan_info[i].key));bytesout(&plan_info[i].total,sizeof plan_info[i].total);
        bytesout(&plan_info[i].smax,sizeof plan_info[i].smax);u32out(plan_info[i].npoly);
        bytesout(plan_info[i].hist,sizeof plan_info[i].hist);
    }
    for(i=0;i<320;i++) { u32out(textures[i].ps2_want);u32out(textures[i].ps2_vis);u32out(textures[i].ps2_pi);u32out(textures[i].ps2_planfr); }
    check_guards();
    if(fail_plan_end) {fail_plan_end=0;unwind_count++;longjmp(required_jump,1);}
}
static void PS2HWD_BatchDraw(void) { event(0x42415400);driver_BatchDraw(); }
static void PS2HWD_TouchTexture(GLMipmap_t *m) { event(0x54434800);u32out(texture_id(m)); }
static int PS2HWD_PalLit(const void *surf,unsigned int flags,const void *m,int shader)
{
    const FSurfaceInfo *s=surf;
    return (fixture_mode&1) && s && m && shader>=0 && shader<=5 && (flags&PF_ColorMapped) && !(flags&(PF_NoTexture|PF_Ripple|PF_Invisible));
}
static void PS2HWD_DrawBlocks(void *surface,const void *pool,unsigned int count,unsigned int flags,const unsigned int *offsets)
{
    const qw_t *p=pool; const FSurfaceInfo *s=surface; unsigned i,j; UINT32 *copy; int draw;
    check_guards();check_pb_capacity();event(0x44524100);u32out(count);u32out(flags);u32out((UINT32)H.shader);
    u32out(H.cur_tex==NOREC?0u:(UINT32)H.cur_tex+1u);bytesout(s,sizeof *s);
    copy=malloc((size_t)count*sizeof *copy);ensure(copy!=NULL,"synchronous indices capture");
    memcpy(copy,offsets,(size_t)count*sizeof *copy);
    draw=begin_draw(flags,s);
    for(i=0;i<count;i++) {
        const qw_t *b=p+copy[i]; unsigned n=b[0].w[0];FOutVector v[PS2HWD_MAXPOLY];
        ensure(n>=3 && n<=PS2HWD_MAXPOLY,"draw polygon size");
        u32out(copy[i]);u32out(n);u32out(b[0].w[1]);bytesout(b[1].w,16);
        for(j=0;j<n;j++) {
            bytesout(b[2+2*j].w,12);bytesout(b[3+2*j].w,8);
            v[j].x=b[2+2*j].f[0];v[j].y=b[2+2*j].f[1];v[j].z=b[2+2*j].f[2];v[j].s=b[3+2*j].f[0];v[j].t=b[3+2*j].f[1];
        }
        if(draw) {
            if(b[0].f[1]!=P.rs.lp.light) {P.rs.lp.light=b[0].f[1];lit_fast_plan();}
            emit_fan(v,NULL,(int)n,NULL);
        }
        draw_count++;
    }
    ensure(memcmp(copy,offsets,(size_t)count*sizeof *copy)==0,"consumer copied indices before reuse");
    free(copy);check_guards();
    if(fail_draw && --fail_draw==0) {unwind_count++;longjmp(required_jump,1);}
}

/* These are unchanged cache-boundary collaborators. The complete PB record
 * producer/replay below is compiled and exercised; engine GC itself is not. */
typedef struct { UINT8 op,n,ext,fl;UINT16 size,state;INT32 bb[6];UINT32 light;float ex[4]; } gcbk_t;
#define GCOP_BLK 4
static UINT32 HWR_FboxOut(const INT32 *bb) { return bb[0]>10000?1u:0u; }
static int hwr_fbox_on;
static int HWR_FrPolyOutside(const FOutVector *v,unsigned int n) { (void)n;return v[0].x>10000.0f; }
static void HWR_PolyHashAdd(const FSurfaceInfo *s,const FOutVector *v,FUINT n,FBITFIELD flags,int target,boolean horizon)
{
    event(0x48415300);bytesout(s,sizeof *s);u32out((UINT32)n);u32out((UINT32)flags);u32out((UINT32)target);u32out((UINT32)horizon);bytesout(v,(size_t)n*sizeof *v);
}
static void HWR_GCRecBlk(UINT32 state,UINT32 light,const float *extent,UINT32 ext,const FOutVector *v,FUINT n)
{ (void)state;(void)v;(void)n;(void)light;(void)ext;(void)extent; }
static void HWR_GCRecPoly(const FSurfaceInfo *s,const FOutVector *v,FUINT n,FBITFIELD flags,int shader,boolean horizon)
{ (void)s;(void)v;(void)n;(void)flags;(void)shader;(void)horizon; }
static void HWR_RenderBatches(void);
static void HWR_StartBatching(void);
#include "batch-capacity.inc"
#include "production.inc"

static void HWR_StartBatching(void) { HWR_PBAlloc();currently_batching=1; }
static void HWR_RenderBatches(void) { HWR_PBRender(); }
static void check_pb_capacity(void)
{
    ensure(pb_n<=pb_ecap && pb_nplan<=pb_ecap,"entry capacity covers every polygon and planner entry");
    ensure(2u*(size_t)pb_ecap*sizeof(UINT32)<=allocation(pb_plan)->bytes,"planner backing has every full-width pair");
    ensure((size_t)pb_n*sizeof(UINT32)<=allocation(pb_plan)->bytes,"planner backing covers complete walk prefix");
}

static UINT32 random_word(void)
{
    rng_state^=rng_state<<13;rng_state^=rng_state>>7;rng_state^=rng_state<<17;
    return (UINT32)(rng_state>>16);
}
static void logical_state(void)
{
    unsigned i,j;
    event(0x53544100);u32out(pb_cap);u32out(pb_ecap);u32out(pb_bcap);u32out(pb_wr);u32out(pb_n);u32out(pb_nplan);u32out(pb_nb);u32out(pb_serial);
    u32out(pb_nogrow);u32out(pb_nfail);u32out(pb_nst);u32out(currently_batching);u32out(texture_id(current_texture));
    for(i=0;i<pb_nb;i++) {
        hwr_pbk_t *b=&pb_bk[i];u32out(texture_id(b->texture));bytesout(&b->surf,sizeof b->surf);
        u32out((UINT32)b->polyFlags);u32out((UINT32)b->shader);u32out((UINT32)b->shader_target);u32out(b->ex);u32out(b->lmask);
        u32out(b->head);u32out(b->tail);u32out(b->cnt);u32out(b->first);u32out(b->planwant);u32out(b->needext);
        bytesout(&pb_sk[i],sizeof pb_sk[i]);
    }
    for(i=0;i<pb_n;i++) {
        hwr_qw_t *b=pb_pool+pb_ent[i].blk;u32out(pb_ent[i].blk);u32out(pb_ent[i].next);
        u32out(b[0].w[0]);u32out(b[0].w[1]);bytesout(b[1].w,16);
        for(j=0;j<b[0].w[0];j++) {bytesout(b[2+2*j].w,12);bytesout(b[3+2*j].w,8);}
    }
    for(i=0;i<pb_nplan;i++) {
#ifdef NEXT12_SHARED
        u32out(HWR_PB_PLAN_BLK(i));u32out(HWR_PB_PLAN_BUCKET(i));
#else
        u32out(pb_plan[i].blk);u32out(pb_plan[i].bucket);
#endif
    }
    check_guards();
}
static void begin_case(unsigned id,unsigned mode)
{
    case_no=id;fixture_mode=mode;rng_state=0x9E3779B97F4A7C15ull+id;
    hwr_geo_off=hwr_gv_off=0;hwr_ph_on=hwr_ph_vis=hwr_fbox_on=0;
    hwr_sprite_batch=hwr_sprite_shadow=0;cv_glshaders.value=gl_shadersavailable=1;
    fail_soft=saw_soft_failure=fail_required=caught_required=0;
    event(0x43415300);u32out(id);u32out(mode);HWR_StartBatching();
}
static void finish_frame(void)
{
    logical_state();HWR_RenderBatches();logical_state();
    event(0x50414300);u32out(cap_n);ensure(fwrite(cap,16,cap_n,packets)==cap_n,"complete packet capture");
    cap_reset();H.frame_no++;frame_no++;leveltime=(int)(372+frame_no%5);hwr_scan_dir=H.frame_no&1u;
    HWR_StartBatching();
}
static void finish_case(void)
{
    finish_frame();currently_batching=0;HWR_PBFree();check_guards();
    fprintf(memory_log,"case=%u live=%zu peak=%zu alloc=%u resize=%u free=%u guards=%u softfail=%u hard=%u\n",case_no,alloc_live,alloc_peak,alloc_calls,resize_calls,free_calls,guard_checks,saw_soft_failure,caught_required);
    ensure(alloc_live==0,"all PB roots released");
}
static void polygon_input(unsigned index,unsigned count,unsigned style,int replay)
{
    FSurfaceInfo s;FOutVector vertices[192];unsigned k;UINT32 flags=PF_Occlude;int shader=(int)(index%6);boolean horizon=0;
    memset(&s,0,sizeof s);s.PolyColor.rgba=0xFFFFFFFFu-(style%7)*0x01010101u;
    s.TintColor.rgba=0x11223344u^style;s.FadeColor.rgba=0x90807060u^style;s.LightTableId=1+(style%31);
    s.LightInfo.light_level=32+(style%224);s.LightInfo.fade_start=style%32;s.LightInfo.fade_end=64+(style%128);
    current_texture=&textures[style%320];
    if(style&1) flags|=PF_ColorMapped;
    if(style%11==0) flags|=PF_NoTexture;
    if(style%17==0) {horizon=1;shader=SHADER_NONE;}
    if(style%19==0) flags|=PF_Translucent;
    if(style%23==0) flags|=PF_Ripple;
    if(style%29==0) flags|=PF_ForceWrapX;
    hwr_sprite_batch=(style%5==0);hwr_sprite_shadow=hwr_sprite_batch && (style%10==0);
    for(k=0;k<count;k++) {
        const float a=(float)k*(6.28318530718f/(float)count);
        vertices[k].x=cosf(a)*(2.0f+(float)(index%13))+(float)(index%23)-11.0f;
        vertices[k].y=sinf(a)*(1.0f+(float)(index%11));vertices[k].z=16.0f+(float)(index%43);
        if(style%13==0)vertices[k].z=-vertices[k].z;
        vertices[k].s=cosf(a)*(1.0f+(float)(style%5));vertices[k].t=sinf(a)*(1.0f+(float)(style%7));
    }
    if(replay && !(flags&PF_NoTexture) && !horizon) {
        unsigned char record[sizeof(gcbk_t)+192*20] __attribute__((aligned(16)));gcbk_t *h=(gcbk_t *)(void *)record;GLMipmap_t *last=NULL;UINT32 state;
        state=HWR_PBIntern(&s,current_texture,flags,shader,horizon);
        if(state!=HWR_ST_NONE && HWR_PBRoomFor(1,count)) {
            memset(h,0,sizeof *h);h->op=GCOP_BLK;h->n=(UINT8)count;h->size=(UINT16)(sizeof *h+count*20);h->state=(UINT16)state;
            h->light=(union{float f;UINT32 u;}){.f=(float)(int)s.LightInfo.light_level}.u;
            h->fl=(style%3==0);h->bb[0]=(style%7==0)?10001:0;
            if(style%7==0)for(k=0;k<count;k++)vertices[k].x+=20000.0f;
            memcpy(h+1,vertices,count*20);HWR_GCReplayRun(record,record+h->size,frame_no+1,&last);return;
        }
    }
    HWR_PBAdd(&s,vertices,count,flags,shader,horizon);
}
static void initialize_driver(void)
{
    unsigned i;
    cap=calloc(CAP_MAX,sizeof *cap);ensure(cap!=NULL,"packet capture memory");
    blk_owner_p=calloc(1,sizeof *blk_owner_p);ovq_p=calloc(1,sizeof *ovq_p);cutbuf_p=calloc(1,sizeof *cutbuf_p);plan_info_p=calloc(1,sizeof *plan_info_p);
    ensure(blk_owner_p && ovq_p && cutbuf_p && plan_info_p,"driver work memory");
    memset(&H,0,sizeof H);H.up=1;H.pool_blocks=12032;H.pool_base=4352;H.rec_free=NOREC;H.lru_head=H.lru_tail=NOREC;H.cur_tex=NOREC;
    H.fbw=640;H.fbh=448;H.fb32=1;H.fbpsm=PSM_CT32;H.vw=640;H.vh=448;H.shader=-1;H.shaders_on=1;
    H.sx=2.0f;H.sy=2.0f;H.scr_w=320;H.scr_h=200;H.z_addr=2293760u;
    col_lut_init();m_identity(H.mv);m_scale(H.mv,1.0f,1.0f,-1.0f);xf_set_2d(0.9f);viewport_set(0,0,320,200);cap_reset();
    for(i=0;i<320;i++) {
        GLMipmap_t *m=&textures[i];memset(m,0,sizeof *m);m->regen_kind=(UINT8)(1+i%3);m->regen_id=(INT32)i;
        m->format=(i%9==0)?GL_TEXFMT_RGBA:GL_TEXFMT_P_8;m->width=(UINT16)(i%4==0?128:256);m->height=256;
        m->flags=(i%3==0)?TF_WRAPXY:0;
    }
    H.rec=calloc(512,sizeof *H.rec);ensure(H.rec!=NULL,"resident metadata fixture");H.rec_cap=512;H.rec_n=320;
    for(i=0;i<320;i++) {
        texrec_t *r=&H.rec[i];GLMipmap_t *m=&textures[i];m->downloaded=i+1;
        r->owner=m;r->used=1;r->blk=H.pool_base+8*i;r->nblk=256;r->w=m->width;r->h=m->height;r->tw=(u8)(m->width==128?7:8);r->th=8;r->tbw=(u8)(m->width/64);
        r->psm=m->format==GL_TEXFMT_P_8?PSM_T8:PSM_CT32;r->clut=m->format==GL_TEXFMT_P_8?1:0;
        r->sw=r->w;r->sh=r->h;r->sxs=r->sys=1.0f;r->share=r->sec=r->sec_next=r->pnext=r->ppar=NOREC;
        r->prev=r->next=NOREC;
    }
}
int main(int argc,char **argv)
{
    unsigned i,c;void *states;
    ensure(argc==4,"events packets memory arguments");events=fopen(argv[1],"wb");packets=fopen(argv[2],"wb");memory_log=fopen(argv[3],"w");
    ensure(events && packets && memory_log,"output files");initialize_driver();
    /* Empty, one, every entry-capacity/growth boundary, then homogeneous and
     * heterogeneous batches, state-limit flushes and repeated frame/view use. */
    { const unsigned counts[]={0,1,2,3,511,512,1023,1024,1535,1536,1537,2303,2304,2305,3455,3456,3457,6000};
      for(c=0;c<sizeof counts/sizeof counts[0];c++) {
        begin_case(c,0);for(i=0;i<counts[c];i++)polygon_input(i,3,1,0);finish_case();
      }
    }
    begin_case(30,1);for(i=0;i<3000;i++) {polygon_input(i,3+i%13,i%127,0);if(i%311==310)finish_frame();}finish_case();
    begin_case(31,0);hwr_geo_off=0x200000u|256u|0x100000u;
    for(i=0;i<400;i++)polygon_input(i,3+i%9,i,0);finish_case();
    begin_case(32,0);fail_soft=1;for(i=0;i<5000;i++)polygon_input(i,4,i%37,0);ensure(saw_soft_failure==1,"soft grow refusal executed");finish_case();
    begin_case(33,1);states=calloc(1,HWR_PBStBytes());ensure(states!=NULL,"state table");HWR_PBStAttach(states);
    hwr_fbox_on=1;hwr_ph_on=hwr_ph_vis=1;
    for(i=0;i<1800;i++) {polygon_input(i,3+i%13,i%61,1);if(i%307==306)finish_frame();}
    HWR_PBStateFlush();HWR_PBNoCull(1);for(i=0;i<100;i++)polygon_input(i,4,i%11,1);HWR_PBNoCull(0);finish_case();free(states);
    begin_case(34,0);cv_glshaders.value=0;gl_shadersavailable=0;
    for(i=0;i<900;i++)polygon_input(i,3+i%23,i%53,0);finish_case();
    begin_case(35,0);for(c=0;c<8;c++) {fixture_mode=c&1;hwr_scan_dir=c&1;for(i=0;i<300+c*13;i++)polygon_input(i,3+i%31,(random_word()%83),0);finish_frame();}finish_case();
    begin_case(36,0);for(i=0;i<1800;i++)polygon_input(i,3+i%4,0,0);finish_case();
    begin_case(37,0);for(i=0;i<1300;i++)polygon_input(i,3+i%9,i%71,0);finish_case();
    for(c=0;c<2;c++) {
        begin_case(38+c,0);for(i=0;i<600;i++)polygon_input(i,3+i%9,i%71,0);
        if(c)fail_draw=2;else fail_plan_end=1;
        if(!setjmp(required_jump)) {finish_frame();ensure(0,"planner/draw unwind executed");}
        else {event(0x554E5700);u32out(c);}
        /* ForceSoftware sets collecting false then HWR_ReleaseBatching frees
         * PB before renderer switching or any later StartBatching/PlanPass. */
        currently_batching=0;HWR_PBFree();ensure(alloc_live==0,"unwound batch released");
        cap_reset();H.frame_no++;frame_no++;HWR_StartBatching();
        for(i=0;i<700;i++)polygon_input(i,3+i%7,i%31,0);finish_case();
    }
    /* A required allocation fails after growth began. The existing fatal/guard
     * path abandons the batch, then renderer release must free every root. */
    for(c=1;c<=2;c++) {
        begin_case(40+c,0);fail_required=c;
        if(!setjmp(required_jump)) {for(i=0;i<5000;i++)polygon_input(i,4,i%37,0);ensure(0,"required failure executed");}
        else {caught_required=1;event(0x46414900);u32out(c);}
        fail_required=0;currently_batching=0;HWR_PBFree();ensure(alloc_live==0,"abandoned roots released");check_guards();
    }
    for(c=1;c<=2;c++) {
        begin_case(45+c,0);fail_required=c;
        if(!setjmp(required_jump)) {for(i=0;i<700;i++)polygon_input(i,3,i,0);ensure(0,"bucket required failure executed");}
        else {caught_required=1;event(0x46414900);u32out(c);}
        fail_required=0;currently_batching=0;HWR_PBFree();ensure(alloc_live==0,"abandoned bucket roots released");check_guards();
    }
    fprintf(memory_log,"FINAL live=%zu peak=%zu alloc=%u resize=%u free=%u guards=%u\n",alloc_live,alloc_peak,alloc_calls,resize_calls,free_calls,guard_checks);
    ensure(unwind_count==2,"planner and post-walk unwind count");
    printf("PB COMPLETE cases=32 frames=%u events=%u planned=%u drawn=%u requiredfail=%u unwind=%u guards=%u\n",frame_no,event_count,plan_count,draw_count,hard_errors,unwind_count,guard_checks);
    fclose(events);fclose(packets);fclose(memory_log);free(cap);free(blk_owner_p);free(ovq_p);free(cutbuf_p);free(plan_info_p);
    free(H.rec);free(scratch);return fixture_errors?1:0;
}
