int TN_ZRaw(u32 *dest);
#define main wf_original_suite_unused
#include "opt17/highres_tiles_native/main.inc"
#undef main

int WF_Reserve(void);
void WF_Trace(int mode,u32 *words);
int WF_Draw(int reference,int scenario,int zwrite,u32 *words,u32 *info);
int WF_ZRaw(u32 *dest);
static u32 *wf_zref,*wf_zgot;
static int wf_stop;

static void wf_pair(int scenario,int linear)
{
    const int widths[9]={1024,1024,1024,512,512,1024,1024,1024,512};
    const int heights[9]={1024,1024,1024,512,512,512,1024,1024,512};
    const int keyed=scenario==1||scenario==4||scenario==5||scenario==8;
    const int wrap=scenario!=2&&scenario!=5;
    GLMipmap_t *m=tn_texture(0,widths[scenario],heights[scenario]);
    m->flags=(wrap?TF_WRAPXY:0)|(keyed?TF_CHROMAKEYED:0);
    u32 words[36]={0};char name[64];
    snprintf(name,sizeof name,"row-c%d-l%d",scenario,linear);
    for(int variant=0;variant<2;variant++)
    {
        u32 source[8]={0},info[20]={0};
        TN_Pool(variant?512:0);
        if(variant)CHECK("wf_reserve",WF_Reserve(),"name=%s permanent=64 ordinary=448",name);
        frame_start(tn_bg);
        D.pfnSetSpecialState(HWD_SET_TEXTUREFILTERMODE,linear?HWD_SET_TEXTUREFILTER_BILINEAR:HWD_SET_TEXTUREFILTER_POINTSAMPLED);
        CHECK("wf_upload",TN_Upload(m,source),"name=%s variant=%d",name,variant);
        CHECK("wf_admission",source[0]==(u32)variant&&source[1]==19&&source[2]==(u32)widths[scenario]
            &&source[3]==(u32)heights[scenario]&&!source[6]&&!source[7],
            "name=%s variant=%d tiled=%u psm=%u size=%ux%u level=%u/%u",name,variant,source[0],source[1],source[2],source[3],source[6],source[7]);
        if(scenario>=7)tn_pattern(5+scenario);
        WF_Trace(variant?2:1,words);
        CHECK("wf_draw",WF_Draw(!variant,scenario,scenario&1,words,info),"name=%s variant=%d",name,variant);
        printf("TN ROWS name=%s variant=%d source=%dx%d keyed=%d wrap=%d linear=%d actual_linear=%u fast=%u boxes=%u masks=%u upload_bytes=%u minimum=%u capacity=%u layout=%u packed_qwords=%u packed_wrong=%u input_changed=%u uploads=%u rows_taken=%u rows_declined=%u rows_failed=%u translucent=%u abe=%u\n",
            name,variant,widths[scenario],heights[scenario],keyed,wrap,linear,info[16],info[0],info[1],info[2],info[3],info[4],info[5],info[6],info[7],info[8],info[9],info[10],info[11],info[12],info[13],info[14],info[15]);
        CHECK("wf_original_vertices",!info[8]&&!info[9]&&info[7]>=9&&!(info[7]%9),
            "name=%s variant=%d actual_STQ_RGBA_XY_Zlow24_wrong=%u input_changed=%u qwords=%u",name,variant,info[8],info[9],info[7]);
        wf_stop |= info[8] != 0 || info[9] != 0;
        if(variant&&scenario!=6)CHECK("wf_actual_rows",!info[0]&&info[1]&&info[2]&&info[4]==64&&info[5]==64&&info[6]==3&&info[11]==1&&!info[12]&&!info[13],
            "name=%s boxes=%u masks=%u minimum=%u capacity=%u layout=%u rows=%u/%u/%u",name,info[1],info[2],info[4],info[5],info[6],info[11],info[12],info[13]);
        if(variant&&scenario==6)CHECK("wf_actual_guard_window",info[0]==1&&!info[1]&&!info[2]&&info[4]==64&&info[5]==64&&info[6]==3,
            "name=%s fast=%u boxes=%u masks=%u minimum=%u capacity=%u layout=%u",name,info[0],info[1],info[2],info[4],info[5],info[6]);
        CHECK("wf_readback",tn_grab(variant?fb:tn_ref)==0,"name=%s variant=%d",name,variant);
        CHECK("wf_depth_readback",WF_ZRaw(variant?wf_zgot:wf_zref)==0,"name=%s variant=%d",name,variant);
        if(scenario>=7)
        {
            u8 seen[256]={0};int unique=0;const u32 *pixels=variant?fb:tn_ref;
            for(int i=0;i<FW*FH;i++)if(!seen[pixels[i]>>24]){seen[pixels[i]>>24]=1;unique++;}
            CHECK("wf_destination_alpha_pattern",unique>8,"name=%s variant=%d alpha_values=%d translucent=%u abe=%u",name,variant,unique,info[14],info[15]);
        }
        char image_name[80];snprintf(image_name,sizeof image_name,"%s-v%d",name,variant);
        tn_save(image_name,variant?fb:tn_ref);
    }
    const int beyond=tn_compare(name,fb,tn_ref,1,0);
    CHECK("wf_full_RGBA_parity",beyond==0,"name=%s complete=%d tolerance=0 beyond=%d",name,FW*FH,beyond);
    int maxerr=0;for(int i=0;i<FW*FH;i++)for(int c=0;c<4;c++)
    {
        int delta=abs((int)((fb[i]>>(c*8))&255)-(int)((tn_ref[i]>>(c*8))&255));
        if(delta>maxerr)maxerr=delta;
    }
    int badz=0;for(int i=0;i<FW*FH;i++)badz+=((wf_zref[i]^wf_zgot[i])&0xffffffu)!=0;
    printf("TN ROWS_DEPTH name=%s compared=%d wrong_Zlow24=%d\n",name,FW*FH,badz);
    CHECK("wf_full_depth_parity",!badz,"name=%s compared=%d wrong=%d",name,FW*FH,badz);
    wf_stop |= maxerr > 1 || badz != 0;
    if(wf_stop)printf("TN ROWS_STOP name=%s max_RGBA_error=%d wrong_Zlow24=%d; review before further cases\n",name,maxerr,badz);
    tex_free(m);
}

int main(int argc,char **argv)
{
    g_argc=argc;g_argv=argv;make_pals();dump_on=1;
    fb=memalign(64,320u*200u*4u);tn_ref=memalign(64,320u*200u*4u);
    wf_zref=memalign(64,320u*200u*4u);wf_zgot=memalign(64,320u*200u*4u);
    CHECK("wf_buffers",fb&&tn_ref&&wf_zref&&wf_zgot,"complete native RGBA and Zlow24 buffers");
    tn_init(320,200,PS2HWD_VIDEO_NTSC);
    if(arg("halo-only"))wf_pair(6,1);
    else for(int scenario=0;scenario<9&&!wf_stop;scenario++)if(scenario!=6)for(int linear=0;linear<2&&!wf_stop;linear++)wf_pair(scenario,linear);
    D.pfnShutdown();free(fb);free(tn_ref);free(wf_zref);free(wf_zgot);
    printf("TN COMPLETE checks=%d failures=%d\n",checks,failures);
    return failures?1:0;
}
