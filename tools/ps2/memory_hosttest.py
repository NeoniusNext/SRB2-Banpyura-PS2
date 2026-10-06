"""Memory-specific integration tests: real batching triangle/state equivalence and real nested map loader.

Run: python tools/ps2/memory_hosttest.py --out build/ps2-memory-check/integration
Uses MSVC x86 and the same toolchain setup as zone_hosttest.py; no engine files are rewritten.
"""
import argparse
import subprocess
from pathlib import Path
from zone_hosttest import ROOT, vc


def build_run(out, name, fixture, definitions):
    work = out / name
    work.mkdir(exist_ok=True)
    result = vc(work, "x86", ["cl", "/nologo", "/std:c17", "/O2", "/W4", "/WX",
        "/D_CRT_SECURE_NO_WARNINGS", "/DPARANOIA", "/I" + str(ROOT / "src"),
        "/I" + str(ROOT / "src/hardware"), *definitions, "/Fe:test.exe",
        str(ROOT / "tools/ps2" / fixture)], "build.log")
    if result.returncode:
        raise RuntimeError(name + " build failed:\n" + result.stdout + result.stderr)
    result = subprocess.run([str(work / "test.exe")], capture_output=True, text=True, timeout=90)
    (work / "test.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(name + " failed:\n" + result.stdout + result.stderr)
    print(name + ": " + result.stdout.strip())
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=str(ROOT / "build/ps2-memory-check/integration"))
    args = parser.parse_args()
    out = Path(args.out).resolve()
    if not out.parent.is_dir():
        parser.error("output parent must already exist")
    out.mkdir(exist_ok=True)
    reference = out / "batch_reference.c"
    reference.write_text(subprocess.run(["git", "show", "HEAD:src/hardware/hw_batching.c"],
        cwd=ROOT, capture_output=True, text=True, check=True).stdout, encoding="utf-8")
    streams = []
    for name, source, definitions in [("batch-reference", reference, []),
            ("batch-ee", ROOT / "src/hardware/hw_batching.c", ["/DPS2"])]:
        streams.append(build_run(out, name, "memory_batch_hosttest.c", definitions +
            ['/DMEMORY_BATCH_SOURCE="' + source.as_posix() + '"']))
    if streams[0] != streams[1]:
        raise RuntimeError("triangle/state stream differs from reference")
    text = (ROOT / "src/w_wad.c").read_text(encoding="utf-8")
    loader = out / "vres_actual.inc"
    loader.write_text(text[text.index("virtres_t* vres_GetMap("):text.index("/** (Debug) Prints lumps")], encoding="utf-8")
    build_run(out, "nested-map", "memory_vres_hosttest.c",
        ['/DMEMORY_VRES_SOURCE="' + loader.as_posix() + '"'])
    print("PASS exact triangle/state equivalence and bounded nested-map allocation")


if __name__ == "__main__":
    main()
