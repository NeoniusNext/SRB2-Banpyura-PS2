"""Captures the REAL master server's room and server lists (HTTP GET only: nothing is registered, no game server is contacted) and writes an anonymised copy for
tools/ps2/mock_masterserver.py --fixture DIR: the same format, room numbers, titles, ports, IPv4/IPv6 mix and chunked/odd shapes, but every address is replaced by
one from the documentation ranges (198.51.100.0/24, 2001:db8::/32) so that a client which asks the listed servers for their info talks to nobody real.

usage: python tools/ps2/ms_fixture.py [--url http://ds.ms.srb2.org/MS/0] [--out build/opt9-n/msfixture]
Writes OUT/rooms.txt (verbatim) and OUT/servers.txt (anonymised; the real addresses are not kept anywhere).
"""
import argparse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def get(url):
    req = urllib.request.Request(url, headers={'User-Agent': 'SRB2/v2.2.15 (ps2 fixture; read-only)'})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read().decode('utf-8', 'replace')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://ds.ms.srb2.org/MS/0')
    ap.add_argument('--out', default=str(ROOT / 'build/opt9-n/msfixture'))
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    rooms = get(a.url + '/rooms')
    servers = get(a.url + '/servers')
    (out / 'rooms.txt').write_text(rooms, encoding='utf-8', newline='')
    mapping = {}
    lines = []
    for line in servers.split('\n'):
        parts = line.split(' ')
        if len(parts) >= 4 and (':' in parts[0] or parts[0].count('.') == 3):
            ip = parts[0]
            if ip not in mapping:
                n = len(mapping) + 1
                mapping[ip] = f'2001:db8::{n:x}' if ':' in ip else f'198.51.100.{(n % 250) + 1}'
            parts[0] = mapping[ip]
            line = ' '.join(parts)
        lines.append(line)
    (out / 'servers.txt').write_text('\n'.join(lines), encoding='utf-8', newline='')
    n6 = sum(1 for v in mapping.values() if ':' in v)
    print(f'rooms {len(rooms)} B, servers {len(servers)} B, {len(mapping)} distinct hosts ({n6} IPv6) -> {out}')


if __name__ == '__main__':
    main()
