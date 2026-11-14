#!/usr/bin/env python3
"""Real pack-header lifetime/reentry proof and byte-identical stock pack replay.

Compiles the complete frozen/current production reader with the engine's host
service stubs and system liblz4. No EE compiler or emulator is invoked.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
from next9_io_variant import prepare

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools/ps2"


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
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, default=ROOT / "build/nov15-next9/baseline-src/src/w_pack.c")
    parser.add_argument("--candidate", type=Path, help="frozen rejected reader; otherwise reconstruct the checked patch")
    parser.add_argument("--out", type=Path, default=ROOT / "build/nov15-next9/io/rejected-replay/host")
    parser.add_argument("--pak", type=Path, default=ROOT / "build/pak")
    parser.add_argument("--negative-controls", action="store_true")
    parser.add_argument("--corpus", action="store_true")
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    sdk = Path(os.environ.get("PS2SDK", "/opt/ps2dev-x/ps2dev/ps2sdk"))
    fixture = TOOLS / "next9_io_header_hosttest.c"
    production = args.candidate.resolve() if args.candidate else prepare(args.out / "inputs/w_pack.c", args.baseline)
    text = production.read_text()
    flags = ["cc", "-O2", "-g", "-std=gnu17", "-fwrapv", "-Wall", "-Werror", "-Wno-unused-function",
             "-Wno-sign-compare", "-Wno-unused-variable", "-Wno-missing-field-initializers",
             "-DPS2_PROFILE", "-DNOHW", "-DNDEBUG", "-I" + str(ROOT / "src"),
             "-I" + str(ROOT / "build/host-gen"), "-I" + str(sdk / "ports/include"),
             '-DPACK_HOST_SOURCE="' + str(TOOLS / "pack_hosttest.c") + '"']
    result = {"sources": {str(path.relative_to(ROOT)): sha(path) for path in
                          (production, ROOT / "src/w_pack.h", ROOT / "src/w_wad.h", fixture, Path(__file__).resolve())},
              "baseline": {"file": str(args.baseline), "sha256": sha(args.baseline)}, "runs": {}}
    executables = {}

    def test(label, source, defines=(), sanitize=False, reject=False):
        executable = args.out / label
        command = [*flags, *defines, '-DWPACK_SOURCE="' + str(source.resolve()) + '"', fixture,
                   "-Wl,-l:liblz4.so.1", "-o", executable]
        if sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
        process = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True)
        (args.out / (label + "-build.log")).write_text(process.stdout + process.stderr)
        assert process.returncode == 0, (label, process.stdout, process.stderr)
        process = subprocess.run([str(executable)], cwd=ROOT, capture_output=True, text=True, timeout=60)
        (args.out / (label + ".log")).write_text(process.stdout + process.stderr)
        if reject:
            assert process.returncode == 1 and "FAIL" in process.stderr, (label, process.returncode, process.stdout, process.stderr)
            record = {"executed": True, "detected": True, "returncode": process.returncode,
                      "diagnostic": process.stderr.strip()}
        else:
            assert process.returncode == 0, (label, process.returncode, process.stdout, process.stderr)
            assert process.stderr == "", (label, process.stderr)
            record = json.loads(process.stdout)
            executables[label] = executable
        result["runs"][label] = record
        print(label, record, flush=True)
        return record

    old = test("baseline", args.baseline)
    current = test("current", production)
    optout = test("old-switch", production, ["-DPS2_NOOPT_PACKHEADER"])
    asan = test("asan", production, sanitize=True)
    assert old == optout
    assert current == asan
    assert old["stream_bytes"] - current["stream_bytes"] == 128
    assert old["slots_bytes"] - current["slots_bytes"] == 768
    semantic = lambda r: {key: value for key, value in r.items() if key not in ("stream_bytes", "slots_bytes")}
    assert semantic(old) == semantic(current)
    if args.negative_controls:
        controls = {
            "raw-overwritten-on-failure": ("if (h->version < SRP2_VERSION_MIN || h->version > SRP2_VERSION_MAX)\n\t{",
                                           "if (h->version < SRP2_VERSION_MIN || h->version > SRP2_VERSION_MAX)\n\t{\n\t\tmemset(s->first, 0, sizeof s->first);"),
            "extension-not-committed": ("s->ext = *ext;", "/* deliberately omit the parsed extension commit */"),
            "late-probe-clobbers-cache": ("if (s->hdrok)\n\t\t\treturn PeekCheckedMagic(s);",
                                          "if (0)\n\t\t\treturn PeekCheckedMagic(s);"),
        }
        for label, (before, after) in controls.items():
            assert text.count(before) == 1, label
            variant = args.out / (label + "-w_pack.c")
            variant.write_text(text.replace(before, after))
            test("negative-" + label, variant, reject=True)
    if args.corpus:
        result["corpus"] = {}
        for pack in ("SRB2.PAK", "ZONES.PAK", "CHARS.PAK", "MUSIC.PAK"):
            record = {"pack_sha256": sha(args.pak / pack), "outputs": {}}
            for label in ("baseline", "current", "old-switch", "asan"):
                table = args.out / (label + "-" + pack + ".tsv")
                literal = args.out / (label + "-" + pack + ".raw")
                process = subprocess.run([str(executables[label]), str(args.pak / pack), str(table)],
                                         cwd=ROOT, capture_output=True, text=True, timeout=180,
                                         env=dict(os.environ, IO_LITERAL_OUT=str(literal)))
                log = args.out / (label + "-" + pack + ".log")
                log.write_text(process.stdout + process.stderr)
                assert process.returncode == 0, (pack, label, process.returncode, process.stderr[-4000:])
                record["outputs"][label] = {"table_sha256": sha(table), "table_bytes": table.stat().st_size,
                                             "log_sha256": sha(log), "literal_sha256": sha(literal),
                                             "literal_bytes": literal.stat().st_size}
                if label != "baseline":
                    assert literal_equal(args.out / ("baseline-" + pack + ".raw"), literal), (pack, label)
            assert len({json.dumps(row, sort_keys=True) for row in record["outputs"].values()}) == 1, pack
            result["corpus"][pack] = record
            print(pack, "all decoded bytes, metadata, partial reads and device counters agree", flush=True)
    (args.out / "report.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
