import sys, glob, os, numpy as np
from PIL import Image
# usage: sheet.py RUNDIR out.png [cols]  : contact sheet of the vidshots of a run (HW only)
d=sys.argv[1]; out=sys.argv[2]; cols=int(sys.argv[3]) if len(sys.argv)>3 else 3
fs=sorted(glob.glob(d+'/vidshot-*.ppm'), key=lambda p:(int(''.join(c for c in os.path.basename(p).split('-')[-1] if c.isdigit()) or 0)))
ims=[np.array(Image.open(f).convert('RGB')) for f in fs]
if not ims: print('no shots'); sys.exit(1)
h,w,_=ims[0].shape
rows=[]
for i in range(0,len(ims),cols):
    r=ims[i:i+cols]
    while len(r)<cols: r.append(np.zeros_like(ims[0]))
    rows.append(np.concatenate(r,axis=1))
Image.fromarray(np.concatenate(rows,axis=0)).save(out); print(out, len(ims), [os.path.basename(f) for f in fs])
