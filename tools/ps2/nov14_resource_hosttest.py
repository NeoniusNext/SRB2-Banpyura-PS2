"""Differential production resource tests against the saved next-iteration baseline.

Uses the real zone on a native host. Target ABI constants come from EE GCC;
reported EE payload/rounded sizes are calculations, not integrated RAM samples.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import shutil

from verify_pack import decode

ROOT = Path(__file__).resolve().parents[2]


def section(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


def source_parts(source):
    definitions = (source / "src/r_defs.h").read_text()
    types = section(definitions, "// posts are runs of non masked source pixels", "//\n// OTHER TYPES")
    types += section(definitions, "#ifdef ROTSPRITE\ntypedef struct\n{\n\tINT32 angles;", "// Possible alpha types for a patch.")
    types += section(definitions, "enum patchalphastyle {", ";\n\n") + ";\n"
    header = (source / "src/r_textures.h").read_text()
    types += section(header, "typedef struct\n{\n\t// Block origin", "// all loaded and prepared textures")
    patch = (source / "src/r_patch.c").read_text()
    patch = section(patch, "patch_t *Patch_Create(", "#ifdef PS2_PROFILE") + section(
        patch, "#define PATCH_ALIGN", "// The zone may drop an embedded patch") + section(
        patch, "void Patch_CalcDataSizes(", "//\n// Frees patches with a tag range.")
    texture = (source / "src/r_textures.c").read_text()
    globals_ = section(texture, "INT32 numtextures = 0;", "// Painfully simple texture id")
    globals_ += section(texture, "static struct {", "//\n// MAPTEXTURE_T CACHING")
    renderer = globals_ + section(texture, "static void R_DrawColumnInCache(",
        "\n#if defined(PS2) && defined(PS2_PROFILE)\n// PS2-180")
    renderer += section(texture, "void R_CheckTextureCache(", "INT32 R_GetTextureNumForFlat(")
    names = texture[texture.index("void R_ClearTextureNumCache("):]
    return {"RESOURCE_TYPES": types, "RESOURCE_PATCH": patch,
            "RESOURCE_RENDERER": renderer, "RESOURCE_NAMECACHE": names}


def valid_patch(data):
    if len(data) < 8:
        return None
    width, height, _, _ = struct.unpack_from("<hhhh", data)
    if width < 0 or height < 0 or len(data) < 8 + width * 4:
        return None
    pixels = posts = 0
    try:
        for x in range(width):
            off = struct.unpack_from("<I", data, 8 + 4 * x)[0]
            if off < 8 + 4 * width:
                return None
            while data[off] != 255:
                length = data[off + 1]
                if off + length + 4 > len(data):
                    return None
                pixels += length
                posts += 1
                off += length + 4
    except (IndexError, struct.error):
        return None
    return width, pixels, posts


def make_corpus(path, pakdir):
    rows = []
    with path.open("wb") as corpus:
        def append(name, data):
            sizes = valid_patch(data)
            assert sizes is not None, name
            corpus.write(struct.pack("<I", len(data)) + data)
            rows.append((name, *sizes))

        for width in (0, 1, 2, 3, 4, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 129, 255):
            for pattern in range(8):
                columns = []
                for x in range(width):
                    column = bytearray()
                    for p in range((x + pattern) % 11):
                        top = (p * 127 + x) % 255
                        length = (x * 19 + p * 11 + pattern) % 33
                        column += bytes((top, length, 0)) + bytes((x + p * 3 + y) % 256 for y in range(length)) + b"\0"
                    columns.append(column + b"\xff")
                off = 8 + 4 * width
                offsets = []
                for column in columns:
                    offsets.append(off)
                    off += len(column)
                data = struct.pack("<hhhh", width, 4096, -32768 + pattern, 32767 - pattern)
                data += struct.pack("<" + "I" * width, *offsets) + b"".join(columns)
                append(f"synthetic-{width}-{pattern}", data)

        for pak in ("SRB2.PAK", "CHARS.PAK"):
            with (pakdir / pak).open("rb") as f:
                header = struct.unpack("<4s11I", f.read(48))
                assert header[0] == b"SRP2"
                count, tableoff, pooloff, poolsize, block = header[4], header[5], header[6], header[7], header[10]
                f.seek(tableoff)
                entries = [struct.unpack("<6I", f.read(24)) for _ in range(count)]
                f.seek(pooloff)
                pool = f.read(poolsize)
                for pos, disk, size, full, _, codec in entries:
                    name = pool[full:pool.index(b"\0", full)].decode("ascii")
                    if not size or not name.lower().startswith(("sprites/", "patches/", "graphics/", "textures/")):
                        continue
                    data = decode(f, pos, disk, size, codec, block)
                    if data.startswith(b"\x89SRPIC\r\n"):
                        data = data[8:]
                    if valid_patch(data) is not None:
                        append(pak + ":" + name, data)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, default=ROOT / "build/nov14-next/baseline-src")
    parser.add_argument("--out", type=Path, default=ROOT / "build/nov14-next/resource-memory")
    parser.add_argument("--pak", type=Path, default=ROOT / "build/pak")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--negative-controls", action="store_true")
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    corpus = out / "patches.bin"
    fixtures = make_corpus(corpus, args.pak)
    (out / "patches.json").write_text(json.dumps(fixtures, indent=1) + "\n")

    def run(label, parts, flags=(), reject=False):
        work = out / label
        work.mkdir(exist_ok=True)
        defines = []
        for key, source in parts.items():
            path = work / (key.lower() + ".inc")
            path.write_text(source)
            defines.append(f'-D{key}="{path}"')
        exe = work / "test"
        command = [*shlex.split(args.cc), "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-function",
                   "-DPARANOIA", "-I", str(ROOT / "src"), "-I", str(ROOT / "src/ps2"),
                   *defines, *flags, str(ROOT / "tools/ps2/nov14_resource_hosttest.c"), "-o", str(exe)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        built = subprocess.run(command, capture_output=True, text=True)
        (work / "build.log").write_text(built.stdout + built.stderr)
        if built.returncode:
            raise RuntimeError(label + " build failed:\n" + built.stdout + built.stderr)
        tested = subprocess.run([str(exe), str(corpus)], capture_output=True, text=True, timeout=180)
        (work / "test.log").write_text(tested.stdout + tested.stderr)
        if reject and tested.returncode:
            assert tested.returncode == 1 and "FAIL:" in tested.stderr, label + " must fail an assertion"
            return None
        if tested.returncode:
            raise RuntimeError(label + " failed:\n" + tested.stdout[-2000:] + tested.stderr)
        hashes = re.findall(r"^(patches=\S+|composite=\S+|namecache=\S+) hash=(\d+)", tested.stdout, re.M)
        assert len(hashes) == 338, len(hashes)
        composites = [tuple(map(int, m)) for m in re.findall(r"^composite=\S+ hash=\d+ peak=(\d+) allocs=(\d+) retained=(\d+)", tested.stdout, re.M)]
        patch = tuple(map(int, re.search(r"^patches=(\d+) hash=(\d+) requested=(\d+) blocks=(\d+)", tested.stdout, re.M).groups()))
        names = tuple(map(int, re.search(r"^namecache=(\d+) hash=(\d+) stride=(\d+) retained=(\d+)", tested.stdout, re.M).groups()))
        return {"hashes": hashes, "composites": composites, "patches": patch, "names": names}

    baseline = run("baseline", source_parts(args.baseline))
    currentparts = source_parts(ROOT)
    current = run("current", currentparts)
    assert baseline["hashes"] == current["hashes"], "normalized resource stream differs"
    assert current["patches"][2] <= baseline["patches"][2] and current["patches"][3] <= baseline["patches"][3]
    assert current["names"][2:] < baseline["names"][2:]
    for old, new in zip(baseline["composites"], current["composites"]):
        assert all(b <= a for a, b in zip(old, new)), (old, new)
    for name, flags in [("old-layout", ["-DPS2_NOOPT_PATCHLAYOUT"]),
                        ("old-reuse", ["-DPS2_NOOPT_texreuse"])]:
        variant = run(name, currentparts, flags)
        assert variant["hashes"] == baseline["hashes"], name + " stream differs"
    if args.negative_controls:
        broken = dict(currentparts)
        broken["RESOURCE_PATCH"] = broken["RESOURCE_PATCH"].replace("posts->data_offset = data_offset;", "posts->data_offset = data_offset + 1;")
        run("negative-patch-offset", broken, reject=True)
        broken = dict(currentparts)
        broken["RESOURCE_RENDERER"] = broken["RESOURCE_RENDERER"].replace("post->topdelta = post->data_offset = (size_t)y;", "post->topdelta = post->data_offset = (size_t)y + 1;")
        mutant = run("negative-composite-offset", broken, reject=True)
        if mutant is not None:
            assert mutant["hashes"] != current["hashes"], "corrupted composite escaped exact stream comparison"
        print("Both corrupted production variants were rejected")
    summary = {"fixtures": len(fixtures), "composites": len(current["composites"]),
               "baseline": {k: v for k, v in baseline.items() if k != "hashes"},
               "current": {k: v for k, v in current.items() if k != "hashes"}}
    eecc = shutil.which("mips64r5900el-ps2-elf-gcc")
    if eecc:
        work = out / "ee-abi"
        work.mkdir(exist_ok=True)
        definitions = work / "types.inc"
        definitions.write_text(currentparts["RESOURCE_TYPES"])
        source = work / "abi.c"
        namedeclaration = section((ROOT / "src/r_textures.c").read_text(), "static struct {", "static INT32 tidcachelen")
        source.write_text("#include <stddef.h>\n#include <stdint.h>\n"
            "typedef uint8_t UINT8; typedef uint16_t UINT16; typedef int16_t INT16;\n"
            "typedef int32_t INT32; typedef uint32_t UINT32; typedef int boolean;\n"
            "#define PS2_PROFILE\n#define ROTSPRITE\n#define ATTRPACK __attribute__((packed))\n"
            f'#include "{definitions}"\n'
            + namedeclaration +
            '__attribute__((section(".resourceabi"),used)) const unsigned resource_abi[] = {'
            "sizeof(void*),sizeof(patch_t),_Alignof(patch_t),sizeof(column_t),_Alignof(column_t),"
            "sizeof(post_t),_Alignof(post_t),offsetof(patch_t,embedded),sizeof(*tidcache)};\n")
        obj, binary = work / "abi.o", work / "abi.bin"
        subprocess.run([eecc, "-G0", "-O2", "-std=gnu23", "-c", str(source), "-o", str(obj)], check=True)
        subprocess.run([eecc[:-3] + "objcopy", "-O", "binary", "-j", ".resourceabi", str(obj), str(binary)], check=True)
        abi = struct.unpack("<9I", binary.read_bytes())
        assert abi == (4, 48, 4, 12, 4, 12, 4, 44, 20), abi
        totals = [0, 0, 0, 0]
        for _, width, pixels, posts in fixtures:
            old = ((abi[1] + 15) & ~15)
            old = (old + abi[3] * width + 15) & ~15
            old = ((old + abi[5] * posts + 15) & ~15) + pixels
            new = ((abi[1] + abi[3] * width + abi[5] * posts + 15) & ~15) + pixels
            assert new <= old
            roundedold, roundednew = (old + 16 + 15) & ~15, (new + 16 + 15) & ~15
            totals = [a + b for a, b in zip(totals, (old, new, roundedold, roundednew))]
        summary["ee_abi"] = abi
        summary["ee_corpus_calculated_totals"] = totals
        print(f"EE ABI verified; corpus requested {totals[0]} -> {totals[1]}, minimum rounded {totals[2]} -> {totals[3]}")
    (out / "results.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"PASS {len(fixtures)} patches, 336 composites, exact name/namespace/add-on cache behavior")
    print(f"Host patch requested {baseline['patches'][2]} -> {current['patches'][2]}, rounded blocks {baseline['patches'][3]} -> {current['patches'][3]}")
    print(f"Name cache stride {baseline['names'][2]} -> {current['names'][2]}, 1200-entry zone {baseline['names'][3]} -> {current['names'][3]}")


if __name__ == "__main__":
    main()
