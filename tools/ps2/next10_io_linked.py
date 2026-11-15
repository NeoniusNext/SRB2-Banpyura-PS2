#!/usr/bin/env python3
"""Expose, link and summarize the actual complete-engine LTO cache helper.

Only ELF symbol metadata changes in exposed object copies; all allocated
section contents/sizes are asserted unchanged. Emulator runs remain explicit.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

from audio_refill_next6_linked_check import ROOT, allocated_sections, run


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def summarize(out):
    versions = {}
    for kind in ("baseline", "candidate"):
        log = (out / kind / "run.log").read_text()
        assert "CACHE10 FAIL" not in log
        rows = [dict(re.findall(r"(\w+)=([\da-f]+)", line)) for line in log.splitlines() if "CACHE10 ROW " in line]
        done = re.search(r"CACHE10 DONE rows=(\d+) digest=([\da-f]+)", log)
        assert done and len(rows) == int(done[1]) == 350
        versions[kind] = rows
    rows = []
    for a, b in zip(versions["baseline"], versions["candidate"]):
        assert {k: v for k, v in a.items() if k != "count"} == {k: v for k, v in b.items() if k != "count"}
        row = {k: int(v) for k, v in a.items() if k not in ("count", "digest", "result")}
        row.update(result=a["result"], digest=a["digest"], baseline_count=int(a["count"]), candidate_count=int(b["count"]))
        row["reduction_pct"] = (row["baseline_count"] - row["candidate_count"]) * 100 / row["baseline_count"]
        rows.append(row)
    groups = {}
    selectors = {"all": lambda r: True, "first_hit": lambda r: r["depth"] == 0,
                 "miss": lambda r: r["depth"] == 64, "short": lambda r: r["mode"] < 2,
                 "long": lambda r: r["mode"] >= 2, "collisions": lambda r: r["collide"],
                 "no_collisions": lambda r: not r["collide"]}
    for name, select in selectors.items():
        selected = [row for row in rows if select(row)]
        a = sum(row["baseline_count"] for row in selected)
        b = sum(row["candidate_count"] for row in selected)
        groups[name] = {"rows": len(selected), "baseline_count": a, "candidate_count": b,
                        "reduction_pct": (a - b) * 100 / a,
                        "faster": sum(row["candidate_count"] < row["baseline_count"] for row in selected),
                        "slower": sum(row["candidate_count"] > row["baseline_count"] for row in selected),
                        "ties": sum(row["candidate_count"] == row["baseline_count"] for row in selected)}
    report = {"groups": groups, "rows": rows, "inputs": json.loads((out / "inputs.json").read_text())}
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(groups, indent=2))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov15-next10/io/linked")
    ap.add_argument("--baseline-object", type=Path, default=ROOT / "build/nov15-next10/baseline/software/engine_lto.o")
    ap.add_argument("--candidate-object", type=Path, default=ROOT / "build/nov15-next10/io/software/engine_lto.o")
    ap.add_argument("--link-report", type=Path, default=ROOT / "build/nov15-next10/baseline/software/build-report.json")
    ap.add_argument("--summarize", action="store_true")
    a = ap.parse_args()
    a.out = a.out.resolve()
    assert a.out.is_relative_to(ROOT / "build")
    if a.summarize:
        summarize(a.out)
        return
    a.out.mkdir(parents=True, exist_ok=True)
    source = ROOT / "tools/ps2/next10_io_linked.c"
    report = json.loads(a.link_report.read_text())
    command = report["link_command"]
    includes = [f for f in report["flags"] if f.startswith("-I")]
    flags = ["-O2", "-G0", "-std=gnu17", "-Wall", "-Wextra", "-Werror", "-D_EE", "-DPS2", *includes]
    driver = a.out / "driver.o"
    compile_command = [command[0], *flags, "-c", source, "-o", driver]
    (a.out / "driver-build.log").write_text(run(compile_command))
    inputs = {"driver_sha256": sha(source), "driver_object_sha256": sha(driver),
              "compile_command": list(map(str, compile_command)), "link_report_sha256": sha(a.link_report)}
    for kind, original in (("baseline", a.baseline_object), ("candidate", a.candidate_object)):
        work = a.out / kind
        work.mkdir(exist_ok=True)
        saved = work / "engine_original.o"
        shutil.copyfile(original, saved)
        exposed = work / "engine_exposed.o"
        names = {"CheckLumpInCache": "Engine_CheckLumpInCache", "lumpnumcache": "Engine_Cache", "lumpnumcacheindex": "Engine_Cursor"}
        run(["mips64r5900el-ps2-elf-objcopy", *["--redefine-sym=" + x + "=" + y for x, y in names.items()], saved, exposed])
        run(["mips64r5900el-ps2-elf-objcopy", *["--globalize-symbol=" + y for y in names.values()], exposed])
        before = allocated_sections(saved)
        assert before == allocated_sections(exposed)
        dis = run(["mips64r5900el-ps2-elf-objdump", "-dr", "-j", ".text.CheckLumpInCache", saved])
        (work / "cache.dis").write_text(dis)
        link = []
        for word in command:
            if word.endswith("/engine_lto.o"):
                link += [str(exposed), str(driver), "-Wl,--wrap=main"]
            elif word.endswith("/SRB2.ELF"):
                link.append(str(work / "CACHE_LINKED10.ELF"))
            elif word.startswith("-Wl,-Map="):
                link.append("-Wl,-Map=" + str(work / "link.map"))
            else:
                link.append(word)
        output = run(link)
        (work / "link.log").write_text(output)
        other = "\n".join(line for line in output.splitlines() if "linking abicalls files with non-abicalls files" not in line).strip()
        assert not other, other
        inputs[kind] = {"object": str(original.resolve()), "object_sha256": sha(saved),
                        "exposed_sha256": sha(exposed), "identical_allocated_sections": len(before),
                        "helper_bytes": before[".text.CheckLumpInCache"][2],
                        "link_command": link, "elf_sha256": sha(work / "CACHE_LINKED10.ELF")}
    (a.out / "inputs.json").write_text(json.dumps(inputs, indent=2) + "\n")
    print(json.dumps({kind: inputs[kind]["helper_bytes"] for kind in ("baseline", "candidate")}))


if __name__ == "__main__":
    main()
