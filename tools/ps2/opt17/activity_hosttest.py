"""Actual isolated activity counters + HUD format/composition/placement checks.

Uses stock TNYFN widths, simulated engine allocation/draw calls, deterministic
Count events and canaries. No emulator is started. GS-busy utilization is not
claimed by the observed submit-to-FINISH queue-span estimate.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[3]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(source, work, assets):
    work.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location("highres_helpers", Path(__file__).with_name("highres_state_hosttest.py"))
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    for name in ("ps2_activity.c", "ps2_activity.h"):
        shutil.copy2(source / "src/ps2" / name, work / name)
    text = (source / "src/ps2/ps2_memhud.c").read_text()
    types = (source / "src/ps2/ps2_memhud.h").read_text()
    (work / "hud-types.inc").write_text(types[types.index("typedef struct"):types.index("} ps2memhud_t;") + len("} ps2memhud_t;")])
    (work / "hud-format.inc").write_text(text[text.index("static char *put_u("):text.index("static memline_t mem_lines")])
    (work / "hud-globals.inc").write_text(text[text.index("static memline_t mem_lines"):text.index("#define BMP_W")])
    (work / "hud-workspace.inc").write_text(text[text.index("#define BMP_W"):text.index("static tic_t compose_retry;") + len("static tic_t compose_retry;")])
    for tag, names in (("compose", ("blit_glyph", "blit_string", "level_color", "detail_color", "line_w", "compose")),
                       ("refresh", ("refresh",)), ("bottom", ("bottom_edge",)), ("onchange", ("showmem_onchange",)), ("draw", ("PS2MemHud_Draw",))):
        (work / ("hud-" + tag + ".inc")).write_text("\n".join(helper.function(text, name) for name in names))
    widths = {}
    with ZipFile(assets) as archive:
        for name in archive.namelist():
            match = re.search(r"/TNYFN(\d+)$", name)
            if match:
                data = archive.read(name)
                widths[int(match[1])] = struct.unpack_from(">I", data, 16)[0] if data.startswith(b"\x89PNG") else struct.unpack_from("<H", data)[0]
    if not all(char in widths for char in map(ord, "CPU GPU~ 0123456789%/AMBNRfree".replace(" ", ""))):
        raise RuntimeError("stock font lacks activity/memory HUD characters")
    (work / "font-widths.inc").write_text(",".join(str(widths.get(char, 0)) for char in range(33, 128)))
    shutil.copy2(Path(__file__).with_name("activity_hud_fixture.inc"), work / "hud-fixture.inc")
    paths = [source / "src/ps2/ps2_activity.c", source / "src/ps2/ps2_activity.h", source / "src/ps2/ps2_memhud.c", source / "src/ps2/ps2_memhud.h", assets,
             Path(__file__), Path(__file__).with_suffix(".c"), Path(__file__).with_name("activity_hud_fixture.inc")]
    return {str(path): sha(path) for path in paths}


def execute(command, log):
    result = subprocess.run(list(map(str, command)), text=True, capture_output=True)
    log.write_text(result.stdout + result.stderr)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source-root", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--font-assets", type=Path, default=Path("/workspace/.onboarding/srb2-assets/srb2.pk3"))
    ap.add_argument("--sanitize", action="store_true")
    ap.add_argument("--negative", choices=("nested", "gpu-token", "buffer40", "off"))
    a = ap.parse_args()
    source, work = a.source_root.resolve(), a.out.resolve()
    manifest = prepare(source, work, a.font_assets.resolve())
    if a.negative:
        name, old, new = {
            "nested": ("ps2_activity.c", "if (!A.idle_depth++)\n\t\tA.idle_start = now;", "A.idle_depth++;\n\tA.idle_start = now;"),
            "gpu-token": ("ps2_activity.c", " || token != A.gpu_serial", ""),
            "buffer40": ("hud-format.inc", "detail[64]", "detail[40]"),
            "off": ("ps2_activity.h", "(PS2_ACTIVITY_ON ? PS2Activity_IdleBegin((thread), (clock)) : 0u)", "PS2Activity_IdleBegin((thread), (clock))"),
        }[a.negative]
        path = work / name
        text = path.read_text()
        if old not in text:
            raise RuntimeError("negative-control anchor missing")
        path.write_text(text.replace(old, new, 1))
    flags = ["-std=gnu17", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter"]
    if a.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
    build = execute(["cc", *flags, "-I" + str(work), Path(__file__).with_suffix(".c"), "-o", work / "test"], work / "build.log")
    if build.returncode:
        print(build.stdout + build.stderr)
        raise RuntimeError("host build failed")
    result = execute([work / "test"], work / "test.log")
    weak_source = '#include "ps2_activity.h"\nint main(void) { unsigned reads=0; if (PS2_ACTIVITY_ON) return 1; if (PS2_ACTIVITY_IDLE_BEGIN(7, ++reads)) return 2; PS2_ACTIVITY_IDLE_END(0, ++reads); return reads != 0; }\n'
    (work / "weak.c").write_text(weak_source)
    weak_build = execute(["cc", *flags, "-I" + str(work), work / "weak.c", "-o", work / "weak"], work / "weak-build.log")
    weak_result = execute([work / "weak"], work / "weak-test.log") if not weak_build.returncode else weak_build
    checks = re.search(r"ACTIVITY COMPLETE checks=(\d+) failed=(\d+)", result.stdout)
    passed = result.returncode == 0 and checks and int(checks[2]) == 0 and weak_result.returncode == 0 and "runtime error:" not in result.stderr
    report = {"scope": __doc__, "passed": bool(passed), "rc": result.returncode, "sanitize": a.sanitize, "negative": a.negative,
              "checks": int(checks[1]) if checks else 0, "weak_unlinked_hooks_rc": weak_result.returncode, "output": result.stdout,
              "source_sha256": manifest, "tested_work_sha256": {name: sha(work / name) for name in ("ps2_activity.c", "ps2_activity.h", "hud-format.inc")},
              "physical_PS2_tested": False, "true_GS_utilization_claimed": False}
    (work / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return int(not passed)


if __name__ == "__main__":
    raise SystemExit(main())
