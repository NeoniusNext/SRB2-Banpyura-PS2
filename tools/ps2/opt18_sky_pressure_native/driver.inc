/* Production allocator, original source pixels, actual copied GIF uploads and
 * original emit_piece boundary. The platform helper only limits the pool. */
#include "opt17/highres_tiles_native/driver.inc"

static qw_t sp_expected[9];
static u32 sp_trace_mode,sp_trace_words,sp_trace_wrong,sp_wide_negative;
static unsigned sp_saved;
static int sp_victim;
static u32 sp_anchor,sp_evictions,sp_old_wr;

static void sp_trace_vertex(const qw_t *first,const qw_t *last)
{
    if(!sp_trace_mode)return;
    if(last-first!=3){sp_trace_wrong++;return;}
    for(int i=0;i<3;i++)
    {
        qw_t value=first[i];
        if(i==2)value.w[2]&=0xffffffu; /* the private stencil high byte is outside Z24 */
        u32 at=sp_trace_words++%9u;
        if(sp_trace_mode==1)sp_expected[at]=value;
        else if(memcmp(&value,&sp_expected[at],sizeof value))sp_trace_wrong++;
    }
}

static void sp_wrong_row(void *ctx,u32 row,u8 *dst)
{
    const u32 width=(u32)(uintptr_t)ctx;
    for(u32 x=0;x<width;x++)dst[x]=(u8)(13u+((x*17u+row*23u)%181u));
}

static void sp_before_window_reader(int wide)
{
    if(wide&&sp_wide_negative&&tile_cache_borrowed_blocks()==64)
        upload_bands(H.rec[tile_cache_rec].blk,2,PSM_T8,128,256,1,sp_wrong_row,(void *)(uintptr_t)128);
}

int SP_Reserve(int pressure)
{
    tile_sky_pressure_reset();sp_wide_negative=0;
    if(!pressure)return 1;
    const int ri=tile_cache_get();
    if(ri==NOREC||H.rec[ri].nblk!=64||tex_usable_blocks()!=448)return 0;
    sp_anchor=H.rec[ri].blk;return 1;
}

int SP_Victim(int pressure,u32 *info)
{
    sp_victim=H.cur_tex;sp_evictions=H.st.evictions;
    if(sp_victim==NOREC)return 0;
    const texrec_t *r=&H.rec[sp_victim];
    info[0]=r->blk;info[1]=r->nblk;info[2]=H.used_blocks;
    return !r->tiled&&r->nblk==448&&(!pressure||(r->blk==H.pool_base
        &&r->blk+448u==sp_anchor&&H.used_blocks==512));
}

void SP_Enter(void){sp_old_wr=H.wr;sp_saved=tile_sky_pressure_enter();}

int SP_AfterSky(int pressure,u32 *info)
{
    info[0]=tile_cache_borrowed_blocks();info[1]=tile_cache_rec==NOREC?0:H.rec[tile_cache_rec].blk;
    info[2]=tile_cache_rec==NOREC?0:H.rec[tile_cache_rec].nblk;info[3]=H.st.evictions-sp_evictions;
    info[4]=H.wr;info[5]=sp_old_wr;info[6]=H.used_blocks;
    if(!pressure)return H.rec[sp_victim].used&&!info[3]&&!info[0];
    if(H.rec[sp_victim].used||info[0]!=64||info[1]!=sp_anchor-64||info[2]!=128||info[3]!=1||info[6]!=128)return 0;
    for(u32 b=sp_anchor-64;b<sp_anchor;b++)if(blk_owner[b]!=tile_cache_rec+1)return 0;
    return 1;
}

int SP_Leave(int pressure,u32 *info)
{
    tile_sky_pressure_leave(sp_saved);
    info[0]=tile_cache.sky_pressure;info[1]=tile_cache.sky_failed;
    info[2]=tile_cache_borrowed_blocks();info[3]=tex_usable_blocks();info[4]=H.used_blocks;
    return !info[0]&&!info[1]&&!info[2]&&(!pressure||(info[3]==448&&info[4]==64
        &&H.rec[tile_cache_rec].blk==sp_anchor&&H.rec[tile_cache_rec].nblk==64));
}

