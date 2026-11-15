#!/usr/bin/env python3
"""Read-only binding of matched-seed I/O pressure to exact diagnostic ELFs.

Checks complete in-place RNG patches, arguments/assets/config snapshots,
all zone/cache/stack/spill rows and actual sound payloads. A reconstruction
regression is a reported result, not a validation failure or a speedup claim.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

from audio_storage_next7_finalbind import Object
from next9_io_codecheck import symbols

ROOT = Path(__file__).resolve().parents[2]
FIELDS = ("arena", "used", "peak", "gpeak", "free", "largest", "evicted", "evictedbytes",
          "failed", "flushes", "stackused", "spillpeak", "spillnow")
BASE_ELF = "ab5de93e035b0942a1024e3dd2ac6235ef6285a7a6406e77259946f852faaefe"
IO_ELF = "f3f0fb73f884d1d3b1052ac81a2509a1f592b58f0cd1927e828170e941c7352d"


def sha(path):
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def diagnostic(original, patched, seed, expected):
    assert sha(original) == expected
    before, after = Object(original), Object(patched)
    old_symbols, new_symbols = symbols(before), symbols(after)
    assert old_symbols == new_symbols
    init, seeder = old_symbols["M_RandomSeedFromOS"], old_symbols["M_RandomSeed"]
    assert init["bytes"] == 212
    section = before.headers[init["section"]]
    offset = section[4] + init["address"] - section[3]
    words = [0x27BDFFE0, 0xFFBF0018, 0x3C040000 | (seed >> 16), 0x34840000 | (seed & 65535),
             0x0C000000 | ((seeder["address"] >> 2) & 0x03FFFFFF), 0, 0xDFBF0018,
             0x24020001, 0x03E00008, 0x27BD0020]
    body = struct.pack("<10I", *words) + bytes(172)
    assert len(before.data) == len(after.data)
    assert before.data[:offset] == after.data[:offset] and before.data[offset + 212:] == after.data[offset + 212:]
    assert after.data[offset:offset + 212] == body
    assert ((init["address"] + 20) ^ seeder["address"]) & 0xF0000000 == 0
    return {"original": str(original), "original_sha256": expected, "diagnostic": str(patched),
            "diagnostic_sha256": sha(patched), "seed_uint32": seed, "seed_hex": "0x%08x" % seed,
            "initializer_bytes": 212, "stub_bytes": 40, "NOP_bytes": 172, "frame_bytes": 32,
            "all_other_ELF_bytes_identical": True, "all_symbols_addresses_sizes_identical": True,
            "actual_M_RandomSeed_target": seeder["address"]}


def runtime(path):
    before_path, after_path = path / "run-pre.json", path / "run-post.json"
    before, after = json.loads(before_path.read_text()), json.loads(after_path.read_text())
    assert after["rc"] == 0
    assert after["all_frozen_inputs_unchanged"] and after["all_non_UI_settings_unchanged"] and after["emulator_binary_unchanged"]
    assert before["command"] == after["command"] and before["input_sha256"] == after["input_sha256"]
    assert before["critical_settings"] == after["critical_settings"]
    assert before["emulator_binary_sha256"] == after["emulator_binary_sha256"]
    return {"before_sha256": sha(before_path), "after_sha256": sha(after_path),
            "critical_settings": before["critical_settings"], "emulator_binary_sha256": before["emulator_binary_sha256"],
            "frozen_input_sha256": before["input_sha256"], "original_inputs_and_non_UI_settings_unchanged": True}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--pressure", type=Path, action="append", required=True)
    ap.add_argument("--baseline", type=Path, default=ROOT / "build/nov15-next11/baseline/software/SRB2.ELF")
    ap.add_argument("--candidate", type=Path, default=ROOT / "build/nov15-next11/io/software/SRB2.ELF")
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    assert a.out.resolve().is_relative_to(ROOT / "build")
    original = {str(path): sha(path) for path in (a.baseline, a.candidate)}
    result = {"checker_sha256": sha(Path(__file__)), "compared_fields": list(FIELDS), "seeds": {},
              "unseeded_historical_results_used_as_causal_evidence": False, "FPS_calculation_claimed": False}
    for directory in a.pressure:
        directory = directory.resolve()
        pressure_path = directory / "next11-pressure-350-report.json"
        report = json.loads(pressure_path.read_text())
        assert report["frames"] == 350 and report["errors"] == [] and report["lhash_equal"]
        row = {"pressure_report": str(pressure_path), "pressure_report_sha256": sha(pressure_path), "runs": {}}
        args = ["-config", "reference.cfg", "-nolog", "-noendtxt", "-skipintro", "-warp", "MAP11",
                "-singletics", "-keepspawn", "-loadprof", "-loadhash", "-zck", "-zheap", "1",
                "-zstack", "-zquit", "350", "-astat"]
        for version, source, expected in (("base", a.baseline, BASE_ELF), ("new", a.candidate, IO_ELF)):
            name = version + "-sw-map11-spawn-350"
            run = directory / "runs" / name
            item = report["runs"][version]
            assert item["errors"] == [] and not item["warnings"]
            assert item["audio_underruns"] and all(value == 0 for value in item["audio_underruns"])
            assert (run / "ps2args").read_text().splitlines() == args
            meta = runtime(directory / "runtime" / name)
            command = json.loads((directory / "runtime" / name / "run-pre.json").read_text())["command"]
            diagnostic_path = Path(command[command.index("--elf") + 1])
            match = re.fullmatch(r"DIAGNOSTIC-LOCAL-RNG-([0-9a-f]{8})\.ELF", diagnostic_path.name)
            assert match
            seed = int(match[1], 16)
            assert seed in (0x01B2D3F5, 0x9E3779B9)
            patch = diagnostic(source, diagnostic_path, seed, expected)
            assert sha(run / "SRB2.ELF") == item["elf_sha256"] == patch["diagnostic_sha256"]
            for asset, expected_asset in report["asset_sha256"].items():
                assert sha(run / asset) == expected_asset
            log = run / "pcsx2.log"
            sounds = [(int(blocks), int(payload)) for blocks, payload in
                      re.findall(r"PU_SOUND\s+\d+ blocks\s+(\d+) B \(payload\s+(\d+) B\)", log.read_text())]
            assert sounds
            assert item["zstat"] and all(z["ram"] == "33554432" and z["map"] == "11" for z in item["zstat"])
            row["runs"][version] = {"patch": patch, "runtime": meta, "full_console_log_sha256": sha(log),
                "reference_config_sha256": sha(run / "reference.cfg"), "sound_rounded_payload_rows": sounds,
                "all_zstat_rows": item["zstat"], "loader_hash_rows": item["lhash"],
                "full_zone_check_rows": item["zck"], "warnings": [], "audio_underrun_rows": item["audio_underruns"]}
        base, new = row["runs"]["base"], row["runs"]["new"]
        seed = base["patch"]["seed_uint32"]
        assert new["patch"]["seed_uint32"] == seed
        assert base["runtime"]["critical_settings"] == new["runtime"]["critical_settings"]
        assert base["runtime"]["emulator_binary_sha256"] == new["runtime"]["emulator_binary_sha256"]
        assert base["reference_config_sha256"] == new["reference_config_sha256"]
        assert base["sound_rounded_payload_rows"] == new["sound_rounded_payload_rows"]
        assert base["loader_hash_rows"] == new["loader_hash_rows"]
        assert len(base["all_zstat_rows"]) == len(new["all_zstat_rows"])
        rows = []
        for left, right in zip(base["all_zstat_rows"], new["all_zstat_rows"]):
            assert left["levelframes"] == right["levelframes"] and left["frames"] == right["frames"]
            rows.append({"levelframes": int(left["levelframes"]),
                "baseline": {key: int(left[key]) for key in FIELDS},
                "candidate": {key: int(right[key]) for key in FIELDS},
                "delta": {key: int(right[key]) - int(left[key]) for key in FIELDS},
                "reported_ms_baseline": int(left["t_ms"]), "reported_ms_candidate": int(right["t_ms"]),
                "reported_ms_delta": int(right["t_ms"]) - int(left["t_ms"])})
        assert rows[-1]["levelframes"] == 350
        row["all_zone_cache_stack_spill_comparisons"] = rows
        row["cache_reconstruction_regression"] = any(z["delta"]["evicted"] > 0 or z["delta"]["evictedbytes"] > 0 for z in rows)
        row["workload_inputs_and_actual_sound_payload_matched"] = True
        key = "%08x" % seed
        assert key not in result["seeds"]
        result["seeds"][key] = row
        print(key, {"sound": base["sound_rounded_payload_rows"], "final": rows[-1],
                    "cache_reconstruction_regression": row["cache_reconstruction_regression"]}, flush=True)
    assert original == {str(path): sha(path) for path in (a.baseline, a.candidate)}
    result["original_unmodified_ELFs_unchanged"] = True
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
