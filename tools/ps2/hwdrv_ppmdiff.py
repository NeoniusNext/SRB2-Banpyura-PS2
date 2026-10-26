import sys, numpy as np
def rd(p):
    b=open(p,'rb').read()
    # P6 w h 255
    parts=b.split(None,4)
    w,h=int(parts[1]),int(parts[2]); data=parts[4] if len(parts)>4 else b''
    hdr=len(b)-w*h*3
    return np.frombuffer(b[hdr:],dtype=np.uint8).reshape(h,w,3).astype(np.int16)
a=rd(sys.argv[1]); b=rd(sys.argv[2])
d=np.abs(a-b).max(axis=2)
print(f"MAD={np.abs(a-b).mean():.3f} over48={(d>48).mean()*100:.2f}% differ={(d>0).mean()*100:.2f}% size={a.shape[1]}x{a.shape[0]}")
