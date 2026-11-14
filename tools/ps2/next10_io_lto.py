#!/usr/bin/env python3
"""Compile only the cache-walk unit and merge the complete production engine.

Consumes one EE compiler/LTO worker. The existing build's canonical objects,
engine and ELF are read-only and checked unchanged. No emulator is started.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

import build as B

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, log, timeout):
    started = time.monotonic()
    process = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True, timeout=timeout)
    log.write_text(process.stdout + process.stderr)
    assert process.returncode == 0, (command, process.returncode, process.stdout, process.stderr)
    return process.stdout, process.stderr, time.monotonic() - started


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build", type=Path, default=ROOT / "build/ps2")
    ap.add_argument("--report", type=Path)
    ap.add_argument("--baseline-engine", type=Path, default=ROOT / "build/nov15-next10/baseline/software/engine_lto.o")
    ap.add_argument("--source", type=Path, default=ROOT / "src/w_wad.c")
    ap.add_argument("--snapshot", type=Path, required=True,
                    help="frozen source/tools/gen tree used for every project include")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--optout", action="store_true")
    ap.add_argument("--link", action="store_true")
    a = ap.parse_args()
    a.build, a.out = a.build.resolve(), a.out.resolve()
    assert a.out.is_relative_to(ROOT / "build")
    a.out.mkdir(parents=True, exist_ok=True)
    report_path = a.report or a.build / "build-report.json"
    report = json.loads(report_path.read_text())
    original_engine = a.build / "obj/engine_lto.o"
    assert sha(original_engine) == sha(a.baseline_engine), "canonical engine differs from frozen baseline"
    original_engine_hash = sha(original_engine)
    original_elf_hash = sha(a.build / "SRB2.ELF")
    assert original_elf_hash == report["elf_sha256"]
    objects = [a.build / "obj" / name.replace("/", "__").replace(".c", ".o") for name in report["sources"]]
    if report["hardware"]:
        objects += [a.build / "obj" / (name + "_vsm.o") for name in ("ps2_hw_vu1", "ps2_hw_vu1_spr")]
    original_hashes = {str(p): sha(p) for p in objects}
    frozen = a.out / "w_wad.c"
    frozen.write_bytes(a.source.read_bytes())
    flags = [f for f in report["flags"] if f not in ("-MMD", "-MP")]
    a.snapshot = a.snapshot.resolve()
    for relative in ("src", "build/ps2/gen"):
        prefix = "-I" + str(ROOT / relative)
        flags = ["-I" + str(a.snapshot / relative) + f[len(prefix):] if f.startswith(prefix) else f for f in flags]
    assert "-flto" in flags
    unit = B.UNIT_FLAGS.get("src/w_wad.c", [])
    flags += unit + (["-DPS2_NOOPT_LUMPCACHEWALK"] if a.optout else []) + ["-Werror"]
    compiler = report["link_command"][0]
    replacement = a.out / "w_wad.o"
    compile_command = [compiler, *flags, "-MMD", "-MF", str(a.out / "w_wad.d"), "-c", str(frozen), "-o", str(replacement)]
    stdout, stderr, compile_seconds = run(compile_command, a.out / "compile.log", 120)
    assert not stdout and not stderr, (stdout, stderr)
    old = a.build / "obj/src__w_wad.o"
    assert objects.count(old) == 1
    candidate_objects = [replacement if obj == old else obj for obj in objects]
    merged = a.out / "engine_lto.o"
    lto_command = [compiler, "-r", "-nostdlib", "-flto=1", "-flinker-output=nolto-rel", "-O2",
                   "-ffunction-sections", "-fdata-sections", *[f for f in flags if f.startswith("-G") or f == "-mno-abicalls"],
                   *map(str, candidate_objects), "-o", str(merged)]
    stdout, stderr, lto_seconds = run(lto_command, a.out / "lto.log", 360)
    expected_diagnostics = (a.build / "lto.log").read_text()
    assert not stdout and stderr == expected_diagnostics, (stdout, stderr, expected_diagnostics)
    dependencies = (a.out / "w_wad.d").read_text().replace("\\\n", " ").split(":", 1)[1].split()
    result = {"canonical_build": str(a.build), "canonical_report_sha256": sha(report_path),
              "canonical_engine_sha256": original_engine_hash, "canonical_elf_sha256": original_elf_hash,
              "only_recompiled_unit": "src/w_wad.c", "optout": a.optout,
              "source": str(a.source.resolve()), "source_sha256": sha(frozen),
              "source_snapshot": str(a.snapshot), "source_snapshot_manifest_sha256": sha(a.snapshot / "manifest.json"),
              "compile_flags": flags, "compile_command": compile_command, "compile_seconds": compile_seconds,
              "dependencies": {p: sha(Path(p)) for p in dependencies},
              "lto_command": lto_command, "lto_seconds": lto_seconds,
              "engine_sha256": sha(merged), "objects": {str(p): sha(p) for p in candidate_objects},
              "canonical_objects": original_hashes, "lto_diagnostics_identical_to_baseline": True,
              "checker_sha256": sha(Path(__file__))}
    if a.link:
        link = list(report["link_command"])
        engine_indices = [i for i, word in enumerate(link) if word.endswith("/engine_lto.o")]
        assert len(engine_indices) == 1
        link[engine_indices[0]] = str(merged)
        link[link.index("-o") + 1] = str(a.out / "SRB2.ELF")
        map_indices = [i for i, word in enumerate(link) if word.startswith("-Wl,-Map=")]
        assert len(map_indices) == 1
        link[map_indices[0]] = "-Wl,-Map=" + str(a.out / "SRB2.map")
        stdout, stderr, seconds = run(link, a.out / "link.log", 120)
        other = "\n".join(line for line in (stdout + stderr).splitlines()
                          if "linking abicalls files with non-abicalls files" not in line).strip()
        assert not other, other
        result.update(link_command=link, elf_sha256=sha(a.out / "SRB2.ELF"), link_seconds=seconds,
                      reported_compile_diagnostics=0, reported_link_diagnostics=0)
        linked_report = dict(report)
        linked_report.update(link_command=link, compiled=1, elf_sha256=result["elf_sha256"],
                             elf_bytes=(a.out / "SRB2.ELF").stat().st_size, io_control=result)
        (a.out / "build-report.json").write_text(json.dumps(linked_report, indent=2) + "\n")
    assert original_hashes == {str(p): sha(p) for p in objects}, "canonical object changed"
    assert original_engine_hash == sha(original_engine), "canonical engine changed"
    assert original_elf_hash == sha(a.build / "SRB2.ELF"), "canonical ELF changed"
    (a.out / "report.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: result[k] for k in ("only_recompiled_unit", "optout", "compile_seconds", "lto_seconds", "engine_sha256")}, indent=2))


if __name__ == "__main__":
    main()
