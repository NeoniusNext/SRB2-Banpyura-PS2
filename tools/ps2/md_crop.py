"""OPT11-MODEL: PC | HW crop of the same box, enlarged: md_crop.py PC.png HW.ppm x,y,w,h OUT.png [zoom]"""
import sys
from PIL import Image
pc = Image.open(sys.argv[1]).convert('RGB')
hw = Image.open(sys.argv[2]).convert('RGB')
x, y, w, h = [int(v) for v in sys.argv[3].split(',')]
z = int(sys.argv[5]) if len(sys.argv) > 5 else 4
a = pc.crop((x, y, x + w, y + h)).resize((w * z, h * z), Image.NEAREST)
b = hw.crop((x, y, x + w, y + h)).resize((w * z, h * z), Image.NEAREST)
im = Image.new('RGB', (w * z * 2 + 8, h * z), (40, 40, 40))
im.paste(a, (0, 0))
im.paste(b, (w * z + 8, 0))
im.save(sys.argv[4])
