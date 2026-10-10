import re, sys, collections, os
import iosim
def parse(path):
    ev = []
    for line in open(path, errors='replace'):
        m = re.match(r'\[\s*([\d.]+)\] IO ([RSOC]) ?(.*)', line)
        if m: ev.append((float(m.group(1)), m.group(2), m.group(3).split()))
    return ev
NAMES = ['SRB2','ZONES','CHARS','MUSIC']
def analyse(path, label, verbose=True):
    ev = parse(path)
    fdname = {}
    reads = []   # (t, name, off, req, ret)
    opens = collections.Counter()
    for t, k, f in ev:
        if k == 'O':
            nm = os.path.basename(f[1]) if len(f) > 1 else '?'
            fdname[int(f[0])] = nm; opens[nm] += 1
        elif k == 'C':
            fdname.pop(int(f[0]), None)
        elif k == 'R':
            fd = int(f[0]); reads.append((t, fdname.get(fd, '?fd%d' % fd), int(f[1]), int(f[2]), int(f[3]), int(f[4])))
    by = collections.defaultdict(lambda: [0, 0])
    log = []
    for t, nm, off, req, ret, us in reads:
        by[nm][0] += 1; by[nm][1] += max(ret, 0)
        base = nm.split('.')[0].upper()
        if base in NAMES and ret > 0:
            log.append((NAMES.index(base), off, ret))
    if verbose:
        print('==', label, ': device reads', len(reads), 'bytes %.2f MB' % (sum(max(r[4], 0) for r in reads) / 1e6))
        print('   opens:', dict(opens))
        for k, v in sorted(by.items(), key=lambda x: -x[1][1]): print('   %-14s reads=%4d bytes=%9d' % (k, v[0], v[1]))
        for m in iosim.MEDIA:
            tt, nf, nn = m.time(log)
            print('   %-34s %6.2f s (far %d near %d)' % (m.name, tt, nf, nn))
    return reads, log, opens
if __name__ == '__main__':
    for p in sys.argv[1:]: analyse(p, p)
