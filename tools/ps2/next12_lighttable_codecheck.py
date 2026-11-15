"""Bind complete actual Hardware light-table graphs, callers and CLUT consumers.

No compiler/emulator is invoked. Full allocated ELF bytes are resolved from
explicit RELAs and exact input-map contributions. Generated CSWTCH names may
match only when the complete table bytes, alignment and no-RELA policy match.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct

from next7_render_fog_codecheck import Elf32
from next8_sw_codecheck import linked_symbols
from next10_sw_bounds_codecheck import code, frame, output_bytes, relocations, symbol_tables
from next11_sw_checkplane_codecheck import Binder, build_binder

ROOT = Path(__file__).resolve().parents[2]
GETTER = 'HWR_GetLightTableID'
INDEXED = 'lt_store_indexed.lto_priv.0'
COMMON = ('lt_store.lto_priv.0', 'hw_CreateLightTable', 'hw_UpdateLightTable',
          'HWR_ClearLightTables', 'hw_ClearLightTables', 'hw_SetScreenPalette',
          'clut_alloc', 'clut_build_into', 'clut_fixed_refresh.lto_priv.0',
          'clut_get.lto_priv.0', 'PS2HWD_FillDriver', 'ov_flush.part.0.lto_priv.0',
          '__wrap_memcpy', '__wrap_memset')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def table_token(obj, symbol):
    if not re.fullmatch(r'CSWTCH\.\d+\.lto_priv\.0', symbol['name']):
        return symbol['name']
    index = symbol['section']; section = obj.sections[index]
    if symbol['value'] or section[1] != 1 or not obj.names[index].startswith('.rodata.'):
        raise SystemExit('Unexpected generated switch object')
    if any(s[1] in (4, 9) and s[7] == index for s in obj.sections):
        # Preserve its exact name. A renamed table with RELAs cannot match;
        # the two permitted renamed value tables have no pointer relocations.
        return symbol['name']
    return ('complete_generated_switch', section[5], section[8], obj.bytes(section).hex())


def signature(obj, name):
    body = code(obj, name)
    return {'words': body['words'], 'bytes': body['text_bytes'],
            'ordered_RELA': [(r['offset'], r['kind'], table_token(obj, r['symbol']),
                              r['addend'], r['symbol']['type'], r['symbol']['value'])
                             for r in relocations(obj, name)]}


def callers(obj):
    result = {}; tables = symbol_tables(obj)
    for section in obj.sections:
        name = obj.names[section[7]]
        if section[1] != 4 or not name.startswith('.text.'):
            continue
        for at in range(section[4], section[4] + section[5], section[9]):
            offset, info, addend = struct.unpack_from('<IIi', obj.data, at)
            if tables[section[6]][info >> 8]['name'] in (GETTER, '.text.' + GETTER):
                result.setdefault(name[6:], []).append((offset, info & 255, addend))
    return result


def scope(reference, candidate):
    names = [{n[6:] for n in obj.names if n.startswith('.text.')} for obj in (reference, candidate)]
    added = names[1] - names[0]
    shared = '.text.' + INDEXED not in candidate.index
    expected = {'PS2HWD_CreateIndexedLightTable', 'PS2HWD_UpdateIndexedLightTable'}
    if not shared: expected.add(INDEXED)
    if names[0] - names[1] or added != expected:
        raise SystemExit('Unexpected complete full-engine function inventory difference')
    changed = []; switch_names = {}
    for name in sorted(names[0] & names[1]):
        left, right = signature(reference, name), signature(candidate, name)
        if left != right:
            changed.append(name)
        elif code(reference, name) != code(candidate, name):
            if frame(reference, name) != frame(candidate, name):
                raise SystemExit('Generated-name-only function frame changed')
            switches = []
            for a, b in zip(relocations(reference, name), relocations(candidate, name)):
                if a['symbol']['name'] != b['symbol']['name']:
                    token = table_token(reference, a['symbol'])
                    if token != table_token(candidate, b['symbol']):
                        raise SystemExit('Generated table semantic difference')
                    switches.append({'reference': a['symbol']['name'], 'candidate': b['symbol']['name'],
                                     'complete_table': token})
            switch_names[name] = switches
    expected_changed = {GETTER, 'lt_store.lto_priv.0', 'hw_CreateLightTable', 'hw_UpdateLightTable'} if shared else {GETTER}
    if set(changed) != expected_changed:
        raise SystemExit('Other complete actual instructions/ordered targets/addends changed: ' + str(changed))
    # Full data/BSS capacities are checked without name/size normalization.
    for prefix in ('.data', '.sdata', '.bss', '.sbss'):
        get = lambda obj: {n: (s[1], s[2], s[5], s[8], None if s[1] == 8 else obj.bytes(s).hex())
                           for n, s in zip(obj.names, obj.sections) if n.startswith(prefix)}
        if get(reference) != get(candidate):
            raise SystemExit('Complete actual initialized data/BSS/capacity changed: ' + prefix)
    if callers(reference) != callers(candidate):
        raise SystemExit('Actual getter caller sites changed')
    return {'semantically_changed_existing_functions': changed, 'added_functions': sorted(added),
            'single_shared_private_RGBA_or_indexed_store': shared,
            'all_other_complete_instructions_ordered_RELA_targets_addends_equal': True,
            'generated_name_only_functions': switch_names,
            'complete_initialized_data_and_BSS_capacities_identical': True,
            'complete_actual_getter_callers_and_sites': callers(reference)}


def copy_binder(game, obj=None, elf=None):
    result = copy.copy(game); result.obj = obj or game.obj; result.elf = elf or game.elf
    result.symbols = linked_symbols(result.elf); result.data = {}; result.literals = {}
    return result


def controls(out, game, caller, store=INDEXED):
    results = []; out.mkdir()

    def reject(name, raw, suffix, check):
        path = out / (name + suffix); path.write_bytes(raw); mutated = Elf32(path)
        try:
            check(mutated)
        except SystemExit as error:
            results.append({'mutation': name, 'rejected': True, 'reason': str(error), 'sha256': sha(path)})
        else:
            raise SystemExit('Actual binary mutation escaped gate: ' + name)

    def equal(mutated, name):
        if signature(game.obj, name) != signature(mutated, name) or frame(game.obj, name) != frame(mutated, name):
            raise SystemExit('Complete actual instructions/ordered RELAs/frame differ')

    names = (GETTER, store, caller, 'clut_build_into', '__wrap_memcpy')
    for ordinal, name in enumerate(names):
        for field in ('register', 'frame'):
            raw = bytearray(game.obj.data)
            if field == 'register':
                section = game.obj.sections[game.obj.index['.text.' + name]]; at = section[4]
                struct.pack_into('<I', raw, at, struct.unpack_from('<I', raw, at)[0] ^ 0x10000)
            else:
                pdr = game.obj.sections[game.obj.index['.pdr']]; locations = []
                for rela in game.obj.sections:
                    if rela[1] != 4 or rela[7] != game.obj.index['.pdr']: continue
                    tables = symbol_tables(game.obj)
                    for at in range(rela[4], rela[4] + rela[5], rela[9]):
                        offset, info, addend = struct.unpack_from('<IIi', raw, at)
                        if tables[rela[6]][info >> 8]['name'] in (name, '.text.' + name): locations.append(pdr[4] + offset + 20)
                if len(locations) != 1: raise SystemExit('One complete actual frame record required')
                at = locations[0]; struct.pack_into('<I', raw, at, struct.unpack_from('<I', raw, at)[0] ^ 16)
            reject(str(ordinal) + '-' + field, raw, '.o', lambda obj, n=name: equal(obj, n))
        raw = bytearray(game.elf.data); address = game.maps['.text.' + name][0]
        section = next(s for s in game.elf.sections if s[1] != 8 and s[3] <= address < s[3] + s[5])
        at = section[4] + address - section[3]
        struct.pack_into('<I', raw, at, struct.unpack_from('<I', raw, at)[0] ^ 0x10000)
        reject(str(ordinal) + '-linked-register', raw, '.ELF', lambda elf, n=name: copy_binder(game, elf=elf).text(n))
    record = relocations(game.obj, store)[0]
    for field in ('target', 'addend', 'kind', 'location'):
        raw = bytearray(game.obj.data); at = record['record_file_offset']
        if field == 'target':
            table = symbol_tables(game.obj)[record['symbol_table']]
            other = next(i for i, s in enumerate(table) if s['name'] == 'malloc')
            info = struct.unpack_from('<I', raw, at + 4)[0]; struct.pack_into('<I', raw, at + 4, (other << 8) | (info & 255))
        elif field == 'addend': struct.pack_into('<i', raw, at + 8, record['addend'] + 4)
        elif field == 'kind':
            info = struct.unpack_from('<I', raw, at + 4)[0]; struct.pack_into('<I', raw, at + 4, (info & ~255) | 1)
        else: struct.pack_into('<I', raw, at, record['offset'] + 4)
        reject('store-RELA-' + field, raw, '.o', lambda obj: equal(obj, store))
    literal = next(r for r in relocations(game.obj, store) if r['symbol']['name'].startswith('.rodata.'))
    section = game.obj.sections[literal['symbol']['section']]; raw = bytearray(game.obj.data); raw[section[4] + literal['addend']] ^= 1
    reject('object-log-literal', raw, '.o', lambda obj: copy_binder(game, obj=obj).text(store))
    log_address, _ = game.resolve(literal['symbol'], literal['addend'], literal['kind'], game.maps['.text.' + store][0] + literal['offset'])
    section = next(s for s in game.elf.sections if s[1] != 8 and s[3] <= log_address < s[3] + s[5])
    raw = bytearray(game.elf.data); raw[section[4] + log_address - section[3]] ^= 1
    reject('linked-log-literal', raw, '.ELF', lambda elf: copy_binder(game, elf=elf).text(store))
    # Prove that a pointer to another identical, aligned pooled literal fails.
    choices = [(entry, r) for entry in game.data.values() for r in entry['ordered_RELA_records']
               if r['exact_symbol'].startswith('.rodata.')]
    for entry, r in choices:
        address = r['resolved_address']; content = output_bytes(game.elf, address, 1)
        if content != b'0' or output_bytes(game.elf, address, 2) != b'0\0': continue
        alternatives = []
        for section in game.elf.sections:
            if section[1] == 8 or not section[2] & 2: continue
            data = game.elf.bytes(section); at = data.find(b'0\0')
            while at >= 0:
                other = section[3] + at
                if other != address and other % 8 == address % 8: alternatives.append(other)
                at = data.find(b'0\0', at + 1)
        if not alternatives: continue
        pointer = entry['linked_address'] + r['offset']; raw = bytearray(game.elf.data)
        section = next(s for s in game.elf.sections if s[1] != 8 and s[3] <= pointer < s[3] + s[5])
        struct.pack_into('<I', raw, section[4] + pointer - section[3], alternatives[0])
        data_index = game.obj.index[entry['input_section']]
        reject('identical-other-zero-literal-pointer', raw, '.ELF', lambda elf, i=data_index: copy_binder(game, elf=elf).bind_data(i))
        break
    else: raise SystemExit('Required duplicate-zero literal control unavailable')
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, default=ROOT / 'build/nov15-next12/lighttable')
    parser.add_argument('--host', type=Path, default=ROOT / 'build/nov15-next12/lighttable/host-v3')
    parser.add_argument('--reference-build', type=Path); parser.add_argument('--candidate-build', type=Path)
    parser.add_argument('--reference-linker', type=Path); parser.add_argument('--candidate-linker', type=Path)
    parser.add_argument('--pb-overlay', action='store_true', help='Permit only separately tested PB08e3 source overlay beyond the table host context')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--negative-controls', action='store_true')
    args = parser.parse_args(); work = args.work.resolve(); out = args.out.resolve(); host = args.host.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'): raise SystemExit('Fresh immutable output under build/ required')
    out.mkdir(parents=True); pinned = out / 'pinned-tools'; pinned.mkdir()
    for name in ('next12_lighttable_codecheck.py', 'next11_sw_checkplane_codecheck.py', 'next11_sw_linker_relas.py',
                 'next10_sw_bounds_codecheck.py', 'next7_render_fog_codecheck.py', 'next8_sw_codecheck.py'):
        shutil.copyfile(ROOT / 'tools/ps2' / name, pinned / name)
    binders = {}
    for variant in ('reference', 'candidate'):
        build = (getattr(args, variant + '_build') or work / (variant + '-hardware')).resolve()
        linker = (getattr(args, variant + '_linker') or work / ('link-rela-' + variant + '-v1')).resolve()
        binders[variant] = build_binder(build / 'obj/engine_lto.o', build / 'SRB2.ELF', build / 'SRB2.map', linker)
    report = {'scope': 'Complete actual default Hardware graphs and all direct callers; static binding, no native execution or CPU/RAM saving claim',
              'whole_engine_scope': scope(binders['reference'].obj, binders['candidate'].obj), 'versions': {}}
    host_manifest = json.loads((host / 'source-hashes.json').read_text())
    if any(sha(host / name) != digest for name, digest in host_manifest['files'].items()): raise SystemExit('Frozen host source/tool changed')
    report['frozen_host_stage_all_input_and_result_sha256'] = {str(p.relative_to(host)): sha(p) for p in sorted(host.rglob('*')) if p.is_file()}
    compiled_report = json.loads((binders['candidate'].map_path.parent / 'build-report.json').read_text())
    compiled_source = compiled_report['lighttable_isolated_source']['source']['source_sha256']
    host_source = {'src/' + n[len('candidate-src/'):]: value for n, value in host_manifest['files'].items() if n.startswith('candidate-src/')}
    host_extras = {}
    if args.pb_overlay:
        name = 'src/hardware/hw_pbatch.inc'; expected_pb = '08e3ce0c810ba69f957ab8aaa0082e28bcdb1e3f7a3939f8d0da1e8133e2e350'
        if compiled_source.get(name) != expected_pb:
            raise SystemExit('Combined context is not the exact separately tested PB overlay')
        host_extras[name] = {'host_source_sha256': host_source[name], 'compiled_separately_tested_PB_sha256': expected_pb}
        host_source[name] = expected_pb
    if compiled_source != host_source or len(host_source) != 602:
        raise SystemExit('Full default build does not use the exact602 tested candidate sources')
    report['full_default_build_source_binding'] = {'source_files': 602,
        'all_table_host_source_files_identical_except_explicit_separately_tested_component': True,
        'separately_tested_source_overlay': host_extras}
    actual_callers = sorted(callers(binders['reference'].obj))
    for variant, binder in binders.items():
        selected = [GETTER, *COMMON, *actual_callers]
        selected += ['HWR_CreateLightTable', 'HWR_UpdateLightTable'] if variant == 'reference' else ['PS2HWD_UpdateIndexedLightTable'] + ([INDEXED] if '.text.' + INDEXED in binder.obj.index else [])
        selected += sorted(report['whole_engine_scope']['generated_name_only_functions'])
        live = linked_symbols(binder.elf)
        roots = {name: binder.text(name) for name in dict.fromkeys(selected)}
        absent = [n for n in ('HWR_CreateLightTable', 'HWR_UpdateLightTable', 'PS2HWD_CreateIndexedLightTable') if n not in live]
        report['versions'][variant] = {'actual_linked_function_bindings': roots,
            'explicitly_discarded_not_game_executed': absent,
            'end': live['_end']['value'], 'gp': live['_gp']['value'],
            'actual_graph_static_pins': {'engine_sha256': sha(binder.obj.path), 'ELF_sha256': sha(binder.elf.path), 'map_sha256': sha(binder.map_path)},
            'all_referenced_complete_data': list(binder.data.values()), 'all_exact_literals': list(binder.literals.values())}
    if args.negative_controls:
        store = INDEXED if '.text.' + INDEXED in binders['candidate'].obj.index else 'lt_store.lto_priv.0'
        report['executed_actual_binary_mutation_controls'] = controls(out / 'binary-controls', binders['candidate'], actual_callers[0], store)
    report['pinned_tool_sha256'] = {p.name: sha(p) for p in pinned.iterdir()}
    (out / 'binding-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Bound complete actual Hardware graphs/callers/data/CLUT consumers: ' + str(out))


if __name__ == '__main__':
    main()
