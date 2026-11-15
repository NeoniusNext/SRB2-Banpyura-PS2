"""Compare frozen/current production GS free-list and CLUT builders, or build an EE benchmark."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
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
    hashes = {}
    current = ROOT / "src/ps2/hw"
    priv = (current / "ps2_hw_priv.inc").read_text()
    draw = (current / "ps2_hw_draw.inc").read_text()
    tex = (current / "ps2_hw_tex.inc").read_text()
    common = [section(priv, "#define GSR_PRIM", "// DMA channel 2"),
              section(priv, "typedef union\n{\n\tu64 d[2];", "static inline u32 cyc"),
              section(priv, "#define HWGIF_TAG", "#define PS2HWD_MAXPOLY"),
              section(priv, "#define NBUF", "#define NSCR"),
              section(priv, "#define CLUT_FIXED", "// OPT8 (PS2-HW-24)"),
              (current / "ps2_hw_light.inc").read_text(),
              section(draw, "// light ramp / fog specification", "enum { LM_NONE"),
              section(draw, "static const float ramp_zmax", "static inline float zfrag_of_w"),
              section(draw, "static u32 spec_hash", "// CLUT image of a ramp"),
              section(tex, "enum\n{\n\tCK_OPAQUE", "// the 256 entries of a CLUT"),
              section((current / "ps2_hw_regs.inc").read_text(), "static inline void ad_set", "// FRAME_1")]
    (work / "common.inc").write_text("\n".join(common))
    names = ["H", "V", "OV", "pk_alloc", "pk_flush", "ad_alloc", "vu_chunk_flush", "ov_flush_all", "ov_flush_pending",
             "vram_alloc", "vram_alloc_hi", "vram_free", "a8_to_gs", "out_pal", "pal_light_entries", "clut_slot",
             "clut_alloc", "clut_write", "clut_swizzle", "clut_entries", "clut_palette_dependent", "clut_build_into",
             "clut_fixed_blk", "clut_fixed_refresh", "clut_hint", "clut_get", "ramp_clut"]
    for tag, directory in [("base", base / "ps2/hw"), ("cur", current)]:
        sources = {name: (directory / name).read_text() for name in
                   ("ps2_hw_priv.inc", "ps2_hw_tex.inc", "ps2_hw_pal.inc", "ps2_hw_gs.inc", "ps2_hw_draw.inc")}
        hashes[tag] = {name: hashlib.sha256(text.encode()).hexdigest() for name, text in sources.items()}
        p, t, pal, gs, d = (sources[name] for name in
                            ("ps2_hw_priv.inc", "ps2_hw_tex.inc", "ps2_hw_pal.inc", "ps2_hw_gs.inc", "ps2_hw_draw.inc"))
        if tag == "base":
            for name in ("ps2_hw_light.inc", "ps2_hw_regs.inc"):
                if (directory / name).read_bytes() != (current / name).read_bytes():
                    raise RuntimeError("A shared fixture helper differs from the frozen baseline: " + name)
            for first, after in [("// light ramp / fog specification", "enum { LM_NONE"),
                                 ("static const float ramp_zmax", "static inline float zfrag_of_w"),
                                 ("static u32 spec_hash", "// CLUT image of a ramp")]:
                if section(d, first, after) != section(draw, first, after):
                    raise RuntimeError("A shared ramp helper differs from the frozen baseline: " + first)
        fields = re.search(r"\tu(?:16|32) free_start\[VRAM_RANGES\], free_len\[VRAM_RANGES\];", p)[0]
        if tag == "cur" and negative == 2:
            fields = fields.replace("u16", "u8")
        (work / (tag + "-ranges.inc")).write_text(fields + "\n")
        body = "\n".join([
            section(gs, "static qw_t *pk_alloc(u32 n)", "// OPT9 (PS2-HW-32): the packet continues"),
            section(gs, "static qw_t *ad_alloc(u32 n)", "// Wait until the GS"),
            section(t, "static u32 vram_alloc(u32 n", "// ---- footprint"),
            section(t, "static inline u32 a8_to_gs", "static inline u32 pal_hash"),
            section(pal, "static inline const u32 *out_pal", "static inline int lt_ok"),
            pal[pal.index("static void pal_light_entries"):],
            section(t, "static inline u32 clut_slot", "enum\n{\n\tCK_OPAQUE"),
            section(t, "static void clut_entries", "// ---- upload of one texture"),
            section(d, "static u32 ramp_clut", "static void ov_flush(void)\n{"),
        ])
        if tag == "cur":
            if negative == 1:
                body = body.replace("entries[i + j + 8] = t;", "entries[i + j + 8] = t ^ 1u;")
            elif negative == 3:
                body = body.replace("return d[0].w;", "return d[1].w;")
            elif negative == 4:
                body = body.replace("ad_set(&d[65], 0, GSR_TEXFLUSH);", "ad_set(&d[65], 0, GSR_TRXDIR);")
        for name in sorted(names, key=len, reverse=True):
            body = re.sub(r"\b" + name + r"\b", tag + "_" + name, body)
        (work / (tag + "-production.inc")).write_text(body)
    (work / "source-sha256.json").write_text(json.dumps(hashes, indent=2) + "\n")


def run(command, log, expected=0):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, text=True, capture_output=True)
    output = result.stdout + result.stderr
    log.write_text(" ".join(map(str, command)) + "\n" + output)
    if result.returncode != expected or (expected == 0 and "runtime error:" in output):
        print(output)
        raise SystemExit("Unexpected result; see " + str(log))
    return output


def compile_host(work, args, sanitize=False):
    binary = work / ("test-sanitize" if sanitize else "test")
    flags = ["-std=gnu17", "-O2", "-ffp-contract=off", "-fwrapv", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function"]
    if sanitize:
        flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    run([args.cc, *flags, "-I" + str(work), TOOLS / "gs_next2_hosttest.c", "-o", binary, "-lm"],
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
        obj = work / "gs_next2.o"
        flags = [f for f in B.CFLAGS if f not in ("-MMD", "-MP")]
        run([B.CC, *flags, "-ffp-contract=off", "-Werror", "-Wno-unused-function", "-DGS_NEXT2_EE", "-I" + str(work),
             *B.INCS, "-c", TOOLS / "gs_next2_hosttest.c", "-o", obj], work / "ee-build.log")
        binary = work / "GS_NEXT2.ELF"
        run([B.CC, *B.LDFLAGS, obj, "-o", binary, "-ldebug", "-lpatches", "-lm"], work / "ee-link.log")
        (work / "ee-build-report.json").write_text(json.dumps({
            "elf_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
            "elf_bytes": binary.stat().st_size,
            "compiler": str(B.CC), "flags": flags,
            "fixture_sha256": hashlib.sha256((TOOLS / "gs_next2_hosttest.c").read_bytes()).hexdigest(),
            "source_sha256": json.loads((work / "source-sha256.json").read_text()),
        }, indent=2) + "\n")
        print("Built " + str(binary) + "; run_pcsx2.py --until 'GS NEXT2 DONE'")


if __name__ == "__main__":
    main()
