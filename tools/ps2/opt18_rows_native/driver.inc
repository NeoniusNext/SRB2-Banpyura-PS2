/* Existing native platform boundaries, with a separate focused probe. */
#include "opt17/highres_tiles_native/driver.inc"

static qw_t wf_expected[9];
static u32 wf_trace_mode, wf_trace_qwords, wf_trace_bad;

static void wf_trace_vertex(const qw_t *first, const qw_t *last)
{
    if (!wf_trace_mode) return;
    if (last - first != 3) { wf_trace_bad++; return; }
    for (int i = 0; i < 3; i++)
    {
        qw_t value = first[i];
        // Z24's unused physical high byte is the existing stencil gate.
        if (i == 2) value.w[2] &= 0x00ffffffu;
        const u32 at = wf_trace_qwords++ % 9u;
        if (wf_trace_mode == 1) wf_expected[at] = value;
        else if (memcmp(&value, &wf_expected[at], sizeof value)) wf_trace_bad++;
    }
}

int WF_Reserve(void)
{
    return tile_cache_get() != NOREC && H.rec[tile_cache_rec].nblk == 64
        && tex_usable_blocks() == H.pool_blocks - 64;
}

void WF_Trace(int mode, u32 *words)
{
    if (mode == 2) memcpy(wf_expected, words, sizeof wf_expected);
    wf_trace_mode = 0; wf_trace_qwords = wf_trace_bad = 0;
}

int WF_Draw(int reference, int scenario, int zwrite, u32 *words, u32 *info)
{
    static const float origin_u[9] = {114,-480,-120,-278,304,880,80.25f,114,-278};
    static const float origin_v[9] = {71,-240,880,-192,-328,-120,100.25f,71,-192};
    const float span_u = scenario == 6 ? 124.0f : scenario < 3 || scenario == 7 ? 680.0f : 722.0f;
    const float span_v = scenario == 6 ? 124.0f : scenario < 3 || scenario == 7 ? 720.0f : 672.0f;
    const float nx[3] = {-.93f,.94f,-.15f}, ny[3] = {-.88f,-.73f,.94f};
    const float w[3] = {1.0f,2.0f,1.5f}, z[3] = {.30f,.15f,.70f};
    const float color[3][4] = {{220,80,50,255},{70,230,140,255},{100,130,250,255}};
    cv_t tri[3], saved[3]; piece_t piece = {0,0,-1,0}; FSurfaceInfo surf;
    const int blend = scenario >= 7;
    memset(&surf,0,sizeof surf);surf.PolyColor.rgba=blend?0x80ffffffu:0xffffffffu;
    const u32 flags=PF_Modulated|PF_NoDepthTest|(blend?PF_Translucent:PF_Masked)|(zwrite?PF_Occlude:0);
    if (!begin_draw(flags,&surf) || P.npass != 1) return 0;
    if (blend && (!P.pass[0].abe || P.pass[0].alpha != ALPHA_TRANS
        || (P.flags&PF_Blending) != PF_Translucent)) return 0;
    P.vcol=1;P.vlight=0;P.fast=P.litfast=P.vuok=0;P.pal=0;P.oldpath=1;P.lmode=LM_NONE;
    memset(tri,0,sizeof tri);
    for(int i=0;i<3;i++)
    {
        tri[i].x=nx[i]*w[i];tri[i].y=ny[i]*w[i];tri[i].z=z[i]*w[i];tri[i].w=w[i];
        tri[i].a[0]=(origin_u[scenario]+(i==1?span_u:i==2?span_u*.21f:0))/(float)P.r->sw;
        tri[i].a[1]=(origin_v[scenario]+(i==2?span_v:i==1?span_v*.08f:0))/(float)P.r->sh;
        for(int c=0;c<4;c++)tri[i].a[2+c]=color[i][c];
        if(blend)tri[i].a[5]=128.0f;
    }
    memcpy(saved,tri,sizeof tri);
    const u32 fast=tile_core_fast_count(),boxes=tile_box_count(),masks=tile_stencil_mask_sprites;
    const u32 bytes=H.st.upload_bytes,uploads=H.st.uploads;
    const u32 rows_taken=wf_rows_taken,rows_declined=wf_rows_declined,rows_failed=wf_rows_failed;
    const u32 actual_linear=P.linear;
    wf_trace_mode=reference?1:2;
    emit_piece(tri,3,&piece);
    wf_trace_mode=0;
    if(reference)memcpy(words,wf_expected,sizeof wf_expected);
    info[0]=tile_core_fast_count()-fast;info[1]=tile_box_count()-boxes;
    info[2]=tile_stencil_mask_sprites-masks;info[3]=H.st.upload_bytes-bytes;
    info[4]=tile_cache_rec==NOREC?0:H.rec[tile_cache_rec].nblk;
    info[5]=info[4];
    info[6]=tile_cache.region;info[7]=wf_trace_qwords;info[8]=wf_trace_bad;
    info[9]=memcmp(tri,saved,sizeof tri)!=0;info[10]=H.st.uploads-uploads;
    info[11]=wf_rows_taken-rows_taken;info[12]=wf_rows_declined-rows_declined;info[13]=wf_rows_failed-rows_failed;
    info[14]=blend;info[15]=P.pass[0].abe;
    info[16]=actual_linear;
    P.vcol=P.vlight=0;
    return 1;
}

int WF_ZRaw(u32 *dest)
{
    return PS2HWD_ReadVram(dest,H.z_addr/256u,H.fbw/64,PSM_Z32,0,0,H.fbw,H.vh);
}

int TN_ZRaw(u32 *dest) { return WF_ZRaw(dest); }
