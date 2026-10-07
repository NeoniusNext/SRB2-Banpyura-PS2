"""OPT11-STAB: damaged config / game data / save files: the game must start (with defaults) or stop with a message, never hang.

usage: stab_files.py --elf SRB2.ELF --tag NAME [--only cfg-garbage,gamedata-trunc] [--timeout 300] [--extra "-renderer Hardware"]
A normal run first makes the real files (<home>/.srb2/gamedata.dat); every variant then starts `-skipintro -warp MAP01 -zquit 90` with one file replaced
(opt_run.py --home-file SRC=DST). Verdict per variant: works (ZQUIT DONE), stopped with a message, or HANG.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PAK = '/home/user/SRB2-Banpyura-PS2/build/pak'
D = ROOT / 'build/stabfiles'


def blob(name, data):
    D.mkdir(parents=True, exist_ok=True)
    (D / name).write_bytes(data)
    return str(D / name)


def run(a, name, files, extra=(), quit_arg=('-zquit', '90')):
    cmd = [sys.executable, '-B', str(ROOT / 'tools/ps2/opt_run.py'), '--name', f'{a.tag}-{name}', '--elf', a.elf, '--pak', PAK, '--out', str(ROOT / 'build/runs'),
           '--timeout', str(a.timeout), '--until', 'end of logstream']
    for src, dst in files:
        cmd += ['--home-file', f'{src}={dst}']
    cmd += ['--', '-skipintro'] + ([] if quit_arg[0] == '-zquitall' else ['-warp', 'MAP01']) + list(quit_arg) + ['-zck'] + list(extra) + a.extra.split()
    subprocess.run(cmd, capture_output=True, text=True)
    d = ROOT / 'build/runs' / f'{a.tag}-{name}'
    text = (d / 'boot.txt').read_text(errors='replace') if (d / 'boot.txt').exists() else ''
    done = 'ZQUIT DONE' in text
    ended = 'end of logstream' in text
    err = [l.strip()[:200] for l in text.splitlines() if 'I_Error' in l or 'ERROR' in l or 'WARNING' in l and ('config' in l.lower() or 'game data' in l.lower() or 'save' in l.lower())]
    verdict = 'works' if done else ('stopped with a message' if ended and err else ('ended without a message' if ended else 'HANG (no end of log)'))
    print(f'{name:22} {verdict:24} {err[:2]}', flush=True)
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--tag', required=True)
    ap.add_argument('--only', default='')
    ap.add_argument('--timeout', type=int, default=300)
    ap.add_argument('--extra', default='')
    a = ap.parse_args()
    base = run(a, 'base', [])
    gd = base / '.srb2' / 'gamedata.dat'
    good_gd = gd.read_bytes() if gd.exists() else b''
    print(f'gamedata.dat of the base run: {len(good_gd)} bytes')
    rnd = os.urandom(5000)
    cfg_base = (ROOT / 'build/runs' / f'{a.tag}-base' / '.srb2' / 'reference.cfg').read_bytes()
    V = {
        'cfg-garbage': [(blob('cfg-garbage', rnd), 'reference.cfg')],
        'cfg-empty': [(blob('cfg-empty', b''), 'reference.cfg')],
        'cfg-longline': [(blob('cfg-longline', b'name "' + b'A' * 200000 + b'"\n' + cfg_base), 'reference.cfg')],
        'cfg-badvalues': [(blob('cfg-badvalues', b'renderer "Banana"\nfpscap "99999999999"\nvid_mode "-5"\nresolution "99999 99999"\ngamma "NaN"\nname ""\nskin "nosuchskin"\ncolor "-3"\nvid_wait "x"\n' + cfg_base), 'reference.cfg')],
        'cfg-binarytail': [(blob('cfg-binarytail', cfg_base + rnd), 'reference.cfg')],
        'gamedata-garbage': [(blob('gd-garbage', rnd), 'gamedata.dat')],
        'gamedata-empty': [(blob('gd-empty', b''), 'gamedata.dat')],
        'gamedata-trunc': [(blob('gd-trunc', good_gd[: max(8, len(good_gd) // 2)]), 'gamedata.dat')],
        'gamedata-flip': [(blob('gd-flip', good_gd[:20] + bytes((b ^ 0x5A) for b in good_gd[20:200]) + good_gd[200:]), 'gamedata.dat')],
        'gamedata-huge': [(blob('gd-huge', good_gd[:16] + b'\xff' * 4096), 'gamedata.dat')],
        'save-garbage': [(blob('sv-garbage', rnd), 'srb2sav1.ssg'), (blob('sv-garbage2', rnd[::-1]), 'srb2sav2.ssg')],
        'save-empty': [(blob('sv-empty', b''), 'srb2sav1.ssg')],
    }
    names = [n for n in (a.only.split(',') if a.only else V) if n]
    sys.path.insert(0, str(ROOT / 'tools/ps2'))
    import padseq
    pad = blob('pad-saves.txt', padseq.script(['120:start', '200:cross', '320:cross', '440:cross', '560:cross', '700:circle*3/40']).encode())
    for n in names:
        if n.startswith('save'):
            # the title screen -> the menu -> 1 Player -> the save slots (the files are read and listed) -> a slot is chosen
            run(a, n, V[n] + [(pad, '../pad.txt')], ['-padscript', 'file:pad.txt', '-vidshot', 'f450,f650'], ('-zquitall', '900'))
        else:
            run(a, n, V[n])
    return 0


if __name__ == '__main__':
    sys.exit(main())
