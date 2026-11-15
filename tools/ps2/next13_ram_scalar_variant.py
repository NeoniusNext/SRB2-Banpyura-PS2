#!/usr/bin/env python3
"""Prepare a scalar ordinal drawnode view; preserve the rejected v1 sources."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path

import next13_ram_variant as first

ROOT, BASE, MAIN, REVISION = first.ROOT, first.BASE, first.MAIN, first.REVISION
OUT = ROOT / "build/nov15-next13/ram-source-v2"
GUARD = first.GUARD

COUNT = r'''// The comparator needs only the draw count. Keep its scalar result separate
// from the draw loop's pointer tuple, avoiding repeated tuple construction.
static inline INT32 HWR_ImplicitDrawCount(size_t n)
{
	if (n < dn_source.nplane)
		return dn_source.plane[n].drawcount;
	if (n - dn_source.nplane < dn_source.npolyplane)
		return dn_source.polyplane[n - dn_source.nplane].drawcount;
	return dn_source.wall[n - dn_source.nplane - dn_source.npolyplane].drawcount;
}

'''


def rewrite(before):
    text = first.rewrite(before)
    text = text.replace("#define HWR_SORTNODE(n) HWR_ImplicitDrawNode(n)",
                        COUNT + "#define HWR_SORTNODE(n) HWR_ImplicitDrawNode(n)", 1)
    start = text.index("\tif (HWR_SORTNODE(n1).plane)")
    end = text.index("\n\tdiff = v2 - v1;", start)
    original = text[start:end]
    text = text[:start] + ("#if " + GUARD + "\n\tv1 = HWR_ImplicitDrawCount(n1);\n"
                         "\tv2 = HWR_ImplicitDrawCount(n2);\n#else\n" + original + "\n#endif\n") + text[end:]
    start = text.index('\tif (!HWR_SORTNODE(n1).plane) I_Error("CompareDrawNodePlanes:')
    end = text.index("\n}", start)
    original = text[start:end]
    scalar = '''\tif (n1 >= dn_source.nplane) I_Error("CompareDrawNodePlanes: Uh.. This isn't a plane! (n1)");
\tif (n2 >= dn_source.nplane) I_Error("CompareDrawNodePlanes: Uh.. This isn't a plane! (n2)");
\treturn abs(dn_source.plane[n2].fixedheight - viewz) - abs(dn_source.plane[n1].fixedheight - viewz);'''
    text = text[:start] + "#if " + GUARD + "\n" + scalar + "\n#else\n" + original + "\n#endif" + text[end:]
    start = text.index("\tfor (i = 0; i < p; i++)", text.index("static void HWR_CreateDrawNodes"))
    end = text.index("\n\tPS_STOP_TIMING(ps_hw_nodedrawtime);", start)
    loop = text[start:end]
    loop = loop.replace("HWR_SORTNODE(sortindex[i])", "HWR_DN_DRAW")
    opening = "\tfor (i = 0; i < p; i++)\n\t{\n"
    capture = (opening + "#if " + GUARD + "\n"
               "\t\tconst gl_drawnode_t drawnode = HWR_SORTNODE(sortindex[i]);\n"
               "#define HWR_DN_DRAW drawnode\n#else\n"
               "#define HWR_DN_DRAW HWR_SORTNODE(sortindex[i])\n#endif\n")
    if loop.count(opening) != 1:
        raise RuntimeError("Exact draw-loop opening changed")
    loop = loop.replace(opening, capture, 1) + "#undef HWR_DN_DRAW\n"
    return text[:start] + loop + text[end:]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=OUT)
    args = parser.parse_args()
    if args.out.resolve() == ROOT:
        raise RuntimeError("Production edits are forbidden")
    original = first.sources()
    candidate = dict(original)
    candidate[MAIN] = rewrite(original[MAIN].decode()).encode()
    first.prepare(BASE, original)
    first.prepare(args.out, candidate)
    sha = lambda raw: hashlib.sha256(raw).hexdigest()
    manifest = {"revision": REVISION, "stage": "scalar-ordinal-v2", "source_count": 602,
                "baseline": str(BASE), "candidate": str(args.out.resolve()),
                "source_sha256": {p: sha(v) for p, v in original.items()},
                "candidate_source_sha256": {p: sha(v) for p, v in candidate.items()},
                "changed": [MAIN], "tool_sha256": sha(Path(__file__).read_bytes()),
                "v1_generator_sha256": sha(Path(first.__file__).read_bytes())}
    patch = "".join(difflib.unified_diff(original[MAIN].decode().splitlines(True), candidate[MAIN].decode().splitlines(True),
                                       fromfile="a/" + MAIN, tofile="b/" + MAIN))
    for name, value in (("source-manifest.json", json.dumps(manifest, indent=2) + "\n"), ("candidate.patch", patch)):
        path = args.out / name
        if path.exists() and path.read_text() != value:
            raise RuntimeError("Refusing to rewrite frozen scalar source metadata")
        path.write_text(value)
    print(json.dumps({"candidate": str(args.out), "source_count": 602, "changed": [MAIN], "main_sha256": sha(candidate[MAIN])}))


if __name__ == "__main__":
    main()
