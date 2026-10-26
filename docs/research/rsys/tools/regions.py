import iolog2, sys, srp2
P3 = {n: srp2.load('/home/user/wt/load/build/pak3/%s.PAK' % n) for n in ('SRB2','ZONES','CHARS','MUSIC')}
for path in sys.argv[1:]:
    reads, log, opens = iolog2.analyse(path, path, verbose=False)
    print('==', path)
    for pk in ('SRB2','ZONES','CHARS','MUSIC'):
        p = P3[pk]
        idx = p['doff']
        rs = [(t, off, ret) for t, nm, off, req, ret, us in reads if nm.upper() == pk + '.PAK' and ret > 0]
        a = sum(r for t, o, r in rs if o < idx)
        b = sum(r for t, o, r in rs if o >= idx)
        # sequential runs
        runs = 0; last = None
        for t, o, r in rs:
            if last is None or o != last: runs += 1
            last = o + r
        print('  %-6s reads=%3d  index-region bytes=%8d  data-region bytes=%8d  runs=%d  index size=%d' % (pk, len(rs), a, b, runs, idx))
