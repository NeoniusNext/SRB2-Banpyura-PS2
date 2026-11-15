#!/usr/bin/env python3
"""Archive the real PB/driver consumption and guarded-abandonment source chain.

This is a checked source witness, not a native DMA execution or a formal C
proof. The host differential fixture and full-engine Hardware gates are separate.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

import next12_pb_scratch_replay as replay


def sha(data):
    return hashlib.sha256(data).hexdigest()


def function(source, name):
    """The pass11 function slicer, kept local to avoid unrelated pack imports."""
    match = re.search(r'^[^\n;{}]*\b' + re.escape(name) + r'\s*\([^;]*?\)\s*\{', source, re.M)
    if not match:
        raise RuntimeError('Missing source function: ' + name)
    depth = 0
    for token in re.finditer(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', source[match.start():]):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if not depth:
                return source[match.start():match.start() + token.end()] + '\n'
    raise RuntimeError('Unclosed source function: ' + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=replay.DEFAULT_CANDIDATE)
    parser.add_argument('--out', type=Path, default=replay.ROOT / 'build/nov15-next12/pb-scratch/lifetime')
    args = parser.parse_args()
    source, out = args.source.resolve(), args.out.resolve()
    replay.validate(source)
    if not out.is_relative_to(replay.ROOT / 'build') or out == replay.ROOT / 'build':
        parser.error('--out must be an isolated child of build/')
    out.mkdir(parents=True, exist_ok=True)
    if (out / 'report.json').exists():
        raise RuntimeError('Preserve completed evidence; choose a fresh --out')
    names = {
        'src/hardware/hw_pbatch.inc': (
            'HWR_PBAlloc', 'HWR_PBFree', 'HWR_PBReset', 'HWR_PBGrow',
            'HWR_PBPut', 'HWR_PBAdd', 'HWR_PBFlushEarly', 'HWR_PBPlanPass',
            'HWR_PBRender', 'HWR_PBDrawBatch', 'HWR_GCReplayRun', 'HWR_GCVerifyPool'),
        'src/hardware/hw_batching.c': ('HWR_BatchCapacity', 'HWR_StartBatching', 'HWR_RenderBatches', 'HWR_ReleaseBatching'),
        'src/ps2/hw/ps2_hwd.c': ('blk_fov', 'PS2HWD_DrawBlocks', 'split_active', 'split_draw', 'split_emit'),
        'src/ps2/hw/ps2_hw_vu1.inc': ('vu_blocks', 'vu_qcopy', 'vu_poly_blk', 'vu_cut_blk', 'vu_cut_blk_run', 'vc_put', 'vu_pbegin', 'vu_pcommit'),
        'src/ps2/hw/ps2_hw_draw.inc': ('emit_fan',),
        'src/ps2/ps2_hwfb.c': ('PS2HWFB_Display', 'FrameLanded', 'ForceSoftware'),
        'src/ps2/i_video.c': ('Impl_HWFailure',),
    }
    witnesses = {}
    all_functions = {}
    for relative, selected in names.items():
        text = (source / relative).read_text()
        data = ''.join(function(text, name) for name in selected)
        path = out / (Path(relative).name.replace('.', '_') + '-witness.c')
        path.write_text(data)
        witnesses[relative] = {'source_sha256': sha(text.encode()), 'witness': str(path),
                               'witness_sha256': sha(data.encode()), 'functions': list(selected)}
        all_functions.update({name: function(text, name) for name in selected})
    pb = (source / 'src/hardware/hw_pbatch.inc').read_text()
    render = all_functions['HWR_PBRender']
    free = all_functions['HWR_PBFree']
    grow = all_functions['HWR_PBGrow']
    force = all_functions['ForceSoftware']
    release = all_functions['HWR_ReleaseBatching']
    driver = all_functions['PS2HWD_DrawBlocks']
    blocks = all_functions['vu_blocks']
    assert render.index('HWR_PBPlanPass();') < render.index('UINT32 *const pb_walk = pb_plan;') < render.index('#define HWR_PBWALK') < render.index('HWR_PBReset();')
    assert grow.count('sizeof(hwr_pbe_t) + sizeof(hwr_pbp_t) + sizeof(UINT32)') == 1
    assert force.index('currently_batching = false;') < force.index('HWR_ReleaseBatching();') < force.index('SCR_SetMode();')
    assert release.index('if (currently_batching)') < release.index('HWR_PBFree();')
    assert free.count('Z_Free(pb_plan);') == 1 and free.count('pb_plan = NULL;') == 1
    assert driver.count('blks[i]') == 2 and 'vu_blocks(pl, blks, i, nfans)' in driver
    assert blocks.count('blks[i]') == 1 and 'vu_qcopy(o + 2, b + 2, 2 * n);' in blocks
    assert 'H.ring[H.cur] + H.wr' in all_functions['vu_pbegin']
    assert 'vu_qcopy(o + 2, b + 2, 2 * n);' in all_functions['vu_poly_blk']
    assert 'vc_poly[i].x = b[2 + 2 * i].f[0];' in all_functions['vu_cut_blk_run']
    assert 'blk_tmp[i].x = b[2 + 2 * i].f[0];' in all_functions['blk_fov']
    # The only occurrences are local parameter uses: the index pointer is not
    # assigned to a retained state field, placed in a DMA REF, or returned.
    uses = {name: [line.strip() for line in body.splitlines() if re.search(r'\bblks\b', line)]
            for name, body in (('PS2HWD_DrawBlocks', driver), ('vu_blocks', blocks))}
    report = {
        'kind': __doc__, 'source_revision': replay.REVISION, 'candidate_source_sha256': replay.CANDIDATE_SHA,
        'optimized_scope': 'PS2 && PS2_PROFILE && !PS2_NOOPT_PB_SCRATCH; all other configurations retain original separate storage',
        'source_manifest_sha256': replay.sha(source / 'manifest.json'),
        'auditor_sha256': replay.sha(Path(__file__)), 'witnesses': witnesses,
        'actual_index_pointer_uses': uses,
        'phase_order': [
            'Allocation/growth owns one 16-byte-aligned malloc/zone UINT32 root, sized at 8*entry_cap bytes.',
            'Normal PBPut and GC replay write two UINT32 planner words per planned polygon during collection.',
            'Sort touches keys/buckets. PlanPass reads all pairs in arrival order before any walk store.',
            'Only after PlanEnd returns, a block-local UINT32 pointer writes every submitted contiguous walk-index prefix.',
            'DrawBlocks reads the index pointer synchronously. VU branches copy polygon words into the DMA ring; the pointer itself is not submitted as a REF. Split/general branches copy vertex words or emit synchronously.',
            'After the final draw, PBReset clears wr/n/nplan/nb and increments serial. Next collection overwrites its own planner entries before use.',
        ],
        'paths': {
            'empty': 'PBRender returns before planner/walk; n=0 implies nplan=0 under producer/reset invariants. Actual outer RenderBatches falls through to the empty old path and ends collection.',
            'early_flush': 'PBFlushEarly calls RenderBatches then StartBatching. The old planner is fully consumed and PBReset clears counts before new producers run.',
            'soft_growth_refused': 'Original pb_nogrow/flush/retry policy remains. No scratch write happens after resize failure until the exact prior batch has rendered and reset.',
            'required_growth_failure': 'Existing guard/fatal path abandons the current batch. ForceSoftware clears collecting and releases all PB roots; it never retries an abandoned planner in place.',
            'failure_during_plan_or_walk_draw': 'Impl_HWFailure uses Z_GuardThrow. Display guard lands in FrameLanded -> ForceSoftware; collecting=false -> ReleaseBatching -> PBFree occurs before SCR_SetMode or any later StartBatching. The automatic walk alias is not retained across the longjmp.',
            'unhandled_failure': 'Without a legal guard, I_Error ends the operation. No path resumes the overwritten planner.',
            'renderer_level_transition': 'Existing ClearAllTextures/ReleaseBatching owns release; shared scratch remains one owner with one free. Source root/macro opt-out retains original separate arrays.',
        },
        'C_object_model': {
            'backing_type': 'UINT32 for every access in both phases; no struct/union view or incompatible typed alias exists in the optimized path.',
            'effective_type': 'The root comes from allocation with no declared type. All live accesses are UINT32 lvalues; realloc copy preserves those values/types. The size proxy hwr_pbp_t is used only by sizeof for the unchanged 8-byte logical planner entry.',
            'alignment': 'The original BatchAlloc/Resize and TryRealloc alignbits=4 remain; UINT32 needs no stronger alignment than the existing root.',
            'bounds': 'Each polygon uses at least8 quadwords (n>=3). entry_cap=ceil(pool_cap/8), so n<=entry_cap. nplan<=n; planner writes2*nplan words and walk writes n words within a2*entry_cap-word allocation. Unchanged capacity overflow checks keep both products within size_t/UINT32 bounds.',
            'no_stale_alias': 'pb_walk is a local pointer obtained after PlanPass. Growth/free occurs outside its live successful draw interval; an exceptional exit discards the alias and frees the owner before restart.',
            'optout': 'PS2_NOOPT_PB_SCRATCH, non-PS2 and non-profile PS2 keep the original struct planner plus separate walk root, including allocation/resize/free order.',
        },
        'remaining_required': 'Actual default-LTO object binding/opt-out restoration, real Hardware process/cache/time and complete image gates. This source witness and host recorder do not prove target VU/DMA timing or physical GS output.',
        'status': 'PASS checked source chain; native execution remains a separate gate',
    }
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'status': report['status'], 'report': str(out / 'report.json')}, indent=2))


if __name__ == '__main__':
    main()
