"""Frame-by-frame comparison of two host-profile builds (MSVC, native PS2_PROFILE engine) on the four golden demos.

usage: python tools/ps2/opt2c_host_hash.py --base BASE.exe --cand CAND.exe --out DIR [--demos DEMO_001,...]
Both executables replay the recorded golden command (controlled clock, -timedemo) with `-ps2ref-hashall`; every rendered
level frame (about 1050 per demo) is hashed (src/ps2ref.c: allhash.csv) and the two runs are compared with
tools/ps2/opt2c_cmp.py (tics.csv byte equality, number of differing frames, golden frames pixel counts).
Host runs take seconds, so this is the cheap exhaustive check; the EE runs (run_ps2_ref.py, run_hash.sh) confirm it on the target.
The host-profile executables come from tools/ps2/build_host_profile.ps1 (-ExtraCFlags '-DPS2_NOOPT_SLOPE -DPS2_NOOPT_SEGS' keeps
the bit-exact comparison apart from the approximating slope code).
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GOLDEN = ROOT / 'golden/phase0-v2/run1'
PACKS = ROOT / 'build/pak-a'
DEPS = Path('D:/AI-projects/SRB2B-plus/build/deps/vcpkg_installed/x64-windows/bin')


def run(exe, demo, out, every1=False):
    ref = GOLDEN / demo
    out.mkdir(parents=True, exist_ok=True)
    home = out / 'home/srb2'
    home.mkdir(parents=True, exist_ok=True)
    (home / 'reference.cfg').write_bytes((ref / 'home/srb2/reference.cfg').read_bytes())
    (home / (demo + '.lmp')).write_bytes((GOLDEN.parent / (demo + '.lmp')).read_bytes())
    cmd = json.loads((ref / 'command.json').read_text())
    cmd[0] = str(exe)
    cmd[cmd.index('-ps2ref') + 1] = str(out)
    cmd[cmd.index('-home') + 1] = str(out / 'home')
    cmd.append('-ps2ref-hashall')
    if every1:
        cmd += ['-ps2ref-every', '1']
    env = dict(os.environ, SRB2WADDIR=str(PACKS))
    env['PATH'] = str(DEPS) + os.pathsep + env.get('PATH', '')
    with (out / 'stdout.log').open('wb') as log:
        rc = subprocess.run(cmd, cwd=PACKS, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=600).returncode
    return rc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', type=Path, required=True)
    ap.add_argument('--cand', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--demos', default='DEMO_001,DEMO_002,DEMO_003,DEMO_004')
    ap.add_argument('--pixels', action='store_true', help='dump every frame (-ps2ref-every 1) and compare pixel by pixel')
    a = ap.parse_args()
    rc_all = 0
    for demo in a.demos.split(','):
        print('==', demo)
        for tag, exe in (('base', a.base), ('cand', a.cand)):
            rc = run(exe.resolve(), demo, a.out.resolve() / tag / demo, a.pixels)
            if rc:
                print('  run failed', tag, rc)
                rc_all = 1
        rc = subprocess.run([sys.executable, str(ROOT / 'tools/ps2/opt2c_cmp.py'), str(a.out.resolve() / 'base' / demo), str(a.out.resolve() / 'cand' / demo),
                             '--golden', str(GOLDEN / demo)] + (['--pixels'] if a.pixels else [])).returncode
        rc_all |= rc
    return rc_all


if __name__ == '__main__':
    sys.exit(main())
