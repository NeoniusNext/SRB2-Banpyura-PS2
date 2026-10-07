#!/usr/bin/env python3
"""HW pictures of many maps in two modes (A/B), one emulator run per map and mode, N runs in parallel.

usage: hwpics.py [--elf ELF] [--b-elf ELF2] [--par N] [--tag T] [--shots k100,k300] [--a-args "..."] [--b-args "-hwdbg 33554432"] MAP ...
Runs build/runs/pic_<tag>_a_<MAP> (default arguments) and pic_<tag>_b_<MAP> (with --b-args), each a static view of the map
('-skipintro -warp MAP -vidshot SHOTS'), then compares the pictures with shotcmp.py semantics and prints the differing share.
"""
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PAK = '/home/user/SRB2-Banpyura-PS2/build/pak'


def run(name, mp, elf, shots, extra, timeout):
    cmd = [sys.executable, str(ROOT / 'tools/ps2/opt_run.py'), '--name', name, '--elf', elf, '--pak', PAK, '--out', str(ROOT / 'build/runs'),
           '--timeout', str(timeout), '--until', 'VIDSHOT COMPLETE', '--map', mp, '--',
           '-renderer', 'Hardware', '-zreserve', '3072', '-vidshot', shots] + extra
    with open(ROOT / 'build/runs' / (name + '.out'), 'w') as out:
        subprocess.run(cmd, stdout=out, stderr=subprocess.STDOUT)
    try:
        (ROOT / 'build/runs' / name / 'pcsx2.log').unlink()  # the emulator log can reach gigabytes in an exception storm
    except OSError:
        pass
    log = (ROOT / 'build/runs' / name / 'boot.txt')
    txt = log.read_text(errors='replace') if log.exists() else ''
    err = 'OOM' if 'OOM:' in txt else ('ERR' if 'I_Error' in txt else 'ok')
    return name, err


def main():
    args = sys.argv[1:]
    elf = str(ROOT / 'build/out/SRB2.ELF')
    par = 2
    tag = 'x'
    shots = 'k100,k250'
    bargs = ['-hwdbg', '33554432']
    aargs = []
    belf = None
    maps = []
    i = 0
    while i < len(args):
        if args[i] == '--elf':
            elf = args[i + 1]; i += 2
        elif args[i] == '--par':
            par = int(args[i + 1]); i += 2
        elif args[i] == '--tag':
            tag = args[i + 1]; i += 2
        elif args[i] == '--shots':
            shots = args[i + 1]; i += 2
        elif args[i] == '--b-elf':
            belf = args[i + 1]; i += 2
        elif args[i] == '--b-args':
            bargs = args[i + 1].split(); i += 2
        elif args[i] == '--a-args':
            aargs = args[i + 1].split(); i += 2
        else:
            maps.append(args[i]); i += 1
    jobs = []
    for m in maps:
        jobs.append((f'pic_{tag}_a_{m}', m, aargs, elf))
        jobs.append((f'pic_{tag}_b_{m}', m, bargs, belf or elf))
    with ThreadPoolExecutor(max_workers=par) as ex:
        futs = [ex.submit(run, n, m, el, shots, e, 900) for n, m, e, el in jobs]
        for f in futs:
            n, e = f.result()
            print(n, e, flush=True)
    for m in maps:
        subprocess.run([sys.executable, str(ROOT / 'tools/ps2/shotcmp.py'), f'pic_{tag}_a_{m}', f'pic_{tag}_b_{m}', '--out', str(ROOT / 'build/runs' / f'pic_{tag}_{m}.png')])


if __name__ == '__main__':
    main()
