#!/usr/bin/env python3
"""Summary of the device-level I/O journal of an emulator run (build option SRB2_PS2_IOLOG=1: src/ps2/ps2_iolog.c prints "IO R/W/S/O/C" lines).

usage: io_log_summary.py pcsx2.log [pcsx2.log ...] [--mark TEXT ...] [--json]
  --mark TEXT   a line containing TEXT ends the current phase (several: the phases are start .. mark1, mark1 .. mark2, ... rest); default marker:
                "Entering main game loop" (the start-up of the engine is over)
For every phase: device reads and bytes (per file), writes, opens, and the time the media of the OPT13-RSYS model (docs/research/rsys/OPT13_RSYS.md, section 1)
would need for these reads: DVD 4 / 6 MB/s (seek 100/25 ms far/near), USB 1.1 (1 MB/s, 3 ms per command), MX4SIO (3 MB/s, 1 ms), HDD (15 MB/s, 12 ms seek).
The model is an ASSUMPTION about media (the emulator serves host: at disk speed); the counts of commands and bytes are what the engine really asked of the IOP.
"""
import collections
import json
import os
import re
import sys

MEDIA = [  # name, bytes/s, far seek s, near seek s, per command s
    ('DVD4', 4e6, 0.100, 0.025, 0.0),
    ('DVD6', 6e6, 0.080, 0.015, 0.0),
    ('USB1.1', 1.0e6, 0.0, 0.0, 0.003),
    ('MX4SIO', 3e6, 0.0, 0.0, 0.001),
    ('HDD', 15e6, 0.012, 0.004, 0.0005),
]
NEAR = 2 * 1024 * 1024
LINE = re.compile(r'\[\s*([\d.]+)\] IO ([RWSOC]) ?(.*)')


def model(log):
    """log: [(file, offset, length)] in device order -> {medium: seconds}"""
    out = {}
    for name, bw, far, near, cmd in MEDIA:
        t = 0.0
        last = None
        for f, off, ln in log:
            if last is not None:
                if last[0] != f:
                    t += far
                else:
                    dist = abs(off - last[1])
                    if dist:
                        t += near if dist <= NEAR else far
            t += cmd + ln / bw
            last = (f, off + ln)
        out[name] = round(t, 2)
    return out


def phases(path, marks):
    fdname = {}
    cur = {'reads': 0, 'bytes': 0, 'writes': 0, 'wbytes': 0, 'opens': collections.Counter(), 'files': collections.defaultdict(lambda: [0, 0]), 'log': [], 'seeks': 0}
    out = []
    mi = 0
    for line in open(path, errors='replace'):
        if mi < len(marks) and marks[mi] in line:
            out.append(cur)
            cur = {'reads': 0, 'bytes': 0, 'writes': 0, 'wbytes': 0, 'opens': collections.Counter(), 'files': collections.defaultdict(lambda: [0, 0]), 'log': [], 'seeks': 0}
            mi += 1
            continue
        m = LINE.match(line)
        if not m:
            continue
        kind, f = m.group(2), m.group(3).split()
        if kind == 'O':
            nm = os.path.basename(f[1]) if len(f) > 1 else '?'
            fdname[int(f[0])] = nm
            cur['opens'][nm] += 1
        elif kind == 'C':
            fdname.pop(int(f[0]), None)
        elif kind == 'S':
            cur['seeks'] += 1
        elif kind in 'RW' and len(f) >= 5:
            fd, pos, ret = int(f[0]), int(f[1]), int(f[3])
            nm = fdname.get(fd, '?fd%d' % fd)
            if ret <= 0:
                continue
            if kind == 'R':
                cur['reads'] += 1
                cur['bytes'] += ret
                cur['files'][nm][0] += 1
                cur['files'][nm][1] += ret
                if nm.upper().split('.')[0] in ('SRB2', 'ZONES', 'CHARS', 'MUSIC', 'MODELS'):
                    cur['log'].append((nm, pos, ret))
            else:
                cur['writes'] += 1
                cur['wbytes'] += ret
    out.append(cur)
    return out


def main():
    args = sys.argv[1:]
    marks = []
    paths = []
    as_json = False
    i = 0
    while i < len(args):
        if args[i] == '--mark':
            marks.append(args[i + 1])
            i += 2
        elif args[i] == '--json':
            as_json = True
            i += 1
        else:
            paths.append(args[i])
            i += 1
    if not marks:
        marks = ['Entering main game loop']
    result = {}
    for p in paths:
        ph = phases(p, marks)
        names = ['start'] + ['after "%s"' % m[:24] for m in marks]
        result[p] = []
        for n, c in zip(names, ph):
            row = {'phase': n, 'reads': c['reads'], 'bytes': c['bytes'], 'seeks': c['seeks'], 'writes': c['writes'], 'wbytes': c['wbytes'],
                   'files': {k: v for k, v in c['files'].items()}, 'opens': dict(c['opens']), 'model_s': model(c['log'])}
            result[p].append(row)
            if not as_json:
                print('== %s  [%s]: device reads %d, %.2f MB, lseek %d, writes %d (%d B)' % (os.path.basename(os.path.dirname(p)) or p, n, c['reads'], c['bytes'] / 1e6, c['seeks'], c['writes'], c['wbytes']))
                print('   opens: %s' % dict(c['opens']))
                for k, v in sorted(c['files'].items(), key=lambda x: -x[1][1])[:8]:
                    print('   %-16s reads=%5d bytes=%10d' % (k, v[0], v[1]))
                print('   model (s): %s' % ', '.join('%s %.2f' % kv for kv in row['model_s'].items()))
    if as_json:
        print(json.dumps(result, indent=1))


if __name__ == '__main__':
    main()
