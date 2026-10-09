import sys, glob, os, numpy as np
from PIL import Image
def rd(p):
    return np.array(Image.open(p).convert('RGB')).astype(np.int16)
tag, scen = sys.argv[1], sys.argv[2]
so = f'build/runs/{tag}-{scen}-so'; ha = f'build/runs/{tag}-{scen}-ha'
out = f'build/panels/{tag}-{scen}'; os.makedirs('build/panels', exist_ok=True)
rows=[]
for p in sorted(glob.glob(so+'/vidshot-*.ppm')):
    q = ha + '/' + os.path.basename(p)
    if not os.path.exists(q): print('missing', q); continue
    a=rd(p); b=rd(q)
    if a.shape!=b.shape: print(os.path.basename(p),'size differs', a.shape, b.shape); continue
    d=np.abs(a-b).max(axis=2)
    print(f"{os.path.basename(p):28s} MAD={np.abs(a-b).mean():6.2f} over48={(d>48).mean()*100:5.1f}%")
    diff=np.clip(np.abs(a-b)*4,0,255)
    rows.append(np.concatenate([a,b,diff],axis=1).astype(np.uint8))
if rows:
    Image.fromarray(np.concatenate(rows,axis=0)).save(out+'.png')
    print('saved', out+'.png', len(rows))
