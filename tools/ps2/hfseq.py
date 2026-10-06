"""OPT10-HF: panels for a sequence of shots (demo frames): PC OpenGL shot-N.png (pcshot.py) against the PS2 vidshot files of one run.

usage: hfseq.py --pc build/ref/dm1_pc --hw build/runs/dm1_hw --tags l100,l200,... --out build/panels/dm1 [--scale 1] [--sw build/runs/other]
Shot n of the PC run = n-th tag. The HW pictures are vidshot-<w>x<h>-<tag>.ppm. Prints one metrics line per frame and writes <out>_<tag>.png panels and a summary JSON.
"""
import argparse
import glob
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pc', required=True)
    ap.add_argument('--hw', required=True)
    ap.add_argument('--sw', default='')
    ap.add_argument('--tags', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--scale', default='1')
    a = ap.parse_args()
    res = {}
    for i, tag in enumerate(a.tags.split(',')):
        pc = Path(a.pc) / f'shot-{i}.png'
        hw = glob.glob(str(Path(a.hw) / f'vidshot-*-{tag}.ppm')) or glob.glob(str(Path(a.hw) / f'vidshot-*-{tag}_*.ppm'))
        if not pc.exists() or not hw:
            print(tag, 'missing', 'pc' if not pc.exists() else '', 'hw' if not hw else '')
            continue
        cmd = [sys.executable, str(ROOT / 'tools/ps2/hfpanel.py'), '--pc', str(pc), '--hw', hw[0], '--out', f'{a.out}_{tag}.png', '--label', tag, '--scale', a.scale, '--json']
        if a.sw:
            sw = glob.glob(str(Path(a.sw) / f'vidshot-*-{tag}.ppm'))
            if sw:
                cmd += ['--sw', sw[0]]
        out = subprocess.run(cmd, capture_output=True, text=True).stdout.strip().splitlines()
        try:
            res[tag] = json.loads(out[-1])
        except Exception:
            res[tag] = {'error': ' '.join(out)[-200:]}
        print(tag, res[tag])
    Path(a.out + '.json').write_text(json.dumps(res, indent=1))


if __name__ == '__main__':
    main()
