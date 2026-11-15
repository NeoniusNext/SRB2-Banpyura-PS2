#!/usr/bin/env python3
"""Read complete default-LTO PB contexts, metadata, RELAs, PDRs and callers.

No compiler or emulator is invoked. Opt-out must restore the complete canonical
allocated object and ELF. The candidate comparison is an exhaustive binding,
not a separate-TU benchmark or a claim of a CPU/FPS improvement.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from audio_storage_next7_finalbind import Object
import next12_pb_scratch_replay as replay


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def allocated(obj):
    return {name: {**obj.signature(name), 'ordered_relocations': obj.relocations.get(name, [])}
            for name, header in obj.sections.items() if header[2] & 2}


def pdr(obj):
    if '.pdr' not in obj.sections:
        raise RuntimeError('Complete MIPS procedure descriptors required')
    data = obj.content(obj.sections['.pdr'])
    records = {}
    for relocation in obj.relocations.get('.pdr', []):
        offset = relocation['offset']
        if offset % 32:
            raise RuntimeError('Unexpected procedure descriptor relocation')
        values = struct.unpack_from('<8I', data, offset)
        records[relocation['target']['section']] = {
            'raw_words': list(values), 'frame_bytes': values[5],
            'saved_integer_mask': values[1], 'integer_save_offset': values[2],
            'saved_float_mask': values[3], 'float_save_offset': values[4],
            'frame_register': values[6], 'return_register': values[7],
            'relocation': relocation,
        }
    return {'signature': obj.signature('.pdr'), 'ordered_relocations': obj.relocations.get('.pdr', []),
            'records': records}


def context(path):
    report_path = path / 'context-report.json'
    report = json.loads(report_path.read_text())
    for name, digest in report['complete_actual_IR_objects'].items():
        if sha(Path(name)) != digest:
            raise RuntimeError('An actual complete LTO input changed')
    if len(report['complete_actual_IR_objects']) != 205 or report['only_recompiled_unit'] != 'src/hardware/hw_batching.c':
        raise RuntimeError('One-unit complete205 Hardware context required')
    if report['project_source_differences'] != ['src/hardware/hw_pbatch.inc']:
        raise RuntimeError('Source scope changed')
    if report['candidate_source_sha256'] != replay.CANDIDATE_SHA:
        raise RuntimeError('Candidate source pin differs')
    actual = {p.relative_to(path / 'snapshot').as_posix(): sha(p)
              for p in sorted((path / 'snapshot/src').rglob('*')) if p.is_file()}
    if actual != report['complete_source_overlay_sha256']:
        raise RuntimeError('Actual source overlay changed after compilation')
    if sha(path / 'obj/engine_lto.o') != report['engine_sha256'] or sha(path / 'SRB2.ELF') != report['ELF_sha256']:
        raise RuntimeError('Context output pin differs')
    return {'path': str(report_path), 'sha256': sha(report_path), 'content': report}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, default=replay.ROOT / 'build/nov15-next12/baseline/hardware')
    parser.add_argument('--candidate', type=Path, default=replay.ROOT / 'build/nov15-next12/pb-scratch/hardware')
    parser.add_argument('--optout', type=Path, default=replay.ROOT / 'build/nov15-next12/pb-scratch/optout-hardware')
    parser.add_argument('--out', type=Path, default=replay.ROOT / 'build/nov15-next12/pb-scratch/objectcheck')
    args = parser.parse_args()
    out = args.out.resolve()
    if not out.is_relative_to(replay.ROOT / 'build') or out == replay.ROOT / 'build':
        parser.error('--out must be an isolated child of build/')
    out.mkdir(parents=True, exist_ok=True)
    if (out / 'report.json').exists():
        raise RuntimeError('Preserve completed evidence; choose a fresh --out')
    paths = {'baseline': args.baseline.resolve(), 'candidate': args.candidate.resolve(), 'optout': args.optout.resolve()}
    contexts = {name: context(paths[name]) for name in ('candidate', 'optout')}
    baseline = json.loads((paths['baseline'] / 'build-report.json').read_text())
    if not baseline['hardware'] or sha(paths['baseline'] / 'SRB2.ELF') != baseline['elf_sha256']:
        raise RuntimeError('Canonical Hardware ELF/report differs')
    objects = {name: Object(path / 'obj/engine_lto.o') for name, path in paths.items()}
    signatures = {name: allocated(obj) for name, obj in objects.items()}
    procedures = {name: pdr(obj) for name, obj in objects.items()}
    restores = {
        'complete_allocated_sections_and_ordered_relocations': signatures['baseline'] == signatures['optout'],
        'complete_procedure_metadata_and_ordered_relocations': procedures['baseline'] == procedures['optout'],
        'complete_ELF': sha(paths['baseline'] / 'SRB2.ELF') == sha(paths['optout'] / 'SRB2.ELF'),
    }
    if not all(restores.values()):
        raise RuntimeError('Narrow opt-out does not restore the complete canonical object/ELF')
    all_names = set(signatures['baseline']) | set(signatures['candidate'])
    changed = sorted(name for name in all_names if signatures['baseline'].get(name) != signatures['candidate'].get(name))
    code = [name for name in changed if any(name in obj.sections and obj.sections[name][2] & 4 for obj in objects.values())]
    changed_targets = set(code)
    callers = {}
    for tag in ('baseline', 'candidate'):
        obj = objects[tag]
        for owner, records in obj.relocations.items():
            if owner not in obj.sections or not obj.sections[owner][2] & 4:
                continue
            target_uses = [r for r in records if r['target']['section'] in changed_targets]
            if target_uses:
                callers.setdefault(owner, {})[tag] = {'signature': obj.signature(owner),
                    'ordered_relocations': records, 'changed_target_bindings': target_uses,
                    'frame': procedures[tag]['records'].get(owner)}
    detail = {}
    for name in sorted(set(code) | set(callers)):
        detail[name] = {}
        for tag in ('baseline', 'candidate'):
            obj = objects[tag]
            if name in obj.sections:
                detail[name][tag] = {'signature': obj.signature(name),
                    'bytes_hex': obj.content(obj.sections[name]).hex(),
                    'ordered_relocations': obj.relocations.get(name, []),
                    'frame': procedures[tag]['records'].get(name)}
    for tag in paths:
        (out / (tag + '-complete-allocated.json')).write_text(json.dumps(signatures[tag], indent=2) + '\n')
        (out / (tag + '-complete-pdr.json')).write_text(json.dumps(procedures[tag], indent=2) + '\n')
    report = {'kind': __doc__, 'source_revision': replay.REVISION,
        'candidate_source_sha256': replay.CANDIDATE_SHA, 'contexts': contexts,
        'object_checks_executed': True, 'full205_actual_default_LTO': True,
        'optout_restoration': restores, 'changed_allocated_sections': changed,
        'changed_code_sections_or_bindings': code, 'direct_actual_callers': callers,
        'changed_code_and_caller_details': detail,
        'complete_signature_reports': {tag: {
            'allocated': str(out / (tag + '-complete-allocated.json')),
            'allocated_sha256': sha(out / (tag + '-complete-allocated.json')),
            'pdr': str(out / (tag + '-complete-pdr.json')),
            'pdr_sha256': sha(out / (tag + '-complete-pdr.json'))} for tag in paths},
        'artifacts': {tag: {name: {'path': str(path / name), 'sha256': sha(path / name), 'bytes': (path / name).stat().st_size}
                           for name in ('obj/engine_lto.o', 'SRB2.ELF', 'SRB2.map', 'build-report.json')}
                      for tag, path in paths.items()},
        'auditor_sha256': sha(Path(__file__)),
        'status': 'PASS actual LTO binding/opt-out restoration; no CPU/FPS or whole-game RAM claim',
    }
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'status': report['status'], 'report': str(out / 'report.json'),
                      'changed_code_sections_or_bindings': code, 'optout_restoration': restores}, indent=2))


if __name__ == '__main__':
    main()
