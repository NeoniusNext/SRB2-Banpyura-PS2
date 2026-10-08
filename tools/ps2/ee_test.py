#!/usr/bin/env python3
"""Build a standalone EE test program (plain C, no engine) and run it in PCSX2 (PS2-3xx audio kernels, tools/ps2/ee_tests/*.c).

usage: python3 tools/ps2/ee_test.py NAME [--src a.c --src b.c ...] [--until TEXT] [--timeout S] [--cflags "..."] [--args "..."] [--out DIR]
NAME is a test in tools/ps2/ee_tests/NAME.c (with more sources by --src). The ELF is build/ee_tests/NAME/NAME.ELF, the emulator log
build/ee_tests/NAME/run.log; the exit code is 0 if the text 'EETEST DONE' (or --until) appeared in the log.
Flags are those of the engine build (build.py CFLAGS) + -O2 -ffp-contract=off unless --cflags is given.
"""
import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build as B  # noqa: E402

ROOT = B.ROOT


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('--src', action='append', default=[])
    ap.add_argument('--until', default='EETEST DONE')
    ap.add_argument('--timeout', type=int, default=240)
    ap.add_argument('--cflags', default='')
    ap.add_argument('--args', default='')
    ap.add_argument('--out', default='')
    ap.add_argument('--build-only', action='store_true')
    a = ap.parse_args()
    out = Path(a.out) if a.out else ROOT / 'build/ee_tests' / a.name
    out.mkdir(parents=True, exist_ok=True)
    srcs = [ROOT / 'tools/ps2/ee_tests' / (a.name + '.c')] + [ROOT / s for s in a.src]
    flags = [f for f in B.CFLAGS if f not in ('-MMD', '-MP')]
    if a.cflags:
        flags += a.cflags.split()
    flags += ['-ffp-contract=off']
    objs = []
    for s in srcs:
        o = out / (s.stem + '.o')
        cmd = [str(B.CC), *flags, *B.INCS, '-c', str(s), '-o', str(o)]
        r = subprocess.run(cmd, env=B.ENV, capture_output=True, text=True)
        if r.returncode or r.stderr.strip():
            print(' '.join(cmd))
            print(r.stdout + r.stderr)
        if r.returncode:
            return 2
        objs.append(str(o))
    elf = out / (a.name + '.ELF')
    cmd = [str(B.CC), *B.LDFLAGS, *objs, '-o', str(elf), '-ldebug', '-lpatches', '-lm']
    r = subprocess.run(cmd, env=B.ENV, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        return 2
    print('built', elf)
    if a.build_only:
        return 0
    log = out / 'run.log'
    cmd = [sys.executable, str(ROOT / 'tools/ps2/run_pcsx2.py'), '--elf', str(elf), '--log', str(log), '--until', a.until, '--timeout', str(a.timeout)]
    if a.args:
        cmd += ['--args=' + a.args]
    rc = subprocess.run(cmd).returncode
    text = log.read_text(errors='replace') if log.exists() else ''
    for line in text.splitlines():
        if 'EETEST' in line:
            print(line)
    return rc


if __name__ == '__main__':
    sys.exit(main())
