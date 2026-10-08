"""UDP relay with network impairments (OPT12 NET): loss, delay, jitter, duplicates, a bandwidth cap and "cable pulled" windows between a game client and a game server.

usage: python3 tools/ps2/udp_netem.py --listen 5031 --server 127.0.0.1:5029 [--bind 0.0.0.0] [--loss 5] [--delay 20] [--jitter 10] [--dup 0] [--rate 0]
                                      [--schedule "40:blackhole=5,90:loss=20,120:loss=0"] [--seed 1] [--log FILE.jsonl]
The client talks to --listen, the server sees the relay (one server-facing socket per client address, so a client keeps its server port). Both directions get the same impairments
(--loss / --dup in percent, --delay / --jitter in ms, --rate in KB/s, 0 = unlimited). --schedule changes settings at seconds after the first packet:
"T:blackhole=S" drops everything for S seconds (the cable is pulled), "T:loss=P", "T:delay=MS", "T:jitter=MS", "T:rate=KBPS" set the value from then on.
Counters of both directions (packets, bytes, dropped) are printed every 5 s and at the end (SIGINT/SIGTERM) and written to --log as JSON lines.
"""
import argparse
import heapq
import json
import random
import select
import signal
import socket
import sys
import time


def parse_sched(s):
    out = []
    for item in [x for x in s.split(',') if x]:
        t, _, kv = item.partition(':')
        k, _, v = kv.partition('=')
        out.append((float(t), k, float(v)))
    return sorted(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--listen', type=int, required=True)
    ap.add_argument('--bind', default='0.0.0.0')
    ap.add_argument('--server', required=True)
    ap.add_argument('--loss', type=float, default=0)
    ap.add_argument('--delay', type=float, default=0)
    ap.add_argument('--jitter', type=float, default=0)
    ap.add_argument('--dup', type=float, default=0)
    ap.add_argument('--rate', type=float, default=0)
    ap.add_argument('--schedule', default='')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--log', default='')
    a = ap.parse_args()
    rnd = random.Random(a.seed)
    host, _, port = a.server.rpartition(':')
    server = (host, int(port))
    front = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    front.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    front.bind((a.bind, a.listen))
    back = {}  # client address -> server-facing socket
    rev = {}  # server-facing socket -> client address
    cfg = {'loss': a.loss, 'delay': a.delay, 'jitter': a.jitter, 'rate': a.rate}
    sched = parse_sched(a.schedule)
    blackhole_until = 0.0
    t0 = None
    queue = []  # (release time, seq, sock, dest, data, direction)
    seq = 0
    stats = {d: {'packets': 0, 'bytes': 0, 'dropped': 0, 'dup': 0} for d in ('up', 'down')}
    link_free = {'up': 0.0, 'down': 0.0}
    stop = [False]
    logf = open(a.log, 'w') if a.log else None
    last_print = time.time()

    def on_sig(*_):
        stop[0] = True
    signal.signal(signal.SIGINT, on_sig)
    signal.signal(signal.SIGTERM, on_sig)

    def emit(ev):
        ev['t'] = round(time.time() - (t0 or time.time()), 3)
        line = json.dumps(ev)
        if logf:
            logf.write(line + '\n')
            logf.flush()

    def handle(data, direction, sock, dest, now):
        nonlocal seq, blackhole_until
        st = stats[direction]
        st['packets'] += 1
        st['bytes'] += len(data)
        if now < blackhole_until or rnd.random() * 100 < cfg['loss']:
            st['dropped'] += 1
            return
        copies = 2 if rnd.random() * 100 < a.dup else 1
        if copies == 2:
            st['dup'] += 1
        for _ in range(copies):
            d = cfg['delay'] + (rnd.uniform(-cfg['jitter'], cfg['jitter']) if cfg['jitter'] else 0)
            rel = now + max(0.0, d) / 1000
            if cfg['rate']:
                tx = len(data) / (cfg['rate'] * 1000)
                link_free[direction] = max(link_free[direction], now) + tx
                rel = max(rel, link_free[direction])
            seq += 1
            heapq.heappush(queue, (rel, seq, sock, dest, data))

    while not stop[0]:
        now = time.time()
        timeout = 0.05
        if queue:
            timeout = max(0.0, min(timeout, queue[0][0] - now))
        rl, _, _ = select.select([front] + list(rev), [], [], timeout)
        now = time.time()
        for s in rl:
            data, addr = s.recvfrom(2048)
            if t0 is None:
                t0 = now
            if s is front:
                if addr not in back:
                    b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                    b.bind(('0.0.0.0', 0))
                    back[addr] = b
                    rev[b] = addr
                    emit({'ev': 'client', 'addr': f'{addr[0]}:{addr[1]}'})
                handle(data, 'up', back[addr], server, now)
            else:
                handle(data, 'down', front, rev[s], now)
        while queue and queue[0][0] <= now:
            _, _, sock, dest, data = heapq.heappop(queue)
            try:
                sock.sendto(data, dest)
            except OSError:
                pass
        if t0 is not None:
            while sched and now - t0 >= sched[0][0]:
                t, k, v = sched.pop(0)
                if k == 'blackhole':
                    blackhole_until = now + v
                else:
                    cfg[k] = v
                emit({'ev': 'schedule', 'what': k, 'value': v})
                print(f'[{now - t0:7.2f}] {k}={v}', flush=True)
        if now - last_print >= 5 and t0 is not None:
            last_print = now
            print(f'[{now - t0:7.2f}] up {stats["up"]} down {stats["down"]}', flush=True)
            emit({'ev': 'stats', 'up': dict(stats['up']), 'down': dict(stats['down'])})
    emit({'ev': 'end', 'up': stats['up'], 'down': stats['down']})
    print('end', json.dumps(stats), flush=True)


if __name__ == '__main__':
    sys.exit(main())
