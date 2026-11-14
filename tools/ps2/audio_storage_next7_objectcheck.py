#!/usr/bin/env python3
"""Require unchanged complete strict music objects when Vorbis is disabled."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', required=True, type=Path)
    ap.add_argument('--candidate', type=Path, default=ROOT)
    ap.add_argument('--out', type=Path, default=ROOT / 'build/nov14-next7/audio-storage-objects')
    ap.add_argument('--sdk', type=Path, default=Path(os.environ.get('PS2SDK', '/workspace/.onboarding/ps2dev-v2/ps2dev/ps2sdk')))
    a = ap.parse_args()
    out = a.out.resolve()
    if not out.is_relative_to(ROOT / 'build'):
        ap.error('--out must be under build')
    out.mkdir(parents=True, exist_ok=True)
    flags = ['-O2', '-std=gnu17', '-Wall', '-Wextra', '-Werror',
             '-I' + str(a.candidate.resolve() / 'src/ps2'), '-I' + str(a.sdk / 'ports/include')]
    report = {'sources': {}, 'policies': {}}
    for kind, source in [('baseline', a.baseline.resolve()), ('candidate', a.candidate.resolve())]:
        source = source / 'src/ps2/ps2_music.c'
        report['sources'][kind] = {'path': str(source), 'sha256': hashlib.sha256(source.read_bytes()).hexdigest()}
    for machine, compiler, extra in [('host', 'gcc', []),
            ('ee', 'mips64r5900el-ps2-elf-gcc', ['-G0', '-D_EE', '-DPS2',
             '-I' + str(a.sdk / 'ee/include'), '-I' + str(a.sdk / 'common/include')])]:
        for mp3 in (False, True):
            policy = machine + ('_mp3' if mp3 else '_no_codecs')
            hashes = []
            for kind, root in [('baseline', a.baseline.resolve()), ('candidate', a.candidate.resolve())]:
                directory = out / policy / kind
                directory.mkdir(parents=True, exist_ok=True)
                source = directory / 'ps2_music.c'
                shutil.copyfile(root / 'src/ps2/ps2_music.c', source)
                obj = directory / 'music.o'
                command = [compiler, *flags, *extra, *(['-DPS2_AUDIO_MP3'] if mp3 else []), '-c', str(source), '-o', str(obj)]
                result = subprocess.run(command, capture_output=True, text=True)
                if result.returncode:
                    raise SystemExit(result.stdout + result.stderr)
                hashes.append(hashlib.sha256(obj.read_bytes()).hexdigest())
            if hashes[0] != hashes[1]:
                raise SystemExit('entire object differs with Vorbis disabled: ' + policy)
            report['policies'][policy] = {'entire_object_byte_identical': True, 'sha256': hashes[0]}
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
