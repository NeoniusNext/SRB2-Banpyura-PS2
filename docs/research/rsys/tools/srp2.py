import struct, sys
def load(path):
    f = open(path,'rb')
    h = f.read(64)
    magic, ver, hs, flags, n, toff, poff, psize, doff, fsize, bsize = struct.unpack('<4sIIIIIIIIII', h[:44])
    assert magic == b'SRP2'
    f.seek(toff); tab = f.read(n*24)
    f.seek(poff); pool = f.read(psize)
    ents = []
    for i in range(n):
        pos, dsz, sz, fn, ln, codec = struct.unpack_from('<IIIIII', tab, i*24)
        name = pool[fn:pool.index(b'\0', fn)].decode('latin1')
        ents.append(dict(i=i,pos=pos,disk=dsz,size=sz,codec=codec,name=name))
    return dict(path=path,ver=ver,flags=flags,n=n,toff=toff,poff=poff,psize=psize,doff=doff,fsize=fsize,ents=ents)
if __name__=='__main__':
    p = load(sys.argv[1])
    print(p['path'], 'ver', p['ver'], 'n', p['n'], 'toff', p['toff'], 'poff', p['poff'], 'psize', p['psize'], 'doff', p['doff'], 'fsize', p['fsize'])
    es = [e for e in p['ents'] if e['size']]
    print('nonempty', len(es), 'sum size', sum(e['size'] for e in es), 'sum disk', sum(e['disk'] for e in es))
