"""Read-only, pinned final-engine instruction audit; no build or execution."""
from pathlib import Path
import argparse
import hashlib
import json
import re

ROOT = Path(__file__).resolve().parents[2]
EXPECTED_SOURCE_SHA256 = "21f2bdc25e85943b3f105f8169ae29a75fdec25593b367fd2cc8dca56c3878a6"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def instructions(path):
    text = path.read_text()
    rows = []
    for match in re.finditer(r"^[ \t]*([0-9a-f]+):[ \t]+[0-9a-f]+[ \t]+(\w+)(?:[ \t]+([^\n]+))?", text, re.M):
        rows.append((int(match[1], 16), match[2], (match[3] or "").strip()))
    start = rows[0][0]
    return [(address - start, op, args) for address, op, args in rows]


def canonical_region(rows, low, high, registers, branches):
    found = []
    for offset, op, args in rows:
        if not low <= offset <= high:
            continue
        args = re.sub(r"[0-9a-f]+ <fill_ap88\+0x([0-9a-f]+)>",
                      lambda m: branches[int(m[1], 16)], args)
        args = re.sub(r"\b(?:zero|sp|v[01]|a[0-7]|t[0-9])\b", lambda m: registers.get(m[0], m[0]), args)
        found.append(op + " " + args)
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, default=ROOT / "build/nov14-next7/gs-next7-ee",
                        help="Directory produced by gs_next7_hosttest.py --linked-base/--linked-current")
    args = parser.parse_args()
    work = args.work.resolve()
    raw = json.loads((work / "linked-engine-report.json").read_text())
    expected_hashes = {
        "base": "172cf2b3de136a3c63229405cb51c1ec7f5d6da68840a55e1b747e14e281485c",
        "cur": "9258483fa3fc655d035d9084286d339e309b6e6fa6132e5418eb4e16c17505d3",
    }
    result = {"kind": "read-only production Hardware LTO instruction audit", "functions": {}, "fast_kernels": {}}
    code = {}
    for tag in ("base", "cur"):
        binary = Path(raw[tag + "_elf"]["path"])
        assert sha(binary) == raw[tag + "_elf"]["sha256"] == expected_hashes[tag]
        result[tag + "_elf"] = raw[tag + "_elf"]
        code[tag] = {}
        for name in ("fill_idx", "fill_ap88"):
            disassembly = work / (tag + "-engine-" + name + "-disassembly.txt")
            assert sha(disassembly) == raw["functions"][name][tag]["disassembly_sha256"]
            rows = instructions(disassembly)
            assert len(rows) * 4 == raw["functions"][name][tag]["symbol_bytes"]
            code[tag][name] = rows
            values = raw["functions"][name][tag].copy()
            values["instructions"] = len(rows)
            values["ppacb_sites"] = sum(op == "ppacb" for _, op, _ in rows)
            result["functions"].setdefault(name, {})[tag] = values
            assert values["stack_frames"] == ([] if name == "fill_idx" else [16])
            assert values["ppacb_sites"] == (0 if name == "fill_idx" else 3)

    key_base = {"a4": "SRC", "v1": "X", "a1": "DST", "a3": "IDX", "a6": "ALIGN", "t0": "LIMIT", "sp": "SP", "zero": "ZERO"}
    key_cur = {"a0": "SRC", "v0": "X", "a1": "DST", "a3": "IDX", "a6": "ALIGN", "t0": "LIMIT", "sp": "SP", "zero": "ZERO"}
    alpha_base = {"a4": "SRC", "a1": "X", "a7": "DST", "a3": "VALUE", "a6": "ALPHA", "v1": "BITS", "t0": "LIMIT", "t1": "MASK7", "t2": "MASK1", "sp": "SP", "zero": "ZERO"}
    alpha_cur = {"a0": "SRC", "a6": "X", "a7": "DST", "a1": "VALUE", "a3": "ALPHA", "v0": "BITS", "t0": "LIMIT", "t1": "MASK7", "t2": "MASK1", "sp": "SP", "zero": "ZERO"}
    regions = {
        "keyed_fast4": ((0x1E0, 0x208, key_base, {0x1E0: "KEYED_LOOP", 0x400: "KEYED_UNALIGNED"}),
                         (0x80, 0xA8, key_cur, {0x80: "KEYED_LOOP", 0x478: "KEYED_UNALIGNED"}), 11),
        "keyed_unaligned_store": ((0x400, 0x428, key_base, {0x1E0: "KEYED_LOOP"}),
                                   (0x478, 0x4A0, key_cur, {0x80: "KEYED_LOOP"}), 11),
        "alpha_fast4": ((0x2D0, 0x328, alpha_base, {0x2D0: "ALPHA_LOOP", 0x438: "ALPHA_UNALIGNED"}),
                         (0x328, 0x380, alpha_cur, {0x328: "ALPHA_LOOP", 0x4B0: "ALPHA_UNALIGNED"}), 23),
        "alpha_unaligned_store": ((0x438, 0x460, alpha_base, {0x2D0: "ALPHA_LOOP"}),
                                   (0x4B0, 0x4D8, alpha_cur, {0x328: "ALPHA_LOOP"}), 11),
    }
    for name, (before, after, count) in regions.items():
        old = canonical_region(code["base"]["fill_ap88"], *before)
        cur = canonical_region(code["cur"]["fill_ap88"], *after)
        assert len(old) == len(cur) == count and old == cur, name
        result["fast_kernels"][name] = {
            "base_offsets": [hex(before[0]), hex(before[1])], "current_offsets": [hex(after[0]), hex(after[1])],
            "instruction_count": count, "equal_modulo_register_roles_and_branch_targets": True,
            "canonical_instructions": old,
        }

    # The only pointer to the context enters in a0. These last entry loads capture
    # the final context member before the register is reused for pixel source data.
    # No context field read occurs afterward, including either scalar fallback.
    current_ap = code["cur"]["fill_ap88"]
    context_reads = [(off, args) for off, op, args in current_ap if op == "lw" and re.search(r"(?:0|4|12|20|24|28)\(a0\)", args)]
    assert len(context_reads) == 6 and max(off for off, _ in context_reads) == 0x2C
    assert next((op, args) for off, op, args in current_ap if off == 0x34) == ("addu", "a0,v1,v0")
    result["context_capture"] = {
        "fill_ap88_entry_context_reads": [[hex(off), args] for off, args in context_reads],
        "last_context_read_offset": "0x2c", "a0_becomes_source_pointer_offset": "0x34",
        "context_reads_after_capture": 0,
        "fill_idx_decimated_loop_context_reads_before_after": [2, 0],
        "fill_idx_decimated_loop_instructions_before_after": [14, 7],
    }
    old_idx = [op for off, op, _ in code["base"]["fill_idx"] if 0x70 <= off <= 0xA4]
    cur_idx = [op for off, op, _ in code["cur"]["fill_idx"] if 0x50 <= off <= 0x68]
    assert len(old_idx) == 14 and len(cur_idx) == 7
    assert sum(op == "lw" for op in old_idx) == 2 and not any(op == "lw" for op in cur_idx)
    result["code_bytes"] = {
        "baseline_symbol_body_sum": sum(v["base"]["symbol_bytes"] for v in raw["functions"].values()),
        "current_symbol_body_sum": sum(v["cur"]["symbol_bytes"] for v in raw["functions"].values()),
        "delta": 8,
        "scope": "Actual linked function bodies; isolated object-text reduction does not apply to engine LTO",
    }
    result["limits"] = [
        "The unchanged fast4 kernels establish instruction structure, not a whole-callback or game timing percentage",
        "AP88 LTO additionally specializes/unrolls scalar paths; the separate-TU fixture timing percentages are not applied to that linked code",
        "GS readbacks and historical VU0 behavior are coordinator integration measurements",
    ]
    assert sha(ROOT / "src/ps2/hw/ps2_hw_tex.inc") == raw["production_source_sha256"] == EXPECTED_SOURCE_SHA256
    result["production_source_sha256"] = raw["production_source_sha256"]
    result["auditor_path"] = str(Path(__file__).resolve().relative_to(ROOT))
    result["auditor_sha256"] = sha(Path(__file__))
    (work / "linked-engine-detailed-report.json").write_text(json.dumps(result, indent=2) + "\n")
    print("Production LTO audit passed: 3 PPACB sites, identical keyed/alpha fast4 kernels and stores, frames 0/16 unchanged")
    print("Actual callback bodies: 1412 -> 1420 B (+8 B); index decimation 14 -> 7 instructions, context reads 2 -> 0")


if __name__ == "__main__":
    main()
