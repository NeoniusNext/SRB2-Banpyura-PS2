/* Reuse only the maintained platform-boundary and initialization fixtures. */
#define main capture_fixture_main_unused
#include "tile_capture_hosttest.c"
#undef main

static void classify_cost_reset(void)
{
#ifdef TILE_CORE_FAST_COST
	tile_core_fast_triangles = 0;
#endif
#ifdef TILE_BOX_COST
	tile_box_nodes = tile_pixel_fallbacks = 0;
#endif
	tile_run_peak_bytes = 0;
}

static void classify_cost_report(void)
{
#ifdef TILE_CORE_FAST_COST
	printf("TILE CLASSIFY fast=%u", tile_core_fast_count());
#ifdef TILE_BOX_COST
	printf(" boxes=%u pixel_fallback=%u", tile_box_count(), tile_pixel_fallback_count());
#endif
	puts("");
#endif
}

static void sky_span_cost(void)
{
	GLMipmap_t m={0};cv_t tri[3]={0};piece_t pc={0,0,-1,0};
	native_init(640,480,1);ps2hwd_dbg_flags=0;
	m.width=1024;m.height=512;m.format=GL_TEXFMT_P_8;m.flags=TF_WRAPXY;m.regen_kind=1;
	m.data=malloc(1024u*512u);check(m.data!=NULL,"sky-span source");memset(m.data,47,1024u*512u);
	const int ri=tex_upload(&m);check(ri!=NOREC&&H.rec[ri].tiled==1,"sky-span virtual source");
	H.cur_tex=ri;check(begin_draw(PF_NoDepthTest,NULL),"sky-span draw setup");
	for(int i=0;i<3;i++)
	{
		const float xy[3][2]={{-.8f,-.7f},{.75f,-.6f},{-.15f,.85f}};
		const float uv[3][2]={{-16,256},{0,384},{-16,384}};
		tri[i].w=1.0f+.5f*i;tri[i].x=xy[i][0]*tri[i].w;tri[i].y=xy[i][1]*tri[i].w;
		tri[i].a[0]=uv[i][0]/m.width;tri[i].a[1]=uv[i][1]/m.height;
	}
	H.st.uploads=H.st.upload_bytes=0;tile_stencil_rectangles=tile_stencil_mask_sprites=0;
	tile_stencil_triangle_submissions=tile_stencil_bbox_pixels=0;classify_cost_reset();cap_reset();cost_qwords=0;
	tile_emit_triangle(tri,&pc,ri);
	printf("TILE SKY SPAN U16 V128 packet_bytes=%llu uploads=%u upload_bytes=%u rectangles=%u mask_sprites=%u triangle_submissions=%llu workspace_peak=%u\n",
		(unsigned long long)(cost_qwords*16u),H.st.uploads,H.st.upload_bytes,tile_stencil_rectangles,tile_stencil_mask_sprites,
		(unsigned long long)tile_stencil_triangle_submissions,tile_workspace_peak());
	classify_cost_report();tex_free_all();free(m.data);
}

int main(void)
{
    float points[SCREENVERTS][SCREENVERTS][2];
    cap=calloc(CAP_MAX,sizeof *cap);check(cap!=NULL,"bounded packet recorder");
    for(int height=480;height<=512;height+=32)
    {
        native_init(640,height,1);fill_frame(1);
        check(scr_capture(HWD_SCREENTEXTURE_GENERIC1),"full cost source capture");
        for(int x=0;x<SCREENVERTS;x++)for(int y=0;y<SCREENVERTS;y++)
        {
            points[x][y][0]=8.8f*x/(SCREENVERTS-1)-4.4f+0.04f*sinf((float)y);
            points[x][y][1]=8.8f*y/(SCREENVERTS-1)-4.4f;
        }
        tile_stencil_rectangles=tile_stencil_mask_sprites=0;
        tile_stencil_triangle_submissions=tile_stencil_bbox_pixels=0;
        H.st.uploads=H.st.upload_bytes=0;classify_cost_reset();cap_reset();cost_qwords=0;
        hw_PostImgRedraw(points);
        printf("TILE COST %dx%d source_bytes=%u packet_bytes=%llu uploads=%u upload_bytes=%u rectangles=%u mask_sprites=%u triangle_submissions=%llu viewport_bbox_upper=%llu workspace_peak=%u recorder_bytes=%u\n",
            H.vw,H.vh,scr_bytes(),(unsigned long long)(cost_qwords*16u),H.st.uploads,H.st.upload_bytes,
            tile_stencil_rectangles,tile_stencil_mask_sprites,(unsigned long long)tile_stencil_triangle_submissions,
            (unsigned long long)tile_stencil_bbox_pixels,tile_workspace_peak(),(unsigned)(CAP_MAX*sizeof *cap));
        classify_cost_report();
        tex_free_all();
    }
    sky_span_cost();
    free(cap);puts("TILE COST COMPLETE");return 0;
}
