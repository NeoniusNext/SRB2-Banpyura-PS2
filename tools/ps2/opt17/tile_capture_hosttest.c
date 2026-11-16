/* Production capture, upload and sprite code through the maintained host GS interface. */
#include "base_hosttest.c"
unsigned int hwr_geo_off;

static unsigned long long capture_checks;
static unsigned tile_blocks(void) { return TILE_SIZE*TILE_SIZE*4u/256u; }
static unsigned cores(int w,int h) { return (unsigned)(((w+TILE_CORE-1)/TILE_CORE)*((h+TILE_CORE-1)/TILE_CORE)); }
static void check(int condition, const char *what)
{
    capture_checks++;
    if (!condition) { fprintf(stderr, "CAPTURE FAIL %s (check %llu)\n",what,capture_checks); exit(1); }
}
static u32 pattern(int x,int y,int version)
{
    return (u32)x | ((u32)y<<10) | ((u32)version<<20) | ((u32)((x*17+y*31+version*67)&255)<<24);
}
static void native_init(int w,int h,int fb32)
{
    u32 pages,fb;
    host_init(fb32); tile_cache_rec=NOREC; tile_invalidate();
    H.fbw=(w+63)&~63;H.vw=w;H.fbh=H.vh=h;H.scr_w=w;H.scr_h=h;H.sx=H.sy=1;
    pages=fb_pages(H.fbw,h,fb32);fb=pages*8192u;
    H.fb_addr[0]=0;H.fb_addr[1]=fb;H.z_addr=fb*2;
    H.clut_base=(H.z_addr+fb_pages(H.fbw,h,1)*8192u)/256u;
    H.pool_base=H.clut_base+512u;H.pool_blocks=VRAM_BLOCKS-H.pool_base;
    H.free_n=1;H.free_range[0].start=H.pool_base;H.free_range[0].len=H.pool_blocks;
    H.target=1;cfg.screen_max_bytes=8u*1024u*1024u;cfg.tex_max_bytes=0;
    viewport_set(0,0,w,h);cap_reset();
}
static void fill_frame(int version)
{
    for(int y=0;y<H.vh;y++)for(int x=0;x<H.fbw;x++)
        if(H.fb32)((u32 *)(void *)(mock_vram+H.fb_addr[H.target]))[y*H.fbw+x]=pattern(x,y,version);
        else ((u16 *)(void *)(mock_vram+H.fb_addr[H.target]))[y*H.fbw+x]=(u16)pattern(x,y,version);
}

/* Decode packet IMAGE rows and exact local transfers independently of the driver. */
static int identity_pixels(void)
{
    u8 tile[TILE_SIZE*TILE_SIZE*4];u8 *rebuilt=calloc(1,scr_bytes());u8 *cover=calloc((size_t)H.vw*H.vh,1);
    pkt_t p;u64 blit=0,pos=0,reg=0;unsigned at,copies=0,images=0;int bpp=H.fb32?4:2;
    check(rebuilt&&cover,"identity oracle allocation");memset(tile,0,sizeof tile);
    for(at=0;at<cap_n&&pkt_at(at,&p);at+=1+p.size)
    {
        if(p.flg==2)
        {
            int w=(int)(reg&4095u),h=(int)(reg>>32),dy=(int)((pos>>48)&2047u);
            check(w==TILE_SIZE&&dy+h<=TILE_SIZE&&p.nloop*16u==(unsigned)(w*h*bpp),"bounded raw tile payload");
            memcpy(tile+(size_t)dy*TILE_SIZE*bpp,&cap[at+1],(size_t)w*h*bpp);images++;
        }
        else if(p.flg==0&&p.nreg==1&&(p.regs&15)==GIF_REGS_AD)
            for(unsigned k=0;k<p.nloop;k++)
            {
                qw_t q=cap[at+1+k];u64 value=q.d[0];unsigned id=(unsigned)q.d[1];
                if(id==GSR_BITBLTBUF)blit=value;
                if(id==GSR_TRXPOS)pos=value;
                if(id==GSR_TRXREG)reg=value;
                if(id==GSR_TRXDIR&&value==2)
                {
                    int sx=(int)(pos&2047u),sy=(int)((pos>>16)&2047u),dx=(int)((pos>>32)&2047u),dy=(int)((pos>>48)&2047u);
                    int w=(int)(reg&4095u),h=(int)(reg>>32);
                    check(sx==1&&sy==1&&w>0&&h>0&&w<=TILE_CORE&&h<=TILE_CORE,"copy consumes only the halo tile core");
                    check((blit&16383u)==H.rec[tile_cache_rec].blk,"copy source is transient tile VRAM");
                    check(((blit>>32)&16383u)==H.fb_addr[H.target]/256u&&dx+w<=H.vw&&dy+h<=H.vh,"copy destination is current target");
                    for(int y=0;y<h;y++)for(int x=0;x<w;x++)
                    {
                        check(!cover[(dy+y)*H.vw+dx+x]++,"identity pixel copied once");
                        memcpy(rebuilt+((size_t)(dy+y)*H.fbw+dx+x)*bpp,tile+((size_t)(sy+y)*TILE_SIZE+sx+x)*bpp,(size_t)bpp);
                    }
                    copies++;
                }
            }
    }
    for(int y=0;y<H.vh;y++)for(int x=0;x<H.vw;x++)check(cover[y*H.vw+x]==1,"identity full pixel coverage");
    check(memcmp(rebuilt,H.scr_data[0],scr_bytes())==0,"identity retained all native bits including alpha");
    check(copies==cores(H.vw,H.vh)&&images==copies,"one bounded tile upload per core");
    free(rebuilt);free(cover);return (int)copies;
}

