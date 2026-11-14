"""Build the deterministic next5 NPOT tilted floor-sprite/flat integration add-on."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools/ps2"
WIDTH, HEIGHT = 63, 45


def pixels(masked=True):
    # Broad transparent holes, several colors, and an asymmetric visible stripe.
    return bytes(255 if masked and ((x // 8 + y // 8) % 4 == 0)
                 else (72 if x < 6 else 1 + (x * 3 + y * 5 + (x ^ y)) % 253)
                 for y in range(HEIGHT) for x in range(WIDTH))


def patch(data):
    header_size = 8 + 4 * WIDTH
    offsets, columns = [], bytearray()
    for x in range(WIDTH):
        offsets.append(header_size + len(columns))
        y = 0
        while y < HEIGHT:
            if data[y * WIDTH + x] == 255:
                y += 1
                continue
            first = y
            while y < HEIGHT and data[y * WIDTH + x] != 255:
                y += 1
            columns.extend(bytes([first, y - first, 0]))
            columns.extend(data[row * WIDTH + x] for row in range(first, y))
            columns.append(0)
        columns.append(255)
    result = struct.pack("<hhhh", WIDTH, HEIGHT, WIDTH // 2, HEIGHT // 2) + struct.pack("<%dI" % WIDTH, *offsets) + columns
    # Parse the actual encoded posts independently, checking every sample/offset.
    decoded = bytearray([255]) * (WIDTH * HEIGHT)
    for x, offset in enumerate(struct.unpack_from("<%dI" % WIDTH, result, 8)):
        while result[offset] != 255:
            first, length, _ = struct.unpack_from("<BBB", result, offset)
            assert first + length <= HEIGHT
            for row in range(length):
                decoded[(first + row) * WIDTH + x] = result[offset + 3 + row]
            offset += length + 4
    assert decoded == data
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov14-next5/npot-addons")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    data = pixels()
    files = {"Sprites/NX5FA0.lmp": patch(data),
             "Textures/NX5FLAT.lmp": patch(data),
             "Textures/NX5BASE.lmp": patch(pixels(masked=False)),
             "Lua/NX5FLOOR.lua": (TOOLS / "next5_render_npot_fixture.lua").read_bytes()}
    target = args.out / "NX5_FLOOR.pk3"
    with zipfile.ZipFile(target, "w") as archive:
        for name, contents in files.items():
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.external_attr = 0o100644 << 16
            archive.writestr(info, contents)
    manifest = {"addon_sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
                "sprite_width": WIDTH, "sprite_height": HEIGHT, "sprites": 8, "sprite_slopes": 8,
                "sprite_opaque_pixels": sum(v != 255 for v in data),
                "sprite_transparent_pixels": data.count(255),
                "files": {name: {"bytes": len(contents), "sha256": hashlib.sha256(contents).hexdigest()}
                          for name, contents in files.items()},
                "notes": "Use identical seed/tics in paired engines. READY counts describe configured objects/planes, not drawer calls. Textures/ Doom patches carry exact NPOT dimensions without a PNG decoder."}
    (args.out / "NX5_FLOOR.expected.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(target)
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
