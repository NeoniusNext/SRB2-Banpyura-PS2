import re,sys
sys.path.insert(0,'/home/user/wt/rdrv/build')
from hwkeys import load
run=sys.argv[1]; wins=[w for w in load(run) if w['win']>=1]
def show(tag, pat):
    vals=[]
    for w in wins:
        l=w['lines'].get(tag)
        if not l: continue
        m=re.search(pat,l)
        if m: vals.append([float(x) for x in m.groups()])
    if vals:
        n=len(vals); k=len(vals[0])
        print(tag, pat[:50], ' mean', [round(sum(v[i] for v in vals)/n,1) for i in range(k)], ' max',[max(v[i] for v in vals) for i in range(k)])
show('HWPROF3', r'batches=(\d+) fans=(\d+) single=(\d+) vu=(\d+)')
show('HWPROF51', r'needed a plan: (\d+) of (\d+) \((\d+) cycles\)')
show('HWPROF51', r'flags=(\d+) colour=(\d+) tint=(\d+) fade=(\d+) ltable=(\d+) texkind=(\d+)')
show('HWPROF24', r'n=(\d+) cyc=(\d+)')
show('HWPROF50', r'polygons=(\d+) in (\d+) batches \((\d+) buckets\)')
show('HWPROF50', r'planned polygons=(\d+)')
show('HWPROF23', r'total=(\d+) \(\+ plan (\d+)')
show('HWPROF23', r'settex n=(\d+) cyc=(\d+)')
show('HWPROF23', r'imm=(\d+) begin=(\d+) emit=(\d+)')
show('HWPROF21', r'sort=(\d+) plan=(\d+) draw=(\d+)')
show('HWPROF4', r'segs=(\d+) subsecs=(\d+) planes=(\d+) sprites=(\d+) proc=(\d+) procbatch=(\d+)')
show('HWPROF', r'polys=(\d+) vin=(\d+) vout=(\d+) clip=(\d+) rej=(\d+) qw=(\d+) state=(\d+)')
show('HWPROF', r'uploads=(\d+) upbytes=(\d+) evict=(\d+) clut=(\d+) kicks=(\d+)')
show('HWPROF', r'regen=(\d+) missing=(\d+) skipped=(\d+)')
