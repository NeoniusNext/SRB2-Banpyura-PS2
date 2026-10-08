"""OPT11-FX: what the PC's palette post process would do to a PS2-HW picture: every pixel -> the nearest colour of the (565 crushed) screen palette.
Measures how much of the difference to the PC picture is the missing quantization of blended colours (translucent water, fog, additive sprites).

usage: fx_quant.py PC.png HW.ppm [x,y,w,h]    (PLAYPAL and COLORMAP from srb2.pk3 are read from <scratch>/lumps by default: --pal FILE)
Prints the MAD of HW against PC before and after the quantization, whole picture and region, and the mean colour of the region.
"""
import sys
import zipfile
import io

import numpy as np
from PIL import Image


def palette():
    z = zipfile.ZipFile('/opt/srb2-assets/srb2.pk3')
    p = np.frombuffer(z.read('PLAYPAL')[:768], dtype=np.uint8).reshape(256, 3).astype(np.float32)
    r = np.floor(p[:, 0] / 8) / 31 * 255
    g = np.floor(p[:, 1] / 4) / 63 * 255
    b = np.floor(p[:, 2] / 8) / 31 * 255
    return np.floor(np.stack([r, g, b], 1))


def main():
    pc = np.asarray(Image.open(sys.argv[1]).convert('RGB')).astype(np.float32)
    hw = np.asarray(Image.open(sys.argv[2]).convert('RGB')).astype(np.float32)
    reg = [int(v) for v in sys.argv[3].split(',')] if len(sys.argv) > 3 else [0, 0, pc.shape[1], pc.shape[0]]
    cp = palette()
    flat = hw.reshape(-1, 3)
    out = np.empty_like(flat)
    for i in range(0, len(flat), 2048):
        d = ((flat[i:i + 2048, None, :] - cp[None, :, :]) ** 2).sum(2)
        out[i:i + 2048] = cp[d.argmin(1)]
    q = out.reshape(hw.shape)
    x, y, w, h = reg

    def mad(a, b, r=None):
        if r:
            a, b = a[y:y + h, x:x + w], b[y:y + h, x:x + w]
        return float(np.abs(a - b).mean())

    print('whole picture: MAD HW %.2f -> quantized %.2f' % (mad(hw, pc), mad(q, pc)))
    print('region %s: MAD HW %.2f -> quantized %.2f   mean PC %s HW %s quantized %s' % (reg, mad(hw, pc, 1), mad(q, pc, 1),
          pc[y:y + h, x:x + w].reshape(-1, 3).mean(0).round(1), hw[y:y + h, x:x + w].reshape(-1, 3).mean(0).round(1), q[y:y + h, x:x + w].reshape(-1, 3).mean(0).round(1)))
    on = np.mean(np.all(np.isin(pc.reshape(-1, 3), cp), axis=1))
    print('share of the PC pixels that are palette colours: %.3f; of the HW pixels: %.3f' % (on, float(np.mean((np.abs(flat - out).sum(1) == 0)))))


if __name__ == '__main__':
    main()
