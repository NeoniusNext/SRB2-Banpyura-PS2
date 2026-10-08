"""OPT11-MODEL: mean and maximum of the HWPROF fields over the windows 1..9 of one or more runs (M cycles per frame), plus the model counters (HWPROF40).

usage: md_wall.py build/runs/NAME/boot.txt [...]  [--fields wall,sprites,bsp]
"""
import re
import sys

def main():
    paths = [a for a in sys.argv[1:] if not a.startswith('--')]
    fields = ['wall', 'sprites', 'bsp', 'batch']
    for i, a in enumerate(sys.argv):
        if a == '--fields':
            fields = sys.argv[i + 1].split(',')
            paths = [p for p in paths if p != sys.argv[i + 1]]
    for p in paths:
        rows, md = {}, []
        for line in open(p, errors='replace'):
            m = re.match(r'HWPROF win=(\d+) (.*)', line)
            if m and 1 <= int(m[1]) <= 9:
                d = dict((k, int(v)) for k, v in re.findall(r'(\w+)=(\d+)', m[2].split('|')[0]))
                for f in fields:
                    rows.setdefault(f, []).append(d.get(f, 0))
            m = re.match(r'HWPROF40 models: (.*)', line)
            if m:
                md.append(dict((k, int(v)) for k, v in re.findall(r'(\w+)=(\d+)', m[1])))
        out = []
        for f in fields:
            v = rows.get(f, [])
            if v:
                out.append(f'{f} mean {sum(v) / len(v) / 1e6:.3f} max {max(v) / 1e6:.3f}')
        print(p.split('/')[-2], ' | '.join(out), f'| windows {len(rows.get(fields[0], []))}')
        if md:
            keys = md[0].keys()
            tot = {k: sum(x[k] for x in md[1:10]) / max(1, len(md[1:10])) for k in keys}
            print('   models (per frame, mean of windows 1..9):', ' '.join(f'{k}={v:.0f}' for k, v in tot.items()))


if __name__ == '__main__':
    main()
