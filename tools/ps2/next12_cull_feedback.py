#!/usr/bin/env python3
"""Execute source-extracted counterexamples: invisible draws affect mip budgets."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from next12_cull_hosttest import between, function
from next12_cull_variant import BASE, ROOT


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


PREFIX = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;
typedef uint32_t UINT32;
typedef struct GLMipmap_s {
 u8 regen_kind,ps2_drop;u16 ps2_pi;u32 ps2_planfr,downloaded;
 struct GLMipmap_s *ps2_twin;
} GLMipmap_t;
#define NOREC -1
#define NCAPS 8
#define PSM_T8 0x13
#define HWDBG_TEX 0x100000
#define HWDBG_IMMDBG 0x40000000
#define HWDBG_LODDBG 0x40000000
#define PLAN_MIN_TEXELS 65536u
#define CONS_Printf(...) ((void)0)
'''

CONTEXT = r'''
static struct {
 texrec_t rec[2];int cur_tex,scr_rec[4];
 u32 frame_no,unplanned_cur,plan_reserve,pool_blocks,plan_last,plan_exact;
 u32 plan_pct,plan_calm,thrash_n,thrash_blk,pred_frame,pred[NCAPS];
 struct {u32 ws_blocks,ws_tex,evictions;}st;
} H;
static struct {u32 need_blk,need_n,need_in_plan,need_out_plan,thrash_n,thrash_blk,unplanned,plan_blk,plan_exact;}TX;
static int ps2hwd_dbg_flags,batch_phase,drop_reason;
static void lru_touch(int i) {(void)i;}
static int img_of(int i) {return H.rec[i].share==NOREC?i:H.rec[i].share;}
static const char *HWR_PS2_TexName(const GLMipmap_t*m) {(void)m;return "known-no255";}
static void lod_account(texrec_t*r) {(void)r;}
static void rec_release(int i) {H.rec[i].used=0;}
static void own_clear(u32 b,u32 n) {(void)b;(void)n;}
static void vram_free(u32 b,u32 n) {(void)b;(void)n;}
static GLMipmap_t owner;
static planinfo_t plan_info[PLAN_MAX];static int plan_n;
static GLMipmap_t *imm_cur[16],*imm_prev[16];static int imm_ncur,imm_nprev;
static float imm_ptotal[16],imm_pseps[16];
static float plan_seps(const planinfo_t*p) {(void)p;abort();}
'''

MAIN = r'''
static void reset(void) {
 memset(&H,0,sizeof H);memset(&TX,0,sizeof TX);memset(&owner,0,sizeof owner);
 H.frame_no=41;H.cur_tex=0;H.pool_blocks=12032;H.plan_pct=75;H.plan_reserve=600;H.unplanned_cur=576;
 owner.regen_kind=1;owner.downloaded=1;
 texrec_t*r=&H.rec[0];r->owner=&owner;r->nblk=64;r->sw=r->sh=r->w=r->h=128;r->psm=PSM_T8;
 r->share=r->sec=r->sec_next=r->pnext=r->ppar=NOREC;r->used=1;r->stamp=H.frame_no;
 for(unsigned i=0;i<NCAPS;i++)r->fp[i]=64;
 imm_ncur=imm_nprev=plan_n=0;
}
static void require(int yes,const char*why) {if(!yes){fprintf(stderr,"FAIL missing coupling witness: %s\n",why);exit(1);}}
int main(void) {
 reset();plan_tex_touch(&H.rec[0]);u32 base_unplanned=H.unplanned_cur;source_reserve_update();u32 base_reserve=H.plan_reserve,base_budget=source_next_budget();
 reset();u32 cur_unplanned=H.unplanned_cur;source_reserve_update();u32 cur_reserve=H.plan_reserve,cur_budget=source_next_budget();
 require(base_unplanned!=cur_unplanned&&base_reserve!=cur_reserve&&base_budget!=cur_budget,"plan_tex_touch -> unplanned -> reserve -> next budget");
 printf("{\"kind\":\"unplanned_reserve\",\"base_unplanned\":%u,\"skip_unplanned\":%u,\"base_reserve\":%u,\"skip_reserve\":%u,\"base_next_budget\":%u,\"skip_next_budget\":%u}\n",base_unplanned,cur_unplanned,base_reserve,cur_reserve,base_budget,cur_budget);
 reset();H.thrash_n=3;H.thrash_blk=192;tex_drop(0,1);u32 base_thrash=H.thrash_blk;plan_frame_end();u32 base_pct=H.plan_pct;source_reserve_update();base_budget=source_next_budget();
 reset();H.thrash_n=3;H.thrash_blk=192;u32 cur_thrash=H.thrash_blk;plan_frame_end();u32 cur_pct=H.plan_pct;source_reserve_update();cur_budget=source_next_budget();
 require(base_thrash!=cur_thrash&&base_pct!=cur_pct&&base_budget!=cur_budget,"tex_drop -> thrash -> plan_pct -> next budget");
 printf("{\"kind\":\"thrash_pct\",\"base_thrash_blk\":%u,\"skip_thrash_blk\":%u,\"base_plan_pct\":%u,\"skip_plan_pct\":%u,\"base_next_budget\":%u,\"skip_next_budget\":%u}\n",base_thrash,cur_thrash,base_pct,cur_pct,base_budget,cur_budget);
 puts("NEXT12_CULL_FEEDBACK REJECTION_CONFIRMED");
 return 0;
}
'''


def prepare(work, negative=0):
    work.mkdir(parents=True, exist_ok=True)
    private = (BASE / "src/ps2/hw/ps2_hw_priv.inc").read_text()
    draw = (BASE / "src/ps2/hw/ps2_hw_draw.inc").read_text()
    tex = (BASE / "src/ps2/hw/ps2_hw_tex.inc").read_text()
    plan = (BASE / "src/ps2/hw/ps2_hw_plan.inc").read_text()
    gs = (BASE / "src/ps2/hw/ps2_hw_gs.inc").read_text()
    record = between(private, "typedef struct\n{\n\tGLMipmap_t *owner;", "\n#define NOREC")
    pi = between(plan, "typedef struct\n{\n\tGLMipmap_t *key;", "static planinfo_t (*plan_info_p)")
    constants = "\n".join(line for line in plan.splitlines() if re.match(r"#define PLAN_(MAX|BINS|PCT_MAX|PCT_MIN|PCT_DOWN|CALM_FRAMES|THRASH_BLK)\b", line)) + "\n"
    touch = function(draw, "plan_tex_touch")
    drop = function(tex, "tex_drop")
    end = function(plan, "plan_frame_end")
    reserve = between(gs, "\t\t// PS2-HW-37: the room the frame plan leaves", "\n\t}\n\tif (ps2hwd_dbg_flags & HWDBG_TEX)")
    next_budget = between(function(plan, "PS2HWD_PlanEnd"), "\tif (!H.plan_pct)", "\tfor (i = 0; i < plan_n; i++)")
    if negative == 1:
        touch = touch.replace("H.unplanned_cur += im->nblk;", "H.unplanned_cur += 0;")
    if negative == 2:
        drop = drop.replace("H.thrash_blk += r->nblk;", "H.thrash_blk += 0;")
    body = PREFIX + constants + record + pi + CONTEXT + touch + drop + end
    body += "static void source_reserve_update(void) {\n" + reserve + "\n}\n"
    body += "static u32 source_next_budget(void) { u32 budget;\n" + next_budget + "return budget;\n}\n" + MAIN
    path = work / "feedback-production.c"
    path.write_text(body)
    (work / "extraction.json").write_text(json.dumps({
        "negative": negative, "generated_source_sha256": sha(path),
        "source_paths": {str(p.relative_to(ROOT)): sha(p) for p in (BASE / "src/ps2/hw/ps2_hw_priv.inc", BASE / "src/ps2/hw/ps2_hw_draw.inc", BASE / "src/ps2/hw/ps2_hw_tex.inc", BASE / "src/ps2/hw/ps2_hw_plan.inc", BASE / "src/ps2/hw/ps2_hw_gs.inc")},
        "exact_functions": ["plan_tex_touch", "tex_drop", "plan_frame_end"],
        "exact_blocks": ["frame-end plan_reserve update", "PS2HWD_PlanEnd next budget"],
        "scope": "Host controlled counterexample using exact production functions/blocks and full texrec type. Minimal H/TX state and no-op external sinks; no allocator reachability, EE ABI, native timing, raster or shipping claim. The two valid 128x128 P8/64-block traces establish budget-output dependence."
    }, indent=2) + "\n")


def run(work, negative=0, sanitize=False):
    prepare(work, negative)
    binary = work / "feedback"
    flags = ["-std=gnu17", "-O2", "-fwrapv", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable"]
    if sanitize:
        flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
    c = subprocess.run(["gcc", *flags, str(work / "feedback-production.c"), "-o", str(binary)], text=True, capture_output=True)
    (work / "build.log").write_text(c.stdout + c.stderr)
    if c.returncode:
        raise RuntimeError("Host feedback build failed: " + c.stderr[-4000:])
    r = subprocess.run([str(binary)], text=True, capture_output=True)
    (work / "run.log").write_text(r.stdout + r.stderr)
    if r.returncode != (1 if negative else 0):
        raise RuntimeError("Counterexample/negative failed: " + r.stdout + r.stderr)
    traces = [json.loads(line) for line in r.stdout.splitlines() if line.startswith("{")]
    return {"negative": negative, "sanitize": sanitize, "rc": r.returncode,
            "binary_sha256": sha(binary), "traces": traces, "output": r.stdout + r.stderr,
            "log": str(work / "run.log")}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov15-next12/cull/feedback")
    args = ap.parse_args()
    work = args.out.resolve()
    rows = [run(work / "positive"), run(work / "sanitize", sanitize=True)]
    rows += [run(work / ("negative-" + str(n)), negative=n) for n in (1, 2)]
    report = {"status": "rejected_resource_optimization", "reason": "Omitting a fully invisible known-no255 draw can alter future visible mip budgets through both unplanned reserve and thrash feedback.",
              "rows": rows, "compiler": "host GCC only", "ee_or_emulator_used": False,
              "shipping_or_fps_or_actual_byte_saving_claim": False,
              "proof_tool_sha256": sha(Path(__file__))}
    (work / "feedback-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
