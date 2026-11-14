"""Exact production TEXTMAP parser streams and real-zone allocation peaks.

Uses the native C compiler. No engine files are rewritten; generated source,
fixtures, logs and binaries stay in --out. An x64 run checks portability and
relative memory effects, not the EE's 16-byte allocation header or its timings.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess

import make_udmf_map

ROOT = Path(__file__).resolve().parents[2]


def extract(text, start, end):
    return text[text.index(start):text.index(end)]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default=str(ROOT / "build/memory-sol/textmap"))
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    ap.add_argument("--sanitize", action="store_true")
    ap.add_argument("--negative-controls", action="store_true")
    args = ap.parse_args()
    out = Path(args.out).resolve()
    if not out.parent.is_dir():
        ap.error("output parent must already exist")
    out.mkdir(exist_ok=True)
    psetup = (ROOT / "src/p_setup.c").read_text()
    definitions = {
        "MEMORY_TOKENIZER_SOURCE": ROOT / "src/m_tokenizer.c",
        "MEMORY_TOKENIZER_GLOBALS": out / "tokenizer_globals.inc",
        "MEMORY_TEXTMAP_SOURCE": out / "textmap_count.inc",
        "MEMORY_TEXTMAP_PARSE_SOURCE": out / "textmap_parse.inc",
    }
    definitions["MEMORY_TOKENIZER_GLOBALS"].write_text(extract(
        (ROOT / "src/m_misc.c").read_text(), "static tokenizer_t *globalTokenizer", "/** Count bits"))
    definitions["MEMORY_TEXTMAP_SOURCE"].write_text(extract(
        psetup, "// Stores positions for relevant map data", "#ifdef PS2_PROFILE\n// PS2-LOAD-17"))
    definitions["MEMORY_TEXTMAP_PARSE_SOURCE"].write_text(extract(
        psetup, "static void TextmapParse(", "/** Provides a fix to the flat alignment"))
    # Check that the production loader releases each type after its final read.
    loadtext = extract(psetup, "static void P_LoadTextmap(void)", "static void P_ProcessLinedefsAfterSidedefs")
    for label, lap in [("vertex", "VERT"), ("sector", "SECT"), ("linedef", "LINE"), ("sidedef", "SIDE"), ("mapthing", "THING")]:
        assert f"TextmapReleasePos(&{label}Blocks);\n\tLP_END(U_{lap}, lpu);" in loadtext
    fixtures = []

    def fixture(name, text):
        path = out / (name + ".TEXTMAP")
        path.write_text(text)
        fixtures.append(path)

    small = make_udmf_map.textmap(4, 3, 99)[0]
    fixture("ordinary", small)
    fixture("comments", small.replace('namespace = "srb2";', 'namespace = "srb2";\n/* fallback */'))
    fixture("extension", small.replace('namespace = "srb2";', 'namespace = "srb2";\nfuture { ignored = true; }'))
    fixture("no-things", re.sub(r"thing\n\{[^}]*\}\n", "", small))
    # Cross the old 32767-entry allocation and subsequent 1024-entry growth
    # boundaries, interleaving every type and keeping values/duplicate keys.
    kinds = ("thing", "linedef", "sidedef", "vertex", "sector")
    large = 'namespace = "srb2";\n' + "".join(
        f'{kinds[i % 5]} {{ value = {i}; value = "label {i}"; }}\n' for i in range(5 * 34010))
    fixture("large-ordinary", large)
    fixture("large-comments", large.replace('namespace = "srb2";', 'namespace = "srb2";\n// fallback\n'))
    fixture("empty-type-comments", 'namespace = "srb2";\n/* fallback */\nvertex { x = 1; y = 2; }\n')

    def run(name, flags=(), definitions_override=None, failure=False, test_args=()):
        localdefs = definitions | (definitions_override or {})
        exe = out / name
        command = [*shlex.split(args.cc), "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-function",
                   "-DPARANOIA", "-I", str(ROOT / "src"), "-I", str(ROOT / "src/ps2"), *flags,
                   *[f'-D{k}="{v.as_posix()}"' for k, v in localdefs.items()],
                   str(ROOT / "tools/ps2/memory_textmap_hosttest.c"), "-o", str(exe)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        built = subprocess.run(command, text=True, capture_output=True)
        (out / (name + ".build.log")).write_text(built.stdout + built.stderr)
        if built.returncode:
            raise RuntimeError(name + " build failed:\n" + built.stdout + built.stderr)
        tested = subprocess.run([str(exe), *test_args, *map(str, fixtures)], capture_output=True, text=True, timeout=90)
        (out / (name + ".test.log")).write_text(tested.stdout + tested.stderr)
        if tested.returncode and not failure:
            raise RuntimeError(name + " failed:\n" + tested.stdout + tested.stderr)
        if failure:
            assert tested.returncode == 1 and "FAIL:" in tested.stderr, name + " must fail an assertion"
            print(name + ": assertion rejected the mutation")
            return None
        rows = [re.fullmatch(r"fixture=.*? hash=(\d+) pairs=(\d+) counts=([\d,]+) loaded=(\d+) retained=(\d+) peak=(\d+)", line).groups()
                for line in tested.stdout.splitlines() if line.startswith("fixture=")]
        assert len(rows) == len(fixtures)
        return rows

    native_reference = run("reference", ["-DPS2_NOOPT_TEXTMAPMEM"])
    # The EE has 4-byte positions AND pointers. Normalize the old pointer-sized
    # expression on x64 as a stricter reference, so its accidental 2x allocation
    # cannot conceal a regression of the EE's fallback allocation peak.
    ee_positions = out / "textmap_ee_positions.inc"
    ee_positions.write_text(definitions["MEMORY_TEXTMAP_SOURCE"].read_text().replace("sizeof(blocks->pos)", "sizeof(*blocks->pos)"))
    reference_defs = {"MEMORY_TEXTMAP_SOURCE": ee_positions}
    baseline = run("reference-ee-positions", ["-DPS2_NOOPT_TEXTMAPMEM"], reference_defs)
    actual = run("compact")
    for path, native, a, b in zip(fixtures, native_reference, baseline, actual):
        assert native[:3] == a[:3] == b[:3], "parser stream/counts differ: " + path.name
        assert int(b[4]) == 0, "temporary storage retained: " + path.name
        assert int(b[5]) <= int(a[5]), "allocation peak regressed: " + path.name
        print(f"{path.stem}: identical {b[1]} pairs; loaded {a[3]} -> {b[3]}, retained {a[4]} -> {b[4]}, peak {a[5]} -> {b[5]} bytes")
    for mode, switches in [("one-sided", ["--one-sided"]), ("redzones", ["--redzones"])]:
        before = run("reference-ee-positions-" + mode, ["-DPS2_NOOPT_TEXTMAPMEM"], reference_defs, test_args=switches)
        after = run("compact-" + mode, test_args=switches)
        for path, a, b in zip(fixtures, before, after):
            assert a[:3] == b[:3], mode + " parser stream/counts differ: " + path.name
            assert int(b[4]) == 0 and int(b[5]) <= int(a[5]), mode + " allocation regressed: " + path.name
        print(mode + ": all seven streams exact, no increased allocation peak")
    if args.negative_controls:
        mutated = out / "textmap_no_release.inc"
        source = definitions["MEMORY_TEXTMAP_SOURCE"].read_text()
        source = source.replace("\tZ_Free(blocks->pos);\n\tblocks->pos = NULL;\n\tblocks->capacity = 0;", "\t(void)blocks;")
        assert source != definitions["MEMORY_TEXTMAP_SOURCE"].read_text()
        mutated.write_text(source)
        run("negative-retained-positions", definitions_override={"MEMORY_TEXTMAP_SOURCE": mutated}, failure=True)
        mutated = out / "textmap_wrong_position.inc"
        mutated.write_text(definitions["MEMORY_TEXTMAP_SOURCE"].read_text().replace("M_TokenizerGetEndPos();", "M_TokenizerGetEndPos() + 2;"))
        broken = run("negative-wrong-position", definitions_override={"MEMORY_TEXTMAP_SOURCE": mutated})
        assert any(a[:3] != b[:3] for a, b in zip(actual, broken)), "wrong positions survived exact comparison"
        print("negative-wrong-position: exact parser-stream comparison rejected the mutation")
        mutated = out / "tokenizer_static_scan.c"
        source = definitions["MEMORY_TOKENIZER_SOURCE"].read_text()
        mutant = source.replace("Z_Realloc(*array, (size_t)ncap * esize, PU_LEVEL, array)",
                                "Z_Realloc(*array, (size_t)ncap * esize, PU_STATIC, NULL)")
        assert source != mutant
        mutated.write_text(mutant)
        run("negative-static-scan", definitions_override={"MEMORY_TOKENIZER_SOURCE": mutated}, failure=True)
    print("PASS production parser equivalence, ownership cleanup, overflow and zone peaks")


if __name__ == "__main__":
    main()
