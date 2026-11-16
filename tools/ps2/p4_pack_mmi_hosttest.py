#!/usr/bin/env python3
"""Test actual P4 functions against literal pre-MMI source, with optional EE work.

Default: extract current source and pinned scalar functions, then run plain and
ASan/UBSan host checks. --ee build additionally builds/disassembles the native
fixture, without launching an emulator. --ee run separately runs a previously
built fixture through the official locked runner. Supply a next12_p4_corpus.py
full-texels.bin with --corpus for host/build work; asset files are not bundled.
"""

import argparse
import hashlib
import importlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SCALAR_COMMIT = "dbc98504e1ad2cc6c2e8e1d24a6ef5d2f31d6ebf"
TEX_PATH = Path("src/ps2/hw/ps2_hw_tex.inc")
EXECUTED_NATIVE_FUNCTIONS_SHA256 = "b608b44870cd8d1cba0fea0dca6c2444ac5c2158daf895bb13f04c508469c67e"
HOST_TIMEOUT = 60
EE_TIMEOUT_MAX = 120


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def digest(text):
    return hashlib.sha256(text.encode()).hexdigest()


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise RuntimeError("Missing or ambiguous actual-source anchor: " + before)
    return text.replace(before, after, 1)


def function(text, name):
    matches = list(re.finditer(r"^static (?:inline )?(?:void|u32|int) " + name + r"\(", text, re.M))
    if len(matches) != 1:
        raise RuntimeError("Missing or ambiguous actual function: " + name)
    match = matches[0]
    first = text.index("{", match.start())
    last, depth = first + 1, 1
    while depth:
        depth += (text[last] == "{") - (text[last] == "}")
        last += 1
    return text[match.start():last] + "\n"


def context(text):
    first = text.index("typedef struct\n{\n\tconst u8 *src; // source rows")
    last = text.index("} fillctx_t;", first) + len("} fillctx_t;")
    return text[first:last]


def extract(source_root, out):
    result = subprocess.run(
        ["git", "show", SCALAR_COMMIT + ":" + TEX_PATH.as_posix()],
        cwd=ROOT, text=True, capture_output=True, timeout=10)
    if result.returncode:
        raise RuntimeError("Pinned scalar commit is required locally: " + SCALAR_COMMIT + "\n" + result.stderr)
    baseline = result.stdout
    source = source_root / TEX_PATH
    candidate = source.read_text()
    if context(baseline) != context(candidate):
        raise RuntimeError("fillctx_t differs from the pinned scalar ABI")
    for name in ("src_row", "src_col"):
        if function(baseline, name) != function(candidate, name):
            raise RuntimeError("Coordinate helper differs from the pinned scalar source: " + name)
    scalar_fill = function(baseline, "fill_p4")
    scalar_band = function(baseline, "p4_band")
    if "__asm__" in scalar_fill + scalar_band:
        raise RuntimeError("The pinned reference is not scalar")
    head = context(candidate) + "\n" + function(candidate, "src_row") + function(candidate, "src_col")
    bf = replace_once(scalar_fill, "static void fill_p4", "static __attribute__((noinline)) void baseline_fill_p4")
    cf = replace_once(function(candidate, "fill_p4"), "static void fill_p4", "static __attribute__((noinline)) void candidate_fill_p4")
    bn = replace_once(function(candidate, "p4_band"), "static int p4_band", "static __attribute__((noinline)) int candidate_p4_band")
    bb = replace_once(scalar_band, "static int p4_band", "static __attribute__((noinline)) int baseline_p4_band")

    # Native mutants change the actual MMI instructions. Their C fallback
    # counterparts let the three packing controls execute in host builds too.
    shift = cf.replace("candidate_fill_p4", "mutation_shift")
    shift = replace_once(shift, "psrlh %[odd],%[value],4", "psrlh %[odd],%[value],5")
    shift = replace_once(shift, "(src[x + 1] & 15u) << 4", "(src[x + 1] & 15u) << 5")
    gather = cf.replace("candidate_fill_p4", "mutation_gather")
    gather = replace_once(gather, "ppacb %[value],$zero,%[value]", "ppacb %[value],%[value],$zero")
    gather = replace_once(gather, "(u8)((src[x] & 15u) | ((src[x + 1] & 15u) << 4))", "0")
    mask = cf.replace("candidate_fill_p4", "mutation_mask")
    mask = replace_once(mask, "0x0F0F0F0F0F0F0F0FULL", "0x0E0E0E0E0E0E0E0EULL")
    mask = replace_once(mask, "(src[x] & 15u)", "(src[x] & 14u)")
    band = bn.replace("candidate_p4_band", "mutation_band")
    band = replace_once(band, "pcpyud %[upper],%[value],%[value]", "pcpyud %[upper],$zero,$zero")
    mutations = [shift, gather, mask, "#ifdef __mips__\n" + band + "#define PACK_BAND_MUTATION 1\n#endif\n"]
    inc = out / "actual-functions.inc"
    inc.write_text(head + bf + cf + bn + bb + "\n".join(mutations))
    (out / "scalar-source.inc").write_text(baseline)
    (out / "candidate-source.inc").write_text(candidate)
    fixture = out / "fixture.c"
    shutil.copyfile(ROOT / "tools/ps2/p4_pack_mmi_hosttest.c", fixture)
    return {
        "candidate_source": str(source), "candidate_source_sha256": sha(source),
        "scalar_commit": SCALAR_COMMIT, "scalar_file_sha256": digest(baseline),
        "literal_function_sha256": {
            "scalar_fill_p4": digest(scalar_fill), "scalar_p4_band": digest(scalar_band),
            "candidate_fill_p4": digest(function(candidate, "fill_p4")),
            "candidate_p4_band": digest(function(candidate, "p4_band"))},
        "coordinate_helpers_and_context_identical_to_pinned_scalar": True,
        "actual_functions_sha256": sha(inc),
        "actual_functions_match_20261116_executed_native_evidence": sha(inc) == EXECUTED_NATIVE_FUNCTIONS_SHA256,
        "reference": "Literal scalar C bodies from git show; only function names/noinline attributes change. No independent packing model.",
        "fixture_sha256": sha(fixture), "runner_sha256": sha(Path(__file__))}


