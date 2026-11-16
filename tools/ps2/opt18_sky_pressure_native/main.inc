int TN_ZRaw(u32 *dest);
#define main sp_original_suite_unused
#include "opt17/highres_tiles_native/main.inc"
#undef main

int SP_Reserve(int pressure);
int SP_Victim(int pressure,u32 *info);
void SP_Enter(void);
int SP_AfterSky(int pressure,u32 *info);
int SP_Leave(int pressure,u32 *info);
int SP_Overwrite(int pressure,u32 *info);
void SP_Negative(int victim,int wide);
int SP_Draw(int reference,int sky,u32 *words,u32 *info);
int SP_ZRaw(u32 *dest);
static u32 *sp_zref,*sp_zgot;
static int sp_stop;

static void sp_save_z(const char *name,const u32 *pixels)
{
    char path[96];snprintf(path,sizeof path,"host:tn-%s-%dx%d.z32",name,FW,FH);
    FILE *file=fopen(path,"wb");if(file){fwrite(pixels,4,(size_t)FW*FH,file);fclose(file);}
}

static u32 sp_hash(GLMipmap_t *m)
{
    u32 hash=2166136261u;const u8 *bytes=m->data;
    for(u32 i=0;i<(u32)m->width*m->height;i++)hash=(hash^bytes[i])*16777619u;
    return hash;
}

