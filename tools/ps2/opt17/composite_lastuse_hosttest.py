#!/usr/bin/env python3
"""Actual compositor last-use lifetime proof from a checkout and original packs.

No retained research build, generated stock census, production edit or emulator
is required. GCC, ASan/UBSan, Python lz4 and the four external cooked packs are
required. The selected source can precede or contain the exact reviewed fix.
"""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))
import cook
from next13_flats_cut_hosttest import function, section

REFERENCE = "ce622181b6adcac285432ede2e705f5aab4c0b1a"
PACK_NAMES = ("SRB2.PAK", "ZONES.PAK", "CHARS.PAK", "MUSIC.PAK")
SOURCE_FILES = ("src/hardware/hw_cache.c", "src/hardware/hw_data.h",
                "src/hardware/hw_defs.h", "src/r_defs.h", "src/r_textures.h",
                "src/r_patch.c", "src/r_textures.c", "src/r_data.c",
                "src/m_misc.c", "src/doomtype.h")
OLD_TYPE = "typedef struct { UINT16 wad, lump; patch_t *p; } ps2_cpatch_t;"
NEW_TYPE = "typedef struct { UINT16 wad, lump; patch_t *p; INT32 last; } ps2_cpatch_t;"
CHANGES = (
    (OLD_TYPE, NEW_TYPE),
    ("ps2_cpatch_t held[PS2_CPATCH_MAX]; // OPT13 RDRV: patches read for this texture, kept until it is composed",
     "ps2_cpatch_t held[PS2_CPATCH_MAX]; // Patches loaded here, retained through their final placement only."),
    ("\t\t\t\t\t\t\theld[nheld].p = realpatch;\n\t\t\t\t\t\t\tnheld++;",
     "\t\t\t\t\t\t\tINT32 j;\n\n"
     "\t\t\t\t\t\t\theld[nheld].p = realpatch;\n"
     "\t\t\t\t\t\t\theld[nheld].last = i;\n"
     "\t\t\t\t\t\t\tfor (j = i + 1; j < texture->patchcount; j++)\n"
     "\t\t\t\t\t\t\t\tif (texture->patches[j].wad == wadnum && texture->patches[j].lump == lumpnum)\n"
     "\t\t\t\t\t\t\t\t\theld[nheld].last = j;\n"
     "\t\t\t\t\t\t\tnheld++;"),
    ("\t\telse if (loaded_here)\n\t\t\tPatch_Free(realpatch);\n#endif",
     "\t\telse if (loaded_here)\n\t\t\tPatch_Free(realpatch);\n"
     "\t\telse\n\t\t\tfor (h = 0; h < nheld; h++)\n"
     "\t\t\t\tif (held[h].p == realpatch && held[h].last == i)\n\t\t\t\t{\n"
     "\t\t\t\t\t// Only patches loaded by this call belong to held. Keep every\n"
     "\t\t\t\t\t// later reuse, then release after the final synchronous copy.\n"
     "\t\t\t\t\tPatch_Free(realpatch);\n\t\t\t\t\theld[h].p = NULL;\n"
     "\t\t\t\t\tbreak;\n\t\t\t\t}\n#endif"),
    ("\tfor (h = 0; h < nheld; h++)\n\t\tPatch_Free(held[h].p); // OPT13 RDRV: the patches this texture was composed from are not kept (PS2-146: the arena would fill with the patches of every texture seen)",
     "\tfor (h = 0; h < nheld; h++)\n\t\tif (held[h].p)\n"
     "\t\t\tPatch_Free(held[h].p); // Entries stay numbered as before, including the original 32-entry fallback."),
)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def text_sha(text):
    return hashlib.sha256(text.encode()).hexdigest()


def rewrite(text, reverse=False):
    for old, new in reversed(CHANGES) if reverse else CHANGES:
        old, new = (new, old) if reverse else (old, new)
        if text.count(old) != 1:
            raise RuntimeError("Reviewed source change missing or ambiguous: " + old[:80])
        text = text.replace(old, new, 1)
    return text


def valid_patch(data):
    """Input selection only, matching the stock census's bounded Doom-post check.

    The C fixture constructs every selected canonical patch with actual engine
    code. This selector creates no reference pixels or compositor output.
    """
    if len(data) < 13:
        return False
    width, height = struct.unpack_from("<hh", data)
    if not (0 < width <= 16384 and 0 < height <= 16384) or 8 + 4 * width > len(data):
        return False
    offsets = struct.unpack_from(f"<{width}I", data, 8)
    if any(p < 8 + 4 * width or p >= len(data) for p in offsets):
        return False
    try:
        for p in offsets:
            previous = 0
            while data[p] != 255:
                delta, count = data[p], data[p + 1]
                top = previous + delta if delta <= previous else delta
                previous = top
                if p + 4 + count > len(data) or top + count > height:
                    return False
                p += count + 4
    except IndexError:
        return False
    return True


