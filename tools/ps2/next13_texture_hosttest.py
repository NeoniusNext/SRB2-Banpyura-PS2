#!/usr/bin/env python3
"""Source-extracted dec_level truth, score selection, sanitizers and corruptions."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from next13_texture_variant import ROOT, PATH, NEW, OLD, rewrite


def section(text, start, end):
    return text[text.index(start):text.index(end, text.index(start))]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(command, log, expected=0):
    result = subprocess.run([str(x) for x in command], text=True, capture_output=True)
    log.write_text(result.stdout + result.stderr)
    if result.returncode != expected:
        raise RuntimeError(str(command) + " returned " + str(result.returncode) + ": " + (result.stdout + result.stderr)[-3000:])
    return result.stdout


MUTATIONS = {
    "absolute-sign": ("(da ^ (0u - (da >> 31))) + (da >> 31)", "(da ^ (0u - (da >> 31)))"),
    "unstable-tie": ("if (db > bestd)", "if (db >= bestd)"),
    "hole-score": ("da = a == 255 ? 0 : da + 1;", "da = da + 1;"),
    "mean-rounding": ("mean = (sum + 2) >> 2;", "mean = (sum + 1) >> 2;"),
    "sample-column": ("const u32 dxb = o1 - o0;", "const u32 dxb = 0;"),
    "extra-output": ("o[x] = (u8)best;", "o[x] = (u8)best; o[x + 1] = 0;"),
}


def prepare(work, base_src, candidate_src, mutation=None):
    work.mkdir(parents=True)
    baseline = (base_src / PATH).read_text()
    candidate = (candidate_src / PATH).read_text()
    if candidate != rewrite(baseline) or candidate.replace(NEW, OLD) != baseline:
        raise RuntimeError("Candidate is not the isolated reversible score variant")
    for tag, text in (("base", baseline), ("cur", candidate)):
        body = section(text, "static void dec_level(const u8 *", "// PS2-HW-38: level")
        if tag == "cur" and mutation:
            old, new = MUTATIONS[mutation]
            if body.count(old) != 1:
                raise RuntimeError("Missing literal corruption anchor: " + mutation)
            body = body.replace(old, new, 1)
        body = body.replace("static void dec_level(", "void " + tag + "_dec_level(", 1)
        (work / (tag + ".c")).write_text('#include "common.h"\n' + body)
    body = section(baseline, "static void dec_level_ref(", "// The same as dec_level_ref")
    body = body.replace("static void dec_level_ref(", "void truth_dec_level(", 1)
    (work / "truth.c").write_text('#include "common.h"\n' + body)
    (work / "common.h").write_text('''#ifndef NEXT13_TEXTURE_COMMON_H
#define NEXT13_TEXTURE_COMMON_H
#include <stdint.h>
#include <stddef.h>
typedef uint8_t u8; typedef uint32_t u32;
typedef struct { u32 pal[256]; } texture_state_t;
extern texture_state_t H;
typedef void (*texture_fn)(const u8 *, u32, u32, u32, u8 *);
void base_dec_level(const u8 *,u32,u32,u32,u8 *);
void cur_dec_level(const u8 *,u32,u32,u32,u8 *);
void truth_dec_level(const u8 *,u32,u32,u32,u8 *);
#endif
''')
    return {"baseline_texture_sha256": sha(base_src / PATH),
            "candidate_texture_sha256": sha(candidate_src / PATH),
            "generated_sha256": {p.name: sha(p) for p in sorted(work.iterdir())}}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--base-src", type=Path, default=ROOT / "build/nov15-next12/final/source/src")
    p.add_argument("--candidate-src", type=Path, default=ROOT / "build/nov15-next13/texture-first/overlay-src")
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--sanitize", action="store_true")
    p.add_argument("--negative-controls", action="store_true")
    args = p.parse_args()
    if args.out.exists():
        raise RuntimeError("Fresh immutable host output required")
    args.out.mkdir(parents=True)
    reports = {}
    configs = [("ordinary", False, None)]
    if args.sanitize:
        configs.append(("sanitize", True, None))
    if args.negative_controls:
        configs += [("negative-" + name, False, name) for name in MUTATIONS]
    for name, sanitize, mutation in configs:
        work = args.out / name
        inputs = prepare(work, args.base_src.resolve(), args.candidate_src.resolve(), mutation)
        flags = ["-std=gnu17", "-O2", "-Wall", "-Wextra", "-Werror", "-fno-strict-aliasing"]
        if sanitize:
            flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
        binary = work / "test"
        command = ["gcc", *flags, "-I" + str(work), work / "base.c", work / "cur.c", work / "truth.c",
                   ROOT / "tools/ps2/next13_texture_hosttest.c", "-o", binary]
        run(command, work / "build.log")
        stdout = run([binary], work / "run.log", 1 if mutation else 0)
        if mutation and "TEXTURE NEXT13 FAIL" not in stdout:
            raise RuntimeError("Corruption failed for an unrelated reason: " + name)
        reports[name] = {**inputs, "command": [str(x) for x in command], "stdout": stdout,
                         "binary_sha256": sha(binary), "expected_failure": bool(mutation)}
        print(name + ": " + stdout.strip().splitlines()[-1])
    reports["fixture_sha256"] = sha(ROOT / "tools/ps2/next13_texture_hosttest.c")
    (args.out / "host-report.json").write_text(json.dumps(reports, indent=2) + "\n")


if __name__ == "__main__":
    main()
