#!/usr/bin/env python3
"""Replay the published/current prism predicate, and optionally time it on EE."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from hw_front_cull_hosttest import BASE, ROOT, function, published


PEELED = '''static inline int PeeledPrismHidden(const polyvertex_t *pts, int n, float lo, float hi)
{
    int k;
    if (n <= 0)
        return 1;
    for (k = 0; k < 5; k++)
    {
        const fr_plane_t *pl = &frv.p[k];
        const float dl = pl->b * lo + pl->d, dh = pl->b * hi + pl->d;
        const float q0 = pl->a * pts[0].x + pl->c * pts[0].y;
        float dm;
        int i;
        if (q0 + dl <= FR_MARGIN || q0 + dh <= FR_MARGIN)
            continue;
        dm = dl < dh || dh != dh ? dl : dh;
        for (i = 1; i < n; i++)
        {
            const float q = pl->a * pts[i].x + pl->c * pts[i].y;
            if (q + dm <= FR_MARGIN)
                goto inside;
        }
        return 1;
inside:;
    }
    return 0;
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default=str(ROOT / "build/ht-front-prism"))
    ap.add_argument("--ee", action="store_true")
    ap.add_argument("--skip-sanitizers", action="store_true")
    args = ap.parse_args()
    work = Path(args.out).resolve()
    work.mkdir(parents=True, exist_ok=True)
    baseline = function(published("src/hardware/hw_front.inc"), "HWR_FrPrismHidden").replace("HWR_FrPrismHidden", "ReferencePrismHidden")
    current = function((ROOT / "src/hardware/hw_front.inc").read_text(), "HWR_FrPrismHidden")
    header = baseline + current + PEELED
    report = {"baseline": BASE, "source_sha256": hashlib.sha256((ROOT / "src/hardware/hw_front.inc").read_bytes()).hexdigest(), "runs": []}
    variants = [("normal", header, [])]
    # min->max rejects planes whose other endpoint is still visible.
    variants.append(("wrong-endpoint", header.replace("dl < dh || dh != dh", "dl > dh || dh != dh"), []))
    variants.append(("lost-nan-endpoint", header.replace("dl < dh || dh != dh", "dl < dh"), []))
    if not args.skip_sanitizers:
        variants.append(("sanitized", header, ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    cc = shutil.which("gcc") or shutil.which("cc")
    for name, contents, flags in variants:
        directory = work / name
        directory.mkdir(exist_ok=True)
        (directory / "hw_front_prism_under_test.h").write_text(contents)
        exe = directory / "test"
        command = [cc, "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-ffp-contract=off", *flags, "-I", str(directory), str(ROOT / "tools/ps2/hw_front_prism_cull_hosttest.c"), "-o", str(exe), "-lm"]
        subprocess.run(command, check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        ok = result.returncode == (0 if name in ("normal", "sanitized") else 1)
        print(name, "PASS" if ok else "FAIL", result.stdout.strip(), result.stderr.strip())
        report["runs"].append({"name": name, "passed": ok, "exit": result.returncode, "stdout": result.stdout, "stderr": result.stderr})
        if not ok:
            return 1
    if args.ee:
        import build as B
        directory = work / "ee"
        directory.mkdir(exist_ok=True)
        (directory / "hw_front_prism_under_test.h").write_text(header)
        obj, elf = directory / "test.o", directory / "PRISM.ELF"
        flags = [f for f in B.CFLAGS if f not in ("-MMD", "-MP")]
        commands = [[str(B.CC), *flags, "-Werror", "-DFRONT_PRISM_NATIVE", "-I", str(directory), *B.INCS, "-c", str(ROOT / "tools/ps2/hw_front_prism_cull_hosttest.c"), "-o", str(obj)], [str(B.CC), *B.LDFLAGS, str(obj), "-o", str(elf), "-ldebug", "-lpatches", "-lm"]]
        (directory / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        for command in commands:
            subprocess.run(command, check=True, env=B.ENV)
        result = subprocess.run([shutil.which("python3"), str(ROOT / "tools/ps2/run_pcsx2.py"), "--elf", str(elf), "--log", str(directory / "run.log"), "--until", "EETEST DONE", "--timeout", "180"])
        log = (directory / "run.log").read_text(errors="replace")
        result_text = "\n".join(line for line in log.splitlines() if "EETEST" in line or "FAIL" in line or "PASS queries" in line)
        print(result_text)
        (directory / "results.txt").write_text(result_text + "\n")
        if result.returncode or "FAIL" in log or "EETEST DONE" not in log:
            return 1
        report["ee"] = {"elf_sha256": hashlib.sha256(elf.read_bytes()).hexdigest(), "passed": True}
    report["passed"] = True
    (work / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
