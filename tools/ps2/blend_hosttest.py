"""Actual-code eager/lazy blend equivalence, no PCSX2 or full engine build.

python tools/ps2/blend_hosttest.py --negative-controls
Source sections are copied verbatim into isolated build output, including their
preprocessor branches. Reference and candidate compile into separate translation
units with separate LUTs. No unresolved-symbol linker override is used.
"""
import argparse
import json
import re
from pathlib import Path
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[2]
VCVARS = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat")


def section(text, start, end):
    if text.count(start) != 1 or text.count(end) != 1:
        raise RuntimeError(f"Source section anchors changed: {start!r} / {end!r}")
    a = text.index(start)
    b = text.index(end, a)
    return text[a:b]


def build(work, name, arch, units):
    """units: (source, object name, defines). Require the parent before mkdir."""
    if not work.parent.is_dir():
        raise RuntimeError(f"Output parent does not exist: {work.parent}")
    work.mkdir(exist_ok=True)
    lines = ["@echo off", f'call "{VCVARS}" {arch} >nul 2>&1', "if errorlevel 1 exit /b 1"]
    objects = []
    for source, obj, defines in units:
        objects.append(obj)
        cmd = ["cl", "/nologo", "/std:c17", "/O2", "/W3", "/DNDEBUG", "/D_CRT_SECURE_NO_WARNINGS",
               "/wd4005", "/I" + str(ROOT / "src"), "/I" + str(ROOT / "tools/ps2"), "/I" + str(work),
               *defines, "/c", str(source), "/Fo:" + obj]
        lines.extend([subprocess.list2cmdline(cmd), "if errorlevel 1 exit /b 1"])
    lines.append(subprocess.list2cmdline(["link", "/nologo", "/OUT:" + name + ".exe", *objects]))
    bat = work / "build.bat"
    bat.write_text("\n".join(lines) + "\n", encoding="utf-8")
    result = subprocess.run(["cmd", "/c", str(bat)], cwd=work, capture_output=True, text=True,
                            encoding="oem", errors="replace")
    (work / "build.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        print(result.stdout + result.stderr)
        raise RuntimeError(f"Build failed: {work}")
    return work / (name + ".exe")


def blend_sections():
    draw = (ROOT / "src/r_draw.c").read_text()
    renderer = section(draw, "#define NUMTRANSTABLES 9", "/**\t\\brief R_DrawTransColumn")
    renderer += section(draw, "/** \\brief Initializes the translucency tables", "// Define for getting accurate color brightness")
    data = (ROOT / "src/r_data.c").read_text()
    support = section(data, "UINT32 ASTBlendPixel(", "INT32 ASTTextureBlendingThreshold")
    support += section(data, "UINT8 NearestPaletteColor(", "#ifdef EXTRACOLORMAPLUMPS\nconst char *R_NameForColormap")
    video = (ROOT / "src/v_video.c").read_text()
    support += section(video, "void InitColorLUT(", "// V_Init\n")
    return renderer, support


def run_case(out, arch, renderer, support, fixture, negative=False, ps2=False, cache_control=''):
    work = out / (("negative-" + cache_control + "-" if cache_control else "negative-old-lazy-" if negative else "blend-ee-" if ps2 else "blend-") + arch)
    work.mkdir(exist_ok=True)
    (work / "blend_renderer.inc").write_text(renderer, encoding="utf-8")
    (work / "blend_support.inc").write_text(support, encoding="utf-8")
    source = ROOT / "tools/ps2/blend_hosttest.c"
    exe = build(work, "blend_hosttest", arch, [(source, "candidate.obj", ["/DPS2"] if ps2 else []),
                (source, "reference.obj", ["/DBLEND_REFERENCE"]), (source, "support.obj", ["/DBLEND_SUPPORT"])])
    tested = subprocess.run([str(exe)], input=fixture, capture_output=True)
    log = (tested.stdout + tested.stderr).decode("ascii", errors="replace")
    (work / "test.log").write_text(log, encoding="utf-8")
    print(f"{work.name}: exit={tested.returncode}\n{log}", end="")
    if cache_control:
        expected = {'trans-touch': 'TRANS current-frame pointer survives pressure',
                    'trans-owner': 'TRANS cache has stable owner'}[cache_control]
        if tested.returncode != 2 or 'FAIL: ' + expected not in log:
            raise RuntimeError(f"TRANS cache negative control did not detect {cache_control}")
    elif negative:
        mismatch = re.search(r"reverse/cold: 31 tables, (\d+) defined-byte differences in (\d+) tables", log)
        if tested.returncode != 1 or not mismatch or int(mismatch[1]) == 0 or int(mismatch[2]) == 0:
            raise RuntimeError("Old lazy negative control did not demonstrate an equivalence mismatch")
    elif tested.returncode:
        raise RuntimeError(f"Blend equivalence failed: {work}")
    return {"case": work.name, "exit": tested.returncode, "negative_control": bool(negative or cache_control)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=ROOT / "build/agent-equivalence-g1")
    ap.add_argument("--negative-controls", action="store_true")
    ap.add_argument("--ps2-alignment", action="store_true", help="also exercise the EE cache-line-aligned table path")
    args = ap.parse_args()
    out = args.out.resolve()
    if not out.parent.is_dir():
        raise SystemExit(f"Output parent does not exist: {out.parent}")
    out.mkdir(exist_ok=True)
    renderer, support = blend_sections()
    with zipfile.ZipFile(ROOT / "srb2-assets/srb2.pk3") as assets:
        def lump(name):
            matches = [n for n in assets.namelist() if Path(n).stem.upper() == name]
            if len(matches) != 1:
                raise RuntimeError((name, matches))
            return assets.read(matches[0])
        palette = lump("PLAYPAL")[:768]
        tables = [lump(f"TRANS{i}0") for i in range(1, 10)]
        if any(len(t) != 65536 for t in tables):
            raise RuntimeError("Unexpected vanilla TRANS table size")
        fixture = palette + b"".join(tables)
    results = [run_case(out, arch, renderer, support, fixture) for arch in ["x86", "x64"]]
    if args.ps2_alignment:
        results.append(run_case(out, "x86", renderer, support, fixture, ps2=True))
    if args.negative_controls:
        start = renderer.index("static void BlendTab_NeedLUT(void)\n{")
        end = renderer.index("\n#endif", start)
        old = ("static void BlendTab_NeedLUT(void)\n{\n"
               "\tif (!transtab_lutp)\n\t\tZ_Calloc(sizeof *transtab_lutp, PU_CACHE, &transtab_lutp);\n"
               "\tInitColorLUT(&transtab_lut, blendpal, false);\n}\n")
        mutant = renderer[:start] + old + renderer[end:]
        # Reference branch is untouched; this recreates the old profile lazy bug.
        results.append(run_case(out, "x64", mutant, support, fixture, True))
        if args.ps2_alignment:
            touch = '\tZ_Touch(transtab_lazy[level]);'
            owner = 'PU_CACHE, &transtab_lazy[level], 6'
            if renderer.count(touch) != 1 or renderer.count(owner) != 1:
                raise RuntimeError('TRANS cache negative-control anchors changed')
            results.append(run_case(out, "x86", renderer.replace(touch, '\t/* missing touch */', 1),
                                    support, fixture, ps2=True, cache_control='trans-touch'))
            results.append(run_case(out, "x86", renderer.replace(owner, 'PU_CACHE, NULL, 6', 1),
                                    support, fixture, ps2=True, cache_control='trans-owner'))
    (out / "blend-results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print("Requested blend host checks passed; old-lazy negative control failed as expected." if args.negative_controls
          else "Requested blend host checks passed.")


if __name__ == "__main__":
    main()
