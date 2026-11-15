#!/usr/bin/env python3
"""Flat v4: share the complete existing map-mipmap cleanup without fake casts."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path

import next13_flats_variant as v1
import next13_flats_cut_variant as v2

ROOT, BASE, PATH, GUARD, REVISION = v2.ROOT, v2.BASE, v2.PATH, v2.GUARD, v2.REVISION
OUT = ROOT / 'build/nov15-next13/flats-cleanup-source-v4'

ORIGINAL_MAP = '''static void FreeMapTexture(GLMapTexture_t *tex, boolean delete_chromakeys)
{
	if (tex->mipmap.nextcolormap)
	{
		DeleteTextureMipmap(tex->mipmap.nextcolormap, delete_chromakeys);
		free(tex->mipmap.nextcolormap);
		tex->mipmap.nextcolormap = NULL;
	}

	DeleteTextureMipmap(&tex->mipmap, true);
}
'''

SHARED = '''#if defined(PS2) && defined(PS2_PROFILE) && !defined(PS2_NOOPT_FLAT_METADATA)
__attribute__((noinline, noclone))
static void FreeMapMipmap(GLMipmap_t *m, boolean deleteChildData)
{
	if (m->nextcolormap)
	{
		DeleteTextureMipmap(m->nextcolormap, deleteChildData);
		free(m->nextcolormap);
		m->nextcolormap = NULL;
	}

	DeleteTextureMipmap(m, true);
}
#endif

'''


def replace(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError('V4 cleanup anchor changed: ' + old[:80])
    return text.replace(old, new, 1)


def rewrite(baseline):
    text = v2.rewrite(baseline)
    text = replace(text, ORIGINAL_MAP, '#if !(' + GUARD + ')\n' + ORIGINAL_MAP + '#endif\n')
    text = replace(text, v1.FREE, SHARED)
    text = replace(text,
        '\t\tFreeMapTexture(&gl_textures[i], true);\n#if ' + GUARD + '\n'
        '\t\tFreeFlatMipmap(HWR_FlatMipmap(i));\n#else\n'
        '\t\tFreeMapTexture(&gl_flats[i], false);\n#endif',
        '#if ' + GUARD + '\n'
        '\t\tFreeMapMipmap(&gl_textures[i].mipmap, true);\n'
        '\t\tFreeMapMipmap(HWR_FlatMipmap(i), false);\n#else\n'
        '\t\tFreeMapTexture(&gl_textures[i], true);\n'
        '\t\tFreeMapTexture(&gl_flats[i], false);\n#endif')
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
        'candidate': str(a.out), 'variant': 'flat-cleanup-v4',
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
