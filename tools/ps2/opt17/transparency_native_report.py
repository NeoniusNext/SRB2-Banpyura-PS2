#!/usr/bin/env python3
"""Judge the paired actual-GS h255 witness from retained logs; no emulator."""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--run", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--profile", type=Path, default=Path("/workspace/.onboarding/pcsx2/slot0/usr/bin/inis/PCSX2.ini"))
    a = ap.parse_args(); run = a.run.resolve(); build = json.loads((run / "build-report.json").read_text())
    report = {"scope": "Actual EE/GS execution in PCSX2: exact CLUT VRAM alpha, decoded TEX0 CLUT address, masked-hole frame interior and opaque control before/after a real SetScreenSize restart. Boundary allocator/raw-flat reads are fixture services; full-engine scenes and physical PS2 remain separate gates.",
              "build": build, "variants": {}, "manifest": {}}
    good = True
    for variant in ("baseline", "fixed"):
        path = run / variant / "gs.log"; text = path.read_text(errors="replace")
        phases = []
        for m in re.finditer(r"T17N phase=(\S+) canonical=(\d+) has255=(\d+) clut=(\d+) clut255=([0-9a-f]+) cbp=(\d+) tex0=([0-9a-f]+) level=(\d+)", text):
            phase, canonical, has255, clut, clut255, cbp, tex0, level = m.groups()
            row = {"phase": phase, "canonical": int(canonical), "has255": int(has255), "clut": int(clut),
                   "clut255": clut255, "clut_alpha": int(clut255, 16) >> 24, "cbp": int(cbp), "tex0": tex0, "level": int(level)}
            row["tex0_cbp_matches"] = ((int(tex0, 16) >> 37) & 0x3fff) == int(cbp)
            phases.append(row)
        pixels = []
        for m in re.finditer(r"T17N PIXEL restart=(\d+) read=(-?\d+) wrong=(\d+) compared=(\d+) hole=([0-9a-f]+) expected_hole=([0-9a-f]+) opaque=([0-9a-f]+) expected_opaque=([0-9a-f]+) clut_alpha=(\d+) canonical=(\d+)", text):
            restart, read, wrong, compared, hole, expected_hole, opaque, expected_opaque, alpha, canonical = m.groups()
            pixels.append({"restart": int(restart), "read": int(read), "wrong": int(wrong), "compared": int(compared),
                           "hole": hole, "expected_hole": expected_hole, "opaque": opaque, "expected_opaque": expected_opaque,
                           "clut_alpha": int(alpha), "canonical": int(canonical)})
        finish = re.findall(r"T17N COMPLETE checks=(\d+) failures=(\d+)", text)
        expected_canonical = 1 if variant == "baseline" else 0
        expected_full = 1 if variant == "baseline" else 2
        complete = (finish == [("26", "0")] and "H0 FAIL " not in text and len(phases) == 8 and len(pixels) == 2
                    and f"T17N restart screen=640x400 internal=640x448 canonical={expected_canonical} handles=0/0" in text)
        if complete:
            complete &= [r["phase"] for r in phases] == ["streamed-L1", "derived-L2", "full-plain", "full-keyed"] * 2
            for i, row in enumerate(phases):
                complete &= row["tex0_cbp_matches"] and row["canonical"] <= 2
                if i % 4 == 1: complete &= row["canonical"] == expected_canonical
                if i % 4 == 2: complete &= row["canonical"] == expected_full
                if i % 4 == 3: complete &= row["clut_alpha"] == (128 if variant == "baseline" else 0) and row["clut"] == (1 if variant == "baseline" else 2)
            for i, row in enumerate(pixels):
                complete &= (row["restart"] == i and row["read"] == 0 and row["compared"] == (7120 if i == 0 else 28640)
                             and row["opaque"] == row["expected_opaque"] == "d67c29" and row["canonical"] == expected_full)
                if variant == "baseline":
                    complete &= row["wrong"] == row["compared"] and row["hole"] == "00aaff" and row["clut_alpha"] == 128
                else:
                    complete &= row["wrong"] == 0 and row["hole"] == row["expected_hole"] == "332211" and row["clut_alpha"] == 0
        report["variants"][variant] = {"expected_behavior_observed": bool(complete), "phases": phases, "pixels": pixels,
                                       "baseline_expected_failure_control": variant == "baseline", "complete": finish}
        good &= bool(complete)
        for item in (path, run / variant / "T17.ELF", run / variant / "native-lto.o", run / variant / "source/src/ps2/hw/ps2_hw_tex.inc"):
            report["manifest"][str(item)] = {"bytes": item.stat().st_size, "sha256": sha(item)}
    report["passed"] = good
    report["zero_pixel_tolerance"] = True
    report["physical_PS2_tested"] = False
    report["manifest"][str(a.profile.resolve())] = {"sha256": sha(a.profile.resolve())}
    report["manifest"][str(run / "build-report.json")] = {"sha256": sha(run / "build-report.json")}
    report["manifest"][str(Path(__file__).resolve())] = {"sha256": sha(Path(__file__).resolve())}
    a.out.parent.mkdir(parents=True, exist_ok=True); a.out.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": good, "baseline_wrong_pixels": sum(r["wrong"] for r in report["variants"]["baseline"]["pixels"]),
                      "fixed_wrong_pixels": sum(r["wrong"] for r in report["variants"]["fixed"]["pixels"]), "out": str(a.out.resolve())}, indent=2))
    return int(not good)


if __name__ == "__main__": raise SystemExit(main())
