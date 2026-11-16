#!/usr/bin/env python3
"""Run full actual cache lifetimes with eager and sparse stable metadata."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess

import next13_flats_cut_hosttest as old
from next16_mipmap_storage_hosttest import materialize

ROOT = Path(__file__).resolve().parents[2]
HEADER = "src/hardware/hw_data.h"
CACHE = "src/hardware/hw_cache.c"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, log=None, check=True):
    r = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True)
    if log:
        log.write_text(" ".join(map(str, command)) + "\n" + r.stdout + r.stderr)
    if check and r.returncode:
        raise RuntimeError("Command failed: " + str(command) + "\n" + (r.stdout + r.stderr)[-4000:])
    return r


def extract(root):
    fixture = (ROOT / "tools/ps2/next16_texture_pages_hosttest.c").read_text()
    preamble, rest = fixture.split("/* DECLARATIONS */")
    services, suffix = rest.split("/* FUNCTIONS */")
    text = (root / CACHE).read_text()
    data = (root / HEADER).read_text()
    defs = (root / "src/hardware/hw_defs.h").read_text()
    args = (root / "src/m_argv.c").read_text()
    body = preamble + old.section(data, "typedef enum GLTextureFormat_e", "// Patch information for the hardware renderer.")
    body += old.section(defs, "enum ETextureFlags", "struct FTextureInfo")
    body += "\nINT32 myargc;char **myargv;static INT32 found;\n"
    for name in ("INT32 M_CheckParm(", "boolean M_IsNextParm(", "const char *M_GetNextParm("):
        body += old.function(args, name)
    marker = "#if defined(PS2) && defined(PS2_PROFILE) && !defined(PS2_NOOPT_TEXTURE_PAGES)"
    body += old.section(text, marker if marker in text else "static size_t gl_numtextures = 0;", "boolean gl_maptexturesloaded = false;")
    body += "boolean gl_maptexturesloaded=false;\n" + services
    body += old.section(text, "void HWR_InitMapTextures(void)", "// --------------------------------------------------------------------------\n// Make sure texture is downloaded")
    for name in ("UINT32 HWR_PS2_TexTransparent(", "UINT32 HWR_PS2_SideTexWord(", "GLMapTexture_t *HWR_GetTexture(", "static void MakeLevelFlatMipmap(", "void HWR_GetLevelFlat(", "void HWR_PS2_RegenerateMipmap(", "void *HWR_PS2_StealData("):
        body += old.function(text, name)
    return body + suffix


def target_layout(root, compiler, role):
    sdk = Path(os.environ["PS2SDK"])
    cfile = root / "actual-header-layout.c"
    cfile.write_text('#include <stddef.h>\n#include "' + str(root / HEADER) + '"\n'
        '_Static_assert(sizeof(void*)==4&&sizeof(size_t)==4,"EE pointers and sizes");\n'
        '_Static_assert(offsetof(GLMipmap_t,data)==4,"pixel owner offset retained");\n'
        '__attribute__((used,section(".mipmap_layout"))) const unsigned int layout[]={sizeof(GLMipmap_t),sizeof(GLMapTexture_t),_Alignof(GLMipmap_t),offsetof(GLMipmap_t,data),sizeof(((GLMipmap_t*)0)->regen_id),sizeof(((GLMipmap_t*)0)->downloaded),sizeof(((GLMipmap_t*)0)->ps2_planfr),sizeof(((GLMipmap_t*)0)->ps2_pi),sizeof(((GLMipmap_t*)0)->ps2_uw)};\n')
    obj = cfile.with_suffix(".o")
    run([compiler, "-std=gnu23", "-O2", "-D_EE", "-DPS2", "-DPS2_PROFILE", "-DHWRENDER", "-DCMAKECONFIG", "-I" + str(ROOT / "src"), "-I" + str(ROOT / "src/hardware"), "-I" + str(ROOT / "src/ps2"), "-I" + str(sdk / "ee/include"), "-I" + str(sdk / "common/include"), "-c", cfile, "-o", obj], root / "actual-header-layout.log")
    binary = root / "actual-header-layout.bin"
    objcopy = compiler.with_name(compiler.name.replace("gcc", "objcopy"))
    run([objcopy, "--dump-section", ".mipmap_layout=" + str(binary), obj])
    values = struct.unpack("<9I", binary.read_bytes())
    expected = (76,84,4,4,4,4,4,2,2) if role == "baseline" else (68,76,4,4,4,4,4,2,2)
    if values != expected:
        raise RuntimeError("Unexpected actual target layout: " + str(values))
    return {"values": values, "object_sha256": sha(obj), "source_sha256": sha(cfile)}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--reference", default="604cfd150")
    ap.add_argument("--candidate", type=Path, default=ROOT)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--negative-controls", action="store_true")
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    roots = {role: out / role for role in ("baseline", "candidate")}
    for role, root in roots.items():
        materialize(root, args.reference if role == "baseline" else None, args.candidate)
    flags = ["gcc", "-DPS2_NOOPT_PATCH_RESCUE", "-std=gnu17", "-O2", "-g", "-fwrapv", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter", "-Wno-misleading-indentation", "-Wno-clobbered", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
    frozen = out / "frozen-tools"
    frozen.mkdir()
    dependencies = {}
    for name in (Path(__file__).name, "next16_texture_pages_hosttest.c", "next16_mipmap_storage_hosttest.py", "next13_flats_cut_hosttest.py", "next13_flats_cut_variant.py", "next13_flats_variant.py", "hw_hosttest.py", "texture_record_hosttest.py"):
        path = ROOT / "tools/ps2" / name
        shutil.copy2(path, frozen / name)
        dependencies[name] = sha(path)
    report = {"patch_rescue": "disabled for independent storage/page scope; patch_rescue_hosttest.py proves rescue separately", "reference": run(["git", "rev-parse", args.reference]).stdout.strip(), "runner_sha256": sha(Path(__file__)), "fixture_sha256": sha(ROOT / "tools/ps2/next16_texture_pages_hosttest.c"), "frozen_tools_sha256": dependencies, "sources": {}, "actual_EE_layout": {}, "cases": [], "negative_controls": {}}
    bodies = {role: extract(root) for role, root in roots.items()}
    configs = {"ps2": ["-DPS2", "-DPS2_PROFILE"], "eager-optout": ["-DPS2", "-DPS2_PROFILE", "-DPS2_NOOPT_TEXTURE_PAGES"], "baseline-storage": ["-DPS2", "-DPS2_PROFILE", "-DPS2_NOOPT_MIPMAP_STORAGE"], "both-optouts": ["-DPS2", "-DPS2_PROFILE", "-DPS2_NOOPT_TEXTURE_PAGES", "-DPS2_NOOPT_MIPMAP_STORAGE"], "old-layout": ["-DPS2", "-DPS2_PROFILE", "-DPS2_NOOPT_MIPMAP_LAYOUT"], "full-flat-records": ["-DPS2", "-DPS2_PROFILE", "-DPS2_NOOPT_FLAT_METADATA"], "nonps2-profile": ["-DPS2_PROFILE"]}
    compiler = shutil.which("mips64r5900el-ps2-elf-gcc")
    for role, root in roots.items():
        report["sources"][role] = {str(p.relative_to(root)): sha(p) for p in sorted((root / "src").rglob("*")) if p.is_file()}
        if compiler:
            report["actual_EE_layout"][role] = target_layout(root, Path(compiler), role)
        for name, defines in configs.items():
            source = root / (name + ".c")
            source.write_text(bodies[role])
            run([*flags, *defines, source, "-o", root / name], root / (name + "-build.log"))
    for name in configs:
        cases = [(scenario, n, argv) for scenario in ("normal", "unused", "fail-first", "fail-second", "fail-variant", "full") for n in (0,1,15,16,17,31,32,33,129,4740) for argv in ([],["-hwgo","65600"])]
        cases += [("normal",65537,[])]
        for scenario, n, argv in cases:
            # Full population with 4740 records is intentional. All logical
            # records must remain available even when sparse storage is full.
            values = {role: json.loads(run([root / name, scenario, n, *argv]).stdout) for role, root in roots.items()}
            for key in ("hash", "events", "errors"):
                if values["baseline"][key] != values["candidate"][key]:
                    raise RuntimeError(str((name, scenario, n, argv, key, values)))
            report["cases"].append({"profile": name, "scenario": scenario, "N": n, "argv": argv, **values})
        if name not in ("eager-optout", "both-optouts", "nonps2-profile"):
            result = json.loads(run([roots["candidate"] / name, "fail-page",129]).stdout)
            if result["errors"] != 2:
                raise RuntimeError("Both page allocation failures must be exercised")
            report["cases"].append({"profile": name, "scenario": "fail-page", "N": 129, "candidate": result})
    if args.negative_controls:
        mutations = {"wrong-wall-page": ("&records[n % HWR_TEXTURE_PAGE_SIZE]", "&records[0]"), "wrong-flat-page": ("(n % HWR_TEXTURE_PAGE_SIZE) * stride", "0 * stride"), "wrong-regen-record": ("HWR_MapTexture((size_t)m->regen_id, true)", "HWR_MapTexture(0, true)"), "missing-flat-twin": ("newMipmap->ps2_twin = originalMipmap; // PS2-HW-30", "newMipmap->ps2_twin = NULL; // PS2-HW-30"), "early-page-free": ("if (wall)\n\t\t\tFreeMapTexture(wall, true);", "if (wall) { free(gl_textures[i / HWR_TEXTURE_PAGE_SIZE]); FreeMapTexture(wall, true); }")}
        for name, (before, after) in mutations.items():
            body = bodies["candidate"]
            if before not in body:
                raise RuntimeError("Missing mutation anchor " + name)
            source = out / ("negative-" + name + ".c")
            source.write_text(body.replace(before,after,1))
            binary = source.with_suffix("")
            run([*flags,"-DPS2","-DPS2_PROFILE",source,"-o",binary], source.with_suffix(".build.log"))
            result = run([binary,"normal",129],source.with_suffix(".log"),check=False)
            caught = result.returncode != 0
            if not caught:
                control = json.loads(run([roots["baseline"] / "ps2","normal",129]).stdout)
                current = json.loads(result.stdout)
                caught = any(control[k] != current[k] for k in ("hash","events","errors"))
            if not caught:
                raise RuntimeError("Undetected negative control: " + name)
            report["negative_controls"][name] = {"detected":True,"exit":result.returncode,"log_sha256":sha(source.with_suffix(".log"))}
    (out / "report.json").write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps({"out":str(out),"comparisons":len(report["cases"]),"negative_controls":len(report["negative_controls"]),"actual_EE_layout":report["actual_EE_layout"]}))


if __name__ == "__main__":
    main()
