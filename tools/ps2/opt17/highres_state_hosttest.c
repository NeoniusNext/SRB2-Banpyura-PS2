/* Reuse the maintained host GS boundary; all included renderer functions are
 * copied verbatim from --source-root by highres_state_hosttest.py. */
#define main base_suite_main
#include "base_hosttest.c"
#undef main
#include "ps2_vmodes.h"
unsigned int hwr_geo_off;
static int vu_noretarget;
#include "key-production.inc"
#include "reset-production.inc"

static int starts, stops;
static void hw_choose_size(int, int, int, int *, int *, int *);
static void view_defaults(void);
boolean PS2HWD_Init(void);
void PS2HWD_Shutdown(void);
int PS2HWD_Sync(void) { ov_flush_all(); pk_flush(); H.serial++; return 1; }
#include "size-production.inc"

/* Only hardware bring-up is simulated. Real texture/cache cleanup runs. */
void PS2HWD_Shutdown(void)
{
    stops++;
    dc_flush();
    tex_free_all();
    plan_reset();
    free(H.rec);
    H.rec = NULL;
    memset(&H, 0, sizeof H);
}

boolean PS2HWD_Init(void)
{
    int w, h, field;
    starts++;
    host_init(1);
    hw_choose_size(cfg.video, cfg.screen_w, cfg.screen_h, &w, &h, &field);
    H.mode = cfg.video;
    H.vw = w; H.vh = H.fbh = h; H.fbw = (w + 63) & ~63;
    H.fb_addr[0] = 0;
    H.fb_addr[1] = fb_pages(H.fbw, h, 1) * 8192u;
    H.z_addr = H.fb_addr[1] * 2;
    H.clut_base = (H.z_addr + H.fb_addr[1]) / 256u;
    H.pool_base = H.clut_base + ((CLUT_SLOTS * CLUT_BLOCKS + PAGE_BLOCKS - 1) / PAGE_BLOCKS) * PAGE_BLOCKS;
    H.pool_blocks = VRAM_BLOCKS - H.pool_base;
    H.free_n = 1;
    H.free_range[0].start = H.pool_base;
    H.free_range[0].len = H.pool_blocks;
    H.tex_filter = cfg.linear;
    view_defaults();
    return true;
}

static void start_mode(int output, int mode)
{
    int w, h;
    ps2vm_mode_size(mode, &w, &h);
    if (H.up) PS2HWD_Shutdown();
    cfg.video = output;
    cfg.screen_w = w; cfg.screen_h = h;
    cfg.fbw = cfg.fbh = 0;
    cfg.fb32 = 1;
    cfg.tex_max_bytes = 0;
    PS2HWD_Init();
}

static void modes_and_layout(void)
{
    static const int expected[11][2] = {{320,200},{320,224},{320,240},{320,256},{400,300},{512,384},{512,448},{640,400},{640,448},{640,480},{640,512}};
    int output, mode, w, h, captures = 0, limited = 0;
    group_begin("modes_layout_capture");
    EXPECT(PS2VM_NUMMODES == 11 && ps2vm_mode_for_size(800, 600) == -1, "800x600 incorrectly treated as a logical mode");
    EXPECT(!ps2vm_mode_size(-1, &w, &h) && !ps2vm_mode_size(11, &w, &h), "out-of-range modes accepted");
    EXPECT(ps2vm_output(16)->fbw == 400 && ps2vm_output(16)->fbh == 300 && (ps2vm_output(16)->flags & PS2VM_VGA), "800x600 output is not the documented 400x300 software framebuffer");
    for (output = 0; output < 3; output++) for (mode = 0; mode < 11; mode++)
    {
        u32 fb, pool, before;
        int ri, should_fit;
        EXPECT(ps2vm_mode_size(mode, &w, &h) && w == expected[mode][0] && h == expected[mode][1], "mode %d table changed", mode);
        start_mode(output, mode);
        fb = H.fb_addr[1]; pool = H.pool_blocks * 256u;
        EXPECT(3u * fb + 131072u + pool == 4194304u, "GS ranges overlap or fail exact 4MiB accounting");
        EXPECT(H.pool_base * 256u == H.clut_base * 256u + 131072u && H.free_range[0].start == H.pool_base, "CLUT/pool boundary mismatch");
        should_fit = fb <= pool;
        before = H.used_blocks;
        ri = scr_tex_get(0);
        EXPECT((ri != NOREC) == should_fit, "capture %dx%d allocation differs from exact footprint %u/pool %u", H.vw, H.vh, fb, pool);
        EXPECT(should_fit || (H.used_blocks == before && H.scr_rec[0] == NOREC), "capture failure mutated allocation state");
        captures += should_fit; limited += !should_fit;
        printf("HIGHRES LAYOUT output=%d mode=%d logical=%dx%d internal=%dx%d fb=%u z=%u clut=131072 pool=%u capture=%s logical_rows_exceed_internal=%d ee_video=%u\n",
            output, mode, w, h, H.vw, H.vh, fb, fb, pool, should_fit ? "fits" : "IMPOSSIBLE", h > H.vh, (unsigned)(w * h * 5));
    }
    printf("HIGHRES CAPTURE_MATRIX fits=%d impossible=%d total=33\n", captures, limited);
    group_end("11 logical modes x 3 HW outputs; actual capture allocator checked including explicit limits");
}

