"""Differential sector snapshots, real zone, and exact production world archives."""
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
    live = types[:types.index("} spawnsector_t;")]
    live += "\tffloor_t *ffloors;\n\tINT32 crumblestate;\n} sector_t;\n"
    setup = (root / "src/p_setup.c").read_text()
    build = section(setup, "// These fields have useful defaults on most maps." if "// These fields have useful defaults on most maps." in setup
                    else "// Most lines have ten zero arguments." if "// Most lines have ten zero arguments." in setup
                    else "// PS2-OPT-02: the spawn state for the save game diffs", "\n#endif\n\n#ifdef PS2_PROFILE\n// Keep full-width cell offsets;")
    save = (root / "src/p_saveg.c").read_text()
    return {
        "SPAWN_TYPES": types,
        "SPAWN_LIVE": live,
        "SPAWN_BUILD": build,
        "SPAWN_GATE": section(setup, '\tif (netgame || M_CheckParm("-keepspawn"))', "#else\n\tspawnsectors = Z_Calloc(numsectors * sizeof(*sectors)"),
        "SPAWN_FLAGS": section(save, "#define SD_FLOORHT", "static boolean P_AreArgsEqual("),
        "SPAWN_EQUAL": section(save, "static boolean P_AreArgsEqual(", "#define FD_FLAGS"),
        "SPAWN_WRITES": section(save, "#define ALLOC_SIZE", "void P_WriteString(") + section(save, "void P_WriteMem(", "void P_SkipStringN("),
        "SPAWN_ARCHIVE": section(save, "static void ArchiveLines(", "static void UnArchiveSide("),
        "SPAWN_SECTOR_ARCHIVE": section(save, "static void ArchiveSectors(", "static void UnArchiveSectors("),
        "SPAWN_SIDE_ARCHIVE": section(save, "static UINT32 GetSideDiff(", "static void ArchiveLines("),
        "SPAWN_FFLOORS": section(save, "#define FD_FLAGS", "static void UnArchiveFFloors("),
        "SPAWN_TAG_COMPARE": section((root / "src/taglist.c").read_text(), "boolean Tag_Compare (", "/// Search for an element"),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, default=ROOT / "build/nov14-next4/baseline-src")
    parser.add_argument("--out", type=Path, default=ROOT / "build/nov14-next4/sector-memory")
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
                   *defines, *flags, str(ROOT / "tools/ps2/next4_sector_hosttest.c"), "-o", str(work / "test")]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        built = subprocess.run(command, capture_output=True, text=True)
        (work / "build.log").write_text(built.stdout + built.stderr)
        if built.returncode:
            raise RuntimeError(name + " build failed:\n" + built.stdout + built.stderr)
        wire = work / "archive.wire"
        wire_paths[name] = wire
        tested = subprocess.run([str(work / "test"), str(wire)], capture_output=True, text=True, timeout=300)
        (work / "test.log").write_text(tested.stdout + tested.stderr)
        if reject and tested.returncode:
            assert tested.returncode == 1 and "FAIL:" in tested.stderr, name
            return None
        if tested.returncode:
            raise RuntimeError(name + " run failed:\n" + tested.stdout[-1000:] + tested.stderr)
        cases = [tuple(map(int, row)) for row in re.findall(
            r"^case n=(\d+) k=(\d+) arena=(\d) mode=(\d) digest=(\d+) peak=(\d+) retained=(\d+) allocs=(\d+) request=(\d+)$",
            tested.stdout, re.M)]
        assert len(cases) == 1702, len(cases)
        assert "failure cases passed" in tested.stdout
        return cases

    summary = json.loads((out / "results.json").read_text()) if args.ee_abi_only and (out / "results.json").is_file() else {}
    if not args.ee_abi_only:
        before = run("baseline", oldparts)
        after = run("current", newparts, ["-DSPAWN_NEW"])
        switch = run("old-switch", newparts, ["-DSPAWN_NEW", "-DPS2_NOOPT_SPAWNSECTORS"])
        assert filecmp.cmp(wire_paths["baseline"], wire_paths["current"], shallow=False), "actual archive bytes differ"
        assert filecmp.cmp(wire_paths["baseline"], wire_paths["old-switch"], shallow=False), "archive switch bytes differ"
        for old, new, restored in zip(before, after, switch):
            assert old[:5] == new[:5] == restored[:5], (old, new, restored)
            assert all(b <= a for a, b in zip(old[5:], new[5:])), (old, new)
            assert old[5:] == restored[5:], (old, restored)
        if args.negative_controls:
            corrupt = dict(newparts)
            corrupt["SPAWN_BUILD"] = corrupt["SPAWN_BUILD"].replace(
                "buffer->extra_colormap = s->extra_colormap;", "buffer->extra_colormap = NULL;")
            run("negative-pointer-identity", corrupt, ["-DSPAWN_NEW"], reject=True)
            corrupt = dict(newparts)
            corrupt["SPAWN_BUILD"] = corrupt["SPAWN_BUILD"].replace("*values++ : (UINT32)(initial)", "*values++ : (UINT32)(initial) ^ 1")
            run("negative-default-value", corrupt, ["-DSPAWN_NEW"], reject=True)
            corrupt = dict(newparts)
            corrupt["SPAWN_SECTOR_ARCHIVE"] = corrupt["SPAWN_SECTOR_ARCHIVE"].replace(
                "P_WriteFixed(save_p, ss->gravity);", "P_WriteFixed(save_p, ss->gravity ^ 1);")
            wrong = run("negative-wire-value", corrupt, ["-DSPAWN_NEW"])
            assert any(a[:5] != b[:5] for a, b in zip(after, wrong)), "wire mutation escaped exact archive comparison"
            assert not filecmp.cmp(wire_paths["current"], wire_paths["negative-wire-value"], shallow=False), "corrupt wire remained byte-identical"
            corrupt = dict(newparts)
            corrupt["SPAWN_BUILD"] = corrupt["SPAWN_BUILD"].replace(
                "spawnsectors = Z_Calloc(bytes, PU_LEVEL, NULL);\n\tcompactspawnsectors = compact;",
                "compactspawnsectors = compact;\n\tspawnsectors = Z_Calloc(bytes, PU_LEVEL, NULL);")
            run("negative-early-representation", corrupt, ["-DSPAWN_NEW"], reject=True)
        summary["cases"] = len(after)
        sha = hashlib.sha256()
        with wire_paths["baseline"].open("rb") as stream:
            while block := stream.read(1 << 20): sha.update(block)
        summary["literal_archive_stream"] = {"bytes": wire_paths["baseline"].stat().st_size,
                                             "sha256": sha.hexdigest(), "comparison": "literal byte equality"}
        summary["native"] = [{"n": a[0], "k": a[1], "arena": a[2], "mode": a[3],
                              "digest": a[4], "before": b[5:], "after": a[5:]} for b, a in zip(before, after)]
        print("PASS 1702 complete snapshots and exact production sector/line/side/FOF archives, old switch, OOM/reload")
        print("Literal archive stream:", summary["literal_archive_stream"])
        if not args.keep_wire:
            for wire in wire_paths.values(): wire.unlink(missing_ok=True)
    if args.no_ee_abi:
        if (out / "ee-abi.bin").is_file():
            summary["ee_abi"] = struct.unpack("<8I", (out / "ee-abi.bin").read_bytes())
        (out / "results.json").write_text(json.dumps(summary, indent=1) + "\n")
        return
    spec = importlib.util.spec_from_file_location("ps2build", ROOT / "tools/ps2/build.py")
    build = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = build
    spec.loader.exec_module(build)
    build.gen_config()
    compact = section(newparts["SPAWN_BUILD"], "typedef struct", "static boolean compactspawnsectors;")
    source = out / "ee-abi.c"
    source.write_text('#include "p_setup.h"\n' + compact +
                      '_Static_assert((INT32)(UINT32)INT32_MIN == INT32_MIN, "signed32 minimum");\n'
                      '_Static_assert((INT32)(UINT32)INT32_MAX == INT32_MAX, "signed32 maximum");\n'
                      '_Static_assert((INT16)(UINT32)INT16_MIN == INT16_MIN, "signed16 minimum");\n'
                      '_Static_assert((INT16)(UINT32)INT16_MAX == INT16_MAX, "signed16 maximum");\n'
                      '_Static_assert((UINT32)(sectorflags_t)UINT32_MAX == UINT32_MAX, "sector flag bits");\n'
                      '_Static_assert((UINT32)(sectorspecialflags_t)UINT32_MAX == UINT32_MAX, "special flag bits");\n'
                      '__attribute__((section(".spawnabi"),used)) const unsigned spawn_abi[] = {'
                      'sizeof(void*),sizeof(spawnsector_t),sizeof(compactspawnsector_t),'
                      '_Alignof(compactspawnsector_t),_Alignof(UINT32),sizeof(boolean),'
                      'sizeof(sectorflags_t),sizeof(sectorspecialflags_t)};\n')
    obj, binary = out / "ee-abi.o", out / "ee-abi.bin"
    subprocess.run([str(build.CC), *build.CFLAGS, *build.INCS, "-fno-lto", "-c", str(source), "-o", str(obj)], check=True)
    subprocess.run([str(build.CC).replace("gcc", "objcopy"), "-O", "binary", "-j", ".spawnabi", str(obj), str(binary)], check=True)
    abi = struct.unpack("<8I", binary.read_bytes())
    assert abi == (4, 104, 40, 4, 4, 1, 4, 4), abi
    summary["ee_abi"] = abi
    (out / "results.json").write_text(json.dumps(summary, indent=1) + "\n")
    print("EE ABI:", abi)


if __name__ == "__main__":
    main()
