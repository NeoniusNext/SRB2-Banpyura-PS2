"""OPT9-K: run the engine in the USB copy of PCSX2 (D:/PCSX2-usb, [USB1] = HID keyboard) and type on the emulated keyboard (tools/ps2/usb_run.py).

usage: kbd_run.py --name NAME --elf SRB2.ELF [--script "T:key:grave;T:type:echo hi;T:key:enter"] [--ready TEXT] [--until TEXT] [--timeout 300]
                  [--cfg 'cvar "val"'] [--out build/opt9-k/run] [--pak build/opt6-s/pak] [--show TEXT,TEXT] -- engine args...
Stages <out>/<name>/ like opt_run.py (ELF, packs, .srb2/reference.cfg), starts usb_run.py (one emulator copy with its own lock; the keys are posted only
to that emulator's window), prints the lines of the emulator log (which carries the engine console: I_OutputMsg writes to stdout) that contain one of --show.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from opt_run import ROOT, stage  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--name', required=True)
    ap.add_argument('--elf', required=True)
    ap.add_argument('--pak', default=str(ROOT / 'build/opt6-s/pak'))
    ap.add_argument('--out', default=str(ROOT / 'build/opt9-k/run'))
    ap.add_argument('--script', default='')
    ap.add_argument('--script-file', default='')
    ap.add_argument('--ready', default='')
    ap.add_argument('--until', default='')
    ap.add_argument('--timeout', type=float, default=300)
    ap.add_argument('--cfg', default='')
    ap.add_argument('--demo', default='')
    ap.add_argument('--files', default='', help='comma separated files copied next to the ELF (add-ons)')
    ap.add_argument('--show', default='PS2 kbd,PS2USB,kbd-test,KBDT,I_Error,Error')
    ap.add_argument('--exe', default='D:/PCSX2-usbk/pcsx2-qt.exe', help="agent K's private copy: no PCSX2 hotkeys, no keyboard bindings on pad 1, USB1 = HID keyboard")
    ap.add_argument('extra', nargs='*')
    a = ap.parse_args()
    run = Path(a.out).resolve() / a.name
    stage(run, Path(a.elf).resolve(), Path(a.pak).resolve(), a.demo or None, a.cfg + ('\n' if a.cfg else ''))
    import shutil
    for f in [x for x in a.files.split(',') if x]:
        src = Path(f) if Path(f).is_absolute() else ROOT / f
        shutil.copy2(src, run / src.name)
    args = ['-logfile', 'boot.txt', '-config', 'reference.cfg', '-nolog', '-noendtxt'] + a.extra
    log = run / 'pcsx2.log'
    # PCSX2 -gameargs is cut at ~128 characters (OPT9-S): everything after "-logfile boot.txt" goes to <run>/ps2args, one argument per line
    (run / 'ps2args').write_text('\n'.join(args[2:]) + '\n')
    args = args[:2]
    cmd = [sys.executable, str(ROOT / 'tools/ps2/usb_run.py'), '--elf', str(run / 'SRB2.ELF'), '--log', str(log), '--args=' + ' '.join(args),
           '--timeout', str(a.timeout)]
    if a.script:
        cmd += ['--script', a.script]
    if a.script_file:
        cmd += ['--script-file', a.script_file]
    if a.ready:
        cmd += ['--ready', a.ready]
    if a.until:
        cmd += ['--until', a.until]
    if a.exe:
        cmd += ['--exe', a.exe]
    print(' '.join(cmd), flush=True)
    p = subprocess.run(cmd, capture_output=True, text=True, env=dict(os.environ))
    (run / 'run.log').write_text(p.stdout + p.stderr, encoding='utf-8')
    text = log.read_text(errors='replace') if log.exists() else ''
    keys = [k for k in a.show.split(',') if k]
    for line in text.splitlines():
        if any(k in line for k in keys):
            print(line)
    print('--- usb_run transcript tail')
    print('\n'.join(p.stdout.splitlines()[-12:]))
    print(f'{a.name}: rc={p.returncode} until={"yes" if a.until and a.until in text else "NO"}')
    return p.returncode


if __name__ == '__main__':
    sys.exit(main())
