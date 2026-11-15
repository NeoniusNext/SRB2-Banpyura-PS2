#!/usr/bin/env python3
"""Prepare pass13 private flat metadata layouts without editing production."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
REVISION = "353cc94103aeb38cef87d26151c779302135d0e9"
BASE = ROOT / "build/nov15-next13/flats-baseline-v1"
OUT = ROOT / "build/nov15-next13/flats-source-v1"
PATH = "src/hardware/hw_cache.c"
GUARD = "defined(PS2) && defined(PS2_PROFILE) && !defined(PS2_NOOPT_FLAT_METADATA)"

LAYOUT = r'''#if defined(PS2) && defined(PS2_PROFILE) && !defined(PS2_NOOPT_FLAT_METADATA)
// Flats only use their mipmap. Keep the full table allocated up front, but omit
// wall-only scales on the default PB path. The legacy collector sorts by raw
// texture pointers, so -hwgo 65536 must retain its original allocation/stride.
static void *gl_flats;
static boolean gl_flats_compact;

static boolean HWR_UseCompactFlatMetadata(void)
{
	INT32 i;

	// GCacheParse runs after the table is allocated. Mirror its first -hwgo
	// lookup without changing M_CheckParm's shared "found" cursor or parse time.
	for (i = 1; i < myargc; i++)
	{
		if (!strcasecmp("-hwgo", myargv[i]))
		{
			if (i + 1 < myargc && myargv[i + 1][0] != '-' && myargv[i + 1][0] != '+')
				return !((UINT32)atoi(myargv[i + 1]) & 0x10000u);
			break;
		}
	}
	return true;
}

static inline GLMipmap_t *HWR_FlatMipmap(size_t n)
{
	if (gl_flats_compact)
		return &((GLMipmap_t *)gl_flats)[n];
	return &((GLMapTexture_t *)gl_flats)[n].mipmap;
}
#else
static GLMapTexture_t *gl_flats; // For all (texture) flats, as normal flats don't need to be cached
#endif
'''

FREE = r'''#if defined(PS2) && defined(PS2_PROFILE) && !defined(PS2_NOOPT_FLAT_METADATA)
static void FreeFlatMipmap(GLMipmap_t *mipmap)
{
	if (mipmap->nextcolormap)
	{
		DeleteTextureMipmap(mipmap->nextcolormap, false);
		free(mipmap->nextcolormap);
		mipmap->nextcolormap = NULL;
	}

	DeleteTextureMipmap(mipmap, true);
}
#endif

'''


def replace(text, old, new, count=1):
    if text.count(old) != count:
        raise RuntimeError("Changed flat candidate anchor: " + old[:100])
    return text.replace(old, new)


def rewrite(text):
    text = replace(text,
        "static GLMapTexture_t *gl_flats; // For all (texture) flats, as normal flats don't need to be cached\n", LAYOUT)
    text = replace(text, "void HWR_FreeMapTextures(void)\n", FREE + "void HWR_FreeMapTextures(void)\n")
    text = replace(text, "\t\tFreeMapTexture(&gl_flats[i], false);",
        "#if " + GUARD + "\n\t\tFreeFlatMipmap(HWR_FlatMipmap(i));\n#else\n"
        "\t\tFreeMapTexture(&gl_flats[i], false);\n#endif")
    text = replace(text, "\tgl_flats = NULL;\n", "\tgl_flats = NULL;\n#if " + GUARD
        + "\n\tgl_flats_compact = false;\n#endif\n", 3)
    # The fault-injection free has extra indentation and is not a layout change.
    # A failed table allocation drops the constructor layout along with storage.
    text = replace(text, "\t\tgl_textures = gl_flats = NULL;\n", "\t\tgl_textures = gl_flats = NULL;\n#if "
        + GUARD + "\n\t\tgl_flats_compact = false;\n#endif\n")
    text = replace(text,
        "\tgl_textures = calloc(gl_numtextures, sizeof(*gl_textures));\n\tgl_flats = calloc(gl_numtextures, sizeof(*gl_flats));",
        "#if " + GUARD + "\n\tgl_flats_compact = HWR_UseCompactFlatMetadata();\n#endif\n"
        "\tgl_textures = calloc(gl_numtextures, sizeof(*gl_textures));\n#if " + GUARD + "\n"
        "\tgl_flats = calloc(gl_numtextures, gl_flats_compact ? sizeof(GLMipmap_t) : sizeof(GLMapTexture_t));\n#else\n"
        "\tgl_flats = calloc(gl_numtextures, sizeof(*gl_flats));\n#endif")
    text = replace(text, "\tINT32 texturenum;\n\tGLMapTexture_t *grtex;\n\tGLMipmap_t *grMipmap, *originalMipmap;",
        "\tINT32 texturenum;\n#if !(" + GUARD + ")\n\tGLMapTexture_t *grtex;\n#endif\n"
        "\tGLMipmap_t *grMipmap, *originalMipmap;")
    text = replace(text, "\tgrtex = &gl_flats[texturenum];\n\tgrMipmap = originalMipmap = &grtex->mipmap;",
        "#if " + GUARD + "\n\tgrMipmap = originalMipmap = HWR_FlatMipmap((size_t)texturenum);\n#else\n"
        "\tgrtex = &gl_flats[texturenum];\n\tgrMipmap = originalMipmap = &grtex->mipmap;\n#endif")
    return text


def sha(data):
    return hashlib.sha256(data).hexdigest()


def sources():
    files = subprocess.check_output(["git", "ls-tree", "-r", "--name-only", REVISION, "src"], cwd=ROOT, text=True).splitlines()
    if len(files) != 602:
        raise RuntimeError("Expected the 602-file pass13 canonical source")
    return {p: subprocess.check_output(["git", "show", REVISION + ":" + p], cwd=ROOT) for p in files}


def prepare(dest, data):
    if dest.resolve() == ROOT:
        raise RuntimeError("Production edits are forbidden")
    expected = {p: sha(v) for p, v in data.items()}
    if dest.exists():
        actual = {p.relative_to(dest).as_posix(): sha(p.read_bytes()) for p in (dest / "src").rglob("*") if p.is_file()}
        if actual != expected:
            raise RuntimeError("Refusing mismatched frozen source " + str(dest))
        return
    for p, v in data.items():
        out = dest / p
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(v)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, default=BASE)
    parser.add_argument("--out", type=Path, default=OUT)
    args = parser.parse_args()
    baseline = sources()
    candidate = dict(baseline)
    candidate[PATH] = rewrite(baseline[PATH].decode()).encode()
    prepare(args.base, baseline)
    prepare(args.out, candidate)
    manifest = {"revision": REVISION, "source_count": len(baseline), "baseline": str(args.base), "candidate": str(args.out),
        "source_sha256": {p: sha(v) for p, v in baseline.items()},
        "candidate_source_sha256": {p: sha(v) for p, v in candidate.items()},
        "changed": [p for p in baseline if baseline[p] != candidate[p]], "tool_sha256": sha(Path(__file__).read_bytes())}
    (args.out / "source-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    patch = "".join(difflib.unified_diff(baseline[PATH].decode().splitlines(True), candidate[PATH].decode().splitlines(True), fromfile="a/" + PATH, tofile="b/" + PATH))
    (args.out / "candidate.patch").write_text(patch)
    print(json.dumps({"changed": manifest["changed"], "source_count": len(baseline), "candidate": str(args.out), "hw_cache_sha256": sha(candidate[PATH])}))


if __name__ == "__main__":
    main()
