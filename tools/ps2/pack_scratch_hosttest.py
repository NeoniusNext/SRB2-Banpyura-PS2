#!/usr/bin/env python3
"""Exercise actual old/new WPack buffer lifetimes with guarded allocations and liblz4.

The full pack-reader suite remains tools/ps2/test_pack_reader.py. This fixture
also compares each stock pack through both production versions using liblz4.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', type=Path, default=ROOT / 'build/nov14-next3/baseline-src')
    ap.add_argument('--out', type=Path, default=ROOT / 'build/nov14-next3/pack-scratch')
    ap.add_argument('--pak', type=Path, default=ROOT / 'build/pak')
    a = ap.parse_args()
    if not (a.baseline / 'src/w_pack.c').exists():
        subprocess.run([sys.executable, str(ROOT / 'tools/ps2/prepare_next3_baseline.py'), '--out', str(a.baseline)], check=True)
    a.out.mkdir(parents=True, exist_ok=True)
    inc = Path(os.environ['PS2DEV']) / 'ps2sdk/ports/include'
    results = {}
    exes = {}
    for name, source, san in [('baseline', a.baseline, False), ('current', ROOT, False), ('asan', ROOT, True)]:
        generated = a.out / (name + '.c')
        generated.write_text('''#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void *ScratchAlloc(size_t n);
static void ScratchFree(void *p);
#define malloc ScratchAlloc
#define free ScratchFree
#include "''' + str(source.resolve() / 'src/w_pack.c') + '''"
#undef malloc
#undef free
#define main pack_corpus_main
#define free ScratchFree
#include "''' + str(ROOT / 'tools/ps2/pack_hosttest.c') + '''"
#undef main
#undef free
#define SCRATCH_BASELINE ''' + str(int(name == 'baseline')) + '''
#include "''' + str(ROOT / 'tools/ps2/pack_scratch_hosttest.c') + '''"
''')
        exe = a.out / name
        cmd = ['cc', '-O2', '-g', '-Wall', '-Wno-unused-function', '-Wno-sign-compare', '-DPS2_PROFILE', '-DNOHW', '-DNDEBUG',
               '-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'build/host-gen'), '-I' + str(inc), str(generated), '-Wl,-l:liblz4.so.1', '-o', str(exe)]
        if san:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
        p = subprocess.run(cmd, capture_output=True, text=True)
        (a.out / (name + '-build.log')).write_text(p.stdout + p.stderr)
        if p.returncode:
            raise RuntimeError(p.stdout + p.stderr)
        p = subprocess.run([str(exe)], capture_output=True, text=True)
        (a.out / (name + '.log')).write_text(p.stdout + p.stderr)
        if p.returncode:
            raise RuntimeError(name + ': ' + p.stdout + p.stderr)
        results[name] = json.loads(p.stdout)
        exes[name] = exe
        print(name, results[name], flush=True)
    assert results['baseline']['scratch_bytes'] - results['current']['scratch_bytes'] == 65536
    assert results['baseline']['cases'] == results['current']['cases'] == results['asan']['cases']
    assert results['current'] == results['asan']
    assert all(r['remaining_bytes'] == 0 for r in results.values())
    for pack in ['SRB2.PAK', 'ZONES.PAK', 'CHARS.PAK', 'MUSIC.PAK']:
        for name in ['baseline', 'current']:
            tsv = a.out / (name + '-' + pack + '.tsv')
            p = subprocess.run([str(exes[name]), str(a.pak / pack), str(tsv)], capture_output=True, text=True)
            (a.out / (name + '-' + pack + '.log')).write_text(p.stdout + p.stderr)
            if p.returncode:
                raise RuntimeError(name + '/' + pack + ': ' + p.stdout + p.stderr)
        assert (a.out / ('baseline-' + pack + '.tsv')).read_bytes() == (a.out / ('current-' + pack + '.tsv')).read_bytes()
        assert (a.out / ('baseline-' + pack + '.log')).read_bytes() == (a.out / ('current-' + pack + '.log')).read_bytes()
        print(pack, 'full/partial reads, CRCs and I/O counters identical', flush=True)
    (a.out / 'report.json').write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
