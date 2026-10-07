"""Tiny UDP sniffer (AF_PACKET, needs root): python3 tools/ps2/udp_sniff.py [seconds] [port ...]
Prints source/destination, length and the first bytes of every UDP/IPv4 datagram (any interface, loopback included) whose port is one of the
given ones (default 5029 5030): what really crosses between PCSX2's DEV9 Sockets adapter and a PC node (OPT10-X network stand)."""
import socket
import struct
import sys
import time

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60
ports = {int(x) for x in sys.argv[2:]} or {5029, 5030}
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(3))
s.settimeout(0.5)
end = time.time() + secs
t0 = time.time()
while time.time() < end:
    try:
        pkt, meta = s.recvfrom(65535)
    except socket.timeout:
        continue
    off = 14 if meta[0] != 'lo' else 14  # both give an Ethernet header (lo: zero MACs)
    if len(pkt) < off + 20 or pkt[off] >> 4 != 4 or pkt[off + 9] != 17:
        continue
    ihl = (pkt[off] & 15) * 4
    src, dst = socket.inet_ntoa(pkt[off + 12:off + 16]), socket.inet_ntoa(pkt[off + 16:off + 20])
    sp, dp, ln = struct.unpack('!HHH', pkt[off + ihl:off + ihl + 6])
    if sp in ports or dp in ports:
        print(f'{time.time() - t0:8.2f} {meta[0]:5s} {src}:{sp} -> {dst}:{dp} len={ln - 8} {pkt[off + ihl + 8:off + ihl + 24].hex()}', flush=True)
