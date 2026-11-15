#!/usr/bin/env python3
"""Build the final-source PB opt-out in the complete final Hardware LTO graph.

Consumes one explicitly granted EE worker, serially. Only hw_batching IR is
recompiled with PS2_NOOPT_PB_SCRATCH; the other204 actual inputs are copied
without changes. All final light-table sources remain active and exact.
"""
import argparse
import json
from pathlib import Path
import shutil

from next10_sw_bounds_context import archive_generated
from next12_pb_scratch_context import run, compare_lto_diagnostics
from next12_pb_scratch_finalbind import ROOT, WORK, REVISION, FINAL_ELF, sha, exact_source, exact_build


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=WORK / 'final/hardware')
    parser.add_argument('--source', type=Path, default=WORK / 'final/source')
    parser.add_argument('--out', type=Path, default=WORK / 'final/pb-noopt-hardware')
    args = parser.parse_args()
    build, source, out = args.build.resolve(), args.source.resolve(), args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build') or out == ROOT / 'build':
        raise SystemExit('A fresh isolated child of build/ is required')
    source_pins = exact_source(source)
    admission = exact_build(build, source, FINAL_ELF)
    original = json.loads((build / 'build-report.json').read_text())
    out.mkdir(parents=True)
    (out / 'obj').mkdir()
    overlay = out / 'snapshot'
    shutil.copytree(source / 'src', overlay / 'src')
    if exact_source(overlay) != source_pins:
        raise SystemExit('Copied complete final-source snapshot differs')
    inputs = {name: digest for name, digest in original['input_sha256'].items()
              if not Path(name).is_relative_to(ROOT / 'src')}
    generated = archive_generated(out, inputs, [build / 'gen/config.h'])
    generated_paths = {entry['original_path']: entry['archive_path'] for entry in generated['details']['entries']}
    generated_dirs = {str(Path(old).parent): str(Path(new).parent) for old, new in generated_paths.items()}
    flags = [flag for flag in original['flags'] if flag not in ('-MMD', '-MP')]
    prefix = '-I' + str(ROOT / 'src')
    flags = ['-I' + str(overlay / 'src') + flag[len(prefix):] if flag.startswith(prefix) else flag for flag in flags]
    flags = ['-I' + generated_dirs.get(flag[2:], flag[2:]) if flag.startswith('-I') else flag for flag in flags]
    flags += ['-DPS2_NOOPT_PB_SCRATCH']
    compiler = original['link_command'][0]
    objects, canonical_IR = [], {}
    for name, digest in admission['complete_actual_IR_objects'].items():
        old = Path(name)
        new = out / 'obj' / old.name
        shutil.copyfile(old, new)
        if sha(new) != digest:
            raise SystemExit('Copied actual IR differs')
        canonical_IR[str(old)] = digest
        objects.append(new)
    replacement = out / 'obj/src__hardware__hw_batching.o'
    command = [compiler, *flags, '-MMD', '-MF', out / 'hw_batching.d', '-c', overlay / 'src/hardware/hw_batching.c', '-o', replacement]
    stdout, stderr, compile_seconds = run(command, out / 'compile.log', 120)
    if stdout or stderr:
        raise SystemExit('Final PB opt-out compile emitted diagnostics')
    engine = out / 'obj/engine_lto.o'
    lto_command = [compiler, '-r', '-nostdlib', '-flto=1', '-flinker-output=nolto-rel', '-O2',
        '-ffunction-sections', '-fdata-sections', *[f for f in flags if f.startswith('-G') or f == '-mno-abicalls'], *objects, '-o', engine]
    stdout, stderr, lto_seconds = run(lto_command, out / 'lto.log', 600)
    diagnostics = compare_lto_diagnostics(build / 'lto.log', stdout, stderr, canonical_IR, objects, original, out)
    if not diagnostics['all_normalized_diagnostics_identical']:
        raise SystemExit('Final PB opt-out complete LTO diagnostics differ; inspect unchanged raw streams')
    command = list(original['link_command'])
    positions = [i for i, arg in enumerate(command) if arg.endswith('/engine_lto.o')]
    if len(positions) != 1:
        raise SystemExit('One exact final engine link input required')
    command[positions[0]] = str(engine)
    command[command.index('-o') + 1] = str(out / 'SRB2.ELF')
    positions = [i for i, arg in enumerate(command) if arg.startswith('-Wl,-Map=')]
    if len(positions) != 1:
        raise SystemExit('One exact final map output required')
    command[positions[0]] = '-Wl,-Map=' + str(out / 'SRB2.map')
    stdout, stderr, link_seconds = run(command, out / 'link.log', 120)
    if (build / 'link.log').read_text() or stdout or stderr:
        raise SystemExit('Final PB opt-out link diagnostics differ')
    if exact_source(source) != source_pins or exact_source(overlay) != source_pins or exact_build(build, source, FINAL_ELF) != admission:
        raise SystemExit('Exact canonical final source/inputs changed during control build')
    actual_IR = {str(path): sha(path) for path in objects}
    changed = [old.name for old, new in zip(map(Path, canonical_IR), objects) if canonical_IR[str(old)] != sha(new)]
    if changed != ['src__hardware__hw_batching.o']:
        raise SystemExit('Final PB opt-out changed a different complete IR input')
    actual_inputs = {}
    for name, digest in original['input_sha256'].items():
        path = Path(name)
        actual = str(overlay / path.relative_to(ROOT)) if path.is_relative_to(ROOT / 'src') else generated_paths.get(name, name)
        actual_inputs[actual] = sha(Path(actual))
        if actual_inputs[actual] != digest:
            raise SystemExit('Actual final opt-out source/generated/external dependency differs')
    context = {'source_revision': REVISION, 'canonical_build': str(build), 'canonical_final_admission': admission,
        'complete_final_source_sha256': source_pins, 'complete_actual_IR_objects': actual_IR,
        'canonical_IR_objects': canonical_IR, 'changed_actual_IR_objects': changed,
        'only_recompiled_unit': 'src/hardware/hw_batching.c', 'all_other_complete_IR_inputs_identical': True,
        'narrow_optout': 'PS2_NOOPT_PB_SCRATCH', 'light_table_final_source_unchanged': True,
        'generated_input_archive': generated, 'compile_command': list(map(str, [compiler, *flags, '-MMD', '-MF', out / 'hw_batching.d', '-c', overlay / 'src/hardware/hw_batching.c', '-o', replacement])),
        'lto_command': list(map(str, lto_command)), 'link_command': command,
        'compile_seconds': compile_seconds, 'lto_seconds': lto_seconds, 'link_seconds': link_seconds,
        'lto_diagnostics': diagnostics, 'engine_sha256': sha(engine), 'ELF_sha256': sha(out / 'SRB2.ELF'),
        'checker_sha256': sha(Path(__file__))}
    report = dict(original)
    report.update(flags=flags, compiled=1, link_command=command, input_sha256=actual_inputs,
        elf_sha256=context['ELF_sha256'], elf_bytes=(out / 'SRB2.ELF').stat().st_size,
        pb_scratch_context_control=context)
    (out / 'context-report.json').write_text(json.dumps(context, indent=2) + '\n')
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Built exact final Hardware narrow PB opt-out: ' + str(out), flush=True)


if __name__ == '__main__':
    main()
