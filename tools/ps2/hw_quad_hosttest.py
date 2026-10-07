"""Host test of src/ps2/hw/ps2_hw_quad.h (PS2-HW-72: a sprite quad that holds no pixel centre draws nothing), with a negative control.
usage: python3 tools/ps2/hw_quad_hosttest.py [--out build/ht-quad]   (gcc or cc on the PATH)"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=str(ROOT / 'build/ht-quad'))
    a = ap.parse_args()
    work = Path(a.out)
    work.mkdir(parents=True, exist_ok=True)
    cc = shutil.which('gcc') or shutil.which('cc')
    if not cc:
        print('no host C compiler')
        return 2
    exe = work / 'hw_quad_hosttest'
    r = subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', str(ROOT / 'tools/ps2/hw_quad_hosttest.c'), '-o', str(exe), '-lm'], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        return 1
    good = subprocess.run([str(exe)], capture_output=True, text=True)
    neg = subprocess.run([str(exe), 'neg=1'], capture_output=True, text=True)
    print('test:', good.stdout.strip().splitlines()[-1] if good.stdout.strip() else '', 'exit', good.returncode)
    print('negative control:', neg.stdout.strip().splitlines()[-1] if neg.stdout.strip() else '', 'exit', neg.returncode)
    ok = good.returncode == 0 and neg.returncode == 1
    print('QUAD TEST PASSED' if ok else 'QUAD TEST FAILED')
    if good.returncode:
        print(good.stdout[:2000])
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
