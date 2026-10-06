"""OPT9-K: every typing key, plain and shifted, through -kbdscript into the console; the executed command line must be exactly what a US keyboard types.

python tools/ps2/kbd_script_all.py --elf build/opt9-k/out/SRB2.ELF [--name all1]
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/ps2'))
from kbd_map_hosttest import text_reference  # noqa: E402

LETTERS = 'abcdefghijklmnopqrstuvwxyz'
NAMES = {}
for i, c in enumerate(LETTERS):
    NAMES[4 + i] = c
for i in range(9):
    NAMES[30 + i] = str(i + 1)
NAMES[39] = '0'
NAMES.update({44: 'space', 45: 'minus', 46: 'equals', 47: 'lbracket', 48: 'rbracket', 49: 'backslash', 50: 'hash', 51: 'semicolon', 52: 'quote',
              53: 'grave', 54: 'comma', 55: 'period', 56: 'slash', 100: 'nonusbs'})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--name', default='all1')
    ap.add_argument('--start', type=int, default=520)
    ap.add_argument('--timeout', type=float, default=300)
    a = ap.parse_args()
    keys = sorted(NAMES)
    p = a.start
    steps = ['%d:f12' % p]
    p += 12
    expect = []
    for shift in (0, 1):
        for chunk in (keys[:26], keys[26:]):  # two console lines per shift state
            for ch in 'echo ':
                steps.append('%d:%s' % (p, 'space' if ch == ' ' else ch)); p += 3
            want = 'echo '
            if shift:
                steps.append('%d:+lshift' % p); p += 2
            for u in chunk:
                steps.append('%d:%s' % (p, NAMES[u])); p += 3
                t = text_reference(u, shift, 0, 1)
                if t:
                    want += chr(t)
            if shift:
                steps.append('%d:-lshift' % p); p += 2
            steps.append('%d:enter' % p); p += 10
            expect.append('$' + want)
    for ch in 'echo zzend':
        steps.append('%d:%s' % (p, 'space' if ch == ' ' else ch)); p += 3
    steps.append('%d:enter' % p)
    p += 10
    steps.append('%d:f12' % p)
    scratch = ROOT / 'build/opt9-k/scratch'
    scratch.mkdir(parents=True, exist_ok=True)
    (scratch / 'kscript_all.txt').write_text(','.join(steps))
    cmd = [sys.executable, str(ROOT / 'tools/ps2/kbd_run.py'), '--name', a.name, '--elf', a.elf, '--ready', 'Entering main game loop',
           '--cfg', 'setcontrol "console" "f12"', '--until', 'zzend ', '--timeout', str(a.timeout), '--show', 'PS2 kbd: usb',
           '--files', str(scratch / 'kscript_all.txt'), '--', '-skipintro', '-kbdscript', 'file:kscript_all.txt']
    print('script steps:', len(steps), 'last poll', p)
    r = subprocess.run(cmd, capture_output=True, text=True)
    print(r.stdout[-300:])
    log = (ROOT / 'build/opt9-k/run' / a.name / 'boot.txt').read_text(errors='replace')  # the engine's own log: PCSX2's console log eats backslash and tilde
    lines = [re.sub(r'^\[[^\]]*\]\s?', '', l).rstrip() for l in log.splitlines()]
    seen = [l for l in lines if l.startswith('$echo')]
    bad = 0
    for want in expect:
        ok = want in seen
        print('%s %s' % ('OK  ' if ok else 'FAIL', want))
        if not ok:
            bad += 1
            near = [s for s in seen if s[:8] == want[:8] and s != want]
            print('     seen', near[:2])
    print('ALL PASS' if not bad else 'FAIL (%d)' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
