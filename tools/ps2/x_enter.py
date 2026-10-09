"""Presses Return in the X display of a running PC engine (OPT12 NET: an ORIGINAL SRB2 client, built without NETSYNC_DIAG, has no NETSYNC_AUTOENTER; its join screens need Enter).

usage: python3 tools/ps2/x_enter.py --match TEXT [--match TEXT2 ...] [--every 3] [--for 90]
The process whose command line holds every --match text (and is not this script) gives the display (DISPLAY in its environment, set by xvfb-run); Return goes through the XTEST
extension (libX11 + libXtst through ctypes: no xdotool in the container) every --every seconds for --for seconds.
"""
import argparse
import ctypes
import glob
import os
import sys
import time


def find_display(matches):
    me = os.getpid()
    for p in glob.glob('/proc/[0-9]*/cmdline'):
        pid = int(p.split('/')[2])
        if pid == me:
            continue
        try:
            cmd = open(p, 'rb').read().replace(b'\0', b' ').decode(errors='replace')
            if 'x_enter.py' in cmd or not all(m in cmd for m in matches):
                continue
            env = dict(i.split(b'=', 1) for i in open(f'/proc/{pid}/environ', 'rb').read().split(b'\0') if b'=' in i)
            if b'DISPLAY' in env:
                if b'XAUTHORITY' in env:  # xvfb-run keeps the display behind an authority file
                    os.environ['XAUTHORITY'] = env[b'XAUTHORITY'].decode()
                return env[b'DISPLAY'].decode(), pid
        except OSError:
            continue
    return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--match', action='append', required=True)
    ap.add_argument('--every', type=float, default=3.0)
    ap.add_argument('--for', dest='duration', type=float, default=90.0)
    a = ap.parse_args()
    t0 = time.time()
    display = None
    while display is None and time.time() - t0 < 30:
        display, pid = find_display(a.match)
        if display is None:
            time.sleep(1)
    if display is None:
        print('no process with a DISPLAY matches', a.match, flush=True)
        return 1
    x11 = ctypes.CDLL('libX11.so.6')
    xtst = ctypes.CDLL('libXtst.so.6')
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XKeysymToKeycode.restype = ctypes.c_ubyte
    x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    x11.XFlush.argtypes = [ctypes.c_void_p]
    xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
    d = x11.XOpenDisplay(display.encode())
    if not d:
        print('cannot open display', display, flush=True)
        return 1
    code = x11.XKeysymToKeycode(d, 0xFF0D)  # XK_Return
    print(f'display {display} of pid {pid}: Return (keycode {code}) every {a.every} s for {a.duration} s', flush=True)
    n = 0
    while time.time() - t0 < a.duration:
        xtst.XTestFakeKeyEvent(d, code, 1, 0)
        x11.XFlush(d)
        time.sleep(0.06)
        xtst.XTestFakeKeyEvent(d, code, 0, 0)
        x11.XFlush(d)
        n += 1
        time.sleep(a.every)
    print(f'{n} Return presses', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
