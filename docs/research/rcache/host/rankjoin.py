#!/usr/bin/env python3
"""OPT13-RCACHE: join the PCSX2 PC-sampler report (true emulator cycles per function, tools/ps2/sample_report.py) with the cachegrind misses of the host profile build
(cgmodel.py --csv) into one ranking: emulator rank vs modelled console rank.
usage: rankjoin.py SAMPLER_REPORT CG_CSV [--frames 1049] [--miss 40] [--ptrfix 0.75] [--top 40]
Functions the host build does not have (audio decoder, kernel, driver) keep their emulator cost (misses unknown = 0): the console numbers of those are a LOWER bound."""
import argparse, csv, re
ap = argparse.ArgumentParser()
ap.add_argument('rep'); ap.add_argument('csv'); ap.add_argument('--frames', type=float, default=1049)
ap.add_argument('--miss', type=float, default=40); ap.add_argument('--imiss', type=float, default=-1); ap.add_argument('--ptrfix', type=float, default=0.75); ap.add_argument('--top', type=int, default=40)
a = ap.parse_args()
if a.imiss < 0: a.imiss = a.miss
norm = lambda n: re.sub(r"(\.(lto_priv|part|constprop|isra|cold)\.\d+)+$", '', n).replace("'2", '')
emu = {}
infn = False
for l in open(a.rep, errors='replace'):
    if l.startswith('function (self)'):
        infn = True; continue
    if infn and not l.strip():
        break
    m = re.match(r'^(\S.*?)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s*$', l)
    if infn and m:
        emu[norm(m.group(1).strip())] = emu.get(norm(m.group(1).strip()), 0) + float(m.group(3))
cg = {}
for r in csv.DictReader(open(a.csv)):
    n = norm(r['fn'])
    d = cg.setdefault(n, [0, 0, 0, r['stage']])
    d[0] += int(r['D1mr']) + int(r['D1mw']); d[1] += int(r['I1mr']); d[2] += int(r['Ir'])
rows = []
for n, k in emu.items():
    d = cg.get(n)
    st = d[3] if d else '-'
    pf = 1.0 if st in ('render:drawers(col/span)', 'render:hud/video') else a.ptrfix
    dm = (a.miss * d[0] * pf / a.frames / 1e3) if d else 0.0
    im = (a.imiss * d[1] / a.frames / 1e3) if d else 0.0
    rows.append((n, k, dm, im, k + dm + im, st))
tot_e = sum(r[1] for r in rows); tot_h = sum(r[4] for r in rows)
er = {r[0]: i + 1 for i, r in enumerate(sorted(rows, key=lambda r: -r[1]))}
print('sampler total %.0f kcyc/frame; with modelled misses %.0f kcyc/frame (x%.2f); functions with host data: %d of %d' % (tot_e, tot_h, tot_h / tot_e, len([r for r in rows if r[5] != '-']), len(rows)))
print('%-4s %-32s %-22s %8s %8s %8s %8s %7s %5s' % ('hw#', 'function', 'stage', 'emu K', 'Dmiss K', 'Imiss K', 'hw K', 'hw/emu', 'emu#'))
for i, r in enumerate(sorted(rows, key=lambda r: -r[4])[:a.top]):
    print('%-4d %-32s %-22s %8.1f %8.1f %8.1f %8.1f %6.2fx %5d' % (i + 1, r[0][:32], r[5], r[1], r[2], r[3], r[4], r[4] / r[1] if r[1] else 0, er[r[0]]))
