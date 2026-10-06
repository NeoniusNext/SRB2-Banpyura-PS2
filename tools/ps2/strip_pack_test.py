"""Small independent fixtures for cooked sidecar verification and cooker subset/reuse.

Writes only --out. Uses an original asset PNG in a disposable zones.pk3 to exercise
--only without srb2.pk3 conversion (the palette still comes from srb2.pk3).
"""
import argparse
import json
import sys
import zipfile
from pathlib import Path

import cook
import strip_pics as sp
import verify_pack as vp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--src', type=Path, default=sp.ROOT / 'srb2-assets')
    a = ap.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    messages = []

    def log(message):
        print(message, flush=True)
        messages.append(message)
        (out / 'test.log').write_text('\n'.join(messages) + '\n', encoding='utf-8')

    with zipfile.ZipFile(a.src / 'srb2.pk3') as z:
        pal = z.read('PLAYPAL')
        png = next(z.read(zi) for zi in z.infolist() if not zi.is_dir() and z.read(zi)[:8] == sp.PNG_SIG)
    with zipfile.ZipFile(out / 'srb2.pk3', 'w') as z:
        z.writestr('PLAYPAL', pal)
    pk3, pak = out / 'zones.pk3', out / 'ZONES.PAK'
    with zipfile.ZipFile(pk3, 'w') as z:
        z.writestr('Textures/', b'')
        z.writestr('Textures/fixture.png', png)
    converted = cook.convert_pngs(out, out / 'tool', ['zones.pk3'], log)
    assert len(converted) == 1 and ('zones.pk3', 1) in converted
    pics = {i: e for (p, i), e in converted.items() if p == 'zones.pk3'}
    cook.cook(pk3, pak, 1, pics)
    sidecar = Path(str(pak) + '.pics.json')
    manifest = sidecar.read_bytes()

    def check(label, reject):
        model = sp.PyOracle(sp.read_palette(pal))
        bad, n, _ = vp.verify(pk3, pak, model, True, log)
        good = bool(bad) == reject
        log(f'{label}: {n} entries, {bad} discrepancies, {"PASS" if good else "FAIL"}')
        return int(not good)

    failures = check('subset zones.pk3 with original PNG decoder', False)
    meta = json.loads(manifest)
    for label, change in [
        ('duplicate picture index', lambda m: m['entries'].append(dict(m['entries'][0]))),
        ('out-of-range picture index', lambda m: m['entries'].append(dict(m['entries'][0], index=99))),
        ('wrong picture name', lambda m: m['entries'][0].update(name='wrong.png')),
        ('wrong cooked CRC', lambda m: m['entries'][0].update(cooked_crc32=0)),
        ('wrong PNG CRC', lambda m: m['entries'][0].update(png_crc32=0)),
        ('wrong offsets', lambda m: m['entries'][0].update(leftoffset=1)),
    ]:
        m = json.loads(manifest)
        change(m)
        sidecar.write_text(json.dumps(m), encoding='utf-8')
        failures += check(label, True)
    sidecar.unlink()
    failures += check('missing cooked sidecar', True)
    sidecar.write_bytes(manifest)
    cook.cook(pk3, pak, 1)  # --keep-png behaviour, same disposable output path
    if sidecar.exists():
        failures += 1
        log('reuse as raw PNG: FAIL (stale sidecar retained)')
    else:
        log('reuse as raw PNG: PASS (stale sidecar removed)')
    failures += check('raw PNG rejected by require-cooked', True)
    bad, _, _ = vp.verify(pk3, pak, log=log)
    failures += int(bool(bad))
    log(f'legacy raw-PNG comparison: {bad} discrepancies')
    log(f'TOTAL failures: {failures}; 8 rejection controls')
    return int(bool(failures))


if __name__ == '__main__':
    sys.exit(main())
