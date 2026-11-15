"""Compare iteration-3 GS texture conversion production functions with the frozen cumulative baseline."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools/ps2"


def section(source, first, after):
    if source.count(first) != 1:
        raise RuntimeError("Missing or ambiguous production anchor: " + first)
    start = source.index(first)
    return source[start:source.index(after, start)]


def prepare(work, base, negative=0):
    work.mkdir(parents=True, exist_ok=True)
    current = ROOT / "src/ps2/hw"
    priv = (current / "ps2_hw_priv.inc").read_text()
    tex = (current / "ps2_hw_tex.inc").read_text()
    common = [section(priv, "#define GSR_PRIM", "// DMA channel 2"),
              section(priv, "typedef union\n{\n\tu64 d[2];", "static inline u32 cyc"),
              section(priv, "#define HWGIF_TAG", "#define PS2HWD_MAXPOLY"),
              section(priv, "#define NBUF", "#define NSCR"),
              section(tex, "static const u8 blk_32_8", "// ---- record table"),
              section(tex, "typedef struct\n{\n\tconst u8 *src;", "// 8-bit index data"),
              section((current / "ps2_hw_regs.inc").read_text(), "static inline void ad_set", "// FRAME_1")]
    (work / "common.inc").write_text("\n".join(common))
    hashes = {}
    names = ["H", "V", "pk_alloc", "pk_flush", "ad_alloc", "vu_chunk_flush", "pal_key", "pal_val", "pal_white",
             "a8_to_gs", "pal_hash", "pal_lookup", "pal_build", "rgba_to_indices", "fill_idx", "fill_ap88",
             "fill_ap88_ct32", "fill_ct32", "fill_ia88", "upload_bands", "vram_guard"]
    for tag, directory in [("base", base / "ps2/hw"), ("cur", current)]:
        t = (directory / "ps2_hw_tex.inc").read_text()
        gs = (directory / "ps2_hw_gs.inc").read_text()
        hashes[tag] = {name: hashlib.sha256((directory / name).read_bytes()).hexdigest()
                       for name in ("ps2_hw_tex.inc", "ps2_hw_priv.inc", "ps2_hw_regs.inc", "ps2_hw_gs.inc")}
        for first, after in [("static const u8 blk_32_8", "// ---- record table"),
                             ("typedef struct\n{\n\tconst u8 *src;", "// 8-bit index data")]:
            if section(t, first, after) != section(tex, first, after):
                raise RuntimeError("Shared unchanged production code differs: " + first)
        body = "\n".join([
            section(gs, "static qw_t *pk_alloc(u32 n)", "// OPT9 (PS2-HW-32): the packet continues"),
            section(gs, "static qw_t *ad_alloc(u32 n)", "// Wait until the GS"),
            section(t, "static u32 pal_key", "static inline u32 clut_slot"),
            section(t, "static int rgba_to_indices", "// write rows [r0"),
            section(t, "// write rows [r0", "// ---- texture data cache"),
            section(t, "static void fill_idx", "// why the last tex_upload failed")])
        if tag == "cur":
            if negative == 1:
                body = body.replace("return (a + 1u) >> 1;", "return a >> 1;")
            elif negative == 2:
                body = body.replace("(t & 0xFFFFFF) | (a8_to_gs", "(t & 0xFFFFFE) | (a8_to_gs")
            elif negative == 3:
                body = body.replace("ad_set(p, 0, GSR_TEXFLUSH);", "ad_set(p, 0, GSR_TRXDIR);")
            elif negative == 4:
                body = body.replace("c->src + (size_t)src_row(c, row) * c->sw * 2", "c->src + (size_t)src_row(c, row) * c->sw * 2 + 2")
        for name in sorted(names, key=len, reverse=True):
            body = re.sub(r"\b" + name + r"\b", tag + "_" + name, body)
        (work / (tag + "-production.inc")).write_text(body)
    (work / "source-sha256.json").write_text(json.dumps(hashes, indent=2) + "\n")
    palette = ROOT / "build/nov14-next3/gs-playpal.bin"
    if not palette.exists():
        sys.path.insert(0, str(TOOLS))
        from verify_pack import decode
        with (ROOT / "build/pak/SRB2.PAK").open("rb") as stream:
            h = struct.unpack("<4s11I", stream.read(48))
            stream.seek(h[5]); entries = [struct.unpack("<6I", stream.read(24)) for _ in range(h[4])]
            stream.seek(h[6]); pool = stream.read(h[7])
            for pos, disk, size, full, _, codec in entries:
                if pool[full:pool.index(b"\0", full)].lower() == b"playpal":
                    palette.parent.mkdir(parents=True, exist_ok=True)
                    palette.write_bytes(decode(stream, pos, disk, size, codec, h[10]))
                    break
    data = palette.read_bytes()
    if len(data) % 768:
        raise RuntimeError("Malformed PLAYPAL")
    rows = ["static const u32 real_palettes[][256] = {"]
    for off in range(0, len(data), 768):
        colors = [data[i] | (data[i + 1] << 8) | (data[i + 2] << 16) for i in range(off, off + 768, 3)]
        rows.append("{" + ",".join(hex(c) + "u" for c in colors) + "},")
    rows.append("};")
    (work / "palettes.inc").write_text("\n".join(rows) + "\n")
    (work / "corpus-sha256.json").write_text(json.dumps({"PLAYPAL": hashlib.sha256(data).hexdigest(),
        "palette_count": len(data) // 768}, indent=2) + "\n")


def run(command, log, expected=0):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, text=True, capture_output=True)
    output = result.stdout + result.stderr
    log.write_text(" ".join(map(str, command)) + "\n" + output)
    if result.returncode != expected or "runtime error:" in output:
        print(output)
        raise SystemExit("Unexpected result; see " + str(log))
    return output


def compile_host(work, args, sanitize=False):
    binary = work / ("test-sanitize" if sanitize else "test")
    flags = ["-std=gnu17", "-O2", "-fwrapv", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function"]
    if sanitize:
        flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    run([args.cc, *flags, "-I" + str(work), TOOLS / "gs_next3_hosttest.c", "-o", binary],
        work / (binary.name + "-build.log"))
    return binary


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--base-src", type=Path, required=True)
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    ap.add_argument("--sanitize", action="store_true")
    ap.add_argument("--negative-controls", action="store_true")
    ap.add_argument("--ee-build", action="store_true")
    ap.add_argument("--ee-only", action="store_true")
    args = ap.parse_args()
    work = args.out.resolve()
    prepare(work, args.base_src.resolve())
    if not args.ee_only:
        binary = compile_host(work, args)
        print(run([binary], work / "test.log").strip())
        if args.sanitize:
            binary = compile_host(work, args, True)
            print(run([binary], work / "test-sanitize.log").strip())
        if args.negative_controls:
            for negative in range(1, 5):
                subdir = work / ("negative-" + str(negative))
                prepare(subdir, args.base_src.resolve(), negative)
                binary = compile_host(subdir, args)
                output = run([binary], subdir / "test.log", expected=1)
                if "FAIL" not in output:
                    raise SystemExit("Negative control did not report a mismatch")
                print("negative=" + str(negative) + " detected: " + output.strip().splitlines()[-1])
    if args.ee_build or args.ee_only:
        sys.path.insert(0, str(TOOLS))
        import build as B
        obj = work / "gs_next3.o"
        flags = [f for f in B.CFLAGS if f not in ("-MMD", "-MP")]
        run([B.CC, *flags, "-Werror", "-Wno-unused-function", "-DGS_NEXT3_EE", "-I" + str(work),
             *B.INCS, "-c", TOOLS / "gs_next3_hosttest.c", "-o", obj], work / "ee-build.log")
        binary = work / "GS_NEXT3.ELF"
        run([B.CC, *B.LDFLAGS, obj, "-o", binary, "-ldebug", "-lpatches", "-lm"], work / "ee-link.log")
        run([B.CC.parent / "mips64r5900el-ps2-elf-objdump", "-d", binary], work / "ee-disassembly.txt")
        (work / "ee-build-report.json").write_text(json.dumps({
            "elf_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(), "elf_bytes": binary.stat().st_size,
            "compiler": str(B.CC), "flags": flags,
            "fixture_sha256": hashlib.sha256((TOOLS / "gs_next3_hosttest.c").read_bytes()).hexdigest(),
            "source_sha256": json.loads((work / "source-sha256.json").read_text()),
        }, indent=2) + "\n")
        print("Built " + str(binary) + "; run_pcsx2.py --until 'GS NEXT3 DONE'")


if __name__ == "__main__":
    main()
