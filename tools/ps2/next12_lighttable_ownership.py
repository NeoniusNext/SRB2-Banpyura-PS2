"""Archive the source ownership argument for deferred indexed light-table reads.

This is a bounded, reviewable source audit, not a claim that temporal fixtures
alone prove callback behavior. Complete callback bodies and all source hashes
are retained so the return/unwind and ownership argument can be inspected.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

from next12_lighttable_hosttest import function

ROOT = Path(__file__).resolve().parents[2]
NODES = {
    'ps2/hw/ps2_hw_draw.inc': ('ov_flush_all', 'ov_flush', 'tex_regs', 'state_flush'),
    'ps2/hw/ps2_hw_gs.inc': ('pk_alloc', 'ad_alloc', 'pk_flush', 'ring_wait', 'poll_dma', 'dma_start_locked', 'wd_recover'),
    'ps2/hw/ps2_hw_vu1.inc': ('vu_chunk_end', 'vu_chunk_flush'),
    'ps2/hw/ps2_hw_tex.inc': ('rel_release_buf', 'rel_release_all', 'rel_poll'),
    'hardware/hw_cache.c': ('HWR_PS2_UnlockData', 'HWR_GetTexturePalette', 'HWR_CreateLightTable', 'HWR_UpdateLightTable'),
    'ps2/ps2_spill.c': ('Spill_Allowed', 'Spill_Alloc', '__wrap__malloc_r'),
    'z_zone.c': ('Z_Evictable', 'Z_FreeBlock', 'Z_AllocBlock', 'Z_Reclaim', 'Z_AddReclaimHook',
                 'Z_SetReclaimHook', 'Z_MallocInternal', 'Z_TryMallocAlign', 'Z_GuardThrow', 'Z_OutOfMemory', 'Z_GuardLanded'),
    'ps2/i_sound.c': ('ReclaimSamples', 'ReleaseSample'),
    'ps2/ps2_models.c': ('Reclaim', 'Pend_Cancel', 'PS2Models_Free'),
    'ps2/hw/ps2_hw_model.inc': ('PS2HWD_ModelWorkReclaim',),
    'hardware/hw_gcache.inc': ('HWR_GCacheReclaim',),
    'lua_script.c': ('LUA_PoolReclaim', 'LUA_PoolTrim', 'LUA_InvalidateUserdata'),
    'blua/lapi.c': ('lua_getfield', 'lua_rawget', 'lua_rawgeti', 'lua_rawset', 'lua_pushnil', 'lua_pushlightuserdata'),
    'blua/lstring.c': ('luaS_newlstr',),
    'ps2/i_system.c': ('PS2_SleepUs', 'I_OutputMsg', 'I_Error'),
    'ps2/i_video.c': ('Impl_HWFailure',),
    'v_video.c': ('LoadPalette',),
    'r_data.c': ('R_CreateLightTable', 'R_UpdateLightTable'),
}


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'build/nov15-next12/baseline-src/src')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args(); source = args.source.resolve(); out = args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'): raise SystemExit('Fresh immutable source audit stage required')
    expected = json.loads((ROOT / 'tools/ps2/next12_baseline.json').read_text())['source_sha256']
    observed = {'src/' + str(p.relative_to(source)): sha(p) for p in sorted(source.rglob('*')) if p.is_file()}
    if observed != expected or len(observed) != 602: raise SystemExit('Complete source audit baseline differs')
    out.mkdir(parents=True); shutil.copytree(source, out / 'source')
    pinned = out / 'pinned-tools'; pinned.mkdir()
    for path in (Path(__file__).resolve(), ROOT / 'tools/ps2/next12_lighttable_hosttest.py', ROOT / 'tools/ps2/next12_baseline.json'):
        shutil.copyfile(path, pinned / path.name)
    bodies = {}; registrations = []
    for name, funcs in NODES.items():
        text = (source / name).read_text()
        for func in funcs:
            body = function(text, func); target = out / 'functions' / name / (func + '.c')
            target.parent.mkdir(parents=True, exist_ok=True); target.write_text(body + '\n')
            bodies[name + ':' + func] = {'path': str(target), 'sha256': sha(target),
                'source_line': text[:text.index(body)].count('\n') + 1}
    for path in sorted(source.rglob('*')):
        if not path.is_file() or path.suffix not in ('.c', '.inc'): continue
        text = path.read_text(errors='replace')
        for m in re.finditer(r'Z_(Add|Set)ReclaimHook\((\w+)\)', text):
            registrations.append({'file': str(path.relative_to(source)), 'line': text[:m.start()].count('\n') + 1,
                                  'kind': m[1], 'callback': m[2]})
    found = {r['callback'] for r in registrations} - {'NULL'}
    if found != {'ReclaimSamples', 'LUA_PoolReclaim', 'HWR_GCacheReclaim', 'Reclaim'}:
        raise SystemExit('Actual subsystem callback registration inventory changed')
    statements = [
        {'claim': 'The old RGBA table is never submitted as a historical snapshot without refill.',
         'evidence': 'HWR_CreateLightTable and HWR_UpdateLightTable each call HWR_GetTexturePalette and overwrite every8192 RGBA entry before the driver call. Stable IDs perform no submission. Complete602-source data-member census is independently available in the source tree.'},
        {'claim': 'Overlay flush does not invoke table creation, palette setters, map loading, or a game/script tick.',
         'evidence': 'ov_flush_all checks OV.n; ov_flush only reads the already resident ramp record, computes GS registers, state_flush/ad_alloc emits registers, pk_alloc reserves packet words, memcpy copies queued words, increments a counter and clears OV.n. It does not call ramp_get or a texture upload. Ring rollover closes VU chunks, validates/emits DMA, waits, and unlocks referenced texture-data cache blocks.'},
        {'claim': 'Successful DMA wait/watchdog service cannot free or mutate the engine forward palettes or source colormap.',
         'evidence': 'Ring/DMA/validator paths operate on H/V/VU/GS packets and texture lock tags. rel_release_buf calls HWR_PS2_UnlockData, which only changes the referenced texture-data tag. PS2_SleepUs changes priority and polls the timer; it does not run NetUpdate or a game tick. Audio/network threads cannot spill into or operate the main-thread-owned zone.'},
        {'claim': 'The driver8192-byte allocation may reclaim caches but cannot evict either input.',
         'evidence': 'Actual default links wrap _malloc_r via ps2_spill.c. Spill_Allowed restricts zone spills to the game thread and spill_busy prevents recursive spilling. Spill_Alloc uses nonfatal Z_TryMallocAlign(...PU_STATIC...). Z_Evictable selects owned PU_CACHE/PU_HWRCACHE_LRU/evictable sprite blocks; emergency ranges free only purgeable or hardware-cache tags. pLocalPalette is PU_STATIC, mapPalette is static storage, and R_CreateLightTable/source lump tables use PU_LEVEL. PU_PURGELEVEL is100. None of those inputs is a purgeable cache block.'},
        {'claim': 'The complete registered reclaim-hook set does not execute gameplay or change these inputs.',
         'evidence': 'ReclaimSamples releases idle decoded audio buffers; model Reclaim frees model/work/pending-model memory; HWR_GCacheReclaim detaches and frees only its geometry/state-cache block; LUA_PoolReclaim trims already empty allocator slabs and does not run lua_gc or a script. The full registration census is checked above.'},
        {'claim': 'Invalidating cache userdata does not invoke Lua code or a collection callback.',
         'evidence': 'LUA_InvalidateUserdata performs registry/raw table reads, sets raw entries to nil and nulls userdata. The registry tables and LREG_EXTVARS string were created during LUA_Load; luaS_newlstr returns the existing interned key. These lapi raw/push/get operations have no luaC_checkGC or lua_call. lua_rawset uses a write barrier, not a GC step. An allocator/error throw cannot resume the suspended store.'},
        {'claim': 'An unsuccessful service cannot return and continue reading freed inputs.',
         'evidence': 'Z_TryMallocAlign passes fatal=false and returns NULL after failed reclaim; lt_store returns before any input read on malloc failure. A driver fatal handler is Impl_HWFailure: Z_GuardThrow longjmps, otherwise I_Error shuts down and PS2Boot_Exit does not return. Z_OutOfMemory similarly longjmps or exits. Thus the successful-return path retains both inputs; abandoned calls never execute the later reads.'},
        {'claim': 'Old and new palette-capture points therefore see the same RGB basis on every continuing invocation.',
         'evidence': 'Between old engine refill and new direct store read, the only intervening calls are the reviewed flush/allocation services above. Neither successful service changes either input or reenters the renderer. A palette change between invocations is covered by the real source refill rule and the temporal host corpus; context restart preserves driver canonical rows.'},
    ]
    report = {'scope': 'Manual source-ownership argument with exact complete sources and callback bodies; supplements temporal host tests and actual LTO/native binding',
              'all602_source_sha256': observed, 'complete_callback_function_bodies': bodies,
              'all_actual_reclaim_registrations': registrations, 'ownership_arguments': statements,
              'external_boundary': 'EE kernel/DMA and standard newlib allocation/stdio services; their engine hooks are the pinned default link wrappers. No arbitrary service plugin or script callback is assumed.',
              'pinned_tool_sha256': {p.name: sha(p) for p in pinned.iterdir()}}
    (out / 'ownership-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Archived bounded within-call source ownership audit: ' + str(out))


if __name__ == '__main__': main()
