#define main base_suite_main
#include "base_hosttest.c"
#undef main
unsigned int hwr_geo_off;
#include "actual-plan.inc"
static int settex_calls;
static void settex_now(GLMipmap_t *m) { (void)m; settex_calls++; }
static unsigned checks, failures;
#define PC(c) do { checks++; if (!(c)) { failures++; printf("PLAN FAIL line%d: %s\n",__LINE__,#c); } } while (0)

static void image(GLMipmap_t *m, unsigned w, unsigned h, int tiled)
{
    int id;
    memset(m,0,sizeof *m);m->width=w;m->height=h;m->format=GL_TEXFMT_P_8;m->regen_kind=1;
    id=rec_new();H.rec[id].owner=m;H.rec[id].tiled=(u8)tiled;H.rec[id].w=H.rec[id].sw=(u16)w;H.rec[id].h=H.rec[id].sh=(u16)h;
    H.rec[id].nblk=tiled ? 0 : tex_plan_blocks(PSM_T8,w,h,0,0);m->downloaded=(u32)id+1;
}

static unsigned run_plan(int sky_kind, unsigned *sum, unsigned *exact)
{
    static GLMipmap_t sky,world;
    planinfo_t *pi;
    host_init(1);plan_reset();H.pool_blocks=512;H.pool_base=VRAM_BLOCKS-512;H.plan_pct=75;H.plan_reserve=0;plan_nofid=1;
    H.free_range[0].start=H.pool_base;H.free_range[0].len=H.pool_blocks;
    PC(tile_cache_get()!=NOREC);PC(tex_usable_blocks()==448); // one real 16 KiB reservation
    image(&world,256,256,0);
    if(sky_kind)image(&sky,1024,512,sky_kind==1);
    PS2HWD_PlanBegin();pi=plan_key(&world);PC(pi!=NULL);plan_add(pi,.65f,10000.0f);
    if(sky_kind){H.imm_tex=&sky;actual_imm_prepare(NULL,0);PC(settex_calls>0);PC(sky.ps2_full_fr==plan_stamp());}
    PS2HWD_PlanEnd();*sum=H.plan_last;*exact=H.plan_exact;
    PC(H.pool_blocks==512&&tex_usable_blocks()==448);
    return world.ps2_want;
}

static void carried_transition(void)
{
    static GLMipmap_t sky,world;
    planinfo_t *pi;
    host_init(1);plan_reset();H.pool_blocks=512;H.pool_base=VRAM_BLOCKS-512;H.plan_pct=75;H.plan_reserve=0;plan_nofid=1;
    H.free_range[0].start=H.pool_base;H.free_range[0].len=H.pool_blocks;
    PC(tile_cache_get()!=NOREC);image(&world,512,512,0);image(&sky,1024,512,1);
    PS2HWD_PlanBegin();pi=plan_key(&world);PC(pi!=NULL);plan_add(pi,.65f,10000.0f);
    // Previous full sky becomes measured immediate geometry while its old
    // force-full hint remains live, and is absent from this batch's plan_n.
    sky.ps2_full_fr=plan_stamp();imm_nprev=1;imm_prev[0]=&sky;imm_ptotal[0]=1000.0f;imm_pseps[0]=.75f;
    PC(plan_n==1);PC(plan_blocks(&sky,0)==0&&plan_blocks(&sky,1)==512);
    PS2HWD_PlanEnd();PC(sky.ps2_ihint==0);PC(world.ps2_want==1);PC(H.plan_exact==1024);PC(H.plan_last==256);
    printf("PLAN CARRIED live_full_virtual=1 density=.75 world_level=%u carried_level=%u exact=%u planned=%u\n",world.ps2_want,sky.ps2_ihint,H.plan_exact,H.plan_last);
}

int main(void)
{
    unsigned base_sum,base_exact,virtual_sum,virtual_exact,ordinary_sum,ordinary_exact;
    unsigned base_lv,virtual_lv,ordinary_lv;
    GLMipmap_t m,twin,view;
    texrec_t *r;
    plan_info_p=calloc(1,sizeof *plan_info_p);if(!plan_info_p)return 2;
    base_lv=run_plan(0,&base_sum,&base_exact);
    virtual_lv=run_plan(1,&virtual_sum,&virtual_exact);
    ordinary_lv=run_plan(2,&ordinary_sum,&ordinary_exact);
    PC(base_lv==0);PC(virtual_lv==base_lv);PC(virtual_sum==base_sum);PC(virtual_exact==base_exact);PC(ordinary_lv>base_lv);PC(ordinary_exact==base_exact+2048);
    printf("PLAN WITNESS pool=512 scratch=64 usable=448 budget=300 world=%u/%u/%u exact=%u/%u/%u planned=%u/%u/%u (alone/virtual-fullsky/ordinary-fullsky)\n",base_lv,virtual_lv,ordinary_lv,base_exact,virtual_exact,ordinary_exact,base_sum,virtual_sum,ordinary_sum);
    image(&m,1024,512,1);m.ps2_full_fr=plan_stamp();PC(plan_blocks(&m,0)==0);PC(plan_blocks(&m,1)==512);
    m.ps2_full_fr=0;PC(plan_blocks(&m,0)==2048);
    m.ps2_full_fr=plan_stamp();plan_advance(PLAN_FULL_KEEP);PC(plan_blocks(&m,0)==2048);
    m.ps2_full_fr=plan_stamp();r=rec_of(&m);r->tiled=0;r->nblk=2048;PC(plan_blocks(&m,0)==2048);r->tiled=1;r->nblk=0;
    memset(&twin,0,sizeof twin);twin.width=1024;twin.height=512;twin.ps2_full_fr=plan_stamp();twin.nextcolormap=&m;m.ps2_twin=&twin;PC(plan_blocks(&twin,0)==0);PC(plan_blocks(&m,0)==0);
    image(&view,1024,512,0);view.ps2_twin=&twin;view.ps2_full_fr=plan_stamp();H.rec[view.downloaded-1].share=(int)m.downloaded-1;PC(plan_blocks(&view,0)==0);
    r->used=0;m.downloaded=0;view.downloaded=0;PC(plan_blocks(&twin,0)==2048); // no stale metadata exemption
    image(&m,128,128,0);m.ps2_full_fr=plan_stamp();PC(plan_blocks(&m,0)==tex_plan_blocks(PSM_T8,128,128,0,0));
    carried_transition();
    printf("PLAN COMPLETE checks=%u failed=%u\n",checks,failures);
    return failures!=0;
}
