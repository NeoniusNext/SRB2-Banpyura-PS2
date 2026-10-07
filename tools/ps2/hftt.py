"""OPT10-HF: run the texture conformance self-test (-hwtextest, PS2-HW-69) on a list of maps and collect the TT lines.

usage: hftt.py --tag tt --maps 1,2,4,... [--elf build/outl/SRB2.ELF] [--emu /opt/pcsx2/hwgl/AppRun] [--extra '-zreserve 3072']
Result: build/runs/<tag>_m<N>/boot.txt, summary printed and written to build/panels/<tag>.txt.
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tag', required=True)
    ap.add_argument('--maps', required=True)
    ap.add_argument('--elf', default=str(ROOT / 'build/outl/SRB2.ELF'))
    ap.add_argument('--emu', default='')
    ap.add_argument('--timeout', default='2400')
    ap.add_argument('--extra', default='-zreserve 3072')
    a = ap.parse_args()
    out = []
    for m in a.maps.split(','):
        name = f'{a.tag}_m{m}'
        cmd = [sys.executable, str(ROOT / 'tools/ps2/hf_run.py'), name, '--elf', a.elf, '--until', 'TT done', '--timeout', a.timeout]
        if a.emu:
            cmd += ['--emu', a.emu]
        cmd += ['--', '-skipintro', '-warp', m, '-renderer', 'Hardware', '-hwfbh', '200', '-hwtextest'] + a.extra.split()
        r = subprocess.run(cmd, capture_output=True, text=True)
        boot = ROOT / 'build/runs' / name / 'boot.txt'
        lines = [l for l in boot.read_text(errors='replace').splitlines() if l.startswith('TT')] if boot.exists() else []
        out.append(f'== map {m}: {r.stdout.strip().splitlines()[0] if r.stdout.strip() else "no output"}')
        out += lines
        print(out[-len(lines) - 1] if lines else out[-1], flush=True)
        for l in lines[-1:]:
            print('  ', l, flush=True)
    (ROOT / 'build/panels' / f'{a.tag}.txt').write_text('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
