#!/usr/bin/env python3
"""Prepare flat v3: share the exact existing cold mipmap-delete body."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path

import next13_flats_variant as v1
import next13_flats_cut_variant as v2

ROOT, BASE, PATH, GUARD, REVISION = v2.ROOT, v2.BASE, v2.PATH, v2.GUARD, v2.REVISION
OUT = ROOT / 'build/nov15-next13/flats-share-source-v3'


def rewrite(baseline):
    text = v2.rewrite(baseline)
    anchor = 'static void DeleteTextureMipmap(GLMipmap_t *grMipmap, boolean delete_mipmap)\n'
    if text.count(anchor) != 1:
        raise RuntimeError('Exact existing delete helper declaration changed')
    return text.replace(anchor, '#if ' + GUARD + '\n__attribute__((noinline, noclone))\n#endif\n' + anchor, 1)


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
        'candidate': str(a.out), 'variant': 'flat-share-v3',
        'source_sha256': {p: sha(v) for p, v in baseline.items()},
        'candidate_source_sha256': {p: sha(v) for p, v in candidate.items()},
        'changed': [p for p in baseline if baseline[p] != candidate[p]],
        'tool_sha256': sha(Path(__file__).read_bytes()),
        'immutable_v1_transform_sha256': sha(Path(v1.__file__).read_bytes()),
        'immutable_v2_transform_sha256': sha(Path(v2.__file__).read_bytes())}
    (a.out / 'source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    patch = ''.join(difflib.unified_diff(baseline[PATH].decode().splitlines(True), candidate[PATH].decode().splitlines(True), fromfile='a/' + PATH, tofile='b/' + PATH))
    (a.out / 'candidate.patch').write_text(patch)
    print(json.dumps({'candidate': str(a.out), 'hw_cache_sha256': sha(candidate[PATH]), 'changed': manifest['changed']}))


if __name__ == '__main__':
    main()
