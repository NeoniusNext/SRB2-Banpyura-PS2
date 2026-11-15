#!/usr/bin/env python3
"""Freeze and exercise the isolated pass-13 32-pixel clear-strip candidate.

Exports the pinned Git tree into a new immutable build stage. Only that export
is changed. The host proof compiles literal packet allocation/register/clear
functions; it is not an EE, GS hardware, or performance test.
"""
import argparse
import difflib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[2]
REVISION = "353cc94103aeb38cef87d26151c779302135d0e9"
GS_PATH = Path("src/ps2/hw/ps2_hw_gs.inc")

CANDIDATE = """static void gs_fill_all(int buf, int colour, int depth, u32 rgba)
{
	qw_t *p = ad_alloc(8);
	qw_t *s;
	u32 x, strips = ((u32)H.vw + 31u) / 32u;

	ad_set(p++, frame_reg(buf, colour ? 0 : 0xFFFFFFFFu), GSR_FRAME_1);
	ad_set(p++, zbuf_reg(!depth), GSR_ZBUF_1);
	ad_set(p++, scissor_reg(0, 0, H.vw - 1, H.vh - 1), GSR_SCISSOR_1);
	ad_set(p++, (1ull << 16) | (1ull << 17), GSR_TEST_1); // ZTE, ZTST ALWAYS
	ad_set(p++, 0, GSR_ALPHA_1);
	ad_set(p++, 0, GSR_TEX1_1);
	ad_set(p++, 0, GSR_CLAMP_1);
	ad_set(p++, 0, GSR_TEXFLUSH);
	// The GS draws horizontally. Narrow vertical sprites reduce page changes;
	// integer boundaries partition exactly the original half-open rectangle.
	s = pk_alloc(1 + 4 * strips);
	s[0].d[0] = HWGIF_TAG(2 * strips, 1, 1, PRIM_SPRITE, 0, 2);
	s[0].d[1] = GIF_REGS_RGBA_XYZ;
	s++;
	for (x = 0; x < (u32)H.vw; x += 32u, s += 4)
	{
		const u32 end = x + 32u < (u32)H.vw ? x + 32u : (u32)H.vw;

		s[0].d[0] = (u64)(rgba & 0xFF) | ((u64)((rgba >> 8) & 0xFF) << 32);
		s[0].d[1] = (u64)((rgba >> 16) & 0xFF) | ((u64)(rgba >> 24) << 32);
		s[1].d[0] = (u64)((2048u + x) * 16u) | ((u64)(2048u * 16u) << 32);
		s[1].d[1] = 0; // Z = 0
		s[2] = s[0];
		s[3].d[0] = (u64)((2048u + end) * 16u) | ((u64)((2048 + H.vh) * 16) << 32);
		s[3].d[1] = 0;
	}
	H.gsr.valid = 0;
}

"""

