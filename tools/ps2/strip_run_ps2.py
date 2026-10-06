"""run_ps2_ref.py against a chosen ELF and cooked-pack directory (PS2-20 strip checks).

usage: strip_run_ps2.py --run DIR --elf SRB2.ELF --pak PACKDIR --mode title|idle|demo [--demo DEMO_001] [--timeout S]
                        [--idle-seconds N] [-- engine args]
run_ps2_ref.py always copies build/pak into its run directory; this wrapper puts the packs of --pak there instead,
deletes every *.pk3 from the run directory (the profile must not need them: the engine log proves the packs are used) and
copies --elf as the ELF to run. Everything else (PCSX2 only through run_pcsx2.py, golden comparison, exit codes) is
run_ps2_ref.py's.
"""
import argparse
import os
import shutil
import sys
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument('--run', required=True)
ap.add_argument('--elf', required=True)
ap.add_argument('--pak', required=True)
ap.add_argument('--mode', choices=['title', 'idle', 'demo'], required=True)
ap.add_argument('--demo', default='DEMO_001')
ap.add_argument('--timeout', default='900')
ap.add_argument('--idle-seconds', default='600')
ap.add_argument('extra', nargs='*')
a = ap.parse_args()

os.environ['SRB2_PS2_RUN'] = str(Path(a.run).resolve())
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_ps2_ref as r  # noqa: E402

orig_prepare = r.prepare


def prepare(demo):
    out = orig_prepare(demo)
    for p in r.RUN.glob('*.PAK'):
        p.unlink()
    for pak in Path(a.pak).glob('*.PAK'):
        shutil.copy2(pak, r.RUN / pak.name)
    for pk3 in r.RUN.glob('*.pk3'):
        pk3.unlink()  # a hardlink made by run_ps2_ref: only this name is removed
    shutil.copy2(a.elf, r.RUN / 'SRB2.ELF')
    return out


r.prepare = prepare
argv = ['run_ps2_ref.py', '--mode', a.mode, '--no-build', '--timeout', a.timeout]
if a.mode == 'demo':
    argv += ['--demo', a.demo]
if a.mode == 'idle':
    argv += ['--idle-seconds', a.idle_seconds]
sys.argv = argv + (['--'] + a.extra if a.extra else [])
sys.exit(r.main())
