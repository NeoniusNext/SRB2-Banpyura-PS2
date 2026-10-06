"""Host test of src/hardware/hw_sort.h (stable radix sort of the batch keys), x86 and x64, with a negative control.
usage: python tools/ps2/hw_sort_hosttest.py [--out build/opt3-h/sort]"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VCVARS = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=str(ROOT / 'build/opt3-h/sort'))
    a = ap.parse_args()
    work = Path(a.out)
    work.mkdir(parents=True, exist_ok=True)
    ok = True
    for arch in ('x64', 'x86'):
        bat = work / f'build-{arch}.bat'
        exe = f'hw_sort_hosttest-{arch}.exe'
        bat.write_text('\n'.join(['@echo off', f'call "{VCVARS}" {arch} >nul 2>&1',
                                  subprocess.list2cmdline(['cl', '/nologo', '/std:c17', '/O2', '/W3', '/WX', '/D_CRT_SECURE_NO_WARNINGS',
                                                           '/I' + str(ROOT / 'src/hardware'), str(ROOT / 'tools/ps2/hw_sort_hosttest.c'), '/Fe:' + exe,
                                                           '/Fo:hw_sort_hosttest-' + arch + '.obj'])]) + '\n')
        r = subprocess.run(['cmd', '/c', str(bat)], cwd=work, capture_output=True, text=True, encoding='oem', errors='replace')
        if r.returncode:
            print(r.stdout + r.stderr)
            return 1
        good = subprocess.run([str(work / exe)], capture_output=True, text=True)
        neg = subprocess.run([str(work / exe), 'neg=1'], capture_output=True, text=True)
        print(f'[{arch}] {good.stdout.strip()} exit={good.returncode}; negative control exit={neg.returncode} ({neg.stdout.strip()})')
        if good.returncode != 0 or neg.returncode != 1:
            ok = False
    print('SORT TEST PASSED' if ok else 'SORT TEST FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
