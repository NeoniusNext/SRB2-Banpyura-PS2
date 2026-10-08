"""OPT11-FX: picture parity of a list of maps, PC OpenGL against PS2-HW, one map at a time (fresh start of the level), at tick N.

usage: fx_sweep.py --tag T --maps 1,2,3,... [--tick 300] [--elf build/out/SRB2.ELF] [--hwargs '...'] [--out build/fx]
Result: build/fx/sweep_<tag>.json (metrics per map), panels build/fx/<tag>_m<N>.png, a table on stdout sorted by the mean absolute difference.
A map that does not run in the PS2 (OOM ...) is listed with the error line of the engine log.
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
T = ROOT / 'tools/ps2'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tag', required=True)
    ap.add_argument('--maps', required=True)
    ap.add_argument('--tick', type=int, default=300)
    ap.add_argument('--elf', default=str(ROOT / 'build/out/SRB2.ELF'))
    ap.add_argument('--hwargs', default='')
    ap.add_argument('--zreserve', default='1536')
    ap.add_argument('--out', default=str(ROOT / 'build/fx'))
    ap.add_argument('--shot', default='', help='-vidshot spec instead of k<tick> (a camera: k20=teleport~..;devmode~0,k300)')
    ap.add_argument('--cmd', default='')
    a = ap.parse_args()
    res = {}
    jf = Path(a.out) / f'sweep_{a.tag}.json'
    if jf.exists():
        res = json.loads(jf.read_text())
    for m in [x for x in a.maps.split(',') if x]:
        name = f'{a.tag}_m{m}'
        cmd = [sys.executable, str(T / 'fx_pair.py'), name, '--ref', f'm{m}_k{a.tick}', '--map', m, '--tick', str(a.tick), '--elf', a.elf, '--zreserve', a.zreserve]
        if a.shot:
            cmd += ['--shot', a.shot]
        if a.cmd:
            cmd += ['--cmd', a.cmd]
        if a.hwargs:
            cmd += ['--hwargs=' + a.hwargs]
        p = subprocess.run(cmd, capture_output=True, text=True)
        out = p.stdout + p.stderr
        js = None
        for line in out.splitlines()[::-1]:
            if line.startswith('{'):
                try:
                    js = json.loads(line)
                except ValueError:
                    pass
                break
        if js is None:
            boot = ROOT / 'build/runs' / f'fx_{name}' / 'boot.txt'
            err = ''
            if boot.exists():
                for l in boot.read_text(errors='replace').splitlines():
                    if re.search(r'I_Error|OOM:|Out of memory|HEAP CHECK|FATAL|WATCHDOG', l):
                        err = l.strip()[:160]
                        break
            js = {'error': err or out.strip()[-160:]}
        res[m] = js
        jf.write_text(json.dumps(res, indent=1))
        print(m, js, flush=True)
    rows = sorted([(v.get('mad', 999.0), k, v) for k, v in res.items()], reverse=True)
    print('map   MAD   blur  >48%  pc mean -> hw mean')
    for mad, k, v in rows:
        if 'mad' in v:
            print('%-4s %5.2f %5.2f %5.2f  %s -> %s' % (k, v['mad'], v['mad_blur'], v['over48'], v['pc_mean'], v['hw_mean']))
        else:
            print('%-4s ERROR %s' % (k, v.get('error')))


if __name__ == '__main__':
    main()
