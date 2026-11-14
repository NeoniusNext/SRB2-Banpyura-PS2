#!/usr/bin/env python3
"""Prove the final target-only guards preserve every previously measured object.

Compile the actual preceding/final source without source normalization. Native
and EE no-MMI objects must match the preceding implementation byte-for-byte;
the default EE object must match the measured pre-guard stereo object. No LTO.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TOKEN = re.compile(rb'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_]\w*|\d[\w.]*|<<=|>>=|\.\.\.|->|\+\+|--|&&|\|\||==|!=|<=|>=|<<|>>|[+*/%&|^!-]=|\S')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args):
    return subprocess.check_output([str(arg) for arg in args])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', required=True, type=Path)
    ap.add_argument('--measured', required=True, type=Path)
    ap.add_argument('--out', type=Path, default=ROOT / 'build/audio-stereo-next5-guard-proof')
    ap.add_argument('--sdk', type=Path, default=Path(os.environ.get('PS2SDK', '/workspace/.onboarding/ps2dev-v2/ps2dev/ps2sdk')))
    a = ap.parse_args()
    out = a.out.resolve()
    if not out.is_relative_to(ROOT / 'build'):
        ap.error('--out must be under the repository build directory')
    out.mkdir(parents=True, exist_ok=True)
    baseline = a.baseline.resolve() / 'src/ps2/ps2_audio.c'
    final = ROOT / 'src/ps2/ps2_audio.c'
    measured = a.measured.resolve() / 'candidate_ps2_audio.c'
    common = ['-O2', '-std=gnu17', '-Wall', '-Wextra', '-Werror',
              '-I' + str(ROOT / 'src/ps2'), '-I' + str(ROOT / 'tools/ps2'),
              '-I' + str(a.sdk / 'ee/include'), '-I' + str(a.sdk / 'common/include')]
    report = {'baseline_sha256': digest(baseline), 'production_sha256': digest(final),
              'measured_source_sha256': digest(measured), 'policies': {}}
    for policy, cc, extra in [('native', 'gcc', []),
                              ('ee_noopt', 'mips64r5900el-ps2-elf-gcc',
                               ['-D_EE', '-DPS2', '-DPS2_NOOPT_AUDIO_MMI', '-G0'])]:
        objects, preprocessed = [], []
        for label, source in [('baseline', baseline), ('final', final)]:
            obj = out / (policy + '-' + label + '.o')
            run([cc, *common, *extra, '-c', source, '-o', obj])
            objects.append(obj)
            preprocessed.append(TOKEN.findall(run([cc, *common, *extra, '-E', '-P', source])))
        assert objects[0].read_bytes() == objects[1].read_bytes(), policy + ' object mismatch'
        assert preprocessed[0] == preprocessed[1], policy + ' tokens mismatch'
        report['policies'][policy] = {'entire_object_byte_identical': True,
                                      'preprocessed_tokens_identical': True,
                                      'object_sha256': digest(objects[0]),
                                      'token_count': len(preprocessed[0])}
    # Preserve the measured TU's basename, including its STT_FILE symbol.
    copied = out / 'candidate_ps2_audio.c'
    copied.write_bytes(final.read_bytes())
    obj = out / 'candidate_ps2_audio.o'
    flags = ['mips64r5900el-ps2-elf-gcc', *common, '-D_EE', '-DPS2', '-G0']
    run([*flags, '-c', copied, '-o', obj])
    original = a.measured.resolve() / 'ee/candidate_ps2_audio.o'
    assert obj.read_bytes() == original.read_bytes(), 'default EE measured/final object mismatch'
    preprocessed = [TOKEN.findall(run([*flags, '-E', '-P', source])) for source in [measured, copied]]
    assert preprocessed[0] == preprocessed[1], 'default EE measured/final tokens mismatch'
    report['policies']['ee_mmi'] = {'entire_object_byte_identical_to_measured': True,
                                   'preprocessed_tokens_identical_to_measured': True,
                                   'object_sha256': digest(obj), 'token_count': len(preprocessed[0])}
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
