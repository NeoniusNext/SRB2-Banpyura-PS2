"""Strict pixel equivalence of two host-profile builds over many demo frames (PS2-16 candidate).

usage: python tools/ps2/host_frames_compare.py --ref EXE --cand EXE --out DIR [--every N] [--demos DEMO_001,...]
  ref   host build with the original code (SRB2 CMake host profile, CL=/DPS2_NOOPT)
   cand  host build with the selected optimisations (same sources)
Both replay the golden demos with the recorded command line and the cooked packs, dumping every N-th rendered
frame (-ps2ref-every N, indexed 320x200). Output: per demo and in total, frames compared, frames that differ,
differing pixels, worst frame (% of pixels), plus tics.csv equality (game state identical; the renderer does not
feed back). Frames are also compared against golden/phase0-v2 (every 35th) when --every divides into them.
Result: <out>/frames-compare.json and a text table on stdout.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
GOLDEN = ROOT / 'golden/phase0-v2/run1'
PACKS = ROOT / 'build/pak'
DEPS = Path('D:/AI-projects/SRB2B-plus/build/deps/vcpkg_installed/x64-windows/bin')


def run_demo(exe, demo, out, every, extra):
    out.mkdir(parents=True, exist_ok=True)
    gamehome = out / 'home/srb2'
    gamehome.mkdir(parents=True, exist_ok=True)
    ref = GOLDEN / demo
    shutil.copy2(ref / 'home/srb2/reference.cfg', gamehome / 'reference.cfg')
    shutil.copy2(GOLDEN.parent / (demo + '.lmp'), gamehome / (demo + '.lmp'))
    cmd = json.loads((ref / 'command.json').read_text())
    cmd[0] = str(exe)
    cmd[cmd.index('-ps2ref') + 1] = str(out)
    cmd[cmd.index('-home') + 1] = str(out / 'home')
    cmd += ['-ps2ref-every', str(every)] + extra
    (out / 'command.json').write_text(json.dumps(cmd, indent=2))
    env = dict(os.environ, SRB2WADDIR=str(PACKS))
    env['PATH'] = str(DEPS) + os.pathsep + env.get('PATH', '')
    startup = None
    if os.name == 'nt':
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
    with (out / 'stdout.log').open('wb') as log:
        p = subprocess.run(cmd, cwd=PACKS, env=env, stdout=log, stderr=subprocess.STDOUT, startupinfo=startup, timeout=600)
    if p.returncode != 0 or not (out / 'complete.txt').exists():
        raise SystemExit(f'{exe.name} {demo}: run failed rc={p.returncode}')


def load(p):
    return np.fromfile(p, dtype=np.uint8)


def compare_dirs(a, b, names):
    res = {'frames': 0, 'differing_frames': 0, 'pixels': 0, 'worst_pct': 0.0, 'worst_frame': None, 'per_frame': {}}
    for n in names:
        fa, fb = load(a / n), load(b / n)
        if fa.size != 64000 or fb.size != 64000:
            raise ValueError(f'invalid 320x200 frame sizes: {n}: {fa.size}, {fb.size}')
        d = int(np.count_nonzero(fa != fb))
        res['frames'] += 1
        if d:
            res['differing_frames'] += 1
            res['pixels'] += d
            pct = 100.0 * d / fa.size
            res['per_frame'][n] = d
            if pct > res['worst_pct']:
                res['worst_pct'], res['worst_frame'] = pct, n
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ref', type=Path, required=True)
    ap.add_argument('--cand', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--every', type=int, default=5)
    ap.add_argument('--demos', default='DEMO_001,DEMO_002,DEMO_003,DEMO_004')
    ap.add_argument('--extra', default='')
    a = ap.parse_args()
    if a.every <= 0:
        ap.error('--every must be positive')
    a.ref, a.cand, a.out = a.ref.resolve(), a.cand.resolve(), a.out.resolve()
    a.out.mkdir(parents=True, exist_ok=False)
    report = {'ref': str(a.ref), 'cand': str(a.cand), 'every': a.every, 'demos': {}}
    report['exe_sha256'] = {k: hashlib.sha256(p.read_bytes()).hexdigest() for k, p in (('ref', a.ref), ('cand', a.cand))}
    passed = True
    tot = {'frames': 0, 'differing_frames': 0, 'pixels': 0}
    for demo in a.demos.split(','):
        ra, rb = a.out / 'ref' / demo, a.out / 'cand' / demo
        for exe, d in ((a.ref, ra), (a.cand, rb)):
            if d.exists():
                shutil.rmtree(d)
            run_demo(exe, demo, d, a.every, a.extra.split())
        names = sorted(p.name for p in ra.glob('frame-*.idx'))
        cand_names = sorted(p.name for p in rb.glob('frame-*.idx'))
        if not names or names != cand_names:
            raise ValueError(f'{demo}: empty or different frame sets')
        r = compare_dirs(ra, rb, names)
        tics_equal = {}
        for f in ('tics.csv',):
            tics_equal[f] = (ra / f).read_bytes() == (rb / f).read_bytes()
        r['tics_identical'] = tics_equal
        # reference build against the PC golden (every 35th frame): validates the harness itself
        g = sorted(p.name for p in (GOLDEN / demo).glob('frame-*.idx'))
        gd = 0
        for n in g:
            if (ra / n).exists():
                gd += int(np.count_nonzero(load(GOLDEN / demo / n) != load(ra / n)))
        r['ref_vs_golden_pixels'] = gd
        r['ref_vs_golden_frames'] = sum(1 for n in g if (ra / n).exists())
        if not r['ref_vs_golden_frames']:
            raise ValueError(f'{demo}: no golden frames compared; choose --every dividing 35')
        passed &= not r['pixels'] and not gd and all(tics_equal.values())
        report['demos'][demo] = r
        for k in tot:
            tot[k] += r[k]
        print(f"{demo}: {r['frames']} frames, {r['differing_frames']} differ, {r['pixels']} px, worst {r['worst_pct']:.4f}% ({r['worst_frame']}), "
              f"ref-vs-golden {r['ref_vs_golden_pixels']} px in {r['ref_vs_golden_frames']} frames, tics identical {tics_equal}")
        shutil.rmtree(ra / 'home', ignore_errors=True)
        shutil.rmtree(rb / 'home', ignore_errors=True)
    report['total'] = dict(tot, mean_pct_per_frame=100.0 * tot['pixels'] / max(1, tot['frames'] * 64000))
    report['passed'] = passed
    print('TOTAL', report['total'])
    (a.out / 'frames-compare.json').write_text(json.dumps(report, indent=1))
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
