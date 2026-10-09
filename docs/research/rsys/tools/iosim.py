"""Model of the pack reader (w_pack.c ReadBytes/WPack_ReadLump) on top of newlib's 64 KB FILE buffer.
Input: RD lines (wad lump size offset name) from -lpreads; pack indexes (srp2.py). Output: device reads (pack, offset, len)."""
import sys, re, struct
import srp2

SECT = 2048
BLK = 65536
BUF = 65536

LOG = []
class Handle:
    def __init__(self, fsize, idx=0):
        self.idx = idx
        self.fsize = fsize
        self.ws = self.we = 0      # valid buffer window [ws, we)
        self.upos = 0              # underlying fd position
        self.pos = 0               # logical FILE position
        self.dev = []              # (offset, len)
    def seek(self, t):
        if self.ws <= t <= self.we and self.we > self.ws:
            self.pos = t
        else:
            self.ws = self.we = 0
            self.upos = t
            self.pos = t
    def read(self, n):
        # fread of n bytes at logical pos
        start = self.pos
        end = start + n
        if end > self.fsize:
            n = self.fsize - start; end = self.fsize
        cur = start
        while cur < end:
            if self.ws <= cur < self.we:
                take = min(end, self.we) - cur
                cur += take
                continue
            # refill: from the underlying position (window end if the window is valid and cur is at it)
            if self.we > self.ws and cur == self.we:
                p = self.we
            else:
                p = cur
            got = min(BUF, self.fsize - p)
            if got <= 0: break
            self.dev.append((p, got)); LOG.append((self.idx, p, got))
            self.ws, self.we = p, p + got
            self.upos = p + got
        self.pos = end

def read_bytes(h, pos, size):
    # ReadBytes at logical position pos (after fseek(pos)): bounce loop
    done = 0
    while done < size:
        skip = pos % SECT
        want = min(size - done, BLK - skip)
        req = (skip + want + SECT - 1) & ~(SECT - 1)
        h.seek(pos - skip)
        h.read(req)
        done += want; pos += want

def wpack_read(h, e, size, offset, idx_blocks=None):
    """e: entry dict; returns nothing, records reads in h"""
    usize = e['size']
    if e['codec'] == 0:
        read_bytes(h, e['pos'] + offset, size)
        return
    if usize <= BLK:
        read_bytes(h, e['pos'], e['disk'])
        return
    nb = 1 + (usize - 1) // BLK
    read_bytes(h, e['pos'], nb * 4)
    # block sizes unknown without reading the file: approximate each block as disk/nb
    first = offset // BLK; last = (offset + size - 1) // BLK
    avg = (e['disk'] - nb * 4) // nb
    p = e['pos'] + nb * 4 + first * avg
    for b in range(first, last + 1):
        read_bytes(h, p, avg)
        p += avg

def parse_rd(path, start_after=None):
    out = []
    for line in open(path, errors='replace'):
        if line.startswith('RD '):
            f = line.split(None, 5)
            out.append((int(f[1]), int(f[2]), int(f[3]), int(f[4]), f[5].strip() if len(f) > 5 else ''))
    return out

def simulate(rd, packs, use_head=True):
    del LOG[:]
    hs = [Handle(p['fsize'], i) for i, p in enumerate(packs)]
    nreq = 0; headhits = 0
    for (w, l, size, off, name) in rd:
        e = packs[w]['ents'][l]
        if e['size'] == 0: continue
        if size == 0 or size > e['size'] - off: size = e['size'] - off
        if use_head and packs[w]['n'] >= 256 and packs[w]['ver'] >= 2 and off == 0 and size <= 16:
            headhits += 1; continue
        nreq += 1
        wpack_read(hs[w], e, size, off)
    return hs, nreq, headhits

class Medium:
    def __init__(self, name, bw, seek_far, seek_near, near=2*1024*1024, per_cmd=0.0):
        self.name, self.bw, self.seek_far, self.seek_near, self.near, self.per_cmd = name, bw, seek_far, seek_near, near, per_cmd
    def time(self, log):
        t = 0.0; last = None; nseek_far = nseek_near = 0
        for pk, off, ln in log:
            if last is None: d = None
            elif last[0] != pk: d = 'far'
            else:
                dist = abs(off - last[1])
                d = 0 if dist == 0 else ('near' if dist <= self.near else 'far')
            if d == 'far': t += self.seek_far; nseek_far += 1
            elif d == 'near': t += self.seek_near; nseek_near += 1
            t += self.per_cmd + ln / self.bw
            last = (pk, off + ln)
        return t, nseek_far, nseek_near

MEDIA = [
    Medium('DVD 4 MB/s, seek 100/25 ms', 4e6, 0.100, 0.025),
    Medium('DVD 6 MB/s, seek 80/15 ms', 6e6, 0.080, 0.015),
    Medium('USB1.1 1 MB/s, 3 ms/cmd', 1.0e6, 0.0, 0.0, per_cmd=0.003),
    Medium('MX4SIO 3 MB/s, 1 ms/cmd', 3e6, 0.0, 0.0, per_cmd=0.001),
    Medium('HDD 15 MB/s, seek 12 ms', 15e6, 0.012, 0.004, per_cmd=0.0005),
]

def interleave(hs):
    # reads in global time order are not tracked per handle; approximate by pack order of appearance: caller passes ordered list
    return [(i, h.dev) for i, h in enumerate(hs)]
