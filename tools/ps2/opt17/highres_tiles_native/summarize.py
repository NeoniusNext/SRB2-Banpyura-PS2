#!/usr/bin/env python3
"""Bind native runs to a guarded source manifest; never runs PCSX2."""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "tools/ps2/build.py").is_file())

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest", type=Path, required=True)
    ap.add_argument("--prefix", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--onecore", action="store_true", help="Require the dedicated one-window and raw-repeat counter control")
    ap.add_argument("--widewindow", action="store_true", help="Also require the V8 125-texel one-window cases")
    ap.add_argument("--indexed-halo", action="store_true", help="Require actual 128-indexed and 64-direct halo negative controls")
    ap.add_argument("--borrowwide", action="store_true", help="Require paired optional 32KiB images, occupied-prefix fallback and queued reclaim/overwrite")
    ap.add_argument("--initial-borrow-report", type=Path, help="Bind the preserved original CT32-boundary branch-expectation failure")
    a = ap.parse_args(); prefix = a.prefix.resolve(); manifest = a.manifest.resolve()
    if a.widewindow and not a.onecore:
        ap.error("--widewindow requires --onecore")
    if a.initial_borrow_report and not a.borrowwide:
        ap.error("--initial-borrow-report requires --borrowwide")
    candidate = json.loads(manifest.read_text()); build = json.loads((prefix / "build-report.json").read_text())
    bound = {}
    for name, expected in candidate["candidate_sha256"].items():
        observed = build["shipping_driver_source_sha256_before_fixture_hooks"][name] if name == "src/ps2/hw/ps2_hwd.c" else sha(prefix / "source" / name)
        if observed != expected:
            raise RuntimeError("Candidate source mismatch: " + name)
        bound[name] = observed
    runs = {}
    suffixes = ["full-run", "common-run", "attributes-run", "coordinate-run", "no-halo-run"]
    if a.onecore:
        suffixes.append("onecore-run")
    if a.borrowwide:
        suffixes.append("borrow-run")
    for suffix in suffixes:
        path = prefix.parent / (prefix.name + "-" + suffix) / "native-report.json"
        report = json.loads(path.read_text())
        invariants = ("profile_32MiB", "single_emulator_slot", "check_totals_consistent", "all_frozen_inputs_unchanged", "all_non_UI_settings_unchanged", "emulator_binary_unchanged")
        if not report["completed"] or not all(report[k] for k in invariants):
            raise RuntimeError("Incomplete or changed runtime: " + str(path))
        runs[suffix] = {"report": str(path.relative_to(ROOT)), "sha256": sha(path), "checks": report["checks"], "failures": report["failure_count"], "failure_lines": report["failures"], "elf_sha256": sha(Path(report["original_elf"])), "all_runtime_invariants_passed": True}
    for name in ("common-run", "attributes-run", "coordinate-run"):
        if runs[name]["failures"]:
            raise RuntimeError("Unexpected positive-control failure: " + name)
    onecore = None
    if a.onecore:
        cells = (1, 2, 3, 5) if a.widewindow else (1, 2, 3)
        pairs = len(cells) * 4 + 1
        if runs["onecore-run"]["failures"] or runs["onecore-run"]["checks"] != 2 + pairs * 15:
            raise RuntimeError("Unexpected one-window native outcome")
        log = prefix.parent / (prefix.name + "-onecore-run") / "native.txt"
        rows = [{k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)}
                for line in log.read_text().splitlines() if line.startswith("TN ONECORE ")]
        expected = {(cell, model, zwrite, variant) for cell in cells
                    for model in (0, 1) for zwrite in (0, 1) for variant in (0, 1)}
        expected |= {(4, 1, 1, variant) for variant in (0, 1)}
        observed = {(r["cell"], r["model_light"], r["zwrite"], r["variant"]) for r in rows}
        if len(rows) != pairs * 2 or observed != expected:
            raise RuntimeError("Missing or duplicate one-window counter cases")
        for row in rows:
            fallback = row["cell"] == 4 and row["variant"] == 1
            fast = row["cell"] in cells and row["variant"] == 1
            if row["fast_delta"] != int(fast):
                raise RuntimeError("Unexpected one-window branch count")
            if fallback:
                if not all(row[k] > 0 for k in ("masks", "mask_sprites", "workspace")):
                    raise RuntimeError("Raw repeat did not use the bounded fallback")
            elif any(row[k] for k in ("masks", "mask_sprites", "workspace")):
                raise RuntimeError("One-window path used stencil workspace")
        pixel_lines = [line for line in log.read_text().splitlines() if line.startswith("TN PIXELS name=attributes-cell")]
        if len(pixel_lines) != pairs or any("compared=64000 rgba=1 exactdiff=0 tolerance=0 beyond=0" not in line for line in pixel_lines):
            raise RuntimeError("One-window complete RGBA comparison changed")
        depth_lines = [line for line in log.read_text().splitlines() if line.startswith("H0 PASS tn_attribute_Zlow24_exact ")]
        if len(depth_lines) != pairs or any(not line.endswith("wrong_depth_pixels=0") for line in depth_lines):
            raise RuntimeError("One-window low-24-bit depth comparison changed")
        onecore = {"scope": "Actual complete GS readbacks through production colored/model-light NoDepth fog emission, perspective Q, and both Z-write settings.",
                   "cases": {"1": "Narrow 80..95 window", "2": "Inclusive 0..32 boundaries", "3": "Nonaligned 61-texel span", "4": "Raw 512-texel repeat must reject the fast path"},
                   "positive_pairs": len(cells) * 4, "fallback_pairs": 1, "compared_RGBA_pixels": pairs * 64000,
                   "different_RGBA_pixels": 0, "different_Zlow24_pixels": 0, "counter_observations": rows,
                   "log": str(log.relative_to(ROOT)), "log_sha256": sha(log)}
        if a.widewindow:
            onecore["cases"]["5"] = "Nonaligned 125-texel span, UV80..205 by100..225"
    if runs["full-run"]["failure_lines"] != ["H0 FAIL tn_texture_parity name=texture-f0-1024x512-s4-l0 beyond=1"]:
        raise RuntimeError("Default strict outcome changed; inspect the report")
    borrow = None
    if a.borrowwide:
        if runs["borrow-run"]["failures"] or runs["borrow-run"]["checks"] != 318:
            raise RuntimeError("Unexpected borrowed-window native outcome")
        borrow_log = prefix.parent / (prefix.name + "-borrow-run") / "native.txt"
        lines = borrow_log.read_text().splitlines()
        rows = [{k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)} for line in lines if line.startswith("TN BORROW ")]
        reclaim = [{k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)} for line in lines if line.startswith("TN BORROWRECLAIM ")]
        expected = {(screen, model, zwrite, variant) for screen in (0, 1) for model in (0, 1) for zwrite in (0, 1) for variant in (0, 1, 2, 3)}
        expected |= {(2, model, zwrite, variant) for model in (0, 1) for zwrite in (0, 1) for variant in (0, 1)}
        if len(rows) != 40 or {(r["screen"], r["model"], r["zwrite"], r["variant"]) for r in rows} != expected:
            raise RuntimeError("Missing or duplicate borrowed-window cases")
        for row in rows:
            if row["screen"] != 2 and row["variant"] in (1, 3):
                if (row["borrowed"], row["capacity"], row["upload_delta"], row["fast_delta"], row["width"], row["height"]) != (64, 128, 32768, 1, 128, 64 if row["screen"] else 256):
                    raise RuntimeError("Borrowed fast path layout changed")
                if row["mask_delta"] or row["sprite_delta"] or row["workspace_after"] != row["workspace_before"] or row["queued_qwords"] <= 1:
                    raise RuntimeError("Borrowed fast path used masks/workspace or has no queued packet")
            elif row["borrowed"] or row["fast_delta"] or (row["variant"] and not row["mask_delta"]):
                raise RuntimeError("Resident or occupied-prefix route changed")
        keys = {(r["screen"], r["model"], r["zwrite"]) for r in reclaim}
        if len(reclaim) != 8 or keys != {(s, m, z) for s in (0, 1) for m in (0, 1) for z in (0, 1)}:
            raise RuntimeError("Missing queued reclaim cases")
        for row in reclaim:
            if row["borrowed_after"] or row["permanent_block"] != row["prefix"] + 64 or row["ordinary_blocks"] != 416 or row["overwritten_blocks"] != 64 or row["fixture_fence_before"] or not (row["ordinary"] <= row["prefix"] and row["ordinary"] + 416 >= row["prefix"] + 64):
                raise RuntimeError("Queued prefix reclaim/overwrite was not exercised")
        pixels = [line for line in lines if line.startswith("TN PIXELS name=borrow-screen")]
        depths = [line for line in lines if line.startswith("H0 PASS tn_borrow_Zlow24_exact ")]
        if len(pixels) != 28 or any("compared=64000 rgba=1 exactdiff=0 tolerance=0 beyond=0" not in line for line in pixels) or len(depths) != 28 or any(not line.endswith("wrong_depth_pixels=0") for line in depths):
            raise RuntimeError("Borrowed complete-frame/depth comparisons changed")
        eligibility = [dict(re.findall(r"(\w+)=([^ ]+)", line)) for line in lines if line.startswith("TN BORROWELIG ")]
        if len(eligibility) != 24 or sum(r["cell"] == "7" for r in eligibility) != 16 or any(r["sw"] != "320" or r["sh"] != "200" or r["eligible"] != ("1" if r["cell"] == "7" else "0") for r in eligibility):
            raise RuntimeError("Screen interior/boundary eligibility changed")
        borrow = {"scope": "Actual GS pairs using resident sources, optional adjacent16KiB borrowing, occupied-prefix fallback, and queued wide draw followed by ordinary allocation and prefix overwrite before first readback.",
                  "sources": {"0": "P8 nonscreen128x256; UV80..205 by100..325", "1": "Real captured CT32 screen128x64 interior; UV80..204 by70..130", "2": "Original CT32 boundary UV80..205 by70..131; exact conservative fallback retained"},
                  "compared_RGBA_pixels": 28 * 64000, "different_RGBA_pixels": 0, "different_Zlow24_pixels": 0,
                  "counter_and_layout_observations": rows, "queued_reclaim_overwrite_observations": reclaim,
                  "native_screen_eligibility_and_raw_UV_bits": eligibility,
                  "log": str(borrow_log.relative_to(ROOT)), "log_sha256": sha(borrow_log)}
    if runs["no-halo-run"]["failures"] != (2 if a.indexed_halo else 1):
        raise RuntimeError("Halo negative control was not detected")
    halo = None
    if a.indexed_halo:
        negative_log = prefix.parent / (prefix.name + "-no-halo-run") / "native.txt"
        physical = [line for line in negative_log.read_text().splitlines() if line.startswith("TN TILEPHYSICAL ")]
        expected_physical = {"texture-f2-512x256-s6-l1": 64, "texture-f0-1024x512-s6-l1": 128}
        if len(physical) != 2 or runs["no-halo-run"]["checks"] != 20:
            raise RuntimeError("Missing dynamic-extent halo control")
        for line in physical:
            values = dict(re.findall(r"(\w+)=([^ ]+)", line))
            if int(values["valid"]) != 1 or int(values["blocks"]) != 64 or expected_physical.pop(values["name"], None) != int(values["extent"]):
                raise RuntimeError("Unexpected physical halo layout")
        wrong = {}
        for line in runs["no-halo-run"]["failure_lines"]:
            match = re.fullmatch(r"H0 FAIL tn_texture_parity name=(texture-f[02]-\d+x\d+-s6-l1) beyond=(\d+)", line)
            if not match or int(match[2]) <= 0:
                raise RuntimeError("Unexpected halo negative failure")
            wrong[match[1]] = int(match[2])
        if set(wrong) != {"texture-f2-512x256-s6-l1", "texture-f0-1024x512-s6-l1"}:
            raise RuntimeError("Both physical halo extents must fail visibly")
        full_log = prefix.parent / (prefix.name + "-full-run") / "native.txt"
        full_physical = [line for line in full_log.read_text().splitlines() if line.startswith("TN TILEPHYSICAL ")]
        if len(full_physical) != 17:
            raise RuntimeError("Missing positive dynamic-extent cache observations")
        for line in full_physical:
            values = dict(re.findall(r"(\w+)=([^ ]+)", line))
            extent = 128 if values["name"].startswith("texture-f0-") else 64
            capacities = (64, 128) if a.borrowwide and extent == 128 else (64,)
            if int(values["valid"]) != 1 or int(values["extent"]) != extent or int(values["blocks"]) not in capacities:
                raise RuntimeError("Unexpected positive physical tile layout")
        halo = {"wrong_pixels": wrong, "physical_tile_observations": physical,
                "mutation": "Clamp region-relative dx/dy after subtracting extent/4 to0..extent/2-1; preserve physical rolled address.",
                "log": str(negative_log.relative_to(ROOT)), "log_sha256": sha(negative_log),
                "positive_physical_tile_observations": full_physical,
                "positive_log": str(full_log.relative_to(ROOT)), "positive_log_sha256": sha(full_log)}
    proofs, analyses = {}, {}
    for suffix in ("capture-frame-proof", "coordinate-frame-proof", "texture-RGBA-proof"):
        path = prefix.parent / (prefix.name + "-" + suffix + ".json")
        proofs[suffix] = {"path": str(path.relative_to(ROOT)), "sha256": sha(path)}
        analyses[suffix] = json.loads(path.read_text())
    if not analyses["capture-frame-proof"]["passed"] or not analyses["coordinate-frame-proof"]["passed"] or analyses["texture-RGBA-proof"]["different_alpha"] or analyses["texture-RGBA-proof"]["different_RGBA"] != 1:
        raise RuntimeError("Unexpected saved-frame analysis result")
    result = {"version": candidate.get("version"), "patch_sha256": candidate["patch_sha256"], "guard_manifest_sha256": sha(manifest), "candidate_sha256": bound,
        "scope": "Standalone actual EE/GS readbacks in unchanged standard 32 MiB single-slot PCSX2. Synthetic sources only; engine allocation/zone/regeneration hooks are fixture services. Not full-game or physical PS2 validation.",
        "native_runs": runs, "raw_readback_analysis": proofs, "default_original_strict_gate_passed": False,
        "complete_texture_readbacks": {k: analyses["texture-RGBA-proof"][k] for k in ("total_compared", "different_RGBA", "different_alpha")},
        "opaque_source_coordinates": [{k: row[k] for k in ("shape", "compared_frame_pixels", "covered_in_both", "changed_source_coordinates", "max_circular_dx", "max_circular_dy", "missing_coverage", "extra_coverage")} for row in analyses["coordinate-frame-proof"]["cases"]],
        "capture_identity_and_wipe": [{k: row[k] for k in ("visible", "compared_pixels_per_check", "identity_different_RGBA_pixels", "wipe_different_RGBA_pixels")} for row in analyses["capture-frame-proof"]["modes"]],
        "retained_reference_exception": "One palette-lightrow pixel differs by one RGB level between the existing fast reference and general path; common EE rendering is exact. No original gate or tolerance was changed.",
        "historical_evidence": ["build/nov17-tiles/native/proof-index-v2.json", "build/nov17-tiles/native/proof-index-v3.json", "build/nov17-tiles/native/proof-index-v4.json"]}
    if onecore is not None:
        result["one_window_native_witness"] = onecore
    if halo is not None:
        result["dynamic_extent_halo_negative_witness"] = halo
    if borrow is not None:
        result["borrowed_wide_native_witness"] = borrow
    if a.initial_borrow_report:
        path = a.initial_borrow_report.resolve(); old = json.loads(path.read_text())
        if not old["completed"] or old["checks"] != 262 or old["failure_count"] != 20 or not all(old[k] for k in invariants):
            raise RuntimeError("Unexpected initial borrowed branch report")
        old_build_path = Path(old["original_elf"]).parent
        old_build = json.loads((old_build_path / "build-report.json").read_text())
        for name, expected in bound.items():
            actual = old_build["shipping_driver_source_sha256_before_fixture_hooks"].get(name) or sha(old_build_path / "source" / name)
            if actual != expected:
                raise RuntimeError("Initial borrowed source mismatch: " + name)
        if len(old["pixels"]) != 24 or any("compared=64000 rgba=1 exactdiff=0 tolerance=0 beyond=0" not in line for line in old["pixels"]):
            raise RuntimeError("Initial borrowed output was not exact")
        if any(not re.match(r"H0 FAIL tn_borrow_(layout|route|queued_reclaim_overwrite) ", line) for line in old["failures"]):
            raise RuntimeError("Unexpected initial borrowed failure family")
        result["historical_borrow_branch_red"] = {"report": str(path.relative_to(ROOT)), "sha256": sha(path),
            "checks": 262, "failures": 20, "all20_source_hashes_match": True, "passed": False,
            "explanation": "Original CT32 UV125x61 touched both neighbor margins. Actual EE division puts vLo below70 and rejects wide eligibility. All24 complete RGBA comparisons were exact. The refreshed fixture retains this original input as an explicit exact fallback control and adds a separate interior124x60 positive; no tolerance changed."}
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"output": str(a.out), "sha256": sha(a.out), "bound_source_files": len(bound), "runs": {k: [v["checks"], v["failures"]] for k, v in runs.items()}}, indent=2))

if __name__ == "__main__": main()
