"""Check native REGLIST against the unchanged PACKED encoder on x86/x64."""
import argparse
import subprocess
import sys
from pathlib import Path
import hw_hosttest as H
sys.stdout.reconfigure(errors='replace')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default='build/ps2-arch-gs-native-host-20261004')
    a = ap.parse_args()
    work = Path(a.out).resolve()
    work.mkdir(parents=True, exist_ok=True)
    H.make_shim(work)
    H.copy_sources(work)
    ok = True
    for arch in ('x64', 'x86'):
        exe = work / f'oct4_gs-{arch}.exe'
        command = ['cl', '/nologo', '/std:c17', '/O2', '/W3', '/WX', '/D_CRT_SECURE_NO_WARNINGS',
                   '/D_USE_MATH_DEFINES', '/I'+str(work), str(H.ROOT/'tools/ps2/oct4_gs_hosttest.c'),
                   '/Fe:'+str(exe), '/Fo:'+str(work/f'oct4_gs-{arch}.obj')]
        bat = work/f'build-{arch}.bat'
        bat.write_text('@echo off\ncall "'+str(H.VCVARS)+'" '+arch+' >nul 2>&1\nif errorlevel 1 exit /b 1\n'+subprocess.list2cmdline(command)+'\n')
        r = subprocess.run(['cmd','/c',str(bat)],cwd=work,capture_output=True,text=True,encoding='utf-8',errors='replace')
        (work/f'build-{arch}.log').write_text(r.stdout+r.stderr, encoding="utf-8")
        if r.returncode:
            print(r.stdout+r.stderr); return 1
        for neg in (0,1,2,3):
            r = subprocess.run([str(exe),str(neg)],capture_output=True,text=True)
            output = r.stdout+r.stderr
            (work/f'run-{arch}-{neg}.log').write_text(output, encoding="utf-8")
            good = (r.returncode == 0) if not neg else (r.returncode == 1 and 'mutated=1' in output and 'HT FAIL native_reglist' in output)
            print(f'{arch} negative={neg} passed={good}\n'+output[-650:])
            ok &= good
    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main())
