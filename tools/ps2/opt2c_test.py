"""Equivalence tests of agent C's exact optimisations (tools/ps2/opt2c_test.c): MSVC x86 + x64 (/W3 /WX) and the EE in PCSX2.

usage: python tools/ps2/opt2c_test.py [--out DIR] [--scale F] [--host] [--ee] [--negative-control]
  --host              build with MSVC for x86 and x64 and run (default when neither --host nor --ee is given: both)
  --ee                build with the EE GCC (engine flags, release) and run through run_pcsx2.py (the lock is held there)
  --negative-control  host only: -DOPT2C_BROKEN, every test must report failures and the exit code must be 1
  --scale F           multiplies the number of cases per test (default 1)
Exit code 0 only if every executed run printed `OT PASS` and (for the negative control) failed as required.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import math_common as C  # noqa: E402

ROOT = C.ROOT
SRC = ROOT / 'src'


def host(out, scale, broken):
    ok = True
    for arch in ('x86', 'x64'):
        work = out / ('host-' + arch + ('-broken' if broken else ''))
        extra = ['/DPS2_PROFILE', '/WX', '/I' + str(SRC), '/I' + str(SRC / 'ps2')]
        if broken:
            extra.append('/DOPT2C_BROKEN')
        (work).mkdir(parents=True, exist_ok=True)
        (work / 'stubs.c').write_text('#include <string.h>\n#include <stddef.h>\nvoid *M_Memcpy(void *a, const void *b, size_t n) { return memcpy(a, b, n); }\n')
        exe = C.msvc_build(work, 'opt2c', [
            (ROOT / 'tools/ps2/opt2c_test.c', 'main.obj', extra),
            (SRC / 'm_fixed.c', 'fixed.obj', ['/DPS2_PROFILE', '/I' + str(SRC), '/I' + str(SRC / 'ps2')]),
            (work / 'stubs.c', 'stubs.obj', []),
        ], arch=arch)
        rc, text = C.run(exe, [str(scale)], log=work / 'run.log')
        print(arch + (' (negative control)' if broken else ''))
        print(text.strip())
        if broken:
            if rc == 0 or 'OT PASS' in text:
                print('NEGATIVE CONTROL NOT DETECTED')
                ok = False
        elif rc != 0 or 'OT PASS' not in text:
            ok = False
    return ok


def ee(out, scale):
    import build as B
    work = out / 'ee'
    work.mkdir(parents=True, exist_ok=True)
    B.gen_config()
    objs = []
    for src in (ROOT / 'tools/ps2/opt2c_test.c', SRC / 'm_fixed.c'):
        obj = work / (src.stem + '.o')
        flags = [f for f in B.CFLAGS if not f.startswith('-MMD') and f != '-MP']
        cmd = [str(B.CC)] + flags + B.INCS + ['-c', str(src), '-o', str(obj)]
        p = subprocess.run(cmd, env=B.ENV, capture_output=True, text=True, cwd=ROOT)
        if p.returncode or (p.stdout + p.stderr).strip():
            print(p.stdout + p.stderr)
            return False
        objs.append(obj)
    (work / 'stubs.c').write_text('#include <string.h>\nvoid *M_Memcpy(void *a, const void *b, unsigned n) { return memcpy(a, b, n); }\n')
    obj = work / 'stubs.o'
    p = subprocess.run([str(B.CC)] + [f for f in B.CFLAGS if not f.startswith('-MMD') and f != '-MP'] + B.INCS + ['-c', str(work / 'stubs.c'), '-o', str(obj)],
                       env=B.ENV, capture_output=True, text=True, cwd=ROOT)
    if p.returncode:
        print(p.stdout + p.stderr)
        return False
    objs.append(obj)
    elf = work / 'OPT2C_TEST.ELF'
    p = subprocess.run([str(B.CC)] + B.LDFLAGS + [str(o) for o in objs] + ['-o', str(elf), '-Wl,-Map=' + str(work / 'test.map'), '-ldebug', '-lpatches', '-lm'],
                       env=B.ENV, capture_output=True, text=True, cwd=ROOT)
    if p.returncode or (p.stdout + p.stderr).strip():
        print(p.stdout + p.stderr)
        return False
    log = work / 'run.log'
    # the EE has no cheap 64-bit random loop: a tenth of the host case count
    rc = subprocess.run([sys.executable, str(ROOT / 'tools/ps2/run_pcsx2.py'), '--elf', str(elf), '--log', str(log), '--until', 'OT DONE',
                         '--args=' + str(scale), '--timeout', '1500']).returncode
    text = log.read_text(errors='replace') if log.exists() else ''
    lines = [l[l.index('OT '):] for l in text.splitlines() if 'OT ' in l]
    print('EE')
    print('\n'.join(lines))
    return rc == 0 and any(l.startswith('OT PASS') for l in lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, default=ROOT / 'build/opt2-c/tests')
    ap.add_argument('--scale', type=float, default=1.0)
    ap.add_argument('--host', action='store_true')
    ap.add_argument('--ee', action='store_true')
    ap.add_argument('--negative-control', action='store_true')
    a = ap.parse_args()
    both = not a.host and not a.ee
    ok = True
    if a.host or both:
        ok = host(a.out.resolve(), a.scale, False) and ok
        if a.negative_control:
            ok = host(a.out.resolve(), a.scale, True) and ok
    if a.ee or both:
        ok = ee(a.out.resolve(), a.scale * 0.1) and ok
    print('RESULT', 'PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
