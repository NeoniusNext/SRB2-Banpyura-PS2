#!/usr/bin/env python3
"""Compare literal604cfd150 and selected-source PlanEnd + its arithmetic dependencies.

The only instrumentation records bounds/work counts and exposes final state.
The two executable negative controls mutate disposable extracted headers only.
No EE tools or emulator are used, and production files are never written.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from hw_front_cull_hosttest import function as source_function

ROOT = Path(__file__).resolve().parents[2]
BASE = "604cfd150c5240142d8cb5367950b53d987916d1"
PLAN = "src/ps2/hw/ps2_hw_plan.inc"
TEX = "src/ps2/hw/ps2_hw_tex.inc"
PRIV = "src/ps2/hw/ps2_hw_priv.inc"
FUNCTIONS = {
    PLAN: ("plan_bin_hi", "plan_seps", "plan_lmax", "plan_blocks", "PS2HWD_PlanEnd"),
    TEX: ("tex_footprint", "tex_plan_blocks", "lod_level_for"),
}
REVIEWED_P4_FOOTPRINT_CASE = """#ifndef PS2_NOOPT_P4_BAND
		case PSM_T4:
			lastpage = (h >> 7) * ((tbw + 1) >> 1) + (w >> 7);
			maxblk = blk_16[(h >> 4) & 7][(w >> 5) & 3];
			break;
