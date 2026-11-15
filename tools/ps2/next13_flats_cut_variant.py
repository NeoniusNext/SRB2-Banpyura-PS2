#!/usr/bin/env python3
"""Prepare isolated flat v2: size_t removed-byte count, preserving v1 resets."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path

import next13_flats_variant as v1

ROOT, BASE, PATH, GUARD, REVISION = v1.ROOT, v1.BASE, v1.PATH, v1.GUARD, v1.REVISION
OUT = ROOT / 'build/nov15-next13/flats-cut-source-v2'


def replace(text, old, new, count=1):
    if text.count(old) != count:
        raise RuntimeError('Flat cut anchor changed: ' + old[:100])
    return text.replace(old, new)


def rewrite(baseline):
    text = v1.rewrite(baseline)
    text = replace(text, 'static boolean gl_flats_compact;',
        '// Zero keeps the original stride, including after initialization/reset.\n'
        'static size_t gl_flats_cut;')
    text = replace(text,
        '\tif (gl_flats_compact)\n\t\treturn &((GLMipmap_t *)gl_flats)[n];\n'
        '\treturn &((GLMapTexture_t *)gl_flats)[n].mipmap;',
        '\tconst size_t stride = sizeof(GLMapTexture_t) - gl_flats_cut;\n\n'
        '\treturn (GLMipmap_t *)((UINT8 *)gl_flats + n * stride);')
    text = replace(text, 'gl_flats_compact = HWR_UseCompactFlatMetadata();',
        'gl_flats_cut = HWR_UseCompactFlatMetadata() ? sizeof(GLMapTexture_t) - sizeof(GLMipmap_t) : 0;')
    text = replace(text, 'gl_flats_compact ? sizeof(GLMipmap_t) : sizeof(GLMapTexture_t)',
        'sizeof(GLMapTexture_t) - gl_flats_cut')
    text = replace(text, 'gl_flats_compact = false;', 'gl_flats_cut = 0;', 4)
    if 'gl_flats_compact' in text:
        raise RuntimeError('A v1 layout boolean remains')
    return text


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, default=OUT)
    a = p.parse_args()
    baseline = v1.sources()
    candidate = dict(baseline)
    candidate[PATH] = rewrite(baseline[PATH].decode()).encode()
    v1.prepare(BASE, baseline)
    v1.prepare(a.out, candidate)
    manifest = {'revision': REVISION, 'source_count': len(baseline), 'baseline': str(BASE),
        'candidate': str(a.out), 'variant': 'flat-cut-v2',
        'source_sha256': {p: sha(v) for p, v in baseline.items()},
        'candidate_source_sha256': {p: sha(v) for p, v in candidate.items()},
        'changed': [p for p in baseline if baseline[p] != candidate[p]],
        'tool_sha256': sha(Path(__file__).read_bytes()),
        'immutable_v1_transform_sha256': sha(Path(v1.__file__).read_bytes())}
    (a.out / 'source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    patch = ''.join(difflib.unified_diff(baseline[PATH].decode().splitlines(True), candidate[PATH].decode().splitlines(True), fromfile='a/' + PATH, tofile='b/' + PATH))
    (a.out / 'candidate.patch').write_text(patch)
    print(json.dumps({'candidate': str(a.out), 'hw_cache_sha256': sha(candidate[PATH]), 'changed': manifest['changed']}))


if __name__ == '__main__':
    main()
