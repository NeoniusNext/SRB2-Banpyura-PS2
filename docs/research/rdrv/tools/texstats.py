import zipfile, struct, re, sys, collections, os
try:
    import lz4.block as lz4b
except Exception:
    lz4b = None
z = zipfile.ZipFile('/opt/srb2-assets/srb2.pk3')
lumps = {}
flats = {}
for i in z.infolist():
    if i.is_dir():
        continue
    fn = i.filename
    base = os.path.basename(fn)
    name = os.path.splitext(base)[0].upper() if '.' in base and not base.startswith('TEXTURES') else base.upper()
    top = fn.split('/')[0]
    if top == 'Flats':
        flats[name] = i
    if top in ('Textures', 'Patches', 'Sprites', 'Graphics', 'Flats'):
        lumps.setdefault(name, i)
def read(i): return z.read(i)
def patch_columns(d):
    w, h, lo, to = struct.unpack_from('<HHhh', d, 0)
    cols = struct.unpack_from('<%dI' % w, d, 8)
    return w, h, lo, to, cols
def draw_patch(d, tex, tw, th, ox, oy):
    # tex: bytearray tw*th of 256 = transparent
    try:
        w, h, lo, to, cols = patch_columns(d)
    except Exception:
        return False
    for x in range(w):
        tx = ox + x
        if tx < 0 or tx >= tw: continue
        p = cols[x]
        while p < len(d):
            top = d[p]
            if top == 255: break
            n = d[p+1]
            p += 3
            for k in range(n):
                ty = oy + top + k
                if 0 <= ty < th:
                    tex[ty*tw+tx] = d[p+k]
            p += n + 1
    return True
# parse TEXTURES
texdefs = []
for i in z.infolist():
    b = os.path.basename(i.filename)
    if b.startswith('TEXTURES.') and i.filename.count('/') == 0:
        t = z.read(i).decode('latin1')
        t = re.sub(r'//[^\n]*', '', t)
        for m in re.finditer(r'(WallTexture|Texture|Sprite|Flat)\s+"([^"]+)"\s*,\s*(\d+)\s*,\s*(\d+)\s*\{(.*?)\n\}', t, re.S):
            kind, name, w, h, body = m.groups()
            patches = re.findall(r'Patch\s+"([^"]+)"\s*,\s*(-?\d+)\s*,\s*(-?\d+)', body)
            texdefs.append((kind, name.upper(), int(w), int(h), patches, i.filename))
print('texture defs', len(texdefs), collections.Counter(k for k,*_ in texdefs))
stats = []
missing = 0
for kind, name, w, h, patches, src in texdefs:
    tex = bytearray([255 if False else 0]) * 0
    buf = bytearray([256 & 255]) * 0
    arr = [256] * (w*h)
    tbuf = bytearray(w*h)
    mask = bytearray(w*h)  # 1 = opaque
    npat = 0
    ok = True
    for pn, x, y in patches:
        info = lumps.get(pn.upper())
        if info is None:
            missing += 1; ok = False; continue
        d = read(info)
        if d[:4] == b'\x89PNG': ok = False; continue
        # draw using mask
        try:
            pw, ph, lo, to, cols = patch_columns(d)
        except Exception:
            ok = False; continue
        npat += 1
        ox, oy = int(x), int(y)
        for cx in range(pw):
            tx = ox + cx
            if tx < 0 or tx >= w: continue
            p = cols[cx]
            while p < len(d):
                top = d[p]
                if top == 255: break
                n = d[p+1]; p += 3
                for k in range(n):
                    ty = oy + top + k
                    if 0 <= ty < h:
                        tbuf[ty*w+tx] = d[p+k]; mask[ty*w+tx] = 1
                p += n + 1
    if not ok and not npat: continue
    colors = set(tbuf[j] for j in range(w*h) if mask[j])
    banks = set(c>>4 for c in colors)
    hasholes = opaque_cnt_dummy = 0
    opaque = sum(mask)
    stats.append((name, w, h, len(patches), len(colors), opaque, bytes(tbuf), kind, len(banks), (min(colors)>>4 if colors else 0), sum(mask) < w*h))
print('composited', len(stats), 'missing patch refs', missing)
tot = sum(s[1]*s[2] for s in stats)
print('total P8 bytes (all textures):', tot, 'MB', tot/1048576)
def bucket(n):
    return '<=16' if n<=16 else '<=32' if n<=32 else '<=64' if n<=64 else '<=128' if n<=128 else '>128'
bk = collections.defaultdict(lambda: [0,0])
for s in stats:
    b = bucket(s[4]); bk[b][0]+=1; bk[b][1]+=s[1]*s[2]
for k in ['<=16','<=32','<=64','<=128','>128']:
    print('colors', k, 'textures', bk[k][0], 'bytes', bk[k][1], '%.1f%% of bytes' % (100*bk[k][1]/tot))
# big textures
big = [s for s in stats if s[1]*s[2] >= 65536]
print('textures >=64K texels:', len(big), 'bytes', sum(s[1]*s[2] for s in big))
bk2 = collections.defaultdict(lambda: [0,0])
for s in big:
    b = bucket(s[4]); bk2[b][0]+=1; bk2[b][1]+=s[1]*s[2]
for k in ['<=16','<=32','<=64','<=128','>128']:
    print('  big colors', k, bk2[k])
# patches per texture distribution
pp = collections.Counter()
for s in stats:
    pp[min(s[3], 100)//10*10] += 1
print('patches per texture hist (x10):', sorted(pp.items()))
# lz4
if lz4b:
    comp = 0
    for s in stats:
        comp += len(lz4b.compress(s[6], mode='high_compression', compression=9, store_size=False))
    print('LZ4HC total of composites:', comp, 'ratio', comp/tot)
# flats
ftot = 0; fcnt = 0; fcol = collections.Counter(); fcolb = collections.Counter()
fsz = collections.Counter()
for n, i in flats.items():
    d = read(i)
    fcnt += 1; ftot += len(d)
    fsz[len(d)] += 1
    c = len(set(d)); fcol[bucket(c)] += 1; fcolb[bucket(c)] += len(d)
print('flats', fcnt, 'bytes', ftot, dict(fsz))
print('flat colors', dict(fcol), 'bytes', dict(fcolb))
import pickle

bb = collections.defaultdict(lambda:[0,0])
for s in stats:
    bb[s[8]][0]+=1; bb[s[8]][1]+=s[1]*s[2]
print('textures by number of 16-banks used:', {k:(v[0], '%.1f%%'%(100*v[1]/tot)) for k,v in sorted(bb.items())})
# flats banks
fb = collections.defaultdict(lambda:[0,0])
for n,i in flats.items():
    d = read(i)
    nb = len(set(c>>4 for c in set(d)))
    fb[nb][0]+=1; fb[nb][1]+=len(d)
print('flats by banks:', {k:(v[0], '%.1f%%'%(100*v[1]/ftot)) for k,v in sorted(fb.items())})
# 1 bank AND colors<=16
one = [s for s in stats if s[8]==1]
print('single-bank textures', len(one), sum(s[1]*s[2] for s in one))

