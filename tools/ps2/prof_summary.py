"""Summarise the PROF windows of engine logs: M cycles per frame per window 1..9, mean, max, FPS (294.912 M / cycles).

usage: python3 tools/ps2/prof_summary.py build/runs/<name>/boot.txt [more boot.txt ...] [--fields total,ptick,bsp,...]
With the LTO profile build only `total` is meaningful (phases need SRB2_PS2_LTO=0, see docs/GATES/g1/opt5-C.md).
"""
import re
import sys

CLOCK = 294.912e6


def windows(path):
    out = []
    for line in open(path, errors='replace'):
        m = re.search(r'PROF win=(\d+) frames=(\d+) tics=(\d+) total=(\d+)(.*)', line)
        if not m:
            continue
        w, fr, tics, tot = int(m[1]), int(m[2]), int(m[3]), int(m[4])
        ph = {}
        for k, a, b in re.findall(r'(\w+)=(\d+)/(\d+)', m[5]):
            ph[k] = int(a)
        out.append((w, fr, tics, tot, ph))
    return out


def main():
    paths = [a for a in sys.argv[1:] if not a.startswith('--')]
    rows = {}
    for p in paths:
        ws = [x for x in windows(p) if x[0] >= 1]
        if not ws:
            print(p, ': no PROF windows')
            continue
        per = [x[3] / x[1] / 1e6 for x in ws]
        mean = sum(x[3] for x in ws) / sum(x[1] for x in ws) / 1e6
        print('%-52s windows %d  mean %.2f M/frame (%.1f FPS)  max %.2f  min %.2f   [%s]' % (
            p, len(ws), mean, CLOCK / (mean * 1e6), max(per), min(per), ' '.join('%.2f' % v for v in per)))
        # phases (non-LTO builds)
        if any(v for x in ws for v in x[4].values()):
            tot = {}
            fr = sum(x[1] for x in ws)
            for x in ws:
                for k, v in x[4].items():
                    tot[k] = tot.get(k, 0) + v
            print('    phases M/frame: ' + ' '.join('%s=%.2f' % (k, v / fr / 1e6) for k, v in tot.items() if v))


if __name__ == '__main__':
    main()
