#!/usr/bin/env python3
"""HW renderer scenario runner on top of opt_run.py: one or several runs (in parallel), then the hwsum table.

usage: hwrun.py [--elf ELF] [--timeout S] [--par] NAME=DEMO_00n[:until] ... -- <engine args>
       hwrun.py ... NAME=map:MAP01[:until] ...   (a static map view: -skipintro -warp MAP01)
Every run gets '-renderer Hardware -zreserve 3072 -ps2prof' plus the engine args after '--'. Runs go to build/runs/NAME, the log is build/runs/NAME.out.
Without ':until' a demo runs to the end of the timedemo ('gametics in').
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PAK = '/home/user/SRB2-Banpyura-PS2/build/pak'


def main():
    args = sys.argv[1:]
    elf = str(ROOT / 'build/out/SRB2.ELF')
    timeout = '1500'
    par = False
    jobs = []
    extra = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == '--':
            extra = args[i + 1:]
            break
        if a == '--elf':
            elf = args[i + 1]; i += 2
        elif a == '--timeout':
            timeout = args[i + 1]; i += 2
        elif a == '--par':
            par = True; i += 1
        else:
            jobs.append(a); i += 1
    procs = []
    for j in jobs:
        name, spec = j.split('=', 1)
        per_run = spec.split('%')[1:]  # NAME=spec%arg%arg: engine arguments of this run only
        spec = spec.split('%')[0]
        parts = spec.split(':')
        cmd = [sys.executable, str(ROOT / 'tools/ps2/opt_run.py'), '--name', name, '--elf', elf, '--pak', PAK, '--out', str(ROOT / 'build/runs'), '--timeout', timeout]
        if parts[0] == 'map':
            cmd += ['--map', parts[1]]
            until = parts[2] if len(parts) > 2 else ''
        else:
            cmd += ['--demo', parts[0], '--no-ref']
            until = parts[1] if len(parts) > 1 else 'gametics in'
        if until:
            cmd += ['--until', until]
        cmd += ['--', '-renderer', 'Hardware', '-zreserve', '3072', '-ps2prof'] + per_run + extra
        out = open(ROOT / 'build/runs' / (name + '.out'), 'w') if (ROOT / 'build/runs').exists() else open('/dev/null', 'w')
        p = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT)
        procs.append((name, p))
        if not par:
            p.wait()
    for name, p in procs:
        p.wait()
        tail = (ROOT / 'build/runs' / (name + '.out')).read_text(errors='replace').strip().splitlines()[-4:]
        print(f'{name}: rc={p.returncode}')
        for t in tail:
            if t.startswith(' ') or 'rc=' in t:
                print(' ', t[:200])
    subprocess.run([sys.executable, str(ROOT / 'tools/ps2/hwsum.py'), '--skip', '1'] + [n for n, _ in procs])


if __name__ == '__main__':
    main()
