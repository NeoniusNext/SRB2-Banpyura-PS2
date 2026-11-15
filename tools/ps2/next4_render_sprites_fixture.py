"""Build the deterministic next4 ordinary P2 floor-sprite/flat integration add-on."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools/ps2"


def pixels():
    # Broad transparent holes, several colors, and an asymmetric visible stripe.
    return bytes(255 if ((x // 8 + y // 8) % 4 == 0)
                 else (72 if x < 6 else 1 + (x * 3 + y * 5 + (x ^ y)) % 253)
                 for y in range(64) for x in range(64))


def patch(data):
    header_size = 8 + 4 * 64
    offsets, columns = [], bytearray()
    for x in range(64):
        offsets.append(header_size + len(columns))
        y = 0
        while y < 64:
            if data[y * 64 + x] == 255:
                y += 1
                continue
            first = y
            while y < 64 and data[y * 64 + x] != 255:
                y += 1
            columns.extend(bytes([first, y - first, 0]))
            columns.extend(data[row * 64 + x] for row in range(first, y))
            columns.append(0)
        columns.append(255)
    result = struct.pack("<hhhh64I", 64, 64, 32, 32, *offsets) + columns
    # Parse the actual encoded posts independently, checking every sample/offset.
    decoded = bytearray([255]) * 4096
    for x, offset in enumerate(struct.unpack_from("<64I", result, 8)):
        while result[offset] != 255:
            first, length, _ = struct.unpack_from("<BBB", result, offset)
            assert first + length <= 64
            for row in range(length):
                decoded[(first + row) * 64 + x] = result[offset + 3 + row]
            offset += length + 4
    assert decoded == data
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov14-next4/sprites-addons")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    data = pixels()
    files = {"Sprites/NX4FA0.lmp": patch(data), "Flats/NX4FLAT": data,
             "Lua/NX4FLOOR.lua": (TOOLS / "next4_render_sprites_fixture.lua").read_bytes()}
    target = args.out / "NX4_FLOOR.pk3"
    with zipfile.ZipFile(target, "w") as archive:
        for name, contents in files.items():
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.external_attr = 0o100644 << 16
            archive.writestr(info, contents)
    manifest = {"addon_sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
                "sprite_width": 64, "sprite_height": 64, "sprites": 8,
                "sprite_opaque_pixels": sum(v != 255 for v in data),
                "sprite_transparent_pixels": data.count(255),
                "files": {name: {"bytes": len(contents), "sha256": hashlib.sha256(contents).hexdigest()}
                          for name, contents in files.items()},
                "notes": "Use identical seed/tics in paired engines; READY FOF counts are configured planes, not draw-call counters."}
    (args.out / "NX4_FLOOR.expected.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(target)
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
