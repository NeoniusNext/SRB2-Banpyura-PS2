/* Host platform hooks/capture are supplied by the existing renderer fixture.
 * Every renderer decision and diagnostic ownership write comes from production. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char last_skip[1024];
static unsigned skip_reports;
static void fixture_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void fixture_log(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    if (!strncmp(format, "HWSKIP ", 7)) {
        vsnprintf(last_skip, sizeof last_skip, format, ap);
        skip_reports++;
    }
    va_end(ap);
}

static unsigned char *reference_data;
static size_t reference_bytes;
static void fixture_capture_ref(const void *data, size_t bytes)
{
    unsigned char *next = realloc(reference_data, reference_bytes + bytes);
    if (!next && bytes) { fprintf(stderr, "reference capture allocation failed\n"); exit(4); }
    reference_data = next;
    if (bytes) memcpy(reference_data + reference_bytes, data, bytes);
    reference_bytes += bytes;
}

/* PRODUCTION_DRIVER */

static unsigned checks;
static void require(int ok, const char *what)
{
    checks++;
    if (!ok) { fprintf(stderr, "SKIP BOOKKEEPING FAIL: %s\n", what); exit(3); }
}

static GLMipmap_t *make_map(int id, unsigned width, unsigned height)
{
    GLMipmap_t *m = calloc(1, sizeof *m);
    unsigned i;
    require(m != NULL, "metadata allocation");
    m->width = width; m->height = height;
    m->format = GL_TEXFMT_P_8;
    m->regen_kind = 1; m->regen_id = id;
    m->data = malloc((size_t)width * height);
    require(m->data != NULL, "texture allocation");
    for (i = 0; i < width * height; i++) ((u8 *)m->data)[i] = (u8)(i % 193);
    return m;
}

static void initialize(void)
{
    cap = calloc(CAP_MAX, sizeof *cap);
    require(cap != NULL, "capture allocation");
    host_init(1);
    plan_info_p = calloc(1, sizeof *plan_info_p);
    require(plan_info_p != NULL, "planner allocation");
    H.frame_no = 26; plan_clock = 27;
    H.shader = 1;
    m_identity(H.mv); m_identity(H.proj); m_identity(H.mvp);
    H.mat_dirty = 0;
    H.vpx = H.vpy = 0; H.vpw = 400; H.vph = 300;
}

static void finish(void)
{
    unsigned i;
    for (i = 0; i < DC_MAX; i++) dc_drop(&dc[i]);
    tex_free_all();
    free(H.rec); H.rec = NULL;
    free(plan_info_p); plan_info_p = NULL;
    free(blk_owner_p); blk_owner_p = NULL;
    free(ovq_p); ovq_p = NULL;
    free(cutbuf_p); cutbuf_p = NULL;
    free(cap); cap = NULL;
    free(reference_data); reference_data = NULL; reference_bytes = 0;
}

/* This is the production single-draw decision sequence, with its return exposed.
 * VU execution is disabled on this host; emit_fan writes the actual GIF stream. */
static int draw(const FOutVector *v, const FSurfaceInfo *s)
{
    int result;
    imm_prepare(v, 4);
    result = begin_draw_inner(PF_Translucent | PF_Modulated | PF_ColorMapped, s);
    if (result) emit_fan(v, NULL, 4, NULL);
    return result;
}

static void require_current_owner(const GLMipmap_t *m, const GLMipmap_t *old)
{
    if (EXPECT_FIXED) {
        require(skip_tex == m && !strcmp(skip_why, "immediate polygon outside view"),
                "current immediate texture owns culled diagnostic");
    } else {
        require(skip_tex == old && !strcmp(skip_why, "selected while collecting"),
                "baseline retains the prior collection owner/reason");
    }
}

static void require_delete_owner(const GLMipmap_t *m, const char *message)
{
    if (EXPECT_FIXED) require(skip_tex == NULL && skip_why == NULL, message);
    else require(skip_tex == m, "baseline retains deleted diagnostic owner");
    /* Match post-check fixture state before freeing metadata in both variants.
     * The separate --freed-owner control performs no such baseline repair. */
    skip_tex = NULL; skip_why = NULL;
}

static int freed_owner_control(void)
{
    GLMipmap_t *m = calloc(1, sizeof *m);
    require(m != NULL, "freed owner allocation");
    m->width = m->height = 64; m->format = GL_TEXFMT_AP_88;
    skip_tex = m; skip_why = "selected while collecting";
    H.imm_tex = m; H.cur_tex = NOREC; H.cur_missing = 1;
    hw_DeleteTexture(m);
    free(m);
    /* Baseline must trip ASan in the actual HWSKIP formatting expressions.
     * Fixed source reaches the same rejection/counter without a stale read. */
    require(!begin_draw_inner(PF_Translucent | PF_Modulated, NULL), "freed owner draw remains rejected");
    require(skip_tex == NULL && skip_why == NULL, "freed owner callback cleared diagnostics");
    require(H.st.tex_skipped == 1 && skip_reports == 0, "freed owner rejection/counter preserved");
    finish();
    printf("{\"safe_after_actual_delete_and_free\":true,\"skipped\":1}\n");
    return 0;
}

