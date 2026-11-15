#!/usr/bin/env python3
"""Build/run real-driver VU0 probes, retaining commands, hashes and parsed metrics.

source /workspace/.onboarding/environment.sh
source /workspace/.onboarding/emulator-env.sh
python tools/ps2/vu0_diagnostic.py --out build/opt15-vu0/baseline --expect-baseline
python tools/ps2/vu0_diagnostic.py --out build/opt15-vu0/fixed

The baseline option expects the known test-hook failure without weakening any
production/test tolerances. Exit 1 still reports any residual loaded projection
failure. Uses the repository's single-slot PCSX2 runner. --analyze-only rechecks
saved logs without rebuilding or starting the emulator.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build as B


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def evaluate(rows, baseline):
    if set(rows) != {'normal', 'scale256', 'no_load', 'need'}:
        return {}, ['missing/extra probe rows']
    normal, negative, raw, need = [rows[k] for k in ('normal', 'scale256', 'no_load', 'need')]

    def strict(r):
        # Exactly the existing hw_test.c correctness bounds; no new tolerance.
        return (r['n'] >= 40000 and r['outcode_diff'] * 2000 <= r['n']
                and r['max_dx'] <= 1 and r['max_dy'] <= 1 and r['max_dz'] <= 8
                and r['max_q_rel'] < 2e-6)

    checks = {
        'scale256_detected': negative['max_dx'] > 1,
        'no_load_reproduced': raw['outcode_diff'] == 24625 and raw['zero_w'] == 49152
            and raw['common_visible'] == 0 and raw['constants_bad'] == 12,
        'loaded_constants_match': need['constants_bad'] == 0 and need['zero_w'] == 0,
        'loaded_common_visible': need['common_visible'] > 0
            and need['common_visible'] == need['scalar_visible'] == need['vu_visible'],
        'loaded_clip_oracles_match': need['scalar_oracle_diff'] == need['vu_oracle_diff'] == 0,
        'loaded_pair_match': need['pair_diff'] == 0,
        'loaded_outcodes_match': need['n'] == 49152 and need['outcode_diff'] == 0,
    }
    if baseline:
        checks['baseline_signature_matches'] = (normal['n'] == 49152 and normal['outcode_diff'] == 24625
                                                and normal['pair_diff'] == 0 and normal['max_dx'] == 0)
    errors = [name for name, passed in checks.items() if not passed]
    checks['standard_strict_gate'] = strict(normal)
    checks['loaded_strict_gate'] = strict(need)
    if not baseline and not checks['standard_strict_gate']:
        errors.append('standard unchanged-tolerance correctness gate failed')
    if not checks['loaded_strict_gate']:
        errors.append('loaded projection exceeds unchanged dZ<=8 and/or q_rel<2e-6 bounds')
    return checks, errors


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', required=True)
    ap.add_argument('--expect-baseline', action='store_true')
    ap.add_argument('--build-only', action='store_true')
    ap.add_argument('--analyze-only', action='store_true')
    ap.add_argument('--timeout', type=int, default=180)
    a = ap.parse_args()
    out = Path(a.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    if a.analyze_only:
        previous = json.loads((out / 'report.json').read_text())
        return analyze(out, previous['command'], previous['wrapper_exit'],
                       previous['source_sha256'], a.expect_baseline)
    B.gen_config()
    inputs = [B.ROOT / p for p in ('tools/ps2/vu0_diagnostic.c', 'tools/ps2/vu0_diagnostic.py',
              'tools/ps2/hw_test.c', 'src/ps2/hw/ps2_hwd.c')]
    inputs += list((B.ROOT / 'src/ps2/hw').glob('*.inc'))
    source_hashes = {str(p.relative_to(B.ROOT)): sha(p) for p in inputs}
    flags = list(dict.fromkeys([f for f in B.CFLAGS if f != '-DNOHW'] + ['-DHWRENDER']))
    commands, logs, objs = [], [], []

    def command(cmd):
        commands.append([str(x) for x in cmd])
        r = subprocess.run(cmd, env=B.ENV, cwd=B.ROOT, capture_output=True, text=True)
        logs.append(r.stdout + r.stderr)
        if r.returncode:
            raise RuntimeError(r.stdout + r.stderr)
        return r.stdout + r.stderr

    try:
        for src, defines in [('tools/ps2/vu0_diagnostic.c', []),
                             ('tools/ps2/hw_test.c', ['-Dmain=vu0_unused_hw_test_main'])]:
            obj = out / (Path(src).stem + '.o')
            if command([B.CC, *flags, *defines, *B.INCS, '-c', B.ROOT / src, '-o', obj]).strip():
                raise RuntimeError('compiler emitted diagnostics')
            objs.append(obj)
        for name in ('ps2_hw_vu1', 'ps2_hw_vu1_spr'):
            obj = out / (name + '_vsm.o')
            command([B.DEV / ('dvp/bin/dvp-as' + B.EXE),
                     B.ROOT / ('src/ps2/hw/vu1/' + name + '.vsm'), '-o', obj])
            objs.append(obj)
        elf = out / 'VU0_DIAGNOSTIC.ELF'
        link = command([B.CC, *B.LDFLAGS, *objs, '-o', elf, '-lgskit', '-ldmakit', '-ldebug', '-lpatches', '-lm'])
        if any(line.strip() and 'linking abicalls files with non-abicalls files' not in line
               for line in link.splitlines()):
            raise RuntimeError('linker emitted diagnostics')
    finally:
        (out / 'build.log').write_text('\n'.join(logs))
        (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    print('built', elf)
    if a.build_only:
        return 0
    log = out / 'run.log'
    cmd = [sys.executable, str(B.ROOT / 'tools/ps2/run_pcsx2.py'), '--elf', str(elf),
           '--log', str(log), '--until', 'EETEST DONE', '--timeout', str(a.timeout)]
    # build.ENV intentionally narrows PATH to the SDK; the emulator needs the
    # caller's xvfb/runtime environment from emulator-env.sh instead.
    result = subprocess.run(cmd, env=os.environ, cwd=B.ROOT, capture_output=True, text=True)
    (out / 'wrapper.log').write_text(result.stdout + result.stderr)
    if result.returncode:
        print((result.stdout + result.stderr)[-2000:])
    return analyze(out, cmd, result.returncode, source_hashes, a.expect_baseline)


def analyze(out, cmd, wrapper_exit, source_hashes, baseline):
    log = out / 'run.log'
    lines = [line.split('EETEST ', 1)[1] for line in log.read_text(errors='replace').splitlines()
             if 'EETEST ' in line] if log.exists() else []
    rows = {}
    for line in lines:
        print('EETEST', line)
        if re.match(r'VU0 (STANDARD|PHASE) ', line):
            fields = dict(token.split('=', 1) for token in line.split()[2:])
            name = fields.pop('name')
            rows[name] = {k: float(v) for k, v in fields.items()}
    checks, errors = evaluate(rows, baseline)
    if wrapper_exit or 'DONE' not in lines:
        errors.append('emulator did not complete successfully')
    diagnostic_checks = {k: v for k, v in checks.items() if not k.endswith('_strict_gate')}
    report = {'passed': not errors, 'diagnosis_passed': all(diagnostic_checks.values()) and bool(rows)
              and not wrapper_exit and 'DONE' in lines,
              'checks': checks, 'errors': errors, 'expect_baseline': baseline,
              'rows': rows, 'command': cmd, 'wrapper_exit': wrapper_exit,
              'elf_sha256': sha(out / 'VU0_DIAGNOSTIC.ELF'), 'source_sha256': source_hashes,
              'analysis_tool_sha256': sha(Path(__file__))}
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': not errors, 'errors': errors}))
    return int(bool(errors))


if __name__ == '__main__':
    sys.exit(main())