COMPACT_CANDIDATE = """static void gs_fill_all(int buf, int colour, int depth, u32 rgba)
{
	qw_t *p = ad_alloc(8);
	qw_t *s;
	u32 x, strips = ((u32)H.vw + 31u) / 32u;

	ad_set(p++, frame_reg(buf, colour ? 0 : 0xFFFFFFFFu), GSR_FRAME_1);
	ad_set(p++, zbuf_reg(!depth), GSR_ZBUF_1);
	ad_set(p++, scissor_reg(0, 0, H.vw - 1, H.vh - 1), GSR_SCISSOR_1);
	ad_set(p++, (1ull << 16) | (1ull << 17), GSR_TEST_1); // ZTE, ZTST ALWAYS
	ad_set(p++, 0, GSR_ALPHA_1);
	ad_set(p++, 0, GSR_TEX1_1);
	ad_set(p++, 0, GSR_CLAMP_1);
	ad_set(p++, 0, GSR_TEXFLUSH);
	// Set PRIM and the constant colour in PACKED mode, retaining its Q=1.
	// REGLIST then sends two exact XYZ2 registers per 32-pixel strip.
	s = pk_alloc(3 + strips);
	s[0].d[0] = HWGIF_TAG(1, 0, 1, PRIM_SPRITE, 0, 1);
	s[0].d[1] = 1; // RGBAQ
	s[1].d[0] = (u64)(rgba & 0xFF) | ((u64)((rgba >> 8) & 0xFF) << 32);
	s[1].d[1] = (u64)((rgba >> 16) & 0xFF) | ((u64)(rgba >> 24) << 32);
	s[2].d[0] = HWGIF_TAG(2 * strips, 1, 0, 0, 1, 1);
	s[2].d[1] = GIF_REGS_XYZ;
	s += 3;
	for (x = 0; x < (u32)H.vw; x += 32u, s++)
	{
		const u32 end = x + 32u < (u32)H.vw ? x + 32u : (u32)H.vw;

		// REGLIST XYZ2 is X16/Y16/Z32, unlike PACKED XYZ2's 32-bit lanes.
		s->d[0] = (u64)((2048u + x) * 16u) | ((u64)(2048u * 16u) << 16);
		s->d[1] = (u64)((2048u + end) * 16u) | ((u64)((2048 + H.vh) * 16) << 16);
	}
	H.gsr.valid = 0;
}

"""


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def section(text, first, after):
    if text.count(first) != 1:
        raise RuntimeError("Missing/ambiguous source anchor: " + first)
    start = text.index(first)
    return text[start:text.index(after, start)]


def manifest(source):
    return {str(path.relative_to(source)): sha(path)
            for path in sorted((source / "src").rglob("*")) if path.is_file()}


def prepare(stage, encoding):
    if stage.exists():
        raise RuntimeError("Refusing to overwrite an existing stage; choose a fresh --out")
    stage.mkdir(parents=True)
    archive = subprocess.check_output(["git", "archive", REVISION, "src", "tools/ps2/hw_hosttest.py"], cwd=ROOT)
    baseline = stage / "baseline"
    baseline.mkdir()
    with tarfile.open(fileobj=io.BytesIO(archive)) as stream:
        stream.extractall(baseline, filter="data")
    candidate = stage / "candidate"
    shutil.copytree(baseline, candidate)
    before = (baseline / GS_PATH).read_text()
    original = section(before, "static void gs_fill_all(", "// ---- bring-up ----")
    after = before.replace(original, COMPACT_CANDIDATE if encoding == "compact" else CANDIDATE, 1)
    (candidate / GS_PATH).write_text(after)
    patch = "".join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                         fromfile="a/" + str(GS_PATH), tofile="b/" + str(GS_PATH)))
    (stage / "candidate.patch").write_text(patch)
    base_manifest, candidate_manifest = manifest(baseline), manifest(candidate)
    changed = [path for path in base_manifest if base_manifest[path] != candidate_manifest[path]]
    if changed != [str(GS_PATH)]:
        raise RuntimeError("Candidate changed unexpected production sources: " + repr(changed))
    (stage / "baseline-source-manifest.json").write_text(json.dumps(base_manifest, indent=2) + "\n")
    (stage / "candidate-source-manifest.json").write_text(json.dumps(candidate_manifest, indent=2) + "\n")
    return baseline, candidate, original, changed


def make_host(stage, baseline, candidate, original):
    work = stage / "host"
    work.mkdir()
    helper_path = baseline / "tools/ps2/hw_hosttest.py"
    spec = importlib.util.spec_from_file_location("frozen_hw_hosttest", helper_path)
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    helper.ROOT = baseline
    helper.make_shim(work)
    (work / "hw_model.h").write_text((baseline / "src/hardware/hw_model.h").read_text().replace('#include "../doomtype.h"', ""))
    for name in ("ps2_hw_priv.inc", "ps2_hw_regs.inc", "ps2_hwd_dbg.h"):
        body = (baseline / "src/ps2/hw" / name).read_text().replace('#include "../../doomtype.h"', '#include "shim.h"')
        (work / name).write_text(body)
    gs = (baseline / GS_PATH).read_text()
    allocation = section(gs, "static qw_t *pk_alloc(", "// OPT9 (PS2-HW-32)")
    allocation += section(gs, "static qw_t *ad_alloc(", "// Wait until the GS")
    (work / "literal-allocation.inc").write_text(allocation)
    (work / "baseline-clear.inc").write_text(original.replace("gs_fill_all(", "baseline_clear(", 1))
    clear = section((candidate / GS_PATH).read_text(), "static void gs_fill_all(", "// ---- bring-up ----")
    (work / "candidate-clear.inc").write_text(clear.replace("gs_fill_all(", "candidate_clear(", 1))
    fixture = ROOT / "tools/ps2/next13_vram_clear_hosttest.c"
    shutil.copy2(fixture, work / fixture.name)
    shutil.copy2(Path(__file__), stage / Path(__file__).name)
    return work


