"""Timedemo run of the PROFILER-ONLY build (build.py --prof: per-phase COP0 profile, no PS2REF hooks) in PCSX2.

usage: SRB2_PS2_RUN=<dir> python tools/ps2/opt2c_prof_run.py --demo DEMO_001 [--no-build] [--windows 10] [--timeout 900] [-- engine args]
The PS2REF build used by run_ps2_ref.py serialises the whole game state every tick (tics.csv) and hashes frames; that
shows up as 0.7-1.4 M cycles per tick in the "gtick" phase. This run uses the production code path instead: no golden
comparison (use run_ps2_ref.py / the hash runs for equivalence), only the PROF lines in <dir>/boot.txt.
Finishes when the engine has printed the last PROF window (windows of 105 frames; 10 windows = the 1050 tics of a demo).
Exit code 0 when the marker was seen.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/ps2'))
import run_ps2_ref as R  # noqa: E402  (reads SRB2_PS2_RUN)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--demo', default='DEMO_001')
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--subprof', action='store_true', help='build with --subprof (the PS2SUB probes)')
    ap.add_argument('--windows', type=int, default=10)
    ap.add_argument('--timeout', type=float, default=900)
    ap.add_argument('extra', nargs='*')
    a = ap.parse_args()
    R.RUN.mkdir(parents=True, exist_ok=True)
    R.prepare(a.demo)
    if not a.no_build:
        env = dict(os.environ, SRB2_PS2_OUT=str(R.BUILD))
        if subprocess.run([sys.executable, str(ROOT / 'tools/ps2/build.py'), '--subprof' if a.subprof else '--prof'], env=env).returncode:
            raise SystemExit('build failed')
        shutil.copy2(R.BUILD / 'SRB2.ELF', R.RUN / 'SRB2.ELF')
    boot = R.RUN / 'boot.txt'
    args = ['-logfile', 'boot.txt', '-config', 'reference.cfg', '-nolog', '-noendtxt', '-timedemo', a.demo + '.lmp', '-ps2prof'] + a.extra
    cmd = [sys.executable, str(ROOT / 'tools/ps2/run_pcsx2.py'), '--elf', str(R.RUN / 'SRB2.ELF'), '--log', str(R.RUN / 'pcsx2.log'),
           '--args=' + ' '.join(args), '--timeout', str(a.timeout), '--until-file', str(boot), '--until', 'PROF win=%d ' % (a.windows - 1)]
    print(' '.join(cmd))
    rc = subprocess.run(cmd).returncode
    txt = boot.read_text(errors='replace') if boot.exists() else ''
    ok = ('PROF win=%d ' % (a.windows - 1)) in txt
    print('finished:', ok, 'wrapper rc', rc)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
