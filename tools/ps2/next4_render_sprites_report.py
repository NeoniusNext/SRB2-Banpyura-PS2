"""Summarize the completed four-variant EE sprite-drawer microbenchmark."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, required=True, help="Directory with SPRITES.ELF and run.log")
    args = ap.parse_args()
    work = args.out.resolve()
    source = (work / "run.log").read_text()
    rows = []
    pattern = (r"SPRITEBENCH (\w+) size=(\d+) n=(\d+) dense=(\d+) calls=(\d+) "
               r"base=(\d+) cand=(\d+) hoists=(\d+) advance=(\d+) unit=(\w+)")
    keys = ["drawer", "size", "length", "dense", "calls", "base", "candidate", "hoists_only", "advance_only", "unit"]
    for match in re.finditer(pattern, source):
        rows.append({key: int(value) if value.isdecimal() else value
                     for key, value in zip(keys, match.groups())})
    if len(rows) != 112 or "SPRITES DONE" not in source or "SPRITES EE FAIL" in source:
        raise SystemExit("Expected 112 rows, no comparison failure and SPRITES DONE")
    report = {"elf_sha256": hashlib.sha256((work / "SPRITES.ELF").read_bytes()).hexdigest(),
              "rows": rows, "summary": {}, "per_drawer": {}}
    variants = ["candidate", "hoists_only", "advance_only"]
    for key in variants:
        base, candidate = sum(row["base"] for row in rows), sum(row[key] for row in rows)
        report["summary"][key] = {
            "sum_base_x10": base, "sum_candidate_x10": candidate,
            "reduction_percent": 100 * (1 - candidate / base),
            "faster": sum(row[key] < row["base"] for row in rows),
            "equal": sum(row[key] == row["base"] for row in rows),
            "slower": sum(row[key] > row["base"] for row in rows),
            "min_reduction_percent": min(100 * (1 - row[key] / row["base"]) for row in rows),
            "max_reduction_percent": max(100 * (1 - row[key] / row["base"]) for row in rows),
        }
    for drawer in dict.fromkeys(row["drawer"] for row in rows):
        selected = [row for row in rows if row["drawer"] == drawer]
        report["per_drawer"][drawer] = {
            key: 100 * (1 - sum(row[key] for row in selected) / sum(row["base"] for row in selected))
            for key in variants
        }
    (work / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: value for key, value in report.items() if key != "rows"}, indent=2))


if __name__ == "__main__":
    main()
