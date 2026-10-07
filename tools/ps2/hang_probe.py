"""Watches the PS2 nodes of a net_session run; when an engine log stops growing for STALL seconds while the emulator is alive, reads the guest EE pc/ra with gdb
(5 samples; the PCSX2 AppImage keeps its symbols: _cpuRegistersPack + 0x2a8 = pc, + 0x1f0 = ra).  OPT10-X: this showed that a "silent" PS2 server was a stuck emulator
(EE in the BIOS idle loop 0x81fc0, GS thread in xcb_wait_for_reply), not a stuck game.

usage: python3 tools/ps2/hang_probe.py RUNDIR NM.txt tools/ps2/hang_probe.gdb      (RUNDIR = build/opt10-x/run/<scenario>, NM.txt = mips64r5900el-ps2-elf-nm -n SRB2.ELF)
Start it before the scenario; it ends when both nodes (srv, cli) were sampled or after 50 minutes. For more: gdb -p PID -batch -ex "thread apply all bt 8".
"""
import bisect
import os
import re
import subprocess
import sys
import time

RUN = sys.argv[1]  # e.g. build/opt10-x/run/soak-ps2srv-ps2cli
NM = sys.argv[2]
GDB = sys.argv[3]
STALL = 100
syms = []
for line in open(NM, errors='replace'):
    p = line.split()
    if len(p) == 3 and p[1] in 'tTwW':
        syms.append((int(p[0], 16), p[2]))
syms.sort()
keys = [s[0] for s in syms]


def sym(v):
    i = bisect.bisect_right(keys, v) - 1
    return f'{syms[i][1]}+{v - syms[i][0]:#x}' if i >= 0 else '?'


def pid_of(node):
    out = subprocess.run(['pgrep', '-af', f'{RUN}/{node}/SRB2.ELF'], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if 'AppRun' in line and 'xvfb-run' not in line and 'pgrep' not in line:
            return int(line.split()[0])
    return None


done = set()
t0 = time.time()
while time.time() - t0 < 3000:
    for node in ('srv', 'cli'):
        if node in done:
            continue
        log = os.path.join(RUN, node, 'boot.txt')
        try:
            age = time.time() - os.stat(log).st_mtime
        except OSError:
            continue
        pid = pid_of(node)
        if pid and age > STALL:
            done.add(node)
            print(f'[{time.strftime("%H:%M:%S")}] {node}: log idle {age:.0f} s, emulator pid {pid}; sampling', flush=True)
            for k in range(5):
                r = subprocess.run(['timeout', '120', 'gdb', '-p', str(pid), '-batch', '-x', GDB], capture_output=True, text=True)
                vals = re.findall(r'^\$\d+ = (0x[0-9a-f]+)', r.stdout, re.M)
                if len(vals) >= 2:
                    pc, ra = int(vals[0], 16), int(vals[1], 16)
                    print(f'  sample {k}: pc {pc:#x} {sym(pc)}  ra {ra:#x} {sym(ra & 0xffffffff)}', flush=True)
                else:
                    print('  sample failed:', r.stdout[-200:], r.stderr[-200:], flush=True)
                time.sleep(4)
    if len(done) == 2:
        break
    time.sleep(10)
print('probe end', flush=True)
