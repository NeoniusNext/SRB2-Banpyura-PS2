/* The extracted functions in epoch-production.inc are actual planner source. */
#define main base_suite_main
#include "base_hosttest.c"
#undef main
unsigned int hwr_geo_off;
#include "epoch-production.inc"
#include "epoch-lifecycle.inc"
static int epoch_start_calls, epoch_stop_calls;
static int epoch_fence_calls;
int PS2HWD_Sync(void) { epoch_fence_calls++; return 1; }
boolean PS2HWD_Init(void) { epoch_start_calls++; return true; }
void PS2HWD_Shutdown(void) { epoch_stop_calls++; }
#include "size-production.inc"

static void run_case(const char *name, u32 old_stamp)
{
    GLMipmap_t retained, fresh;
    int visible, stale_registration, stale_full;
    int stale_hint, revived_full, revived_hint;
    u32 keep_lod;
    memset(&retained, 0, sizeof retained);
    memset(&fresh, 0, sizeof fresh);
    epoch_state_init();
    H.up = 1;
    epoch_fixture_clock_init();
    plan_n = 0; plan_frame = 0;
    plan_info_p = calloc(1, sizeof *plan_info_p);
    if (!plan_info_p) exit(2);
    /* Real registration during the previous driver lifetime's frame zero. */
    H.frame_no = 0;
    if (!plan_key(&retained)) exit(3);
    retained.ps2_want = 3;
    retained.ps2_vis = 1;
    retained.ps2_full_fr = old_stamp;
    retained.ps2_ihfr = old_stamp;
    retained.ps2_ihint = 4;
    /* Model 200 completed frames using the source's exact counter clocks. */
    epoch_tick_frames(200);
    if (old_stamp != 1 && !plan_key(&retained)) exit(10);
    /* Actual Shutdown's metadata cleanup, then its state reset statements.
     * Engine-owned GLMipmap deliberately survives a same-level mode restart. */
    dc_flush();
    tex_free_all();
    plan_reset();
    epoch_state_shutdown();
    epoch_state_init();
    H.up = 1;
    stale_registration = plan_key(&retained) == NULL && plan_n == 0;
    if (!plan_key(&fresh)) exit(4);
    /* At a later newly registered frame the old full-size hint revives. */
    epoch_tick_frames(1);
    if (!plan_key(&retained)) exit(5);
    retained.ps2_want = 3;
    retained.ps2_vis = 1;
    batch_phase = 2;
    stale_full = epoch_tex_want(&retained, &visible) == 0;
    stale_hint = epoch_hint_live(&retained);
    epoch_tick_frames(199);
    if (!plan_key(&retained)) exit(6);
    retained.ps2_want = 3; retained.ps2_vis = 1;
    revived_full = epoch_tex_want(&retained, &visible) == 0;
    revived_hint = epoch_hint_live(&retained);
    retained.ps2_full_fr = 0;
    keep_lod = epoch_tex_want(&retained, &visible);
    printf("HIGHRES EPOCH case=%s stale_registration=%d stale_full_hint=%d stale_immediate_hint=%d delayed_full_revival=%d delayed_immediate_revival=%d planned_lod_without_stale_hint=%u frame_no=%u\n",
        name, stale_registration, stale_full, stale_hint, revived_full, revived_hint, keep_lod, H.frame_no);
    free(plan_info_p);
    free(H.rec);
}

int main(void)
{
    host_init(1);
    run_case("expired", 1);
    run_case("recent", 201);
    /* A logical resize with unchanged 640x480 storage also invalidates hints. */
    {
        GLMipmap_t retained;
        int visible, stale_full, stale_hint;
        u32 keep_lod;
        memset(&retained, 0, sizeof retained);
        epoch_state_init(); epoch_fixture_clock_init(); H.frame_no = 0;
        H.up = 1; H.mode = 2;
        H.vw = H.fbw = 640; H.vh = H.fbh = 480;
        H.scr_w = 640; H.scr_h = 400;
        H.sx = 1; H.sy = 1.2f;
        cfg.fbw = cfg.fbh = 0;
        plan_n = 0; plan_frame = 0;
        plan_info_p = calloc(1, sizeof *plan_info_p);
        if (!plan_info_p) return 7;
        epoch_tick_frames(99);
        if (!plan_key(&retained)) return 8;
        retained.ps2_full_fr = retained.ps2_planfr;
        retained.ps2_ihfr = retained.ps2_planfr;
        epoch_tick_frames(1);
        PS2HWD_SetScreenSize(640, 480);
        if (!plan_key(&retained)) return 9;
        retained.ps2_want = 3; retained.ps2_vis = 1; batch_phase = 2;
        stale_full = epoch_tex_want(&retained, &visible) == 0;
        stale_hint = epoch_hint_live(&retained);
        retained.ps2_full_fr = 0;
        keep_lod = epoch_tex_want(&retained, &visible);
        printf("HIGHRES EPOCH case=same_framebuffer stale_registration=%d stale_full_hint=%d stale_immediate_hint=%d delayed_full_revival=0 delayed_immediate_revival=0 planned_lod_without_stale_hint=%u frame_no=%u\n",
            epoch_start_calls || epoch_stop_calls, stale_full, stale_hint, keep_lod, H.frame_no);
        free(plan_info_p);
    }
    epoch_wrap_test();
    return 0;
}
