#!/usr/bin/env python3
"""One serial EE worker: actual header ABI, object frames and opt-out restoration.

No emulator or complete-engine build is invoked. These are isolated production
object quantities; final full-LTO quantities are audited separately.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

import build as B
from audio_storage_next7_finalbind import Object
from next9_io_variant import prepare

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, log):
    process = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True, timeout=120)
    log.write_text(process.stdout + process.stderr)
    assert process.returncode == 0 and not process.stderr, (str(log), process.returncode, process.stderr)
    return process.stdout


def frames(text):
    result = {}
    current = None
    for line in text.splitlines():
        match = re.match(r"[0-9a-f]+ <([^>]+)>:", line)
        if match:
            current = match[1]
            result[current] = 0
        match = re.search(r"(?:d?addiu)\s+sp,sp,-(\d+)", line)
        if match and current:
            result[current] = max(result[current], int(match[1]))
    return result


def allocated(obj):
    return {name: {"type": section[1], "flags": section[2], "bytes": section[5],
                   "alignment": section[8], "sha256": hashlib.sha256(obj.content(section)).hexdigest(),
                   "relocations": obj.relocations.get(name, [])}
            for name, section in obj.sections.items() if section[2] & 2 and name != ".io_header_abi"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, default=ROOT / "build/nov15-next9/baseline-src/src/w_pack.c")
    parser.add_argument("--candidate", type=Path, help="frozen rejected reader; otherwise reconstruct the checked patch")
    parser.add_argument("--out", type=Path, default=ROOT / "build/nov15-next9/io/rejected-replay/objects")
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    candidate = args.candidate.resolve() if args.candidate else prepare(args.out / "inputs/rejected-w_pack.c", args.baseline)
    flags = [flag for flag in B.CFLAGS if flag not in ("-flto", "-MMD", "-MP")] + ["-Werror"]
    objdump = B.DEV / ("ee/bin/mips64r5900el-ps2-elf-objdump" + B.EXE)
    fields = ("fp", "fd", "buf", "start", "len", "lastend", "run", "size", "pol", "magic", "first", "hdrok", "h", "ext", "path")
    expressions = ["sizeof(srp2_header_t)", "sizeof(srp2_ext_t)", "sizeof(pkstream_t)", "_Alignof(pkstream_t)", "sizeof(pkst)"]
    expressions += ["offsetof(pkstream_t," + field + ")" for field in fields]
    result = {"source_sha256": {"baseline": sha(args.baseline), "current": sha(candidate)},
              "flags": flags + B.INCS, "abi": {}, "sections": {}, "frames": {}, "preprocessed_sha256": {}}
    inputs = args.out / "inputs"
    inputs.mkdir(exist_ok=True)
    baseline = inputs / "baseline-w_pack.c"
    current = inputs / "current-w_pack.c"
    baseline.write_bytes(args.baseline.read_bytes())
    current.write_bytes(candidate.read_bytes())
    for label, source, defines in (("baseline", baseline, []), ("current", current, []),
                                   ("old-switch", current, ["-DPS2_NOOPT_PACKHEADER"])):
        work = args.out / label
        work.mkdir(exist_ok=True)
        generated = work / "probe.c"
        generated.write_text('#include "' + str(source) + '"\n#include <stddef.h>\n'
                             'const unsigned io_header_abi[] __attribute__((section(".io_header_abi"),used))={'
                             + ",".join(expressions) + '};\n')
        objfile = work / "w_pack.o"
        run([B.CC, *flags, *B.INCS, *defines, "-c", generated, "-o", objfile], work / "build.log")
        obj = Object(objfile)
        data = obj.content(obj.sections[".io_header_abi"])[:len(expressions) * 4]
        result["abi"][label] = dict(zip(expressions, struct.unpack("<" + "I" * len(expressions), data)))
        result["sections"][label] = allocated(obj)
        assembly = run([objdump, "-dr", objfile], work / "w_pack.s")
        result["frames"][label] = frames(assembly)
        text = run([B.CC, *flags, *B.INCS, *defines, "-E", "-P", source], work / "preprocessed.i")
        result["preprocessed_sha256"][label] = hashlib.sha256(text.encode()).hexdigest()
        print(label, result["abi"][label], flush=True)
    assert result["abi"]["baseline"] == result["abi"]["old-switch"]
    assert result["sections"]["baseline"] == result["sections"]["old-switch"], "opt-out allocated bytes or relocations differ"
    assert result["preprocessed_sha256"]["baseline"] == result["preprocessed_sha256"]["old-switch"]
    assert result["abi"]["baseline"]["sizeof(pkst)"] - result["abi"]["current"]["sizeof(pkst)"] == 768
    assert result["abi"]["baseline"]["_Alignof(pkstream_t)"] == result["abi"]["current"]["_Alignof(pkstream_t)"]
    categories = {"text": (".text",), "rodata": (".rodata",), "data": (".data", ".sdata"), "bss": (".bss", ".sbss")}
    result["totals"] = {label: {category: sum(row["bytes"] for name, row in sections.items() if name.startswith(prefixes))
                               for category, prefixes in categories.items()}
                        for label, sections in result["sections"].items()}
    result["delta"] = {key: result["totals"]["current"][key] - result["totals"]["baseline"][key] for key in categories}
    assert result["delta"]["bss"] == -768
    assert result["delta"]["data"] == result["delta"]["rodata"] == 0
    result["frame_differences"] = {name: [size, result["frames"]["current"].get(name)]
                                     for name, size in result["frames"]["baseline"].items()
                                     if size != result["frames"]["current"].get(name)}
    (args.out / "report.json").write_text(json.dumps(result, indent=2) + "\n")
    print("delta", result["delta"], "existing-frame changes", result["frame_differences"], flush=True)
    print("late-probe frame", result["frames"]["current"].get("PeekCheckedMagic"), flush=True)


if __name__ == "__main__":
    main()
