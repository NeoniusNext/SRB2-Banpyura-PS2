#!/usr/bin/env python3
"""PS2-315 host regression: the vendored Vorbis decode units (src/ps2/vorbis) against stock libvorbis on the real tracks, bit for bit.

usage: python3 tools/ps2/vorbis_host_check.py [--tracks N] [--sec 12] [--mode rn|trunc|both] [--prev GITREF]
Builds four host decoders from tools/ps2/ee_tests/vorbis_host.c (x86-64 gcc, -ffp-contract=off, static libvorbis/libogg of the system):
  stock    libvorbisfile + libvorbis as installed;
  vendored the vendored units (codebook/floor1/mapping0/res0/mdct/synthesis/block) with the scalar code (no PS2_VORBIS_VU0);
  twin     the vendored units with PS2_VORBIS_VU0 + PS2A_MODEL: the vector structure with the C twins of the VU0 kernels.
Decodes N stock music tracks of /opt/srb2-assets/music.pk3 (12 s each, half rate, as ps2_music.c) and compares the PCM sample by sample. Round to
nearest and, with --mode trunc/both, round toward zero (what VU0 does).  With --prev GITREF a fourth decoder is built from the vendored files of that commit and must give the same PCM as the current vendored files
(for changes that must not alter the result: entropy decode, tables).  Pass = vendored and twin are bit-identical (the data flow, the tables and
the operation order of the vector structure equal the scalar code); stock is listed for information: the system libvorbis is built with -ffast-math,
so it differs from the vendored code by a few samples of 1 LSB (this host test is x86 and says nothing about the EE).
"""
import argparse
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/vorbis-host'
UNITS = ['codebook.c', 'floor1.c', 'mapping0.c', 'res0.c', 'mdct.c', 'synthesis.c', 'block.c']
BASE = ['-O2', '-ffp-contract=off', '-mno-fma', '-w', '-DHAVE_ALLOCA_H']


def build(name, extra, units, rz, srcdir=None):
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / (name + ('_rz' if rz else ''))
    vdir = srcdir or (ROOT / 'src/ps2/vorbis')
    cmd = ['gcc', *BASE, *extra, '-I' + str(vdir), '-I' + str(ROOT / 'tools/ps2/ee_tests')]
    if rz:
        cmd += ['-DTRUNC', '-frounding-math']
    cmd += [str(ROOT / 'tools/ps2/ee_tests/vorbis_host.c')] + [str(vdir / u) for u in units]
    cmd += ['-lvorbisfile', '-lvorbis', '-logg', '-lm', '-o', str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(' '.join(cmd)); print(r.stdout + r.stderr); raise SystemExit('build failed: ' + name)
    return exe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tracks', type=int, default=20)
    ap.add_argument('--sec', type=int, default=12)
    ap.add_argument('--mode', default='both')
    ap.add_argument('--prev', default='')
    a = ap.parse_args()
    z = zipfile.ZipFile('/opt/srb2-assets/music.pk3')
    names = sorted(n for n in z.namelist() if n.startswith('Music/O_') and not n.endswith('/'))
    step = max(1, len(names) // a.tracks)
    names = names[::step][:a.tracks]
    tmp = Path(tempfile.mkdtemp(prefix='vh_'))
    files = []
    for n in names:
        f = tmp / (n.split('/')[-1] + '.ogg')
        f.write_bytes(z.read(n))
        files.append(f)
    bad = 0
    prevdir = None
    if a.prev:
        prevdir = tmp / 'prev'
        prevdir.mkdir()
        listing = subprocess.run(['git', 'ls-tree', '--name-only', a.prev, 'src/ps2/vorbis/'], capture_output=True, text=True, cwd=ROOT).stdout.split()
        for name in listing:
            data = subprocess.run(['git', 'show', a.prev + ':' + name], capture_output=True, cwd=ROOT).stdout
            (prevdir / Path(name).name).write_bytes(data)
    for rz in ([False, True] if a.mode == 'both' else [a.mode == 'trunc']):
        exes = {
            'stock': build('stock', [], [], rz),
            'vendored': build('vendored', [], UNITS, rz),
            'twin': build('twin', ['-DPS2_VORBIS_VU0', '-DPS2A_MODEL'], UNITS, rz),
        }
        if prevdir:
            exes['prev'] = build('prev', ['-DPS2_VORBIS_VU0', '-DPS2A_MODEL'], UNITS, rz, prevdir)
        print('rounding:', 'toward zero' if rz else 'nearest')
        import numpy as np
        for f in files:
            pcm = {}
            for k, e in exes.items():
                raw = tmp / (f.stem + '.' + k + '.raw')
                r = subprocess.run([str(e), str(f), str(a.sec), '-dump', str(raw)], capture_output=True, text=True)
                pcm[k] = np.fromfile(raw, dtype='<i2').astype(np.int32) if r.returncode == 0 else None
                raw.unlink(missing_ok=True)
            same = pcm['vendored'] is not None and pcm['twin'] is not None and np.array_equal(pcm['vendored'], pcm['twin'])
            if 'prev' in pcm:
                same = same and pcm['prev'] is not None and np.array_equal(pcm['prev'], pcm['twin'])
            bad += not same
            st = pcm['stock']
            m = min(len(st), len(pcm['vendored']))
            nd = int((st[:m] != pcm['vendored'][:m]).sum())
            print('  %-10s vendored==twin%s: %s;  stock vs vendored: %d of %d samples differ (max %d LSB)' % (f.stem, '==prev' if 'prev' in pcm else '', 'yes' if same else 'NO', nd, m,
                  int(np.abs(st[:m] - pcm['vendored'][:m]).max())))
    print('tracks %d, differing %d' % (len(files), bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
