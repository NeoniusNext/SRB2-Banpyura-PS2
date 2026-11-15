"""Create deterministic solid-color/fog FOF scene data for paired full engines."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, default=ROOT / 'build/nov14-next7/fog-addons')
    a = p.parse_args()
    out = a.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    # Width=height=1, one opaque palette-index sample. Validate actual post data.
    patch = struct.pack('<hhhhI', 1, 1, 0, 0, 12) + bytes([0, 1, 0, 73, 0, 255])
    width, height, left, top, offset = struct.unpack_from('<hhhhI', patch)
    assert (width, height, left, top, offset) == (1, 1, 0, 0, 12)
    assert patch[offset:] == bytes([0, 1, 0, 73, 0, 255])
    files = {'Textures/NX7SOL.lmp': patch,
             'Lua/NX7FOG.lua': (ROOT / 'tools/ps2/next7_render_fog_fixture.lua').read_bytes()}
    target = out / 'NX7_FOG.pk3'
    with zipfile.ZipFile(target, 'w') as archive:
        for name, data in files.items():
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data)
    manifest = {'addon_sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
                'texture_dimensions': [1, 1], 'texture_palette_sample': 73,
                'files': {name: {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                          for name, data in files.items()},
                'notes': 'Existing sloped base planes get a 1x1 flat; renderable FOFs cycle fog/translucent/ripple water. Geometry and collision flags remain. READY counts describe configured planes, not observed drawer calls. Use identical seed/tics in paired engine captures.'}
    (out / 'NX7_FOG.expected.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(target)
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
