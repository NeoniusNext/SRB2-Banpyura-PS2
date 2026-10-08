"""OPT11-FX3: the difference of two sampler runs (build.py --sample, -ps2sample) per function: what the drawing of the sprites costs (run A all on, run B with -hwfx 8192, no sprite drawing).

usage: fx_sampdiff.py ELF A/boot.txt B/boot.txt [--top 40]    (kcycles per frame, A - B, with the self time of the function in A and B)
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def table(elf, log):
    out = subprocess.run([sys.executable, str(ROOT / 'tools/ps2/sample_report.py'), '--elf', str(elf), '--log', str(log), '--top', '400', '--lines', '6000'], capture_output=True, text=True,
                         env={'PATH': '/opt/ps2dev-x/ps2dev/ee/bin:/usr/bin:/bin'}).stdout
    res = {}
    lines = {}
    sec = 0
    for l in out.splitlines():
        if l.startswith('function (self)'):
            sec = 1
            continue
        if l.startswith('category'):
            sec = 2
            continue
        if l.startswith('source line'):
            sec = 3
            continue
        if sec == 1:
            m = re.match(r'(\S+)\s+(\d+)\s+([\d.]+)\s+([\d.]+)', l)
            if m:
                res[m.group(1)] = float(m.group(3))
        elif sec == 3:
            m = re.match(r'(.+?)(?: \(discriminator \d+\))?\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s*$', l)
            if m:
                lines[m.group(1)] = lines.get(m.group(1), 0.0) + float(m.group(3))
    return res, lines


def main():
    elf, a, b = sys.argv[1:4]
    top = 40
    if '--top' in sys.argv:
        top = int(sys.argv[sys.argv.index('--top') + 1])
    (ta, la), (tb, lb) = table(elf, a), table(elf, b)
    rows = []
    for k in set(ta) | set(tb):
        rows.append((ta.get(k, 0.0) - tb.get(k, 0.0), ta.get(k, 0.0), tb.get(k, 0.0), k))
    rows.sort(reverse=True)
    tot = sum(r[0] for r in rows)
    print('total difference %.1f kcyc/frame' % tot)
    for d, x, y, k in rows[:top]:
        print('%-44s %8.1f  (A %8.1f  B %8.1f)' % (k, d, x, y))
    if '--lines' in sys.argv:
        nl = int(sys.argv[sys.argv.index('--lines') + 1])
        lr = sorted(((la.get(k, 0.0) - lb.get(k, 0.0), k) for k in set(la) | set(lb)), reverse=True)
        print('--- source lines (A - B, kcyc/frame)')
        for d, k in lr[:nl]:
            print('%-60s %8.1f' % (k[-60:], d))


main()
