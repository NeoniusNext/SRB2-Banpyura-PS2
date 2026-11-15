#!/usr/bin/env python3
"""Read-only complete native legacy PPM gate for a frozen final image plan."""
import argparse
import hashlib
import json
from pathlib import Path
import re

from next11_integration_check import inspect
from next11_smoke_check import runtime_evidence
from next13_flats_pressure import admit_elf, sha, verify_files


def analyze(readiness, out):
    plan = json.loads(readiness.read_text())
    verify_files(plan)
    if plan['phase'] != 'images' or not plan['no_hwstats_zspill_or_console_HUD_override']:
        raise RuntimeError('Expected the normal-observer image protocol')
    cases, errors = {}, []
    for case in plan['cases']:
        role = case['role']
        admitted = plan['admissions'][role]
        admit_elf(Path(admitted['build']), Path(admitted['diagnostic_ELF']), int(plan['seed'], 16))
        command, run = case['command'], Path(case['run'])
        if any(flag in command for flag in ('--cfg', '-hwstats', '-zreport', '-zspill', '-loadprof', '-loadhash')):
            raise RuntimeError('Pressure/console observer in normal image command')
        item = inspect(run, case['name'])
        runtime = runtime_evidence(Path(case['runtime']), command)
        if runtime['wrapper_rc'] or runtime['critical_settings'] != plan['settings']['critical_settings'] or runtime['emulator_binary_sha256'] != plan['settings']['emulator_binary_sha256']:
            item['errors'].append('Complete frozen emulator/runtime identity differs')
        if sha(run / 'SRB2.ELF') != admitted['diagnostic_ELF_sha256']:
            item['errors'].append('Actual staged diagnostic ELF differs')
        if (run / 'ps2args').read_text().splitlines() != case['expected_ps2args']:
            item['errors'].append('Exact staged engine arguments differ')
        if (run / '.srb2/reference.cfg').read_text() != plan['expected_staged_config']:
            item['errors'].append('Exact normal staged game configuration differs')
        boot = (run / 'boot.txt').read_text(errors='replace')
        if 'VIDSHOT COMPLETE 3' not in boot or 'HWE startup renderer=Hardware chosen=2 hardware=1' not in boot or re.search(r'HARDWARE -> SOFTWARE|Switching to the Software renderer', boot):
            item['errors'].append('Three complete actual Hardware images absent')
        if set(item['captures']) != set(plan['capture_names']):
            item['errors'].append('Exact three PPM names differ')
        for path, digest in plan['immutable_input_sha256'].items():
            original = Path(path)
            if original.suffix == '.PAK' or original.name == 'FINEACON.DAT':
                if sha(run / original.name) != digest:
                    item['errors'].append('Actual staged stock asset differs: ' + original.name)
        item['runtime'] = runtime
        item['complete_capture_bytes'] = {name: (run / name).stat().st_size for name in plan['capture_names']}
        item['boot_sha256'] = sha(run / 'boot.txt')
        item['summary_sha256'] = sha(run / 'summary.json')
        cases[case['id']] = item
        errors += [case['id'] + ': ' + error for error in item['errors']]
    pairs = {}
    by_id = {case['id']: case for case in plan['cases']}
    for mode in ('legacy', 'legacy-scalar'):
        baseline = Path(by_id['baseline-' + mode]['run'])
        candidate = Path(by_id['candidate-' + mode]['run'])
        rows = {}
        for name in plan['capture_names']:
            old, new = (baseline / name).read_bytes(), (candidate / name).read_bytes()
            equal = old == new
            rows[name] = {'complete_raw_bytes_equal': equal, 'bytes': [len(old), len(new)],
                'baseline_sha256': hashlib.sha256(old).hexdigest(), 'candidate_sha256': hashlib.sha256(new).hexdigest(),
                'first_different_file_byte': next((i for i, (x, y) in enumerate(zip(old, new)) if x != y), None)}
            if not equal:
                errors.append(mode + ': complete raw PPM differs: ' + name)
        pairs[mode] = rows
    report = {'status': 'pass' if not errors else 'reject', 'errors': errors, 'readiness_sha256': sha(readiness),
        'cases': cases, 'full_raw_PPM_pairs': pairs, 'all_headers_and_pixels_compared_without_crop_or_normalization': True,
        'only_audited_seed_initializer_differs_from_each_shipping_ELF': True, 'emulator_or_compiler_invoked': False}
    out.mkdir(parents=True, exist_ok=False)
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'status': report['status'], 'errors': errors, 'full_PPM_pairs': pairs}, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--readiness', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    analyze(args.readiness.resolve(), args.out.resolve())


if __name__ == '__main__':
    main()
