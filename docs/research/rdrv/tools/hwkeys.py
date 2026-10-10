import re,sys
def load(run):
    lines=open('/home/user/wt/rdrv/build/runs/%s/boot.txt'%run,errors='replace').read().splitlines()
    wins=[]  # list of dict line-tag -> text, starting at each 'HWPROF win=N'
    cur=None
    for l in lines:
        m=re.match(r'^HWPROF win=(\d+)',l)
        if m:
            cur={'win':int(m.group(1)),'lines':{}}
            wins.append(cur)
        elif cur is not None:
            m=re.match(r'^(HWPROF\d*)\b',l)
            if m and m.group(1) not in cur['lines']:
                cur['lines'][m.group(1)]=l
        # HWPROF line itself
        if cur is not None and l.startswith('HWPROF win='):
            cur['lines']['HWPROF']=l
    return wins
def kv(line):
    return {k:float(v) for k,v in re.findall(r'(\w+)=(-?\d+(?:\.\d+)?)',line)}
if __name__=='__main__':
    run=sys.argv[1]; wins=[w for w in load(run) if w['win']>=1]
    keys=sys.argv[2:]
    for k in keys:
        tag,key=k.split(':')
        vals=[]
        for w in wins:
            l=w['lines'].get(tag)
            if l:
                d=kv(l)
                if key in d: vals.append(d[key])
        if vals: print('%s %s mean %.1f min %.1f max %.1f  (%d windows)'%(tag,key,sum(vals)/len(vals),min(vals),max(vals),len(vals)))
