/* Actual V9 dispatch, copied GIF records and ordinary allocation; no GS raster. */
#define main capture_fixture_main_unused
#include "tile_capture_hosttest.c"
#undef main

static void row_overwrite(void *ctx,u32 y,u8 *out)
{
    (void)ctx;(void)y;memset(out,0xD3,128);
}

static void reset_counts(void)
{
    tile_stencil_reset_state();tile_core_fast_triangles=tile_box_nodes=tile_pixel_fallbacks=0;
    tile_stencil_rectangles=tile_stencil_mask_sprites=0;tile_run_peak_bytes=0;
    tile_stencil_triangle_submissions=tile_stencil_bbox_pixels=0;
    H.st.uploads=H.st.upload_bytes=0;cap_reset();
}

static void triangle(cv_t t[3],const texrec_t *r,int screen)
{
    const float xy[3][2]={{-.8f,-.7f},{.75f,-.6f},{-.15f,.85f}};
    const float u[3]={80,205,80},v[3]={100,100,325};
    const float vs[3]={70,70,131};
    memset(t,0,3*sizeof *t);
    for(int i=0;i<3;i++)
    {
        t[i].w=1.0f+.5f*i;t[i].x=xy[i][0]*t[i].w;t[i].y=xy[i][1]*t[i].w;t[i].z=.125f*t[i].w;
        t[i].a[0]=u[i]/r->sw;t[i].a[1]=(screen?vs[i]:v[i])/r->sh;
    }
}

static int create_source(int screen,GLMipmap_t *m,u8 **owned)
{
    int ri;
    native_init(640,512,1);memset(&cfg,0,sizeof cfg);cfg.screen_max_bytes=8u*1024u*1024u;
    ps2hwd_dbg_flags=HWDBG_NOPLAN;memset(m,0,sizeof *m);*owned=NULL;
    if(screen)
    {
        fill_frame(3);check(scr_capture(0),"wide screen capture source");ri=H.scr_rec[0];
    }
    else
    {
        m->width=1024;m->height=512;m->format=GL_TEXFMT_P_8;m->flags=TF_WRAPXY;m->regen_kind=1;
        *owned=malloc(1024u*512u);check(*owned!=NULL,"wide source allocation");memset(*owned,47,1024u*512u);m->data=*owned;
        ri=tex_upload(m);
    }
    check(ri!=NOREC&&H.rec[ri].tiled==1,"wide source is virtual");
    check(tile_cache_get()!=NOREC,"permanent image exists before optional borrow");
    check(H.pool_blocks==512&&H.used_blocks==64&&tex_usable_blocks()==448,"PAL permanent capacity");
    return ri;
}

