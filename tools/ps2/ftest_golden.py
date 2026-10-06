"""Golden check of a PS2REF build on one demo (OPT8-F): run the demo in PCSX2 and compare tics.csv, frames.csv and every frame-*.idx with golden/phase0-v2/run1/<demo>
(or with another run's refout, --compare-to), waiting as long as it takes for the machine-wide emulator lock.

usage: ftest_golden.py --name NAME --elf SRB2.ELF --demo DEMO_001 [--pak build/opt6-f/pak] [--out build/opt8-f/run] [--compare-to RUN] [--timeout 900]
The ELF must be a "build.py --ps2ref" build. Prints one line: "golden DEMO_00n: N files, K differ" (K = 0 is the pass), exit code 0 on a pass.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from opt_run import PCSX2, ROOT, stage  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--name', required=True)
    ap.add_argument('--elf', required=True)
    ap.add_argument('--demo', required=True)
    ap.add_argument('--pak', default=str(ROOT / 'build/opt6-f/pak'))
    ap.add_argument('--out', default=str(ROOT / 'build/opt8-f/run'))
    ap.add_argument('--compare-to', default='')
    ap.add_argument('--timeout', type=float, default=900)
    ap.add_argument('--lock-wait', type=float, default=7200)
    a = ap.parse_args()
    run = Path(a.out).resolve() / a.name
    refout = stage(run, Path(a.elf).resolve(), Path(a.pak).resolve(), a.demo)
    import shutil
    if (Path(a.pak) / 'FINEACON.DAT').exists():  # lazily read data file that lives next to the packs
        shutil.copy2(Path(a.pak) / 'FINEACON.DAT', run / 'FINEACON.DAT')
    boot = run / 'boot.txt'
    if boot.exists():
        boot.unlink()
    args = ['-logfile', 'boot.txt', '-config', 'reference.cfg', '-nolog', '-noendtxt', '-ps2ref', 'host:/refout', '-timedemo', a.demo + '.lmp']
    cmd = [sys.executable, str(ROOT / 'tools/ps2/run_pcsx2.py'), '--elf', str(run / 'SRB2.ELF'), '--log', str(run / 'pcsx2.log'),
           '--args=' + ' '.join(args), '--timeout', str(a.timeout), '--lock-wait', str(a.lock_wait),
           '--until-file', str(refout / 'complete.txt'), '--until', 'complete']
    env = dict(os.environ, SRB2_PCSX2=PCSX2[32])
    p = subprocess.run(cmd, env=env, capture_output=True, text=True)
    (run / 'run.log').write_text(p.stdout + p.stderr, encoding='utf-8')
    complete = (refout / 'complete.txt').is_file()
    other = (Path(a.out).resolve() / a.compare_to / 'refout') if a.compare_to else (ROOT / 'golden/phase0-v2/run1' / a.demo)
    names = sorted(x.name for x in other.glob('*') if x.name == 'tics.csv' or x.name == 'frames.csv' or x.name.startswith('frame-'))
    bad = [n for n in names if not (refout / n).exists() or (refout / n).read_bytes() != (other / n).read_bytes()]
    print(f'golden {a.demo}: {len(names)} files, {len(bad)} differ' + (f': {bad[:6]}' if bad else '') + ('' if complete else '  (run did not complete)'))
    errs = [l for l in (boot.read_text(errors='replace').splitlines() if boot.exists() else []) if 'I_Error' in l or 'OOM' in l]
    for e in errs[:4]:
        print('  ERR', e)
    return 0 if complete and names and not bad else 3


if __name__ == '__main__':
    sys.exit(main())
