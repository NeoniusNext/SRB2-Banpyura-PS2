#!/usr/bin/env python3
"""Independently compare saved actual native 16KiB row-path RGBA frame pairs."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, required=True)
    parser.add_argument('--negative', action='store_true')
    parser.add_argument('--control-only', action='store_true')
    args = parser.parse_args()
    run = args.run.resolve()
    native = json.loads((run / 'native-report.json').read_text())
    host = Path(native['private_host_stage'])
    pairs = []
    for reference in sorted(host.glob('tn-row-*-v0-320x200.rgba')):
        candidate = reference.with_name(reference.name.replace('-v0-', '-v1-'))
        a, b = reference.read_bytes(), candidate.read_bytes()
        if len(a) != 320 * 200 * 4 or len(b) != len(a):
            raise RuntimeError('Incomplete native RGBA frame pair')
        rgba = alpha = coverage = missing = maxerr = 0
        for at in range(0, len(a), 4):
            error = max(abs(a[at + c] - b[at + c]) for c in range(4))
            rgba += error != 0
            alpha += a[at + 3] != b[at + 3]
            maxerr = max(error, maxerr)
            visible = a[at:at + 3] != bytes([0x1d, 0x2b, 0x35])
            coverage += visible
            missing += visible and b[at:at + 3] == bytes([0x1d, 0x2b, 0x35])
        pairs.append(dict(name=reference.name.split('-v0-')[0], compared=64000,
            different_RGBA=rgba, different_alpha=alpha, max_channel_error=maxerr,
            reference_coverage=coverage, missing_coverage=missing,
            resident_sha256=hashlib.sha256(a).hexdigest(),
            virtual_sha256=hashlib.sha256(b).hexdigest()))
    log = (run / 'native.txt').read_text()
    depth = re.findall(r'TN ROWS_DEPTH .*compared=(\d+) wrong_Zlow24=(\d+)', log)
    vertex = re.findall(r'H0 (PASS|FAIL) wf_original_vertices .*actual_STQ_RGBA_XY_Zlow24_wrong=(\d+) input_changed=(\d+) qwords=(\d+)', log)
    expected_pairs = 1 if args.negative or args.control_only else 16
    complete = len(pairs) == expected_pairs and len(depth) == expected_pairs and len(vertex) == expected_pairs * 2
    nonempty = all(pair['reference_coverage'] > 1000 for pair in pairs)
    vertices_exact = all(row[0] == 'PASS' and int(row[1]) == int(row[2]) == 0 and int(row[3]) >= 9 for row in vertex)
    depth_exact = all(int(n) == 64000 and int(bad) == 0 for n, bad in depth)
    rgba_exact = all(pair['different_RGBA'] == 0 for pair in pairs)
    profile = all(native.get(key) for key in ['completed', 'profile_32MiB', 'single_emulator_slot',
        'all_frozen_inputs_unchanged', 'all_non_UI_settings_unchanged', 'emulator_binary_unchanged'])
    matched = complete and nonempty and vertices_exact and profile
    if args.negative:
        matched &= not rgba_exact and native['failure_count'] > 0
    else:
        matched &= rgba_exact and depth_exact and native['failure_count'] == 0
    report = dict(scope=__doc__, native_report=str(run / 'native-report.json'),
        negative=args.negative, complete_pairs=complete, nonempty_reference_frames=nonempty,
        packed_original_vertices_exact=vertices_exact, RGBA_exact=rgba_exact,
        Zlow24_exact=depth_exact, strict_native_failure_count=native['failure_count'],
        profile_and_bindings_unchanged=profile, matched_expectation=bool(matched), pairs=pairs,
        tolerance=0, physical_PS2_tested=False)
    (run / 'focused-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: report[key] for key in ['negative', 'complete_pairs', 'RGBA_exact',
        'Zlow24_exact', 'packed_original_vertices_exact', 'strict_native_failure_count', 'matched_expectation']}, indent=2))
    return int(not matched)


if __name__ == '__main__':
    main()
