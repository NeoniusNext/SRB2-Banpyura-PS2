#!/usr/bin/env python3
"""Audit the four completed late-shutdown normal-RNG smokes without reruns.

The original checker incorrectly also required ZQUIT DONE. The existing
Impl_VidShot calls I_Quit immediately after its last capture, before175.
This report preserves that failed analysis and checks the actual post-save
marker and complete files. It never changes or fabricates runtime evidence.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct

from next12_smoke_check import ROOT, MARKER, CAPTURES, STAGE_CONFIG, sha
from next11_smoke_check import runtime_evidence
from next12_integration_check import inspect
from next10_sw_bounds_codecheck import code, contributions, frame, output_bytes
from next7_render_fog_codecheck import Elf32
from next8_sw_codecheck import linked_symbols

FALSE_CRITERION = 'Late post-save shutdown marker or ZQUIT DONE absent'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, default=ROOT / 'build/nov15-next12/final-unmodified')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/nov15-next12/final-unmodified-audit')
    args = parser.parse_args(); work, out = args.work.resolve(), args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'):
        raise RuntimeError('Choose a fresh immutable report stage')
    readiness_path, old_path = work / 'readiness.json', work / 'report.json'
    ready, old = [json.loads(p.read_text()) for p in (readiness_path, old_path)]
    if len(ready['cases']) != 4 or old['errors'] != [e['name'] + ': ' + FALSE_CRITERION for e in ready['cases']]:
        raise RuntimeError('Only the documented four false ZQUIT conditions may be corrected')
    if any(sha(Path(p)) != value for p, value in ready['input_sha256'].items()):
        raise RuntimeError('An actual executed smoke input has changed')
    report = copy.deepcopy(old)
    report['errors'] = []
    report['corrected_analysis'] = {'original_report': str(old_path), 'original_report_sha256': sha(old_path),
        'original_checker_sha256': ready['input_sha256'][str(ROOT / 'tools/ps2/next12_smoke_check.py')],
        'removed_false_criterion': FALSE_CRITERION,
        'actual_exit_trigger': 'Impl_VidShot calls I_Quit after VIDSHOT COMPLETE3 at level frame140. The later zquit175 is not reached.',
        'runtime_rerun_or_evidence_rewrite': False}
    bindings = {}
    for entry in ready['cases']:
        name = entry['name']; run = work / 'runs' / name
        item = inspect(run, name)
        if item['errors'] or item['rc'] or item['captures'] != old['runs'][name]['captures']:
            raise RuntimeError('Independent runtime/image inspection changed: ' + name)
        runtime_evidence(work / 'runtime' / name, entry['command'])
        boot = (run / 'boot.txt').read_text(errors='replace')
        if boot.count(MARKER) != 1 or boot.find(MARKER) < boot.find('VIDSHOT COMPLETE 3') or 'ZQUIT DONE' in boot:
            raise RuntimeError('Exact capture→late-shutdown sequence differs: ' + name)
        if (run / '.srb2/reference.cfg').read_bytes() != STAGE_CONFIG or (run / 'reference.cfg.tmp').exists():
            raise RuntimeError('Staged configuration or completed atomic save differs')
        saved = run / 'reference.cfg'
        if not saved.stat().st_size or sha(saved) != old['runs'][name]['saved_configuration_sha256']:
            raise RuntimeError('Full completed configuration differs')
        if sha(run / 'SRB2.ELF') != entry['release']['ELF_sha256']:
            raise RuntimeError('Unmodified actually executed shipping ELF differs')
        for capture in CAPTURES:
            raw = (run / capture).read_bytes()
            if len(raw) != 192015 or not raw.startswith(b'P6\n320 200\n255\n'):
                raise RuntimeError('Complete PPM extent/header differs')
        build = Path(entry['release']['ELF']).parent
        engine, elf = Elf32(build / 'obj/engine_lto.o'), Elf32(build / 'SRB2.ELF')
        body = code(engine, 'Impl_VidShot')
        address, size = contributions(build / 'SRB2.map')['.text.Impl_VidShot']
        symbols = linked_symbols(elf)
        if size != body['text_bytes'] or address != symbols['Impl_VidShot']['value']:
            raise RuntimeError('Actual full capture body map/symbol extents differ')
        raw = output_bytes(elf, address, size)
        calls = []
        for offset, kind, target, addend, _, _, target_value in body['ordered_relocations']:
            if target not in ('I_Quit', '.text.I_Quit'):
                continue
            if kind != 4 or addend or target_value:
                raise RuntimeError('Unexpected actual I_Quit call relocation')
            expected = body['words'][offset // 4] | ((symbols['I_Quit']['value'] >> 2) & 0x03ffffff)
            if struct.unpack_from('<I', raw, offset)[0] != expected:
                raise RuntimeError('Actual linked capture I_Quit instruction differs')
            calls.append({'offset': offset, 'actual_linked_instruction': expected, 'actual_target': symbols['I_Quit']['value']})
        bindings[name] = {'scope': 'Complete input bytes/ordered RELAs/PDR and linked-body hash; I_Quit call instructions individually resolved. Other data RELAs are not claimed recursively reconstructed.',
                          'complete_input_body': body, 'input_PDR': frame(engine, 'Impl_VidShot'),
                          'complete_linked_body_address': address, 'complete_linked_body_bytes': size,
                          'complete_linked_body_sha256': hashlib.sha256(raw).hexdigest(), 'resolved_I_Quit_call_witnesses': calls}
        if not calls:
            raise RuntimeError('Actual complete linked capture body lacks I_Quit call')
        if old['runs'][name]['errors'] != [FALSE_CRITERION]:
            raise RuntimeError('A non-ZQUIT error cannot be dismissed')
        report['runs'][name]['errors'] = []
        report['runs'][name]['post_save_and_sound_graphics_shutdown_marker_observed'] = True
    for pair in report['pairs']:
        before, after = [work / 'runs' / (r + '-normal-' + pair['scene']) for r in ('base', 'final')]
        if any((before / capture).read_bytes() != (after / capture).read_bytes() for capture in CAPTURES):
            raise RuntimeError('A complete normal-RNG frame pair differs')
        if (before / 'reference.cfg').read_bytes() != (after / 'reference.cfg').read_bytes():
            raise RuntimeError('A complete saved configuration pair differs')
        if not all(pair[k] for k in ('LHASH_equal', 'complete_PPMs_literal_equal', 'complete_saved_configuration_literal_equal')):
            raise RuntimeError('Existing non-timing pair comparison failed')
    report['actual_capture_exit_call_witnesses'] = bindings
    report['source_sha256'] = {str(p): sha(p) for p in (ROOT / 'src/ps2/i_video.c', ROOT / 'src/ps2/i_system.c')}
    report['analysis_checker_sha256'] = sha(Path(__file__))
    report['status'] = 'PASS four unchanged normal-RNG games; six complete PPM pairs and full saved configurations equal; late sound/graphics shutdown observed'
    out.mkdir(parents=True)
    (out / Path(__file__).name).write_bytes(Path(__file__).read_bytes())
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ('status', 'pairs', 'errors')}, indent=2))


if __name__ == '__main__':
    main()
