#!/usr/bin/env python3
"""One serial EE worker: frozen-header stream ABI and exact opt-out object.

Isolated objects establish actual target layout only; whole-engine LTO code and
linked frames/storage are separately checked by next11_io_codecheck.py.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from audio_storage_next7_finalbind import Object
from next9_io_variant import BASE_SHA, CANDIDATE_SHA

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def run(command, log):
    p = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True, timeout=120)
    log.write_text(p.stdout + p.stderr)
    assert p.returncode == 0 and not p.stderr, (str(log), p.returncode, p.stderr)
    return p.stdout


def allocated(obj):
    return {name: {"signature": obj.signature(name), "ordered_relocations": obj.relocations.get(name, [])}
            for name, section in obj.sections.items() if section[2] & 2 and name != ".io_header_abi"}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--canonical", type=Path, default=ROOT / "build/nov15-next11/baseline/software")
    ap.add_argument("--baseline", type=Path, default=ROOT / "build/nov15-next11/baseline-src/src/w_pack.c")
    ap.add_argument("--snapshot", type=Path, default=ROOT / "build/nov15-next11/io/candidate-src")
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov15-next11/io/objects")
    a = ap.parse_args()
    a.out, a.snapshot, a.canonical = a.out.resolve(), a.snapshot.resolve(), a.canonical.resolve()
    assert a.out.is_relative_to(ROOT / "build")
    a.out.mkdir(parents=True, exist_ok=True)
    build = json.loads((a.canonical / "build-report.json").read_text())
    assert not build["hardware"]
    compiler = build["link_command"][0]
    candidate = a.snapshot / "src/w_pack.c"
    assert sha(a.baseline) == BASE_SHA and sha(candidate) == CANDIDATE_SHA
    manifest = json.loads((a.snapshot / "manifest.json").read_text())
    for relative, expected in manifest["snapshot_sha256"].items():
        assert sha(a.snapshot / relative) == expected
    config = a.snapshot / "build/software/gen/config.h"
    assert sha(config) == sha(a.canonical / "gen/config.h")
    flags = [f for f in build["flags"] if f not in ("-flto", "-MMD", "-MP")]
    for index, flag in enumerate(flags):
        if flag.startswith("-I"):
            include = Path(flag[2:])
            if include == ROOT / "src" or include.is_relative_to(ROOT / "src"):
                flags[index] = "-I" + str(a.snapshot / include.relative_to(ROOT))
            elif include.is_relative_to(ROOT / "build") and include.name == "gen":
                flags[index] = "-I" + str(config.parent)
    flags += ["-Werror"]
    fields = ("fp", "fd", "buf", "start", "len", "lastend", "run", "size", "pol", "magic", "first", "hdrok", "h", "ext", "path")
    expressions = ["sizeof(boolean)", "sizeof(srp2_header_t)", "sizeof(srp2_ext_t)", "sizeof(pkstream_t)",
                   "_Alignof(pkstream_t)", "sizeof(pkst)", "sizeof(((pkstream_t *)0)->path)"]
    expressions += ["offsetof(pkstream_t," + field + ")" for field in fields]
    result = {"source_sha256": {"baseline": sha(a.baseline), "current": sha(candidate)},
              "snapshot_manifest_sha256": sha(a.snapshot / "manifest.json"),
              "generated_config_sha256": sha(config), "flags": flags, "abi": {}, "sections": {},
              "preprocessed_sha256": {}, "checker_sha256": sha(Path(__file__))}
    for label, source, defines in (("baseline", a.baseline, []), ("current", candidate, []),
                                   ("old-switch", candidate, ["-DPS2_NOOPT_PACKHEADER"])):
        work = a.out / label
        work.mkdir(exist_ok=True)
        generated = work / "probe.c"
        generated.write_text('#include "' + str(source.resolve()) + '"\n#include <stddef.h>\n'
            'const unsigned io_header_abi[] __attribute__((section(".io_header_abi"),used))={'
            + ",".join(expressions) + '};\n')
        objfile = work / "w_pack.o"
        command = [compiler, *flags, *defines, "-c", generated, "-o", objfile]
        run(command, work / "build.log")
        obj = Object(objfile)
        data = obj.content(obj.sections[".io_header_abi"])[:len(expressions) * 4]
        result["abi"][label] = dict(zip(expressions, struct.unpack("<" + "I" * len(expressions), data)))
        result["sections"][label] = allocated(obj)
        preprocessed = run([compiler, *flags, *defines, "-E", "-P", source], work / "preprocessed.i")
        result["preprocessed_sha256"][label] = hashlib.sha256(preprocessed.encode()).hexdigest()
        print(label, result["abi"][label], flush=True)
    assert result["abi"]["baseline"] == result["abi"]["old-switch"]
    assert result["sections"]["baseline"] == result["sections"]["old-switch"]
    assert result["preprocessed_sha256"]["baseline"] == result["preprocessed_sha256"]["old-switch"]
    before, after = result["abi"]["baseline"], result["abi"]["current"]
    assert before["sizeof(boolean)"] == after["sizeof(boolean)"] == 1
    assert before["sizeof(srp2_header_t)"] == after["sizeof(srp2_header_t)"] == 64
    assert before["sizeof(srp2_ext_t)"] == after["sizeof(srp2_ext_t)"] == 64
    assert (before["sizeof(pkstream_t)"], after["sizeof(pkstream_t)"]) == (492, 364)
    assert before["_Alignof(pkstream_t)"] == after["_Alignof(pkstream_t)"] == 4
    assert (before["sizeof(pkst)"], after["sizeof(pkst)"]) == (2952, 2184)
    assert before["sizeof(((pkstream_t *)0)->path)"] == after["sizeof(((pkstream_t *)0)->path)"] == 192
    assert after["offsetof(pkstream_t,first)"] == after["offsetof(pkstream_t,h)"]
    assert after["offsetof(pkstream_t,ext)"] == after["offsetof(pkstream_t,h)"] + 64
    result["target_fixed_bss_saving_bytes"] = 768
    result["optout_preprocessing_and_every_allocated_section_RELA_exact"] = True
    result["whole_game_RAM_saving_claimed"] = False
    (a.out / "report.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
