"""OPT9-M: run the engine in the USB stand (D:/PCSX2-usb through tools/ps2/usb_run.py) and show the lines that matter.

usage: usb_engine.py --name NAME --elf SRB2.ELF [--script "T:cmd,cmd;T:cmd"] [--ready TEXT] [--until TEXT] [--timeout 300]
                     [--cfg 'cvar "val"'] [--out build/opt9-m/run] [--pak build/opt6-s/pak] [--show TEXT,TEXT] [--demo DEMO_001]
                     [--pointer] [--nopad] [--set SECTION.KEY=VALUE] -- engine args...
Stages <out>/<name>/ like opt_run.py (ELF, packs, .srb2/reference.cfg), writes every engine argument after "-logfile boot.txt" to <run>/ps2args
(PCSX2 cuts -gameargs at ~128 characters), starts usb_run.py (one emulator copy with its own lock; keys are posted only to that emulator's window)
and prints the log lines that contain one of the --show texts. usb_run.py documents the script commands (key:, type:, btn:, sleep:, mv:/wheel: only
with --pointer).
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
    ap.add_argument('--out', default=str(ROOT / 'build/opt9-m/run'))
    ap.add_argument('--script', default='')
    ap.add_argument('--script-file', default='')
    ap.add_argument('--ready', default='')
    ap.add_argument('--until', default='')
    ap.add_argument('--timeout', type=float, default=300)
    ap.add_argument('--cfg', default='', help='extra reference.cfg lines, separated by ";"')
    ap.add_argument('--demo', default='')
    ap.add_argument('--files', default='', help='comma separated files copied next to the ELF')
    ap.add_argument('--show', default='PS2 mouse,PS2USB,PS2 kbd,MTEST,I_Error,Error,Warning')
    ap.add_argument('--exe', default='')
    ap.add_argument('--pointer', action='store_true')
    ap.add_argument('--nopad', action='store_true')
    ap.add_argument('--set', action='append', default=[])
    ap.add_argument('extra', nargs='*')
    a = ap.parse_args()
    run = Path(a.out).resolve() / a.name
    cfg = '\n'.join(x for x in a.cfg.split(';') if x)
    stage(run, Path(a.elf).resolve(), Path(a.pak).resolve(), a.demo or None, cfg + ('\n' if cfg else ''))
    import shutil
    for f in [x for x in a.files.split(',') if x]:
        src = Path(f) if Path(f).is_absolute() else ROOT / f
        shutil.copy2(src, run / src.name)
    args = ['-logfile', 'boot.txt', '-config', 'reference.cfg', '-nolog', '-noendtxt']
    if a.demo:
        args += ['-timedemo', a.demo + '.lmp']
    args += a.extra
    (run / 'ps2args').write_text('\n'.join(args[2:]) + '\n')
    log = run / 'pcsx2.log'
    cmd = [sys.executable, str(ROOT / 'tools/ps2/usb_run.py'), '--elf', str(run / 'SRB2.ELF'), '--log', str(log),
           '--args=' + ' '.join(args[:2]), '--timeout', str(a.timeout)]
    for opt, val in (('--script', a.script), ('--script-file', a.script_file), ('--ready', a.ready), ('--until', a.until), ('--exe', a.exe)):
        if val:
            cmd += [opt, val]
    if a.pointer:
        cmd.append('--pointer')
    if a.nopad:
        cmd.append('--nopad')
    for s in a.set:
        cmd += ['--set', s]
    print(' '.join(cmd), flush=True)
    p = subprocess.run(cmd, capture_output=True, text=True, env=dict(os.environ))
    (run / 'run.log').write_text(p.stdout + p.stderr, encoding='utf-8')
    text = log.read_text(errors='replace') if log.exists() else ''
    keys = [k for k in a.show.split(',') if k]
    for line in text.splitlines():
        if any(k in line for k in keys):
            print(line)
    print('--- usb_run:')
    print('\n'.join(l for l in p.stdout.splitlines() if l.startswith('usb_run') or l.startswith('pcsx2-usb')))
    return p.returncode


if __name__ == '__main__':
    sys.exit(main())