static void queued_reclaim(int screen)
{
    GLMipmap_t m,normal={0};u8 *owned;cv_t t[3];piece_t pc={0,0,-1,0};
    const int ri=create_source(screen,&m,&owned);const u32 anchor=H.rec[tile_cache_rec].blk;
    const u32 failed_before=TX.alloc_fail,evicted_before=H.st.evictions;
    H.cur_tex=ri;check(begin_draw(PF_NoDepthTest,NULL),"wide plan");triangle(t,&H.rec[ri],screen);
    int x,y;check(!tile_one_core(t,&pc,&H.rec[ri],&x,&y),"case must reject original V8 one-window path");
    check(tile_one_wide(t,&pc,&H.rec[ri],&x,&y),"case fits optional wide window");reset_counts();
    tile_emit_triangle(t,&pc,ri);
    check(tile_core_fast_count()==1&&tile_box_count()==0&&tile_workspace_peak()==0,"wide bypasses classifier and workspace");
    check(!tile_stencil_mask_sprites&&H.st.uploads==1&&H.st.upload_bytes==32768,"one wide image and zero masks");
    check(tile_cache_borrowed_blocks()==64&&H.rec[tile_cache_rec].nblk==128,"borrow is actual physical occupancy");
    check(H.rec[tile_cache_rec].blk==anchor-64&&H.used_blocks==128&&tex_usable_blocks()==448,"borrow does not lower ordinary capacity");
    u32 image_at=~0u,primitive_at=~0u;u64 blit=0,reg=0,tex0=0;pkt_t p;
    for(u32 at=0;at<cap_n&&pkt_at(at,&p);at+=1+p.size)
    {
        if(p.flg==0&&p.nreg==1&&(p.regs&15)==GIF_REGS_AD)
            for(u32 k=0;k<p.nloop;k++)
            {
                const qw_t q=cap[at+1+k];
                if(q.d[1]==GSR_BITBLTBUF)blit=q.d[0];if(q.d[1]==GSR_TRXREG)reg=q.d[0];if(q.d[1]==GSR_TEX0_1)tex0=q.d[0];
            }
        if(p.flg==2&&((blit>>32)&16383u)>=H.pool_base)
        {
            check(image_at==~0u&&p.size*16u==32768,"single copied 32KiB IMAGE record");image_at=at;
            check(((blit>>32)&16383u)==anchor-64&&((blit>>48)&63u)==2,"wide image uploads to borrowed prefix with TBW2");
            check((reg&4095u)==128&&(reg>>32)==(screen?64u:256u),"wide transfer has the non-square shape");
            if(!screen)for(u32 b=0;b<32768;b++)check(((u8 *)&cap[at+1])[b]==47,"copied source byte before ordinary reuse");
        }
        if(p.pre&&(p.prim&16u)&&p.nloop>=3)
        {
            check((tex0&16383u)==anchor-64,"queued texture reference keeps borrowed base");
            if(primitive_at==~0u)primitive_at=at;
        }
    }
    check(image_at!=~0u&&primitive_at>image_at&&primitive_at<cap_n,"old image precedes old wide primitive");
    const u32 old_end=cap_n;qw_t *snapshot=malloc((size_t)old_end*sizeof *snapshot);check(snapshot!=NULL,"queued record snapshot");memcpy(snapshot,cap,(size_t)old_end*sizeof *snapshot);
    if(owned)memset(owned,0xEE,1024u*512u);
    normal.width=128;normal.height=896;normal.format=GL_TEXFMT_P_8;normal.data=malloc(128u*896u);check(normal.data!=NULL,"ordinary source");memset(normal.data,0xD3,128u*896u);
    // No fixture flush, fence or readback separates the old wide draw and reuse.
    const int ordinary=tex_upload(&normal);
    check(ordinary!=NOREC&&H.rec[ordinary].nblk==448,"ordinary admission reclaims borrowed blocks");
    check(TX.alloc_fail==failed_before&&H.st.evictions==evicted_before,"reclaim precedes failure accounting and victims");
    check(!tile_cache_borrowed_blocks()&&H.used_blocks==512&&H.rec[tile_cache_rec].blk==anchor,"ordinary reuse restores permanent anchor");
    check(!memcmp(snapshot,cap,(size_t)old_end*sizeof *snapshot),"old queued image and vertex records remain immutable");
    int new_images=0;
    for(u32 at=old_end;at<cap_n&&pkt_at(at,&p);at+=1+p.size)
        if(p.flg==2){for(u32 b=0;b<p.size*16u;b++)check(((u8 *)&cap[at+1])[b]==0xD3,"ordinary overwrite follows old records");new_images++;}
    check(new_images==2,"ordinary overwrite uses the existing two bands");
    for(u32 b=anchor-64;b<anchor;b++)check(blk_owner[b]==ordinary+1,"ordinary image owns every returned prefix block");
    check(!tile_cache_try_borrow()&&H.rec[ordinary].used&&normal.downloaded==(u32)ordinary+1,"occupied prefix cannot be borrowed or evicted");
    check(TX.alloc_fail==failed_before&&H.st.evictions==evicted_before,"failed optional claim has no ordinary side effects");
    // The exact original classifier remains usable while the prefix is occupied.
    H.cur_tex=ri;check(begin_draw(PF_NoDepthTest,NULL),"fallback plan");reset_counts();tile_emit_triangle(t,&pc,ri);
    check(!tile_core_fast_count()&&tile_box_count()&&tile_workspace_peak()&&tile_stencil_mask_sprites,"blocked prefix uses V8 classifier");
    check(!tile_cache_borrowed_blocks()&&H.rec[ordinary].used,"fallback preserves occupied prefix");
    u64 target=0;int narrow_images=0;
    for(u32 at=0;at<cap_n&&pkt_at(at,&p);at+=1+p.size)
    {
        if(p.flg==0&&p.nreg==1&&(p.regs&15)==GIF_REGS_AD)
            for(u32 k=0;k<p.nloop;k++)if(cap[at+1+k].d[1]==GSR_BITBLTBUF)target=cap[at+1+k].d[0];
        if(p.flg==2&&((target>>32)&16383u)>=H.pool_base){check(((target>>32)&16383u)==anchor,"fallback uploads only the permanent anchor");check(p.size*16u==16384,"fallback image stays16KiB");narrow_images++;}
    }
    check(narrow_images>0&&H.used_blocks==512&&tex_usable_blocks()==448,"fallback preserves capacity ledger");
    free(snapshot);tex_free_all();free(normal.data);free(owned);
    printf("WIDE QUEUED screen=%d image_before_draw_before_overwrite=PASS ordinary448=PASS fallback=PASS\n",screen);
}

int main(void)
{
    cap=calloc(CAP_MAX,sizeof *cap);check(cap!=NULL,"wide packet recorder");queued_reclaim(0);queued_reclaim(1);
    printf("WIDE DRAW COMPLETE checks=%llu failed=0\n",capture_checks);return 0;
}