def corpus(packs, target):
    readers = [cook.PackReader(packs / name) for name in PACK_NAMES]
    records = []
    # Preserve the independently audited input order: SRB2, CHARS, ZONES.
    for wad in (0, 2, 1):
        reader = readers[wad]
        for lump, name in enumerate(reader.names):
            if name.endswith(b"/"):
                continue
            raw = reader.get(lump)
            category = name.split(b"/")[0]
            if category == b"Fades":
                continue
            if category == b"Flats":
                side = int(len(raw) ** 0.5)
                if side in (16, 32, 64, 128, 256, 512, 1024) and side * side == len(raw) and not raw.startswith(b"\x89SRPIC\r\n"):
                    continue
            patch = raw[8:] if raw.startswith(b"\x89SRPIC\r\n") else raw
            if valid_patch(patch):
                records.append((wad, lump, 0, name, raw))
    patches = len(records)
    for wad, reader in enumerate(readers):
        for lump, name in enumerate(reader.names):
            if name.rsplit(b"/", 1)[-1].split(b".")[0].upper() == b"TEXTURES":
                records.append((wad, lump, 1, name, reader.get(lump)))
    palette = readers[0].get(readers[0].names.index(b"PLAYPAL"))[:768]
    trans = []
    for level in range(1, 10):
        matches = [i for i, name in enumerate(readers[0].names)
                   if name.rsplit(b"/", 1)[-1].split(b".")[0].upper() == f"TRANS{level}0".encode()]
        if len(matches) != 1:
            raise RuntimeError("Expected one original TRANS table")
        table = readers[0].get(matches[0])
        if len(table) != 65536:
            raise RuntimeError("Malformed original TRANS table")
        trans.append(table)
    with target.open("wb") as output:
        output.write(b"LASTUSE1" + struct.pack("<I", len(records)) + palette + b"".join(trans))
        for wad, lump, kind, name, raw in records:
            output.write(struct.pack("<5I", wad, lump, kind, len(name), len(raw)) + name + raw)
    return {"records": len(records), "patches": patches, "TEXTURES_lumps": len(records) - patches,
            "sha256": sha(target), "bytes": target.stat().st_size,
            "packs_sha256": {name: sha(packs / name) for name in PACK_NAMES}}


def prepare(source_root, out):
    selected = (source_root / "src/hardware/hw_cache.c").read_text()
    for policy in ("#define PS2_CPATCH_MAX 32", "#define HWR_TRYPATCH_MIN (64u<<10)"):
        if selected.count(policy) != 1:
            raise RuntimeError("Existing compositor policy changed: " + policy)
    pinned = subprocess.run(["git", "show", REFERENCE + ":src/hardware/hw_cache.c"],
                            cwd=ROOT, text=True, capture_output=True, check=True).stdout
    old_body = function(pinned, "static void HWR_GenerateTexture(")
    new_body = function(rewrite(pinned), "static void HWR_GenerateTexture(")
    selected_body = function(selected, "static void HWR_GenerateTexture(")
    if selected_body == old_body and OLD_TYPE in selected:
        before, after, state = selected, rewrite(selected), "before-reviewed-fix"
    elif selected_body == new_body and NEW_TYPE in selected:
        before, after, state = rewrite(selected, True), selected, "reviewed-fix-already-present"
    else:
        raise RuntimeError("Selected generation body is neither the pinned reference nor the exact reviewed change")
    if function(before, "static void HWR_GenerateTexture(") != old_body or function(after, "static void HWR_GenerateTexture(") != new_body:
        raise RuntimeError("Literal reference/candidate body binding failed")
    (out / "baseline-hw_cache.c").write_text(before)
    (out / "candidate-hw_cache.c").write_text(after)
    patch = "".join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                       fromfile="a/src/hardware/hw_cache.c", tofile="b/src/hardware/hw_cache.c"))
    (out / "reviewed-change.patch").write_text(patch)
    return before, after, {"selected_generation_state": state, "reference_commit": REFERENCE,
                            "reference_generation_sha256": text_sha(old_body),
                            "candidate_generation_sha256": text_sha(new_body),
                            "baseline_cache_sha256": text_sha(before), "candidate_cache_sha256": text_sha(after),
                            "reviewed_patch_sha256": text_sha(patch),
                            "original_32_slots_and_64KiB_try_policy_preserved": True}


