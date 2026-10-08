"""OPT10-HF: one PS2 engine run in PCSX2 for picture checks (-vidshot) with an optional extra config.

usage: python3 tools/ps2/hf_run.py NAME [--elf build/out/SRB2.ELF] [--out build/runs] [--timeout 600] [--cfg 'chasecam "Off"'] [--until TEXT] -- engine args
Same staging as opt_run.py (packs, reference.cfg, ps2args one argument per line: PCSX2 passes at most 16 arguments), but the config can be extended
(opt_run.stage(cfg_extra)); stops at 'VIDSHOT COMPLETE' / 'ZQUIT DONE' / --until in boot.txt. The pictures are <out>/NAME/.srb2/vidshot-*.ppm or <out>/NAME/vidshot-*.ppm.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import opt_run  # noqa: E402

PAK = ROOT / 'build/pak'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('--elf', default=str(ROOT / 'build/out/SRB2.ELF'))
    ap.add_argument('--out', default=str(ROOT / 'build/runs'))
    ap.add_argument('--timeout', type=float, default=600)
    ap.add_argument('--cfg', default='', help='extra lines for reference.cfg, ";" separates lines')
    ap.add_argument('--until', default='')
    ap.add_argument('--demo', default='', help='attract demo (DEMO_001..4): staged and played with -timedemo')
    ap.add_argument('--pak', default=str(PAK))
    ap.add_argument('--files', default='', help='add-ons (paths, comma separated): copied next to the ELF and loaded with -file')
    ap.add_argument('--tree', default='', help='a directory copied into the run directory (host:/ of the PS2: models.dat, models/*.md3 ...)')
    ap.add_argument('--emu', default='', help='AppRun of another emulator copy (e.g. /opt/pcsx2/hwgl: PCSX2 with the OpenGL hardware renderer)')
    ap.add_argument('extra', nargs='*')
    a = ap.parse_args()
    run = Path(a.out).resolve() / a.name
    cfg = ''.join(x.strip() + '\n' for x in a.cfg.split(';') if x.strip())
    opt_run.stage(run, Path(a.elf).resolve(), Path(a.pak).resolve(), a.demo or None, cfg)
    import shutil
    if a.tree:
        shutil.copytree(a.tree, run, dirs_exist_ok=True)
    fargs = []
    for f in [x for x in a.files.split(',') if x]:
        shutil.copy2(f, run / Path(f).name)
        fargs += ['-file', Path(f).name]
    args = ['-logfile', 'boot.txt', '-config', 'reference.cfg', '-nolog', '-noendtxt'] + (['-timedemo', a.demo + '.lmp'] if a.demo else []) + fargs + a.extra
    until = a.until
    if not until:
        until = 'VIDSHOT COMPLETE' if '-vidshot' in a.extra else ('ZQUIT DONE' if any(x in ('-zquit', '-zquitall') for x in a.extra) else '')
    (run / 'ps2args').write_text('\n'.join(args[2:]) + '\n')
    cmd = [sys.executable, str(ROOT / 'tools/ps2/run_pcsx2.py'), '--elf', str(run / 'SRB2.ELF'), '--log', str(run / 'pcsx2.log'),
           '--args=' + ' '.join(args[:2]), '--timeout', str(a.timeout)]
    if until:
        cmd += ['--until-file', str(run / 'boot.txt'), '--until', until]
    env = dict(os.environ, SRB2_PCSX2=a.emu or opt_run.PCSX2[32])
    p = subprocess.run(cmd, env=env, capture_output=True, text=True)
    (run / 'run.log').write_text(p.stdout + p.stderr, encoding='utf-8')
    boot = run / 'boot.txt'
    text = boot.read_text(errors='replace') if boot.exists() else ''
    errs = [l for l in text.splitlines() if 'I_Error' in l or 'OOM:' in l or 'Out of memory' in l or 'HEAP CHECK FAILED' in l or 'FATAL' in l]
    shots = sorted(list(run.glob('vidshot-*.ppm')) + list((run / '.srb2').glob('vidshot-*.ppm')))
    print(f"{a.name}: rc={p.returncode} until={'ok' if (not until or until in text) else 'MISSING'} shots={len(shots)} errors={len(errs)}")
    for e in errs[:5]:
        print('  ERR', e)
    return 0 if (not errs and (not until or until in text)) else 1


if __name__ == '__main__':
    sys.exit(main())
