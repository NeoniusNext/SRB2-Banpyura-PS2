#!/usr/bin/env python3
"""Prepare the pass13 implicit drawnode candidate in an isolated source tree."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
REVISION = "353cc94103aeb38cef87d26151c779302135d0e9"
OUT = ROOT / "build/nov15-next13/ram-source-v1"
BASE = ROOT / "build/nov15-next13/ram-baseline-v1"
MAIN = "src/hardware/hw_main.c"
GUARD = "defined(PS2) && defined(PS2_PROFILE) && !defined(PS2_NOOPT_IMPLICIT_DN)"

HELPER = r'''#if defined(PS2) && defined(PS2_PROFILE) && !defined(PS2_NOOPT_IMPLICIT_DN)
// The unchanged sort index already identifies each node: planes, polyplanes,
// then walls. Keep the original bases/counts instead of four pointers per node.
// Capture after allocation and before filling indices, matching the old tuples
// even if a later draw callback changes a source list's global pointer/count.
static struct
{
	planeinfo_t *plane;
	polyplaneinfo_t *polyplane;
	wallinfo_t *wall;
	size_t nplane, npolyplane;
} dn_source;

static inline gl_drawnode_t HWR_ImplicitDrawNode(size_t n)
{
	gl_drawnode_t node = {NULL, NULL, NULL, NULL};

	if (n < dn_source.nplane)
		node.plane = &dn_source.plane[n];
	else if (n - dn_source.nplane < dn_source.npolyplane)
		node.polyplane = &dn_source.polyplane[n - dn_source.nplane];
	else
		node.wall = &dn_source.wall[n - dn_source.nplane - dn_source.npolyplane];
	return node;
}
#define HWR_SORTNODE(n) HWR_ImplicitDrawNode(n)
#else
#define HWR_SORTNODE(n) sortnode[n]
#endif

'''


def sha(data):
    return hashlib.sha256(data).hexdigest()


def sources():
    paths = subprocess.check_output(
        ["git", "ls-tree", "-r", "--name-only", REVISION, "src"], cwd=ROOT,
        text=True).splitlines()
    if len(paths) != 602:
        raise RuntimeError("Expected all 602 canonical source files")
    return {p: subprocess.check_output(["git", "show", REVISION + ":" + p], cwd=ROOT)
            for p in paths}


def rewrite(text):
    anchor = "static int CompareDrawNodes(const void *p1, const void *p2)\n"
    if text.count(anchor) != 1:
        raise RuntimeError("Comparator anchor changed")
    text = text.replace(anchor, HELPER + anchor, 1)
    a = text.index("static int CompareDrawNodes(")
    b = text.index("//  Draw all vissprites", a)
    body = text[a:b]
    import re
    body = re.sub(r"sortnode\[(sortindex\[(?:i|run_start)\]|n[12])\]", r"HWR_SORTNODE(\1)", body)
    body = body.replace("\t\tstatic gl_drawnode_t *dn_buf;",
                        "#if !(" + GUARD + ")\n\t\tstatic gl_drawnode_t *dn_buf;\n#endif", 1)
    body = body.replace("\t\t\tdn_buf = realloc(dn_buf, cap * sizeof *dn_buf);",
                        "#if !(" + GUARD + ")\n\t\t\tdn_buf = realloc(dn_buf, cap * sizeof *dn_buf);\n#endif", 1)
    body = body.replace("\t\t\tif (!dn_buf || !di_buf)",
                        "#if " + GUARD + "\n\t\t\tif (!di_buf)\n#else\n\t\t\tif (!dn_buf || !di_buf)\n#endif", 1)
    body = body.replace("\t\tmemset(dn_buf, 0, need * sizeof *dn_buf);\n\t\tsortnode = dn_buf;",
                        "#if !(" + GUARD + ")\n\t\tmemset(dn_buf, 0, need * sizeof *dn_buf);\n\t\tsortnode = dn_buf;\n#endif", 1)
    anchor = "\tfor (i = 0; i < numplanes; i++, p++)"
    snapshot = ("#if " + GUARD + "\n"
                "\tdn_source.plane = planeinfo;\n\tdn_source.polyplane = polyplaneinfo;\n"
                "\tdn_source.wall = wallinfo;\n\tdn_source.nplane = numplanes;\n"
                "\tdn_source.npolyplane = numpolyplanes;\n#endif\n\n")
    body = body.replace(anchor, snapshot + anchor, 1)
    for field, source in (("plane", "planeinfo"), ("polyplane", "polyplaneinfo"), ("wall", "wallinfo")):
        statement = "\t\tsortnode[p]." + field + " = &" + source + "[i];"
        if body.count(statement) != 1:
            raise RuntimeError("Node writer changed: " + field)
        body = body.replace(statement, "#if !(" + GUARD + ")\n" + statement + "\n#endif", 1)
    text = text[:a] + body + text[b:]
    return text


def prepare(dest, data):
    expected = {p: sha(v) for p, v in data.items()}
    if dest.exists():
        actual = {p.relative_to(dest).as_posix(): sha(p.read_bytes())
                  for p in (dest / "src").rglob("*") if p.is_file()}
        if actual != expected:
            raise RuntimeError("Refusing mismatched existing source tree " + str(dest))
        return
    for path, value in data.items():
        output = dest / path
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(value)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=OUT)
    parser.add_argument("--base", type=Path, default=BASE)
    args = parser.parse_args()
    if args.out.resolve() == ROOT or args.base.resolve() == ROOT:
        raise RuntimeError("Production-tree edits are forbidden")
    original = sources()
    candidate = dict(original)
    candidate[MAIN] = rewrite(original[MAIN].decode()).encode()
    prepare(args.base, original)
    prepare(args.out, candidate)
    report = {
        "revision": REVISION, "source_count": len(original),
        "baseline": str(args.base), "candidate": str(args.out),
        "source_sha256": {p: sha(v) for p, v in original.items()},
        "candidate_source_sha256": {p: sha(v) for p, v in candidate.items()},
        "changed": [p for p in original if original[p] != candidate[p]],
        "tool_sha256": sha(Path(__file__).read_bytes()),
    }
    patch = "".join(difflib.unified_diff(original[MAIN].decode().splitlines(True),
                                      candidate[MAIN].decode().splitlines(True),
                                      fromfile="a/" + MAIN, tofile="b/" + MAIN))
    for name, value in (("source-manifest.json", json.dumps(report, indent=2) + "\n"),
                        ("candidate.patch", patch)):
        path = args.out / name
        if path.exists() and path.read_text() != value:
            raise RuntimeError("Refusing to rewrite frozen " + str(path))
        path.write_text(value)
    print(json.dumps({"changed": report["changed"], "source_count": len(original),
                      "candidate": str(args.out), "main_sha256": sha(candidate[MAIN])}))


if __name__ == "__main__":
    main()
