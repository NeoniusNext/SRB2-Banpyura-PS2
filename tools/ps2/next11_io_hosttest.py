#!/usr/bin/env python3
"""Replay the exact header overlay against the frozen eleventh-pass source.

The complete production readers, historical lifetime/reentry fixture, stock
decoded bytes and executed negative controls are compared. Host compilers only.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from next9_io_variant import BASE_SHA, CANDIDATE_SHA

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def literal_equal(before, after):
    with before.open("rb") as old, after.open("rb") as new:
        while True:
            a, b = old.read(65536), new.read(65536)
            if a != b:
                return False
            if not a:
                return True


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--baseline", type=Path, default=ROOT / "build/nov15-next11/baseline-src/src/w_pack.c")
    ap.add_argument("--snapshot", type=Path, default=ROOT / "build/nov15-next11/io/candidate-src")
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov15-next11/io/host")
    ap.add_argument("--pak", type=Path, default=ROOT / "build/pak")
    ap.add_argument("--negative-controls", action="store_true")
    ap.add_argument("--corpus", action="store_true")
    a = ap.parse_args()
    a.out, a.snapshot = a.out.resolve(), a.snapshot.resolve()
    assert a.out.is_relative_to(ROOT / "build")
    a.out.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((a.snapshot / "manifest.json").read_text())
    for relative, expected in manifest["snapshot_sha256"].items():
        assert sha(a.snapshot / relative) == expected, "snapshot differs: " + relative
    candidate = a.snapshot / "src/w_pack.c"
    assert sha(a.baseline) == BASE_SHA and sha(candidate) == CANDIDATE_SHA
    fixture = a.snapshot / "tools/ps2/next9_io_header_hosttest.c"
    pack_fixture = a.snapshot / "tools/ps2/pack_hosttest.c"
    sdk = Path(os.environ.get("PS2SDK", "/workspace/.onboarding/ps2dev-v2/ps2dev/ps2sdk"))
    flags = ["cc", "-O2", "-g", "-std=gnu17", "-fwrapv", "-Wall", "-Werror", "-Wno-unused-function",
             "-Wno-sign-compare", "-Wno-unused-variable", "-Wno-missing-field-initializers",
             "-DPS2_PROFILE", "-DNOHW", "-DNDEBUG", "-I" + str(a.snapshot / "src"),
             "-I" + str(a.snapshot / "build/host-gen"), "-I" + str(sdk / "ports/include"),
             '-DPACK_HOST_SOURCE="' + str(pack_fixture) + '"']
    result = {"snapshot_manifest_sha256": sha(a.snapshot / "manifest.json"),
              "baseline": {"file": str(a.baseline.resolve()), "sha256": sha(a.baseline)},
              "candidate": {"file": str(candidate), "sha256": sha(candidate)},
              "sources": {str(p): sha(p) for p in (fixture, pack_fixture, Path(__file__))}, "runs": {}}
    executables = {}

    def test(label, source, defines=(), sanitize=False, reject=False):
        executable = a.out / label
        command = [*flags, *defines, '-DWPACK_SOURCE="' + str(source.resolve()) + '"',
                   str(fixture), "-Wl,-l:liblz4.so.1", "-o", str(executable)]
        if sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
        built = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        (a.out / (label + "-build.log")).write_text(built.stdout + built.stderr)
        assert built.returncode == 0, (label, built.stdout, built.stderr)
        run = subprocess.run([str(executable)], cwd=ROOT, capture_output=True, text=True, timeout=60)
        (a.out / (label + ".log")).write_text(run.stdout + run.stderr)
        if reject:
            assert run.returncode == 1 and "FAIL" in run.stderr, (label, run.returncode, run.stdout, run.stderr)
            record = {"compiled_and_executed": True, "detected": True, "returncode": run.returncode,
                      "diagnostic": run.stderr.strip()}
        else:
            assert run.returncode == 0 and not run.stderr, (label, run.returncode, run.stdout, run.stderr)
            record = json.loads(run.stdout)
            executables[label] = executable
        result["runs"][label] = record
        print(label, record, flush=True)
        return record

    old = test("baseline", a.baseline)
    current = test("current", candidate)
    optout = test("old-switch", candidate, ["-DPS2_NOOPT_PACKHEADER"])
    asan = test("asan", candidate, sanitize=True)
    assert old == optout and current == asan
    assert old["stream_bytes"] - current["stream_bytes"] == 128
    assert old["slots_bytes"] - current["slots_bytes"] == 768
    semantic = lambda row: {key: value for key, value in row.items() if key not in ("stream_bytes", "slots_bytes")}
    assert semantic(old) == semantic(current)
    if a.negative_controls:
        text = candidate.read_text()
        controls = {
            "raw-overwritten-on-failure": ("if (h->version < SRP2_VERSION_MIN || h->version > SRP2_VERSION_MAX)\n\t{",
                "if (h->version < SRP2_VERSION_MIN || h->version > SRP2_VERSION_MAX)\n\t{\n\t\tmemset(s->first, 0, sizeof s->first);"),
            "extension-not-committed": ("s->ext = *ext;", "/* deliberate missing parsed extension commit */"),
            "late-probe-clobbers-cache": ("if (s->hdrok)\n\t\t\treturn PeekCheckedMagic(s);",
                "if (0)\n\t\t\treturn PeekCheckedMagic(s);"),
        }
        for label, (before, after) in controls.items():
            assert text.count(before) == 1
            source = a.out / (label + "-w_pack.c")
            source.write_text(text.replace(before, after))
            test("negative-" + label, source, reject=True)
    if a.corpus:
        result["corpus"] = {}
        for pack in ("SRB2.PAK", "ZONES.PAK", "CHARS.PAK", "MUSIC.PAK"):
            record = {"pack_sha256": sha(a.pak / pack), "outputs": {}}
            for label in ("baseline", "current", "old-switch", "asan"):
                table = a.out / (label + "-" + pack + ".tsv")
                literal = a.out / (label + "-" + pack + ".raw")
                run = subprocess.run([str(executables[label]), str(a.pak / pack), str(table)], cwd=ROOT,
                                     capture_output=True, text=True, timeout=180,
                                     env=dict(os.environ, IO_LITERAL_OUT=str(literal)))
                log = a.out / (label + "-" + pack + ".log")
                log.write_text(run.stdout + run.stderr)
                assert run.returncode == 0, (pack, label, run.returncode, run.stderr[-4000:])
                record["outputs"][label] = {"table_sha256": sha(table), "table_bytes": table.stat().st_size,
                    "log_sha256": sha(log), "literal_sha256": sha(literal), "literal_bytes": literal.stat().st_size}
                if label != "baseline":
                    assert literal_equal(a.out / ("baseline-" + pack + ".raw"), literal), (pack, label)
            assert len({json.dumps(row, sort_keys=True) for row in record["outputs"].values()}) == 1, pack
            result["corpus"][pack] = record
            print(pack, "literal decoded bytes, metadata, partial reads and device counters agree", flush=True)
    result["production_sources_edited"] = False
    result["ee_compiler_invoked"] = False
    (a.out / "report.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