def run(command, log, commands, env=None, timeout=HOST_TIMEOUT):
    command = list(map(str, command))
    commands.append(command)
    try:
        result = subprocess.run(command, cwd=ROOT, env=env, text=True, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        log.write_text(str(error) + "\n")
        raise RuntimeError("Command exceeded its bounded timeout: " + str(log)) from error
    log.write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError("Command failed: " + str(log) + "\n" + result.stdout + result.stderr)
    return result


def parse_result(text, native):
    match = re.search(r"PACK selftest checks=(\d+) cases=(\d+) failures=(\d+)", text)
    mutations = re.search(r"PACK mutations detected=(\d+) expected=(\d+)", text)
    corpus = re.search(r"PACK corpus textures=(\d+) source_bytes=(\d+) packed_source_bytes=(\d+)", text)
    expected = 4 if native else 3
    if (not match or match.group(3) != "0" or not mutations or not corpus
            or tuple(map(int, mutations.groups())) != (expected, expected)
            or "PACK DONE failures=0" not in text or "PACK FAIL" in text
            or "runtime error:" in text or "ERROR: AddressSanitizer" in text):
        raise RuntimeError("Executed fixture did not pass every semantic/mutation gate")
    result = {
        "passed": True, "checks": int(match.group(1)), "cases": int(match.group(2)),
        "failures": 0, "executed_mutations_detected": expected,
        "corpus": dict(zip(("textures", "source_bytes", "packed_source_bytes"), map(int, corpus.groups())))}
    if native:
        result["packing_Count"] = [dict(zip(("width", "height", "source_offset", "scalar", "candidate", "texels"), map(int, m)))
            for m in re.findall(r"PACK bench width=(\d+) height=(\d+) src_off=(\d+) scalar=(\d+) candidate=(\d+) texels=(\d+)", text)]
        result["band_Count"] = [dict(zip(("texels", "source_offset", "scalar", "candidate", "sink"), map(int, m)))
            for m in re.findall(r"PACK band texels=(\d+) src_off=(\d+) scalar=(\d+) candidate=(\d+) sink=(\d+)", text)]
        if len(result["packing_Count"]) != 18 or len(result["band_Count"]) != 2:
            raise RuntimeError("Native fixture did not complete every bounded Count observation")
    return result


def host(out, corpus, commands):
    results = {}
    for sanitize in (False, True):
        tag = "sanitize" if sanitize else "host"
        flags = ["-std=gnu17", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable", "-Wno-unused-function", "-I" + str(out)]
        if sanitize:
            flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
        run(["gcc", *flags, out / "fixture.c", "-o", out / tag], out / (tag + "-build.log"), commands)
        result = run([out / tag, corpus], out / (tag + ".log"), commands)
        results[tag] = parse_result(result.stdout + result.stderr, False)
    return results


def ee_build(out, corpus, commands):
    os.environ["SRB2_PS2_OUT"] = str(out / "ee")
    build = importlib.import_module("build")
    ee = out / "ee"
    ee.mkdir()
    shutil.copyfile(corpus, ee / "full-texels.bin")
    flags = [f for f in build.CFLAGS if f not in ("-flto", "-MMD", "-MP")] + ["-Werror", "-I" + str(out)]
    obj, elf = ee / "fixture.o", ee / "PACK_BENCH.ELF"
    run([build.CC, *flags, *build.INCS[3:], "-c", out / "fixture.c", "-o", obj], ee / "build.log", commands, build.ENV)
    linked = run([build.CC, *build.LDFLAGS, obj, "-o", elf, "-Wl,-Map=" + str(ee / "pack.map"), "-ldebug", "-lpatches", "-lm"], ee / "link.log", commands, build.ENV)
    unexpected = [line for line in (linked.stdout + linked.stderr).splitlines()
                  if line.strip() and "linking abicalls files with non-abicalls files" not in line]
    if unexpected:
        raise RuntimeError("Unexpected linker diagnostics: " + str(ee / "link.log"))
    objdump = build.DEV / ("ee/bin/mips64r5900el-ps2-elf-objdump" + build.EXE)
    disassembly = run([objdump, "-dr", obj], ee / "disassembly.log", commands, build.ENV)
    for opcode in ("lq", "pand", "psrlh", "por", "ppacb", "sd", "pxor", "pcpyud"):
        if not re.search(r"\b" + opcode + r"\b", disassembly.stdout):
            raise RuntimeError("Missing required native opcode: " + opcode)
    return {"built": True, "executed": False, "ELF_sha256": sha(elf), "ELF_bytes": elf.stat().st_size,
            "compiler": str(build.CC), "compiler_sha256": sha(build.CC), "flags": flags,
            "frozen_input_sha256": {str(path.relative_to(out)): sha(path) for path in
                (out / "fixture.c", out / "actual-functions.inc", ee / "full-texels.bin", elf)}}


def ee_run(out, timeout, lock_wait):
    report_path = out / "report.json"
    report = json.loads(report_path.read_text())
    built = report.get("EE", {})
    if not built.get("built"):
        raise RuntimeError("Build first with --ee build")
    for name, expected in built["frozen_input_sha256"].items():
        if sha(out / name) != expected:
            raise RuntimeError("Frozen native input changed after compilation: " + name)
    command = [sys.executable, ROOT / "tools/ps2/run_pcsx2.py", "--elf", out / "ee/PACK_BENCH.ELF",
               "--log", out / "ee/run.log", "--until", "PACK DONE", "--marker", "PACK ",
               "--timeout", str(timeout), "--lock-wait", str(lock_wait)]
    report["emulator_invoked"] = True
    built["run_attempted"] = True
    built["executed"] = False
    built.pop("run", None)
    built.pop("run_log_sha256", None)
    built["timeout_seconds"] = timeout
    built["lock_wait_seconds"] = lock_wait
    try:
        run(command, out / "ee/runner.log", report["commands"], timeout=timeout + lock_wait + 40)
        built["run"] = parse_result((out / "ee/run.log").read_text(errors="replace"), True)
        built["executed"] = True
        built["run_log_sha256"] = sha(out / "ee/run.log")
        built.pop("last_run_error", None)
    except Exception as error:
        built["last_run_error"] = str(error)
        raise
    finally:
        report_path.write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT, help="Candidate checkout or frozen root containing src/")
    parser.add_argument("--corpus", type=Path, help="External full-texels.bin from next12_p4_corpus.py")
    parser.add_argument("--out", type=Path, default=ROOT / "build/p4-pack-mmi-hosttest", help="Fresh output directory; --ee run reuses a prior build")
    parser.add_argument("--ee", choices=("build", "run"), help="Build native ELF without launch, or separately run the previously built ELF")
    parser.add_argument("--timeout", type=float, default=120, help="Native run timeout, at most 120 seconds")
    parser.add_argument("--lock-wait", type=float, default=20, help="Official emulator lock wait, at most 60 seconds")
    args = parser.parse_args()
    if not 0 < args.timeout <= EE_TIMEOUT_MAX or not 0 <= args.lock_wait <= 60:
        parser.error("Keep the native timeout within 120 seconds and lock wait within 60 seconds")
    out = args.out.resolve()
    if args.ee == "run":
        report = ee_run(out, args.timeout, args.lock_wait)
        print(json.dumps({"report": str(out / "report.json"), "native": report["EE"]["run"]}, indent=2))
        return 0
    if not args.corpus or not args.corpus.is_file():
        parser.error("--corpus requires an external next12_p4_corpus.py full-texels.bin")
    if out.exists():
        parser.error("Use a fresh --out directory; --ee run reuses a completed --ee build")
    source_root, corpus = args.source_root.resolve(), args.corpus.resolve()
    if not (source_root / TEX_PATH).is_file():
        parser.error("--source-root must contain " + str(TEX_PATH))
    out.mkdir(parents=True)
    commands = []
    report = {"source": extract(source_root, out), "corpus": str(corpus), "corpus_sha256": sha(corpus),
              "commands": commands, "host": host(out, corpus, commands), "emulator_invoked": False,
              "scope": "Actual-source packing/band checks and bounded COP0 Count observations; no full-game or physical PS2 performance claim."}
    if args.ee == "build":
        report["EE"] = ee_build(out, corpus, commands)
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"report": str(out / "report.json"), "host": report["host"],
                      "EE_built": args.ee == "build", "emulator_invoked": False}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
