"""Run one content test (OPT6-F) in PCSX2: the engine ELF + the packs + the add-ons you name, engine arguments after "--".

usage: ftest_run.py --name NAME --elf SRB2.ELF [--files a.pk3,b.wad,dir/...,SRC=REL/PATH] [--cfg 'cvar "val"'] [--ram 32|128]
                    [--until TEXT] [--timeout 600] [--out build/opt6-f/run] [--demo DEMO_001] -- engine args...
Stages <out>/<name>/ (like opt_run.py; the add-on files are copied next to the ELF, so "host:" = that directory and
"-file NAME.pk3" finds them), starts PCSX2 only through run_pcsx2.py (machine-wide lock) and waits for TEXT in boot.txt
(default "FT_DONE").  Prints the FT_ lines (and I_Error / OOM lines) of the engine log.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from opt_run import PCSX2, ROOT, stage  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--name', required=True)
    ap.add_argument('--elf', required=True)
    ap.add_argument('--files', default='')
    ap.add_argument('--cfg', default='')
    ap.add_argument('--ram', type=int, choices=[32, 128], default=32)
    ap.add_argument('--pak', default=str(ROOT / 'build/opt6-f/pak'))
    ap.add_argument('--out', default=str(ROOT / 'build/opt6-f/run'))
    ap.add_argument('--until', default='FT_DONE')
    ap.add_argument('--timeout', type=float, default=600)
    ap.add_argument('--demo', default='')
    ap.add_argument('--retries', type=int, default=4, help='restarts when the emulator itself did not start')
    ap.add_argument('--lock-wait', type=float, default=7200, help='seconds to wait for the machine-wide emulator lock (other agents run long sweeps)')
    ap.add_argument('--show', default='FT_,FTLUA,ZSTAT,I_Error,OOM,Out of memory,Lua,lua,error,Error,Added file', help='comma separated: log lines containing one of these are printed')
    ap.add_argument('extra', nargs='*')
    a = ap.parse_args()
    run = Path(a.out).resolve() / a.name
    stage(run, Path(a.elf).resolve(), Path(a.pak).resolve(), a.demo or None, a.cfg + ('\n' if a.cfg else ''))
    shutil.rmtree(run / 'refout', ignore_errors=True)
    boot = run / 'boot.txt'
    if boot.exists():
        boot.unlink()
    # FINEACON.DAT (lazily read data file next to the packs) is linked by opt_run.stage() together with the packs
    for f in [x for x in a.files.split(',') if x]:
        rel = None
        if '=' in f:  # SRC=REL/PATH: the file goes to <run>/REL/PATH (e.g. build/x/ZH.pk3=.srb2/addons/ZH.pk3 for the Add-ons menu folder)
            f, rel = f.split('=', 1)
        src = Path(f)
        if not src.is_absolute():
            src = ROOT / src
        dst = run / (rel or src.name)
        dst.parent.mkdir(parents=True, exist_ok=True)
        if src.is_dir():
            shutil.rmtree(dst, ignore_errors=True)
            shutil.copytree(src, dst)
        else:
            shutil.copy2(src, dst)
    args = ['-logfile', 'boot.txt', '-config', 'reference.cfg', '-nolog', '-noendtxt'] + a.extra
    # PCSX2 -gameargs is cut at ~128 characters: everything after "-logfile boot.txt" goes to <run>/ps2args, one argument per line (as opt_run.py)
    (run / 'ps2args').write_text('\n'.join(args[2:]) + '\n')
    args = args[:2]
    cmd = [sys.executable, str(ROOT / 'tools/ps2/run_pcsx2.py'), '--elf', str(run / 'SRB2.ELF'), '--log', str(run / 'pcsx2.log'),
           '--args=' + ' '.join(args), '--timeout', str(a.timeout), '--lock-wait', str(a.lock_wait), '--until-file', str(boot), '--until', a.until]
    env = dict(os.environ, SRB2_PCSX2=PCSX2[a.ram])
    print(' '.join(cmd), flush=True)
    for attempt in range(a.retries + 1):  # PCSX2 sometimes fails to start ("Failed to create swap chain", lock lost): start again
        if boot.exists():
            boot.unlink()
        p = subprocess.run(cmd, env=env, capture_output=True, text=True)
        (run / 'run.log').write_text(p.stdout + p.stderr, encoding='utf-8')
        plog = (run / 'pcsx2.log').read_text(errors='replace') if (run / 'pcsx2.log').exists() else ''
        if 'could not get the PCSX2 lock' in p.stdout + p.stderr or 'Failed to create swap chain' in plog or (p.returncode == 1 and not boot.exists()):
            print(f'  emulator did not start (attempt {attempt + 1}), again', flush=True)
            continue
        break
    text = boot.read_text(errors='replace') if boot.exists() else ''
    keys = [k for k in a.show.split(',') if k]
    shown = 0
    for line in text.splitlines():
        if any(k in line for k in keys):
            print(line)
            shown += 1
            if shown > 400:
                print('... (more lines in', boot, ')')
                break
    ok = a.until in text
    print(f'{a.name}: rc={p.returncode} until={"yes" if ok else "NO"} lines={len(text.splitlines())}')
    return 0 if ok else 4


if __name__ == '__main__':
    sys.exit(main())
