import iolog2, sys, collections, iosim
def phases(path, cuts, names):
    reads, log, opens = iolog2.analyse(path, path, verbose=False)
    for i, nm in enumerate(names):
        lo = cuts[i]; hi = cuts[i+1] if i+1 < len(cuts) else 1e9
        rs = [r for r in reads if lo <= r[0] < hi]
        lg = [(iolog2.NAMES.index(n.split('.')[0].upper()), off, ret) for t, n, off, req, ret, us in rs if n.split('.')[0].upper() in iolog2.NAMES and ret > 0]
        by = collections.defaultdict(int)
        for t, n, off, req, ret, us in rs: by[n] += max(ret, 0)
        row = '  %-22s reads=%4d MB=%6.2f ' % (nm, len(rs), sum(max(r[4],0) for r in rs)/1e6)
        for m in iosim.MEDIA:
            tt, nf, nn = m.time(lg); row += ' %s=%5.1fs' % (m.name.split()[0]+m.name.split()[1][:3], tt)
        print(row, dict((k, v) for k, v in by.items() if v > 100000))
path = sys.argv[1]
cuts = [float(x) for x in sys.argv[2].split(',')]; names = sys.argv[3].split(',')
phases(path, [0] + cuts, names)
