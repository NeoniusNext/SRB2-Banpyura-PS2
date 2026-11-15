#!/usr/bin/env python3
"""Replace only canonical w_pack IR, merge and optionally link the whole engine.

One EE worker. Canonical objects/config/ELF remain read-only and hash-checked.
Project includes use the complete isolated source snapshot; production source
and generated metadata are never changed. No emulator is started.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

from next9_io_variant import CANDIDATE_SHA

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def run(command, log, timeout):
    started = time.monotonic()
    process = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True, timeout=timeout)
    log.write_text(process.stdout + process.stderr)
    assert process.returncode == 0, (str(log), process.returncode, process.stdout, process.stderr)
    return process.stdout, process.stderr, time.monotonic() - started


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--canonical", type=Path, required=True)
    ap.add_argument("--snapshot", type=Path, default=ROOT / "build/nov15-next11/io/candidate-src")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--optout", action="store_true")
    ap.add_argument("--link", action="store_true")
    a = ap.parse_args()
    a.canonical, a.snapshot, a.out = a.canonical.resolve(), a.snapshot.resolve(), a.out.resolve()
    assert a.out.is_relative_to(ROOT / "build") and not a.out.is_relative_to(a.canonical)
    a.out.mkdir(parents=True, exist_ok=True)
    assert not (a.out / "engine_lto.o").exists(), "use a fresh merge destination"
    report_path = a.canonical / "build-report.json"
    report = json.loads(report_path.read_text())
    renderer = "hardware" if report["hardware"] else "software"
    assert sha(a.canonical / "SRB2.ELF") == report["elf_sha256"]
    snapshot_manifest = json.loads((a.snapshot / "manifest.json").read_text())
    for relative, expected in snapshot_manifest["snapshot_sha256"].items():
        assert sha(a.snapshot / relative) == expected, "snapshot changed: " + relative
    config = a.snapshot / "build" / renderer / "gen/config.h"
    assert sha(config) == sha(a.canonical / "gen/config.h")
    source = a.snapshot / "src/w_pack.c"
    assert sha(source) == CANDIDATE_SHA
    objects = [a.canonical / "obj" / name.replace("/", "__").replace(".c", ".o")
               for name in report["sources"]]
    if report["hardware"]:
        objects += [a.canonical / "obj" / (name + "_vsm.o") for name in ("ps2_hw_vu1", "ps2_hw_vu1_spr")]
    assert len(objects) == (205 if report["hardware"] else 189)
    original = [*objects, a.canonical / "obj/engine_lto.o", a.canonical / "SRB2.ELF",
                a.canonical / "SRB2.map", a.canonical / "gen/config.h", report_path]
    original_hashes = {str(path): sha(path) for path in original}
    old = a.canonical / "obj/src__w_pack.o"
    assert objects.count(old) == 1
    flags = [f for f in report["flags"] if f not in ("-MMD", "-MP")]
    for index, flag in enumerate(flags):
        if not flag.startswith("-I"):
            continue
        include = Path(flag[2:])
        if include == ROOT / "src" or include.is_relative_to(ROOT / "src"):
            flags[index] = "-I" + str(a.snapshot / include.relative_to(ROOT))
        elif include.is_relative_to(ROOT / "build") and include.name == "gen":
            # The original recorded metadata can name an older live build path.
            # Only its canonical frozen bytes are the control input.
            flags[index] = "-I" + str(config.parent)
    assert "-I" + str(config.parent) in flags and "-flto" in flags and "-G8" in flags
    flags += (["-DPS2_NOOPT_PACKHEADER"] if a.optout else [])
    if "-Werror" not in flags:
        flags += ["-Werror"]
    frozen = a.out / "w_pack.c"
    frozen.write_bytes(source.read_bytes())
    replacement = a.out / "w_pack.o"
    compiler = report["link_command"][0]
    compile_command = [compiler, *flags, "-MMD", "-MF", str(a.out / "w_pack.d"),
                       "-c", str(frozen), "-o", str(replacement)]
    stdout, stderr, compile_seconds = run(compile_command, a.out / "compile.log", 120)
    assert not stdout and not stderr, (stdout, stderr)
    dependencies = (a.out / "w_pack.d").read_text().replace("\\\n", " ").split(":", 1)[1].split()
    for name in dependencies:
        path = Path(name).resolve()
        if path.is_relative_to(ROOT / "src"):
            raise AssertionError("mutable production dependency: " + name)
        if path.is_relative_to(a.snapshot / "src"):
            relative = path.relative_to(a.snapshot).as_posix()
            assert sha(path) == snapshot_manifest["snapshot_sha256"][relative]
    candidate_objects = [replacement if path == old else path for path in objects]
    merged = a.out / "engine_lto.o"
    lto_command = [compiler, "-r", "-nostdlib", "-flto=1", "-flinker-output=nolto-rel", "-O2",
                   "-ffunction-sections", "-fdata-sections", *[f for f in flags if f.startswith("-G") or f == "-mno-abicalls"],
                   *map(str, candidate_objects), "-o", str(merged)]
    stdout, stderr, lto_seconds = run(lto_command, a.out / "lto.log", 360)
    expected_diagnostics = (a.canonical / "lto.log").read_text()
    def diagnostic_lines(text):
        return [line for line in text.splitlines() if "linking abicalls files with non-abicalls files" not in line]
    assert not stdout and diagnostic_lines(stderr) == diagnostic_lines(expected_diagnostics), (stdout, stderr, expected_diagnostics)
    expected_abicalls = [line for line in expected_diagnostics.splitlines() if "linking abicalls files with non-abicalls files" in line]
    actual_abicalls = [line for line in stderr.splitlines() if "linking abicalls files with non-abicalls files" in line]
    assert len(actual_abicalls) == len(expected_abicalls) == (1 if report["hardware"] else 0)
    if actual_abicalls:
        for old_line, new_line in zip(expected_abicalls, actual_abicalls):
            old_prefix, old_object = old_line.split(": ", 1)
            new_prefix, new_object = new_line.split(": ", 1)
            assert old_prefix == new_prefix
            suffix = ": warning: linking abicalls files with non-abicalls files"
            assert old_object.endswith(suffix) and new_object.endswith(suffix)
            old_path, new_path = Path(old_object.removesuffix(suffix)), Path(new_object.removesuffix(suffix))
            assert old_path.name == new_path.name == "ps2_hw_vu1_vsm.o"
            assert new_path == a.canonical / "obj/ps2_hw_vu1_vsm.o"
            assert sha(new_path) == original_hashes[str(new_path)]
    result = {"renderer": renderer, "canonical_build": str(a.canonical),
              "canonical_build_report_sha256": original_hashes[str(report_path)],
              "canonical_engine_sha256": original_hashes[str(a.canonical / "obj/engine_lto.o")],
              "canonical_elf_sha256": report["elf_sha256"], "canonical_object_count": len(objects),
              "unchanged_ir_count": len(objects) - 1, "only_recompiled_unit": "src/w_pack.c",
              "optout": a.optout, "source_sha256": sha(source), "source_snapshot": str(a.snapshot),
              "source_snapshot_manifest_sha256": sha(a.snapshot / "manifest.json"),
              "generated_config_sha256": sha(config), "compile_command": compile_command,
              "compile_seconds": compile_seconds, "dependencies": {p: sha(Path(p)) for p in dependencies},
              "lto_command": lto_command, "lto_seconds": lto_seconds, "engine_sha256": sha(merged),
              "canonical_artifacts": original_hashes, "lto_inputs": {str(p): sha(p) for p in candidate_objects},
              "lto_diagnostics_identical_except_known_abicalls_input_path": True,
              "canonical_lto_diagnostics": expected_diagnostics, "actual_lto_diagnostics": stderr,
              "production_sources_edited": False,
              "checker_sha256": sha(Path(__file__))}
    if a.link:
        link = list(report["link_command"])
        engines = [i for i, word in enumerate(link) if word.endswith("/engine_lto.o")]
        assert len(engines) == 1
        link[engines[0]] = str(merged)
        link[link.index("-o") + 1] = str(a.out / "SRB2.ELF")
        maps = [i for i, word in enumerate(link) if word.startswith("-Wl,-Map=")]
        assert len(maps) == 1
        link[maps[0]] = "-Wl,-Map=" + str(a.out / "SRB2.map")
        stdout, stderr, seconds = run(link, a.out / "link.log", 120)
        other = "\n".join(line for line in (stdout + stderr).splitlines()
                          if "linking abicalls files with non-abicalls files" not in line).strip()
        assert not other, other
        result.update(link_command=link, elf_sha256=sha(a.out / "SRB2.ELF"), link_seconds=seconds,
                      reported_compile_diagnostics=0, reported_link_diagnostics=0)
        linked = dict(report)
        linked.update(link_command=link, compiled=1, elf_sha256=result["elf_sha256"],
                      elf_bytes=(a.out / "SRB2.ELF").stat().st_size, io_control=result)
        (a.out / "build-report.json").write_text(json.dumps(linked, indent=2) + "\n")
    assert original_hashes == {str(path): sha(path) for path in original}, "canonical artifacts changed"
    result["canonical_artifacts_unchanged"] = True
    (a.out / "report.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: result[key] for key in ("renderer", "only_recompiled_unit", "unchanged_ir_count",
        "optout", "compile_seconds", "lto_seconds", "engine_sha256")}, indent=2))


if __name__ == "__main__":
    main()