#endif
"""


def function(source, name):
    # plan_bin_hi has a trailing signature comment. Mask comments in place for
    # the existing balanced-brace parser, then return the exact original bytes.
    pattern = r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/'
    masked = re.sub(pattern, lambda m: m[0] if m[0].startswith(('"', "'")) else re.sub(r"[^\n]", " ", m[0]), source)
    extracted = source_function(masked, name)[:-1]
    start = masked.index(extracted)
    return source[start:start + len(extracted)] + "\n"


def digest(text):
    return hashlib.sha256(text.encode()).hexdigest()


def canonical(text):
    # Keep string/character literals intact while removing comments and spacing.
    pattern = r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/|\s+'
    return re.sub(pattern, lambda m: m[0] if m[0].startswith(('"', "'")) else "", text)


def read_sources(revision=None, source_root=ROOT):
    return {
        path: subprocess.check_output(["git", "show", revision + ":" + path], cwd=ROOT, text=True)
        if revision else (source_root / path).read_text()
        for path in (PLAN, TEX, PRIV)
    }


def define(source, name):
    match = re.search(r"^#define " + re.escape(name) + r"\b[^\n]*", source, re.M)
    if not match:
        raise RuntimeError("missing production define " + name)
    return match[0]


def replace_one(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError("missing unique instrumentation/mutation anchor: " + old)
    return text.replace(old, new, 1)


def footprint_tables(tex):
    tables = re.search(r"^static const u8 blk_32_8\[4\]\[8\].*?^static u32 tex_footprint", tex, re.M | re.S)
    if not tables:
        raise RuntimeError("missing GS footprint tables")
    return tables[0][:tables[0].rindex("static u32 tex_footprint")]


def check_expected_change(old, new):
    old_end = function(old[PLAN], "PS2HWD_PlanEnd")
    expected = old_end.replace("blk_l[ENT_MAX][8]", "blk_l[ENT_MAX][PLAN_LEVEL_MAX + 1]")
    expected = replace_one(expected, "l <= cap[i] + 1 && l < 8", "l <= cap[i]")
    if canonical(expected) != canonical(function(new[PLAN], "PS2HWD_PlanEnd")):
        raise RuntimeError("PlanEnd contains a change outside the two reviewed scratch edits")
    for path, names in FUNCTIONS.items():
        for name in names:
            if name == "PS2HWD_PlanEnd":
                continue
            expected = function(old[path], name)
            if name == "plan_lmax":
                expected = replace_one(expected, "int lv = 5;", "int lv = PLAN_LEVEL_MAX;")
            if name == "tex_footprint" and canonical(expected) != canonical(function(new[path], name)):
                # Final integration adds exactly this guarded P4 case. Preserve
                # every pre-existing T8/CT path rather than skipping the check.
                expected = replace_one(expected, "\t\tcase PSM_T8:", REVIEWED_P4_FOOTPRINT_CASE + "\t\tcase PSM_T8:")
            if canonical(expected) != canonical(function(new[path], name)):
                raise RuntimeError("unexpected arithmetic dependency change: " + name)
    if canonical(footprint_tables(old[TEX])) != canonical(footprint_tables(new[TEX])):
        raise RuntimeError("unexpected GS footprint table change")
    if "case PSM_T4:" in function(new[TEX], "tex_footprint") and canonical(define(new[PRIV], "PSM_T4")) != "#definePSM_T40x14":
        raise RuntimeError("unexpected PSM_T4 constant")
    if canonical(define(new[PLAN], "PLAN_LEVEL_MAX")) != "#definePLAN_LEVEL_MAX5":
        raise RuntimeError("PLAN_LEVEL_MAX must remain five")
    for name in re.findall(r"^#define (PLAN_\w+)\b", old[PLAN], re.M):
        if canonical(define(old[PLAN], name)) != canonical(define(new[PLAN], name)):
            raise RuntimeError("unexpected planner constant change: " + name)
    for name in ("PSM_T8", "PSM_CT32", "PSM_CT16", "PSM_CT16S", "PAGE_BLOCKS", "PLAN_FULL_KEEP"):
        if canonical(define(old[PRIV], name)) != canonical(define(new[PRIV], name)):
            raise RuntimeError("unexpected footprint/keep constant change: " + name)


def source_guard_controls(old, new):
    controls = []
    footprint = function(new[TEX], "tex_footprint")
    variants = [("changed-old-T8-path", "lastpage = (h >> 6) * ((tbw + 1) >> 1)", "lastpage = (h >> 7) * ((tbw + 1) >> 1)")]
    if "case PSM_T4:" in footprint:
        variants.append(("changed-reviewed-P4-case", "lastpage = (h >> 7) * ((tbw + 1) >> 1)", "lastpage = (h >> 6) * ((tbw + 1) >> 1)"))
    for name, before, after in variants:
        changed = replace_one(footprint, before, after)
        sources = dict(new)
        sources[TEX] = replace_one(new[TEX], footprint, changed)
        try:
            check_expected_change(old, sources)
        except RuntimeError as error:
            if str(error) != "unexpected arithmetic dependency change: tex_footprint":
                raise
            controls.append({"name": name, "rejected": True, "reason": str(error)})
        else:
            raise RuntimeError("source guard accepted an unreviewed footprint edit: " + name)
    return controls


def extract(source, mutation=None):
    plan, tex, priv = (source[p] for p in (PLAN, TEX, PRIV))
    names = re.findall(r"^#define (PLAN_\w+)\b", plan, re.M)
    macros = [define(plan, name) for name in names]
    macros += [define(priv, name) for name in ("PSM_T8", "PSM_CT32", "PSM_CT16", "PSM_CT16S", "PAGE_BLOCKS", "PLAN_FULL_KEEP", "HWDBG_LODDBG")]
    if "case PSM_T4:" in function(tex, "tex_footprint"):
        macros.append(define(priv, "PSM_T4"))
    macros += [define(plan, "IMM_MAX")]
    layout = re.search(r"typedef struct\s*\{[^{}]*\}\s*planinfo_t;", plan)
    if not layout:
        raise RuntimeError("missing planinfo_t")
    tables = footprint_tables(tex)
    end = function(plan, "PS2HWD_PlanEnd")
    if mutation == "cap-plus-one-oob":
        end = replace_one(end, "l <= cap[i];", "l <= cap[i] + 1;")
    elif mutation == "missing-cap-cost":
        end = replace_one(end, "l <= cap[i];", "l < cap[i];")
    end = replace_one(end, "int i, n = 0, step;", "int i, n = 0, step;\n\tfixture_output->scratch_bytes = sizeof blk_l;")
    end = replace_one(end, "exact_sum = sum;", """exact_sum = sum;
	fixture_output->entries = (uint16_t)n;
	memcpy(fixture_output->start_level, lx, (size_t)n);
	memcpy(fixture_output->cap, cap, (size_t)n);""")
    parts = [
        *macros, layout[0], tables,
        function(tex, "tex_footprint"), function(tex, "tex_plan_blocks"), function(tex, "lod_level_for"),
        function(plan, "plan_bin_hi"), function(plan, "plan_seps"), function(plan, "plan_lmax"), function(plan, "plan_blocks"),
        """static int plan_active, plan_n, plan_nofid, imm_nprev;
