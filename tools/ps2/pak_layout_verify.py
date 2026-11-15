#!/usr/bin/env python3
"""Prove a layout-only SRP2 repack with literal Python and production C reads.

Inputs are read-only. The C fixture links real liblz4, reads every byte with
guards and partial reads, then emits literal decoded bytes for comparison.
Disposable fixtures exercise gap placement, dedup, v1/v2 and malformed input.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import time
import zipfile
import zlib

import cook
import strip_pics
import test_pack_reader
import verify_pack

ROOT = Path(__file__).resolve().parents[2]


def digest(path, algorithm="sha256"):
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, algorithm).hexdigest()


def literal_equal(first, second):
    with first.open("rb") as a, second.open("rb") as b:
        while True:
            x, y = a.read(65536), b.read(65536)
            if x != y:
                return False
            if not x:
                return True


def order_names(path):
    return [line.strip() for line in path.read_text().splitlines()
            if line.strip() and not line.startswith("#")]


def metadata(path):
    with path.open("rb") as handle:
        header = handle.read(64)
        fields = struct.unpack_from("<4s10I", header)
        _magic, version, _hs, _flags, n, toff, poff, psize, _doff, _fsize, block = fields
        extension = handle.read(64) if version >= 2 else b""
        handle.seek(toff)
        table = [struct.unpack("<6I", handle.read(24)) for _ in range(n)]
        handle.seek(poff)
        pool = handle.read(psize)
        names = [pool[e[3]:pool.index(b"\0", e[3])] for e in table]
        regions = {"pool": pool}
        if version >= 2:
            _es, ho, hb, co, *_checks = struct.unpack_from("<8I", extension)
            handle.seek(ho)
            regions["heads"] = handle.read(n * hb)
            handle.seek(co)
            regions["crcs"] = handle.read(n * 4)
    return dict(header=header, fields=fields, extension=extension, table=table,
                names=names, regions=regions, block=block)


def compare(reference, candidate, order, pk3=None):
    """Compare every literal decoded and stored byte, name and protected offset."""
    old, new = metadata(reference), metadata(candidate)
    # Only filesize and the entry-position checksum may differ in the indexes.
    assert old["fields"][:9] == new["fields"][:9], "header layout changed"
    assert old["fields"][10:] == new["fields"][10:], "block size changed"
    assert old["header"][44:] == new["header"][44:], "reserved header bytes changed"
    assert old["extension"][:16] == new["extension"][:16], "extension layout changed"
    assert old["extension"][20:] == new["extension"][20:], "non-position checksums changed"
    assert old["regions"] == new["regions"], "name/head/CRC bytes changed"
    assert old["names"] == new["names"], "entry names/order changed"
    index = {}
    for i, name in enumerate(old["names"]):
        index.setdefault(name.decode("ascii"), i)
    protected = {index[name] for name in order if name in index}
    protected_payloads = {old["table"][i][0] for i in protected if old["table"][i][2]}
    moved = decoded = stored = 0
    archive = zipfile.ZipFile(pk3) if pk3 else None
    infos = archive.infolist() if archive else []
    if archive:
        assert len(infos) == len(old["names"])
    with reference.open("rb") as a, candidate.open("rb") as b:
        for i, (first, second) in enumerate(zip(old["table"], new["table"])):
            assert first[1:] == second[1:], f"entry {i}: non-position fields changed"
            if first[0] in protected_payloads:
                assert first[0] == second[0], f"protected startup payload {i} moved"
            if first[0] != second[0]:
                moved += 1
            for handle, entry in ((a, first), (b, second)):
                handle.seek(entry[0])
            assert a.read(first[1]) == b.read(second[1]), f"entry {i}: stored bytes changed"
            x = verify_pack.decode(a, first[0], first[1], first[2], first[5], old["block"])
            y = verify_pack.decode(b, second[0], second[1], second[2], second[5], new["block"])
            assert x == y, f"entry {i}: decoded bytes changed"
            decoded += len(x)
            stored += first[1]
            if archive:
                zi = infos[i]
                assert zi.filename.encode("ascii") == old["names"][i]
                source = b"" if zi.is_dir() else archive.read(zi)
                if source[:8] != strip_pics.PNG_SIG:
                    assert x == source, f"entry {i}: literal bytes differ from source archive"
    if archive:
        archive.close()
    return dict(lumps=len(old["names"]), decoded_bytes=decoded, stored_bytes=stored,
                moved_entries=moved, protected_entries=len(protected),
                protected_unique_payloads=len(protected_payloads),
                reference_bytes=reference.stat().st_size, candidate_bytes=candidate.stat().st_size,
                saved_bytes=reference.stat().st_size - candidate.stat().st_size)


def build(out, reader, sanitize=False):
    sdk = Path(os.environ.get("PS2SDK", "/workspace/.onboarding/ps2dev-v2/ps2dev/ps2sdk"))
    snapshot = out / "source"
    snapshot.mkdir(exist_ok=True)
    for source in (reader, ROOT / "src/w_pack.h", ROOT / "src/w_wad.h",
                   ROOT / "tools/ps2/pak_layout_hosttest.c", ROOT / "tools/ps2/pack_hosttest.c"):
        destination = snapshot / source.name
        if not destination.exists():
            shutil.copyfile(source, destination)
    label = "reader-asan" if sanitize else "reader"
    executable = out / label
    command = ["cc", "-O2", "-g", "-std=gnu17", "-fwrapv", "-Wall", "-Werror",
               "-Wno-unused-function", "-Wno-sign-compare", "-Wno-unused-variable",
               "-DPS2_PROFILE", "-DNOHW", "-DNDEBUG", "-I" + str(snapshot),
               "-I" + str(ROOT / "src"), "-I" + str(ROOT / "build/host-gen"),
               "-I" + str(sdk / "ports/include"), str(snapshot / "pak_layout_hosttest.c"),
               str(snapshot / "w_pack.c"), "-Wl,-l:liblz4.so.1", "-o", str(executable)]
    if sanitize:
        command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    (out / (label + "-build.log")).write_text(result.stdout + result.stderr)
    assert result.returncode == 0, result.stderr
    return executable


def run_reader(executable, pack, out, label, nohead=False):
    raw, table = out / (label + ".raw"), out / (label + ".tsv")
    t0 = time.perf_counter()
    result = subprocess.run([str(executable), str(pack), str(table)] + (["nohead"] if nohead else []),
                            capture_output=True, text=True, env=dict(os.environ, PACK_LITERAL_OUT=str(raw)), timeout=180)
    (out / (label + ".log")).write_text(result.stdout + result.stderr)
    assert result.returncode == 0, (label, result.returncode, result.stderr[-3000:])
    info = {}
    for line in result.stderr.splitlines():
        if line.startswith("open:") or line.startswith("iostat"):
            tag, *pairs = line.split()
            info[tag.rstrip(":")] = {k: int(v) for k, v in (pair.split("=") for pair in pairs if "=" in pair)}
    match = re.search(r"(\d+) partial reads, (\d+) failures", result.stderr)
    assert match and int(match[2]) == 0
    return raw, table, dict(seconds=round(time.perf_counter() - t0, 4), decoded_bytes=raw.stat().st_size,
                            partial_reads=int(match[1]), io=info)


def fixtures(executable, out):
    directory = out / "fixtures"
    directory.mkdir(exist_ok=True)
    # Large compressed lumps leave sector-alignment gaps; cold raw data fits
    # them. Aliases, an empty folder, a directory-order predecessor and protected
    # first-use entries exercise the choices the stock corpus does not isolate.
    import random
    rnd = random.Random(1701)
    entries = [("Cold/FIRST", bytes(rnd.randrange(256) for _ in range(700))),
               ("Keep/EARLY", b"keep early" * 300), ("Big/A", b"large A" * 30000),
               ("Cold/SMALL", bytes(rnd.randrange(256) for _ in range(1000))),
               ("Keep/LATE", b"keep late" * 300), ("Big/B", b"large B" * 29000),
               ("Cold/TINY", b"tiny"), ("Empty/", b"")]
    entries.append(("Cold/ALIAS", entries[3][1]))
    source = directory / "source.pk3"
    with zipfile.ZipFile(source, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in entries:
            archive.writestr(name, data)
    order = ["Keep/LATE", "Keep/EARLY", "Big/A", "Big/B"]
    before, after = directory / "reference.PAK", directory / "candidate.PAK"
    cook.cook(source, before, 1, order=order, gap_fill=False)
    cooked = cook.cook(source, after, 1, order=order, gap_fill=True)
    checked = compare(before, after, order, source)
    assert checked["saved_bytes"] > 0 and cooked["gap_lumps"] > 0
    roundtrip = directory / "roundtrip.PAK"
    cook.cook(after, roundtrip, 1, order=order, gap_fill=True)
    assert literal_equal(after, roundtrip), "layout-only recook is not idempotent"
    a, at, _ = run_reader(executable, before, directory, "reference")
    b, bt, _ = run_reader(executable, after, directory, "candidate")
    assert literal_equal(a, b) and literal_equal(at, bt)
    # A valid, CRC-consistent payload change must still fail literal equality.
    data = bytearray(after.read_bytes())
    m = metadata(after)
    changed_index = 0  # a raw cold lump, including when stored in an alignment gap
    pos, disksize, size, _fo, _lo, codec = m["table"][changed_index]
    assert codec == 0 and size == disksize
    data[pos + size - 1] ^= 1
    _es, _ho, _hb, co, *_checks = struct.unpack_from("<8I", data, 64)
    struct.pack_into("<I", data, co + 4 * changed_index, zlib.crc32(data[pos:pos + size]))
    crcs = data[co:co + 4 * len(entries)]
    struct.pack_into("<I", data, 64 + 28, verify_pack.fletcher(crcs))
    changed = directory / "negative-valid-but-changed.PAK"
    changed.write_bytes(data)
    c, _ct, _ = run_reader(executable, changed, directory, "negative-valid-but-changed")
    assert not literal_equal(a, c), "literal comparator accepted changed decoded data"
    # A startup-order regression has valid content but must fail the offset proof.
    unordered = directory / "negative-startup-order.PAK"
    cook.cook(source, unordered, 1, order=None, gap_fill=True)
    detected = False
    try:
        compare(before, unordered, order, source)
    except AssertionError as error:
        detected = "protected startup payload" in str(error)
    assert detected, "protected-offset comparator accepted a startup-order regression"
    messages = []
    failures = test_pack_reader.validation_tests(executable, directory, messages.append)
    failures += test_pack_reader.v2_tests(executable, directory, messages.append)
    (directory / "tests.log").write_text("\n".join(messages) + "\n")
    assert failures == 0, messages[-6:]
    return dict(gap_fixture=checked, raw_bytes_checked=True, negative_changed_payload_detected=True,
                negative_startup_order_detected=True, recook_idempotent=True, malformed_rejections=26,
                existing_validation_failures=failures)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, default=ROOT / "build/pak")
    parser.add_argument("--candidate", type=Path, default=ROOT / "build/pak-opt17")
    parser.add_argument("--src", type=Path, default=Path("/workspace/.onboarding/srb2-assets"))
    parser.add_argument("--out", type=Path, default=ROOT / "build/nov17-pak/verify")
    parser.add_argument("--reader", type=Path, default=ROOT / "src/w_pack.c")
    parser.add_argument("--order", type=Path, default=cook.DEFAULT_ORDER)
    parser.add_argument("--rebuilt-reference", type=Path, help="assert an opt-out recook is literally identical to the original packs")
    args = parser.parse_args()
    args.out = args.out.resolve()
    assert args.reference.resolve() != args.candidate.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    os.environ["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    os.environ["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    inputs = []
    for pk3, pak in cook.PACKS:
        inputs.extend((args.src / pk3, args.reference / pak, args.candidate / pak))
        for directory in (args.reference, args.candidate):
            sidecar = directory / (pak + ".pics.json")
            if sidecar.exists():
                inputs.append(sidecar)
    for directory in (args.reference, args.candidate):
        fine = directory / "FINEACON.DAT"
        if fine.exists():
            inputs.append(fine)
    preservation = {str(path): dict(bytes=path.stat().st_size, sha256=digest(path), md5=digest(path, "md5")) for path in inputs}
    report = dict(inputs=preservation, packs={}, reader_sources={}, source_md5_note=
                  "SRP2 v1/v2 has no embedded source MD5; reserved bytes and original archive MD5 are preserved. "
                  "Existing PS2 cooked-pack zero-digest semantics and add-on hashing are unchanged.")
    with zipfile.ZipFile(args.src / "srb2.pk3") as archive:
        model = strip_pics.PyOracle(strip_pics.read_palette(archive.read("PLAYPAL")))
    order = order_names(args.order)
    for pk3, pak in cook.PACKS:
        reference, candidate = args.reference / pak, args.candidate / pak
        row = compare(reference, candidate, order, args.src / pk3)
        errors, _n, _stats = verify_pack.verify(args.src / pk3, candidate, model=model, require_cooked=True)
        assert errors == 0, (pak, errors)
        if args.rebuilt_reference:
            assert literal_equal(reference, args.rebuilt_reference / pak), f"{pak}: opt-out differs from original"
            row["optout_reproduces_original_bytes"] = True
        row["independent_archive_verifier_errors"] = errors
        report["packs"][pak] = row
        print(pak, row, flush=True)
    reader, asan = build(args.out, args.reader), build(args.out, args.reader, sanitize=True)
    for path in sorted((args.out / "source").iterdir()):
        report["reader_sources"][path.name] = digest(path)
    for _pk3, pak in cook.PACKS:
        raw, table, first = run_reader(reader, args.reference / pak, args.out, "reference-" + pak)
        candidate_raw, candidate_table, second = run_reader(reader, args.candidate / pak, args.out, "candidate-" + pak)
        assert literal_equal(raw, candidate_raw) and literal_equal(table, candidate_table), pak
        assert first["decoded_bytes"] == report["packs"][pak]["decoded_bytes"]
        # After startup, all reads take the file path; sanitizer and red zones
        # check that the packed gaps need no overread or neighboring payload.
        checked_raw, checked_table, checked = run_reader(asan, args.candidate / pak, args.out, "asan-nohead-" + pak, nohead=True)
        assert literal_equal(raw, checked_raw) and literal_equal(table, checked_table), pak
        report["packs"][pak]["c_reader"] = dict(reference=first, candidate=second, asan_nohead=checked,
                                                literal_decoded_bytes_equal=True, metadata_equal=True)
        print(pak, "actual C full bytes, metadata, partial reads and ASan/UBSan pass", flush=True)
    report["fixtures"] = fixtures(asan, args.out)
    changed = [path for path, value in preservation.items() if digest(Path(path)) != value["sha256"]]
    assert not changed, changed
    report["inputs_changed"] = changed
    report["total_lumps"] = sum(row["lumps"] for row in report["packs"].values())
    report["total_decoded_bytes"] = sum(row["decoded_bytes"] for row in report["packs"].values())
    report["total_saved_bytes"] = sum(row["saved_bytes"] for row in report["packs"].values())
    report["tools"] = {str(path.relative_to(ROOT)): digest(path) for path in
                       (Path(__file__), ROOT / "tools/ps2/cook.py", ROOT / "tools/ps2/verify_pack.py", args.order)}
    (args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("PASS", report["total_lumps"], "lumps,", report["total_decoded_bytes"], "literal decoded bytes,",
          report["total_saved_bytes"], "stored bytes saved", flush=True)


if __name__ == "__main__":
    main()
