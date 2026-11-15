"""Admit the reviewed70->68 serial LTRANS note, then link a frozen control.

The rejected merge stage is never edited or rerun. Only the two exact reviewed
scheduling-message integers may differ; every other diagnostic byte survives.
Consumes one granted EE link worker, serially; never starts an emulator.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True); parser.add_argument('--failed', type=Path, required=True)
    parser.add_argument('--snapshot', type=Path, required=True); parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--original-five', action='store_true', help='The exact original-five-source control with final generated headers and PB active')
    args = parser.parse_args(); build, failed, snapshot, out = [getattr(args, n).resolve() for n in ('build', 'failed', 'snapshot', 'out')]
    if out.exists() or not out.is_relative_to(ROOT / 'build'): raise SystemExit('Fresh immutable link/admission output required')
    if (failed / 'build-report.json').exists(): raise SystemExit('Only the preserved rejected pre-link stage is resumable')
    before = {str(p): sha(p) for p in failed.rglob('*') if p.is_file()}
    source = json.loads((snapshot / 'manifest.json').read_text())
    actual_source = {str(p.relative_to(failed / 'snapshot')): sha(p) for p in (failed / 'snapshot/src').rglob('*') if p.is_file()}
    expected_source = dict(source['source_sha256'])
    legacy_files = ('src/hardware/hw_cache.c','src/ps2/hw/ps2_hw_pal.inc','src/ps2/hw/ps2_hwd.c','src/ps2/hw/ps2_hwd.h','src/ps2/hw/ps2_hwd_null.c')
    if args.original_five:
        original_source = json.loads((ROOT / 'tools/ps2/next12_baseline.json').read_text())['source_sha256']
        for name in legacy_files: expected_source[name] = original_source[name]
    if actual_source != expected_source or len(actual_source) != 602:
        raise SystemExit('The rejected stage does not use the complete exact published source')
    original = json.loads((build / 'build-report.json').read_text())
    if sha(build / 'SRB2.ELF') != original['elf_sha256'] or len(original['sources']) != 203:
        raise SystemExit('Exact complete actual canonical Hardware source graph required')
    diag = json.loads((failed / 'lto-diagnostics.json').read_text())
    if diag['all_normalized_diagnostics_identical']: raise SystemExit('Rejected difference must remain explicitly recorded')
    if (failed / 'lto-stdout.txt').read_bytes(): raise SystemExit('No control stdout may be admitted')
    left = (failed / 'canonical-lto.log').read_text(); right = (failed / 'lto-stderr.txt').read_text()
    count_line = 'lto-wrapper: warning: using serial compilation of %d LTRANS jobs\n'
    if not left.startswith(count_line % 70) or not right.startswith(count_line % 68):
        raise SystemExit('Only the exact reviewed70->68 serial partition diagnostic is admissible')
    paths = {}
    for entry in diag['byte_identical_input_paths']:
        for key in ('canonical_path', 'copied_path'):
            path = Path(entry[key])
            if sha(path) != entry['sha256']: raise SystemExit('Independently byte-identical input changed')
            paths[str(path)] = entry['identity']
            if path.is_relative_to(ROOT): paths[str(path.relative_to(ROOT))] = entry['identity']
    for entry in diag['checked_diagnostic_aliases']:
        if sha(Path(entry['actual_path'])) != entry['sha256']: raise SystemExit('Admitted diagnostic alias changed')
        paths[entry['diagnostic_spelling']] = entry['identity']
    changed = {Path(e['canonical_path']).name for e in diag['changed_inputs_never_path_normalized']}
    if changed != {'src__hardware__hw_cache.o', 'src__ps2__hw__ps2_hwd.o'}:
        raise SystemExit('Exactly the two intended opt-out IR replacements are required')
    def normalize(text):
        for spelling, identity in sorted(paths.items(), key=lambda item: len(item[0]), reverse=True):
            text = re.sub(r'(?<![\w./-])' + re.escape(spelling) + r'(?=[:\s\'"\)]|$)', lambda m: identity, text)
        return text.replace(str(ROOT) + '/', '')
    if normalize(left[len(count_line % 70):]) != normalize(right[len(count_line % 68):]):
        raise SystemExit('Additional LTO diagnostic text differs; cannot admit')
    commands = []
    for stem, unit in (('hw_cache', 'src/hardware/hw_cache.c'), ('ps2_hwd', 'src/ps2/hw/ps2_hwd.c')):
        lines = (failed / (stem + '-compile.log')).read_text().splitlines()
        if len(lines) != 1: raise SystemExit('The exact two source compiles must have no diagnostics')
        command = shlex.split(lines[0])
        if (('-DPS2_NOOPT_LIGHTTABLE_INDEXED' in command) == args.original_five) or str(failed / 'snapshot' / unit) not in command:
            raise SystemExit('Actual opt-out compile command differs')
        commands.append(command)
    objects = [failed / 'obj' / n.replace('/', '__').replace('.c', '.o') for n in original['sources']]
    objects += [failed / 'obj' / (n + '_vsm.o') for n in ('ps2_hw_vu1', 'ps2_hw_vu1_spr')]
    canonical_ir = {}; actual_ir = {}
    for path in objects:
        old = build / 'obj' / path.name; canonical_ir[str(old)] = sha(old); actual_ir[str(path)] = sha(path)
    if {Path(n).name for n in actual_ir if actual_ir[n] != canonical_ir[str(build / 'obj' / Path(n).name)]} != changed:
        raise SystemExit('Unexpected complete actual IR input differences')
    shutil.copytree(failed, out)
    pinned = out / 'pinned-finish-tools'; pinned.mkdir(); shutil.copyfile(Path(__file__), pinned / Path(__file__).name)
    command = list(original['link_command']); engine = out / 'obj/engine_lto.o'
    site = [i for i, arg in enumerate(command) if arg.endswith('/engine_lto.o')]
    if len(site) != 1: raise SystemExit('One exact canonical merged-engine link input required')
    command[site[0]] = str(engine); command[command.index('-o') + 1] = str(out / 'SRB2.ELF')
    site = [i for i, arg in enumerate(command) if arg.startswith('-Wl,-Map=')]
    if len(site) != 1: raise SystemExit('One exact canonical map output required')
    command[site[0]] = '-Wl,-Map=' + str(out / 'SRB2.map')
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    (out / 'completed-link.log').write_text(' '.join(command) + '\n' + result.stdout + result.stderr)
    diagnostics = '\n'.join(line for line in (result.stdout + result.stderr).splitlines() if 'linking abicalls files with non-abicalls files' not in line).strip()
    if result.returncode or diagnostics: raise SystemExit('Opt-out control link has new diagnostics or failed')
    if before != {str(p): sha(p) for p in failed.rglob('*') if p.is_file()}:
        raise SystemExit('The original rejected stage changed')
    generated = json.loads((failed / 'generated-input-archive.json').read_text())
    archive_paths = {e['original_path']: e['archive_path'] for e in generated['entries']}
    inputs = {}
    for n, value in original['input_sha256'].items():
        path = Path(n); actual = failed / 'snapshot/src' / path.relative_to(ROOT / 'src') if path.is_relative_to(ROOT / 'src') else Path(archive_paths.get(n, n))
        expected = expected_source['src/' + str(path.relative_to(ROOT / 'src'))] if path.is_relative_to(ROOT / 'src') else value
        if sha(actual) != expected: raise SystemExit('Actual canonical dependency changed during finish')
        inputs[str(actual)] = expected
    admitted = {'canonical_serial_LTRANS_jobs': 70, 'optout_serial_LTRANS_jobs': 68,
        'sole_reviewed_diagnostic_difference': True, 'all_other_text_exact_after_independently_equal_input_path_binding': True,
        'no_warning_text_filtered_or_silently_equalized': True, 'preserved_rejected_stage': str(failed),
        'rejected_stage_all_file_sha256': before, 'source_revision': source['revision'],
        'original_five_source_overlay': args.original_five, 'actual_complete602_source_sha256': actual_source,
        'canonical_IR': canonical_ir, 'actual_executed_LTO_IR': actual_ir,
        'no_compilation_or_LTO_rerun_for_reporting_admission': True, 'source_compile_commands_as_executed': commands,
        'copied_frozen_merged_engine_sha256': sha(engine), 'actual_completed_link_command': command}
    (out / 'reviewed-diagnostic-admission.json').write_text(json.dumps(admitted, indent=2) + '\n')
    report = dict(original); report.update(compiled=2, link_command=command, input_sha256=inputs,
        elf_sha256=sha(out / 'SRB2.ELF'), elf_bytes=(out / 'SRB2.ELF').stat().st_size,
        lighttable_context_control=admitted)
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Linked unchanged frozen opt-out engine with exact reviewed serial-partition admission: ' + str(out))


if __name__ == '__main__': main()
