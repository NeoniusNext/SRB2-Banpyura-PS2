"""Host test for the PNG -> cooked picture replacement (PS2-20).

usage: strip_pics_test.py [--out DIR] [--pak DIR] [--src DIR] [--negative-controls]

1. extracts every PNG lump of the four pk3 (srb2-assets) in pk3 order;
2. builds the ORIGINAL engine PNG path (src/r_picformats.c + libpng, tools/ps2/strip_pics_host.c) and converts all of them in
   that order (engine's persistent nearest-colour memo): pixels/transparency/offsets of the patch and the flat;
3. encodes them as cooked pictures (strip_pics.py) -- or, with --pak, takes the lumps out of the cooked packs;
4. builds the PS2-profile path (src/r_picformats.c with PS2_PROFILE, no libpng/zlib), runs Picture_PNGDimensions/
   Picture_PNGConvert on the cooked lumps and compares the .matrix/.flat dumps byte for byte with the original;
5. also compares the pure-Python model of the decoder, conversion order sensitivity (forward/reverse/fresh memo),
   encoder round-trips on synthetic tall/holey patches (run through the engine code as well);
6. --negative-controls: damaged lumps must make the comparison fail.
Exit code 0 only with 0 differences. Nothing under srb2-assets or build/pak is written.
"""
import argparse
import random
import struct
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import strip_pics as sp  # noqa: E402
from verify_pack import PAIRS, cstr  # noqa: E402

ROOT = sp.ROOT


def extract_pngs(src):
    out = []
    for pk3, pak in PAIRS:
        z = zipfile.ZipFile(Path(src) / pk3)
        for i, zi in enumerate(z.infolist()):
            if zi.file_size >= 8 and not zi.is_dir():
                d = z.read(zi)
                if d[:8] == sp.PNG_SIG:
                    out.append((pk3, pak, i, zi.filename, d))
    return out


def pack_lump(pak_path, index):
    """Decoded lump `index` of an SRP2 pack (independent reader from verify_pack)."""
    from verify_pack import decode
    with open(pak_path, 'rb') as f:
        hdr = f.read(64)
        _m, _v, _hs, _fl, n, table_off, pool_off, pool_size, data_off, file_size, block, _r = struct.unpack('<4s11I', hdr[:48])
        f.seek(table_off + 24 * index)
        pos, disksize, size, fo, lo, codec = struct.unpack('<6I', f.read(24))
        return decode(f, pos, disksize, size, codec, block)


