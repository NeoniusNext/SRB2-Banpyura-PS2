"""READ-ONLY relay of the real SRB2 master server for the PS2 network stand (OPT10-X).

usage: python3 tools/ps2/ms_relay.py [--port 8092] [--bind 0.0.0.0] [--log FILE.jsonl] [--upstream https://ds.ms.srb2.org]
The PS2 engine speaks plain HTTP (no TLS) and the Linux container reaches the real master server only over HTTPS through its egress proxy, so the PS2 is
pointed at this relay (cvar masterserver "http://<host>:8092/MS/0"), which answers
    GET /MS/0/rooms, /MS/0/servers, /MS/0/rooms/<id>/servers, /MS/0/versions/<id>      (nothing else)
with the real answer of <upstream> (one HTTP GET each). EVERYTHING ELSE is refused with 405/403 and never forwarded: no registration (POST .../register),
no update/unlist, no write of any kind ever reaches the real master server; no game server of the list is contacted by this script.
Every request is logged (JSON lines): that is the proof of what the PS2 asked for and what was refused.
"""
import argparse
import json
import re
import subprocess
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

OPTS = None
ALLOWED = re.compile(r'^/MS/0/(rooms|servers|rooms/\d+/servers|versions/\d+)$')


def log(**kw):
    kw['t'] = round(time.time(), 3)
    line = json.dumps(kw, sort_keys=True)
    print(line, flush=True)
    if OPTS.log:
        with open(OPTS.log, 'a', encoding='utf-8') as f:
            f.write(line + '\n')


class H(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, fmt, *args):
        pass

    def reply(self, code, body=b'', ctype='text/plain'):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(body)

    def refuse(self):
        log(method=self.command, path=self.path, refused=True)
        self.reply(405, b'read-only relay: only GET of the room/server/version lists\n')

    do_POST = do_PUT = do_DELETE = do_PATCH = refuse

    def do_GET(self):
        path = self.path.split('?')[0]
        if not ALLOWED.match(path):
            log(method='GET', path=self.path, refused=True)
            return self.reply(403, b'not on the read-only list\n')
        r = subprocess.run(['curl', '-sS', '-m', '25', '-X', 'GET', '-w', '\n%{http_code}', OPTS.upstream + path], capture_output=True)
        out = r.stdout
        code = 502
        body = b''
        if r.returncode == 0 and b'\n' in out:
            body, _, c = out.rpartition(b'\n')
            code = int(c or 502)
        log(method='GET', path=path, upstream_status=code, bytes=len(body), curl_rc=r.returncode)
        self.reply(code if code in (200, 404) else 502, body)


def main():
    global OPTS
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=8092)
    ap.add_argument('--bind', default='0.0.0.0')
    ap.add_argument('--log', default='')
    ap.add_argument('--upstream', default='https://ds.ms.srb2.org')
    OPTS = ap.parse_args()
    ThreadingHTTPServer((OPTS.bind, OPTS.port), H).serve_forever()


if __name__ == '__main__':
    sys.exit(main())
