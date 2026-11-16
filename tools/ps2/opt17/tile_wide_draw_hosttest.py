#!/usr/bin/env python3
"""Actual borrowed-window draw packets followed by ordinary reclaim/overwrite; no raster."""
import argparse
import json
from pathlib import Path
from tile_host_common import HERE, ROOT, compile_run, prepare, replace_once


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source-root', type=Path, default=ROOT)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--sanitize', action='store_true')
    ap.add_argument('--negative', action='store_true', help='bind a wide primitive to the upper anchor instead of its copied prefix')
    a = ap.parse_args()
    source, work = a.source_root.resolve(), a.out.resolve()
    hashes = prepare(source, work)
    base = (work / 'base_hosttest.c').read_text().split('int main(int argc, char **argv)')[0]
    (work / 'base_hosttest.c').write_text(base)
    if a.negative:
        p = work / 'ps2_hw_tile_draw.inc'
        s = replace_once(p.read_text(), '\t// TW/TH and coordinate scale retain the original texture\'s units.',
                         '\tif (wide) tile.blk += 64; // deliberate stale wide reference\n\t// TW/TH and coordinate scale retain the original texture\'s units.')
        p.write_text(s)
    ran = compile_run(source, work, HERE / 'tile_wide_draw_hosttest.c', a.sanitize)
    output = ran.stdout + ran.stderr
    good = ran.returncode == 0 and 'WIDE DRAW COMPLETE' in output
    matched = good if not a.negative else ran.returncode == 1 and 'queued texture reference keeps borrowed base' in output
    matched = matched and 'runtime error:' not in output and 'ERROR: AddressSanitizer' not in output
    (work / 'result.json').write_text(json.dumps({'source_root': str(source), 'source_sha256': hashes,
        'sanitize': a.sanitize, 'negative': a.negative, 'returncode': ran.returncode,
        'matched_expectation': matched, 'emulator_used': False, 'output': output,
        'scope': __doc__}, indent=2) + '\n')
    return int(not matched)


if __name__ == '__main__':
    raise SystemExit(main())