def compare_dirs(ref, got, count, label, log):
    """Byte compare NNN.matrix / NNN.flat of two dumps; returns number of differing files."""
    bad = 0
    for i in range(count):
        for ext in ('matrix', 'flat'):
            a = (ref / f'{i:03d}.{ext}').read_bytes()
            b = (got / f'{i:03d}.{ext}').read_bytes()
            if a != b:
                bad += 1
                log(f'  {label}: {i:03d}.{ext} differs ({len(a)} vs {len(b)} bytes)')
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=str(ROOT / 'build/agent-strip/pics-test'))
    ap.add_argument('--src', default=str(ROOT / 'srb2-assets'))
    ap.add_argument('--pak', default=None, help='directory with cooked packs (strip-profile cook output): lumps are taken from them')
    ap.add_argument('--negative-controls', action='store_true')
    a = ap.parse_args()
    out = Path(a.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    messages = []

    def log(m):
        print(m, flush=True)
        messages.append(m)
        (out / 'test.log').write_text('\n'.join(messages) + '\n', encoding='utf-8')

    pngs = extract_pngs(a.src)
    log(f'{len(pngs)} PNG lumps in the four pk3 (pk3 order)')
    pngdir = out / 'png'
    pngdir.mkdir(exist_ok=True)
    files = []
    for k, (pk3, pak, idx, name, data) in enumerate(pngs):
        p = pngdir / f'{k:03d}.png'
        p.write_bytes(data)
        files.append(p)
    with zipfile.ZipFile(Path(a.src) / 'srb2.pk3') as z:
        playpal = z.read('PLAYPAL')
    (out / 'PLAYPAL').write_bytes(playpal)
    pal = sp.read_palette(playpal)

    total_bad = 0
    log('building the original engine PNG path (HAVE_PNG, libpng) and the PS2 profile path (PS2_PROFILE, no libpng)')
    oracle = sp.build_oracle(out / 'oracle-build')
    check = sp.build_check(out / 'check-build')

    # 1. original decoder, pk3 order (this is the reference order used by the cooker)
    log(sp.run_tool(oracle, out / 'PLAYPAL', out / 'oracle', files).strip())
    mats = [sp.read_matrix(out / 'oracle' / f'{k:03d}.matrix') for k in range(len(files))]

    # 2. order sensitivity of the original decoder (the memo keeps the first colour seen per RGB565 bucket)
    sp.run_tool(oracle, out / 'PLAYPAL', out / 'oracle-rev', list(reversed(files)))
    diff_order = 0
    for k in range(len(files)):
        j = len(files) - 1 - k
        if (out / 'oracle' / f'{k:03d}.matrix').read_bytes() != (out / 'oracle-rev' / f'{j:03d}.matrix').read_bytes():
            diff_order += 1
    diff_fresh = 0
    for k, f in enumerate(files):
        sp.run_tool(oracle, out / 'PLAYPAL', out / f'oracle-fresh-{k:03d}', [f])
        if (out / 'oracle' / f'{k:03d}.matrix').read_bytes() != (out / f'oracle-fresh-{k:03d}' / '000.matrix').read_bytes():
            diff_fresh += 1
    log(f'original decoder: {diff_order} of {len(files)} images change when the conversion order is reversed, '
        f'{diff_fresh} change with an empty memo (cook order = pk3 order is the canonical one)')

    # 3. pure-Python model of the original decoder, same order
    model = sp.PyOracle(pal)
    bad_model = 0
    for k, (pk3, pak, idx, name, data) in enumerate(pngs):
        m = model.convert(data)
        if m != mats[k]:
            bad_model += 1
            log(f'  python model differs on {name}')
    log(f'independent Python decoder model vs original decoder: {bad_model} differing images of {len(pngs)}')
    total_bad += bad_model

    # 4. cooked lumps (fresh encoder output, and/or the ones inside the packs)
    cooked = [sp.cooked_from_matrix(*m) for m in mats]
    cdir = out / 'cooked'
    cdir.mkdir(exist_ok=True)
    for k, c in enumerate(cooked):
        (cdir / f'{k:03d}.lmp').write_bytes(c)
    if a.pak:
        bad_pack = 0
        for k, (pk3, pak, idx, name, data) in enumerate(pngs):
            lump = pack_lump(Path(a.pak) / pak, idx)
            if lump != cooked[k]:
                bad_pack += 1
                log(f'  pack lump {pak}[{idx}] {name} differs from the encoder output')
        log(f'lumps in {a.pak}: {len(pngs)} compared with the encoder output, {bad_pack} differences')
        total_bad += bad_pack
        for k, (pk3, pak, idx, name, data) in enumerate(pngs):
            (cdir / f'{k:03d}.lmp').write_bytes(pack_lump(Path(a.pak) / pak, idx))
    log(sp.run_tool(check, out / 'PLAYPAL', out / 'check', [cdir / f'{k:03d}.lmp' for k in range(len(files))]).strip().splitlines()[0] + ' ...')
    bad = compare_dirs(out / 'oracle', out / 'check', len(files), 'cooked vs original', log)
    px = sum(m[0] * m[1] for m in mats)
    tp = sum(m[4].count(sp.TRANSPARENT) for m in mats)
    log(f'cooked pictures run through the engine functions vs the original decoder: {len(files)} images, {px} pixels '
        f'({tp} transparent), offsets+matrix+flat byte compare -> {bad} differing files')
    total_bad += bad
    sizes = [(len(c), m[0], m[1]) for c, m in zip(cooked, mats)]
    log('cooked sizes (bytes, w, h): ' + ', '.join(f'{s}/{w}x{h}' for s, w, h in sizes))

    # 5. encoder round trips (tall patches, holes, dummy posts) and the engine code on them
    rng = random.Random(1)
    synth = []
    for (w, h, mode) in [(1, 1, 'full'), (3, 5, 'holes'), (4, 300, 'full'), (4, 300, 'holes'), (5, 512, 'full'), (5, 512, 'sparse'),
                         (3, 1024, 'full'), (3, 1024, 'sparse'), (2, 2048, 'full'), (2, 2048, 'sparse'), (7, 255, 'full'),
                         (7, 256, 'full'), (7, 509, 'sparse'), (6, 600, 'tail')]:
        pix = []
        for y in range(h):
            for x in range(w):
                if mode == 'full':
                    pix.append((x * 31 + y * 7) % 256)
                elif mode == 'holes':
                    pix.append(sp.TRANSPARENT if rng.random() < 0.3 else rng.randrange(256))
                elif mode == 'sparse':
                    pix.append(rng.randrange(256) if rng.random() < 0.02 or (y > h - 5) else sp.TRANSPARENT)
                else:  # tail: opaque only at the very bottom
                    pix.append(rng.randrange(256) if y >= h - 3 else sp.TRANSPARENT)
        synth.append((w, h, 3, -2, pix))
    sdir = out / 'synth'
    sdir.mkdir(exist_ok=True)
    bad_syn = 0
    for k, m in enumerate(synth):
        patch = sp.encode_doom_patch(*m)
        if sp.decode_doom_patch(patch) != m:
            bad_syn += 1
            log(f'  python round trip failed for synthetic {k}: {m[0]}x{m[1]}')
        (sdir / f'{k:03d}.lmp').write_bytes(sp.MARKER + patch)
        # expected dumps in the host tool's format
        (sdir / f'{k:03d}.matrix.expected').write_bytes(struct.pack('<4i', *m[:4]) + struct.pack(f'<{len(m[4])}H', *m[4]))
        (sdir / f'{k:03d}.flat.expected').write_bytes(bytes(255 if v == sp.TRANSPARENT else v for v in m[4]))
    sp.run_tool(check, out / 'PLAYPAL', out / 'synth-check', [sdir / f'{k:03d}.lmp' for k in range(len(synth))])
    for k in range(len(synth)):
        for ext in ('matrix', 'flat'):
            if (out / 'synth-check' / f'{k:03d}.{ext}').read_bytes() != (sdir / f'{k:03d}.{ext}.expected').read_bytes():
                bad_syn += 1
                log(f'  engine decode of synthetic {k} ({synth[k][0]}x{synth[k][1]}) differs in .{ext}')
    log(f'{len(synth)} synthetic patches (tall, holey, sparse, up to 2048 high): Python round trip and engine decode -> {bad_syn} failures')
    total_bad += bad_syn

    # 6. negative controls
    if a.negative_controls:
        ctrl = 0
        ndir = out / 'negative'
        ndir.mkdir(exist_ok=True)
        # (a) one pixel changed inside a post
        c = bytearray(cooked[0])
        c[8 + 8 + 4 * mats[0][0] + 4] ^= 0x55
        (ndir / 'pixel.lmp').write_bytes(c)
        # (b) the left offset changed
        c = bytearray(cooked[1])
        c[8 + 4] ^= 1
        (ndir / 'offset.lmp').write_bytes(c)
        # (c) a transparent pixel made opaque: first column's first post top moved
        c = bytearray(cooked[0])
        c[8 + 8 + 4 * mats[0][0]] ^= 1
        (ndir / 'top.lmp').write_bytes(c)
        for name, base in [('pixel.lmp', 0), ('offset.lmp', 1), ('top.lmp', 0)]:
            sp.run_tool(check, out / 'PLAYPAL', ndir / ('out-' + name), [ndir / name])
            same = all((ndir / ('out-' + name) / f'000.{e}').read_bytes() == (out / 'oracle' / f'{base:03d}.{e}').read_bytes() for e in ('matrix', 'flat'))
            log(f'negative control {name}: comparison {"MISSED the damage" if same else "detects it"}')
            ctrl += int(same)
        # (d) a PNG fed to the profile path is rejected (no decoder)
        import subprocess
        p = subprocess.run([str(check), str(out / 'PLAYPAL'), str(ndir / 'out-png'), str(files[0])], capture_output=True, text=True)
        rejected = p.returncode != 0
        log(f'negative control raw PNG into the profile path: exit {p.returncode} ({"rejected" if rejected else "ACCEPTED"})')
        ctrl += int(not rejected)
        total_bad += ctrl

    log(f'TOTAL differences/failures: {total_bad}')
    return 1 if total_bad else 0


if __name__ == '__main__':
    sys.exit(main())
