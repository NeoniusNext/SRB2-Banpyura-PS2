import iosim, srp2, sys
names = ['SRB2','ZONES','CHARS','MUSIC']
P3 = [srp2.load('/home/user/wt/load/build/pak3/%s.PAK' % n) for n in names]
SECT = 2048
class Win:
    """Alternative reader: policy 'adaptive' = exact aligned span for a non-sequential request, 64 KB-window refill when the request continues right after the last one"""
    def __init__(self, idx, fsize, policy, maxwin=65536):
        self.idx, self.fsize, self.policy, self.maxwin = idx, fsize, policy, maxwin
        self.ws = self.we = 0; self.lastend = -1; self.run = 0
    def read(self, pos, n, out):
        end = min(pos + n, self.fsize); cur = pos
        while cur < end:
            if self.ws <= cur < self.we:
                cur = min(end, self.we); continue
            # miss
            start = cur & ~(SECT - 1)
            need = ((end - start) + SECT - 1) & ~(SECT - 1)
            if self.policy == 'exact':
                ln = need
            else:
                seq = (cur == self.we and self.we > self.ws) or start == self.lastend
                ln = max(need, min(self.maxwin, 16384 << min(self.run, 2))) if seq else need
                self.run = self.run + 1 if seq else 0
            ln = min(ln, self.fsize - start)
            out.append((self.idx, start, ln))
            self.ws, self.we = start, start + ln; self.lastend = start + ln
            cur = min(end, self.we) if self.we > cur else end
def sim(rd, policy, maxwin=65536):
    out = []; hs = [Win(i, p['fsize'], policy, maxwin) for i, p in enumerate(P3)]
    for (w, l, size, off, name) in rd:
        e = P3[w]['ents'][l]
        if e['size'] == 0: continue
        if size == 0 or size > e['size'] - off: size = e['size'] - off
        if P3[w]['n'] >= 256 and off == 0 and size <= 16: continue
        # emulate wpack_read positions
        if e['codec'] == 0: reqs = [(e['pos'] + off, size)]
        elif e['size'] <= 65536: reqs = [(e['pos'], e['disk'])]
        else:
            nb = 1 + (e['size'] - 1) // 65536
            first = off // 65536; last = (off + size - 1) // 65536
            avg = (e['disk'] - nb * 4) // nb
            reqs = [(e['pos'], nb * 4)] + [(e['pos'] + nb * 4 + b * avg, avg) for b in range(first, last + 1)]
        for pos, n in reqs: hs[w].read(pos, n, out)
    return out
for rn, path in (('title', '/home/user/wt/rsys-x/build/runs/io2_title/boot.txt'), ('MAP01', '/home/user/wt/rsys-x/build/runs/io2_map01/boot.txt')):
    rd = iosim.parse_rd(path)
    print('==', rn, '(data region only, no index/headers)')
    for label, f in (('current 64K FILE window', None), ('adaptive 16K..64K', ('adaptive', 65536)), ('adaptive 16K..32K', ('adaptive', 32768)), ('exact (no readahead)', ('exact', 0))):
        if f is None:
            iosim.simulate(rd, P3); log = list(iosim.LOAD) if False else list(iosim.LOG)
        else:
            log = sim(rd, *f)
        tot = sum(l for _, _, l in log)
        row = '  %-26s reads=%4d MB=%5.2f' % (label, len(log), tot / 1e6)
        for m in iosim.MEDIA:
            t, nf, nn = m.time(log); row += '  %s=%5.2f' % (m.name.split()[0] + m.name.split()[1][:3], t)
        print(row)