static uint32_t plan_t0;
static planinfo_t (*plan_info_p)[PLAN_MAX];
#define plan_info (*plan_info_p)
static GLMipmap_t *imm_prev[IMM_MAX];
static float imm_ptotal[IMM_MAX], imm_pseps[IMM_MAX];
static u32 observe_plan_blocks(const GLMipmap_t *m, int lv)
{
	u32 cost;
	if (lv < 0 || lv >= 8) abort();
	cost = plan_blocks(m, lv);
	fixture_output->cost_calls++;
	fixture_output->calls_by_level[lv]++;
	if (cost > fixture_output->max_cost) fixture_output->max_cost = cost;
	return cost;
}
#define plan_blocks observe_plan_blocks
""", end,
    ]
    return "\n".join(parts)


def wrapper(prefix):
    return r'''/* Generated: scalar host stand-ins only, production math is in extracted.inc. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "plan_scratch_opt16_hosttest.h"
typedef uint8_t u8;
typedef uint32_t u32;
typedef struct GLMipmap_s {
	uint16_t width, height;
	uint32_t ps2_full_fr, ps2_ihfr;
	uint8_t ps2_want, ps2_ihint;
	struct GLMipmap_s *nextcolormap;
} GLMipmap_t;
typedef struct { unsigned dx, nblk; } texrec_t;
static struct { u32 frame_no, pool_blocks, plan_reserve, plan_pct, plan_last, plan_exact; } H;
static struct { u32 plan_cyc; } TX;
static u32 ps2hwd_dbg_flags;
static test_output_t *fixture_output;
static u32 cyc(void) { return 100; }
static const texrec_t *rec_of(const GLMipmap_t *m) { (void)m; return NULL; }
#define CONS_Printf(...) ((void)fixture_output->debug_calls++)
#define PS2HWD_PlanEnd @PREFIX@_PlanEnd
#include "extracted.inc"

int @PREFIX@_lmax(uint16_t width, uint16_t height)
{
	GLMipmap_t m = {0};
	m.width = width; m.height = height;
	return plan_lmax(&m);
}

void @PREFIX@_run(const test_input_t *in, test_output_t *out)
{
	static GLMipmap_t texture[TEST_ENT_MAX];
	static planinfo_t information[PLAN_MAX];
	unsigned i;
	memset(texture, 0, sizeof texture);
	memset(information, 0, sizeof information);
	memset(out, 0, sizeof *out);
	memset(&H, 0, sizeof H);
	TX.plan_cyc = 19;
	fixture_output = out;
	for (i = 0; i < TEST_ENT_MAX; i++)
	{
		texture[i].width = in->texture[i].width;
		texture[i].height = in->texture[i].height;
		texture[i].ps2_full_fr = in->texture[i].full_frame;
		texture[i].ps2_want = in->texture[i].want;
		texture[i].ps2_ihint = in->texture[i].hint;
		texture[i].ps2_ihfr = in->texture[i].hint_frame;
	}
	for (i = 0; i < (unsigned)in->plan_count; i++)
	{
		information[i].key = &texture[in->plan[i].key];
		information[i].total = in->plan[i].total;
		information[i].npoly = in->plan[i].polygons;
		memcpy(information[i].hist, in->plan[i].hist, sizeof information[i].hist);
	}
	for (i = 0; i < (unsigned)in->immediate_count; i++)
	{
		imm_prev[i] = &texture[in->immediate[i].key];
		imm_ptotal[i] = in->immediate[i].total;
		imm_pseps[i] = in->immediate[i].seps;
	}
	H.frame_no = in->frame;
	H.pool_blocks = in->pool_blocks;
	H.plan_reserve = in->reserve;
	H.plan_pct = in->percentage;
	plan_active = in->active;
	plan_n = in->plan_count;
	plan_nofid = in->no_fidelity;
	plan_info_p = (planinfo_t (*)[PLAN_MAX])&information;
	imm_nprev = in->immediate_count;
	ps2hwd_dbg_flags = in->debug ? HWDBG_LODDBG : 0;
	plan_t0 = 3;
	PS2HWD_PlanEnd();
	out->last = H.plan_last;
	out->exact = H.plan_exact;
	out->percentage = H.plan_pct;
	out->plan_cycles = TX.plan_cyc;
	out->active = (uint8_t)plan_active;
	for (i = 0; i < TEST_ENT_MAX; i++)
	{
		out->want[i] = texture[i].ps2_want;
		out->hint[i] = texture[i].ps2_ihint;
		out->hint_frame[i] = texture[i].ps2_ihfr;
	}
}
'''.replace("@PREFIX@", prefix)


def execute(command, log, env=None):
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, env=env)
    log.write_text(" ".join(command) + "\n" + result.stdout + result.stderr)
    return result


def build_run(directory, baseline, candidate, compiler, flags):
    directory.mkdir(parents=True, exist_ok=True)
    commands = []
    objects = []
    for prefix, header in (("baseline", baseline), ("candidate", candidate)):
        part = directory / prefix
        part.mkdir(exist_ok=True)
        (part / "extracted.inc").write_text(header)
        source = part / "wrapper.c"
        source.write_text(wrapper(prefix))
        obj = part / "planner.o"
        command = [compiler, *flags, "-I" + str(ROOT / "tools/ps2"), "-c", str(source), "-o", str(obj)]
        commands.append(command)
        result = execute(command, part / "compile.log")
        if result.returncode:
            raise RuntimeError("host compile failed: " + str(part / "compile.log"))
        objects.append(str(obj))
    binary = directory / "plan_scratch_hosttest"
    command = [compiler, *flags, str(ROOT / "tools/ps2/plan_scratch_opt16_hosttest.c"), *objects, "-o", str(binary), "-lm"]
    commands.append(command)
    result = execute(command, directory / "link.log")
    if result.returncode:
        raise RuntimeError("host link failed: " + str(directory / "link.log"))
    (directory / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    result = execute([str(binary)], directory / "run.log", env)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "build/opt16/plan-scratch-host")
    parser.add_argument("--source-root", type=Path, default=ROOT, help="candidate root containing src/; defaults to the working tree")
    args = parser.parse_args()
    work = args.out.resolve()
    work.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which("gcc") or shutil.which("cc")
    if not compiler:
        raise RuntimeError("host C compiler unavailable")
    source_root = args.source_root.resolve()
    old, new = read_sources(BASE), read_sources(source_root=source_root)
    working = read_sources()
    check_expected_change(old, new)
    baseline, candidate = extract(old), extract(new)
    report = {
        "baseline": BASE,
        "compiler": subprocess.check_output([compiler, "--version"], text=True).splitlines()[0],
        "candidate_source_root": str(source_root),
        "candidate_sources": {path: digest(text) for path, text in new.items()},
        "working_tree_sources": {path: digest(text) for path, text in working.items()},
        "candidate_matches_working_tree": new == working,
        "planner_cost_psm": {"macro": "PSM_T8", "value": "0x13", "candidate_P4_case_enabled": "case PSM_T4:" in function(new[TEX], "tex_footprint")},
        "source_guard_negative_controls": source_guard_controls(old, new),
        "baseline_sources": {path: digest(text) for path, text in old.items()},
        "extracted_functions": {name: {"baseline": digest(function(old[path], name)), "candidate": digest(function(new[path], name))} for path, names in FUNCTIONS.items() for name in names},
        "fixture_sources": {name: digest((ROOT / "tools/ps2" / name).read_text()) for name in ("plan_scratch_opt16_hosttest.c", "plan_scratch_opt16_hosttest.h", "plan_scratch_opt16_hosttest.py")},
        "runs": [],
        "limits": "Host decision/memory proof; no EE timing or whole-game visual claim.",
    }
    flags = ["-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-but-set-variable", "-ffp-contract=off", "-fno-strict-aliasing"]
    sanitized = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-sanitize-recover=all", "-no-pie"]
    for name, mutation, extra in (
        ("plain", None, []),
        ("sanitized", None, sanitized),
        ("missing-cap-cost", "missing-cap-cost", []),
        ("cap-plus-one-oob", "cap-plus-one-oob", sanitized),
    ):
        result = build_run(work / name, baseline, extract(new, mutation) if mutation else candidate, compiler, flags + extra)
        if mutation == "missing-cap-cost":
            passed = result.returncode == 1 and "PLAN SCRATCH FAIL: working-set blocks differ" in result.stderr
        elif mutation == "cap-plus-one-oob":
            passed = result.returncode != 0 and ("index 6 out of bounds" in result.stderr or "global-buffer-overflow" in result.stderr)
        else:
            passed = result.returncode == 0 and "runtime error:" not in result.stderr and "ERROR: AddressSanitizer" not in result.stderr
        item = {"name": name, "expected_failure": bool(mutation), "exit": result.returncode, "passed": passed, "log": str(work / name / "run.log")}
        if passed and mutation:
            item["observed_failure"] = result.stderr.strip()
        elif passed:
            item["results"] = json.loads(result.stdout)
        report["runs"].append(item)
        (work / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print(name + ": " + ("PASS" if passed else "FAIL"))
        if not passed:
            print(result.stdout + result.stderr)
            return 1
    nm = shutil.which("nm")
    if not nm:
        raise RuntimeError("nm unavailable for compiled scratch-symbol verification")
    report["compiled_scratch_symbols"] = {}
    for prefix, expected in (("baseline", 8704), ("candidate", 6528)):
        obj = work / "plain" / prefix / "planner.o"
        symbols = subprocess.check_output([nm, "-S", "--size-sort", str(obj)], text=True)
        (obj.parent / "symbols.txt").write_text(symbols)
        match = re.search(r"^[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+[bB]\s+(blk_l(?:\.\d+)?)$", symbols, re.M)
        if not match or int(match[1], 16) != expected:
            raise RuntimeError("compiled scratch symbol has an unexpected size: " + prefix)
        report["compiled_scratch_symbols"][prefix] = {"symbol": match[2], "bytes": expected}
    report["passed"] = True
    (work / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["runs"][0]["results"], sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
