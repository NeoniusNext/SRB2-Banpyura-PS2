"""Rewrite the reads of the UDMF-only sidedef fields in src/hardware/hw_main.c to the SIDE_* accessors of r_defs.h (PS2-74).

usage: python tools/ps2/hw_side_accessors.py [--check] [FILE...]    (default src/hardware/hw_main.c; writes in place unless --check)
`gl_sidedef->scalex_mid` becomes `SIDE_SCALEX_MID(gl_sidedef)`, `side->light` becomes `SIDE_LIGHT(side)`, and so on. With the accessors the
hardware build can use the 36-byte sidedef of the software build (r_defs.h: define PS2_HW_SIDE_COMPACT, or drop `|| defined(HWRENDER)`):
MAP11 has 43 170 sides, 60 bytes each = 2.6 MB less in the 32 MB zone. Idempotent; only reads are rewritten (the script refuses on a write).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIELDS = r'(offsetx_(?:top|mid|bottom)|offsety_(?:top|mid|bottom)|scalex_(?:top|mid|bottom)|scaley_(?:top|mid|bottom)|light_top|light_mid|light_bottom|lightabsolute_top|lightabsolute_mid|lightabsolute_bottom|lightabsolute|light)'
READ = re.compile(r'\b(gl_sidedef|side)->' + FIELDS + r'\b')
WRITE = re.compile(r'\b(?:gl_sidedef|side)->' + FIELDS + r'\b\s*(?:[-+*/|&^]|<<|>>)?=(?!=)')


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    check = '--check' in sys.argv
    files = [Path(a) for a in args] or [ROOT / 'src/hardware/hw_main.c']
    for f in files:
        text = f.read_bytes().decode('latin-1')
        if WRITE.search(text):
            raise SystemExit(f'{f}: a UDMF side field is written; only reads can use the accessors')
        new, n = READ.subn(lambda m: 'SIDE_' + m.group(2).upper() + '(' + m.group(1) + ')', text)
        print(f'{f}: {n} reads {"found" if check else "rewritten"}')
        if n and not check:
            f.write_bytes(new.encode('latin-1'))


if __name__ == '__main__':
    main()
