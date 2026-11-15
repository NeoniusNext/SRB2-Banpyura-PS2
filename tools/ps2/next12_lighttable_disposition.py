"""Map every historical root to real matched current-control code or absence.

Does not invent a function body for an inlined/discarded spelling. Exact live
variants and compiled callers are fully resolved in both current controls.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

from next8_sw_codecheck import linked_symbols
from next10_sw_bounds_codecheck import code, frame, relocations
from next12_pb_scratch_codecheck import binder
from next12_lighttable_codecheck import signature

ROOT = Path(__file__).resolve().parents[2]


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def logical(name):
    return re.sub(r'\.(?:lto_priv|part|constprop|isra)\.\d+', '', name)


def source_calls(root, name):
    result = []
    headers = re.compile(r'(?m)^(?:static\s+)?(?:inline\s+)?[A-Za-z_][\w \t*]*\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{')
    erase = re.compile(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
    for path in sorted(root.rglob('*')):
        if not path.is_file() or path.suffix not in ('.c', '.inc'): continue
        raw = path.read_text(errors='replace')
        if not re.search(r'\b' + re.escape(name) + r'\s*\(', raw): continue
        text = erase.sub(lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), raw)
        spans = []
        for match in headers.finditer(text):
            at = match.end(); depth = 1
            while depth and at < len(text):
                depth += (text[at] == '{') - (text[at] == '}'); at += 1
            if depth: raise SystemExit('Cannot identify complete source caller body')
            spans.append((match.end(), at, match[1]))
        for match in re.finditer(r'\b' + re.escape(name) + r'\s*\(', text):
            parents = [owner for start, end, owner in spans if start <= match.start() < end]
            if not parents: continue  # declaration/definition, not a call.
            result.append({'file': 'src/' + str(path.relative_to(root)), 'line': raw[:match.start()].count('\n') + 1,
                           'source_function': parents[-1], 'source_file_sha256': sha(path)})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args(); out = args.out.resolve(); work = ROOT / 'build/nov15-next12/lighttable-final'
    if out.exists() or not out.is_relative_to(ROOT / 'build'): raise SystemExit('Fresh immutable disposition stage required')
    out.mkdir(parents=True); shutil.copyfile(Path(__file__), out / Path(__file__).name)
    final_path = work / 'finalbind-v4/final-binding-report.json'; final = json.loads(final_path.read_text())
    if not final['current_header_PB_active_optout_original_graph']['all_whole_engine_function_sections_and_ordered_RELA_records_equal']:
        raise SystemExit('The complete matched current-control identity must already pass')
    reference = binder(work / 'original-source-v2', work / 'linker-original-source-v2')
    optout = binder(work / 'optout-v2', work / 'linker-optout-v2')
    inventories = [{n[6:] for n in value.obj.names if n.startswith('.text.')} for value in (reference, optout)]
    if inventories[0] != inventories[1]: raise SystemExit('Complete matched input function inventories differ')
    names = inventories[0]; live = [linked_symbols(value.elf) for value in (reference, optout)]
    historical_path = ROOT / 'build/nov15-next12/lighttable-combined-v2/binding-v1/binding-report.json'
    historical = list(json.loads(historical_path.read_text())['versions']['reference']['actual_linked_function_bindings'])
    if len(historical) != 26: raise SystemExit('All26 historical roots are required')
    bound = {}; dispositions = {}
    def bind_live(name):
        if name in bound: return
        if name not in live[0] or name not in live[1]: raise SystemExit('Cannot invent an unlinked current body')
        if signature(reference.obj, name) != signature(optout.obj, name) or frame(reference.obj, name) != frame(optout.obj, name):
            raise SystemExit('Actual current variant/body/frame differs')
        bound[name] = {'original_source_control': reference.text(name), 'specific_optout_control': optout.text(name)}
    for old in historical:
        base = logical(old); exact = old in names
        current = [old] if exact else sorted(n for n in names if logical(n) == base)
        entry = {'historical_name': old, 'historical_compilation_context_is_not_substituted_for_current_context': True,
                 'input_name_present_in_both_current_inventories': exact, 'current_variants': []}
        for name in current:
            if name in live[0] and name in live[1]:
                bind_live(name); status = 'live complete current body bound in both matched ELFs'
            elif name not in live[0] and name not in live[1]:
                status = 'input body retained, linker discarded in both current ELFs; no linked-body claim'
            else: raise SystemExit('Current-control liveness differs')
            sites = []
            for owner in sorted(names):
                for r in relocations(optout.obj, owner):
                    if r['symbol']['name'] in (name, '.text.' + name):
                        record = {'caller': owner, 'offset': r['offset'], 'kind': r['kind'], 'addend': r['addend'], 'caller_linked': owner in live[1]}
                        if owner in live[1]: record['linked_call_address'] = optout.maps['.text.' + owner][0] + r['offset']
                        sites.append(record)
            entry['current_variants'].append({'name': name, 'status': status, 'actual_direct_RELA_callers': sites})
        calls = source_calls(work / 'original-source-v2/snapshot/src', base)
        for call in calls:
            variants = sorted(n for n in names if logical(n) == call['source_function'])
            call['actual_current_compiled_caller_variants'] = []
            for variant in variants:
                record = {'name': variant, 'linked_in_both': variant in live[0] and variant in live[1]}
                if record['linked_in_both']:
                    bind_live(variant); record['original_source_address'] = live[0][variant]['value']; record['optout_address'] = live[1][variant]['value']
                call['actual_current_compiled_caller_variants'].append(record)
        entry['source_call_locations'] = calls
        if not current:
            remaining = []
            for owner in sorted(names):
                remaining += [(owner, r['offset'], r['symbol']['name']) for r in relocations(optout.obj, owner)
                              if r['symbol']['name'] in (old, '.text.' + old, base, '.text.' + base)]
            if remaining: raise SystemExit('Absent root unexpectedly retains a direct relocation target')
            entry['status'] = 'absent from both complete input function inventories; no standalone body/size/address is fabricated'
            entry['no_direct_RELA_target_survives_in_either_matched_inventory'] = True
        elif not exact: entry['status'] = 'historical spelling absent; actual current compiler variant names are bound explicitly'
        else: entry['status'] = 'historical spelling exists in both current inventories; actual current liveness/body recorded'
        dispositions[old] = entry
    report = {'scope': 'All26 historical source85-root spellings mapped to actual matched current-generated-header/PB-active control code, aliases or absence; no historical-body substitution',
        'final_v4_report_sha256': sha(final_path), 'historical_inventory_report_sha256': sha(historical_path),
        'historical_roots': len(dispositions), 'whole_matched_input_function_sections': len(names),
        'all26_dispositions': dispositions, 'complete_actual_live_variants_and_source_callers_bound': bound,
        'all_complete_current_referenced_data_original': list(reference.data.values()),
        'all_complete_current_referenced_data_optout': list(optout.data.values()),
        'pinned_checker_sha256': sha(out / Path(__file__).name)}
    (out / 'disposition-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Mapped all26 historical roots to real current code or explicit absence: ' + str(out))


if __name__ == '__main__': main()
