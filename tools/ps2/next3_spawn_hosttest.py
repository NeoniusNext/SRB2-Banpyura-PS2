"""Differential real-zone spawn snapshot and actual line archive verification."""
import argparse
import filecmp
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def section(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


def parts(root):
    defs = (root / "src/r_defs.h").read_text()
    types = section(defs, "typedef struct\n{\n\tfixed_t floorheight, ceilingheight;", "#else\ntypedef sector_t spawnsector_t;")
    setup = (root / "src/p_setup.c").read_text()
    build = section(setup, "// Most lines have ten zero arguments." if "// Most lines have ten zero arguments." in setup
                    else "// PS2-OPT-02: the spawn state for the save game diffs", "\n#endif\n\n#ifdef PS2_PROFILE\n// Keep full-width cell offsets;")
    save = (root / "src/p_saveg.c").read_text()
    return {
        "SPAWN_TYPES": types,
        "SPAWN_BUILD": build,
        "SPAWN_GATE": section(setup, '\tif (netgame || M_CheckParm("-keepspawn"))', "#else\n\tspawnsectors = Z_Calloc(numsectors * sizeof(*sectors)"),
        "SPAWN_FLAGS": section(save, "#define LD_FLAG", "// sidedef flags"),
        "SPAWN_EQUAL": section(save, "static boolean P_AreArgsEqual(", "#define FD_FLAGS"),
        "SPAWN_WRITES": section(save, "#define ALLOC_SIZE", "void P_WriteString("),
        "SPAWN_ARCHIVE": section(save, "static void ArchiveLines(", "static void UnArchiveSide("),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, default=ROOT / "build/nov14-next3/baseline-src")
    parser.add_argument("--out", type=Path, default=ROOT / "build/nov14-next3/spawn-memory")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--negative-controls", action="store_true")
    parser.add_argument("--ee-abi-only", action="store_true")
    parser.add_argument("--no-ee-abi", action="store_true")
    parser.add_argument("--keep-wire", action="store_true", help="retain the large literal archive streams")
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    oldparts, newparts = parts(args.baseline), parts(ROOT)
    wire_paths = {}

    def run(name, extracts, flags=(), reject=False):
        work = out / name
        work.mkdir(exist_ok=True)
        defines = []
        for key, value in extracts.items():
            path = work / (key.lower() + ".inc")
            path.write_text(value)
            defines.append(f'-D{key}="{path}"')
        command = [*shlex.split(os.environ.get("CC", "cc")), "-std=c17", "-O1", "-g", "-Wall", "-Wextra",
                   "-Wno-unused-function", "-DPARANOIA", "-I", str(ROOT / "src"), "-I", str(ROOT / "src/ps2"),
                   *defines, *flags, str(ROOT / "tools/ps2/next3_spawn_hosttest.c"), "-o", str(work / "test")]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        built = subprocess.run(command, capture_output=True, text=True)
        (work / "build.log").write_text(built.stdout + built.stderr)
        if built.returncode:
            raise RuntimeError(name + " build failed:\n" + built.stdout + built.stderr)
        wire = work / "archive.wire"
        wire_paths[name] = wire
        tested = subprocess.run([str(work / "test"), str(wire)], capture_output=True, text=True, timeout=180)
        (work / "test.log").write_text(tested.stdout + tested.stderr)
        if reject and tested.returncode:
            assert tested.returncode == 1 and "FAIL:" in tested.stderr, name
            return None
        if tested.returncode:
            raise RuntimeError(name + " run failed:\n" + tested.stdout[-1000:] + tested.stderr)
        cases = [tuple(map(int, row)) for row in re.findall(
            r"^case n=(\d+) k=(\d+) arena=(\d) mode=(\d) digest=(\d+) peak=(\d+) retained=(\d+) allocs=(\d+) request=(\d+)$",
            tested.stdout, re.M)]
        assert len(cases) == 768, len(cases)
        assert "failure cases passed" in tested.stdout
        return cases

    summary = {}
    if not args.ee_abi_only:
        before = run("baseline", oldparts)
        after = run("current", newparts, ["-DSPAWN_NEW"])
        switch = run("old-switch", newparts, ["-DSPAWN_NEW", "-DPS2_NOOPT_SPAWNLINES"])
        assert filecmp.cmp(wire_paths["baseline"], wire_paths["current"], shallow=False), "actual archive bytes differ"
        assert filecmp.cmp(wire_paths["baseline"], wire_paths["old-switch"], shallow=False), "archive switch bytes differ"
        for old, new, restored in zip(before, after, switch):
            assert old[:5] == new[:5] == restored[:5], (old, new, restored)
            assert all(b <= a for a, b in zip(old[5:], new[5:])), (old, new)
            assert old[5:] == restored[5:], (old, restored)
        if args.negative_controls:
            corrupt = dict(newparts)
            corrupt["SPAWN_BUILD"] = corrupt["SPAWN_BUILD"].replace(
                "memset(buffer->args, 0, sizeof (buffer->args));", "memset(buffer->args, 1, sizeof (buffer->args));")
            run("negative-zero-vector", corrupt, ["-DSPAWN_NEW"], reject=True)
            corrupt = dict(newparts)
            corrupt["SPAWN_BUILD"] = corrupt["SPAWN_BUILD"].replace("d->args = spawnargs;", "d->args = s->args;")
            run("negative-live-alias", corrupt, ["-DSPAWN_NEW"], reject=True)
            corrupt = dict(newparts)
            corrupt["SPAWN_ARCHIVE"] = corrupt["SPAWN_ARCHIVE"].replace(
                "P_WriteINT32(save_p, li->args[j]);", "P_WriteINT32(save_p, li->args[j] ^ 1);")
            wrong = run("negative-wire-value", corrupt, ["-DSPAWN_NEW"])
            assert any(a[:5] != b[:5] for a, b in zip(after, wrong)), "wire mutation escaped exact archive comparison"
            assert not filecmp.cmp(wire_paths["current"], wire_paths["negative-wire-value"], shallow=False), "corrupt wire remained byte-identical"
            corrupt = dict(newparts)
            corrupt["SPAWN_BUILD"] = corrupt["SPAWN_BUILD"].replace(
                "spawnlines = Z_Calloc(bytes, PU_LEVEL, NULL);\n\tcompactspawnlines = compact;",
                "compactspawnlines = compact;\n\tspawnlines = Z_Calloc(bytes, PU_LEVEL, NULL);")
            run("negative-early-representation", corrupt, ["-DSPAWN_NEW"], reject=True)
        summary["cases"] = len(after)
        sha = hashlib.sha256()
        with wire_paths["baseline"].open("rb") as stream:
            while block := stream.read(1 << 20): sha.update(block)
        summary["literal_archive_stream"] = {"bytes": wire_paths["baseline"].stat().st_size,
                                             "sha256": sha.hexdigest(), "comparison": "literal byte equality"}
        summary["native"] = [{"n": a[0], "k": a[1], "arena": a[2], "mode": a[3],
                              "digest": a[4], "before": b[5:], "after": a[5:]} for b, a in zip(before, after)]
        print("PASS 768 complete snapshots and exact production ArchiveLines streams, old switch, OOM/reload")
        print("Literal archive stream:", summary["literal_archive_stream"])
        if not args.keep_wire:
            for wire in wire_paths.values(): wire.unlink(missing_ok=True)
    if args.no_ee_abi:
        if (out / "ee-abi.bin").is_file():
            summary["ee_abi"] = struct.unpack("<6I", (out / "ee-abi.bin").read_bytes())
        (out / "results.json").write_text(json.dumps(summary, indent=1) + "\n")
        return
    spec = importlib.util.spec_from_file_location("ps2build", ROOT / "tools/ps2/build.py")
    build = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = build
    spec.loader.exec_module(build)
    build.gen_config()
    compact = section(newparts["SPAWN_BUILD"], "typedef struct", "static boolean compactspawnlines;")
    source = out / "ee-abi.c"
    source.write_text('#include "p_setup.h"\n' + compact +
                      '__attribute__((section(".spawnabi"),used)) const unsigned spawn_abi[] = {'
                      'sizeof(void*),sizeof(spawnline_t),sizeof(compactspawnline_t),'
                      '_Alignof(compactspawnline_t),_Alignof(INT32),sizeof(boolean)};\n')
    obj, binary = out / "ee-abi.o", out / "ee-abi.bin"
    subprocess.run([str(build.CC), *build.CFLAGS, *build.INCS, "-fno-lto", "-c", str(source), "-o", str(obj)], check=True)
    subprocess.run([str(build.CC).replace("gcc", "objcopy"), "-O", "binary", "-j", ".spawnabi", str(obj), str(binary)], check=True)
    abi = struct.unpack("<6I", binary.read_bytes())
    assert abi == (4, 60, 24, 4, 4, 1), abi
    summary["ee_abi"] = abi
    (out / "results.json").write_text(json.dumps(summary, indent=1) + "\n")
    print("EE ABI:", abi)


if __name__ == "__main__":
    main()
