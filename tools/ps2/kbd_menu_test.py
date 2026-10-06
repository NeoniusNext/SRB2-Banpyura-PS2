"""OPT9-K: menus through the REAL USB keyboard path in PCSX2 (usb_run.py -> HID keyboard -> ps2kbd.irx -> PS2Kbd_Poll): text fields and control binding, checked in the config the engine saves on exit.

python tools/ps2/kbd_menu_test.py --elf build/opt9-k/out/SRB2.ELF [--tag sw]

 run 1 (Multiplayer > Player 1): the player name field is cleared with Backspace and typed again (capitals via shift, a space, digits); the server
        address field ("Specify server address") takes digits and dots from the main keys and the keypad; Esc backs out; the console is opened with F12
        and "quit" saves the config: name "KbdUsr 7" is checked in the file.
 run 2 (Options > Player 1 Controls > Control configuration): Enter on "Move Forward", then 'z' binds it; Enter on "Move Backward", then left shift binds it;
        the saved config must have setcontrol "forward" "w" "z" and setcontrol "backward" "s" "lshift".
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def run(name, elf, script, until, extra=()):
    cmd = [sys.executable, str(ROOT / 'tools/ps2/kbd_run.py'), '--name', name, '--elf', elf, '--ready', 'Entering main game loop',
           '--cfg', 'setcontrol "console" "f12"', '--script', script, '--until', until, '--timeout', '300', '--show', 'PS2 kbd: usb',
           '--', '-skipintro', *extra]
    r = subprocess.run(cmd, capture_output=True, text=True)
    cfg = ROOT / 'build/opt9-k/run' / name / '.srb2/reference.cfg'
    return r, (cfg.read_text(errors='replace') if cfg.exists() else '')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--tag', default='sw')
    a = ap.parse_args()
    bad = 0

    # run 1: name + address fields
    s = ('6:key:enter;9:key:down;11:key:enter;15:key:down;'
         '17:type:192.168.0.12;19:key:bksp;21:key:0x62;22:key:0x63;24:key:up;'  # address field: ...0.1, keypad 2 3 -> 192.168.0.123
         '26:key:down;27:key:down;28:key:down;29:key:down;31:key:enter;'          # Player 1...
         '34:key:bksp;34.4:key:bksp;34.8:key:bksp;35.2:key:bksp;35.6:key:bksp;36:key:bksp;'  # "Sonic" -> ""
         '37:type:KbdUsr;39:key:space;40:type:7;42:key:esc;44:key:esc;'
         '47:key:f12;49:type:quit;50.5:key:enter')
    r, cfg = run('mn1-' + a.tag, a.elf, s, 'PS2BOOT exit code', ())
    m = re.search(r'^name "(.*)"', cfg, re.M)
    ok = bool(m) and m.group(1) == 'KbdUsr 7'
    print('player name typed with the USB keyboard -> saved name %r : %s' % (m.group(1) if m else None, 'OK' if ok else 'FAIL'))
    bad += 0 if ok else 1

    # run 2: control binding
    s = ('6:key:enter;9:key:down;10:key:down;12:key:enter;14:key:enter;18:key:enter;21:key:enter;24:key:z;'
         '27:key:down;28:key:enter;31:key:lshift;'
         '34:key:f12;36:type:quit;37.5:key:enter')
    r, cfg = run('mn2-' + a.tag, a.elf, s, 'PS2BOOT exit code', ())
    f = re.search(r'^setcontrol "forward" "(.*?)"(?: "(.*?)")?', cfg, re.M)
    b = re.search(r'^setcontrol "backward" "(.*?)"(?: "(.*?)")?', cfg, re.M)
    okf = bool(f) and f.group(1) == 'w' and f.group(2) == 'z'
    okb = bool(b) and b.group(1) == 's' and b.group(2) == 'lshift'
    print('forward  bound to %s : %s' % (f.groups() if f else None, 'OK' if okf else 'FAIL'))
    print('backward bound to %s : %s' % (b.groups() if b else None, 'OK' if okb else 'FAIL'))
    bad += (0 if okf else 1) + (0 if okb else 1)
    print('ALL PASS' if not bad else 'FAIL (%d)' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
