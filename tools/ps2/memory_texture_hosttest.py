"""Production composite caches in the real zone, including in-place and moving root growth."""
import argparse
import re
import subprocess
from pathlib import Path
from memory_hosttest import ROOT, build_run


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", default=str(ROOT / "build/ps2-memory-continuation/composite-zone"))
    p.add_argument("--negative-controls", action="store_true")
    args = p.parse_args(); out = Path(args.out).resolve()
    if not out.parent.is_dir(): p.error("output parent must already exist")
    out.mkdir(exist_ok=True)
    defs = []
    def emit(name, text):
        dest = out / (name + ".inc"); dest.write_text(text, encoding="utf-8")
        defs.append('/D' + name.upper() + '="' + dest.as_posix() + '"')
    rdefs = (ROOT / "src/r_defs.h").read_text(encoding="utf-8")
    types = rdefs[rdefs.index("// posts are runs of non masked source pixels"):rdefs.index("} column_t;") + len("} column_t;")]
    types += rdefs[rdefs.index("typedef struct\n{\n\tINT16 width, height;"):rdefs.index("// Possible alpha types for a patch.")]
    rtexh = (ROOT / "src/r_textures.h").read_text(encoding="utf-8")
    types += rtexh[rtexh.index("typedef struct\n{\n\t// Block origin"):rtexh.index("// all loaded and prepared textures")]
    emit("memory_texture_types", types)
    patch = (ROOT / "src/r_patch.c").read_text(encoding="utf-8")
    ptext = patch[patch.index("patch_t *Patch_Create("):patch.index("#ifdef PS2_PROFILE")]
    ptext += patch[patch.index("#define PATCH_ALIGN"):patch.index("// The zone may drop an embedded patch")]
    ptext += patch[patch.index("void Patch_CalcDataSizes("):patch.index("//\n// Frees patches with a tag range.")]
    emit("memory_texture_patch", ptext)
    tex = (ROOT / "src/r_textures.c").read_text(encoding="utf-8")
    renderer = tex[tex.index("INT32 numtextures = 0;"):tex.index("// Painfully simple texture id")]
    renderer += tex[tex.index("static void R_DrawColumnInCache("):tex.index("//\n// R_GetTextureNum")]
    renderer += tex[tex.index("void R_CheckTextureCache("):tex.index("INT32 R_GetTextureNumForFlat(")]
    emit("memory_texture_renderer", renderer)
    pic = (ROOT / "src/r_picformats.c").read_text(encoding="utf-8")
    emit("memory_texture_flat", pic[pic.index("void *Picture_TextureToFlat("):pic.index("#ifndef PS2_PROFILE // PS2-20: the profile has no PNG decoder")])
    outputs = []
    for name, flags in [("reference", ["/DPS2_NOOPT_texmask", "/DPS2_NOOPT_texreuse", "/DPS2_NOOPT_texstream", "/DPS2_NOOPT_TEXPOSTS", "/DPS2_NOOPT_TEXPLACE"]),
                        ("previous", ["/DPS2_NOOPT_TEXPOSTS", "/DPS2_NOOPT_TEXPLACE"]),
                        ("counted", ["/DPS2_NOOPT_TEXPLACE"]), ("optimized", [])]:
        outputs.append(build_run(out, name, "memory_texture_hosttest.c", defs + flags))
    rows = [[m.groups() for m in re.finditer(r"texture cooked=(\d+) move=(\d+) hash=(\d+) peak=(\d+) resizes=(\d+) moves=(\d+)", o)] for o in outputs]
    assert all(len(r) == 6 for r in rows)
    for row in zip(*rows): assert all(r[:3] == row[0][:3] for r in row), row
    for old, new in zip(rows[1], rows[2]): assert int(new[3]) <= int(old[3]), (old, new)
    for name, expected in [("reference", 2), ("optimized", 0)]:
        result = subprocess.run([str(out / name / "test.exe"), "24000"], capture_output=True, text=True, timeout=90)
        (out / name / "tight.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        assert result.returncode == expected, result.stdout + result.stderr
    print("PASS exact composite pixel/post stream in real zone, root in-place/move, 24000-byte pressure fallback")
    for name, expected in [("previous", 2), ("optimized", 0)]:
        result = subprocess.run([str(out / name / "test.exe"), "80000", "fragmented"], capture_output=True, text=True, timeout=90)
        (out / name / "fragmented-tight.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        assert result.returncode == expected, result.stdout + result.stderr
    print("PASS fragmented composite fits 80000-byte arena; previous implementation reports OOM")
    if args.negative_controls:
        broken = renderer.replace("post->topdelta = post->data_offset = (size_t)y;", "post->topdelta = (size_t)y + 1; post->data_offset = (size_t)y;")
        assert broken != renderer
        emit("memory_texture_renderer", broken)
        try:
            mutated = build_run(out, "negative-post-offset", "memory_texture_hosttest.c", defs)
        except RuntimeError as error:
            assert "FAIL: post stays inside texture" in str(error), error
        else:
            hashes = re.findall(r" hash=(\d+)", mutated)
            assert hashes != [r[2] for r in rows[3]], "negative control escaped exact comparison"
        print("PASS negative control: corrupted post offsets detected by exact stream comparison")


if __name__ == "__main__": main()
