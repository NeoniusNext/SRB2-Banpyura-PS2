"""OPT10-HF: parity batch. For every map: PS2-HW picture (PCSX2, hf_run.py) + PC-OpenGL reference (pcshot.py, cached) -> panel PC | HW | diff + metrics.

usage: hfbatch.py --tag base --maps 1,2,4,5 [--elf build/out/SRB2.ELF] [--tick 300] [--cfg 'chasecam "Off"'] [--refcache build/ref] [--panels build/panels] [--sw]
  --hwextra '...' extra engine arguments for the PS2 run (e.g. '-hwdbg 0x100000')
Outputs: build/panels/<tag>_m<N>.png, build/panels/<tag>.json (metrics per map), the HW pictures stay in build/runs/<tag>_m<N>/.
The PC reference of a map is made once per (map, tick, cfg): build/ref/m<N>_k<tick>_<cfgtag>/shot-0.png.
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
T = ROOT / 'tools/ps2'


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tag', required=True)
    ap.add_argument('--maps', required=True)
    ap.add_argument('--elf', default=str(ROOT / 'build/out/SRB2.ELF'))
    ap.add_argument('--tick', type=int, default=300)
    ap.add_argument('--cfg', default='')
    ap.add_argument('--refcache', default=str(ROOT / 'build/ref'))
    ap.add_argument('--panels', default=str(ROOT / 'build/panels'))
    ap.add_argument('--hwextra', default='')
    ap.add_argument('--timeout', type=float, default=600)
    ap.add_argument('--norun', action='store_true', help='do not run the emulator, only (re)build the panels from the existing runs')
    ap.add_argument('--refonly', action='store_true')
    a = ap.parse_args()
    cfgtag = hashlib.md5(a.cfg.encode()).hexdigest()[:4] if a.cfg else 'def'
    Path(a.panels).mkdir(parents=True, exist_ok=True)
    res = {}
    jf = Path(a.panels) / f'{a.tag}.json'
    if jf.exists():
        res = json.loads(jf.read_text())
    for m in [x for x in a.maps.split(',') if x]:
        shot = Path(a.refcache) / f'm{m}_k{a.tick}_{cfgtag}' / 'shot-0.png'
        if not shot.exists():
            rc, out = run([sys.executable, str(T / 'pcshot.py'), f'm{m}_k{a.tick}_{cfgtag}', '--shots', f'k{a.tick}', '--warp', m, '--cfg', a.cfg, '--out', a.refcache])
            print(out.strip().splitlines()[0] if out.strip() else 'pcshot no output', flush=True)
        if a.refonly:
            continue
        runname = f'{a.tag}_m{m}'
        if not a.norun:
            cmd = [sys.executable, str(T / 'hf_run.py'), runname, '--elf', a.elf, '--timeout', str(a.timeout), '--cfg', a.cfg, '--',
                   '-skipintro', '-warp', m, '-renderer', 'Hardware', '-zreserve', '3072', '-vidshot', f'k{a.tick}'] + a.hwextra.split()
            rc, out = run(cmd)
            print(out.strip(), flush=True)
        hw = sorted((ROOT / 'build/runs' / runname).glob('vidshot-*k*.ppm'))
        if not hw or not shot.exists():
            res[m] = {'error': 'missing picture' + ('' if hw else ' hw') + ('' if shot.exists() else ' pc')}
            continue
        panel = Path(a.panels) / f'{a.tag}_m{m}.png'
        rc, out = run([sys.executable, str(T / 'hfpanel.py'), '--pc', str(shot), '--hw', str(hw[0]), '--out', str(panel), '--label', f'MAP{m} k{a.tick}', '--json'])
        try:
            res[m] = json.loads(out.strip().splitlines()[-1])
        except Exception:
            res[m] = {'error': out.strip()[-200:]}
        print(f'map {m}: {res[m]}', flush=True)
        jf.write_text(json.dumps(res, indent=1))
    jf.write_text(json.dumps(res, indent=1))


if __name__ == '__main__':
    sys.exit(main())
