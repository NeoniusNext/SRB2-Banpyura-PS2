#!/usr/bin/env python3
"""PS2-314: bit-exactness and cost of the Ogg decoder over many lumps, between ELFs (PCSX2, the engine's -abench).

usage: python3 tools/ps2/audio_exact.py --out DIR --elf TAG=ELF[|extra engine args] [--elf ...] [--lumps FILE | --music | --sfx N]
                                        [--sec 12] [--batch 12] [--jobs 3] [--ref TAG] [--dump] [--keep]
--dump: also write the decoded PCM (-abenchdump) and compare every ELF with the reference sample by sample: number of differing samples,
largest |difference| in LSB, signal-to-error ratio (the same ELF can be run twice with different engine arguments, e.g.
--elf scalar=build/outV/SRB2.ELF|-novu0a --elf vu0=build/outV/SRB2.ELF).  The raw files are deleted unless --keep.
Runs `-abench lump,lump,...` (12 s of the decoded lump through PS2_MusicRender: cycles of the decode only, FNV-1a of the PCM) in batches
per ELF, parallel over the emulator copies (run_pcsx2.py takes the locks), writes DIR/<TAG>.json = {lump: {cpf, fnv, frames}} and prints
a table: per lump the cycles per output frame of each ELF and "same"/"DIFF" of the hash against --ref (default: the first ELF).
--music: all O_* music lumps (stock music.pk3 list in DIR/../tracks.txt when given by --lumps); --sfx N: N sound effects of every kind
(mono 22/44 kHz, stereo) taken from srb2.pk3.
"""
import argparse
import json
import re
import subprocess
import sys
import zipfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PAK = ROOT.parents[2] / 'build/pak' if (ROOT.parents[2] / 'build/pak').exists() else Path('/home/user/SRB2-Banpyura-PS2/build/pak')


def music_lumps():
    z = zipfile.ZipFile('/opt/srb2-assets/music.pk3')
    return sorted(n.split('/')[-1] for n in z.namelist() if n.startswith('Music/O_') and not n.endswith('/'))


