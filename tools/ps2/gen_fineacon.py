"""Write FINEACON.DAT, the arccos table of src/t_facon.c (65536*2 little-endian UINT32), next to the packs.

usage: gen_fineacon.py OUTDIR            (build/opt6-f/pak ...)
The PS2 profile keeps the table out of the ELF (512 KB of resident memory used only by Lua's acos/asin): src/tables.c reads this file
on the first FixedAcos call. Without it the engine falls back to computing every value (truncated double acos, PS2-101).
"""
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    out = Path(sys.argv[1]) / 'FINEACON.DAT'
    text = (ROOT / 'src/t_facon.c').read_text()
    body = text[text.index('{') + 1:text.rindex('}')]
    vals = [4294967295 if x == 'ANGLE_MAX' else int(x) for x in re.findall(r'ANGLE_MAX|-?\d+', body)]
    assert len(vals) == 65536 * 2, len(vals)
    out.write_bytes(struct.pack('<%dI' % len(vals), *vals))
    print('wrote', out, out.stat().st_size, 'bytes')


if __name__ == '__main__':
    main()
