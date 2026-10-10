import re,sys,statistics as st
def win(run):
    out=[]
    for l in open(f'build/runs/{run}/pcsx2.log',errors='replace'):
        if 'HWPROF win=' in l and 'wall=' in l:
            d=dict(re.findall(r'(\w+)=(\d+)',l.split('| polys=')[-1]))
            w=re.search(r'HWPROF win=(\d+)',l); 
            ws=re.search(r'ws=(\d+)/(\d+)',l); pool=re.search(r'pool=(\d+)/(\d+)',l)
            d['win']=int(w.group(1)); d['wsb']=int(ws.group(1)) if ws else 0; d['wsn']=int(ws.group(2)) if ws else 0
            d['pool']=int(pool.group(1)) if pool else 0; d['poolt']=int(pool.group(2)) if pool else 0
            out.append(d)
    return [x for x in out if x['win']>=1]
print('run uploads upbytesKB evict regen decim(last) ws_blocks(mean/max) ws_tex pool_used(mean/max)/total')
for r in sys.argv[1:]:
    w=win(r)
    if not w: print(r,'none'); continue
    g=lambda k:[int(x.get(k,0)) for x in w]
    print(f"{r:12s} up={st.mean(g('uploads')):.0f} upKB={st.mean(g('upbytes'))/1024:.0f} ev={st.mean(g('evict')):.0f} regen={st.mean(g('regen')):.0f} decim={g('decim')[-1]} ws={st.mean(g('wsb')):.0f}/{max(g('wsb'))} wsn={st.mean(g('wsn')):.0f} pool={st.mean(g('pool')):.0f}/{max(g('pool'))}/{w[0]['poolt']}")
