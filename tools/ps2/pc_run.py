"""Run the PC reference game (build/pc-net: this source tree, Linux, own Xvfb) with add-ons and collect its FTLUA lines (OPT9-F, ported to Linux in OPT10-X).

usage: pc_run.py --name NAME [--files a.pk3,b.wad,...] [--warp 1] [--until 'FTLUA map'] [--timeout 120] [--out build/opt10-x/pc] [-- extra engine args]
Files are given as paths (relative to the repo root or absolute); they are passed with -file in this order (the last one is loaded last).
Writes <out>/<name>/pc.out (stdout) and ftlua.txt (the FTLUA lines, "FTLUA " prefix kept), kills the game when --until appears or the timeout is over.
"""
import argparse
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import net_env  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--name', required=True)
    ap.add_argument('--files', default='')
    ap.add_argument('--warp', default='1')
    ap.add_argument('--until', default='FTLUA things')
    ap.add_argument('--timeout', type=float, default=120)
    ap.add_argument('--out', default=str(ROOT / 'build/opt10-x/pc'))
    ap.add_argument('--exe', default=str(ROOT / net_env.pc_exe()), help='PC reference: default the PC build of this source tree (build/pc-net, tools/ps2/net_env.py); srb2-assets/srb2win.exe is another version')
    ap.add_argument('--nomusic', action='store_true')
    ap.add_argument('extra', nargs='*')
    a = ap.parse_args()
    o = Path(a.out).resolve() / a.name
    (o / 'home' / '.srb2').mkdir(parents=True, exist_ok=True)
    a.exe = str(Path(a.exe) if Path(a.exe).is_absolute() else ROOT / a.exe)
    args = [a.exe, '-home', str(o / 'home'), '-skipintro'] + (['-warp', a.warp] if a.warp else []) + ['-nosound']  # -nosound: no sound device; the music still goes through S_ChangeMusic (an error for a missing lump is printed either way)
    if a.nomusic:
        args.append('-nomusic')
    for f in [x for x in a.files.split(',') if x]:
        p = Path(f)
        if not p.is_absolute():
            p = ROOT / p
        args += ['-file', str(p)]
    args += a.extra
    outf = open(o / 'pc.out', 'wb')
    errf = open(o / 'pc.err', 'wb')
    env = dict(os.environ, SRB2WADDIR=os.environ.get('SRB2WADDIR', '/opt/srb2-assets'), SDL_AUDIODRIVER='dummy', LIBGL_ALWAYS_SOFTWARE='1')
    args = ['xvfb-run', '-a', '-s', '-screen 0 800x600x24'] + args
    p = subprocess.Popen(args, cwd=str(o), stdout=outf, stderr=subprocess.STDOUT, env=env, start_new_session=True)
    t0 = time.time()
    state = 'timeout'
    while time.time() - t0 < a.timeout:
        if p.poll() is not None:
            state = 'exit %d' % p.returncode
            break
        if a.until and a.until in (o / 'pc.out').read_text(errors='replace'):
            state = 'until'
            time.sleep(1.0)
            break
        time.sleep(1.0)
    if p.poll() is None:
        os.killpg(os.getpgid(p.pid), signal.SIGINT)
        time.sleep(2)
    try:
        os.killpg(os.getpgid(p.pid), signal.SIGKILL)
    except OSError:
        pass
    outf.close()
    errf.close()
    text = (o / 'pc.out').read_text(errors='replace')
    lines = [l.rstrip() for l in text.splitlines() if l.startswith('FTLUA ')]
    (o / 'ftlua.txt').write_text('\n'.join(lines) + '\n')
    errs = [l.rstrip() for l in text.splitlines() if any(k in l for k in ('Lua error', 'lua error', 'WARNING', 'ERROR', 'error:', 'Error'))]
    (o / 'errors.txt').write_text('\n'.join(errs) + '\n')
    print(f'{a.name}: {state} after {time.time() - t0:.0f}s, {len(lines)} FTLUA lines, {len(errs)} warning/error lines')
    return 0


if __name__ == '__main__':
    sys.exit(main())
