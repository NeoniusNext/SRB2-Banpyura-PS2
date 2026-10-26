"""Rewrite the accesses to seg_t.pv1 / pv2 / flength in the hardware renderer sources to the SEG_PV1 / SEG_PV2 / SEG_FLENGTH accessors of r_defs.h (OPT12-CORE, PS2-505).

usage: python tools/ps2/hw_seg_accessors.py [--check] [FILE...]    (default src/hardware/hw_main.c src/hardware/hw_bsp.c; writes in place unless --check)
`gl_curline->pv1` becomes `SEG_PV1(gl_curline)`, `lseg->flength = x` becomes `SEG_FLENGTH(lseg) = x`, and so on (the macros are lvalues). With the accessors the three
hardware-only fields (12 bytes) are not in the 56-byte seg of the software build: in PS2 builds they live in a per-level array that HWR_LoadLevel allocates
(`ps2_seghw`, r_defs.h), 595 KB of the 32 MB zone on MAP11 (49 592 segs) in software mode. Idempotent. Re-run it after merging new hardware code that reads the fields.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PAT = re.compile(r'\b([A-Za-z_][A-Za-z_0-9]*(?:\[[^\]]*\])?)->(pv1|pv2|flength)\b')


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    check = '--check' in sys.argv
    files = [Path(a) for a in args] or [ROOT / 'src/hardware/hw_main.c', ROOT / 'src/hardware/hw_bsp.c']
    for f in files:
        text = f.read_bytes().decode('latin-1')
        new, n = PAT.subn(lambda m: 'SEG_' + m.group(2).upper() + '(' + m.group(1) + ')', text)
        print(f'{f}: {n} accesses {"found" if check else "rewritten"}')
        if n and not check:
            f.write_bytes(new.encode('latin-1'))


if __name__ == '__main__':
    main()
