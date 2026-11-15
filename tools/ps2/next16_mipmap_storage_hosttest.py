#!/usr/bin/env python3
"""Compare actual mipmap layouts through engine lifetimes and texture uploads."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

import hw_hosttest
import next13_flats_cut_hosttest as flats
import texture_record_hosttest as texture

ROOT = Path(__file__).resolve().parents[2]
HEADER = "src/hardware/hw_data.h"
SCALARS = "format flags height width downloaded regen_id ps2_planfr ps2_ihfr ps2_imfr ps2_full_fr ps2_gcv ps2_spv ps2_pi ps2_uw ps2_uh regen_kind ps2_h255 ps2_want ps2_ihint ps2_vis ps2_nup ps2_drop ps2_cost".split()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, log=None, check=True):
    r = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True)
    if log:
        log.write_text(" ".join(map(str, command)) + "\n" + r.stdout + r.stderr)
    if check and r.returncode:
        raise RuntimeError("Command failed: " + str(command) + "\n" + (r.stdout + r.stderr)[-4000:])
    return r


def materialize(work, reference, candidate=ROOT):
    for path in (HEADER, "src/hardware/hw_cache.c", "src/hardware/hw_defs.h", "src/m_argv.c"):
        dest = work / path
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(subprocess.check_output(["git", "show", reference + ":" + path], cwd=ROOT) if reference else (candidate / path).read_bytes())


def normalize_fixture(source):
    # Normalize fields by value instead of hashing representation/padding, so
    # both executions retain the same independent service/owner trace oracle.
    old = "event(60,id,0);hash_bytes(&copy,sizeof copy);"
    names = ["format", "flags", "height", "width", "downloaded"]
    ps2 = [x for x in SCALARS if x not in names]
    def values(fields):
        return "".join("{UINT32 value=(UINT32)copy." + name + ";hash_bytes(&value,sizeof value);}" for name in fields)
    new = "event(60,id,0);" + values(names)
    new += "{uintptr_t value=(uintptr_t)copy.data;hash_bytes(&value,sizeof value);}"
    new += "{uintptr_t value=(uintptr_t)copy.nextcolormap;hash_bytes(&value,sizeof value);}"
    new += "{uintptr_t value=(uintptr_t)copy.colormap;hash_bytes(&value,sizeof value);}\n#ifdef PS2_PROFILE\n"
    new += values(ps2)
    new += "{uintptr_t value=(uintptr_t)copy.ps2_twin;hash_bytes(&value,sizeof value);}\n#endif\n"
    if source.count(old) != 1:
        raise RuntimeError("Fixture normalization anchor changed")
    return source.replace(old, new)


def field_fixture(header):
    types = "#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\ntypedef uint8_t UINT8;typedef uint16_t UINT16;typedef uint32_t UINT32;typedef int32_t INT32;\n"
    declarations = flats.section(header, "typedef enum GLTextureFormat_e", "// Patch information for the hardware renderer.")
    return types + declarations + r'''
static unsigned checks;static UINT32 hash=2166136261u,rng=0x9e3779b9u;
static UINT32 random_word(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static void save(UINT32 v){hash=(hash^v)*16777619u;}
#define FIELD(field,value) do{UINT32 expected=(UINT32)(value);m.field=expected;if((UINT32)m.field!=expected){fprintf(stderr,"field %s lost %u\n",#field,expected);return 1;}checks++;save((UINT32)m.field);}while(0)
#define VERIFY(field,value) do{if((UINT32)m.field!=(UINT32)(value)){fprintf(stderr,"neighbor %s changed\n",#field);return 1;}checks++;save((UINT32)m.field);}while(0)
int main(void){
 const UINT32 formats[]={0,GL_TEXFMT_P_8,GL_TEXFMT_AP_88,GL_TEXFMT_RGBA,GL_TEXFMT_ALPHA_8,GL_TEXFMT_INTENSITY_8,GL_TEXFMT_ALPHA_INTENSITY_88};
 GLMipmap_t m;memset(&m,0,sizeof m);
 for(unsigned i=0;i<100000;i++){
  UINT32 kind=i%3,h255=(i/3)%3,vis=(i/9)%2,drop=(i/18)%6;
  FIELD(regen_kind,kind);FIELD(ps2_h255,h255);FIELD(ps2_vis,vis);FIELD(ps2_drop,drop);
  FIELD(format,formats[i%7]);FIELD(flags,((i&1)?1u:0u)|((i&2)?2u:0u)|((i&4)?16u:0u)|((i&8)?64u:0u));
  FIELD(downloaded,random_word());FIELD(regen_id,random_word());
  FIELD(height,random_word()&65535);FIELD(width,random_word()&65535);
  FIELD(ps2_pi,random_word()&65535);FIELD(ps2_uw,random_word()&65535);FIELD(ps2_uh,random_word()&65535);
  FIELD(ps2_planfr,random_word());FIELD(ps2_ihfr,random_word());FIELD(ps2_imfr,random_word());
  FIELD(ps2_full_fr,random_word());FIELD(ps2_gcv,random_word());FIELD(ps2_spv,random_word());
  FIELD(ps2_want,i&255);FIELD(ps2_ihint,(i+127)&255);FIELD(ps2_nup,(i+255)&255);FIELD(ps2_cost,(i+254)&255);
  m.data=(void*)(uintptr_t)0xfedcba98u;m.nextcolormap=&m;m.colormap=(GLColormap_t*)(uintptr_t)0x87654321u;m.ps2_twin=&m;
  VERIFY(regen_kind,kind);VERIFY(ps2_h255,h255);VERIFY(ps2_vis,vis);VERIFY(ps2_drop,drop);
  if(m.data!=(void*)(uintptr_t)0xfedcba98u||m.nextcolormap!=&m||m.colormap!=(GLColormap_t*)(uintptr_t)0x87654321u||m.ps2_twin!=&m)return 2;
 }
 printf("{\"checks\":%u,\"hash\":%u}\n",checks,hash);return 0;
}
'''


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
    if (roots["baseline"] / "src/hardware/hw_cache.c").read_bytes() != (roots["candidate"] / "src/hardware/hw_cache.c").read_bytes():
        raise RuntimeError("Isolated layout suite requires baseline hw_cache.c; use --candidate with baseline cache code and the final header. The separate page and rescue suites test final cache code.")
    flags = ["gcc", "-DPS2_NOOPT_PATCH_RESCUE", "-std=gnu17", "-O2", "-g", "-fwrapv", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter", "-Wno-misleading-indentation", "-Wno-clobbered", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
    frozen = out / "frozen-tools"
    frozen.mkdir()
    dependencies = {}
    for name in (Path(__file__).name, "next13_flats_cut_hosttest.py", "next13_flats_cut_hosttest.c", "next13_flats_host_prelude.h", "next13_flats_cut_variant.py", "next13_flats_variant.py", "hw_hosttest.py", "texture_record_hosttest.py", "texture_record_hosttest.c"):
        path = ROOT / "tools/ps2" / name
        shutil.copy2(path, frozen / name)
        dependencies[name] = sha(path)
    report = {"patch_rescue": "disabled for independent storage/page scope; patch_rescue_hosttest.py proves rescue separately", "reference": run(["git", "rev-parse", args.reference]).stdout.strip(), "runner_sha256": sha(Path(__file__)), "frozen_tools_sha256": dependencies, "sanitizers": "address,undefined", "sources": {}, "fields": {}, "lifetimes": {}, "textures": {}, "negative_controls": {}}
    for role, root in roots.items():
        report["sources"][role] = {str(p.relative_to(root)): sha(p) for p in sorted((root / "src").rglob("*")) if p.is_file()}
        header = (root / HEADER).read_text()
        fields = root / "fields.c"
        fields.write_text(field_fixture(header))
        run([*flags, "-DPS2", "-DPS2_PROFILE", fields, "-o", root / "fields"], root / "fields-build.log")
        report["fields"][role] = json.loads(run([root / "fields"], root / "fields.log").stdout)
    if report["fields"]["baseline"] != report["fields"]["candidate"]:
        raise RuntimeError("Field snapshots differ")
    configs = {"ps2": ["-DPS2", "-DPS2_PROFILE"], "optout": ["-DPS2", "-DPS2_PROFILE", "-DPS2_NOOPT_MIPMAP_LAYOUT"], "storage-optout": ["-DPS2", "-DPS2_PROFILE", "-DPS2_NOOPT_MIPMAP_STORAGE"], "nonps2-profile": ["-DPS2_PROFILE"], "generic": [], "ps2-noprofile": ["-DPS2"]}
    prelude = (ROOT / "tools/ps2/next13_flats_host_prelude.h").read_text()
    for name, defines in configs.items():
        for role, root in roots.items():
            body = normalize_fixture(flats.extract(root))
            cfile = root / (name + "-lifetimes.c")
            cfile.write_text(prelude + "\n#define FLAT_CANDIDATE 1\n" + body)
            run([*flags, *defines, cfile, "-o", root / (name + "-lifetimes")], cfile.with_suffix(".log"))
        cases = []
        for scenario in flats.SCENARIOS:
            arguments = [[], ["-hwgo", "65536"], ["-hwgo", "65600"]]
            for argv in arguments:
                values = {role: json.loads(run([root / (name + "-lifetimes"), scenario, *argv]).stdout) for role, root in roots.items()}
                baseline, candidate = values["baseline"], values["candidate"]
                for key in baseline:
                    if key not in ("wall_bytes", "flat_bytes") and baseline[key] != candidate[key]:
                        raise RuntimeError(str((name, scenario, argv, key, values)))
                for key in ("wall_bytes", "flat_bytes"):
                    # Host pointers are 64-bit; actual EE table savings are
                    # measured by the separate target ABI probe, not this model.
                    if name != "ps2" and baseline[key] != candidate[key]:
                        raise RuntimeError("Inactive layout request changed")
                cases.append({"scenario": scenario, "argv": argv, **values})
        report["lifetimes"][name] = cases
    # Compile the complete pinned texture manager twice, changing only the
    # actual mipmap declaration. Source bytes stay frozen while other workers
    # investigate the current driver.
    for role, root in roots.items():
        work = root / "texture"
        sources = texture.prepare(work, args.reference)
        current = (ROOT / HEADER).read_text()
        header = (root / HEADER).read_text()
        shim = (work / "shim.h").read_text()
        shim = shim.replace(hw_hosttest.section(current, "typedef enum GLTextureFormat_e", "typedef struct GLMipmap_s GLMipmap_t;"), hw_hosttest.section(header, "typedef enum GLTextureFormat_e", "typedef struct GLMipmap_s GLMipmap_t;"))
        (work / "shim.h").write_text(shim)
        binary = work / "sanitize"
        run([*flags, "-DPS2", "-DPS2_PROFILE", "-Wno-unused-but-set-variable", "-I" + str(work), ROOT / "tools/ps2/texture_record_hosttest.c", "-o", binary, "-lm"], work / "sanitize-build.log")
        result = run([binary, work / "snapshot.bin"], work / "sanitize.log")
        report["textures"][role] = {"sources": sources, "summary": json.loads(result.stdout), "snapshot_sha256": sha(work / "snapshot.bin")}
    if report["textures"]["baseline"] != report["textures"]["candidate"]:
        raise RuntimeError("Complete texture snapshots differ")
    if args.negative_controls:
        mutations = {"format-range": ("UINT8                 format;", "UINT8                 format : 5;"), "flags-range": ("UINT8                 flags;", "UINT8                 flags : 6;"), "kind-range": ("regen_kind : 2", "regen_kind : 1"), "h255-range": ("ps2_h255 : 2", "ps2_h255 : 1"), "drop-range": ("ps2_drop : 3", "ps2_drop : 2"), "full-index": ("UINT16                ps2_pi;\n\tUINT16                ps2_uw, ps2_uh;", "UINT8                 ps2_pi;\n\tUINT16                ps2_uw, ps2_uh;")}
        for name, (old, new) in mutations.items():
            header = (roots["candidate"] / HEADER).read_text()
            if header.count(old) != 1:
                raise RuntimeError("Negative-control anchor changed: " + name)
            source = out / ("negative-" + name + ".c")
            source.write_text(field_fixture(header.replace(old, new, 1)))
            binary = source.with_suffix("")
            run([*flags, "-DPS2", "-DPS2_PROFILE", source, "-o", binary], source.with_suffix(".build.log"))
            result = run([binary], source.with_suffix(".log"), check=False)
            if result.returncode == 0:
                raise RuntimeError("Undetected mutation: " + name)
            report["negative_controls"][name] = {"detected": True, "exit": result.returncode, "output": result.stderr.strip()}
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"out": str(out), "field_checks_per_role": report["fields"]["baseline"]["checks"], "lifetime_comparisons": sum(map(len, report["lifetimes"].values())), "texture_summary": report["textures"]["baseline"]["summary"], "negative_controls": len(report["negative_controls"])}))


if __name__ == "__main__":
    main()
