#!/usr/bin/env python3
"""Prove optional wide tile conversion, physical ownership and real allocator retry.

The renderer and allocator are compiled verbatim through the maintained host GS
packet interface.  Native GS sampling is deliberately a separate proof.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path
from tile_host_common import HERE, ROOT, compile_run, prepare, replace_once


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source-root', type=Path, default=ROOT)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--sanitize', action='store_true')
    ap.add_argument('--negative', choices=['borrow', 'free-prefix', 'cache', 'anchor', 'reference'],
                    help='Mutate only the private candidate copy; require a matching assertion.')
    a = ap.parse_args()
    source, work = a.source_root.resolve(), a.out.resolve()
    hashes = prepare(source, work)
    base = (work / 'base_hosttest.c').read_text().split('int main(int argc, char **argv)')[0]
    base = base.replace('static void pk_ref(const void *data, u32 qwc)',
                        'static unsigned borrow_refs;\nstatic void pk_ref(const void *data, u32 qwc)')
    base = base.replace('(void)data; (void)qwc;', '(void)data; (void)qwc; borrow_refs++;')
    base = base.replace('static void pk_ref_tail(const void *data, u32 bytes) { (void)data; (void)bytes; }',
                        'static void pk_ref_tail(const void *data, u32 bytes) { (void)data; (void)bytes; borrow_refs++; }')
    (work / 'base_hosttest.c').write_text(base)
    controls = {
        'borrow': ('prefix = anchor - 64u;', 'prefix = anchor - 128u;', 'BORROW FAIL exact borrowed prefix'),
        'free-prefix': ('own_clear(prefix, 64);', 'own_clear(prefix + 64u, 64);', 'BORROW FAIL used owner identity'),
        'cache': ('tile_cache.region == layout;', '((tile_cache.region & 3) == (layout & 3));', 'BORROW FAIL shape transition upload'),
        'anchor': ('out->blk = wide ? H.rec[ri].blk : (u16)anchor;', 'out->blk = H.rec[ri].blk;', 'BORROW FAIL ordinary load keeps permanent anchor'),
        'reference': ('tile_source_unlock(locked, dc_source, retained);\n\treturn 1;', 'pk_ref(c.data, 1);\n\ttile_source_unlock(locked, dc_source, retained);\n\treturn 1;', 'BORROW FAIL upload has no source DMA reference'),
    }
    if a.negative:
        p = work / 'ps2_hw_tile.inc'
        old, new, _ = controls[a.negative]
        p.write_text(replace_once(p.read_text(), old, new))
    ran = compile_run(source, work, HERE / 'tile_borrow_hosttest.c', a.sanitize)
    output = ran.stdout + ran.stderr
    totals = re.search(r'BORROW COMPLETE checks=(\d+) cases=(\d+) bytes=(\d+) failed=0', output)
    matched = (ran.returncode == 0 and totals is not None
               and (int(totals[2]), int(totals[3])) == (10560, 180224000))
    if a.negative:
        matched = ran.returncode == 1 and controls[a.negative][2] in output
    matched = matched and 'runtime error:' not in output and 'ERROR: AddressSanitizer' not in output
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    report = {'source_root': str(source), 'source_sha256': hashes,
              'runner_sha256': sha(Path(__file__)), 'fixture_sha256': sha(HERE / 'tile_borrow_hosttest.c'),
              'shared_host_sha256': sha(HERE / 'tile_host_common.py'),
              'sanitize': a.sanitize, 'negative': a.negative, 'returncode': ran.returncode,
              'checks': int(totals[1]) if totals else None,
              'conversion_cases': int(totals[2]) if totals else None,
              'compared_bytes': int(totals[3]) if totals else None,
              'matched_expectation': matched, 'emulator_used': False, 'output': output,
              'free_list_bound': {'all_vram_blocks': 16384, 'range_capacity': 8193,
                  'borrowed128_global_conservative_max_intervals': 8130,
                  'return_adds_at_most': 1,
                  'largest_nonzero_page_aligned_pool': [32, 16384],
                  'highest_valid_page_aligned_borrowed_intervals': 8112,
                  'witness_returned_intervals': 8113},
              'scope': ['actual allocator, owner map, cache helper, converter and copied GIF uploads',
                        'strict sorted disjoint coalesced free-list and whole-pool coverage',
                        'real allocator hook reclaims prefix before failure accounting or eviction',
                        'independent per-axis source-coordinate and pixel conversion oracle',
                        'queued wide primitive versus reclaim ordering is covered by tile_wide_draw_hosttest.py; native GS is separate']}
    (work / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    return int(not matched)


if __name__ == '__main__':
    raise SystemExit(main())
