#!/usr/bin/env python3
"""Measure a reversible block-local PCM predictor; never writes game packs.

This is a format-design experiment, not a codec supported by the PS2 reader.
Each 64 KiB block keeps an original first-byte anchor; the first block also
keeps the eight-byte DMX header. All other bytes are modulo-256 differences.
The complete LZ4 output is decoded and the predictor reversed before counting
any opportunity. The 90% acceptance rule remains the existing cooker's rule.
"""
import argparse
from concurrent.futures import ProcessPoolExecutor
import hashlib
import io
import json
from pathlib import Path
import time

import cook
import verify_pack

ROOT = Path(__file__).resolve().parents[2]
_reader = None


def predict(data):
    output = bytearray(data)
    for base in range(0, len(data), cook.BLOCK):
        start = 9 if base == 0 else base + 1
        for i in range(start, min(base + cook.BLOCK, len(data))):
            output[i] = (data[i] - data[i - 1]) & 255
    return bytes(output)


def restore(data):
    output = bytearray(data)
    for base in range(0, len(data), cook.BLOCK):
        start = 9 if base == 0 else base + 1
        for i in range(start, min(base + cook.BLOCK, len(data))):
            output[i] = (output[i] + output[i - 1]) & 255
    return bytes(output)


def measure(item):
    global _reader
    path, i = item
    if _reader is None or _reader[0] != path:
        _reader = path, cook.PackReader(path)
    reader = _reader[1]
    pos, disksize, size, _fo, _lo, codec = cook.ENTRY.unpack_from(reader.data, reader.toff + 24 * i)
    data = reader.get(i)
    if data[:2] != b"\x03\0" or len(data) < 9:
        return None
    delta = predict(data)
    payload = cook.lz4_lump(delta)
    decoded = verify_pack.decode(io.BytesIO(payload), 0, len(payload), len(data), cook.CM_LZ4, cook.BLOCK)
    assert restore(decoded) == data, (reader.names[i], "inverse predictor differs")
    adopted = len(payload) < disksize and len(payload) <= size * cook.MAX_RATIO
    return dict(index=i, name=reader.names[i].decode("ascii"), decoded_bytes=size,
                source_codec=codec, existing_payload_bytes=disksize,
                predicted_lz4_bytes=len(payload), accepted=adopted,
                chosen_payload_bytes=len(payload) if adopted else disksize,
                payload_saved_bytes=disksize - len(payload) if adopted else 0,
                original_position=pos, decoded_sha256=hashlib.sha256(data).hexdigest(),
                literal_inverse_checked=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pak", type=Path, default=ROOT / "build/pak")
    parser.add_argument("--out", type=Path, default=ROOT / "build/nov17-pak/predictor.json")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    assert args.jobs > 0
    started = time.perf_counter()
    report = dict(production_codec_implemented=False, format_change_required=True,
                  byte_exact_inverse=True, block_bytes=cook.BLOCK, maximum_stored_ratio=cook.MAX_RATIO,
                  savings_scope="Unique stored payload bytes only; not file/alignment or resident RAM savings.", packs={})
    for _source, pack in cook.PACKS:
        path = args.pak / pack
        before = hashlib.sha256(path.read_bytes()).hexdigest()
        reader = cook.PackReader(path)
        seen, items = set(), []
        for i, name in enumerate(reader.names):
            entry = cook.ENTRY.unpack_from(reader.data, reader.toff + 24 * i)
            if entry[2] and cook.engine_names(name)[0].startswith(b"DS") and entry[0] not in seen:
                items.append((str(path), i))
                seen.add(entry[0])
        with ProcessPoolExecutor(args.jobs) as executor:
            rows = [row for row in executor.map(measure, items, chunksize=4) if row is not None]
        report["packs"][pack] = dict(source_sha256=before, dmx_unique_lumps=len(rows),
                                    accepted_lumps=sum(row["accepted"] for row in rows),
                                    decoded_bytes=sum(row["decoded_bytes"] for row in rows),
                                    existing_payload_bytes=sum(row["existing_payload_bytes"] for row in rows),
                                    chosen_payload_bytes=sum(row["chosen_payload_bytes"] for row in rows),
                                    payload_saved_bytes=sum(row["payload_saved_bytes"] for row in rows), entries=rows)
        assert hashlib.sha256(path.read_bytes()).hexdigest() == before, "source pack changed"
    report["total_payload_saved_bytes"] = sum(row["payload_saved_bytes"] for row in report["packs"].values())
    report["seconds"] = round(time.perf_counter() - started, 4)
    report["audit_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: value for key, value in report.items() if key != "packs"}, indent=2))
    for name, row in report["packs"].items():
        print(name, {key: value for key, value in row.items() if key != "entries"})


if __name__ == "__main__":
    main()
