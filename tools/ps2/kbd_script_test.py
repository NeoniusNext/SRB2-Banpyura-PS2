"""OPT9-K: the -kbdscript path (no keyboard needed): the same RawEvent -> engine events code as a real keyboard, for the keys the emulated HID keyboard cannot send
(caps lock, num lock). Types into the console (opened with F12) and checks the executed command lines.

python tools/ps2/kbd_script_test.py --elf build/opt9-k/out/SRB2.ELF [--name scr1]
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NAMES = {' ': 'space'}


def tap_text(poll, text, step=3):
    out = []
    for ch in text:
        out.append('%d:%s' % (poll, NAMES.get(ch, ch)))
        poll += step
    return out, poll


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--name', default='scr1')
    ap.add_argument('--start', type=int, default=520, help='poll number of the first step (after the title screen is up)')
    ap.add_argument('--timeout', type=float, default=300)
    a = ap.parse_args()
    p = a.start
    steps = ['%d:f12' % p]
    p += 12
    expect = []

    def line(text_steps, want):
        nonlocal p
        steps.extend(text_steps)
        steps.append('%d:enter' % p)
        p += 10
        expect.append(want)

    # caps lock: capitals; shift while caps is on gives lowercase again
    s, q = tap_text(p, 'echo'); p = q
    s += ['%d:space' % p]; p += 3
    s += ['%d:+caps' % p, '%d:-caps' % (p + 1)]; p += 4
    t2, p = tap_text(p, 'capsx'); s += t2
    s += ['%d:+lshift' % p]; p += 2
    s += ['%d:y' % p]; p += 3
    s += ['%d:-lshift' % p]; p += 2
    s += ['%d:+caps' % p, '%d:-caps' % (p + 1)]; p += 4
    t3, p = tap_text(p, 'z'); s += t3
    line(s, '$echo CAPSXyz')
    # num lock: the keypad types digits only while it is on
    s, q = tap_text(p, 'echo'); p = q
    s += ['%d:space' % p]; p += 3
    s += ['%d:kp1' % p]; p += 3
    s += ['%d:+numlock' % p, '%d:-numlock' % (p + 1)]; p += 4
    s += ['%d:kp2' % p]; p += 3
    s += ['%d:+numlock' % p, '%d:-numlock' % (p + 1)]; p += 4
    s += ['%d:kp3' % p, '%d:kpdot' % (p + 3)]; p += 6
    line(s, '$echo 13.')
    # shift + keypad: the navigation function, no digit (as on a PC)
    s, q = tap_text(p, 'echo'); p = q
    s += ['%d:space' % p]; p += 3
    s += ['%d:+lshift' % p]; p += 2
    s += ['%d:kp4' % p]; p += 3
    s += ['%d:-lshift' % p]; p += 2
    s += ['%d:kp5' % p, '%d:kpplus' % (p + 3), '%d:kpmul' % (p + 6)]; p += 9
    line(s, '$echo 5+*')
    # right shift and the symbol keys
    s, q = tap_text(p, 'echo'); p = q
    s += ['%d:space' % p]; p += 3
    s += ['%d:+rshift' % p]; p += 2
    for k in ('quote', 'semicolon', 'comma', 'period', 'slash', 'grave'):
        s += ['%d:%s' % (p, k)]; p += 3
    s += ['%d:-rshift' % p]; p += 2
    line(s, '$echo ":<>?~')   # shift + ' ; , . / ` on a US keyboard
    s, q = tap_text(p, 'echo'); p = q
    s += ['%d:space' % p]; p += 3
    t2, p = tap_text(p, 'zzend'); s += t2
    line(s, '$echo zzend')
    steps.append('%d:f12' % p)
    scratch = ROOT / 'build/opt9-k/scratch'
    scratch.mkdir(parents=True, exist_ok=True)
    (scratch / 'kscript.txt').write_text(','.join(steps))
    cmd = [sys.executable, str(ROOT / 'tools/ps2/kbd_run.py'), '--name', a.name, '--elf', a.elf, '--ready', 'Entering main game loop',
           '--cfg', 'setcontrol "console" "f12"', '--until', 'zzend ', '--timeout', str(a.timeout), '--show', 'PS2 kbd: usb',
           '--files', str(scratch / 'kscript.txt'), '--', '-skipintro', '-kbdscript', 'file:kscript.txt', '-kbdlog']
    print('script steps:', len(steps), 'last poll', p)
    r = subprocess.run(cmd, capture_output=True, text=True)
    print(r.stdout[-400:])
    log = (ROOT / 'build/opt9-k/run' / a.name / 'boot.txt').read_text(errors='replace')  # the engine's own log: PCSX2's console log eats backslash and tilde
    lines = [re.sub(r'^\[[^\]]*\]\s?', '', l).rstrip() for l in log.splitlines()]
    bad = 0
    for want in expect:
        ok = want in lines
        print('%-24s %s' % (want, 'OK' if ok else 'FAIL'))
        bad += 0 if ok else 1
    seen = [l for l in lines if l.startswith('$echo')]
    print('console lines seen:', seen)
    print('ALL PASS' if not bad else 'FAIL (%d)' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
