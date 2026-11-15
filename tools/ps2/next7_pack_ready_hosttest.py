#!/usr/bin/env python3
"""Actual WPack readiness query: zero state/heap changes and prepared reads."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, default=ROOT / 'build/nov14-next7/memory-pack-ready')
    ap.add_argument('--sanitize', action='store_true')
    ap.add_argument('--negative-controls', action='store_true')
    a = ap.parse_args()
    a.out = a.out.resolve()
    a.out.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'src/w_pack.c'
    text = source.read_text()
    sdk = Path(os.environ.get('PS2SDK', '/opt/ps2dev-x/ps2dev/ps2sdk'))
    flags = ['cc', '-O2', '-g', '-std=gnu17', '-Wall', '-Wno-unused-function',
             '-Wno-sign-compare', '-Wno-unused-variable', '-DPS2_PROFILE', '-DNOHW', '-DNDEBUG',
             '-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'build/host-gen'), '-I' + str(sdk / 'ports/include'),
             '-DPACK_HOST_SOURCE="' + str(TOOLS / 'pack_hosttest.c') + '"']
    if a.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
    report = {'sources': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in [source, ROOT / 'src/w_pack.h',
                                    TOOLS / 'next7_pack_ready_hosttest.c', Path(__file__).resolve()]}}

    def test(label, production, reject=False):
        exe = a.out / label
        command = [*flags, '-DWPACK_SOURCE="' + str(production) + '"',
                   TOOLS / 'next7_pack_ready_hosttest.c', '-Wl,-l:liblz4.so.1', '-o', exe]
        p = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True)
        (a.out / (label + '-build.log')).write_text(p.stdout + p.stderr)
        assert p.returncode == 0, p.stderr
        p = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True, timeout=30)
        (a.out / (label + '.log')).write_text(p.stdout + p.stderr)
        if reject:
            assert p.returncode == 1 and 'FAIL' in p.stderr, (label, p.stdout, p.stderr)
        else:
            assert p.returncode == 0 and 'READY PASS checks=392 allocations=10 frees=10 live=0' in p.stdout, (p.stdout, p.stderr)
        report[label] = {'rc': p.returncode, 'stdout': p.stdout.strip(), 'stderr': p.stderr.strip()}
        print(label, report[label], flush=True)

    test('current', source)
    if a.negative_controls:
        controls = {
            'create-slot': ('s = StreamFind(handle); // never create, replace or allocate a stream slot',
                            's = StreamFor(handle); // deliberately creates a cold slot'),
            'missing-codec-buffer': ('if (!handle || !l || !iobuf || !cbuf)', 'if (!handle || !l)'),
            'heap-index': ('l->size <= 64u * WPACK_BLOCK', 'l->size <= 65u * WPACK_BLOCK'),
        }
        for label, (old, new) in controls.items():
            assert old in text, label
            variant = a.out / (label + '-w_pack.c')
            variant.write_text(text.replace(old, new))
            test('negative-' + label, variant, reject=True)
    (a.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
