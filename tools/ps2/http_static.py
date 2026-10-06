"""Static HTTP file server standing in for the "HTTP source" of a game server (cvar http_source) in the add-on download tests.

usage: python tools/ps2/http_static.py --dir DIR [--port 8091] [--bind 0.0.0.0] [--log FILE.jsonl] [--mode plain|chunked|notfound|slow]
GET /<name>?md5=<hex> answers DIR/<name> (the query is ignored, the md5 is logged); modes: chunked = Transfer-Encoding: chunked,
notfound = always 404 (the client has to fall back to the transfer over the game connection), slow = 64 KB pieces with a pause.
Every request is appended to --log as one JSON line: that file is the proof that the client really took the HTTP route.
"""
import argparse
import json
import sys
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

OPTS = None


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

    def do_GET(self):
        u = urllib.parse.urlsplit(self.path)
        name = urllib.parse.unquote(u.path).lstrip('/')
        q = urllib.parse.parse_qs(u.query)
        f = (Path(OPTS.dir) / name) if name and '..' not in name else None
        if OPTS.mode == 'notfound' or not f or not f.is_file():
            log(ev='get', path=self.path, peer=self.client_address[0], status=404, ua=self.headers.get('User-Agent', ''))
            self.send_response(404)
            self.send_header('Content-Length', '10')
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(b'not found\n')
            self.close_connection = True
            return
        data = f.read_bytes()
        log(ev='get', path=self.path, peer=self.client_address[0], status=200, size=len(data), md5=q.get('md5', [''])[0], ua=self.headers.get('User-Agent', ''))
        self.send_response(200)
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Connection', 'close')
        try:
            if OPTS.mode == 'chunked':
                self.send_header('Transfer-Encoding', 'chunked')
                self.end_headers()
                for i in range(0, len(data), 7000):
                    part = data[i:i + 7000]
                    self.wfile.write(b'%x\r\n' % len(part) + part + b'\r\n')
                self.wfile.write(b'0\r\n\r\n')
            else:
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                step = 65536 if OPTS.mode == 'slow' else len(data) or 1
                for i in range(0, len(data), step):
                    self.wfile.write(data[i:i + step])
                    if OPTS.mode == 'slow':
                        self.wfile.flush()
                        time.sleep(0.25)
            log(ev='done', path=self.path, size=len(data))
        except (ConnectionError, OSError) as e:
            log(ev='aborted', path=self.path, error=str(e))
        self.close_connection = True


def main():
    global OPTS
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', required=True)
    ap.add_argument('--port', type=int, default=8091)
    ap.add_argument('--bind', default='0.0.0.0')
    ap.add_argument('--log', default='')
    ap.add_argument('--mode', default='plain', choices=['plain', 'chunked', 'notfound', 'slow'])
    OPTS = ap.parse_args()
    srv = ThreadingHTTPServer((OPTS.bind, OPTS.port), H)
    print(f'http source on {OPTS.bind}:{OPTS.port} serving {OPTS.dir} ({OPTS.mode})', flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    sys.exit(main())