static void capture_and_identity(int w,int h,int fb32)
{
    u32 bytes,used,upload;void *backing;int ri;
    native_init(w,h,fb32);if(!fb32){H.pool_blocks=64;H.free_range[0].len=64;}bytes=scr_bytes();check(fb_pages(H.fbw,H.fbh,H.fb32)*PAGE_BLOCKS>H.pool_blocks,"full capture genuinely exceeds pool");
    fill_frame(1);check(scr_capture(0),"native direct capture succeeds");ri=H.scr_rec[0];
    check(ri!=NOREC&&H.rec[ri].tiled==1&&H.rec[ri].nblk==0&&H.rec[ri].blk==0,"capture retained as zero-block source proxy");
    check(H.rec[ri].w==w&&H.rec[ri].h==h&&H.rec[ri].sw==w&&H.rec[ri].sh==h,"capture full dimensions preserved");
    check(H.used_blocks==0&&H.screen_bytes==bytes&&H.screen_peak_bytes==bytes,"direct capture footprint accounting");
    check(!memcmp(H.scr_data[0],mock_vram+H.fb_addr[1],bytes),"captured current target, every native byte");
    {
        u8 *rgb=malloc((size_t)w*h*3);
        check(rgb!=NULL,"RGB oracle allocation");upload=H.st.screen_restores;
        check(PS2HWD_ReadScreenRGB(0,rgb)&&H.st.screen_restores==upload,"EE capture RGB readback avoids GS restore");
        for(int y=0;y<h;y++) for(int x=0;x<w;x++)
        {
            u32 pixel=pattern(x,y,1),expanded=expand_pixel(fb32?pixel:(u16)pixel,fb32);
            for(int ch=0;ch<3;ch++)check(rgb[((size_t)y*w+x)*3+ch]==(u8)(expanded>>(ch*8)),"RGB readback preserves native size and selected slot");
        }
        free(rgb);
    }
    upload=H.st.screen_restores;used=H.used_blocks;cap_reset();check(scr_resident(0)==ri&&H.st.screen_restores==upload&&H.used_blocks==used&&!cap_n,"proxy residency needs no whole image restore");
    check(!scr_spill_victim()&&H.scr_rec[0]==ri,"zero-block capture never a spill victim");
    cfg.screen_max_bytes=bytes;check(!scr_capture(1)&&H.scr_rec[1]==NOREC&&H.screen_bytes==bytes,"budget rejection retains previous capture and frees empty proxy");
    cfg.screen_max_bytes=bytes*2;mock_read_fail=1;check(!scr_capture(1)&&H.scr_rec[1]==NOREC&&H.screen_bytes==bytes,"failed direct readback frees provisional EE allocation");mock_read_fail=0;
    cap_reset();draw_screen_tex(0,NULL,PF_NoDepthTest,1,1);
    int copies=identity_pixels();check(H.used_blocks==tile_blocks()&&H.st.screen_restores==upload,"only bounded GS tile needed for exact identity presentation");
    texrec_t tile;backing=H.scr_data[0];check(tile_load(&H.rec[ri],0,0,0,0,&tile),"prime same-frame tile cache");
    fill_frame(2);check(scr_capture(0)&&H.scr_data[0]==backing&&H.screen_bytes==bytes&&H.screen_peak_bytes==bytes,"same-slot same-frame recapture reuses accounted backing");
    upload=H.st.uploads;cap_reset();check(tile_load(&H.rec[ri],0,0,0,0,&tile)&&H.st.uploads==upload+1,"recapture invalidates otherwise matching cached tile");
    check(!memcmp(H.scr_data[0],mock_vram+H.fb_addr[1],bytes),"recapture replaces all native pixels");
    mock_read_fail=1;check(!scr_capture(0)&&H.scr_rec[0]==NOREC&&!H.scr_data[0]&&!H.screen_bytes&&H.screen_peak_bytes==bytes,"failed refreshed backing is retired instead of exposed as partial pixels");mock_read_fail=0;
    {u8 rgb[3]={0xA5,0xA5,0xA5};check(!PS2HWD_ReadScreenRGB(0,rgb)&&rgb[0]==0xA5,"failed recapture cannot be read as a valid slot");}
    hw_FlushScreenTextures();check(!H.screen_bytes&&H.scr_rec[0]==NOREC&&H.used_blocks==tile_blocks(),"screen flush releases EE proxy and preserves reusable tile");tex_free_all();check(!H.used_blocks,"full cleanup releases tile");
    printf("CAPTURE IDENTITY %dx%d CT%d cores=%d source_bytes=%u tile_bytes=%u pool_bytes=%u\n",w,h,fb32?32:16,copies,bytes,TILE_SIZE*TILE_SIZE*4u,H.pool_blocks*256u);
}

