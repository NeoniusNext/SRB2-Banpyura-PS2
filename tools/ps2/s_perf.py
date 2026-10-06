"""Phase profile (COP0 cycles per frame) of an existing ELF in PCSX2 without the PS2REF tracing cost.

usage: python tools/ps2/s_perf.py --name NAME --elf SRB2.ELF [--scenarios DEMO_001,DEMO_002,title] [--out build/opt-s/runs]
                                  [--pak build/pak-a] [--windows 9] [--args "extra engine args"]
perf_phases.py runs the engine with -ps2ref, so PS2Ref_Tic (a full game-state serialisation every tic) is charged to G_Ticker.
Here the ELF (a --ps2ref build, for -ps2prof) is started WITHOUT -ps2ref: the hooks stay inert. The emulator is started only by
tools/ps2/opt_run.py -> run_pcsx2.py (machine-wide lock). Result: <out>/<name>/<scenario>/summary.json + <out>/<name>/perf.json.
Demo: -timedemo (1050 tics, one tic per frame, window 0 = level load is dropped); title: 105 title frames; map:MAPxx: a level
warp (static view). The table prints the mean over the windows >= 1 of the demo."""
import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/ps2'))
import perf_phases as pp


def run_one(a, sc):
    cmd = [sys.executable, str(ROOT / 'tools/ps2/opt_run.py'), '--name', f'{a.name}/{sc}', '--elf', a.elf, '--out', a.out,
           '--pak', a.pak, '--no-ref', '--timeout', str(a.timeout)]
    if sc.startswith('DEMO_'):
        cmd += ['--demo', sc, '--until', f'PROF win={a.windows}']
    elif sc == 'title':
        cmd += ['--until', 'PROF win=1']
    elif sc.startswith('map:'):
        cmd += ['--map', sc[4:], '--until', 'PROF win=4']
    cmd += ['--', '-ps2prof'] + (['-skipintro'] if sc == 'title' else []) + a.args.split()
    p = subprocess.run(cmd, capture_output=True, text=True)
    run = Path(a.out) / a.name / sc
    (run / 'driver.log').write_text(p.stdout + p.stderr, encoding='utf-8')
    wins = pp.parse(run / 'boot.txt') if (run / 'boot.txt').exists() else []
    return p.returncode, wins


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--name', required=True)
    ap.add_argument('--elf', required=True)
    ap.add_argument('--scenarios', default='DEMO_001,DEMO_002,title')
    ap.add_argument('--out', default=str(ROOT / 'build/opt-s/runs'))
    ap.add_argument('--pak', default=str(ROOT / 'build/pak-a'))
    ap.add_argument('--windows', type=int, default=9)
    ap.add_argument('--timeout', type=float, default=900)
    ap.add_argument('--args', default='')
    a = ap.parse_args()
    result = {'name': a.name, 'elf': a.elf, 'scenarios': {}}
    for sc in [s for s in a.scenarios.split(',') if s]:
        rc, wins = run_one(a, sc)
        skip = 0 if sc == 'title' else 1
        use = [w for w in wins if w['win'] >= skip and w['win'] <= a.windows]
        s = pp.summarise(use, skip)
        result['scenarios'][sc] = {'rc': rc, 'summary': s, 'windows': wins}
        if not s:
            print(sc, 'rc', rc, 'NO PROF WINDOWS')
            continue
        print(f"{sc}: rc={rc} frames={s['frames']} " + ' '.join(f'{ph}={s[ph]/1e6:.3f}' for ph in pp.PHASES + ['total']))
    out = Path(a.out) / a.name
    out.mkdir(parents=True, exist_ok=True)
    (out / 'perf.json').write_text(json.dumps(result, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
