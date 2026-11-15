#!/usr/bin/env python3
"""Resolve actual PB/producers/callers/data/literals against complete game ELFs.

Read-only code verification and binary negative controls. Exact retained linker
RELAs select merged-string addresses; no instruction/data relocation is masked.
No compiler, emulator, native timing or production source edit is performed.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

from next7_render_fog_codecheck import Elf32
from next8_sw_codecheck import linked_symbols
from next10_sw_bounds_codecheck import frame, output_bytes, relocations, symbol_tables
from next11_sw_checkplane_codecheck import Binder
from next11_local_rng import stub_words, verify_patch
import next12_pb_scratch_replay as replay

ROOT = replay.ROOT
WORK = ROOT / 'build/nov15-next12/pb-scratch'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def source_relas(obj, index):
    result = []
    for section in obj.sections:
        if section[7] != index or section[1] not in (4, 9):
            continue
        if section[1] != 4:
            raise SystemExit('Explicit ordered RELAs required')
        table = symbol_tables(obj)[section[6]]
        for offset in range(section[4], section[4] + section[5], section[9]):
            place, info, addend = struct.unpack_from('<IIi', obj.data, offset)
            result.append({'offset': place, 'kind': info & 255, 'symbol': table[info >> 8],
                           'addend': addend, 'record_file_offset': offset, 'symbol_table': section[6]})
    return result


def encoded_frame(raw, metadata):
    """Instruction witnesses, not invented metadata in the stripped game ELF."""
    words = struct.unpack('<' + 'I' * (len(raw) // 4), raw)
    fields = struct.unpack('<8I', bytes.fromhex(metadata['frame_record_hex']))
    amount, mask, save = fields[5], fields[1], struct.unpack('<i', struct.pack('<I', fields[2]))[0]
    reservations, saves = [], []
    for at, word in enumerate(words):
        op, rs, rt = word >> 26, (word >> 21) & 31, (word >> 16) & 31
        immediate = struct.unpack('<h', struct.pack('<H', word & 65535))[0]
        if op in (9, 25) and rs == 29 and rt == 29 and immediate < 0:
            reservations.append({'offset': at * 4, 'bytes': -immediate})
        if op in (43, 63, 31) and rs == 29:
            saves.append({'offset': at * 4, 'register': rt, 'stack_offset': immediate, 'opcode': op})
    if amount and not any(row['bytes'] == amount for row in reservations):
        raise SystemExit('Advertised PDR stack reservation lacks an exact body instruction witness')
    highest = mask.bit_length() - 1
    if mask and not any(row['register'] == highest and row['stack_offset'] == amount + save for row in saves):
        raise SystemExit('Advertised highest PDR register-save offset lacks an exact body instruction witness')
    if mask and not all(any(row['register'] == register for row in saves) for register in range(32) if mask & (1 << register)):
        raise SystemExit('Advertised PDR integer-save mask lacks complete body store witnesses')
    return {'complete_input_PDR_words': list(fields), 'constant_stack_reservations': reservations,
            'SP_relative_register_stores': saves, 'advertised_reservation_and_integer_save_mask_witnessed': True,
            'scope': 'Shipping link discards .pdr; exact input metadata is tied to the fully resolved complete body and explicit stack/save instruction witnesses.'}


class StrictBinder(Binder):
    def resolve(self, symbol, addend, kind, source_address=None):
        address, target = super().resolve(symbol, addend, kind, source_address)
        index = symbol['section']
        if index and index < len(self.obj.sections) and self.obj.sections[index][2] & 0x30 == 0x30:
            name = self.obj.names[index]
            position = symbol['value'] + addend + (self.gp if kind == 7 and symbol['type'] == 3 else 0)
            literal = self.literals[(name, position)]
            self.literal_uses.append({'source_address': source_address, 'kind': kind,
                'input_symbol': symbol, 'input_section': name, 'input_literal_offset': position,
                'explicit_addend': addend, 'resolved_address': address,
                'complete_literal_hex': literal['complete_literal_hex'],
                'exact_retained_linker_anchor': literal['retained_exact_linker_RELA_anchor']})
        return address, target

    def bind_data(self, index):
        name = self.obj.names[index]
        if name in self.data:
            return
        section = self.obj.sections[index]
        address = self.mapped(index)
        if section[5]:
            matches = [s for s in self.elf.sections if s[2] & 2 and s[3] <= address
                       and address + section[5] <= s[3] + s[5] and (s[1] == 8) == (section[1] == 8)]
            if len(matches) != 1 or (matches[0][2] & 3) != (section[2] & 3):
                raise SystemExit('Complete final data/BSS extent, flags or capacity differs ' + name)
        super().bind_data(index)
        entry = self.data[name]
        entry.update(type=section[1], flags=section[2], entry_bytes=section[9])
        source = source_relas(self.obj, index)
        if len(source) != len(entry['ordered_RELA_records']):
            raise SystemExit('Complete ordered data RELA count differs')
        for record, original in zip(entry['ordered_RELA_records'], source):
            if (record['offset'], record['kind'], record['explicit_addend']) != (original['offset'], original['kind'], original['addend']):
                raise SystemExit('Complete ordered data RELA location/type/addend differs')
            record['input_symbol'] = original['symbol']
        anchors = []
        final_symbols = [s for table in symbol_tables(self.elf).values() for s in table]
        for symbol in [s for table in symbol_tables(self.obj).values() for s in table
                       if s['section'] == index and s['type'] == 1 and s['name']]:
            exact = {(s['value'], s['size']) for s in final_symbols if s['name'] == symbol['name']
                     and s['section'] and s['value'] == address + symbol['value']}
            if exact != {(address + symbol['value'], symbol['size'])}:
                raise SystemExit('Exact final named data object address/extent differs ' + symbol['name'])
            anchors.append({'symbol': symbol['name'], 'address': address + symbol['value'], 'bytes': symbol['size']})
        entry['exact_named_object_anchors'] = anchors

    def text(self, name):
        result = super().text(name)
        actual = output_bytes(self.elf, self.maps['.text.' + name][0], result['complete_text_bytes'])
        result['resolved_text_sha256'] = digest(actual)
        result['frame_instruction_witness'] = encoded_frame(actual, result['exact_frame'])
        source = relocations(self.obj, name)
        if len(source) != len(result['ordered_RELA_records']):
            raise SystemExit('Complete ordered code RELA count differs')
        for record, original in zip(result['ordered_RELA_records'], source):
            record['input_symbol'] = original['symbol']
        result.pop('referenced_data')
        result.pop('complete_literals')
        return result


def binder(build, linker):
    report = json.loads((build / 'build-report.json').read_text())
    if sha(build / 'SRB2.ELF') != report['elf_sha256']:
        raise SystemExit('Complete actual build/ELF pin differs')
    value = StrictBinder(Elf32(build / 'obj/engine_lto.o'), Elf32(build / 'SRB2.ELF'),
                         build / 'SRB2.map', report['link_command'], linker)
    value.literal_uses = []
    return value


def selected(inventory, obj):
    mandatory = set(inventory['changed_code_sections_or_bindings']) | set(inventory['direct_actual_callers'])
    extra = {name for name in obj.names if name.startswith(('.text.HWR_PB', '.text.PS2HWD_Plan', '.text.__wrap'))}
    extra |= {'.text.PS2HWD_DrawBlocks', '.text.Z_ReallocAlign', '.text.Z_TryReallocAlign', '.text.Z_Free', '.text.Z_MallocAlign'}
    return mandatory, mandatory | extra


def boundary_records(value, checked):
    records = {}
    final = [s for table in symbol_tables(value.elf).values() for s in table]
    for owner, body in checked.items():
        for relocation in body['ordered_RELA_records']:
            symbol = relocation['resolved_symbol']
            rows = [s for s in final if s['name'] == symbol and s['section'] and s['value'] == relocation['resolved_address']]
            if not rows:
                continue  # section-symbol plus interior addend is described by the exact RELA/map binding.
            key = symbol + '@' + hex(relocation['resolved_address'])
            if key not in records:
                row = rows[0]
                entry = {'resolved_symbol': symbol, 'resolved_address': row['value'], 'symbol_type': row['type'],
                         'symbol_binding': row['binding'], 'exact_symbol_bytes': row['size'], 'actual_call_sites': []}
                if row['size'] and value.elf.sections[row['section']][1] != 8:
                    entry['complete_linked_symbol_sha256'] = digest(output_bytes(value.elf, row['value'], row['size']))
                entry['complete_body_reconstructed_here'] = symbol in checked
                records[key] = entry
            records[key]['actual_call_sites'].append({'owner': owner, **relocation})
    return records


def sdk_memory(value, build, out):
    """Reconstruct the actual three newlib memory bodies from exact archive members."""
    content = (build / 'SRB2.map').read_text()
    results = {}
    for function_name in ('memcpy', 'memset', 'memmove'):
        owner_pattern = r'(/[^\s()]+\.a)\((libc_a-' + function_name + r'\.o)\)'
        owners = set(re.findall(owner_pattern, content))
        if len(owners) != 1:
            raise SystemExit('One actual SDK memory archive/member required ' + function_name)
        archive, member = next(iter(owners))
        raw = subprocess.check_output(['ar', 'p', archive, member])
        path = out / (function_name + '-actual-sdk.o')
        path.write_bytes(raw)
        obj = Elf32(path)
        owner = archive + '(' + member + ')'
        maps = {}
        pattern = r'^\s+(\.[^\s]+)\s+(?:\n\s+)?0x([a-f0-9]+)\s+0x([a-f0-9]+)\s+' + re.escape(owner) + r'\s*$'
        for name, address, size in re.findall(pattern, content, re.M):
            if name in maps and maps[name] != (int(address, 16), int(size, 16)):
                raise SystemExit('Ambiguous exact SDK contribution')
            maps[name] = (int(address, 16), int(size, 16))
        source = [s for table in symbol_tables(obj).values() for s in table
                  if s['name'] == function_name and s['type'] == 2 and s['section']]
        if len(source) != 1:
            raise SystemExit('One exact SDK function symbol required')
        symbol = source[0]
        index = symbol['section']
        name = obj.names[index]
        section = obj.sections[index]
        if maps.get(name, (0, 0))[1] != section[5] or not maps[name][0]:
            raise SystemExit('Exact complete SDK text contribution differs')
        current = copy.copy(value)
        current.obj, current.maps, current.data, current.literals = obj, maps, {}, {}
        current.literal_uses = []
        current.gp = struct.unpack_from('<I', obj.bytes(obj.sections[obj.index['.reginfo']]), 20)[0]
        address = maps[name][0]
        expected = bytearray(obj.bytes(section))
        checked = []
        for relocation in source_relas(obj, index):
            place, kind = relocation['offset'], relocation['kind']
            target, resolved = current.resolve(relocation['symbol'], relocation['addend'], kind, address + place)
            if kind == 4:
                mask, field = 0x03ffffff, (target >> 2) & 0x03ffffff
            elif kind == 5:
                mask, field = 65535, ((target + 0x8000) >> 16) & 65535
            elif kind == 6:
                mask, field = 65535, target & 65535
            elif kind == 7:
                delta = target - current.symbols['_gp']['value']
                if not -32768 <= delta <= 32767:
                    raise SystemExit('SDK exact GP field outside signed16')
                mask, field = 65535, delta & 65535
            else:
                raise SystemExit('Unhandled actual SDK code RELA kind')
            word = struct.unpack_from('<I', expected, place)[0]
            if word & mask:
                raise SystemExit('SDK encoded explicit RELA field is nonzero')
            struct.pack_into('<I', expected, place, word | field)
            checked.append({'offset': place, 'kind': kind, 'exact_symbol': relocation['symbol']['name'],
                            'explicit_addend': relocation['addend'], 'resolved_symbol': resolved, 'resolved_address': target})
        if bytes(expected) != output_bytes(value.elf, address, len(expected)):
            raise SystemExit('Complete actual SDK memory resolved text/padding differs ' + function_name)
        final = value.symbols[function_name]
        if final['value'] != address + symbol['value'] or final['size'] != symbol['size']:
            raise SystemExit('SDK actual final symbol extent differs')
        metadata = frame(obj, function_name)
        results[function_name] = {'archive': archive, 'archive_sha256': sha(Path(archive)), 'member': member,
            'extracted_object': str(path), 'object_sha256': sha(path), 'input_text_section': name,
            'complete_bytes': len(expected), 'exact_symbol_bytes': symbol['size'], 'linked_address': address,
            'ordered_RELA_records': checked, 'resolved_sha256': digest(expected), 'all_resolved_bytes_and_padding_equal': True,
            'exact_input_frame': metadata, 'frame_instruction_witness': encoded_frame(expected, metadata),
            'referenced_data': list(current.data.values()), 'complete_literals': list(current.literals.values())}
    return results


def diagnostic_copies(value, folder, checked):
    report_path = folder / 'patch-report.json'
    report = json.loads(report_path.read_text())
    if report['original_inputs']['elf']['sha256'] != digest(value.elf.data) or not report['diagnostic_only']:
        raise SystemExit('Diagnostic original exact ELF differs')
    result = {}
    initializer, seeder = value.symbols['M_RandomSeedFromOS'], value.symbols['M_RandomSeed']
    section = value.elf.sections[initializer['section']]
    start = section[4] + initializer['value'] - section[3]
    size = initializer['size']
    for name, variant in report['variants'].items():
        path = Path(variant['ELF'])
        raw = path.read_bytes()
        if sha(path) != variant['ELF_sha256']:
            raise SystemExit('Exact diagnostic copy changed')
        expected = struct.pack('<10I', *stub_words(variant['seed_uint32'], seeder['value']))
        verify_patch(value.elf.data, raw, start, size, expected)
        parsed = Elf32(path)
        if linked_symbols(parsed) != value.symbols:
            raise SystemExit('Diagnostic symbol address/size changed')
        bound = {}
        for function_name, body in checked.items():
            address = value.maps['.text.' + function_name][0]
            if address < initializer['value'] + size and initializer['value'] < address + body['complete_text_bytes']:
                raise SystemExit('PB body overlaps diagnostic-only initializer')
            actual = output_bytes(parsed, address, body['complete_text_bytes'])
            if digest(actual) != body['resolved_text_sha256']:
                raise SystemExit('Resolved bound PB/caller body changed in executed diagnostic copy')
            bound[function_name] = {'complete_bytes': len(actual), 'resolved_sha256': digest(actual)}
        result[name] = {'path': str(path), 'sha256': sha(path), 'seed': variant['seed_hex'],
            'initializer_file_offset': start, 'only_authorized_initializer_symbol_bytes': size,
            'every_other_file_byte_identical': True, 'all_original_symbol_addresses_sizes_identical': True,
            'all_resolved_bound_body_spans_identical': bound, 'patch_report_sha256': sha(report_path)}
    return result


def instruction_inventory(before, after, inventory):
    result = {}
    for section in inventory['changed_code_sections_or_bindings']:
        index_a, index_b = before.obj.index[section], after.obj.index[section]
        bytes_equal = before.obj.bytes(before.obj.sections[index_a]) == after.obj.bytes(after.obj.sections[index_b])
        def semantic(value, index):
            rows = []
            for record in source_relas(value.obj, index):
                symbol = dict(record['symbol'])
                symbol.pop('size')  # classification only; all original sizes remain in the binding records.
                if symbol['section'] < len(value.obj.names):
                    symbol['section_name'] = value.obj.names[symbol['section']]
                symbol.pop('section')
                rows.append({'offset': record['offset'], 'kind': record['kind'],
                             'explicit_addend': record['addend'], 'symbol': symbol})
            return rows
        relas_equal = semantic(before, index_a) == semantic(after, index_b)
        result[section] = {'complete_input_instruction_and_padding_bytes_equal': bytes_equal,
            'ordered_input_RELA_semantics_except_target_size_descriptor_equal': relas_equal,
            'pure_target_size_descriptor_change': bytes_equal and relas_equal,
            'input_instruction_bytes_changed': not bytes_equal,
            'baseline_input_bytes': before.obj.sections[index_a][5], 'candidate_input_bytes': after.obj.sections[index_b][5],
            'baseline_linked': bool(before.maps.get(section, (0, 0))[0]),
            'candidate_linked': bool(after.maps.get(section, (0, 0))[0])}
    return result


def run_variant(tag, build, linker, inventory, out):
    value = binder(build, linker)
    mandatory, names = selected(inventory, value.obj)
    checked, discarded = {}, []
    for section in sorted(names):
        name = section[6:]
        if section not in value.maps or not value.maps[section][0] or name not in value.symbols:
            if (section in value.maps and value.maps[section][0]) or name in value.symbols:
                raise SystemExit('A live body has inconsistent exact map/symbol metadata ' + section)
            index = value.obj.index[section]
            discarded.append({'section': section, 'map_contribution': value.maps.get(section),
                'inventoried_root': section in mandatory,
                'complete_input_body_sha256': digest(value.obj.bytes(value.obj.sections[index])),
                'complete_input_frame': frame(value.obj, name),
                'ordered_input_relocations': [{key: record[key] for key in ('offset', 'kind', 'symbol', 'addend')}
                                               for record in source_relas(value.obj, index)],
                'reason': 'Linker-discarded out-of-line wrapper/helper; actual inlined/part bodies and their calling parents are bound, but no nonexistent runtime bytes are counted'})
            continue
        checked[name] = value.text(name)
    sdk_out = out / (tag + '-sdk')
    sdk_out.mkdir()
    sdk = sdk_memory(value, build, sdk_out)
    result = {'build': str(build), 'build_report_sha256': sha(build / 'build-report.json'),
        'engine_sha256': digest(value.obj.data), 'ELF_sha256': digest(value.elf.data), 'map_sha256': sha(build / 'SRB2.map'),
        'required_changed_and_direct_caller_union': sorted(mandatory), 'complete_bound_text': checked,
        'discarded_extra_helpers': discarded, 'complete_referenced_data': value.data,
        'complete_literals': list(value.literals.values()), 'every_literal_RELA_use_and_exact_anchor': value.literal_uses,
        'actual_memory_wrappers': sorted(value.wrappers),
        'actual_reached_ABI_boundaries': boundary_records(value, checked), 'actual_SDK_memory_bodies': sdk,
        'shipping_ELF_has_PDR_section': '.pdr' in value.elf.index,
        'literal_selection_rule': 'Every merged literal uses its exact retained final linker RELA; complete bytes and required alignment are checked at that exact address. Identical bytes elsewhere cannot substitute.',
        'all_full_resolved_text_data_initializers_padding_and_capacity_checks_passed': True}
    return value, result


def binary_controls(out, original, positive):
    """Mutate real binary payloads; refreshed outer pins pass before inner binding."""
    directory = out / 'binary-negatives'
    directory.mkdir()
    results = []
    primary = 'HWR_RenderBatches'

    def clone(obj=None, elf=None):
        current = copy.copy(original)
        current.obj = obj or original.obj
        current.elf = elf or original.elf
        current.symbols = linked_symbols(current.elf)
        current.data, current.literals = {}, {}
        current.literal_uses = []
        return current

    def reject(title, raw, suffix, callback):
        path = directory / (title + suffix)
        path.write_bytes(raw)
        parsed = Elf32(path)
        current = clone(obj=parsed) if suffix == '.o' else clone(elf=parsed)
        refreshed = {'engine_sha256': digest(current.obj.data), 'ELF_sha256': digest(current.elf.data)}
        # These admissions intentionally accept the new artifact hashes. The
        # unchanged authoritative map/RELA anchors then test the actual body,
        # initializer, literal target, frame or capacity, rather than an outer pin.
        if digest(current.obj.data) != refreshed['engine_sha256'] or digest(current.elf.data) != refreshed['ELF_sha256']:
            raise SystemExit('Negative outer-pin refresh did not admit its exact mutation')
        try:
            callback(current)
        except SystemExit as error:
            reason = str(error)
            if any(word in reason.lower() for word in ('sha256', 'checksum', 'hash pin', 'build/elf pin')):
                raise SystemExit('Mutation rejected only by outer metadata: ' + title)
            results.append({'mutation': title, 'path': str(path), 'sha256': sha(path),
                'refreshed_outer_pins': refreshed, 'outer_pin_admission_passed': True,
                'inner_resolved_binding_rejected': True, 'reason': reason})
        else:
            raise SystemExit('Actual binary mutation escaped inner binding: ' + title)

    def linked_offset(address, size=1):
        matches = [section for section in original.elf.sections if section[1] != 8 and section[2] & 2
                   and section[3] <= address and address + size <= section[3] + section[5]]
        if len(matches) != 1:
            raise SystemExit('One exact linked mutation span required')
        return matches[0][4] + address - matches[0][3]

    section = original.obj.sections[original.obj.index['.text.' + primary]]
    raw = bytearray(original.obj.data)
    struct.pack_into('<I', raw, section[4], struct.unpack_from('<I', raw, section[4])[0] ^ 0x10000)
    reject('object_instruction_register', raw, '.o', lambda current: current.text(primary))
    raw = bytearray(original.elf.data)
    at = linked_offset(original.maps['.text.' + primary][0], 4)
    struct.pack_into('<I', raw, at, struct.unpack_from('<I', raw, at)[0] ^ 0x10000)
    reject('linked_instruction_register', raw, '.ELF', lambda current: current.text(primary))
    call = next(record for record in relocations(original.obj, primary) if record['kind'] == 4)
    raw = bytearray(original.obj.data)
    struct.pack_into('<i', raw, call['record_file_offset'] + 8, call['addend'] + 4)
    reject('object_call_RELA_addend', raw, '.o', lambda current: current.text(primary))
    raw = bytearray(original.obj.data)
    symbols = symbol_tables(original.obj)[call['symbol_table']]
    replacement = next(i for i, symbol in enumerate(symbols) if symbol['name'] == 'Z_Free')
    at = call['record_file_offset'] + 4
    info = struct.unpack_from('<I', raw, at)[0]
    struct.pack_into('<I', raw, at, (replacement << 8) | (info & 255))
    reject('object_call_RELA_target', raw, '.o', lambda current: current.text(primary))
    raw = bytearray(original.obj.data)
    at = call['record_file_offset'] + 4
    info = struct.unpack_from('<I', raw, at)[0]
    struct.pack_into('<I', raw, at, (info & ~255) | 6)
    reject('object_call_RELA_type', raw, '.o', lambda current: current.text(primary))
    raw = bytearray(original.obj.data)
    struct.pack_into('<I', raw, call['record_file_offset'], call['offset'] + 4)
    reject('object_call_RELA_location', raw, '.o', lambda current: current.text(primary))
    raw = bytearray(original.elf.data)
    at = linked_offset(original.maps['.text.' + primary][0] + call['offset'], 4)
    struct.pack_into('<I', raw, at, struct.unpack_from('<I', raw, at)[0] ^ 1)
    reject('linked_resolved_call_target', raw, '.ELF', lambda current: current.text(primary))

    data_name = '.data.cv_glshaders'
    if data_name not in original.data:
        raise SystemExit('Actual referenced shader initializer is required for mutation coverage')
    data_index = original.obj.index[data_name]
    data_section = original.obj.sections[data_index]
    data_raw = original.obj.bytes(data_section)
    data_relas = source_relas(original.obj, data_index)
    encoded = {i for relocation in data_relas for i in range(relocation['offset'], relocation['offset'] + 4)}
    byte = next(i for i, value in enumerate(data_raw) if value and i not in encoded)
    raw = bytearray(original.obj.data)
    raw[data_section[4] + byte] ^= 1
    reject('object_initialized_data_value', raw, '.o', lambda current: current.bind_data(data_index))
    raw = bytearray(original.elf.data)
    raw[linked_offset(original.maps[data_name][0] + byte)] ^= 1
    reject('linked_initialized_data_value', raw, '.ELF', lambda current: current.bind_data(data_index))
    candidates = []
    for name, entry in original.data.items():
        index = original.obj.index[name]
        for relocation in source_relas(original.obj, index):
            target = relocation['symbol']['section']
            if target and target < len(original.obj.sections) and original.obj.sections[target][2] & 4:
                candidates.append((index, relocation))
    if not candidates:
        raise SystemExit('A real data-to-function initializer RELA is required')
    index, relocation = candidates[0]
    raw = bytearray(original.obj.data)
    struct.pack_into('<i', raw, relocation['record_file_offset'] + 8, relocation['addend'] + 4)
    reject('object_data_pointer_RELA_addend', raw, '.o', lambda current: current.bind_data(index))
    raw = bytearray(original.obj.data)
    at = relocation['record_file_offset'] + 4
    table = symbol_tables(original.obj)[relocation['symbol_table']]
    other = next(i for i, symbol in enumerate(table) if symbol['name'] == 'Z_Free')
    info = struct.unpack_from('<I', raw, at)[0]
    struct.pack_into('<I', raw, at, (other << 8) | (info & 255))
    reject('object_data_pointer_RELA_target', raw, '.o', lambda current: current.bind_data(index))

    # The real shipping link discards .pdr. Altering its input frame metadata
    # must still fail the exact linked-body stack/save witnesses.
    pdr_index = original.obj.index['.pdr']
    pdr = original.obj.sections[pdr_index]
    sites = [relocation['offset'] for relocation in source_relas(original.obj, pdr_index)
             if relocation['symbol']['name'] in (primary, '.text.' + primary)]
    if len(sites) != 1:
        raise SystemExit('One exact PDR mutation record required')
    for title, field, bit in (('object_frame_size', 20, 16), ('object_integer_save_offset', 8, 8)):
        raw = bytearray(original.obj.data)
        at = pdr[4] + sites[0] + field
        struct.pack_into('<I', raw, at, struct.unpack_from('<I', raw, at)[0] ^ bit)
        reject(title, raw, '.o', lambda current: current.text(primary))

    bss_name = next(name for name, entry in original.data.items() if 'pb_plan' in name and entry.get('zero_initialized_capacity'))
    bss_index = original.obj.index[bss_name]
    bss = original.obj.sections[bss_index]
    raw = bytearray(original.obj.data)
    header = struct.unpack_from('<16sHHIIIIIHHHHHH', raw)
    struct.pack_into('<I', raw, header[6] + bss_index * header[11] + 20, bss[5] + 16)
    reject('object_BSS_capacity', raw, '.o', lambda current: current.bind_data(bss_index))
    address = original.maps[bss_name][0]
    final_index = next(i for i, section in enumerate(original.elf.sections)
                       if section[1] == 8 and section[2] & 2 and section[3] <= address < section[3] + section[5])
    final = original.elf.sections[final_index]
    raw = bytearray(original.elf.data)
    header = struct.unpack_from('<16sHHIIIIIHHHHHH', raw)
    struct.pack_into('<I', raw, header[6] + final_index * header[11] + 20, address - final[3] + bss[5] - 1)
    reject('linked_BSS_capacity', raw, '.ELF', lambda current: current.bind_data(bss_index))

    literal_sites = []
    for name, entry in original.data.items():
        for record in entry['ordered_RELA_records']:
            for literal in original.literals.values():
                if literal['linked_address'] == record['resolved_address']:
                    text = bytes.fromhex(literal['complete_literal_hex'])
                    alternatives = []
                    for i, final in enumerate(original.elf.sections):
                        if not original.elf.names[i].startswith('.rodata') or not final[2] & 2:
                            continue
                        content = original.elf.bytes(final)
                        at = content.find(text)
                        while at >= 0:
                            address = final[3] + at
                            if address != literal['linked_address'] and (address - literal['offset']) % literal['input_alignment'] == 0:
                                alternatives.append(address)
                            at = content.find(text, at + 1)
                    if alternatives:
                        literal_sites.append((name, record, literal, alternatives))
    if not literal_sites:
        raise SystemExit('An ambiguous complete aligned literal-pointer mutation is required')
    nonempty = [row for row in literal_sites if len(bytes.fromhex(row[2]['complete_literal_hex'])) > 1]
    zero = [row for row in nonempty if bytes.fromhex(row[2]['complete_literal_hex']) == b'0\0']
    if not nonempty:
        raise SystemExit('A real nonempty ambiguous complete literal pointer is required')
    name, record, literal, alternatives = min(zero or nonempty, key=lambda row: len(row[2]['complete_literal_hex']))
    index = original.obj.index[name]
    raw = bytearray(original.elf.data)
    at = linked_offset(original.maps[name][0] + record['offset'], 4)
    struct.pack_into('<I', raw, at, alternatives[0])
    reject('linked_pointer_to_identical_other_literal', raw, '.ELF', lambda current: current.bind_data(index))
    results[-1]['literal_pointer_evidence'] = {'complete_literal_hex': literal['complete_literal_hex'],
        'authoritative_linker_selected_address': literal['linked_address'], 'incorrect_identical_aligned_address': alternatives[0],
        'exact_retained_linker_anchor': literal['retained_exact_linker_RELA_anchor']}
    literal_index = original.obj.index[literal['input_section']]
    raw = bytearray(original.obj.data)
    raw[original.obj.sections[literal_index][4] + literal['offset']] ^= 1
    reject('object_complete_literal_byte', raw, '.o', lambda current: current.bind_data(index))
    raw = bytearray(original.elf.data)
    raw[linked_offset(literal['linked_address'])] ^= 1
    reject('linked_complete_literal_byte', raw, '.ELF', lambda current: current.bind_data(index))
    # This directly exercises the admitted exact --wrap semantics beneath the
    # outer build report; the real word must target the wrapper, not newlib.
    current = clone()
    current.wrappers = set(current.wrappers) - {'memcpy'}
    try:
        current.text('__wrap_memcpy')
    except SystemExit as error:
        results.append({'mutation': 'admitted_memcpy_wrapper_route', 'outer_link_wrapper_contract_refreshed': True,
                        'inner_resolved_binding_rejected': True, 'reason': str(error)})
    else:
        raise SystemExit('Actual memory wrapper route mutation escaped inner binding')
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-build', type=Path, default=ROOT / 'build/nov15-next12/baseline/hardware')
    parser.add_argument('--candidate-build', type=Path, default=WORK / 'hardware')
    parser.add_argument('--baseline-linker-relas', type=Path, default=WORK / 'resolved-binding/baseline-linker-relas')
    parser.add_argument('--candidate-linker-relas', type=Path, default=WORK / 'resolved-binding/candidate-linker-relas')
    parser.add_argument('--object-report', type=Path, default=WORK / 'objectcheck/report.json')
    parser.add_argument('--out', type=Path, default=WORK / 'resolved-binding/codecheck')
    parser.add_argument('--negative-controls', action='store_true')
    parser.add_argument('--verify-root', action='store_true', help='Explicitly verify only the integrated PB source hash; defaults use immutable snapshots/contexts')
    args = parser.parse_args()
    out = args.out.resolve()
    if not out.is_relative_to(ROOT / 'build') or out == ROOT / 'build' or out.exists():
        raise SystemExit('A fresh child of build/ is required; preserve older evidence')
    out.mkdir(parents=True)
    inventory = json.loads(args.object_report.read_text())
    if inventory['candidate_source_sha256'] != replay.CANDIDATE_SHA:
        raise SystemExit('Frozen PB source pin differs')
    if args.verify_root and sha(ROOT / 'src/hardware/hw_pbatch.inc') != replay.CANDIDATE_SHA:
        raise SystemExit('Explicit integrated PB source verification differs')
    admissions = {}
    for tag, build in (('baseline', args.baseline_build.resolve()), ('candidate', args.candidate_build.resolve())):
        admissions[tag] = {}
        for name in ('obj/engine_lto.o', 'SRB2.ELF', 'SRB2.map', 'build-report.json'):
            expected = inventory['artifacts'][tag][name]['sha256']
            actual = sha(build / name)
            if actual != expected:
                raise SystemExit('Actual component artifact differs from the explicit frozen inventory: ' + tag + '/' + name)
            admissions[tag][name] = {'path': str(build / name), 'exact_frozen_sha256': actual}
    report = {'kind': __doc__, 'checker_sha256': sha(Path(__file__)),
        'candidate_source_sha256': replay.CANDIDATE_SHA, 'object_inventory_sha256': sha(args.object_report),
        'exact_frozen_component_artifact_admission': admissions,
        'scope': 'All17 code-or-binding sections plus all42 complete direct callers (45-section union): every live complete body is reconstructed, and linker-discarded out-of-line wrappers are explicitly retained as object-only. Additional emitted PB/planner/driver consumers, actual memory wrappers/zone entry points, all referenced engine data/literal initializers/capacities and reached explicit ABI boundaries are bound. Three actual SDK memory bodies are separately reconstructed.',
        'helper_sha256': {name: sha(ROOT / 'tools/ps2' / name) for name in ('next11_sw_checkplane_codecheck.py',
            'next11_sw_linker_relas.py', 'next10_sw_bounds_codecheck.py', 'next7_render_fog_codecheck.py',
            'next8_sw_codecheck.py', 'next11_local_rng.py', 'next12_pb_scratch_replay.py')}, 'variants': {}}
    binders = {}
    for tag, build, linker in (('baseline', args.baseline_build.resolve(), args.baseline_linker_relas.resolve()),
                               ('candidate', args.candidate_build.resolve(), args.candidate_linker_relas.resolve())):
        value, result = run_variant(tag, build, linker, inventory, out)
        binders[tag] = value
        result['actual_RNG_diagnostic_copies'] = diagnostic_copies(value, WORK / 'local-rng' / tag, result['complete_bound_text'])
        report['variants'][tag] = result
    report['changed_instruction_vs_target_size_descriptor_inventory'] = instruction_inventory(binders['baseline'], binders['candidate'], inventory)
    common = set(report['variants']['baseline']['complete_bound_text']) & set(report['variants']['candidate']['complete_bound_text'])
    report['exact_complete_input_frame_and_save_metadata_equal'] = all(
        report['variants']['baseline']['complete_bound_text'][name]['exact_frame'] ==
        report['variants']['candidate']['complete_bound_text'][name]['exact_frame'] for name in common)
    if not report['exact_complete_input_frame_and_save_metadata_equal']:
        raise SystemExit('Actual component frame/save metadata changed')
    if args.negative_controls:
        report['binary_negative_controls'] = binary_controls(out, binders['candidate'], report['variants']['candidate'])
    report['status'] = 'PASS complete actual resolved code/data/literal/capacity/frame binding; no new execution or FPS claim'
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'status': report['status'], 'report': str(out / 'report.json'),
        'bound_functions': {tag: len(result['complete_bound_text']) for tag, result in report['variants'].items()},
        'negative_controls': len(report.get('binary_negative_controls', []))}, indent=2))


if __name__ == '__main__':
    main()
