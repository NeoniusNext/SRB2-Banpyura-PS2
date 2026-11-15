#!/usr/bin/env python3
"""Execute negative controls against the actual full-ELF pack graph binder.

ELF mutations update the copy's top-level file hash, so rejection must reach
the code/relocation/literal checks rather than merely fail a file-hash gate.
Original canonical/candidate artifacts remain unchanged.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

from audio_storage_next7_finalbind import Object

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def elf_offset(elf, address):
    sections = [s for s in elf.headers if s[2] & 2 and s[1] != 8 and s[3] <= address < s[3] + s[5]]
    assert len(sections) == 1
    section = sections[0]
    return section[4] + address - section[3]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--baseline", type=Path, default=ROOT / "build/nov15-next11/baseline/software")
    ap.add_argument("--candidate", type=Path, default=ROOT / "build/nov15-next11/io/software")
    ap.add_argument("--binding", type=Path, default=ROOT / "build/nov15-next11/io/software-codecheck.json")
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov15-next11/io/binder-negatives")
    a = ap.parse_args()
    a.out, a.baseline, a.candidate = a.out.resolve(), a.baseline.resolve(), a.candidate.resolve()
    assert a.out.is_relative_to(ROOT / "build") and not a.out.is_relative_to(a.candidate)
    a.out.mkdir(parents=True, exist_ok=True)
    original_paths = [a.candidate / name for name in ("engine_lto.o", "SRB2.ELF", "SRB2.map", "build-report.json")]
    original_hashes = {str(path): sha(path) for path in original_paths}
    graph = json.loads(a.binding.read_text())["current"]
    assert graph["engine_sha256"] == original_hashes[str(a.candidate / "engine_lto.o")]
    assert graph["elf_sha256"] == original_hashes[str(a.candidate / "SRB2.ELF")]
    engine, elf = Object(a.candidate / "engine_lto.o"), Object(a.candidate / "SRB2.ELF")
    helper = graph["linked_binding"]["sections"][".text.PeekCheckedMagic"]
    calls = [row for row in helper["ordered_resolved_relocations"] if row["type"] == 4]
    assert calls
    policy = next(row for row in graph["linked_binding"]["merged_literal_ranges"][".rodata.str1.8"].values()
                  if row["bytes"] == 4 and elf.data[elf_offset(elf, row["linked_address"]):][:4] == b"dvd\0")
    result = {"binding_sha256": sha(a.binding), "checker_sha256": sha(ROOT / "tools/ps2/next11_io_codecheck.py"),
              "control_driver_sha256": sha(Path(__file__)), "original_artifacts": original_hashes, "runs": {}}
    for label in ("instruction", "call-address", "policy-literal", "object-addend"):
        out = a.out / label
        out.mkdir(exist_ok=True)
        source_elf, source_engine = a.candidate / "SRB2.ELF", a.candidate / "engine_lto.o"
        if label == "object-addend":
            data = bytearray(engine.data)
            name = ".text.PeekCheckedMagic"
            rows = engine.relocations[name]
            index = next(i for i, row in enumerate(rows) if row["type"] == 4)
            section = engine.sections[rows[index]["relocation_section"]]
            offset = section[4] + index * section[9] + 8
            old = struct.unpack_from("<i", data, offset)[0]
            struct.pack_into("<i", data, offset, old + 4)
            source_engine = out / "engine_lto.o"
            source_engine.write_bytes(data)
        else:
            data = bytearray(elf.data)
            if label == "instruction":
                offset = elf_offset(elf, helper["address"])
                data[offset] ^= 1
            elif label == "call-address":
                offset = elf_offset(elf, helper["address"] + calls[0]["offset"])
                data[offset] ^= 1
            else:
                offset = elf_offset(elf, policy["linked_address"])
                data[offset] = ord("x")
            source_elf = out / "SRB2.ELF"
            source_elf.write_bytes(data)
            (out / "SRB2.map").write_bytes((a.candidate / "SRB2.map").read_bytes())
            report = json.loads((a.candidate / "build-report.json").read_text())
            report["elf_sha256"] = sha(source_elf)
            (out / "build-report.json").write_text(json.dumps(report, indent=2) + "\n")
        command = [sys.executable, "-B", str(ROOT / "tools/ps2/next11_io_codecheck.py"),
                   "--baseline-engine", str(a.baseline / "obj/engine_lto.o"),
                   "--baseline-elf", str(a.baseline / "SRB2.ELF"),
                   "--current-engine", str(source_engine), "--current-elf", str(source_elf),
                   "--out", str(out / "unexpected-success.json")]
        p = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=90)
        (out / "run.log").write_text(p.stdout + p.stderr)
        assert p.returncode == 1 and "AssertionError" in p.stderr
        assert "sha(binary) == build" not in p.stderr, "file hash rejected before actual binding"
        assert ".text.PeekCheckedMagic" in p.stderr if label != "policy-literal" else ".data.policies" in p.stderr
        assert not (out / "unexpected-success.json").exists()
        result["runs"][label] = {"executed": True, "detected_by_actual_binding": True,
            "returncode": p.returncode, "elf_sha256": sha(source_elf), "engine_sha256": sha(source_engine),
            "diagnostic": p.stderr.strip(), "command": command}
        print(label, "detected by actual binding", flush=True)
    assert original_hashes == {str(path): sha(path) for path in original_paths}
    result["original_artifacts_unchanged"] = True
    (a.out / "report.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
