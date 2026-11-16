#!/usr/bin/env python3
"""Prepare a private SKY-only adjacent-prefix pressure-borrow candidate."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil


HELPERS = r'''
static void tile_sky_pressure_reset(void)
{
    tile_cache.sky_pressure = tile_cache.sky_failed = 0;
}

// Revoke before an external error callback can longjmp without Shutdown.
// State2 is fail-closed until Init/Shutdown explicitly resets it. No flush,
// allocation, reclaim or recursion is permitted from this error boundary.
static void tile_sky_pressure_revoke(void)
{
    tile_cache.sky_pressure = 2;
    tile_cache.sky_failed = 1;
}

static unsigned tile_sky_pressure_enter(void)
{
    const unsigned saved = tile_cache.sky_pressure | (tile_cache.sky_failed << 1);
    if (tile_cache.sky_pressure == 2) return saved;
    if (!tile_cache.sky_pressure) tile_cache.sky_failed = 0;
    tile_cache.sky_pressure = 1;
    return saved;
}

static void tile_sky_pressure_leave(unsigned saved)
{
    if (!H.up) { tile_sky_pressure_reset(); return; }
    if (tile_cache.sky_pressure != 1) return;
    const unsigned failed = tile_cache.sky_failed;
    (void)tile_cache_return_borrow(); // old readers precede ordinary reuse in the same GIF stream
    if (!H.up) { tile_sky_pressure_reset(); return; }
    if (tile_cache.sky_pressure != 1) return;
    tile_cache.sky_pressure = saved & 1u;
    tile_cache.sky_failed = (saved >> 1) | ((saved & 1u) ? failed : 0u);
}

// tex_drop also drops the paired physical piece and all its shared views.
// Check that entire closure before the first mutation, including protections
// that normally reach the physical owner through the renderer's selected view.
static int tile_sky_pressure_physical_allowed(int image)
{
    if (image < 0 || image >= H.rec_n) return 0;
    const texrec_t *r = &H.rec[image];
    if (!r->used || !r->nblk || r->share != NOREC || r->tiled || r->pin || r->screen
        || r->wgt == WEIGHT_STAY || H.cur_tex == image
        || (batch_phase == 1 && r->stamp == H.frame_no)) return 0;
    int views = 0;
    for (int v = r->sec; v != NOREC; v = H.rec[v].sec_next)
    {
        if (v < 0 || v >= H.rec_n || ++views > H.rec_n) return 0;
        const texrec_t *view = &H.rec[v];
        if (!view->used || view->share != image || view->pin || view->screen
            || H.cur_tex == v || (batch_phase == 1 && view->stamp == H.frame_no)) return 0;
    }
    return 1;
}

static int tile_sky_pressure_allowed(int image)
{
    if (!tile_sky_pressure_physical_allowed(image)) return 0;
    const texrec_t *r = &H.rec[image];
    if (r->ppar != NOREC)
    {
        if (!tile_sky_pressure_physical_allowed(r->ppar) || H.rec[r->ppar].pnext != image) return 0;
    }
    if (r->pnext != NOREC)
    {
        if (!tile_sky_pressure_physical_allowed(r->pnext) || H.rec[r->pnext].ppar != image) return 0;
    }
    return 1;
}

static int tile_cache_try_borrow(void)
{
    if (tile_cache_try_borrow_free()) return 1;
    if (tile_cache.sky_pressure != 1 || tile_cache.sky_failed) return 0;
    // At most one failed pressure preflight per dome. Free-prefix borrowing
    // above remains available even after this memo is set.
    tile_cache.sky_failed = 1;
    ov_flush_all();
    if (tile_cache.sky_pressure != 1) return 0;
    if (tile_cache_rec < 0 || tile_cache_rec >= H.rec_n
        || !H.rec[tile_cache_rec].used || H.rec[tile_cache_rec].tiled != 2
        || H.rec[tile_cache_rec].nblk != 64) return 0;
    const u32 anchor = H.rec[tile_cache_rec].blk;
    if ((anchor & (PAGE_BLOCKS - 1u)) || anchor < H.pool_base + 64u
        || anchor + 64u > H.pool_base + H.pool_blocks || anchor + 64u > VRAM_BLOCKS) return 0;
    const u32 prefix = (anchor - 64u);
    int victims[64], victim_n = 0, free_at = 0;
    weights_prepare();
    for (u32 block = prefix; block < anchor; block++)
    {
        const u16 owner = blk_owner[block];
        if (!owner)
        {
            while (free_at < H.free_n && (u32)H.free_range[free_at].start + H.free_range[free_at].len <= block) free_at++;
            if (free_at == H.free_n || H.free_range[free_at].start > block) return 0;
            continue;
        }
        const int image = (int)owner - 1;
        if (!tile_sky_pressure_allowed(image)
            || block < H.rec[image].blk || block >= (u32)H.rec[image].blk + H.rec[image].nblk) return 0;
        int at;
        for (at = 0; at < victim_n && victims[at] != image; at++) {}
        if (at == victim_n) victims[victim_n++] = image;
    }
    // Every occupied block and every paired/view closure is now approved.
    // Physical records own no tile_source lease/palette history: tex_drop
    // releases GPU ownership only and retains all canonical CPU pixels.
    const u32 used_before = H.used_blocks;
    for (int i = 0; i < victim_n; i++)
        if (H.rec[victims[i]].used) tex_drop(victims[i], 1);
    const int borrowed = tile_cache_try_borrow_free();
    if (borrowed) tile_cache.sky_failed = 0;
    if (ps2hwd_dbg_flags & HWDBG_IMMDBG)
        CONS_Printf("HWSKYPRESS f=%u victims=%d released_blocks=%u borrowed=%d capacity=%u/%u\n",
            (unsigned)H.frame_no,victim_n,(unsigned)(used_before-H.used_blocks+(borrowed?64u:0u)),borrowed,
            (unsigned)tex_usable_blocks(),(unsigned)H.pool_blocks);
    return borrowed;
}
'''

WRAPPER = r'''
static void hw_RenderSkyDome(gl_sky_t *sky)
{
    // Match the original no-op boundary: invalid domes must not return a
    // previous caller's optional borrow or change its queued state.
    if (!H.up || !sky || !sky->data || !sky->loops || sky->vertex_count <= 0)
        return;
    const unsigned saved = tile_sky_pressure_enter();
    hw_RenderSkyDome_inner(sky);
    tile_sky_pressure_leave(saved);
}

'''


def once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError('Missing/ambiguous SKY candidate anchor: ' + repr(old))
    return text.replace(old, new)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    base, out = a.base.resolve(), a.out.resolve()
    shutil.copytree(base, out)
    tile = out / 'src/ps2/hw/ps2_hw_tile.inc'
    original_tile = tile.read_text()
    text = once(original_tile, '\tu8 screen, psm;\n', '\tu8 screen, psm;\n\tu8 sky_pressure, sky_failed; // existing EE tail padding\n')
    text = once(text, 'static int tile_cache_try_borrow(void)', 'static int tile_cache_try_borrow_free(void)')
    at = text.index('static int tile_coord(')
    text = text[:at] + HELPERS + '\n' + text[at:]
    tile.write_text(text)
    driver = out / 'src/ps2/hw/ps2_hwd.c'
    original_driver = driver.read_text()
    text = once(original_driver, 'static void hw_RenderSkyDome(gl_sky_t *sky)', 'static void hw_RenderSkyDome_inner(gl_sky_t *sky)')
    at = text.index('static void hw_SetBlend(')
    text = text[:at] + WRAPPER + text[at:]
    text = once(text, 'boolean PS2HWD_Init(void)\n{\n\tint rc;', 'boolean PS2HWD_Init(void)\n{\n\tint rc;\n\n\ttile_sky_pressure_reset();')
    # Reset even a no-op/repeated shutdown after a failed startup.
    at = text.index('void PS2HWD_Shutdown(void)')
    shutdown = text[at:]
    brace = shutdown.index('{')
    insert = shutdown.index('\n', shutdown.index('int i;', brace))
    shutdown = shutdown[:insert] + '\n\ttile_sky_pressure_reset();' + shutdown[insert:]
    driver.write_text(text[:at] + shutdown)
    text = driver.read_text()
    text = once(text, 'void PS2HWD_Abort(void)\n{\n\tu32 irq;',
        'void PS2HWD_Abort(void)\n{\n\tu32 irq;\n\ttile_sky_pressure_revoke();')
    driver.write_text(text)
    private = out / 'src/ps2/hw/ps2_hw_priv.inc'
    original_private = private.read_text()
    text = once(original_private, 'static void hw_failure(const char *message)',
        'static void tile_sky_pressure_revoke(void);\n\nstatic void hw_failure(const char *message)')
    text = once(text, 'static void hw_failure(const char *message)\n{',
        'static void hw_failure(const char *message)\n{\n\ttile_sky_pressure_revoke();')
    private.write_text(text)
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    restored = tile.read_text().replace(HELPERS + '\n', '')
    restored = restored.replace('\tu8 sky_pressure, sky_failed; // existing EE tail padding\n', '')
    restored = restored.replace('static int tile_cache_try_borrow_free(void)', 'static int tile_cache_try_borrow(void)')
    if restored != original_tile:
        raise RuntimeError('Original allocator/conversion helper literal proof failed')
    report = dict(scope=__doc__, base=str(base), source=str(out), emulator_used=False,
        permanent_cache_blocks=64, ordinary_capacity_unchanged=True,
        original_tile_body_restored_byte_for_byte=True,
        original_source_sha256={name:sha(base / 'src/ps2/hw' / name) for name in ['ps2_hw_tile.inc','ps2_hwd.c','ps2_hw_tile_draw.inc','ps2_hw_priv.inc']},
        candidate_source_sha256={name:sha(out / 'src/ps2/hw' / name) for name in ['ps2_hw_tile.inc','ps2_hwd.c','ps2_hw_tile_draw.inc','ps2_hw_priv.inc']},
        helper_sha256=sha(Path(__file__)))
    (out.parent / 'source-proof.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__ == '__main__':
    main()
