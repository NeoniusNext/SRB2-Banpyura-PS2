"""Freeze the seven-drawer diff; invert only those bodies and metadata macros."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from next7_render_fog_hosttest import ROOT, NAMES, function


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--base-src', required=True, type=Path)
    a = p.parse_args()
    out = a.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    base = a.base_src.resolve()
    original = (base / 'r_draw8.c').read_text()
    current = (ROOT / 'src/r_draw8.c').read_text()
    restored = current
    begin = restored.index('#ifdef PS2_OPT_DRAW\n#define SOLID_TILT_LIGHT')
    end = restored.index('void R_DrawTiltedFogSpan_8(void)', begin)
    restored = restored[:begin] + restored[end:]
    restored = restored.replace('#undef SOLID_TILT_LIGHT\n#undef SOLID_TRANSMAP\n\n', '', 1)
    for name in NAMES:
        old, new = function(original, name), function(restored, name)
        if old == new:
            raise SystemExit('Intended function not changed: ' + name)
        restored = restored.replace(new, old, 1)
    if restored != original:
        raise SystemExit('Restoring only seven bodies/macros did not reconstruct baseline')
    sibling = {name: {'baseline': sha(base / name), 'current': sha(ROOT / 'src' / name)}
               for name in ('r_draw.c', 'r_draw8_npo2.c')}
    if any(d['baseline'] != d['current'] for d in sibling.values()):
        raise SystemExit('Sibling production file changed')
    source_dir = out / 'src'; source_dir.mkdir(exist_ok=True)
    (source_dir / 'r_draw8.c').write_text(current)
    diff = subprocess.run(['diff', '-u', str(base / 'r_draw8.c'), str(ROOT / 'src/r_draw8.c')],
                          capture_output=True, text=True)
    if diff.returncode != 1:
        raise SystemExit('Expected a nonempty isolated diff')
    (out / 'change.diff').write_text(diff.stdout)
    report = {'reference': str(base), 'source_sha256': sha(ROOT / 'src/r_draw8.c'),
              'baseline_source_sha256': sha(base / 'r_draw8.c'),
              'inverse_seven_function_bodies_and_macros_reconstructs_complete_reference': True,
              'only_changed_functions': NAMES, 'unchanged_sibling_files': sibling,
              'metadata_lifetime_audit': {
                  'framebuffers': 'src/ps2/i_video.c Impl_VideoSetupBuffer uses memalign; Impl_SliceScreens and src/v_video.c V_Init slice vid.buffer',
                  'global_variable_storage': 'ds_x1/ds_colormap/ds_transmap/dc_colormap in r_draw.c, planezlight in r_plane.c, colormaps in r_data.c; separate from allocated buffers',
                  'helper_boundary': 'All tilted hoists occur after original R_CalcSlopeLight, with original ds_x1; final cursor published before return',
                  'callbacks_inside_hoisted_regions': False,
                  'live_reads': ['planezlight entries', 'tiltlighting entries', 'colormap contents', 'blend contents', 'background contents'],
                  'persistent_pointer_cache_or_restrict': False,
                  'allocation_or_capacity_changes': False,
                  'arithmetic_or_precision_changes': False}}
    (out / 'change-audit.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