def generate(root, cache, candidate, mutation=None):
    data = (root / "src/hardware/hw_data.h").read_text()
    defs = (root / "src/hardware/hw_defs.h").read_text()
    rd = (root / "src/r_defs.h").read_text()
    tex = (root / "src/r_textures.h").read_text()
    patch = (root / "src/r_patch.c").read_text()
    dtype = (root / "src/doomtype.h").read_text()
    misc = (root / "src/m_misc.c").read_text()
    rtex = (root / "src/r_textures.c").read_text()
    rdata = (root / "src/r_data.c").read_text()
    pre, rest = (HERE / "composite_lastuse_hosttest.c").read_text().split("/* TYPES */")
    services, main = rest.split("/* FUNCTIONS */")
    services = services.replace("typedef struct {UINT16 wad,lump;patch_t *p;} base_cpatch_t;",
                                OLD_TYPE.replace("ps2_cpatch_t", "base_cpatch_t"), 1)
    services = services.replace("typedef struct {UINT16 wad,lump;patch_t *p;INT32 last;} ps2_cpatch_t;", NEW_TYPE, 1)
    types = section(dtype, "typedef struct\n{\n\tUINT8 red;", "typedef union FColorRGBA RGBA_t;") + "typedef union FColorRGBA RGBA_t;\n"
    types += section(data, "typedef enum GLTextureFormat_e", "// Patch information for the hardware renderer.")
    types += section(defs, "enum ETextureFlags", "struct FTextureInfo")
    types += section(rd, "// posts are runs of non masked source pixels", "typedef struct\n{\n\tunsigned topdelta;")
    types += section(rd, "typedef struct\n{\n\tunsigned topdelta;", "//\n// OTHER TYPES")
    types += section(rd, "#ifdef ROTSPRITE\ntypedef struct\n{\n\tINT32 angles;", "#if defined(_MSC_VER)\n#pragma pack(1)")
    types += section(rd, "typedef struct\n{\n\tINT16 width;          // bounding box size", "typedef enum\n{\n\tRF_HORIZONTALFLIP")
    types += section(tex, "typedef struct\n{\n\t// Block origin", "// all loaded and prepared textures")
    code = pre + types + services + section(patch, "#define PATCH_ALIGN", "patch_t *Patch_CreateFromDoomPatch(")
    for name in ("patch_t *Patch_Create(", "patch_t *Patch_CreateFromDoomPatch(", "patch_t *Patch_TryCreateFromDoomPatch(",
                 "void Patch_CalcDataSizes(", "void Patch_MakeColumns(", "static void Patch_FreeData(", "void Patch_Free("):
        code += function(patch, name)
    code += section(misc, "// Token parser variables", "static tokenizer_t *globalTokenizer")
    code += function(rtex, "static texpatch_t *R_ParsePatch(") + function(rtex, "static texture_t *R_ParseTexture(")
    code += function(rdata, "UINT32 ASTBlendPixel(")
    code += section(rdata, "INT32 ASTTextureBlendingThreshold[2]", "// Blends a pixel for a texture patch.")
    code += function(rdata, "UINT32 ASTBlendTexturePixel(") + function(rdata, "UINT8 ASTBlendPaletteIndexes(")
    for name in ("static INT32 format2bpp(", "static void HWR_DrawColumnInCache(", "static void HWR_DrawFlippedColumnInCache(",
                 "static void HWR_DrawTexturePatchInCache(", "static UINT8 *MakeBlockEx(", "static UINT8 *MakeBlock(", "static patch_t *HWR_TryTexturePatch("):
        code += function(cache, name)
    base, fixed = (function(text, "static void HWR_GenerateTexture(") for text in (cache, candidate))
    changes = {
        "early-free": ("held[nheld].last = j;", "held[nheld].last = i;"),
        "free-nonheld": ("\t\telse\n\t\t\tfor (h = 0; h < nheld; h++)", "\t\telse if (!nheld)\n\t\t\tPatch_Free(realpatch);\n\t\telse\n\t\t\tfor (h = 0; h < nheld; h++)"),
        "end-only": ("held[h].last == i", "false"),
    }
    if mutation:
        old, new = changes[mutation]
        if fixed.count(old) != 1:
            raise RuntimeError("Mutation source anchor changed: " + mutation)
        fixed = fixed.replace(old, new, 1)
    code += "\n#define HWR_GenerateTexture base_generate\n#define ps2_cpatch_t base_cpatch_t\n" + base + "\n#undef HWR_GenerateTexture\n#undef ps2_cpatch_t\n"
    return code + "\n#define HWR_GenerateTexture candidate_generate\n" + fixed + "\n#undef HWR_GenerateTexture\n" + main


