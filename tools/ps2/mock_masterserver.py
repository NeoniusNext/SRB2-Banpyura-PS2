"""Local mock of the SRB2 HTTP master server (API v1, the one src/netcode/http-mserv.c talks to) for the PS2 network tests.

usage: python tools/ps2/mock_masterserver.py [--port 8080] [--base /MS/0] [--log FILE] [--chunked] [--redirect] [--seed ip:port:title ...]
Endpoints (relative to --base):
  GET  rooms                       "id\\ntitle\\nmotd\\n\\n\\n" per room
  POST rooms/<room>/register       form port=&title=&version=  -> token line; the server's address is the peer address of the request
  POST servers/<token>/update      form title=
  POST servers/<token>/unlist
  GET  servers | rooms/<id>/servers   "room\\naddr port title version\\n..." sections separated by a blank line
  GET  versions/<modid>            "<version> <name>"
--chunked answers with Transfer-Encoding: chunked, --redirect serves the API under a 302 from "/" (the client must follow it).
Everything that happens is appended to --log as JSON lines: this file is the proof of what the PS2 host registered.
"""
import argparse
import json
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOMS = [(1, 'Standard', 'Standard rooms: any gametype.'), (2, 'Casual', 'Casual play, no cheats.'), (3, 'Custom', 'Mods and add-ons.')]
SERVERS = {}  # token -> dict
LOCK = threading.Lock()
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

    def reply(self, code, body, ctype='text/plain'):
        data = body.encode('utf-8') if isinstance(body, str) else body
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Connection', 'close')
        if OPTS.chunked and code == 200 and data:
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            i = 0
            while i < len(data):
                part = data[i:i + 23]  # small chunks: split lines across chunks on purpose
                self.wfile.write(b'%x\r\n' % len(part) + part + b'\r\n')
                i += 23
            self.wfile.write(b'0\r\n\r\n')
        else:
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        self.close_connection = True

    def route(self, method):
        u = urllib.parse.urlsplit(self.path)
        path = u.path
        query = urllib.parse.parse_qs(u.query)
        n = int(self.headers.get('Content-Length') or 0)
        form = urllib.parse.parse_qs(self.rfile.read(n).decode('utf-8', 'replace')) if n else {}
        peer = self.client_address[0]
        log(ev='request', method=method, path=self.path, peer=peer, form={k: v[0] for k, v in form.items()}, ua=self.headers.get('User-Agent', ''))
        if OPTS.redirect and path in ('/', '/old' + OPTS.base):
            self.send_response(302)
            self.send_header('Location', OPTS.base + '/servers' if path == '/' else OPTS.base)
            self.send_header('Content-Length', '0')
            self.send_header('Connection', 'close')
            self.end_headers()
            self.close_connection = True
            return
        if not path.startswith(OPTS.base + '/'):
            return self.reply(404, 'not found\n')
        rel = path[len(OPTS.base) + 1:].strip('/').split('/')
        with LOCK:
            if method == 'GET' and rel == ['rooms']:
                return self.reply(200, ''.join(f'{i}\n{t}\n{m}\n\n\n' for i, t, m in ROOMS))
            if method == 'POST' and len(rel) == 3 and rel[0] == 'rooms' and rel[2] == 'register':
                room = int(rel[1])
                token = f'tok{len(SERVERS) + 1:04d}'
                SERVERS[token] = {'room': room, 'addr': peer, 'port': form.get('port', ['5029'])[0], 'title': form.get('title', ['?'])[0],
                                  'version': form.get('version', ['?'])[0], 'seen': time.time()}
                log(ev='registered', token=token, **SERVERS[token])
                return self.reply(200, token + '\n')
            if method == 'POST' and len(rel) == 3 and rel[0] == 'servers' and rel[1] in SERVERS and rel[2] == 'update':
                SERVERS[rel[1]]['title'] = form.get('title', [SERVERS[rel[1]]['title']])[0]
                SERVERS[rel[1]]['seen'] = time.time()
                log(ev='updated', token=rel[1], title=SERVERS[rel[1]]['title'])
                return self.reply(200, 'ok\n')
            if method == 'POST' and len(rel) == 3 and rel[0] == 'servers' and rel[1] in SERVERS and rel[2] == 'unlist':
                s = SERVERS.pop(rel[1])
                log(ev='unlisted', token=rel[1], addr=s['addr'], port=s['port'])
                return self.reply(200, 'ok\n')
            if method == 'GET' and (rel == ['servers'] or (len(rel) == 3 and rel[0] == 'rooms' and rel[2] == 'servers')):
                want = int(rel[1]) if len(rel) == 3 else None
                out = []
                for room, _, _ in ROOMS:
                    if want is not None and room != want:
                        continue
                    lines = [f'{room}']
                    for tok, s in SERVERS.items():
                        if s['room'] == room:
                            lines.append(f"{s['addr']} {s['port']} {urllib.parse.quote(s['title'], safe='')} {s['version']}")
                    out.append('\n'.join(lines) + '\n')
                body = '\n'.join(out)  # sections: "<room>\n<server>\n...\n" + "\n"
                return self.reply(200, body)
            if method == 'GET' and len(rel) == 2 and rel[0] == 'versions':
                return self.reply(200, '0 none\n')
        return self.reply(404, 'not found\n')

    def do_GET(self):
        self.route('GET')

    def do_POST(self):
        self.route('POST')


def main():
    global OPTS
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=8080)
    ap.add_argument('--bind', default='0.0.0.0')
    ap.add_argument('--base', default='/MS/0')
    ap.add_argument('--log', default='')
    ap.add_argument('--chunked', action='store_true')
    ap.add_argument('--redirect', action='store_true')
    ap.add_argument('--seed', action='append', default=[], help='room:addr:port:title (pre-registered server)')
    OPTS = ap.parse_args()
    for i, s in enumerate(OPTS.seed):
        room, addr, port, title = s.split(':', 3)
        SERVERS[f'seed{i}'] = {'room': int(room), 'addr': addr, 'port': port, 'title': title, 'version': '2.2.15', 'seen': time.time()}
    srv = ThreadingHTTPServer((OPTS.bind, OPTS.port), H)
    print(f'mock master server on {OPTS.bind}:{OPTS.port}{OPTS.base}', flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    sys.exit(main())