static void scaled_sprite(void)
{
    int x0=-17,y0=-9,x1=503,y1=391,clipw=480,cliph=360;float u0=3.25f,v0=1.5f,u1=629.5f,v1=477.25f;
    u64 wantframe,wantalpha=ALPHA_AD,frame=0,alpha=0,tex1=0,clamp=0;unsigned at,sprites=0;pkt_t p;
    int ox=0,oy=0;u8 *cover=calloc((size_t)clipw*cliph,1);check(cover!=NULL,"scaled oracle allocation");
    native_init(640,480,1);fill_frame(3);check(scr_capture(0),"scaled source capture");wantframe=frame_reg(H.target,0xFF00FF00u);cap_reset();
    check(sprite_screen(&H.rec[H.scr_rec[0]],1,wantframe,wantalpha,1,x0,y0,x1,y1,u0,v0,u1,v1,clipw,cliph,0),"scaled bilinear sprite draw");
    for(at=0;at<cap_n&&pkt_at(at,&p);at+=1+p.size)
    {
        if(p.flg==2){const u32 *raw=(const u32 *)(const void *)&cap[at+1];ox=(int)(raw[TILE_SIZE+1]&1023u);oy=(int)((raw[TILE_SIZE+1]>>10)&1023u);}
        else if(p.flg==0&&p.nreg==1&&(p.regs&15)==GIF_REGS_AD)
            for(unsigned k=0;k<p.nloop;k++)
            {qw_t q=cap[at+1+k];if(q.d[1]==GSR_FRAME_1)frame=q.d[0];if(q.d[1]==GSR_ALPHA_1)alpha=q.d[0];if(q.d[1]==GSR_TEX1_1)tex1=q.d[0];if(q.d[1]==GSR_CLAMP_1)clamp=q.d[0];}
        else if(p.pre&&(p.prim&7)==PRIM_SPRITE)
        {
            const qw_t *q=&cap[at+1];int dx0=(int)(q[2].d[0]&65535u)/16-2048,dy0=(int)((q[2].d[0]>>32)&65535u)/16-2048;
            int dx1=(int)(q[5].d[0]&65535u)/16-2048,dy1=(int)((q[5].d[0]>>32)&65535u)/16-2048;
            float tu0=(float)(q[0].d[0]&16383u)/16,tv0=(float)((q[0].d[0]>>32)&16383u)/16;
            float tu1=(float)(q[3].d[0]&16383u)/16,tv1=(float)((q[3].d[0]>>32)&16383u)/16;
            check(frame==wantframe&&alpha==wantalpha&&(p.prim&PRIM_ABE),"scaled sprites preserve mask and blend equation");
            check(((tex1>>5)&3)==3&&((clamp>>14)&1023u)==TILE_SIZE-1&&((clamp>>34)&1023u)==TILE_SIZE-1,"bilinear sprites retain full halo clamp");
            check(dx0>=0&&dy0>=0&&dx1<=clipw&&dy1<=cliph&&dx1>dx0&&dy1>dy0,"scaled tile clipped to visible destination pixels");
            check(fabsf(tu0-(u0+(dx0-x0)*(u1-u0)/(x1-x0)-ox+1))<0.0626f&&fabsf(tu1-(u0+(dx1-x0)*(u1-u0)/(x1-x0)-ox+1))<0.0626f,"scaled U retains original affine phase and halo offset");
            check(fabsf(tv0-(v0+(dy0-y0)*(v1-v0)/(y1-y0)-oy+1))<0.0626f&&fabsf(tv1-(v0+(dy1-y0)*(v1-v0)/(y1-y0)-oy+1))<0.0626f,"scaled V retains original affine phase and halo offset");
            for(int y=dy0;y<dy1;y++)for(int x=dx0;x<dx1;x++)
            {
                float su=u0+(x-x0)*(u1-u0)/(x1-x0),sv=v0+(y-y0)*(v1-v0)/(y1-y0);
                check(su>=ox-0.0001f&&su<ox+TILE_CORE+0.0001f&&sv>=oy-0.0001f&&sv<oy+TILE_CORE+0.0001f,"each scaled destination pixel belongs to its original source core");
                check(!cover[y*clipw+x]++,"scaled destination pixels drawn once");
            }
            sprites++;
        }
    }
    for(int y=0;y<cliph;y++)for(int x=0;x<clipw;x++)check(cover[y*clipw+x]==1,"scaled sprite complete clipped coverage");
    free(cover);tex_free_all();printf("CAPTURE SCALE sprites=%u clipped=%dx%d bilinear=1\n",sprites,clipw,cliph);
}