int SP_Overwrite(int pressure,u32 *info)
{
    const int ri=H.cur_tex;if(ri==NOREC)return 0;
    const texrec_t *r=&H.rec[ri];
    info[0]=r->blk;info[1]=r->nblk;info[2]=tile_cache_borrowed_blocks();info[3]=H.used_blocks;info[4]=H.wr;
    if(r->tiled||r->nblk!=448||info[2])return 0;
    if(!pressure)return 1;
    for(u32 b=sp_anchor-64;b<sp_anchor;b++)if(blk_owner[b]!=ri+1)return 0;
    return r->blk==H.pool_base&&r->blk+448u==sp_anchor&&info[3]==512;
}

void SP_Negative(int victim,int wide)
{
    sp_wide_negative=(u32)wide;
    if(victim)upload_bands(sp_anchor-64,2,PSM_T8,128,128,1,sp_wrong_row,(void *)(uintptr_t)128);
}

int SP_Draw(int reference,int sky,u32 *words,u32 *info)
{
    const float nx[2][3]={{-.94f,-.08f,-.54f},{.08f,.94f,.54f}};
    const float ny[3]={-.9f,-.8f,.9f},w[3]={1.0f,2.0f,1.5f},z[3]={.3f,.15f,.7f};
    const float color[3][4]={{220,80,50,255},{70,230,140,255},{100,130,250,255}};
    cv_t tri[3],saved[3];piece_t piece={0,0,-1,0};FSurfaceInfo surf;
    memset(&surf,0,sizeof surf);surf.PolyColor.rgba=0xffffffffu;
    if(!begin_draw(PF_Modulated|PF_Masked|PF_NoDepthTest|PF_Occlude,&surf)||P.npass!=1)return 0;
    P.vcol=1;P.vlight=0;P.fast=P.litfast=P.vuok=0;P.pal=0;P.oldpath=1;P.lmode=LM_NONE;
    memset(tri,0,sizeof tri);
    for(int i=0;i<3;i++)
    {
        tri[i].x=nx[sky][i]*w[i];tri[i].y=ny[i]*w[i];tri[i].z=z[i]*w[i];tri[i].w=w[i];
        const float u=sky?(i==1?0.0f:-16.0f):(i==1?120.25f:4.25f);
        const float v=sky?(i==0?256.0f:384.0f):(i==2?892.25f:772.25f);
        tri[i].a[0]=u/(float)P.r->sw;tri[i].a[1]=v/(float)P.r->sh;
        for(int c=0;c<4;c++)tri[i].a[2+c]=color[i][c];
    }
    memcpy(saved,tri,sizeof tri);
    const u32 fast=tile_core_fast_count(),boxes=tile_box_count(),masks=tile_stencil_mask_sprites;
    const u32 bytes=H.st.upload_bytes,uploads=H.st.uploads;
    if(!reference)memcpy(sp_expected,words,sizeof sp_expected);
    sp_trace_words=sp_trace_wrong=0;sp_trace_mode=reference?1:2;
    const u32 linear=P.linear;
    emit_piece(tri,3,&piece);sp_trace_mode=0;
    if(reference)memcpy(words,sp_expected,sizeof sp_expected);
    info[0]=sp_trace_words;info[1]=sp_trace_wrong;info[2]=memcmp(saved,tri,sizeof tri)!=0;
    info[3]=tile_core_fast_count()-fast;info[4]=tile_box_count()-boxes;info[5]=tile_stencil_mask_sprites-masks;
    info[6]=H.st.upload_bytes-bytes;info[7]=H.st.uploads-uploads;info[8]=linear;info[9]=H.wr;
    P.vcol=P.vlight=0;return 1;
}

int SP_ZRaw(u32 *dest){return PS2HWD_ReadVram(dest,H.z_addr/256u,H.fbw/64,PSM_Z32,0,0,H.fbw,H.vh);}
int TN_ZRaw(u32 *dest){return SP_ZRaw(dest);}
