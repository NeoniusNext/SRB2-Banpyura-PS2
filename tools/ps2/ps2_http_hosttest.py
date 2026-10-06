"""Host test of src/ps2/ps2_curl.c (the PS2 master server HTTP client and the add-on download) against tools/ps2/mock_masterserver.py and a file server.

python tools/ps2/ps2_http_hosttest.py [--out build/opt9-n/httptest]
Runs the same scenario list against the mock in three modes: plain, --chunked, --redirect (redirect: base URL under "/old/MS/0"), then the streaming GET
(PS2HttpGet_*, PS2-137) against a local file server in the shapes real servers answer in: Content-Length, chunked (with extensions and a trailer),
no length at all, relative redirect, redirect loop, truncated body, empty file, header in two pieces, 404, a stall (timeout).
"""
import argparse
import hashlib
import random
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VCVARS = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat")


def build(work):
    work.mkdir(parents=True, exist_ok=True)
    bat = work / 'build.bat'
    bat.write_text(f'@echo off\ncall "{VCVARS}" x64 >nul 2>&1\ncl /nologo /std:c17 /O2 /W3 /D_CRT_SECURE_NO_WARNINGS '
                   f'"{ROOT}/tools/ps2/ps2_http_hosttest.c" "{ROOT}/src/ps2/ps2_curl.c" /I"{ROOT}/src/ps2" /Fe:httptest.exe /Fo:{work}\\ ws2_32.lib\n')
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


BLOB = random.Random(7).randbytes(3 * 1024 * 1024 + 123)


class FileHandler(BaseHTTPRequestHandler):
    """the HTTP source of a game server: files for the streaming GET in the odd shapes real servers answer in"""
    protocol_version = 'HTTP/1.1'

    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        path = self.path.split('?')[0]
        w = self.wfile
        try:
            if path == '/file/plain':
                self.send_response(200)
                self.send_header('Content-Length', str(len(BLOB)))
                self.end_headers()
                w.write(BLOB)
            elif path == '/file/chunked':
                self.send_response(200)
                self.send_header('Transfer-Encoding', 'chunked')
                self.end_headers()
                i, n = 0, 1
                while i < len(BLOB):
                    part = BLOB[i:i + n]
                    w.write(b'%x;ext=1\r\n' % len(part) + part + b'\r\n')
                    i += n
                    n = n * 3 % 40009 + 1
                w.write(b'0\r\nTrailer: x\r\n\r\n')
            elif path == '/file/noclen':
                self.send_response(200)
                self.send_header('Connection', 'close')
                self.end_headers()
                w.write(BLOB[:300000])
                self.close_connection = True
            elif path == '/file/redirect':
                self.send_response(302)
                self.send_header('Location', 'plain')  # relative to /file/
                self.send_header('Content-Length', '0')
                self.end_headers()
            elif path == '/file/loop':
                self.send_response(302)
                self.send_header('Location', '/file/loop')
                self.send_header('Content-Length', '0')
                self.end_headers()
            elif path == '/file/trunc':
                self.send_response(200)
                self.send_header('Content-Length', '1000')
                self.end_headers()
                w.write(BLOB[:500])
                self.close_connection = True
            elif path == '/file/empty':
                self.send_response(200)
                self.send_header('Content-Length', '0')
                self.end_headers()
            elif path == '/file/slowhead':
                w.write(b'HTTP/1.1 200 OK\r\nContent-Le')
                w.flush()
                time.sleep(0.4)
                w.write(b'ngth: 5000\r\n\r\n' + BLOB[:10])
                w.flush()
                time.sleep(0.4)
                w.write(BLOB[10:5000])
            elif path == '/file/stall':
                self.send_response(200)
                self.send_header('Content-Length', '5000')
                self.end_headers()
                w.write(BLOB[:100])
                w.flush()
                time.sleep(6)
            else:
                self.send_response(404)
                self.send_header('Content-Length', '10')
                self.end_headers()
                w.write(b'not found\n')
        except (ConnectionError, OSError):
            pass


def run_get(exe, work, port):
    srv = ThreadingHTTPServer(('127.0.0.1', port), FileHandler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    ok = True
    # name, expected rc (-CURLcode), expected body, stall seconds
    cases = [('plain', 0, BLOB, 5), ('chunked', 0, BLOB, 5), ('noclen', 0, BLOB[:300000], 5), ('redirect', 0, BLOB, 5), ('empty', 0, b'', 5),
             ('slowhead', 0, BLOB[:5000], 5), ('trunc', -56, BLOB[:500], 5), ('missing', -22, b'', 5), ('loop', -47, b'', 5), ('stall', -28, BLOB[:100], 2)]
    for name, rc, body, stall in cases:
        outfile = work / f'get-{name}.bin'
        outfile.unlink(missing_ok=True)
        t0 = time.time()
        r = subprocess.run([str(exe), 'get', f'http://127.0.0.1:{port}/file/{name}?md5=00', str(outfile), str(stall)], capture_output=True, text=True, timeout=120)
        got = outfile.read_bytes() if outfile.exists() else b''
        line = r.stdout.strip()
        good = (f'GET rc={rc} ' in line) and got == body
        ok &= good
        print(f'T get {name:10} {"PASS" if good else "FAIL"} {time.time() - t0:5.1f}s  {line}  body {len(got)} B sha1 {hashlib.sha1(got).hexdigest()[:8]}')
    srv.shutdown()
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=str(ROOT / 'build/opt9-n/httptest'))
    a = ap.parse_args()
    work = Path(a.out).resolve()
    exe = build(work)
    ok = True
    for i, mode in enumerate(('plain', 'chunked', 'redirect')):
        ok &= run_mode(exe, work, mode, 18080 + i)
    ok &= run_get(exe, work, 18090)
    print('ALL PASS' if ok else 'SOME FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
