"""UDP probe for the PS2 network stand (OPT10-X): python3 tools/ps2/udp_probe.py HOST PORT [--bind 5030] [--seconds 120] [--interval 1.0]
Binds 0.0.0.0:<bind> (the fixed client port of a PC node), sends a datagram to HOST:PORT every interval and prints every datagram it receives.
Used to check what DEV9 Sockets mode of PCSX2 lets through in each direction (guest -> host: the PS2 "punch" console command; host -> guest: the
PS2 server's NETSTAT rx counter)."""
import argparse
import socket
import time

ap = argparse.ArgumentParser()
ap.add_argument('host')
ap.add_argument('port', type=int)
ap.add_argument('--bind', type=int, default=5030)
ap.add_argument('--seconds', type=float, default=120)
ap.add_argument('--interval', type=float, default=1.0)
a = ap.parse_args()
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('0.0.0.0', a.bind))
s.settimeout(0.2)
end = time.time() + a.seconds
nxt = 0
n = 0
print('probe bound', a.bind, 'target', a.host, a.port, flush=True)
while time.time() < end:
    if time.time() >= nxt:
        s.sendto(b'probe %d' % n, (a.host, a.port))
        print('tx', n, flush=True)
        n += 1
        nxt = time.time() + a.interval
    try:
        d, src = s.recvfrom(2048)
        print('rx', src, d[:40], flush=True)
    except socket.timeout:
        pass
