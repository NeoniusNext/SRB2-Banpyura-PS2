"""OPT14: compare the LQ lines of the nodes of a network run made by net_session.py with a scenario of net_specs14.py.

usage: lua_net.py <run dir> [--ref srv] [--logs srv=srv/out.txt pc=pc/out.txt cli=cli/boot.txt]
The lines "LQ tic N ..." and "LQ ref N ..." (printed by lm_net.lua every 100 tics of the mod's own counter, which a joining client takes over through NetVars) are compared by N between the reference node
(the PC server) and every other node, for the N both have; the events (cv, cmd, chat) are compared as sequences from the first one the later node saw. Exit code 0: nothing differs.
"""
import argparse
import re
import sys
from pathlib import Path


def lq_lines(path):
    out = []
    for l in Path(path).read_text(errors='replace').splitlines():
        i = l.find('LQ ')
        if i >= 0 and (i == 0 or not l[i - 1].isalnum()):
            out.append(l[i + 3:].rstrip())
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--ref', default='srv')
    ap.add_argument('--logs', nargs='*', default=['srv=srv/out.txt', 'pc=pc/out.txt', 'cli=cli/boot.txt'])
    a = ap.parse_args()
    run = Path(a.run)
    logs = {}
    for item in a.logs:
        name, rel = item.split('=', 1)
        if (run / rel).exists():
            logs[name] = lq_lines(run / rel)
    ref = logs[a.ref]

    def keyed(lines):
        d = {}
        for l in lines:
            m = re.match(r'(tic|ref) (\d+) (.*)$', l)
            if m:
                d[(m.group(1), int(m.group(2)))] = m.group(3)
        return d

    def events(lines):
        return [l for l in lines if re.match(r'(cv|cmd|chat|join) ', l)]

    rk = keyed(ref)
    bad = 0
    for name, lines in logs.items():
        print(f'{name}: {len(lines)} LQ lines, {len(keyed(lines))} keyed')
        if name == a.ref:
            continue
        k = keyed(lines)
        common = sorted(set(k) & set(rk))
        diffs = [(key, rk[key], k[key]) for key in common if rk[key] != k[key]]
        print(f'  {name} against {a.ref}: {len(common)} common keyed lines, {len(diffs)} differ')
        for key, x, y in diffs[:12]:
            print(f'    {key}\n      {a.ref}: {x}\n      {name}: {y}')
        bad += len(diffs)
        if not common:
            print('  NO COMMON LINES: the node never joined the mod state')
            bad += 1
        re_, ev = events(ref), events(lines)
        # the later node saw a suffix of the events
        if ev:
            first = ev[0]
            try:
                at = re_.index(first, 0)
            except ValueError:
                at = -1
            tail = re_[at:] if at >= 0 else []
            print(f'  {name} events: {ev}')
            if ev != tail[:len(ev)] and not (a.ref == 'srv' and name == 'pc' and ev == re_):
                print(f'  EVENTS DIFFER: {a.ref} has {tail[:len(ev)]}')
                bad += 1
    print('RESULT', 'SAME' if not bad else 'DIFFERENT')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
