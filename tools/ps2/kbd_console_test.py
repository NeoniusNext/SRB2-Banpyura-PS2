"""OPT9-K: console typing through the REAL USB keyboard path in PCSX2 (usb_run.py posts keys to the emulator's window; ps2kbd.irx -> PS2Kbd_Poll).

python tools/ps2/kbd_console_test.py --elf build/opt9-k/out/SRB2.ELF [--name con1] [--tag ps2/hw/full]

Opens the console with F12 (setcontrol "console" "f12": the emulated HID keyboard has no backtick, F1..F7/F10/F11 are menu keys), types command lines and checks what the engine
executed: the console echoes the command ("$echo ...") and the output of "echo". Checks: letters, shift (capitals and symbols), backspace, left/right/home/end, delete,
ctrl+a (select all), tab completion, caps lock, auto-repeat of a held key, keypad digits.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# (name, steps, expected "$..." command line as the console shows it, expected echo output or None)
# step: ('t', text) type text, ('k', key) tap, ('d', key) hold, ('u', key) release
CASES = [
    ('plain', [('t', 'echo hello world'), ('k', 'enter')], '$echo hello world', 'hello world'),
    ('caps', [('t', 'echo ABC xyz'), ('k', 'enter')], '$echo ABC xyz', 'ABC xyz'),
    ('shift-digits', [('t', 'echo'), ('k', 'space'), ('d', 'lshift'), ('k', '1'), ('k', '2'), ('k', '3'), ('u', 'lshift'), ('k', 'enter')], '$echo !@#', '!@#'),
    ('shift-sym', [('t', 'echo'), ('k', 'space'), ('d', 'lshift'), ('k', '0x6B'), ('u', 'lshift'), ('k', 'enter')], None, None),  # placeholder, replaced below
    ('backspace', [('t', 'echo abcX'), ('k', 'bksp'), ('k', 'enter')], '$echo abc', 'abc'),
    ('left-insert', [('t', 'echo ac'), ('k', 'left'), ('t', 'b'), ('k', 'enter')], '$echo abc', 'abc'),
    ('home', [('t', 'cho x'), ('k', 'home'), ('t', 'e'), ('k', 'enter')], '$echo x', 'x'),
    ('end', [('t', 'echo y'), ('k', 'home'), ('k', 'end'), ('t', 'z'), ('k', 'enter')], '$echo yz', 'yz'),
    ('delete', [('t', 'echo abXc'), ('k', 'left'), ('k', 'left'), ('k', 'del'), ('k', 'enter')], '$echo abc', 'abc'),
    ('right', [('t', 'echo ab'), ('k', 'left'), ('k', 'left'), ('k', 'right'), ('t', 'X'), ('k', 'enter')], '$echo aXb', 'aXb'),
    ('ctrl-a', [('t', 'foo'), ('d', 'lctrl'), ('k', 'a'), ('u', 'lctrl'), ('k', 'bksp'), ('t', 'echo sel'), ('k', 'enter')], '$echo sel', 'sel'),
    ('tab', [('t', 'ps2k'), ('k', 'tab'), ('k', 'enter')], '$ps2kbd', None),
    ('keypad', [('t', 'echo'), ('k', 'space'), ('k', '0x61'), ('k', '0x62'), ('k', '0x63'), ('k', 'enter')], '$echo 123', '123'),
]


def make_script(t0, gap):
    t = t0
    out = []
    for name, steps, _, _ in CASES:
        if not steps:
            continue
        for kind, arg in steps:
            if kind == 't':
                out.append('%.1f:type:%s' % (t, arg))
                t += 0.35 + 0.1 * len(arg)
            elif kind == 'k':
                out.append('%.1f:key:%s' % (t, arg))
                t += 0.35
            elif kind == 'd':
                out.append('%.1f:key:%s:down' % (t, arg))
                t += 0.3
            elif kind == 'u':
                out.append('%.1f:key:%s:up' % (t, arg))
                t += 0.3
        t += gap
    return ';'.join(out), t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--name', default='con1')
    ap.add_argument('--pak', default=str(ROOT / 'build/opt6-s/pak'))
    ap.add_argument('--timeout', type=float, default=400)
    a = ap.parse_args()
    # case 'shift-sym' uses the keypad-free path: shift + minus/equals/[ (usage 0x2D 0x2E 0x2F) -> "_+{"
    for i, c in enumerate(CASES):
        if c[0] == 'shift-sym':
            CASES[i] = ('shift-sym', [('t', 'echo'), ('k', 'space'), ('d', 'lshift'), ('k', '0xBD'), ('k', '0xBB'), ('k', '0xDB'), ('u', 'lshift'), ('k', 'enter')], '$echo _+{', '_+{')
    script, tend = make_script(3.0, 0.4)
    script = '1.0:key:f12;' + script + ';%.1f:type:echo zqxkw;%.1f:key:enter;%.1f:key:f12' % (tend + 1, tend + 2, tend + 3)
    cmd = [sys.executable, str(ROOT / 'tools/ps2/kbd_run.py'), '--name', a.name, '--elf', a.elf, '--pak', a.pak, '--ready', 'Entering main game loop',
           '--cfg', 'setcontrol "console" "f12"', '--script', script, '--until', 'zqxkw ', '--timeout', str(a.timeout), '--show', 'PS2 kbd: usb',
           '--', '-skipintro', '-kbdlog']
    print(' '.join(cmd[:8]), '...')
    p = subprocess.run(cmd, capture_output=True, text=True)
    print(p.stdout[-600:])
    log = (ROOT / 'build/opt9-k/run' / a.name / 'boot.txt').read_text(errors='replace')  # the engine's own log: PCSX2's console log eats backslash and tilde
    lines = [re.sub(r'^\[[^\]]*\]\s?', '', l) for l in log.splitlines()]
    bad = 0
    # the console prints the command line it executes ("$echo ..."), then the output of the command
    for name, steps, want_cmd, want_out in CASES:
        if want_cmd is None:
            continue
        idx = [i for i, l in enumerate(lines) if l.rstrip() == want_cmd]
        ok_cmd = bool(idx)
        ok_out = True
        if ok_cmd and want_out is not None:
            ok_out = any(lines[i + 1].rstrip() == want_out for i in idx if i + 1 < len(lines))
        print('%-14s %-22s %s' % (name, repr(want_cmd), 'OK' if ok_cmd and ok_out else ('FAIL (command line not seen)' if not ok_cmd else 'FAIL (output differs)')))
        bad += 0 if ok_cmd and ok_out else 1
    print('raw events seen:', sum(1 for l in lines if l.startswith('PS2 kbd: raw down')))
    print('ALL PASS' if not bad else 'FAIL (%d)' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
