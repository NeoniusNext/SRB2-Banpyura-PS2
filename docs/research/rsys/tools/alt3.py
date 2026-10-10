import re, sys, iosim, srp2
names = ['SRB2','ZONES','CHARS','MUSIC']
P3 = [srp2.load('/home/user/wt/load/build/pak3/%s.PAK' % n) for n in names]
SECT = 2048
class W:
    def __init__(self, idx, fsize, pol):
        self.idx, self.fsize, self.pol = idx, fsize, pol; self.ws = self.we = 0; self.lastend = -1; self.lastmiss = -1
    def read(self, pos, n, out):
        end = min(pos + n, self.fsize); cur = pos
        while cur < end:
            if self.ws <= cur < self.we: cur = min(end, self.we); continue
            s = cur & ~(SECT - 1); need = ((end - s) + SECT - 1) & ~(SECT - 1)
            kind, win, fwd = self.pol
            near_fwd = self.lastend >= 0 and 0 <= s - self.lastend <= fwd
            ln = max(need, win) if near_fwd else max(need, kind)
            ln = min(ln, self.fsize - s)
            out.append((self.idx, s, ln)); self.ws, self.we = s, s + ln; self.lastend = s + ln
            cur = min(end, self.we) if self.we > cur else end
def sim(rd, pol):
    out = []; hs = [W(i, p['fsize'], pol) for i, p in enumerate(P3)]
    for (w, l, size, off, name) in rd:
        e = P3[w]['ents'][l]
        if e['size'] == 0: continue
        if size == 0 or size > e['size'] - off: size = e['size'] - off
        if P3[w]['n'] >= 256 and off == 0 and size <= 16: continue
        if e['codec'] == 0: reqs = [(e['pos'] + off, size)]
        elif e['size'] <= 65536: reqs = [(e['pos'], e['disk'])]
        else:
            nb = 1 + (e['size'] - 1) // 65536; first = off // 65536; last = (off + size - 1) // 65536; avg = (e['disk'] - nb * 4) // nb
            reqs = [(e['pos'], nb * 4)] + [(e['pos'] + nb * 4 + b * avg, avg) for b in range(first, last + 1)]
        for pos, n in reqs: hs[w].read(pos, n, out)
    return out
def rd_from(path, lo=None, hi=None):
    out = []
    for line in open(path, errors='replace'):
        if lo is None:
            m = re.match(r'RD (\d+) (\d+) (\d+) (\d+) (.*)', line); o = 0
        else:
            m = re.match(r'\[\s*([\d.]+)\] RD (\d+) (\d+) (\d+) (\d+) (.*)', line)
            if m and not (lo <= float(m.group(1)) < hi): continue
            o = 1
        if m: out.append((int(m.group(1+o)), int(m.group(2+o)), int(m.group(3+o)), int(m.group(4+o)), m.group(5+o)))
    return out
def row(label, log):
    tot = sum(l for _, _, l in log); r = '  %-30s reads=%4d MB=%6.2f' % (label, len(log), tot / 1e6)
    for m in iosim.MEDIA:
        t, nf, nn = m.time(log); r += '  %s=%5.1f' % (m.name.split()[0] + m.name.split()[1][:3], t)
    print(r)
cases = [('title boot->120fr', '/home/user/wt/rsys-x/build/runs/io2_title/boot.txt', None, None),
         ('MAP01 boot->10fr', '/home/user/wt/rsys-x/build/runs/io2_map01/boot.txt', None, None),
         ('HW D2 play', '/home/user/wt/rsys-x/build/runs/io2_hw_d2/pcsx2.log', 31.1, 70),
         ('HW D4 play', '/home/user/wt/rsys-x/build/runs/io2_hw_d4/pcsx2.log', 28.0, 90)]
pols = [('64K window (now)', None), ('exact 2K sectors only', (2048, 2048, 0)), ('exact 4K only', (4096, 4096, 0)), ('4K / 16K if fwd<=16K', (4096, 16384, 16384)), ('4K exact / 64K if fwd<=64K', (4096, 65536, 65536)), ('4K / 32K if fwd<=128K', (4096, 32768, 131072)), ('4K / 64K if fwd<=256K', (4096, 65536, 262144)), ('8K / 64K if fwd<=256K', (8192, 65536, 262144)), ('16K / 64K if fwd<=1M', (16384, 65536, 1<<20))]
for cn, path, lo, hi in cases:
    rd = rd_from(path, lo, hi); print('==', cn, '(data region only)', len(rd), 'lump reads')
    for label, pol in pols:
        if pol is None: iosim.simulate(rd, P3); row(label, list(iosim.LOG))
        else: row(label, sim(rd, pol))
