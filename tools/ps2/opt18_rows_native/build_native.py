#!/usr/bin/env python3
"""Build actual EE/GS 16KiB analytic-row comparisons; never starts PCSX2."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
BASE = ROOT / 'tools/ps2/opt17/highres_tiles_native/build_native.py'


def main():
    spec = importlib.util.spec_from_file_location('wide_native_base', BASE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.HERE = HERE
    out = Path(sys.argv[sys.argv.index('--out') + 1]).resolve()
    module.B.GEN = out / 'metadata-gen'
    module.B.INCS.append('-I' + str(ROOT / 'tools/ps2'))
    original_run = module.run
    hooked = False
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()

    def run(command, log):
        nonlocal hooked
        if not hooked:
            path = out / 'source/src/ps2/hw/ps2_hw_draw.inc'
            text = path.read_text()
            start = text.index('static inline qw_t *put_vertex(')
            end = text.index('// The three-register indexed formats', start)
            body = text[start:end]
            if body.count('return o + 1;') != 1:
                raise RuntimeError('Packed vertex trace anchor changed')
            body = body.replace('{\n', '{\n\tconst qw_t *wf_trace_first = o;\n', 1)
            body = body.replace('return o + 1;', 'wf_trace_vertex(wf_trace_first, o + 1);\n\treturn o + 1;')
            path.write_text(text[:start] + 'static void wf_trace_vertex(const qw_t *, const qw_t *);\n' + body + text[end:])
            path = out / 'source/src/ps2/hw/ps2_hw_tile_draw.inc'
            text = path.read_text()
            marker = 'static void tile_emit_triangle('
            if text.count(marker) != 1:
                raise RuntimeError('Row dispatch function anchor changed')
            text = text.replace(marker, 'static u32 wf_rows_taken,wf_rows_declined,wf_rows_failed;\n' + marker)
            marker = 'const int rows = tile_emit_rows(tri, pc, &source, &plane, &box, batch, &batch_n);'
            if text.count(marker) != 1:
                raise RuntimeError('Row observation anchor changed')
            text = text.replace(marker, marker + '\n\tif(rows>0)wf_rows_taken++;else if(rows==0)wf_rows_declined++;else wf_rows_failed++;')
            path.write_text(text)
            hooked = True
        original_run(command, log)

    module.run = run
    module.main()
    report_path = out / 'build-report.json'
    report = json.loads(report_path.read_text())
    report['focused_rows_fixture'] = {
        'purpose': 'Full GS RGBA/Zlow24 and actual packed STQ/RGB/XY/Zlow24 sequence equality; resident versus forced proxy with original64-block scratch, including real PF_Translucent alpha128 and destination-alpha patterns.',
        'private_packed_vertex_trace': 'Only a callback after actual put_vertex writes; masks the private stencil depth high byte when comparing logical Z24. No source, geometry, sampling, allocator or packing arithmetic is changed.',
        'files': {str(p): sha(p) for p in [Path(__file__), HERE / 'driver.inc', HERE / 'main.inc', BASE]},
        'emulator_used': False,
    }
    report_path.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
