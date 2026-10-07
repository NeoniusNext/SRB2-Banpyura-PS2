"""Host (gcc, Linux) equivalence test of the OPT10-SW helpers (tools/ps2/sw_hosttest.c) against the original expressions.

usage: python3 tools/ps2/sw_hosttest.py [--out build/ht] [--negative-control]
The negative control rebuilds with -DSW_BROKEN (deliberately wrong reference) and must fail.
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def build_run(out, flags):
    out.mkdir(parents=True, exist_ok=True)
    stubs = out / 'stubs.c'
    stubs.write_text('#include <string.h>\nvoid *M_Memcpy(void *a, const void *b, size_t n) { return memcpy(a, b, n); }\n')
    exe = out / ('sw_hosttest_' + ('_'.join(f.lstrip('-D').replace('=', '') for f in flags) if flags else 'main'))
    cmd = ['gcc', '-O2', '-fwrapv', '-DPS2_PROFILE', '-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'src/ps2'), *flags,
           str(ROOT / 'tools/ps2/sw_hosttest.c'), str(ROOT / 'src/m_fixed.c'), str(stubs), '-o', str(exe), '-lm']
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        sys.exit(2)
    p = subprocess.run([str(exe)], capture_output=True, text=True)
    return p.returncode, p.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', type=Path, default=ROOT / 'build/ht')
    ap.add_argument('--negative-control', action='store_true')
    a = ap.parse_args()
    ok = True
    rc, text = build_run(a.out, [])
    print(text.strip())
    ok &= rc == 0 and 'SW DONE failures=0' in text
    if a.negative_control:
        for flag in ('-DSW_BROKEN', '-DPS2_NEGCTL=7', '-DPS2_NEGCTL=8'): # edge test, node scan x-test, unstable sort
            rc, text = build_run(a.out, [flag])
            print('negative control %s:' % flag, text.strip().splitlines()[-1] if text.strip() else '(no output)', '->', 'detected' if rc else 'NOT DETECTED')
            ok &= rc != 0
    print('RESULT', 'PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
