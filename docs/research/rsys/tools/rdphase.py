import re, sys, collections, srp2
P3 = [srp2.load('/home/user/wt/load/build/pak3/%s.PAK' % n) for n in ('SRB2','ZONES','CHARS','MUSIC')]
path, lo, hi = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
cnt = collections.Counter(); dsk = {}
n = 0
for line in open(path, errors='replace'):
    m = re.match(r'\[\s*([\d.]+)\] RD (\d+) (\d+) (\d+) (\d+) (.*)', line)
    if not m: continue
    t = float(m.group(1))
    if not (lo <= t < hi): continue
    w, l, size, off = int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5))
    e = P3[w]['ents'][l]
    if e['size'] == 0 or (off == 0 and (size or e['size']) <= 16): continue
    n += 1; cnt[(w, l)] += 1; dsk[(w, l)] = (e['disk'], e['size'], m.group(6))
uniq = len(cnt); tot_d = sum(dsk[k][0] for k in cnt)
rep = sum(c - 1 for c in cnt.values())
rep_bytes = sum((c - 1) * dsk[k][0] for k, c in cnt.items())
print('lump reads (non-head) %d, unique lumps %d, repeats %d; compressed bytes of unique lumps %.2f MB, of repeats %.2f MB' % (n, uniq, rep, tot_d / 1e6, rep_bytes / 1e6))
bycat = collections.defaultdict(lambda: [0, 0, 0])
for k, c in cnt.items():
    nm = dsk[k][2]; top = nm.split('/')[0] if '/' in nm else '(root)'
    if k[0] == 2: top = 'CHARS:' + (nm.split('/')[0])
    b = bycat[top]; b[0] += 1; b[1] += c; b[2] += dsk[k][0] * c
for k, v in sorted(bycat.items(), key=lambda x: -x[1][2])[:10]: print('  %-18s uniq=%4d reads=%4d bytes=%.2f MB' % (k, v[0], v[1], v[2] / 1e6))
top = sorted(cnt.items(), key=lambda x: -x[1])[:8]
print('most re-read:', [(dsk[k][2].split('/')[-1], c, dsk[k][0]) for k, c in top])
