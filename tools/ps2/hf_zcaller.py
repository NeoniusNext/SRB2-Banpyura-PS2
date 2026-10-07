"""OPT10-HF: summarises a -zcaller log (allocations / frees of 16 KiB and more with the caller address): per tag and per caller, the bytes that were
allocated and not freed again by the end of the log (or by the end of each level: --split), callers resolved with the toolchain's addr2line.

usage: hf_zcaller.py run/boot.txt [--elf SRB2.ELF] [--tag 14] [--split]
"""
import argparse
import re
import subprocess
from collections import defaultdict

A2L = '/opt/ps2dev-x/ps2dev/ee/bin/mips64r5900el-ps2-elf-addr2line'


def resolve(elf, addrs):
    out = {}
    if not addrs:
        return out
    r = subprocess.run([A2L, '-f', '-C', '-e', elf] + list(addrs), capture_output=True, text=True)
    lines = r.stdout.splitlines()
    for i, a in enumerate(addrs):
        fn = lines[2 * i] if 2 * i < len(lines) else '?'
        loc = lines[2 * i + 1].rsplit('/', 1)[-1] if 2 * i + 1 < len(lines) else '?'
        out[a] = '%s (%s)' % (fn, loc)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--elf', default='build/val5.ELF')
    ap.add_argument('--tag', type=int, default=14)
    ap.add_argument('--split', action='store_true', help='print the summary at every map change')
    a = ap.parse_args()
    live = {}  # (size) -> list of callers still allocated (the free line has no address of the block: match by size and tag, LIFO)
    pool = defaultdict(list)
    level = 0
    rows = []
    for line in open(a.log, errors='replace'):
        m = re.match(r'\[zcaller\] (\w+) (\d+) tag (\d+) caller (0x[0-9a-f]+|\(nil\)|[0-9a-f]+)', line)
        if m:
            kind, size, tag, caller = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)
            if not caller.startswith('0x'):
                caller = '0x' + caller
            key = (tag, size)
            if kind == 'free':
                if pool[key]:
                    pool[key].pop()
            else:
                pool[key].append(caller)
            continue
        if line.startswith('VIDSHOT') or 'Speeding off to level' in line:
            rows.append((line.strip()[:60], {k: list(v) for k, v in pool.items()}))
    rows.append(('end', {k: list(v) for k, v in pool.items()}))
    for name, snap in rows if a.split else rows[-1:]:
        tot = defaultdict(int)
        calls = defaultdict(lambda: defaultdict(int))
        for (tag, size), callers in snap.items():
            for c in callers:
                tot[tag] += size
                calls[tag][(c, size)] += 1
        print('== %s: not freed (>= 16 KiB) by tag: %s' % (name, ', '.join('%d: %.2f MB' % (t, v / 1048576.0) for t, v in sorted(tot.items()))))
        if name == rows[-1][0] or a.split is False:
            addrs = sorted({c for (c, s) in calls[a.tag]})
            res = resolve(a.elf, addrs)
            agg = defaultdict(lambda: [0, 0])
            for (c, size), n in calls[a.tag].items():
                agg[res.get(c, c)][0] += n
                agg[res.get(c, c)][1] += n * size
            for k, (n, b) in sorted(agg.items(), key=lambda x: -x[1][1])[:12]:
                print('   tag %d: %6d blocks %8.2f MB  %s' % (a.tag, n, b / 1048576.0, k))


if __name__ == '__main__':
    main()
