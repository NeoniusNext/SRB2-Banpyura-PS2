#!/usr/bin/env python3
"""Build actual GS queued-owner pressure witnesses; never starts PCSX2."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
BASE = ROOT / 'tools/ps2/opt17/highres_tiles_native/build_native.py'


def main():
    spec = importlib.util.spec_from_file_location('sky_pressure_native_base', BASE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.HERE = HERE
    out = Path(sys.argv[sys.argv.index('--out') + 1]).resolve()
    module.B.GEN = out / 'metadata-gen'
    module.B.INCS.append('-I' + str(ROOT / 'tools/ps2'))
    original_run = module.run
    hooked = False
    hook_proof = {}
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()

    def run(command, log):
        nonlocal hooked
        if not hooked:
            path = out / 'source/src/ps2/hw/ps2_hw_draw.inc'
            original = path.read_text()
            start = original.index('static inline qw_t *put_vertex(')
            end = original.index('// The three-register indexed formats', start)
            body = original[start:end]
            if body.count('return o + 1;') != 1:
                raise RuntimeError('Packed vertex trace anchor changed')
            body = body.replace('{\n', '{\n\tconst qw_t *sp_trace_first = o;\n', 1)
            body = body.replace('return o + 1;', 'sp_trace_vertex(sp_trace_first, o + 1);\n\treturn o + 1;')
            hooked_text = original[:start] + 'static void sp_trace_vertex(const qw_t *, const qw_t *);\n' + body + original[end:]
            path.write_text(hooked_text)
            restored = hooked_text.replace('static void sp_trace_vertex(const qw_t *, const qw_t *);\n', '')
            restored = restored.replace('\tconst qw_t *sp_trace_first = o;\n', '')
            restored = restored.replace('sp_trace_vertex(sp_trace_first, o + 1);\n\treturn o + 1;', 'return o + 1;')
            if restored != original:
                raise RuntimeError('Packed observer literal restoration failed')
            hook_proof['packed_observer_restores_entire_draw_file'] = True
            # The positive callback only observes. Explicit runtime negative
            # mode injects a deliberately wrong upload between the real wide
            # IMAGE and reader, proving that its disjoint pixels are observed.
            path = out / 'source/src/ps2/hw/ps2_hw_tile_draw.inc'
            original = path.read_text()
            marker = 'static int tile_draw_window_layout('
            start = original.index(marker)
            end = original.index('static int tile_draw_window(', start)
            body = original[start:end]
            anchor = 'emit_piece(tri, 3, &part);'
            if body.count(anchor) != 1:
                raise RuntimeError('Wide reader observer anchor changed')
            body = body.replace(anchor, 'sp_before_window_reader(wide);\n\t' + anchor)
            hooked_text = original[:start] + 'static void sp_before_window_reader(int);\n' + body + original[end:]
            path.write_text(hooked_text)
            restored = hooked_text.replace('static void sp_before_window_reader(int);\n', '')
            restored = restored.replace('sp_before_window_reader(wide);\n\t' + anchor, anchor)
            if restored != original:
                raise RuntimeError('Wide reader observer literal restoration failed')
            hook_proof['window_observer_restores_entire_tile_draw_file'] = True
            hooked = True
        original_run(command, log)

    module.run = run
    module.main()
    path = out / 'build-report.json'
    report = json.loads(path.read_text())
    report['sky_pressure_fixture'] = {
        'scope': 'Actual ordinary448 victim draw, SKY-only eviction/borrowed32KiB draw, return, actual ordinary448 upload overwriting the prefix, then first GS readback. Resident control has identical geometry and source pixels.',
        'positive_hooks': 'Only observes actual packed words and the actual window-reader boundary; no production packing, source, geometry, allocator or GPU arithmetic changes.',
        'negative_modes': ['victim-order-negative', 'wide-order-negative'],
        'literal_restoration': hook_proof,
        'fixture_sha256': {str(p): sha(p) for p in [Path(__file__), HERE / 'driver.inc', HERE / 'main.inc', BASE]},
        'emulator_used': False,
    }
    path.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
