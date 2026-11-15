"""Differential side snapshots, real zone, and exact production world archives."""
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
    parser.add_argument("--baseline", type=Path, default=ROOT / "build/nov14-next5/baseline-src")
    parser.add_argument("--out", type=Path, default=ROOT / "build/nov14-next5/side-memory")
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
                   *defines, *flags, str(ROOT / "tools/ps2/next5_side_hosttest.c"), "-o", str(work / "test")]
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
        if reject:
            assert tested.returncode == 1 and "FAIL:" in tested.stderr, name
            return None
        if tested.returncode:
            raise RuntimeError(name + " run failed:\n" + tested.stdout[-1000:] + tested.stderr)
        cases = [tuple(map(int, row)) for row in re.findall(
            r"^case n=(\d+) k=(\d+) arena=(\d) mode=(\d) digest=(\d+) peak=(\d+) retained=(\d+) allocs=(\d+) request=(\d+)$",
            tested.stdout, re.M)]
        assert len(cases) == (902 if "-DSIDE_UDMF" in flags else 656), len(cases)
        assert "failure cases passed" in tested.stdout
        return cases

    summary = json.loads((out / "results.json").read_text()) if args.ee_abi_only and (out / "results.json").is_file() else {}
    if not args.ee_abi_only:
        for family, extra in (("side", []), ("side-udmf", ["-DSIDE_UDMF"])):
            before = run(family + "-baseline", oldparts, extra)
            after = run(family + "-current", newparts, ["-DSPAWN_NEW", *extra])
            switch = run(family + "-old-switch", newparts, ["-DSPAWN_NEW", "-DPS2_NOOPT_SPAWNSIDES", *extra])
            for name in ("current", "old-switch"):
                assert filecmp.cmp(wire_paths[family + "-baseline"], wire_paths[family + "-" + name], shallow=False), "actual archive bytes differ: " + family + " " + name
            for old, new, restored in zip(before, after, switch):
                assert old[:5] == new[:5] == restored[:5], (old, new, restored)
                assert all(b <= a for a, b in zip(old[5:], new[5:])), (old, new)
                assert old[5:] == restored[5:], (old, restored)
            if args.negative_controls:
                def changed(key, old, new):
                    corrupt = dict(newparts)
                    assert old in corrupt[key], old
                    corrupt[key] = corrupt[key].replace(old, new)
                    return corrupt
                corrupt = changed("SPAWN_BUILD", "X(textureoffset, 0) X(rowoffset, 0)", "X(textureoffset, 1) X(rowoffset, 0)")
                run(family + "-negative-default", corrupt, ["-DSPAWN_NEW", *extra], reject=True)
                corrupt = changed("SPAWN_BUILD", "((UINT32)(sidevalues - sidebase) << SPAWNSIDE_FIELD_BITS)", "((UINT32)0 << SPAWNSIDE_FIELD_BITS)")
                run(family + "-negative-index", corrupt, ["-DSPAWN_NEW", *extra], reject=True)
                corrupt = changed("SPAWN_SIDE_ARCHIVE", "P_WriteINT16(save_p, si->repeatcnt);", "P_WriteINT16(save_p, si->repeatcnt ^ 1);")
                wrongname = family + "-negative-wire"
                wrong = run(wrongname, corrupt, ["-DSPAWN_NEW", *extra])
                assert any(a[:5] != b[:5] for a, b in zip(after, wrong)), "wire mutation escaped exact archive comparison"
                assert not filecmp.cmp(wire_paths[family + "-current"], wire_paths[wrongname], shallow=False), "corrupt wire remained byte-identical"
                corrupt = changed("SPAWN_BUILD",
                    "spawnsides = Z_Calloc(bytes, PU_LEVEL, NULL);\n\tcompactspawnsides = compact;",
                    "compactspawnsides = compact;\n\tspawnsides = Z_Calloc(bytes, PU_LEVEL, NULL);")
                run(family + "-negative-early-representation", corrupt, ["-DSPAWN_NEW", *extra], reject=True)
            sha = hashlib.sha256()
            wire = wire_paths[family + "-baseline"]
            with wire.open("rb") as stream:
                while block := stream.read(1 << 20): sha.update(block)
            summary[family] = {"cases": len(after), "literal_archive_stream": {
                "bytes": wire.stat().st_size, "sha256": sha.hexdigest(), "comparison": "literal byte equality"},
                "native": [{"n": a[0], "k": a[1], "arena": a[2], "mode": a[3],
                            "digest": a[4], "before": b[5:], "after": a[5:]} for b, a in zip(before, after)]}
            print("PASS", family, len(after), "complete snapshots and exact sector/line/side/FOF archives, old switch, OOM/reload")
            print("Literal archive stream:", summary[family]["literal_archive_stream"])
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
    compact = section(newparts["SPAWN_BUILD"], "// A side usually has only one or two textures", "static boolean compactspawnsides;")
    source = out / "ee-abi.c"
    source.write_text('#include "p_setup.h"\n' + compact +
                      '_Static_assert((INT32)(UINT32)INT32_MIN == INT32_MIN, "signed32 minimum");\n'
                      '_Static_assert((INT32)(UINT32)INT32_MAX == INT32_MAX, "signed32 maximum");\n'
                      '_Static_assert((INT16)(UINT32)INT16_MIN == INT16_MIN, "signed16 minimum");\n'
                      '_Static_assert((INT16)(UINT32)INT16_MAX == INT16_MAX, "signed16 maximum");\n'
                      '_Static_assert((UINT32)(sectorflags_t)UINT32_MAX == UINT32_MAX, "sector flag bits");\n'
                      '_Static_assert((UINT32)(sectorspecialflags_t)UINT32_MAX == UINT32_MAX, "special flag bits");\n'
                      '__attribute__((section(".spawnabi"),used)) const unsigned spawn_abi[] = {'
                      'sizeof(void*),sizeof(spawnside_t),sizeof(compactspawnside_t),'
                      '_Alignof(compactspawnside_t),_Alignof(UINT32),sizeof(boolean),'
                      '__builtin_offsetof(spawnside_t,repeatcnt),SPAWNSIDE_FIELD_BITS};\n')
    obj, binary = out / "ee-abi.o", out / "ee-abi.bin"
    subprocess.run([str(build.CC), *build.CFLAGS, *build.INCS, "-fno-lto", "-c", str(source), "-o", str(obj)], check=True)
    subprocess.run([str(build.CC).replace("gcc", "objcopy"), "-O", "binary", "-j", ".spawnabi", str(obj), str(binary)], check=True)
    abi = struct.unpack("<8I", binary.read_bytes())
    assert abi == (4, 24, 4, 4, 4, 1, 20, 6), abi
    summary["ee_abi"] = abi
    (out / "results.json").write_text(json.dumps(summary, indent=1) + "\n")
    print("EE ABI:", abi)


if __name__ == "__main__":
    main()
