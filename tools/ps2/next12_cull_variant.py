#!/usr/bin/env python3
"""Prepare the isolated conservative late-wall prototype; never edit ROOT src."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / "build/nov15-next12/baseline-src"
BASE_MANIFEST = ROOT / "tools/ps2/next12_baseline.json"
OUT = ROOT / "build/nov15-next12/cull-src"
MAIN = "src/hardware/hw_main.c"
CACHE = "src/hardware/hw_cache.c"

HELPER = r'''#ifdef PS2_PROFILE
// A late wall can avoid texture selection only when that selection cannot change
// the engine's material classification (hw_cache.c). Unknown metadata falls back.
extern boolean HWR_PS2_CullTextureStable(INT32 tex);

// Keep the transparent node in BOTH sorts: even an invisible wall separates two
// runs of planes. Only its final draw can be omitted. No distance/far-plane cut.
static unsigned int HWR_FrWallCullPlane(const FOutVector *v, FBITFIELD blend)
{
	unsigned int k, i;

	if (!FR_GATE() || HWR_IsWireframeMode()
		|| (blend & (PF_NoTexture | PF_NoDepthTest | PF_WireFrame | PF_Corona | PF_Decal | PF_Ripple)))
		return 0;
	// These are map coordinates, not model vertices. This range also rejects all
	// infinities/NaNs and bounds floating point roundoff well below FR_MARGIN.
	for (i = 0; i < 4; i++)
		if (!(fabsf(v[i].x) < 32768.0f && fabsf(v[i].y) < 32768.0f && fabsf(v[i].z) < 32768.0f))
			return 0;
	for (k = 0; k < 5; k++)
	{
		const fr_plane_t *p = &frv.p[k];

		if (!isfinite(p->a) || !isfinite(p->b) || !isfinite(p->c) || !isfinite(p->d))
			return 0;
	}
	for (k = 0; k < 5; k++)
	{
		const fr_plane_t *p = &frv.p[k];

		for (i = 0; i < 4; i++)
		{
			const float d = p->a * v[i].x + p->b * v[i].y + p->c * v[i].z + p->d;

			if (!(d > FR_MARGIN) || !isfinite(d))
				goto next_plane;
		}
		return k + 1;
next_plane:;
	}
	return 0;
}
#endif

'''

QUERY = r'''// No allocation or texture selection here. HWR_GetTexture is not just a bind:
// it can reset TF_TRANSPARENT, create a chroma variant, and classify new texels.
// Limit elision to already initialized P_8/no-key metadata whose writes would be
// idempotent. Palette colours cannot change whether an index equals 255; palette
// mode/source/add-on rebuilds replace these mipmaps and lose this known state.
boolean HWR_PS2_CullTextureStable(INT32 tex)
{
	const GLMapTexture_t *g;
	const GLMipmap_t *m, *v;
	INT32 w, h;

	if (!gl_maptexturesloaded || !gl_textures || !textures || numtextures <= 0
		|| gl_numtextures != (size_t)numtextures || tex < 0 || tex >= numtextures
		|| !textures[tex] || textureformat != GL_TEXFMT_P_8 || textures[tex]->transparency)
		return false;
	w = textures[tex]->width;
	h = textures[tex]->height;
	if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || (UINT32)w * (UINT32)h >= 65536u)
		return false; // the existing immediate planner handles larger images
	g = &gl_textures[tex];
	m = &g->mipmap;
	v = m->nextcolormap;
	if (m->format != GL_TEXFMT_P_8 || m->flags != TF_WRAPXY
		|| m->width != w || m->height != h || m->regen_kind != 1 || m->regen_id != tex
		|| m->ps2_twin || m->colormap || m->ps2_h255 != 1
		|| g->scaleX != 1.0f / (w * FRACUNIT) || g->scaleY != 1.0f / (h * FRACUNIT)
		|| !v || v->format != GL_TEXFMT_P_8 || v->flags != (TF_WRAPXY | TF_CHROMAKEYED)
		|| v->width != w || v->height != h || v->regen_kind != 1 || v->regen_id != tex
		|| v->ps2_twin != m || v->colormap || v->nextcolormap || v->ps2_h255 != 1)
		return false;
	return true;
}

'''

BEFORE = '''\t\t\tif (!(sortnode[sortindex[i]].wall->blend & PF_NoTexture))
\t\t\t\tHWR_GetTexture(sortnode[sortindex[i]].wall->texnum, true);
\t\t\tHWR_RenderWall(sortnode[sortindex[i]].wall->wallVerts, &sortnode[sortindex[i]].wall->Surf, sortnode[sortindex[i]].wall->blend, sortnode[sortindex[i]].wall->fogwall,
\t\t\t\tsortnode[sortindex[i]].wall->lightlevel, sortnode[sortindex[i]].wall->wallcolormap);'''

AFTER = '''#ifdef PS2_PROFILE
\t\t\twallinfo_t *const w = sortnode[sortindex[i]].wall;
\t\t\tconst unsigned int cullplane = HWR_FrWallCullPlane(w->wallVerts, w->blend);
\t\t\tconst boolean skipwall = cullplane && HWR_PS2_CullTextureStable(w->texnum);

\t\t\tif (skipwall)
\t\t\t{
\t\t\t\t// Keep lighting/material preparation (including light-table IDs) in the
\t\t\t\t// same order as RenderWall; only texture selection and the empty draw go.
\t\t\t\tif (r_renderwalls)
\t\t\t\t{
\t\t\t\t\tconst UINT8 alpha = w->Surf.PolyColor.s.alpha;

\t\t\t\t\tHWR_Lighting(&w->Surf, w->lightlevel, w->wallcolormap);
\t\t\t\t\tw->Surf.PolyColor.s.alpha = alpha;
\t\t\t\t}
\t\t\t}
\t\t\telse
#endif
\t\t\t{
\t\t\t\tif (!(sortnode[sortindex[i]].wall->blend & PF_NoTexture))
\t\t\t\t\tHWR_GetTexture(sortnode[sortindex[i]].wall->texnum, true);
\t\t\t\tHWR_RenderWall(sortnode[sortindex[i]].wall->wallVerts, &sortnode[sortindex[i]].wall->Surf, sortnode[sortindex[i]].wall->blend, sortnode[sortindex[i]].wall->fogwall,
\t\t\t\t\tsortnode[sortindex[i]].wall->lightlevel, sortnode[sortindex[i]].wall->wallcolormap);
\t\t\t}'''


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rewrite(path, source):
    if path == MAIN:
        anchor = "//\n// HWR_CreateDrawNodes\n"
        if source.count(anchor) != 1 or source.count(BEFORE) != 1:
            raise RuntimeError("Exact late-wall anchors changed")
        return source.replace(anchor, HELPER + anchor, 1).replace(BEFORE, AFTER, 1)
    if path == CACHE:
        anchor = "UINT32 HWR_PS2_TexTransparent(INT32 tex)\n"
        if source.count(anchor) != 1:
            raise RuntimeError("Exact texture metadata anchor changed")
        return source.replace(anchor, QUERY + anchor, 1)
    raise RuntimeError("Unowned production file: " + path)


def apply(snapshot):
    snapshot = snapshot.resolve()
    if snapshot != OUT.resolve():
        raise RuntimeError("Only the authorized isolated cull-src snapshot may be edited")
    baseline = json.loads(BASE_MANIFEST.read_text())
    if not snapshot.exists():
        shutil.copytree(BASE, snapshot)
    before = {name: sha(snapshot / name) for name in baseline["source_sha256"]}
    if before != baseline["source_sha256"]:
        raise RuntimeError("Candidate source was not the exact immutable 602-file baseline")
    patches = []
    for path in (MAIN, CACHE):
        old = (BASE / path).read_text()
        new = rewrite(path, old)
        (snapshot / path).write_text(new)
        patches += difflib.unified_diff(old.splitlines(True), new.splitlines(True),
                                        fromfile="a/" + path, tofile="b/" + path)
    current = {name: sha(snapshot / name) for name in before}
    changed = sorted(name for name in before if current[name] != before[name])
    if changed != sorted((MAIN, CACHE)):
        raise RuntimeError("Unexpected source overlay")
    work = ROOT / "build/nov15-next12/cull"
    work.mkdir(parents=True, exist_ok=True)
    (work / "candidate.patch").write_text("".join(patches))
    (work / "source-freeze.json").write_text(json.dumps({
        "baseline_revision": baseline["revision"], "baseline_manifest": str(BASE_MANIFEST),
        "source_root": str(snapshot), "source_file_count": len(current),
        "only_baseline_differences": changed, "source_sha256": current,
        "tool_sha256": {str(p.relative_to(ROOT)): sha(p) for p in sorted((snapshot / "tools/ps2").iterdir()) if p.is_file()},
        "prototype_not_accepted": True, "target_build_or_runtime_performed": False,
    }, indent=2) + "\n")
    print(json.dumps({"snapshot": str(snapshot), "source_files": len(current), "only_changed": changed}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshot", type=Path, default=OUT)
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    if not args.apply:
        parser.error("Use --apply to prepare only the isolated prototype")
    apply(args.snapshot)
