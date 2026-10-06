"""Per-phase EE cycle profile (COP0 Count) of the PS2 build in PCSX2: scenarios x variants -> JSON + table.

usage: python tools/ps2/perf_phases.py --variant NAME [--noopt LIST] [--scenarios DEMO_001,DEMO_002,title,...]
                                        [--out DIR] [--no-build] [--args "extra engine args"]
  --variant  label (directory name under --out)
  --noref    production code path: build.py --prof (profiler wrappers, NO PS2REF hooks) run by opt2c_prof_run.py; no golden compare,
             demos only (the PS2REF build adds 0.7-1.4 M cycles per tick of state serialisation to the gtick phase)
  --noopt    value of SRB2_PS2_NOOPT for the build: "all" / "1" = original code everywhere, or a list of groups
             (math,slope,segs,draw,gs) to switch off; empty = every optimisation on
  scenarios  DEMO_00n: -timedemo of the golden demo (1050 tics, one tic per frame); title: 105 title frames
Builds the --ps2ref ELF (with the profiler wrappers) into <out>/<variant>-build and runs each scenario through
tools/ps2/run_ps2_ref.py (which holds the PCSX2 lock). The engine prints "PROF win=..." lines (src/ps2/ps2_prof.c);
window 0 is the level load and is dropped. Result: <out>/<variant>/perf.json (cycles per rendered frame).
PCSX2 does not model the EE caches: the numbers are an estimate of instruction cost, not of cache stalls.
"""
import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PHASES = ['gtick', 'ptick', 'render', 'bsp', 'planes', 'masked', 'hud', 'gs', 'audio', 'sleep', 'other']


def parse(boot):
    wins = []
    for line in boot.read_text(errors='replace').splitlines():
        line = re.sub(r'^(?:\[[^\]\r\n]*\]\s*)?', '', line)
        if not line.startswith('PROF win='):
            continue
        kv = dict(p.split('=', 1) for p in line.split()[1:])
        w = {'win': int(kv['win']), 'frames': int(kv['frames']), 'tics': int(kv['tics']), 'total': int(kv['total'])}
        for ph in PHASES:
            cyc, calls = kv[ph].split('/')
            w[ph] = int(cyc)
            w[ph + '_calls'] = int(calls)
        wins.append(w)
    return wins


def summarise(wins, skip_first, last=None):
    # demos: windows 1..9 (a timedemo is 1050 tics = windows 0..9; the production run keeps printing windows of the title screen afterwards)
    use = [w for w in wins if w['win'] >= skip_first and w['frames'] > 0 and (last is None or w['win'] <= last)]
    frames = sum(w['frames'] for w in use)
    if not frames:
        return None
    out = {'windows': len(use), 'frames': frames, 'tics': sum(w['tics'] for w in use)}
    for ph in PHASES + ['total']:
        out[ph] = sum(w[ph] for w in use) / frames
    out['work'] = sum(out[ph] for ph in PHASES if ph not in ('other', 'sleep'))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--variant', required=True)
    ap.add_argument('--noopt', default='')
    ap.add_argument('--scenarios', default='DEMO_001,DEMO_002,DEMO_003,DEMO_004,title')
    ap.add_argument('--out', type=Path, default=ROOT / 'build/agent-b-perf')
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--noref', action='store_true')
    ap.add_argument('--timeout', type=int, default=900)
    ap.add_argument('--args', default='')
    a = ap.parse_args()
    run = (a.out / a.variant).resolve()
    run.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, SRB2_PS2_RUN=str(run))
    if a.noopt:
        env['SRB2_PS2_NOOPT'] = a.noopt
    else:
        env.pop('SRB2_PS2_NOOPT', None)
    result = {'variant': a.variant, 'noopt': a.noopt, 'scenarios': {}}
    passed = True
    built = a.no_build
    for sc in [s for s in a.scenarios.split(',') if s]:
        if a.noref:
            if sc == 'title':
                print('title: skipped (needs the PS2REF title mode)')
                continue
            cmd = [sys.executable, str(ROOT / 'tools/ps2/opt2c_prof_run.py'), '--demo', sc, '--timeout', str(a.timeout)]
            if built:
                cmd.append('--no-build')
            cmd += ['--'] + a.args.split()
        else:
            cmd = [sys.executable, str(ROOT / 'tools/ps2/run_ps2_ref.py'), '--mode', 'title' if sc == 'title' else 'demo', '--timeout', str(a.timeout)]
            if sc != 'title':
                cmd += ['--demo', sc]
            if built:
                cmd.append('--no-build')
            cmd += ['--', '-ps2prof'] + a.args.split()
        p = subprocess.run(cmd, env=env, capture_output=True, text=True)
        (run / f'run-{sc}.log').write_text(p.stdout + p.stderr, encoding='utf-8')
        built = True
        boot = run / 'boot.txt'
        wins = parse(boot) if boot.exists() else []
        (run / f'boot-{sc}.txt').write_text(boot.read_text(errors='replace') if boot.exists() else '', encoding='utf-8')
        s = summarise(wins, 1 if sc != 'title' else 0, 9 if sc != 'title' else None)
        # Timing is useful only for a completed, equivalent engine run.
        valid = p.returncode == 0 and s is not None
        passed = passed and valid
        result['scenarios'][sc] = {'rc': p.returncode, 'passed': valid, 'summary': s, 'windows': wins}
        print(sc, 'rc', p.returncode, ('frames %d work %.0f total %.0f cycles/frame' % (s['frames'], s['work'], s['total'])) if s else 'NO PROF LINES')
    result['passed'] = passed and bool(result['scenarios'])
    (run / 'perf.json').write_text(json.dumps(result, indent=1))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
