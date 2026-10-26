import re, sys, iosim, srp2, alt
P3 = alt.P3
def rd_from_pcsx2(path, lo, hi):
    out = []
    for line in open(path, errors='replace'):
        m = re.match(r'\[\s*([\d.]+)\] RD (\d+) (\d+) (\d+) (\d+) (.*)', line)
        if m and lo <= float(m.group(1)) < hi:
            out.append((int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)), m.group(6)))
    return out
path, lo, hi = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
rd = rd_from_pcsx2(path, lo, hi)
print('lump reads', len(rd))
def show(label, log):
    tot = sum(l for _, _, l in log); row = '  %-34s reads=%4d MB=%6.2f' % (label, len(log), tot / 1e6)
    for m in iosim.MEDIA:
        t, nf, nn = m.time(log); row += '  %s=%6.1f' % (m.name.split()[0] + m.name.split()[1][:3], t)
    print(row)
iosim.simulate(rd, P3); show('current: 64K stdio window', list(iosim.LOG))
for lab, f in (('adaptive 16..64K', ('adaptive', 65536)), ('exact aligned spans', ('exact', 0))):
    show(lab, alt.sim(rd, *f))
# ideal bundle: unique lumps sorted by position, coalesced runs, one sweep
seen = {}
for (w, l, size, off, name) in rd:
    e = P3[w]['ents'][l]
    if e['size'] == 0 or (off == 0 and (size or e['size']) <= 16): continue
    seen[(w, l)] = e
by = {}
for (w, l), e in seen.items(): by.setdefault(w, []).append((e['pos'], e['disk']))
log = []
for w, lst in sorted(by.items()):
    lst.sort(); cur = None
    for pos, n in lst:
        s = pos & ~2047; e_ = ((pos + n) + 2047) & ~2047
        if cur and s <= cur[1] + 65536: cur[1] = max(cur[1], e_)     # merge when the gap is under 64 KB (read through it)
        else:
            if cur: log.append((w, cur[0], cur[1] - cur[0]))
            cur = [s, e_]
    log.append((w, cur[0], cur[1] - cur[0]))
show('bundle sweep (sorted, gap<64K merged)', log)
