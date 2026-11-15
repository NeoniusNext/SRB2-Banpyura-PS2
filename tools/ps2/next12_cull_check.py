#!/usr/bin/env python3
"""Pin the rejected isolated cull experiment and execute source-drift negatives."""
import argparse
import hashlib
import json
from pathlib import Path

from next12_cull_hosttest import function
from next12_cull_variant import BASE, OUT, ROOT, MAIN, CACHE, BEFORE, rewrite


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_map(root):
    return {str(p.relative_to(root)): sha(p) for p in sorted((root / "src").rglob("*")) if p.is_file()}


def check_texts(main, cache):
    if main != rewrite(MAIN, (BASE / MAIN).read_text()):
        raise RuntimeError("Isolated main source is not the exact approved prototype")
    if cache != rewrite(CACHE, (BASE / CACHE).read_text()):
        raise RuntimeError("Isolated cache source is not the exact approved query")
    create = function(main, "HWR_CreateDrawNodes")
    old = function((BASE / MAIN).read_text(), "HWR_CreateDrawNodes")
    prefix = old[:old.index(BEFORE)]
    if not create.startswith(prefix):
        raise RuntimeError("Collection, both sorting passes, transform or wall branch prefix changed")
    if create.index("HWR_FrWallCullPlane(w->wallVerts") <= create.rindex("qsort("):
        raise RuntimeError("Guard moved before original sort barriers")
    if create.index("HWR_FrWallCullPlane(w->wallVerts") <= create.index("HWD.pfnSetTransform(&atransform)"):
        raise RuntimeError("Guard moved before the exact view transform")
    if create.count("HWR_PS2_CullTextureStable(w->texnum)") != 1:
        raise RuntimeError("Unknown classification fallback changed")
    if create.count("HWR_Lighting(&w->Surf, w->lightlevel, w->wallcolormap);") != 1:
        raise RuntimeError("Skipped-wall lighting/ID order changed")
    if main.count("\t\tHWR_CreateDrawNodes();") != 2 or main.count("HWR_FrViewSetup(); // OPT12 PS2-HW-400") != 2:
        raise RuntimeError("Sky/main view generation caller set changed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify-root", action="store_true", help="Also require ROOT src to remain baseline-identical; use before merging independent pass12 changes")
    args = parser.parse_args()
    freeze = json.loads((ROOT / "build/nov15-next12/cull/source-freeze.json").read_text())
    baseline = json.loads((ROOT / "tools/ps2/next12_baseline.json").read_text())
    original = source_map(BASE)
    if original != baseline["source_sha256"]:
        raise RuntimeError("Immutable source is not the complete published next12 baseline")
    if source_map(OUT) != freeze["source_sha256"]:
        raise RuntimeError("Frozen rejected candidate changed")
    if args.verify_root and source_map(ROOT) != original:
        raise RuntimeError("ROOT production source changed during this rejection check")
    main_text, cache_text = (OUT / MAIN).read_text(), (OUT / CACHE).read_text()
    check_texts(main_text, cache_text)
    differences = sorted(name for name in original if original[name] != freeze["source_sha256"][name])
    if differences != sorted((MAIN, CACHE)):
        raise RuntimeError("Unowned source overlay")
    negatives = []
    for name, m, c in (
        ("guard-source-drift", main_text.replace("d > FR_MARGIN", "d > 0.0f", 1), cache_text),
        ("query-unknown-h255", main_text, cache_text.replace("v->ps2_h255 != 1", "v->ps2_h255 == 2", 1)),
        ("skip-lighting", main_text.replace("HWR_Lighting(&w->Surf, w->lightlevel, w->wallcolormap);", "((void)0);", 1), cache_text),
    ):
        try:
            check_texts(m, c)
        except RuntimeError as error:
            negatives.append({"name": name, "detected": True, "reason": str(error)})
        else:
            raise RuntimeError("Source negative was not detected: " + name)
    report = {
        "status": "rejected_candidate_source_pinned", "baseline_revision": baseline["revision"],
        "published_source_revision": baseline["published_source_revision"],
        "complete_source_count": len(original), "only_isolated_differences": differences,
        "root_production_source_unchanged": True if args.verify_root else None,
        "root_identity_check_requested": args.verify_root,
        "no_ee_compiler_or_emulator": True,
        "source_negatives": negatives,
        "source_files": {name: {"baseline": original[name], "rejected_candidate": freeze["source_sha256"][name]} for name in differences},
        "tool_sha256": sha(Path(__file__)),
        "limit": "This pins source scope and placement. The resource optimization is rejected because actual driver feedback affects future budgets; this is not an EE, raster, performance or resource-saving acceptance."
    }
    path = ROOT / "build/nov15-next12/cull/source-check-report.json"
    path.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