def run(command, log, check=True):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, text=True, capture_output=True,
                            env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1"), timeout=180)
    log.write_text(" ".join(map(str, command)) + "\n" + result.stdout + result.stderr)
    if check and result.returncode:
        raise RuntimeError(str(command) + "\n" + result.stderr[-3500:] + result.stdout[-1500:])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--packs", type=Path, default=ROOT / "build/pak")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--negative-controls", action="store_true")
    args = parser.parse_args()
    root, out = args.source_root.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    before, after, binding = prepare(root, out)
    inputs = corpus(args.packs.resolve(), out / "stock.bin")
    frozen = out / "frozen-tools"
    frozen.mkdir()
    for path in (Path(__file__), HERE / "composite_lastuse_hosttest.c", HERE.parent / "cook.py", HERE.parent / "next13_flats_cut_hosttest.py"):
        shutil.copyfile(path, frozen / path.name)
    actual = out / "actual-source.c"
    actual.write_text(generate(root, before, after))
    results, missing, artifacts = {}, {}, {}
    flags = ["gcc", "-std=gnu17", "-O2", "-g", "-fwrapv", "-DPS2", "-DPS2_PROFILE", "-DROTSPRITE", "-DHWRENDER",
             "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter",
             "-Wno-misleading-indentation", "-Wno-clobbered"]
    sanitizers = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
    for name, extra in (("plain", []), ("sanitize", sanitizers)):
        binary = out / name
        run([*flags, *extra, actual, "-o", binary, "-lm"], out / (name + "-build.log"))
        result = run([binary, out / "stock.bin"], out / (name + ".log"))
        results[name] = json.loads(result.stdout)
        missing[name] = sorted(re.findall(r"^MISSING_STOCK (.+)$", result.stderr, re.M))
        artifacts[name] = {"binary_sha256": sha(binary), "run_log_sha256": sha(out / (name + ".log")),
                           "build_log_sha256": sha(out / (name + "-build.log"))}
        print(name, results[name], flush=True)
    if results["plain"] != results["sanitize"] or missing["plain"] != missing["sanitize"]:
        raise RuntimeError("Plain/sanitized corpus results differ")
    negatives = {}
    expected = {"early-free": "per-resource canonical build counts unchanged",
                "free-nonheld": "nonheld hardware source must remain live",
                "end-only": "last-use release lowers measured live payload"}
    if args.negative_controls:
        for name, diagnostic in expected.items():
            mutated = out / ("negative-" + name + ".c")
            mutated.write_text(generate(root, before, after, name))
            binary = mutated.with_suffix("")
            run([*flags, *sanitizers, mutated, "-o", binary, "-lm"], out / ("negative-" + name + "-build.log"))
            result = run([binary, out / "stock.bin", "focused"], out / ("negative-" + name + ".log"), False)
            if result.returncode != 101 or diagnostic not in result.stderr:
                raise RuntimeError("Exact executed control assertion not detected: " + name)
            negatives[name] = {"exit": result.returncode, "expected_assertion": diagnostic,
                               "diagnostic": result.stderr.strip(), "source_sha256": sha(mutated), "binary_sha256": sha(binary),
                               "run_log_sha256": sha(out / ("negative-" + name + ".log")),
                               "build_log_sha256": sha(out / ("negative-" + name + "-build.log"))}
    inactive = {}
    for name, defines in (("generic", []), ("PS2-without-profile", ["-DPS2"])):
        output = [subprocess.run(["gcc", "-E", "-P", "-x", "c", *defines, "-"],
                                 input=function(text, "static void HWR_GenerateTexture("),
                                 text=True, capture_output=True, check=True).stdout for text in (before, after)]
        if output[0] != output[1]:
            raise RuntimeError("Inactive generation changed: " + name)
        inactive[name] = True
    report = {"scope": __doc__.strip(), "source_root": str(root), "binding": binding,
              "source_inputs": {name: sha(root / name) for name in SOURCE_FILES},
              "frozen_tools_sha256": {path.name: sha(path) for path in frozen.iterdir()},
              "corpus": inputs, "generated_sha256": sha(actual), "results": results, "run_artifacts": artifacts,
              "unresolved_stock_definitions": missing["plain"], "negative_controls": negatives,
              "inactive_generation_exact": inactive,
              "boundaries": "Archive reads/source lookup, zone allocation/placement, nearest-palette/master-palette/TRANS access, optional TEXC/PNG/flat services and GPU services are controlled host boundaries. Literal AST blend code and original TRANS tables are retained. EE normalized payload is not game RAM or fragmentation. Reference and candidate execute literal complete generation/draw/blend/patch/parser functions.",
              "production_written": False, "emulator_invoked": False, "MAP11_Hardware_admission_established": False}
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("REPORT", out / "report.json")


if __name__ == "__main__":
    main()
