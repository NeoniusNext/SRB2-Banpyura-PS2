"""Judge the unchanged-v7 480p mode/switch probe; never starts an emulator."""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]
EXPECTED = [(12, 320, 200, "Hardware"), (24, 640, 400, "Hardware"), (36, 640, 400, "Software"),
            (48, 640, 480, "Software"), (60, 640, 480, "Hardware"), (72, 640, 448, "Hardware"),
            (84, 320, 200, "Hardware")]


def sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for part in iter(lambda: stream.read(1 << 20), b""):
            digest.update(part)
    return digest.hexdigest()


def ppm(path):
    data = path.read_bytes()
    header = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if not header:
        raise ValueError("unsupported PPM header: " + str(path))
    w, h = map(int, header.groups())
    payload = data[header.end():]
    if len(payload) != w * h * 3:
        raise ValueError("truncated/trailing PPM bytes: " + str(path))
    return w, h, payload


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--run-dir", type=Path, required=True)
    ap.add_argument("--source-root", type=Path, required=True)
    ap.add_argument("--elf", type=Path, required=True)
    ap.add_argument("--profile", type=Path, default=Path("/workspace/.onboarding/pcsx2/slot0/usr/bin/inis/PCSX2.ini"))
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    run = a.run_dir.resolve()
    boot = (run / "boot.txt").read_text(errors="replace")
    summary = json.loads((run / "summary.json").read_text())
    errors = [line for line in boot.splitlines() if any(token in line for token in ("I_Error", "OOM:", "HEAP CHECK FAILED", "WATCHDOG"))]
    missing = [line for line in boot.splitlines() if "texture not resident" in line]
    limits = [line for line in boot.splitlines() if "experimental limitation:" in line]
    frames, layouts = {}, []
    renderer, internal = None, None
    for line in boot.splitlines():
        layout = re.search(r"internal (\d+)x(\d+) \(frame (\d+) x (\d+) (CT\w+), CRTC x(\d+)/x(\d+), output (\d+)\).*texture pool (\d+)K \((\d+) pages\)", line)
        if layout:
            values = list(layout.groups())
            internal = {"render": list(map(int, values[:2])), "stride_rows": list(map(int, values[2:4])),
                        "format": values[4], "crtc": list(map(int, values[5:7])), "output": int(values[7]),
                        "pool_bytes": int(values[9]) * 8192, "log": line}
            layouts.append(internal)
        if "HWE acquired driver=ps2_hwd renderer=Hardware" in line:
            renderer = "Hardware"
        elif "HWE acquired renderer=Software" in line:
            renderer = "Software"
        match = re.search(r"VIDSHOT l(\d+) (\d+)x(\d+) .*saved .* lt=(\d+)", line)
        if match:
            frame, w, h, tic = map(int, match.groups())
            frames[frame] = {"frame": frame, "logical": [w, h], "renderer": renderer, "leveltime": tic,
                             "internal_observed_from_latest_init": internal if renderer == "Hardware" else None}
    shots = []
    for frame, w, h, renderer in EXPECTED:
        path = run / f"vidshot-{w}x{h}-l{frame}.ppm"
        row = dict(frames.get(frame, {}), expected_logical=[w, h], expected_renderer=renderer)
        row["metadata_ok"] = row.get("logical") == [w, h] and row.get("renderer") == renderer
        row["path"] = str(path)
        row["exists"] = path.exists()
        if path.exists():
            pw, ph, payload = ppm(path)
            row.update({"ppm_size": [pw, ph], "ppm_sha256": sha(path), "ppm_size_ok": [pw, ph] == [w, h]})
            if frame in (48, 60):
                # An illustrative fixed-camera sky interior, not a pixel parity oracle.
                black = sum(payload[(y * w + x) * 3:(y * w + x) * 3 + 3] == b"\0\0\0" for y in range(65, 90) for x in range(288, 352))
                row["sky_roi_black_pixels"] = {"roi": [288, 65, 352, 90], "black": black, "pixels": 1600,
                                                 "scope": "illustration only; shot times and renderer algorithms differ"}
        shots.append(row)
    paths = [a.elf.resolve(), run / "SRB2.ELF", run / "ps2args", run / ".srb2/reference.cfg",
             run / "boot.txt", run / "pcsx2.log", run / "summary.json", a.profile.resolve(), Path(__file__).resolve(),
             *sorted(run.glob("*.PAK")), run / "FINEACON.DAT"]
    paths += [a.source_root.resolve() / "src/ps2/i_video.c", a.source_root.resolve() / "src/ps2/ps2_vmodes.h",
              *sorted((a.source_root.resolve() / "src/ps2/hw").glob("*.inc")), a.source_root.resolve() / "src/ps2/hw/ps2_hwd.c"]
    manifest = {str(path): {"bytes": path.stat().st_size, "sha256": sha(path)} for path in paths}
    completed = summary["rc"] == 0 and "VIDSHOT COMPLETE 7" in boot and all(row["metadata_ok"] and row["exists"] and row.get("ppm_size_ok") for row in shots)
    result = {"scope": __doc__, "completed": completed, "clean_render_gate_passed": completed and not errors and not missing and not limits,
              "profile_32MiB_observed": "RAM 32 MiB = 32 MB profile" in boot and "EnableExtraMemory = false" in a.profile.read_text(),
              "guard_disabled_for_diagnosis": "recovery is OFF (-hwnofb)" in boot,
              "normal_guard_behavior_tested": False, "physical_PS2_tested": False, "pixel_parity_claimed": False,
              "lower_bound_texture_residency_failure_log_count": len(missing), "residency_failures": missing, "limitation_lines": limits,
              "inherited_selected_while_collecting_HWSKIP": sum(line.startswith("HWSKIP ") and "selected while collecting" in line for line in boot.splitlines()),
              "errors": errors, "layouts_observed": layouts, "shots": shots, "manifest": manifest}
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: result[k] for k in ("completed", "clean_render_gate_passed", "profile_32MiB_observed", "lower_bound_texture_residency_failure_log_count", "inherited_selected_while_collecting_HWSKIP")}, indent=2))
    return int(not completed)


if __name__ == "__main__":
    raise SystemExit(main())
