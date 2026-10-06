"""Host test of src/ps2/ps2_curl.c (the PS2 master server HTTP client) against tools/ps2/mock_masterserver.py.

python tools/ps2/ps2_http_hosttest.py [--out build/opt7-s/httptest]
Runs the same scenario list against the mock in three modes: plain, --chunked, --redirect (redirect: base URL under "/old/MS/0").
"""
import argparse
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VCVARS = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat")


def build(work):
    work.mkdir(parents=True, exist_ok=True)
    bat = work / 'build.bat'
    bat.write_text(f'@echo off\ncall "{VCVARS}" x64 >nul 2>&1\ncl /nologo /std:c17 /O2 /W3 /D_CRT_SECURE_NO_WARNINGS '
                   f'"{ROOT}/tools/ps2/ps2_http_hosttest.c" "{ROOT}/src/ps2/ps2_curl.c" /I"{ROOT}/src/ps2" /Fe:httptest.exe /Fo:{work}\ ws2_32.lib\n')
    r = subprocess.run(['cmd', '/c', str(bat)], cwd=work, capture_output=True, text=True, encoding='oem', errors='replace')
    (work / 'build.log').write_text(r.stdout + r.stderr)
    if r.returncode or not (work / 'httptest.exe').exists():
        print(r.stdout + r.stderr)
        raise SystemExit('build failed')
    return work / 'httptest.exe'


def run_mode(exe, work, mode, port):
    log = work / f'mock-{mode}.jsonl'
    log.unlink(missing_ok=True)
    args = [sys.executable, str(ROOT / 'tools/ps2/mock_masterserver.py'), '--port', str(port), '--log', str(log), '--bind', '127.0.0.1']
    base = '/MS/0'
    if mode == 'chunked':
        args.append('--chunked')
    if mode == 'redirect':
        args.append('--redirect')
    srv = subprocess.Popen(args, stdout=subprocess.DEVNULL)
    time.sleep(1.0)
    try:
        t0 = time.time()
        r = subprocess.run([str(exe), f'http://127.0.0.1:{port}{base}'] + (['redirect'] if mode == 'redirect' else []), capture_output=True, text=True, timeout=120)
        dt = time.time() - t0
    finally:
        srv.terminate()
        srv.wait()
    (work / f'out-{mode}.txt').write_text(r.stdout + r.stderr)
    print(f'--- mode {mode} ({dt:.1f}s) rc={r.returncode}')
    print(r.stdout)
    return r.returncode == 0 and 'RESULT PASS' in r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=str(ROOT / 'build/opt7-s/httptest'))
    a = ap.parse_args()
    work = Path(a.out)
    exe = build(work)
    ok = True
    for i, mode in enumerate(('plain', 'chunked', 'redirect')):
        ok &= run_mode(exe, work, mode, 18080 + i)
    # redirect mode: the client is pointed at /old/MS/0 and must follow the 302 to /MS/0; only the rooms request is checked
    print('ALL PASS' if ok else 'SOME FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
