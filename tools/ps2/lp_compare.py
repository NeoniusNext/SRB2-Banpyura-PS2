"""Compare the load-profiler slots (-loadprof, "LP <tag> <slot> <cycles> <count> |<text>") of two engine logs: "stage - cycles - share".

usage: lp_compare.py BASE_boot.txt CUR_boot.txt [--prefix B_,W_,LV_,U_,R_,LUA_] [--share-of B_ | LV_TOTAL | W_INITFILE ...] [--sum B_]
Prints, per slot present in either log: cycles (M) before and now, ratio, share of the chosen total in the "now" column, and the call counts.
--sum PREFIX adds the sum of the slots whose name starts with PREFIX (the boot laps B_* are disjoint: their sum is main() to the first frame).
Cycles are COP0 Count (EE core clock 294 912 000 Hz: 294 912 cycles per ms of EE time).
"""
import argparse
import re
import sys

CPMS = 294912.0


def read(path):
    d = {}
    desc = {}
    for line in open(path, errors='replace'):
        m = re.match(r'LP (\S+) (\S+) (\d+) (\d+) \|(.*)', line)
        if not m:
            continue
        tag, slot, cyc, cnt, text = m.groups()
        d[slot] = (int(cyc), int(cnt))
        desc[slot] = text.strip()
    return d, desc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('base')
    ap.add_argument('cur')
    ap.add_argument('--prefix', default='')
    ap.add_argument('--sum', default='')
    ap.add_argument('--share-of', default='')
    ap.add_argument('--md', action='store_true', help='markdown table: | stage | base M | now M | ratio | share of now % |')
    ap.add_argument('--skip-zero', action='store_true', help='leave out the slots that are zero in both logs')
    a = ap.parse_args()
    b, db = read(a.base)
    c, dc = read(a.cur)
    desc = dict(db)
    desc.update(dc)
    prefixes = [p for p in a.prefix.split(',') if p]
    slots = []
    for s in list(c) + [x for x in b if x not in c]:
        if s in ('ee_count_at_main',):
            continue
        if prefixes and not any(s.startswith(p) for p in prefixes):
            continue
        slots.append(s)
    total = 0
    if a.share_of:
        if a.share_of in c:
            total = c[a.share_of][0]
        else:
            total = sum(v[0] for k, v in c.items() if k.startswith(a.share_of))
    if a.md:
        print('| stage | base M | now M | ratio | share now |')
        print('|---|---:|---:|---:|---:|')
    else:
        print('%-14s %10s %10s %7s %7s %9s  %s' % ('slot', 'base M', 'now M', 'ratio', 'share%', 'count', 'what'))
    for s in slots:
        bc, bn = b.get(s, (0, 0))
        cc, cn = c.get(s, (0, 0))
        if a.skip_zero and not bc and not cc:
            continue
        ratio = ('%.2fx' % (bc / cc)) if bc and cc else '-'
        share = ('%.1f %%' % (100.0 * cc / total)) if total else ''
        if a.md:
            print('| %s (`%s`) | %.1f | %.1f | %s | %s |' % (desc.get(s, '').strip(), s, bc / 1e6, cc / 1e6, ratio, share))
        else:
            print('%-14s %10.1f %10.1f %7s %7s %4d/%-4d  %s' % (s, bc / 1e6, cc / 1e6, ratio, share, bn, cn, desc.get(s, '')))
    if a.sum:
        sb = sum(v[0] for k, v in b.items() if k.startswith(a.sum))
        sc = sum(v[0] for k, v in c.items() if k.startswith(a.sum))
        print('SUM %s*: base %.1f M (%.0f ms)  now %.1f M (%.0f ms)  %.2fx' % (a.sum, sb / 1e6, sb / CPMS, sc / 1e6, sc / CPMS, sb / sc if sc else 0))


if __name__ == '__main__':
    sys.exit(main())