int main(int argc, char **argv)
{
    const FOutVector outside[4] = {{2,-.5f,0,0,0},{3,-.5f,0,1,0},{3,.5f,0,1,1},{2,.5f,0,0,1}};
    const FOutVector visible[4] = {{-.5f,-.5f,0,0,0},{.5f,-.5f,0,1,0},{.5f,.5f,0,1,1},{-.5f,.5f,0,0,1}};
    FSurfaceInfo surf = {0};
    GLMipmap_t patch = {0}, *a, *b, *dead;
    unsigned i, used, qwords_after_first_cull;
    int r[4];
    FILE *packets;
    ps2hwd_stats_t stats;
    u64 captured_sizes[2];
    initialize();
    if (argc == 2 && !strcmp(argv[1], "--freed-owner")) return freed_owner_control();
    require(argc == 2, "packet output argument");
    a = make_map(1, 256, 256); b = make_map(2, 512, 256);
    patch.width = patch.height = 64; patch.format = GL_TEXFMT_AP_88;
    surf.PolyColor.rgba = 0xFFFFFFFFu;
    PS2HWD_BatchBegin(); hw_SetTexture(&patch);
    require(H.cur_missing && H.cur_tex == NOREC && skip_tex == &patch,
            "actual collection selection defers patch upload");
    require(!strcmp(skip_why, "selected while collecting"), "actual collection skip reason");
    PS2HWD_BatchEnd();
    hw_SetTexture(a);
    require(H.imm_tex == a && H.st.uploads == 0, "actual immediate selection defers map upload");
    require(plan_measure(a, outside, 4) == 0 && plan_measure(a, visible, 4) > 0,
            "actual clipper distinguishes the explicit outside/visible quads");
    r[0] = draw(outside, &surf); qwords_after_first_cull = cap_n;
    require(!r[0] && H.st.uploads == 0 && TX.invisible == 1 && H.st.tex_skipped == 1,
            "first cull preserves rejection and counter behavior without upload");
    require_current_owner(a, &patch);
    require(cap_n == 0, "culled geometry emits no GIF data");
    r[1] = draw(visible, &surf);
    require(r[1] && H.st.uploads > 0 && H.st.polys > 0, "visible control uploads and draws");
    hw_SetTexture(b);
    r[2] = draw(outside, &surf); require_current_owner(b, &patch);
    r[3] = draw(visible, &surf);
    require(!r[2] && r[3], "second map cull/visible returns");
    for (i = 0; i < 40; i++) require(!draw(outside, &surf), "repeated culled geometry remains rejected");
    require_current_owner(b, &patch);
    require(skip_reports == 40 && H.st.tex_skipped == 42 && TX.invisible == 42,
            "original forty-report cap and all skipped/invisible counters preserved");
    require(strstr(last_skip, EXPECT_FIXED ? "immediate polygon outside view" : "selected while collecting") != NULL,
            "diagnostics remain visible under the existing filter");
    require(regenerate_calls == 0, "existing source pixels require no regeneration");
    stats = H.st; used = H.used_blocks;
    packets = fopen(argv[1], "wb"); require(packets != NULL, "open complete GIF capture");
    captured_sizes[0] = (u64)cap_n * sizeof *cap; captured_sizes[1] = reference_bytes;
    require(reference_bytes == stats.upload_bytes, "all actual texture DMA reference bytes captured");
    require(fwrite(captured_sizes, sizeof captured_sizes, 1, packets) == 1, "write capture lengths");
    require(fwrite(cap, sizeof *cap, cap_n, packets) == cap_n, "write complete inline GIF capture");
    require(fwrite(reference_data, 1, reference_bytes, packets) == reference_bytes, "write all referenced payload bytes"); fclose(packets);
    hw_DeleteTexture(NULL); hw_DeleteTexture(&patch);
    require_current_owner(b, &patch);
    skip_tex = b; skip_why = "immediate polygon outside view";
    hw_DeleteTexture(b);
    require_delete_owner(b, "matching live deletion clears diagnostic owner");
    require(H.imm_tex == NULL && b->downloaded == 0, "actual live delete clears selected map and residency");
    free(b->data); free(b);
    hw_DeleteTexture(a); free(a->data); free(a);
    dead = calloc(1, sizeof *dead); require(dead != NULL, "inactive metadata allocation");
    skip_tex = dead; skip_why = "selected while collecting";
    H.up = 0; hw_DeleteTexture(dead);
    require_delete_owner(dead, "matching inactive deletion clears diagnostic owner");
    free(dead); H.up = 1;
    printf("{\"behavior\":{\"returns\":[%d,%d,%d,%d],\"skipped\":%u,\"invisible\":%u,\"uploads\":%u,\"upload_bytes\":%u,\"polys\":%u,\"vertices_in\":%u,\"vertices_out\":%u,\"rejected\":%u,\"qwords\":%u,\"pool_used\":%u,\"first_cull_qwords\":%u,\"captured_inline_bytes\":%llu,\"captured_reference_bytes\":%llu,\"skip_reports\":%u,\"regenerate_calls\":%u},\"fixed_ownership\":%s,\"checks\":%u}\n",
        r[0],r[1],r[2],r[3],stats.tex_skipped,TX.invisible,stats.uploads,stats.upload_bytes,
        stats.polys,stats.verts_in,stats.verts_out,stats.rejected,stats.qwords,used,
        qwords_after_first_cull,(unsigned long long)captured_sizes[0],(unsigned long long)captured_sizes[1],skip_reports,regenerate_calls,
        EXPECT_FIXED ? "true" : "false",checks);
    finish();
    return 0;
}