def sfx_lumps(n):
    z = zipfile.ZipFile('/opt/srb2-assets/srb2.pk3')
    import struct
    kinds = {}
    for name in sorted(z.namelist()):
        if not name.startswith('Sounds/') or name.endswith('/'):
            continue
        d = z.read(name)
        if d[:4] != b'OggS':
            continue
        i = d.find(b'\x01vorbis')
        if i < 0:
            continue
        key = (d[i + 11], struct.unpack('<I', d[i + 12:i + 16])[0])
        kinds.setdefault(key, []).append(name.split('/')[-1])
    out = []
    per = max(1, n // max(1, len(kinds)))
    for key, names in sorted(kinds.items()):
        step = max(1, len(names) // per)
        out += names[::step][:per]
    return out


def run_batch(tag, elf, idx, lumps, sec, outdir, extra=(), dump=False):
    name = f'ax_{tag}_{idx}'
    cmd = [sys.executable, str(ROOT / 'tools/ps2/opt_run.py'), '--name', name, '--elf', str(elf), '--pak', str(PAK), '--out', str(outdir / 'runs'),
           '--no-ref', '--timeout', '900', '--until', 'ABENCH done', '--', '-skipintro', '-nomusic', '-abench', ','.join(lumps), '-abenchsec', str(sec)]
    if dump:
        cmd += ['-abenchdump']
    cmd += list(extra)
    run = outdir / 'runs' / name
    subprocess.run(['rm', '-rf', str(run)])
    p = subprocess.run(cmd, capture_output=True, text=True)
    boot = run / 'boot.txt'
    res = {}
    if boot.exists():
        for line in boot.read_text(errors='replace').splitlines():
            m = re.match(r'ABENCH (\S+) type=(\d+) rate=(\d+) frames=(\d+) cycles=(\d+) cyc_per_frame=(\d+) fnv=([0-9a-f]+)', line)
            if m:
                res[m.group(1)] = {'frames': int(m.group(4)), 'cycles': int(m.group(5)), 'cpf': int(m.group(6)), 'fnv': m.group(7)}
    for junk in ('SRB2.ELF', 'pcsx2.log'):
        (run / junk).unlink(missing_ok=True)
    if not res:
        print(f'{name}: no result ({p.returncode})', file=sys.stderr)
    return res


def dump_path(out, tag, bi, lump):
    return out / 'runs' / f'ax_{tag}_{bi}' / '.srb2' / f'abench_{lump}.raw'


def compare_dumps(out, elfs, lumps, ref, nbatches, bsize, keep):
    """Sample-by-sample comparison of the decoded PCM of every ELF against the reference ELF."""
    import numpy as np
    print('\nPCM comparison against', ref)
    for tag, _ in elfs:
        if tag == ref:
            continue
        n_tot = n_diff = n_files = 0
        worst = 0
        sig = err = 0.0
        hist = {}
        worst_lump = ''
        for i, l in enumerate(lumps):
            bi = i // bsize
            pa, pb = dump_path(out, ref, bi, l), dump_path(out, tag, bi, l)
            if not pa.exists() or not pb.exists():
                continue
            a = np.fromfile(pa, dtype='<i2').astype(np.int64)
            b = np.fromfile(pb, dtype='<i2').astype(np.int64)
            m = min(len(a), len(b))
            d = b[:m] - a[:m]
            n_files += 1
            n_tot += m
            nd = int((d != 0).sum())
            n_diff += nd
            w = int(np.abs(d).max()) if m else 0
            if w > worst:
                worst, worst_lump = w, l
            sig += float((a[:m].astype(np.float64) ** 2).sum())
            err += float((d.astype(np.float64) ** 2).sum())
            for v, c in zip(*np.unique(np.clip(d, -3, 3), return_counts=True)):
                hist[int(v)] = hist.get(int(v), 0) + int(c)
        snr = 10 * np.log10(sig / err) if err > 0 else float('inf')
        print(f'{tag}: {n_files} lumps, {n_tot} samples, differing {n_diff} ({100.0 * n_diff / max(1, n_tot):.4f} %), max |diff| {worst} LSB ({worst_lump}), '
              f'signal/error {snr:.1f} dB, histogram of the difference (clipped at +-3) {dict(sorted(hist.items()))}')
    if not keep:
        for tag, _ in elfs:
            for bi in range(nbatches):
                for f in (out / 'runs' / f'ax_{tag}_{bi}' / '.srb2').glob('abench_*.raw'):
                    f.unlink()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--elf', action='append', required=True)
    ap.add_argument('--lumps')
    ap.add_argument('--music', action='store_true')
    ap.add_argument('--sfx', type=int, default=0)
    ap.add_argument('--sec', type=int, default=12)
    ap.add_argument('--batch', type=int, default=12)
    ap.add_argument('--jobs', type=int, default=3)
    ap.add_argument('--ref')
    ap.add_argument('--dump', action='store_true')
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    lumps = []
    if a.lumps:
        lumps += [l.strip() for l in Path(a.lumps).read_text().split() if l.strip()]
    if a.music:
        lumps += music_lumps()
    if a.sfx:
        lumps += sfx_lumps(a.sfx)
    elfs = []
    extras = {}
    for e in a.elf:
        tag, rest = e.split('=', 1)
        elf, _, ex = rest.partition('|')
        elfs.append((tag, elf))
        extras[tag] = ex.split()
    batches = [lumps[i:i + a.batch] for i in range(0, len(lumps), a.batch)]
    results = {}
    with ThreadPoolExecutor(a.jobs) as ex:
        futs = {}
        for tag, elf in elfs:
            for i, b in enumerate(batches):
                futs[(tag, i)] = ex.submit(run_batch, tag, elf, i, b, a.sec, out, extras[tag], a.dump)
        for tag, _ in elfs:
            results[tag] = {}
            for i in range(len(batches)):
                results[tag].update(futs[(tag, i)].result())
            (out / f'{tag}.json').write_text(json.dumps(results[tag], indent=1))
    ref = a.ref or elfs[0][0]
    bad = 0
    print('%-12s' % 'lump' + ''.join('%12s' % t for t, _ in elfs) + '  hash vs ' + ref)
    tot = {t: [0, 0] for t, _ in elfs}
    for l in lumps:
        row = '%-12s' % l
        same = True
        for t, _ in elfs:
            r = results[t].get(l)
            row += '%12s' % (r['cpf'] if r else '-')
            if r:
                tot[t][0] += r['cycles']
                tot[t][1] += r['frames']
            if r is None or results[ref].get(l) is None or r['fnv'] != results[ref][l]['fnv']:
                same = False
        bad += not same
        print(row + '  ' + ('same' if same else 'DIFF'))
    print('%-12s' % 'ALL' + ''.join('%12s' % (tot[t][0] // max(1, tot[t][1])) for t, _ in elfs) + f'  lumps {len(lumps)}, differing {bad}')
    if a.dump:
        compare_dumps(out, elfs, lumps, ref, len(batches), a.batch, a.keep)


if __name__ == '__main__':
    main()