static void wipe_sprites(int tinted,int additive,int screenh,int mw,int mh,int tiledmask)
{
    GLMipmap_t m={0};FSurfaceInfo surf={0};pkt_t p;unsigned at,sprites=0,base=0,end=0,mask=0,channels[3]={0};
    u64 frame=0,alpha=0,tex0=0;int ri;native_init(640,screenh,1);fill_frame(1);check(scr_capture(0),"wipe start capture");fill_frame(2);check(scr_capture(1),"wipe end capture");
    m.width=(u16)mw;m.height=(u16)mh;m.format=GL_TEXFMT_ALPHA_8;m.data=malloc((size_t)mw*mh);check(m.data!=NULL,"wipe mask allocation");memset(m.data,127,(size_t)mw*mh);
    ri=tex_upload(&m);check(ri!=NOREC&&H.rec[ri].tiled==tiledmask&&H.rec[ri].clut==3,"ALPHA_8 mask residency matches available pool");
    H.cur_tex=ri;H.rec_cap=H.rec_n; /* force the first tile's rec_new through realloc */
    surf.PolyColor.s.red=31;surf.PolyColor.s.green=113;surf.PolyColor.s.blue=227;surf.PolyColor.s.alpha=tinted?255:0;
    cap_reset();hw_DoScreenWipe(0,1,tinted?&surf:NULL,additive?PF_Additive:0);
    check(H.cur_tex==ri&&!H.rec[ri].pin,"wipe restores selected mask and its pin after first-tile record allocation");
    for(at=0;at<cap_n&&pkt_at(at,&p);at+=1+p.size)
    {
        if(p.flg==0&&p.nreg==1&&(p.regs&15)==GIF_REGS_AD)
            for(unsigned k=0;k<p.nloop;k++){qw_t q=cap[at+1+k];if(q.d[1]==GSR_FRAME_1)frame=q.d[0];if(q.d[1]==GSR_ALPHA_1)alpha=q.d[0];if(q.d[1]==GSR_TEX0_1)tex0=q.d[0];}
        else if(p.pre&&(p.prim&7)==PRIM_SPRITE)
        {
            unsigned fbmask=(u32)(frame>>32),psm=(unsigned)((tex0>>20)&63u);
            if(psm==PSM_CT32||tiledmask)check((tex0&16383u)==H.rec[tile_cache_rec].blk&&((tex0>>26)&15)==log2_u32(TILE_SIZE)&&((tex0>>30)&15)==log2_u32(TILE_SIZE),"wipe sprite samples bounded tile, never proxy block zero");
            else check((tex0&16383u)==H.rec[ri].blk,"resident wipe mask samples original allocation");
            if(psm==PSM_CT32){if(alpha==ALPHA_AD){check(!fbmask&&(p.prim&PRIM_ABE),"wipe end uses destination alpha");end++;}else{check(alpha==ALPHA_TRANS&&!fbmask&&!(p.prim&PRIM_ABE),"wipe base retains unblended native colour");base++;}}
            else {check(psm==PSM_T8,"wipe mask format preserved");mask++;if(!tinted)check(fbmask==0x00FFFFFFu&&alpha==ALPHA_TRANS&&!(p.prim&PRIM_ABE),"wipe mask writes alpha channel only");else{int ch;for(ch=0;ch<3;ch++)if(fbmask==~(0xFFu<<(8*ch)))break;check(ch<3&&alpha==(additive?ALPHA_ADD:ALPHA_RSUB)&&(p.prim&PRIM_ABE),"tinted wipe preserves per-channel mask and blend");channels[ch]++;}}
            sprites++;
        }
    }
    check(base==cores(640,screenh)&&end==(tinted?0u:cores(640,screenh))&&mask==(tinted?3u:1u)*(tiledmask?cores(mw,mh):1u),"every wipe pass covers all source cores");
    if(tinted)check(channels[0]==(tiledmask?cores(mw,mh):1u)&&channels[1]==channels[0]&&channels[2]==channels[0],"all three tinted channels retain complete tiled masks");
    tex_free_all();free(m.data);printf("CAPTURE WIPE tinted=%d additive=%d internal=640x%d mask=%dx%d tiled=%d sprites=%u base=%u end=%u mask_packets=%u\n",tinted,additive,screenh,mw,mh,tiledmask,sprites,base,end,mask);
}
static void post_image(void)
{
    float points[SCREENVERTS][SCREENVERTS][2];int source,saved;unsigned at,triangles=0;pkt_t p;u64 tex0=0,clamp=0;
    native_init(640,480,1);fill_frame(1);check(scr_capture(HWD_SCREENTEXTURE_GENERIC1),"post-image native capture");source=H.scr_rec[HWD_SCREENTEXTURE_GENERIC1];saved=H.cur_tex;
    for(int x=0;x<SCREENVERTS;x++)for(int y=0;y<SCREENVERTS;y++){points[x][y][0]=8.8f*x/(SCREENVERTS-1)-4.4f;points[x][y][1]=8.8f*y/(SCREENVERTS-1)-4.4f;points[x][y][0]+=0.04f*sinf((float)y);}
    /* Full-size source/backing, bounded destination viewport for the host packet
     * recorder. Full-frame post-image raster parity is a separate native test. */
    H.scr_w=64;H.scr_h=48;H.sx=H.sy=1;
    cap_reset();hw_PostImgRedraw(points);check(H.cur_tex==saved&&H.rec[source].tiled==1,"post-image restores binding and retains source proxy");
    for(at=0;at<cap_n&&pkt_at(at,&p);at+=1+p.size)
    {
        if(p.flg==0&&p.nreg==1&&(p.regs&15)==GIF_REGS_AD){for(unsigned k=0;k<p.nloop;k++){if(cap[at+1+k].d[1]==GSR_TEX0_1)tex0=cap[at+1+k].d[0];if(cap[at+1+k].d[1]==GSR_CLAMP_1)clamp=cap[at+1+k].d[0];}}
        else if(p.pre&&(p.prim&PRIM_TME))
        {
            const u32 masku=(u32)(clamp>>4)&1023u,maskv=(u32)(clamp>>24)&1023u;
            const u32 anchor=(u32)H.rec[tile_cache_rec].blk+H.rec[tile_cache_rec].nblk-tile_blocks();
            check((masku==63u||masku==127u)&&maskv==63u,"post-image uses the exact supported narrow/wide masks");
            check((tex0&16383u)==(masku==127u?anchor-tile_blocks():anchor),"post-image texture base matches its physical narrow/wide layout");triangles++;
        }
    }
    check(triangles>0&&H.used_blocks==H.rec[tile_cache_rec].nblk&&!H.st.screen_restores,"warped post-image uses tile uploader without full restore");tex_free_all();printf("CAPTURE POSTIMG destination=64x48 source=640x480 textured_packets=%u\n",triangles);
}
static void preserve_selected(int post,int shared)
{
    GLMipmap_t selected_owner={0}, filler_owner={0}, view_owner={0};
    float points[SCREENVERTS][SCREENVERTS][2];
    int selected,image,filler,source;u32 selected_blk,filler_blk;
    native_init(640,512,1);fill_frame(4);
    check(scr_capture(HWD_SCREENTEXTURE_GENERIC1),"selected-state capture");
    source=H.scr_rec[HWD_SCREENTEXTURE_GENERIC1];
    if(shared)
    {
        texrec_t tile;
        check(tile_load(&H.rec[source],0,0,0,0,&tile),"shared-view scratch priming");
        tex_drop(tile_cache_rec,0);
        check(H.scr_data[HWD_SCREENTEXTURE_GENERIC1]&&H.rec[source].used,"EE source survives scratch cache drop");
    }
    selected_blk=vram_alloc(64,PAGE_BLOCKS);filler_blk=vram_alloc(448,PAGE_BLOCKS);
    check(selected_blk&&filler_blk&&H.used_blocks==H.pool_blocks,"selected-state pool completely occupied before first tile");
    image=selected=rec_new();check(image!=NOREC,"selected-state image record allocation");
    H.rec[image].blk=(u16)selected_blk;H.rec[image].nblk=64;H.rec[image].owner=&selected_owner;
    selected_owner.downloaded=(UINT32)(image+1);own_set(image);lru_push_head(image);
    if(shared)
    {
        selected=rec_new();check(selected!=NOREC,"selected-state shared CLUT view allocation");
        H.rec[selected].blk=(u16)selected_blk;H.rec[selected].nblk=64;H.rec[selected].owner=&view_owner;
        H.rec[selected].share=image;H.rec[image].sec=selected;
        view_owner.downloaded=(UINT32)(selected+1);lru_push_head(selected);
    }
    filler=rec_new();check(filler!=NOREC,"selected-state filler record allocation");
    H.rec[filler].blk=(u16)filler_blk;H.rec[filler].nblk=448;H.rec[filler].owner=&filler_owner;
    filler_owner.downloaded=(UINT32)(filler+1);own_set(filler);lru_push_head(filler);
    H.cur_tex=selected;H.cur_missing=1;H.rec_cap=H.rec_n;
    for(int x=0;x<SCREENVERTS;x++)for(int y=0;y<SCREENVERTS;y++)
    {
        points[x][y][0]=8.8f*x/(SCREENVERTS-1)-4.4f;
        points[x][y][1]=8.8f*y/(SCREENVERTS-1)-4.4f;
    }
    cap_reset();
    if(post){H.scr_w=H.scr_h=32;H.sx=H.sy=1;hw_PostImgRedraw(points);}
    else draw_screen_tex(HWD_SCREENTEXTURE_GENERIC1,NULL,PF_NoDepthTest,0.9f,0.8f);
    check(selected_owner.downloaded==(UINT32)(image+1)&&H.rec[image].used&&H.rec[image].owner==&selected_owner,"screen draw keeps saved physical image owner alive throughout first tile allocation");
    check(H.cur_tex==selected&&H.cur_missing==1&&!H.rec[selected].pin&&!H.rec[image].pin,"screen draw restores original selected handle, missing flag and physical image pin");
    if(shared)check(view_owner.downloaded==(UINT32)(selected+1)&&H.rec[selected].used&&H.rec[selected].share==image,"selected CLUT view survives with its physical image owner");
    check(!filler_owner.downloaded&&H.rec[source].used&&H.rec[source].tiled==1,"first tile evicts other resident while retaining source proxy");
    check(H.used_blocks==64u+H.rec[tile_cache_rec].nblk&&H.st.uploads>0,"selected-state draw completes with bounded cache and selected resident");
    tex_free_all();printf("CAPTURE SELECTED post=%d shared=%d preserved=1 unrelated_eviction=1\n",post,shared);
}
int main(void)
{
    cap=calloc(CAP_MAX,sizeof *cap);check(cap!=NULL,"packet allocation");
    capture_and_identity(640,480,1);capture_and_identity(640,448,1);capture_and_identity(640,400,1);capture_and_identity(640,512,1);capture_and_identity(640,480,0);
    scaled_sprite();
    wipe_sprites(0,0,480,1024,512,1);wipe_sprites(1,0,480,1024,512,1);wipe_sprites(1,1,480,1024,512,1);
    wipe_sprites(0,0,512,320,200,0);wipe_sprites(1,0,512,320,200,0);wipe_sprites(1,1,512,320,200,0);post_image();preserve_selected(0,0);preserve_selected(1,0);preserve_selected(0,1);preserve_selected(1,1);
    printf("CAPTURE COMPLETE failed=0 checks=%llu\n",capture_checks);return 0;
}
