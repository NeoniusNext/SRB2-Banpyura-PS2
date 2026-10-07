"""Load the same add-ons in the PC build of this source tree and in the PS2 port (PCSX2) and compare what the game looks like afterwards (OPT9-F).

usage: addon_compare.py --name NAME --files a.pk3,b.wad,... [--elf ELF] [--warp 1] [--timeout 400] [--ps2-args "..."] [--pak ...]
The add-on tools/ps2/make_addons.py `sum` makes (ZSUM.pk3, build/opt10-x/addons) is appended as the LAST file: it prints FTLUA lines (skins, object types, states,
colours, sounds, level) through the Lua API, the same on both. Compared: those lines, and the WARNING/ERROR/Lua error lines the add-ons cause (what the
base game prints alone is subtracted; paths are cut to the file name).
Output: build/opt10-x/cmp/NAME.txt; exit code 0 when everything agrees.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PY = sys.executable
SUM = ROOT / 'build/opt10-x/addons/ZSUM.pk3'
PAK = str(ROOT / 'build/pakx')  # links of the cooked packs + FINEACON.DAT (python3 tools/ps2/net_env.py)


def norm(line):
    line = re.sub(r'(\.\.\.)?[^\s|:"]*[\\/](?=[^\\/\s|"]+\.(?:pk3|wad|lua|soc)\b)', '', line, flags=re.I)
    line = re.sub(r'^\s+|\s+$', '', line)
    return line


def alerts(text):
    out = []
    for l in text.splitlines():
        if re.search(r'WARNING|ERROR|Lua error|lua error|Error', l) and 'FTLUA' not in l:
            out.append(norm(l))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--name', required=True)
    ap.add_argument('--files', required=True, help='comma separated; empty = only ZSUM (the baseline of the base game: name it sum-base)')
    ap.add_argument('--elf', default=str(ROOT / 'build/out/SRB2.ELF'))
    ap.add_argument('--warp', default='1')
    ap.add_argument('--timeout', type=float, default=400)
    ap.add_argument('--ps2-args', default='')
    ap.add_argument('--pc-args', default='')
    ap.add_argument('--no-pc', action='store_true')
    ap.add_argument('--until', default='FTLUA things')
    a = ap.parse_args()
    files = [str(Path(f) if Path(f).is_absolute() else ROOT / f) for f in a.files.split(',') if f]
    names = [Path(f).name for f in files] + ['ZSUM.pk3']
    allfiles = files + [str(SUM)]
    out = ROOT / 'build/opt10-x/cmp'
    out.mkdir(parents=True, exist_ok=True)
    pcdir = ROOT / 'build/opt10-x/pc' / ('x-' + a.name)
    if not a.no_pc:
        subprocess.run([PY, str(ROOT / 'tools/ps2/pc_run.py'), '--name', 'x-' + a.name, '--files', ','.join(allfiles), '--warp', a.warp, '--timeout', '240', '--until', a.until]
                       + (['--'] + a.pc_args.split() if a.pc_args else []), check=False)
    ps2args = ['-skipintro', '-warp', a.warp] + sum([['-file', n] for n in names], []) + ['-zquit', '200'] + a.ps2_args.split()
    cmd = [PY, str(ROOT / 'tools/ps2/ftest_run.py'), '--name', 'x-' + a.name, '--out', str(ROOT / 'build/opt10-x/run-ft'), '--pak', PAK, '--elf', a.elf, '--files', ','.join(allfiles),
           '--until', a.until, '--timeout', str(a.timeout), '--'] + ps2args
    pr = subprocess.run(cmd, check=False, capture_output=True, text=True)
    (out / (a.name + '.ps2.out')).write_text(pr.stdout + pr.stderr, encoding='utf-8')
    ps2log = ROOT / 'build/opt10-x/run-ft' / ('x-' + a.name) / 'boot.txt'
    basepc = ROOT / 'build/opt10-x/pc/x-sum-base/pc.out'
    baseps2 = ROOT / 'build/opt10-x/run-ft/x-sum-base/boot.txt'
    rep = []
    ps2t = ps2log.read_text(errors='replace') if ps2log.exists() else ''
    pct = (pcdir / 'pc.out').read_text(errors='replace') if (pcdir / 'pc.out').exists() else ''
    # a 64-bit PC build prints a negative 32-bit hash as ffffffffXXXXXXXX (sign-extended %x of a Lua integer); the PS2 and a 32-bit PC print XXXXXXXX
    ft = lambda t: [re.sub(r'\bffffffff([0-9a-f]{8})\b', r'\1', l.strip()) for l in t.splitlines() if l.startswith('FTLUA ')]
    p2, pc = ft(ps2t), ft(pct)
    bad = 0
    for n in range(max(len(p2), len(pc))):
        x = p2[n] if n < len(p2) else '<missing>'
        y = pc[n] if n < len(pc) else '<missing>'
        if x != y:
            rep.append(f'DIFF line {n + 1}\n  ps2: {x}\n  pc : {y}')
            bad += 1
    rep.append(f'FTLUA: {len(p2)} lines on PS2, {len(pc)} on PC, {bad} different')
    a2 = alerts(ps2t)
    ac = alerts(pct)
    b2 = set(alerts(baseps2.read_text(errors='replace'))) if baseps2.exists() else set()
    bc = set(alerts(basepc.read_text(errors='replace'))) if basepc.exists() else set()
    d2 = [l for l in a2 if l not in b2]
    dc = [l for l in ac if l not in bc]
    only2 = [l for l in d2 if l not in dc]
    onlyc = [l for l in dc if l not in d2]
    rep.append(f'alerts caused by the add-ons: PS2 {len(d2)}, PC {len(dc)}; only on PS2 {len(only2)}, only on PC {len(onlyc)}')
    for l in only2[:25]:
        rep.append('  PS2 only: ' + l[:200])
    for l in onlyc[:25]:
        rep.append('  PC only : ' + l[:200])
    (out / (a.name + '.txt')).write_text('\n'.join(rep) + '\n')
    print('\n'.join(rep))
    return 0 if not bad and not only2 and not onlyc and p2 else 1


if __name__ == '__main__':
    sys.exit(main())