def build_run(work, name, flags, mutation=0, compact=False):
    binary = work / name
    command = ["gcc", "-std=gnu17", "-O2", "-fwrapv", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter", "-DPS2_PROFILE",
               "-DMUTATION=" + str(mutation), "-DCOMPACT=" + str(int(compact)), "-I" + str(work), *flags,
               str(work / "next13_vram_clear_hosttest.c"), "-o", str(binary), "-lm"]
    built = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    (work / (name + "-build.log")).write_text(" ".join(command) + "\n" + built.stdout + built.stderr)
    if built.returncode:
        raise RuntimeError("Host build failed: " + str(work / (name + "-build.log")))
    run = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True)
    output = run.stdout + run.stderr
    (work / (name + ".log")).write_text(output)
    if mutation:
        if run.returncode == 0 or "clear proof failed:" not in output:
            raise RuntimeError("Mutation did not execute a semantic failure: " + name)
        return {"mutation": mutation, "exit_code": run.returncode, "executed_failure": output.strip(), "binary_sha256": sha(binary)}
    if run.returncode or "runtime error:" in output or "ERROR: AddressSanitizer" in output:
        raise RuntimeError("Host proof failed: " + str(work / (name + ".log")))
    return {"result": json.loads(run.stdout), "binary_sha256": sha(binary)}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov15-next13/vram-clear/stages/packed-v1")
    ap.add_argument("--negative-controls", action="store_true")
    ap.add_argument("--encoding", choices=("packed", "compact"), default="packed")
    args = ap.parse_args()
    stage = args.out.resolve()
    if not stage.is_relative_to(ROOT / "build/nov15-next13"):
        ap.error("--out must stay within build/nov15-next13/")
    baseline, candidate, original, changed = prepare(stage, args.encoding)
    work = make_host(stage, baseline, candidate, original)
    compact = args.encoding == "compact"
    results = {"plain": build_run(work, "plain", [], compact=compact),
               "sanitize": build_run(work, "sanitize", ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"], compact=compact)}
    negative = []
    if args.negative_controls:
        for mutation in range(1, 7):
            negative.append(build_run(work, "mutation" + str(mutation), [], mutation, compact))
    report = {"revision": REVISION, "stage": str(stage), "encoding": args.encoding, "changed_sources": changed,
              "baseline_source_manifest_sha256": sha(stage / "baseline-source-manifest.json"),
              "candidate_source_manifest_sha256": sha(stage / "candidate-source-manifest.json"),
              "source_files": len(manifest(baseline)), "patch_sha256": sha(stage / "candidate.patch"),
              "fixture_sha256": sha(work / "next13_vram_clear_hosttest.c"),
              "driver_sha256": sha(Path(__file__)), "results": results, "negative_controls": negative,
              "EE_compiler_invocations": 0, "emulator_launches": 0, "production_sources_edited": False,
              "scope": "Host packet/allocation/coverage/state proof. No native GS behavior, timing, integrated source-cache history, or VRAM saving claim."}
    (stage / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    evidence = {str(path.relative_to(stage)): sha(path) for path in sorted(stage.rglob("*"))
                if path.is_file() and "__pycache__" not in path.parts}
    (stage / "evidence-manifest.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