static void sp_pair(int linear)
{
    GLMipmap_t *victim=tn_texture(0,128,896),*sky=tn_texture(0,1024,512),*overwrite=tn_texture(0,128,896);
    victim->flags=sky->flags=overwrite->flags=TF_WRAPXY;
    for(int y=0;y<896;y++)for(int x=0;x<128;x++)((u8 *)overwrite->data)[y*128+x]=(u8)(3+((x*11+y*31)%233));
    const u32 victim_hash=sp_hash(victim),sky_hash=sp_hash(sky),overwrite_hash=sp_hash(overwrite);
    u32 words[2][36]={{0}};char name[48];snprintf(name,sizeof name,"pressure-l%d",linear);
    for(int variant=0;variant<2;variant++)
    {
        u32 source[8]={0},info[12]={0};TN_Pool(variant?512:0);
        CHECK("sp_reserve",SP_Reserve(variant),"name=%s variant=%d permanent16KiB=1",name,variant);
        frame_start(tn_bg);
        D.pfnSetSpecialState(HWD_SET_TEXTUREFILTERMODE,linear?HWD_SET_TEXTUREFILTER_BILINEAR:HWD_SET_TEXTUREFILTER_POINTSAMPLED);
        CHECK("sp_victim_upload",TN_Upload(victim,source)&&!source[0]&&source[4]==448,"name=%s variant=%d tiled=%u blocks=%u",name,variant,source[0],source[4]);
        CHECK("sp_exact_prefix_victim",SP_Victim(variant,info),"name=%s variant=%d victim_block=%u blocks=%u used=%u",name,variant,info[0],info[1],info[2]);
        SP_Negative(variant&&arg("victim-order-negative"),0);
        CHECK("sp_victim_draw",SP_Draw(!variant,0,words[0],info),"name=%s variant=%d",name,variant);
        CHECK("sp_packed_victim",info[0]==9&&!info[1]&&!info[2]&&info[8]==(u32)linear,"name=%s variant=%d actual_STQ_RGBA_XY_Zlow24_wrong=%u input_changed=%u qwords=%u actual_linear=%u",name,variant,info[1],info[2],info[0],info[8]);
        printf("TN PRESSURE_DRAW name=%s variant=%d kind=victim qwords=%u packed_wrong=%u input_changed=%u queue_after=%u\n",name,variant,info[0],info[1],info[2],info[9]);
        sp_stop|=info[1]!=0||info[2]!=0;
        // No fixture flush, fence or read separates the old victim reader,
        // pressure-triggered wide upload/reader and the later ordinary upload.
        SP_Enter();
        CHECK("sp_sky_upload",TN_Upload(sky,source)&&source[0]==(u32)variant&&source[2]==1024&&source[3]==512&&!source[6]&&!source[7],"name=%s variant=%d tiled=%u size=%ux%u level=%u/%u",name,variant,source[0],source[2],source[3],source[6],source[7]);
        SP_Negative(0,variant&&arg("wide-order-negative"));
        CHECK("sp_sky_draw",SP_Draw(!variant,1,words[1],info),"name=%s variant=%d",name,variant);
        CHECK("sp_packed_sky",info[0]==9&&!info[1]&&!info[2]&&info[8]==(u32)linear,"name=%s variant=%d actual_STQ_RGBA_XY_Zlow24_wrong=%u input_changed=%u qwords=%u actual_linear=%u",name,variant,info[1],info[2],info[0],info[8]);
        const u32 negative_uploads=variant&&arg("wide-order-negative")?1u:0u;
        // H.st accounts production texture callers. The deliberately wrong
        // raw upload_bands control adds GS commands without those counters.
        CHECK("sp_wide_route",!variant||(info[3]==1&&!info[4]&&!info[5]&&info[6]==32768u&&info[7]==1u),"name=%s variant=%d fast=%u boxes=%u masks=%u upload_bytes=%u uploads=%u deliberate_wrong_uploads=%u",name,variant,info[3],info[4],info[5],info[6],info[7],negative_uploads);
        printf("TN PRESSURE_DRAW name=%s variant=%d kind=sky qwords=%u packed_wrong=%u input_changed=%u queue_after=%u fast=%u boxes=%u masks=%u upload_bytes=%u uploads=%u\n",name,variant,info[0],info[1],info[2],info[9],info[3],info[4],info[5],info[6],info[7]);
        sp_stop|=info[1]!=0||info[2]!=0;
        CHECK("sp_actual_pressure_eviction",SP_AfterSky(variant,info),"name=%s variant=%d borrowed=%u block=%u capacity=%u evictions=%u queued_after=%u queued_before=%u used=%u",name,variant,info[0],info[1],info[2],info[3],info[4],info[5],info[6]);
        printf("TN PRESSURE_OWNER name=%s variant=%d borrowed=%u block=%u capacity=%u evictions=%u queue_after=%u queue_before=%u used=%u fixture_fence_before=0\n",name,variant,info[0],info[1],info[2],info[3],info[4],info[5],info[6]);
        CHECK("sp_end_dome_return",SP_Leave(variant,info),"name=%s variant=%d permission=%u failed=%u borrowed=%u ordinary_capacity=%u used=%u",name,variant,info[0],info[1],info[2],info[3],info[4]);
        CHECK("sp_overwrite_upload",TN_Upload(overwrite,source)&&!source[0]&&source[4]==448,"name=%s variant=%d blocks=%u",name,variant,source[4]);
        CHECK("sp_actual_ordinary_overwrite",SP_Overwrite(variant,info),"name=%s variant=%d ordinary_block=%u blocks=%u borrowed=%u used=%u queued_after=%u",name,variant,info[0],info[1],info[2],info[3],info[4]);
        printf("TN PRESSURE_OVERWRITE name=%s variant=%d ordinary=%u blocks=%u borrowed_after=%u used=%u queue_after=%u fixture_fence_before=0\n",name,variant,info[0],info[1],info[2],info[3],info[4]);
        CHECK("sp_CPU_pixels_retained",sp_hash(victim)==victim_hash&&sp_hash(sky)==sky_hash&&sp_hash(overwrite)==overwrite_hash,"name=%s variant=%d victim=%08x sky=%08x overwrite=%08x",name,variant,sp_hash(victim),sp_hash(sky),sp_hash(overwrite));
        CHECK("sp_first_RGBA_readback",tn_grab(variant?fb:tn_ref)==0,"name=%s variant=%d",name,variant);
        CHECK("sp_first_Z_readback",SP_ZRaw(variant?sp_zgot:sp_zref)==0,"name=%s variant=%d",name,variant);
        char image[64];snprintf(image,sizeof image,"%s-v%d",name,variant);tn_save(image,variant?fb:tn_ref);sp_save_z(image,variant?sp_zgot:sp_zref);
    }
    const int beyond=tn_compare(name,fb,tn_ref,1,0);
    CHECK("sp_full_RGBA_parity",!beyond,"name=%s complete=%d tolerance=0 beyond=%d",name,FW*FH,beyond);
    int left=0,right=0,badz=0,maxerr=0;
    for(int i=0;i<FW*FH;i++)
    {
        const int visible=(tn_ref[i]&0xffffffu)!=tn_bg;
        if(i%FW<FW/2)left+=visible;else right+=visible;
        badz+=((sp_zref[i]^sp_zgot[i])&0xffffffu)!=0;
        for(int c=0;c<4;c++){int d=abs((int)((fb[i]>>(c*8))&255)-(int)((tn_ref[i]>>(c*8))&255));if(d>maxerr)maxerr=d;}
    }
    printf("TN PRESSURE_DEPTH name=%s compared=%d wrong_Zlow24=%d victim_visible=%d wide_visible=%d\n",name,FW*FH,badz,left,right);
    CHECK("sp_disjoint_visible_readers",left>3000&&right>3000,"name=%s victim_visible=%d wide_visible=%d",name,left,right);
    CHECK("sp_full_Z_parity",!badz,"name=%s compared=%d wrong_Zlow24=%d",name,FW*FH,badz);
    sp_stop|=maxerr>1||badz!=0;
    tex_free(victim);tex_free(sky);tex_free(overwrite);
}

int main(int argc,char **argv)
{
    g_argc=argc;g_argv=argv;make_pals();dump_on=1;
    fb=memalign(64,320u*200u*4u);tn_ref=memalign(64,320u*200u*4u);
    sp_zref=memalign(64,320u*200u*4u);sp_zgot=memalign(64,320u*200u*4u);
    CHECK("sp_buffers",fb&&tn_ref&&sp_zref&&sp_zgot,"complete GS RGBA/Zlow24 readbacks");
    tn_init(320,200,PS2HWD_VIDEO_NTSC);
    for(int linear=0;linear<2&&!sp_stop;linear++)sp_pair(linear);
    D.pfnShutdown();free(fb);free(tn_ref);free(sp_zref);free(sp_zgot);
    printf("TN COMPLETE checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
