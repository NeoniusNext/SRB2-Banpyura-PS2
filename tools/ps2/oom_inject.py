"""OPT11-STAB (PS2-170): fault injection for the recoverable out-of-memory paths (src/z_zone.c Z_GUARD_TRY, src/ps2/ps2_hwfb.c).

usage: oom_inject.py --elf SRB2.ELF --tag NAME [--out build/runs/inject] [--mode level|frame|hwlevel|hwframe] [--points 1,5,20,...] [--renderer Hardware]
                     [--first MAP01] [--second MAP02] [--timeout 600]

level    the Nth allocation made while the level of `-netcmd map` loads (any tag, `-zoomnth N -zoomafter 60`) fails; the game must go back to the
         title screen with a message, a later `map` command must load a level and play it, the zone heap check (ZA_Check) must pass
frame    the Nth allocation made during the drawing of frames 60.. fails (any tag): the software frame is drawn again / the hardware renderer is left
hwlevel  as `level` in the Hardware renderer (the fallback to software, then the return to hardware at the next map)
hwframe  as `frame` in the Hardware renderer

Per point: a run of opt_run.py (-zquitall at the end), result: rc, `ps2_hwfb:` counters, I_Error / HEAP CHECK lines, final ZSTAT.
A point passes when the run ends with ZQUIT DONE, no I_Error and no HEAP CHECK FAILED line, and the last level of the run is the second map.
"""
import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--tag', required=True)
    ap.add_argument('--out', default=str(ROOT / 'build/runs/inject'))
    ap.add_argument('--mode', default='level', choices=['level', 'frame', 'hwlevel', 'hwframe'])
    ap.add_argument('--points', default='1,2,3,5,8,13,21,34,55,89,144,233,377,610,987,1597,2584,4181,6765')
    ap.add_argument('--first', default='MAP01')
    ap.add_argument('--second', default='MAP02')
    ap.add_argument('--timeout', type=int, default=700)
    ap.add_argument('--pak', default='/home/user/SRB2-Banpyura-PS2/build/pak')
    a = ap.parse_args()
    hw = a.mode.startswith('hw')
    outdir = Path(a.out) / a.tag
    outdir.mkdir(parents=True, exist_ok=True)
    results = []
    for n in [int(x) for x in a.points.split(',') if x]:
        name = f'{a.tag}-{a.mode}-{n}'
        extra = ['-zck', '-zquitall', '900', '-zoomany', '-zoomnth', str(n), '-zoomafter', '60']
        if a.mode in ('level', 'hwlevel'):
            extra += ['-netcmd', f'60:map {a.second} -force|450:map {a.first} -force']
        if hw:
            extra = ['-renderer', 'Hardware'] + extra
        cmd = [sys.executable, '-B', str(ROOT / 'tools/ps2/opt_run.py'), '--name', name, '--elf', a.elf, '--out', str(outdir), '--pak', a.pak,
               '--map', a.first, '--timeout', str(a.timeout), '--until', 'ZQUIT DONE', '--'] + extra
        t0 = time.time()
        for _ in range(20):
            subprocess.run(cmd, capture_output=True, text=True)
            rl = outdir / name / 'run.log'
            if not (rl.exists() and 'could not get the PCSX2 lock' in rl.read_text(errors='replace')):
                break
        text = (outdir / name / 'boot.txt').read_text(errors='replace') if (outdir / name / 'boot.txt').exists() else ''
        done = 'ZQUIT DONE' in text
        ierr = [l.strip()[:160] for l in text.splitlines() if 'I_Error' in l or 'HEAP CHECK FAILED' in l]
        hwfb = [l.strip()[:300] for l in text.splitlines() if l.startswith('ps2_hwfb: fallbacks')]
        zst = [l for l in text.splitlines() if l.startswith('ZSTAT')]
        m = re.search(r'map=(\d+)', zst[-1]) if zst else None
        ev = [l.strip()[:140] for l in text.splitlines() if l.startswith('OOM (recoverable)') or 'LEVEL LOAD FAILED' in l or 'HARDWARE ->' in l]
        ok = done and not ierr
        results.append({'n': n, 'ok': ok, 'done': done, 'errors': ierr[:3], 'events': ev[:4], 'hwfb': hwfb[-1] if hwfb else '', 'map': m.group(1) if m else None,
                        'secs': round(time.time() - t0)})
        print(f"{a.mode} N={n}: {'ok' if ok else 'FAIL'} map={results[-1]['map']} events={len(ev)} {ev[:1]} {ierr[:1]} ({results[-1]['secs']}s)", flush=True)
        (outdir / 'results.json').write_text(json.dumps(results, indent=1))
    bad = [r['n'] for r in results if not r['ok']]
    print(f"inject {a.tag} {a.mode}: {len(results) - len(bad)}/{len(results)} ok; failed points: {bad if bad else 'none'}")
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
