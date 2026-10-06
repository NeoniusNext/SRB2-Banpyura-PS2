"""UDP echo server for the PS2 network probe (OPT6-S): python tools/ps2/udp_echo.py [port] [seconds]. Prints every datagram."""
import socket
import sys
import time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 5029
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 600
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('0.0.0.0', port))
s.settimeout(1.0)
end = time.time() + secs
print('echo listening on', port, flush=True)
while time.time() < end:
    try:
        d, a = s.recvfrom(2048)
    except socket.timeout:
        continue
    print('rx', a, d, flush=True)
    s.sendto(b'echo:' + d, a)
