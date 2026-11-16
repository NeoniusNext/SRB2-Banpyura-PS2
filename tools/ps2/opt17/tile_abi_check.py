#!/usr/bin/env python3
"""Compile the tiled renderer's 140/68-byte EE ABI and original prefix assertions."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from tile_host_common import HERE, ROOT


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source-root", type=Path, default=ROOT)
    ap.add_argument("--build-report", type=Path, required=True, help="tools/ps2/build.py target build-report.json")
    ap.add_argument("--compiler", help="EE GCC override; defaults to the report's linker executable")
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    source, out = a.source_root.resolve(), a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    report = json.loads(a.build_report.read_text())
    flags = []
    for flag in report["flags"]:
        if flag in ("-flto", "-MMD", "-MP"):
            continue
        if flag.startswith("-I") and flag.endswith("/src"):
            flag = "-I" + str(source / "src")
        elif flag.startswith("-I") and flag.endswith("/src/ps2"):
            flag = "-I" + str(source / "src/ps2")
        flags.append(flag)
    compiler = a.compiler or report["link_command"][0]
    command = [compiler, *flags, "-Werror", "-fno-lto", "-I" + str(source / "src/ps2/hw"),
               "-c", str(HERE / "tile_abi.c"), "-o", str(out / "tile_abi.o")]
    (out / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    ran = subprocess.run(command, text=True, capture_output=True)
    (out / "build.log").write_text(ran.stdout + ran.stderr)
    paths = [HERE / "tile_abi.c", source / "src/hardware/hw_data.h", source / "src/ps2/hw/ps2_hwd.c",
             *sorted((source / "src/ps2/hw").glob("*.inc"))]
    (out / "result.json").write_text(json.dumps({"returncode": ran.returncode,
        "source_sha256": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},
        "build_report_sha256": hashlib.sha256(a.build_report.read_bytes()).hexdigest(),
        "assertions": "texrec140, GLMipmap68, all55 original texrec prefix offsets", "emulator_used": False}, indent=2) + "\n")
    print("EE tile ABI returncode:", ran.returncode)
    print(ran.stdout + ran.stderr, end="")
    return int(ran.returncode != 0)


if __name__ == "__main__":
    raise SystemExit(main())
