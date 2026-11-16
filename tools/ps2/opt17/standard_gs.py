#!/usr/bin/env python3
"""Run the unmodified CT32/CT16 GS checks and retain every strict failure.

The optional reference checks regressions separately. A known failure still
makes this program return one; matching it never makes the strict suite pass.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
TOOLS = ROOT / 'tools/ps2'
sys.path.insert(0, str(TOOLS))
from next12_runtime import run_verified


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--reference', type=Path)
    a = p.parse_args()
    out = a.out.resolve()
    if out.exists() or out == ROOT / 'build' or not out.is_relative_to(ROOT / 'build'):
        p.error('Choose a fresh child of build/')
    out.mkdir(parents=True)
    environment = dict(os.environ, SRB2_PS2_OUT=str(out))
    # The published reference includes the two GS-list model checks. The basic
    # build_hw_test fixture omits them and their four readback images.
    builder = TOOLS / 'gs_list_eetest.py'
    build = subprocess.run([sys.executable, '-B', str(builder)],
                           cwd=ROOT, env=environment, text=True, capture_output=True)
    (out / 'build-orchestration.log').write_text(build.stdout + build.stderr)
    if build.returncode:
        raise RuntimeError('GS fixture build failed; see build-orchestration.log')
    built_path = out / 'hwt/build-report.json'
    built = json.loads(built_path.read_text())
    inputs = [Path(__file__), builder, TOOLS / 'build.py', TOOLS / 'hw_test.c',
              TOOLS / 'run_hw_test.py', out / 'HW_TEST.ELF', built_path]
    for name, digest in built['input_sha256'].items():
        path = Path(name)
        path = path if path.is_absolute() else ROOT / path
        if sha(path) != digest:
            raise RuntimeError('Changed build input: ' + str(path))
        inputs.append(path)
    # Bind transitive compiler inputs as well as the builder's explicit list.
    for dep in sorted((out / 'hwt').glob('*.d')):
        inputs.append(dep)
        primary = dep.read_text().replace('\\\n', ' ').splitlines()[0]
        if ': ' not in primary:
            raise RuntimeError('Malformed compiler dependency file: ' + str(dep))
        for name in shlex.split(primary.split(': ', 1)[1]):
            path = Path(name)
            inputs.append(path if path.is_absolute() else ROOT / path)
    reference = json.loads(a.reference.read_text()) if a.reference else None
    if a.reference:
        inputs.append(a.reference)
    report = {'runs': {}, 'images': {}, 'strict_errors': [], 'regression_errors': [],
              'ELF_sha256': sha(out / 'HW_TEST.ELF'), 'build_report_sha256': sha(built_path),
              'physical_PS2_tested': False, 'FPS_claimed': False,
              'reference_sha256': sha(a.reference) if a.reference else None,
              'fixture_builder': str(builder), 'GS_list_model_checks_included': True}
    previous = os.environ.get('SRB2_PS2_OUT')
    os.environ['SRB2_PS2_OUT'] = str(out)
    try:
        for mode, flags in (('ct32', ['quick', 'dump']), ('ct16', ['quick', 'dump', 'fb16'])):
            command = [sys.executable, '-B', str(TOOLS / 'run_hw_test.py'), '--no-build',
                       '--tag', mode, '--timeout', '300', '--', *flags]
            print('START standard GS ' + mode, flush=True)
            result = run_verified(command, out / ('runtime-' + mode), inputs)
            item = json.loads((out / ('hwt-' + mode + '.json')).read_text())
            observed = {'suite_exit': result.returncode, 'wrapper_exit': item['wrapper_exit'],
                        'complete': item['complete'], 'failures': item['fail'],
                        'pass_count': len(item['pass'])}
            report['runs'][mode] = observed
            if result.returncode or item['wrapper_exit'] or item['fail'] or not item['complete']:
                report['strict_errors'].append(mode + ': strict suite failed')
            if reference and observed != reference['runs'][mode]:
                report['regression_errors'].append(mode + ': exact reference signature changed')
            print(json.dumps({mode: observed}), flush=True)
    finally:
        if previous is None:
            os.environ.pop('SRB2_PS2_OUT', None)
        else:
            os.environ['SRB2_PS2_OUT'] = previous
    report['images'] = {f.name: sha(f) for f in sorted(out.glob('hw-*.ppm'))}
    if reference and report['images'] != reference['images']:
        report['regression_errors'].append('Complete CT32/CT16 readback hashes changed')
    report['strict_passed'] = not report['strict_errors']
    report['reference_unchanged'] = not report['regression_errors'] if reference else None
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ('strict_passed', 'reference_unchanged',
                                           'strict_errors', 'regression_errors')}), flush=True)
    return int(not report['strict_passed'] or bool(report['regression_errors']))


if __name__ == '__main__':
    raise SystemExit(main())
