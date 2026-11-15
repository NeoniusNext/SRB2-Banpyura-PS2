#!/usr/bin/env python3
"""Recompile only the pack reader's opt-out and rerun the real engine LTO merge.

Uses one EE compiler/LTO worker, an existing complete candidate build and an
isolated output. Optional --link reuses the exact candidate link command; it
does not alter candidate objects or run emulation.
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
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--link", action="store_true", help="also link the complete isolated control ELF and map")
    args = parser.parse_args()
    args.build, args.out = args.build.resolve(), args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    report = json.loads((args.build / "build-report.json").read_text())
    assert sha(args.build / "SRB2.ELF") == report["elf_sha256"], "candidate ELF changed"
    for name, expected in report["input_sha256"].items():
        path = Path(name)
        assert path.exists() and sha(path) == expected, "candidate input changed: " + name
    source = ROOT / "src/w_pack.c"
    frozen = args.out / "w_pack.c"
    frozen.write_bytes(source.read_bytes())
    flags = [flag for flag in report["flags"] if flag not in ("-MMD", "-MP")]
    assert "-flto" in flags
    flags += B.UNIT_FLAGS.get("src/w_pack.c", []) + ["-DPS2_NOOPT_PACKHEADER", "-Werror"]
    compiler = report["link_command"][0]
    replacement = args.out / "w_pack.o"
    command = [compiler, *flags, "-c", str(frozen), "-o", str(replacement)]
    process = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=120)
    (args.out / "compile.log").write_text(process.stdout + process.stderr)
    assert process.returncode == 0 and not process.stdout and not process.stderr, process.stderr
    objects = [args.build / "obj" / source.replace("/", "__").replace(".c", ".o") for source in report["sources"]]
    old_pack = args.build / "obj/src__w_pack.o"
    assert objects.count(old_pack) == 1
    original_objects = list(objects)
    objects = [replacement if obj == old_pack else obj for obj in objects]
    if report["hardware"]:
        objects += [args.build / "obj" / (name + "_vsm.o") for name in ("ps2_hw_vu1", "ps2_hw_vu1_spr")]
        original_objects += [args.build / "obj" / (name + "_vsm.o") for name in ("ps2_hw_vu1", "ps2_hw_vu1_spr")]
    assert all(path.exists() for path in objects)
    original_hashes = {str(path): sha(path) for path in original_objects}
    original_engine_hash = sha(args.build / "obj/engine_lto.o")
    merged = args.out / "engine_lto.o"
    command = [compiler, "-r", "-nostdlib", "-flto=1", "-flinker-output=nolto-rel", "-O2",
               "-ffunction-sections", "-fdata-sections", *[flag for flag in flags if flag.startswith("-G") or flag == "-mno-abicalls"],
               *map(str, objects), "-o", str(merged)]
    process = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=360)
    (args.out / "lto.log").write_text(process.stdout + process.stderr)
    candidate_lto_diagnostics = (args.build / "lto.log").read_text()
    assert process.returncode == 0 and not process.stdout and process.stderr == candidate_lto_diagnostics, process.stderr
    result = {"candidate_build": str(args.build), "candidate_engine_sha256": sha(args.build / "obj/engine_lto.o"),
              "production_pack_source_sha256": sha(source), "optout_engine_sha256": sha(merged),
              "lto_diagnostics_identical_to_candidate": True, "lto_diagnostics": process.stderr,
              "compile_flags": flags, "lto_command": command,
              "objects": {str(path): sha(path) for path in objects}, "original_objects": original_hashes,
              "only_recompiled_unit": "src/w_pack.c", "global_flags_stamp_changed": False}
    if args.link:
        started = time.monotonic()
        output_elf, output_map = args.out / "SRB2.ELF", args.out / "SRB2.map"
        original_engine = args.build / "obj/engine_lto.o"
        link_command = list(report["link_command"])
        engine_indices = [i for i, value in enumerate(link_command) if value == str(original_engine)]
        assert len(engine_indices) == 1
        link_command[engine_indices[0]] = str(merged)
        assert link_command.count("-o") == 1
        link_command[link_command.index("-o") + 1] = str(output_elf)
        map_indices = [i for i, value in enumerate(link_command) if value.startswith("-Wl,-Map=")]
        assert len(map_indices) == 1
        link_command[map_indices[0]] = "-Wl,-Map=" + str(output_map)
        (args.out / "link-command.json").write_text(json.dumps(link_command, indent=2) + "\n")
        process = subprocess.run(link_command, cwd=ROOT, capture_output=True, text=True, timeout=120)
        output = process.stdout + process.stderr
        (args.out / "link.log").write_text(output)
        diagnostics = "\n".join(line for line in output.splitlines()
                                if "linking abicalls files with non-abicalls files" not in line).strip()
        assert process.returncode == 0 and not diagnostics, output
        linked_report = dict(report)
        linked_report.update(command=[str(Path(__file__).resolve()), "--build", str(args.build), "--out", str(args.out), "--link"],
                             compiled=1, link_command=link_command, elf_bytes=output_elf.stat().st_size,
                             elf_sha256=sha(output_elf), compile_diagnostics=0, link_diagnostics=0,
                             unit_flags={"src/w_pack.c": B.UNIT_FLAGS.get("src/w_pack.c", []) + ["-DPS2_NOOPT_PACKHEADER"]},
                             control_kind="canonical object list; only w_pack.c recompiled with PS2_NOOPT_PACKHEADER",
                             canonical_build_report_sha256=sha(args.build / "build-report.json"),
                             canonical_elf_sha256=report["elf_sha256"], canonical_engine_sha256=original_engine_hash,
                             lto_object_inputs=result["objects"], original_lto_object_inputs=original_hashes,
                             optout_engine_sha256=sha(merged), global_flags_stamp_changed=False,
                             original_input_sha256=report["input_sha256"],
                             link_seconds=time.monotonic() - started)
        linked_report["input_sha256"] = dict(report["input_sha256"])
        linked_report["input_sha256"][str(frozen)] = sha(frozen)
        (args.out / "build-report.json").write_text(json.dumps(linked_report, indent=2) + "\n")
        result["elf_sha256"] = linked_report["elf_sha256"]
        result["link_command"] = link_command
        result["reported_link_diagnostics"] = 0
    assert original_hashes == {str(path): sha(path) for path in original_objects}, "canonical LTO inputs changed"
    assert original_engine_hash == sha(args.build / "obj/engine_lto.o"), "canonical engine changed"
    assert sha(args.build / "SRB2.ELF") == report["elf_sha256"], "canonical ELF changed"
    result["all_original_lto_inputs_and_canonical_engine_elf_unchanged"] = True
    (args.out / "report.json").write_text(json.dumps(result, indent=2) + "\n")
    print("Opt-out actual full-engine LTO merge complete:", merged)


if __name__ == "__main__":
    main()
