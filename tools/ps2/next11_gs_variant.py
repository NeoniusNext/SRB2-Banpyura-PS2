#!/usr/bin/env python3
"""Checked CLUT active-core/inline-guard experiment, exactly reversible."""
import argparse
from pathlib import Path

BEFORE = 'static void clut_swizzle(u32 *entries)\n{\n\tu32 i;\n\n\tif (ps2hwd_dbg_flags & 16)\n\t\treturn;\n\tfor (i = 0; i < 256; i += 32)\n\t{\n\t\tu32 j;\n\n\t\tfor (j = 8; j < 16; j++)\n\t\t{\n\t\t\tu32 t = entries[i + j];\n\n\t\t\tentries[i + j] = entries[i + j + 8];\n\t\t\tentries[i + j + 8] = t;\n\t\t}\n\t}\n}\n'
AFTER = 'static __attribute__((noinline, noclone)) void clut_swizzle_active(u32 *entries)\n{\n\tu32 i;\n\n#ifdef __mips__\n\t// CLUT packets are quadword-aligned. Keep the scalar path for other word-aligned inputs.\n\tif (!((uintptr_t)entries & 15))\n\t{\n#pragma GCC unroll 1\n\t\tfor (i = 0; i < 256; i += 32)\n\t\t{\n\t\t\tu32 *p = entries + i;\n\n\t\t\t__asm__ volatile(\n\t\t\t\t"lq $8, 32(%0)\\n"\n\t\t\t\t"lq $9, 64(%0)\\n"\n\t\t\t\t"sq $9, 32(%0)\\n"\n\t\t\t\t"sq $8, 64(%0)\\n"\n\t\t\t\t"lq $8, 48(%0)\\n"\n\t\t\t\t"lq $9, 80(%0)\\n"\n\t\t\t\t"sq $9, 48(%0)\\n"\n\t\t\t\t"sq $8, 80(%0)\\n"\n\t\t\t\t: : "r"(p) : "$8", "$9", "memory");\n\t\t}\n\t\treturn;\n\t}\n#endif\n\tfor (i = 0; i < 256; i += 32)\n\t{\n\t\tu32 j;\n\n#pragma GCC unroll 1\n\t\tfor (j = 8; j < 16; j++)\n\t\t{\n\t\t\tu32 t = entries[i + j];\n\n\t\t\tentries[i + j] = entries[i + j + 8];\n\t\t\tentries[i + j + 8] = t;\n\t\t}\n\t}\n}\n\nstatic inline __attribute__((always_inline)) void clut_swizzle(u32 *entries)\n{\n\tif (ps2hwd_dbg_flags & 16)\n\t\treturn;\n\tclut_swizzle_active(entries);\n}\n'
ASM = '\t\t\t__asm__ volatile(\n\t\t\t\t"lq $8, 32(%0)\\n"\n\t\t\t\t"lq $9, 64(%0)\\n"\n\t\t\t\t"sq $9, 32(%0)\\n"\n\t\t\t\t"sq $8, 64(%0)\\n"\n\t\t\t\t"lq $8, 48(%0)\\n"\n\t\t\t\t"lq $9, 80(%0)\\n"\n\t\t\t\t"sq $9, 48(%0)\\n"\n\t\t\t\t"sq $8, 80(%0)\\n"\n\t\t\t\t: : "r"(p) : "$8", "$9", "memory");'
INLINE_BEFORE = "static void clut_swizzle(u32 *entries)"
INLINE_AFTER = "static inline __attribute__((always_inline)) void clut_swizzle(u32 *entries)"

def rewrite(body, reverse=False):
    original, replacement = (AFTER, BEFORE) if reverse else (BEFORE, AFTER)
    if body.count(original) != 1:
        raise RuntimeError("Missing unique complete CLUT swizzle source anchor")
    return body.replace(original, replacement, 1)


def host_emulation(body):
    if body.count(ASM) != 1 or body.count("#ifdef __mips__\n\t// CLUT packets") != 1:
        raise RuntimeError("Missing exact quadword-emulation anchors")
    replacement = """            unsigned char left[16], right[16];
            memcpy(left, (unsigned char *)p + 32, 16);
            memcpy(right, (unsigned char *)p + 64, 16);
            memcpy((unsigned char *)p + 32, right, 16);
            memcpy((unsigned char *)p + 64, left, 16);
            memcpy(left, (unsigned char *)p + 48, 16);
            memcpy(right, (unsigned char *)p + 80, 16);
            memcpy((unsigned char *)p + 48, right, 16);
            memcpy((unsigned char *)p + 80, left, 16);"""
    return body.replace("#ifdef __mips__\n\t// CLUT packets", "#if 1\n\t// CLUT packets", 1).replace(ASM, replacement, 1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--reverse", action="store_true")
    args = parser.parse_args()
    args.destination.parent.mkdir(parents=True, exist_ok=True)
    args.destination.write_text(rewrite(args.source.read_text(), args.reverse))
