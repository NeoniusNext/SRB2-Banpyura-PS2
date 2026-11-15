"""Reproduce frame-ID reuse across a same-level HW restart; no production edit.

Exit zero means the expected bug was reproduced, or that --expect-correct passed
for an isolated candidate. GS bring-up is simulated; engine texture metadata is
retained while actual planner/reset state is exercised.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[3]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source-root", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--expect-correct", action="store_true", help="require both stale-state faults to be absent in an isolated candidate")
    ap.add_argument("--sanitize", action="store_true", help="compile with ASan/UBSan")
    a = ap.parse_args()
    spec = importlib.util.spec_from_file_location("highres_state", Path(__file__).with_name("highres_state_hosttest.py"))
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    source, work = a.source_root.resolve(), a.out.resolve()
    helper.prepare(source, work, "")
    plan = (source / "src/ps2/hw/ps2_hw_plan.inc").read_text()
    prefix = plan[plan.index("static int plan_active;"):plan.index("void PS2HWD_PlanBegin(void)")]
    immediate = plan[plan.index("#define IMM_MAX 16"):plan.index("// the engine drops its textures")]
    start = plan.index("static planinfo_t *plan_key(")
    end = plan.index("static inline void plan_add(", start)
    wanted = helper.function(plan, "tex_want").replace(" tex_want(", " epoch_tex_want(")
    code = prefix + immediate + plan[start:end] + helper.function(plan, "plan_reset") + "static int imm_level = -1;\n" + wanted
    (work / "epoch-production.inc").write_text(code)
    driver = (source / "src/ps2/hw/ps2_hwd.c").read_text()
    reset = r"(?:\thw_frame_clock = H.frame_no \+ 1u;\n)?\tmemset\(&H, 0, sizeof H\);(?:\n\tH.frame_no = hw_frame_clock;)?"
    declaration = "static u32 hw_frame_clock;\n" if "static u32 hw_frame_clock;" in driver else ""
    begin = re.findall(reset, helper.function(driver, "PS2HWD_Init"))
    end = re.findall(reset, helper.function(driver, "PS2HWD_Shutdown"))
    if len(begin) != 1 or len(end) != 1:
        raise RuntimeError("state reset contract missing/ambiguous")
    lifecycle = declaration + "static void epoch_state_init(void)\n{\n" + begin[0] + "\n}\nstatic void epoch_state_shutdown(void)\n{\n" + end[0] + "\n}\n"
    (work / "epoch-lifecycle.inc").write_text(lifecycle)
    gs = (source / "src/ps2/hw/ps2_hw_gs.inc").read_text()
    if "plan_advance(1u);" in gs:
        lifecycle += "static void epoch_tick_frames(u32 count) { while (count--) { H.frame_no++; plan_advance(1u); } }\nstatic void epoch_fixture_clock_init(void) { plan_clock = 1; }\n"
    elif "plan_clock++;" in gs:
        lifecycle += "static void epoch_tick_frames(u32 count) { H.frame_no += count; plan_clock += count; }\nstatic void epoch_fixture_clock_init(void) { plan_clock = 0; }\n"
    else:
        lifecycle += "static void epoch_tick_frames(u32 count) { H.frame_no += count; }\nstatic void epoch_fixture_clock_init(void) {}\n"
    hint = re.search(r"if \(verts && (k->ps2_ihfr[^\n]+)\)\n", plan)
    if not hint:
        raise RuntimeError("immediate hint validity expression missing")
    lifecycle += "static int epoch_hint_live(const GLMipmap_t *k) { return " + hint[1] + "; }\n"
    if "plan_advance(1u);" in gs:
        lifecycle += r'''
static void epoch_wrap_test(void)
{
    unsigned failures = 0, cases = 0;
    plan_info_p = calloc(1, sizeof *plan_info_p);
    if (!plan_info_p) exit(11);
    for (u32 offset = 0; offset <= 160; offset++)
    {
        GLMipmap_t m;
        int visible;
        u32 old;
        memset(&m, 0, sizeof m);
        plan_clock = ~0u - offset;
        old = plan_stamp();
        plan_advance(1u);
        failures += !old || !plan_stamp() || plan_stamp() - old < 1u || plan_stamp() - old > 2u;
        cases++;
        plan_clock = old;
        m.ps2_planfr = m.ps2_full_fr = m.ps2_ihfr = old;
        plan_reset();
        failures += !plan_stamp() || plan_stamp() - old < PLAN_FULL_KEEP;
        failures += plan_key(&m) == NULL;
        m.ps2_want = 3; m.ps2_vis = 1; batch_phase = 2;
        failures += epoch_tex_want(&m, &visible) != 3 || epoch_hint_live(&m);
        cases++;
    }
    free(plan_info_p);
    printf("HIGHRES WRAP tested=1 failures=%u cases=%u\n", failures, cases);
}
'''
    else:
        lifecycle += 'static void epoch_wrap_test(void) { printf("HIGHRES WRAP tested=0 failures=0 cases=0\\n"); }\n'
    (work / "epoch-lifecycle.inc").write_text(lifecycle)
    binary = work / "probe"
    command = ["cc", "-std=gnu17", "-O2", "-g", "-fwrapv", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
               "-Wno-unused-variable", "-Wno-unused-parameter", "-Wno-unused-but-set-variable", "-DPS2_PROFILE", "-I" + str(work),
               "-I" + str(source / "src/hardware"), str(Path(__file__).with_suffix(".c")), "-o", str(binary), "-lm"]
    if a.sanitize:
        command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
    result = subprocess.run(command, text=True, capture_output=True)
    (work / "build.log").write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError("build failed: " + str(work / "build.log"))
    result = subprocess.run([str(binary)], text=True, capture_output=True)
    (work / "test.log").write_text(result.stdout + result.stderr)
    names = ("stale_registration", "stale_full_hint", "stale_immediate_hint", "delayed_full_revival", "delayed_immediate_revival", "planned_lod_without_stale_hint", "frame_no")
    fields = re.findall(r"case=(\w+) " + " ".join(name + r"=(\d+)" for name in names), result.stdout)
    faults = {row[0]: dict(zip(names, map(int, row[1:]))) for row in fields}
    wrap_fields = re.search(r"HIGHRES WRAP tested=(\d+) failures=(\d+) cases=(\d+)", result.stdout)
    wrap = dict(zip(("tested", "failures", "cases"), map(int, wrap_fields.groups()))) if wrap_fields else None
    correct = result.returncode == 0 and len(faults) == 3 and all(not any(row[name] for name in names[:5]) and row["planned_lod_without_stale_hint"] == 3 for row in faults.values()) and wrap and wrap["tested"] == 1 and wrap["failures"] == 0 and wrap["cases"] == 322
    buggy = result.returncode == 0 and len(faults) == 3 and any(any(row[name] for name in names[:5]) for row in faults.values())
    report = {"scope": __doc__, "bug_reproduced": buggy, "correct": correct, "expected_correct": a.expect_correct,
              "faults": faults, "wrap": wrap, "output": result.stdout, "stderr": result.stderr, "rc": result.returncode, "sanitize": a.sanitize,
              "GS_init_shutdown_simulated": True, "actual_init_shutdown_state_reset_statements": lifecycle, "production_source_modified": False}
    (work / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return int(not (correct if a.expect_correct else buggy))


if __name__ == "__main__":
    raise SystemExit(main())
