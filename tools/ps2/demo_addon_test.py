"""Record a demo with add-ons loaded, play it back, compare the player state printed by Lua (OPT10-X: "demos with add-ons").

usage: python3 tools/ps2/demo_addon_test.py --elf build/out/SRB2.ELF [--name demo1] [--addons ZS.pk3,ZP.pk3] [--tics 1100]
Run 1 (record): -file <addons> -skipintro -warp 1 -record zd1 with a walking/jumping pad script; the console command stopdemo (-netcmd) closes the file.
Run 2 (play back): the recorded .lmp is put into .srb2/ of a fresh run directory, -file <addons> -playdemo zd1.
ZP.pk3 (make_addons.py pos) prints "FTLUA pos <leveltime> x y z rings score skin rnd" every 35 level tics; the lines of both runs must be identical for every
leveltime both printed (position, momentum-derived state, rings, score, skin and the random-number state all agree), and the recording must be at least
--min-lines long. Exit 0 = identical.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def pos_lines(path):
    out = {}
    for m in re.finditer(r'^FTLUA pos (\d+) (.*)$', Path(path).read_text(errors='replace'), re.M):
        out[int(m.group(1))] = m.group(2)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', default='build/out/SRB2.ELF')
    ap.add_argument('--name', default='demo1')
    ap.add_argument('--addons', default='ZS.pk3,ZP.pk3')
    ap.add_argument('--addon-dir', default='build/opt10-x/addons')
    ap.add_argument('--tics', type=int, default=1100)
    ap.add_argument('--min-lines', type=int, default=10)
    ap.add_argument('--pak', default='build/pakx')
    ap.add_argument('--out', default='build/runs')
    a = ap.parse_args()
    names = [x for x in a.addons.split(',') if x]
    run1, run2 = ROOT / a.out / (a.name + '-rec'), ROOT / a.out / (a.name + '-play')
    work = ROOT / 'build/opt10-x/demo-test'
    work.mkdir(parents=True, exist_ok=True)
    pad = subprocess.run([sys.executable, str(ROOT / 'tools/ps2/gen_padscript.py'), '1', '60', str(a.tics + 100), '--seed', '5'], capture_output=True, text=True).stdout.strip()
    (work / 'pad.txt').write_text(pad)
    (work / 'cmd.txt').write_text(f'{a.tics}:stopdemo')
    files = [str(work / 'pad.txt'), str(work / 'cmd.txt')] + [str(ROOT / a.addon_dir / n) for n in names]
    base = [sys.executable, str(ROOT / 'tools/ps2/ftest_run.py'), '--elf', a.elf, '--pak', a.pak, '--out', str(ROOT / a.out)]
    r = subprocess.run(base + ['--name', a.name + '-rec', '--files', ','.join(files), '--timeout', '600', '--until', f'NETCMD frame {a.tics}', '--show', 'FTLUA pos,Added file,rror,demo',
                               '--', '-skipintro', '-warp', '1', '-file'] + names + ['-record', 'zd1', '-padscript', 'file:pad.txt', '-netcmd', 'file:cmd.txt'],
                       capture_output=True, text=True)
    print(r.stdout[-1500:])
    lmp = run1 / '.srb2' / 'zd1.lmp'
    if not lmp.exists():
        print('FAIL: no demo file', lmp)
        return 2
    print('demo', lmp, lmp.stat().st_size, 'bytes')
    files2 = [f'{lmp}=.srb2/zd1.lmp'] + [str(ROOT / a.addon_dir / n) for n in names]
    r = subprocess.run(base + ['--name', a.name + '-play', '--files', ','.join(files2), '--timeout', '600', '--until', 'FTLUA pos ' + str((a.tics // 35 - 2) * 35),
                               '--show', 'FTLUA pos,Added file,rror,demo,Demo', '--', '-file'] + names + ['-playdemo', 'zd1'],
                       capture_output=True, text=True)
    print(r.stdout[-1500:])
    A, B = pos_lines(run1 / 'boot.txt'), pos_lines(run2 / 'boot.txt')
    common = sorted(set(A) & set(B))
    bad = [(t, A[t], B[t]) for t in common if A[t] != B[t]]
    print(f'record: {len(A)} lines, playback: {len(B)} lines, common {len(common)} (leveltime {common[0] if common else "-"}..{common[-1] if common else "-"}), different {len(bad)}')
    for t, x, y in bad[:5]:
        print('  DIFF', t, '\n    rec :', x, '\n    play:', y)
    ok = len(common) >= a.min_lines and not bad
    print('OK' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
