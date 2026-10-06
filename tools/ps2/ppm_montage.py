"""Montage of PPM screenshots (vidshot) into one PNG: ppm_montage.py OUT.png a.ppm b.ppm ... (2 per row, 2x scale)."""
import sys
from PIL import Image

out, files = sys.argv[1], sys.argv[2:]
imgs = [Image.open(f).convert('RGB') for f in files]
w, h = imgs[0].size
cols = 2
rows = (len(imgs) + cols - 1) // cols
m = Image.new('RGB', (cols * w, rows * h))
for i, im in enumerate(imgs):
    m.paste(im, ((i % cols) * w, (i // cols) * h))
m = m.resize((m.width * 2 // 2 * 1, m.height))
m.save(out)
print(out, m.size)