static void check_view(int w, int h)
{
    EXPECT(H.scr_w == w && H.scr_h == h && cfg.screen_w == w && cfg.screen_h == h, "logical size stale");
    EXPECT(fabsf(H.sx - (float)H.vw / w) < 1e-6f && fabsf(H.sy - (float)H.vh / h) < 1e-6f, "logical/internal scale stale");
    EXPECT(H.vpx == 0 && H.vpy == 0 && fabsf(H.vpw - H.vw) < 0.001f && fabsf(H.vph - H.vh) < 0.001f, "viewport stale");
    EXPECT(H.sciss_x0 == 0 && H.sciss_y0 == 0 && H.sciss_x1 == (u32)H.vw - 1 && H.sciss_y1 == (u32)H.vh - 1, "scissor stale");
    EXPECT(H.guard_x >= 1024 && H.guard_y >= 1024, "invalid guard volume");
}

static void same_framebuffer_transitions(void)
{
    int output, a, b, rep, cases = 0;
    group_begin("same_framebuffer_transitions");
    for (output = 0; output < 3; output++) for (a = 0; a < 11; a++) for (b = 0; b < 11; b++)
    {
        int aw, ah, bw, bh, x, y, field, nstart, nstop;
        ps2vm_mode_size(a, &aw, &ah); ps2vm_mode_size(b, &bw, &bh);
        start_mode(output, a);
        hw_choose_size(output, bw, bh, &x, &y, &field);
        if (a == b || x != H.vw || y != H.vh) continue;
        nstart = starts; nstop = stops;
        for (rep = 0; rep < 6; rep++)
        {
            int w = rep & 1 ? aw : bw, h = rep & 1 ? ah : bh;
            viewport_set(3, 5, H.scr_w - 7, H.scr_h - 9);
            H.tex_filter = 3; H.shaders_on = 1; H.pal_gen = 77;
            PS2HWD_SetScreenSize(w, h);
            check_view(w, h);
            EXPECT(starts == nstart && stops == nstop, "same-FB branch restarted the driver");
            EXPECT(H.tex_filter == 3 && H.shaders_on == 1 && H.pal_gen == 77, "same-FB state lost");
            cases++;
        }
    }
    printf("HIGHRES SAME_FB changes=%d\n", cases);
    group_end("every equivalent-layout pair, six alternating changes each after an inset viewport");
}

static void restart_and_keys(void)
{
    int output, a, b, cases = 0;
    group_begin("restart_state_owner_keys");
    for (output = 0; output < 3; output++) for (a = 0; a < 11; a++) for (b = 0; b < 11; b++)
    {
        int bw, bh, x, y, field, ri, nstart, nstop;
        u32 pal[256];
        GLMipmap_t owner;
        vukey_t before, after;
        start_mode(output, a);
        ps2vm_mode_size(b, &bw, &bh);
        hw_choose_size(output, bw, bh, &x, &y, &field);
        if (x == H.vw && y == H.vh) continue;
        memset(&owner, 0, sizeof owner);
        ri = rec_new(); EXPECT(ri != NOREC, "cannot create owner record");
        H.rec[ri].owner = &owner; owner.downloaded = (unsigned)(ri + 1);
        for (int i = 0; i < 256; i++) H.pal[i] = pal[i] = (u32)i * 0x010101u;
        H.pal_set = 1; H.tex_filter = 3; H.shaders_on = 1;
        nstart = starts; nstop = stops;
        PS2HWD_SetScreenSize(bw, bh);
        check_view(bw, bh);
        EXPECT(starts == nstart + 1 && stops == nstop + 1, "different-FB branch did not restart exactly once");
        EXPECT(owner.downloaded == 0, "GS restart left stale engine handle");
        EXPECT(H.pal_set && !memcmp(H.pal, pal, sizeof pal) && H.tex_filter == 3 && H.shaders_on, "palette/filter/shader restore changed");
        viewport_set(0, 0, bw, bh); xf_update(); vu_key_make(&before, PF_NoTexture, NULL);
        viewport_set(0, 1, bw, bh - 1); vu_key_make(&after, PF_NoTexture, NULL);
        EXPECT(vu_key_cmp(&before, &after) == 0, "VU material plan accepted stale viewport/scissor");
        viewport_set(0, 0, bw, bh);
        EXPECT(begin_draw(PF_NoTexture | PF_NoDepthTest, NULL), "2D draw refused");
        EXPECT(fabsf(P.kx - H.vw * 8.0f) < 0.001f && fabsf(P.ky + H.vh * 8.0f) < 0.001f, "GS coordinate scale stale");
        EXPECT(P.ox - P.kx >= 0 && P.ox + P.kx < 65536 && P.oy + P.ky >= 0 && P.oy - P.ky < 65536, "GS 16-bit coordinates overflow");
        cases++;
    }
    printf("HIGHRES RESTART changes=%d\n", cases);
    group_end("all different-layout mode pairs; actual owner cleanup and VU keys; GS lifecycle simulated");
}

int main(void)
{
    cap = calloc(CAP_MAX, sizeof *cap);
    if (!cap) return 2;
    modes_and_layout();
    same_framebuffer_transitions();
    restart_and_keys();
    if (H.up) PS2HWD_Shutdown();
    printf("HIGHRES COMPLETE failed=%d\n", total_fail);
    return total_fail != 0;
}
