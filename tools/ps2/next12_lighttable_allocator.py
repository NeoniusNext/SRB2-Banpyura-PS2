"""Resolve the actual libc malloc and PS2 spill allocator graph into the game ELF.

Extracts exact SDK archive members, retaining all code bytes, RELAs and PDR
records. No compiler, linker or emulator is invoked.
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
from next10_sw_bounds_codecheck import output_bytes, symbol_tables
from next11_sw_checkplane_codecheck import Binder, build_binder

ROOT = Path(__file__).resolve().parents[2]


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def member(archive, wanted):
    raw = archive.read_bytes()
    if raw[:8] != b'!<arch>\n': raise SystemExit('Expected ordinary exact SDK archive')
    at = 8; names = b''
    while at < len(raw):
        header = raw[at:at + 60]
        if header[58:] != b'`\n': raise SystemExit('Invalid exact archive header')
        size = int(header[48:58]); name = header[:16].decode().rstrip(); data = raw[at + 60:at + 60 + size]
        if name == '//': names = data
        elif re.fullmatch(r'/\d+', name): name = names[int(name[1:]):].split(b'/\n', 1)[0].decode()
        else: name = name.rstrip('/')
        if name == wanted: return data
        at += 60 + size + (size & 1)
    raise SystemExit('Exact SDK archive member missing: ' + wanted)


class MemberBytes:
    def __init__(self, data, name): self.data = data; self.name = name
    def read_bytes(self): return self.data
    def __str__(self): return '<authoritative archive member ' + self.name + '>'


def member_records(obj):
    """Independent exact original instructions/symbols/ordered-RELA identity.

    A HI16 low-bit addend can round to identical emitted bits. Preserve the
    original full addend separately rather than pretending those bits expose it.
    """
    tables = symbol_tables(obj); sections = {}; records = []
    for index, (name, section) in enumerate(zip(obj.names, obj.sections)):
        if section[2] & 2 or name == '.pdr':
            sections[name] = (section[1], section[2], section[5], section[8],
                              None if section[1] == 8 else obj.bytes(section).hex())
        if section[1] not in (4, 9): continue
        if not (obj.sections[section[7]][2] & 2 or obj.names[section[7]] == '.pdr'): continue
        if section[1] != 4: raise SystemExit('Explicit authoritative SDK RELAs required')
        for at in range(section[4], section[4] + section[5], section[9]):
            offset, info, addend = struct.unpack_from('<IIi', obj.data, at)
            records.append((obj.names[section[7]], offset, info & 255, tables[section[6]][info >> 8], addend))
    return {'sections': sections, 'complete_symbol_tables': tables, 'ordered_explicit_RELAs': records}


class SDK(Binder):
    def __init__(self, obj, game, archive_spelling, member_name):
        super().__init__(obj, game.elf, game.map_path, game.link_command)
        self.maps = {}
        owner = re.escape(archive_spelling + '(' + member_name + ')')
        pattern = r'^\s+(\.[^\s]+)\s+(?:\n\s+)?0x([a-f0-9]+)\s+0x([a-f0-9]+)\s+' + owner + r'\s*$'
        for name, address, size in re.findall(pattern, game.map_path.read_text(), re.M):
            address, size = int(address, 16), int(size, 16)
            if not address or not size: continue
            if name in self.maps: raise SystemExit('Ambiguous SDK member input contribution')
            self.maps[name] = (address, size)
        self.linker_relas = game.linker_relas; self.linker_rela_evidence = game.linker_rela_evidence
        self.authoritative = Elf32(MemberBytes(member(Path(archive_spelling), member_name), member_name))

    def section_text(self, function):
        if member_records(self.obj) != member_records(self.authoritative):
            raise SystemExit('Complete SDK instructions/symbols/PDR/ordered RELA records differ from independently extracted authoritative member')
        matches = [s for table in symbol_tables(self.obj).values() for s in table if s['name'] == function and s['type'] == 2 and s['section']]
        if len(matches) != 1: raise SystemExit('One exact SDK function required')
        symbol = matches[0]; index = symbol['section']; section = self.obj.sections[index]
        raw = self.obj.bytes(section); resolved = bytearray(raw); address = self.mapped(index); records = []
        for rela in self.obj.sections:
            if rela[7] != index or rela[1] not in (4, 9): continue
            if rela[1] != 4: raise SystemExit('Explicit SDK code RELAs required')
            table = symbol_tables(self.obj)[rela[6]]
            for at in range(rela[4], rela[4] + rela[5], rela[9]):
                offset, info, addend = struct.unpack_from('<IIi', self.obj.data, at); kind = info & 255; target = table[info >> 8]
                value, name = self.resolve(target, addend, kind, address + offset)
                if kind == 4: mask = 0x03ffffff; field = (value >> 2) & mask
                elif kind == 5: mask = 0xffff; field = ((value + 0x8000) >> 16) & mask
                elif kind == 6: mask = 0xffff; field = value & mask
                elif kind == 7:
                    mask = 0xffff; displacement = value - self.symbols['_gp']['value']
                    if not -32768 <= displacement <= 32767: raise SystemExit('SDK exact GP displacement outside signed16')
                    field = displacement & mask
                else: raise SystemExit('Unexpected SDK instruction RELA kind')
                word = struct.unpack_from('<I', resolved, offset)[0]
                if word & mask: raise SystemExit('Nonzero encoded explicit SDK RELA field')
                struct.pack_into('<I', resolved, offset, word | field)
                records.append({'offset': offset, 'kind': kind, 'exact_original_symbol': target['name'],
                                'explicit_addend': addend, 'actual_wrapped_symbol': name, 'resolved_address': value})
        if output_bytes(self.elf, address, len(resolved)) != resolved:
            raise SystemExit('Fully resolved entire SDK input text/padding differs')
        linked = self.symbols[function]
        if linked['value'] != address + symbol['value'] or linked['size'] != symbol['size']:
            raise SystemExit('SDK exact function extent differs')
        pdr_index = self.obj.index['.pdr']; pdr = self.obj.sections[pdr_index]; frame_records = []
        for rela in self.obj.sections:
            if rela[7] != pdr_index or rela[1] not in (4, 9): continue
            if rela[1] != 4: raise SystemExit('Explicit complete SDK PDR RELAs required')
            table = symbol_tables(self.obj)[rela[6]]
            for at in range(rela[4], rela[4] + rela[5], rela[9]):
                offset, info, addend = struct.unpack_from('<IIi', self.obj.data, at); target = table[info >> 8]
                if info & 255 != 2 or offset % 32: raise SystemExit('Unexpected complete SDK PDR RELA')
                value, name = self.resolve(target, addend, 2)
                frame_records.append({'offset': offset, 'target': name, 'addend': addend, 'resolved_function_address': value,
                                      'complete_frame_record_hex': self.obj.bytes(pdr)[offset:offset + 32].hex()})
        return {'function': function, 'input_section': self.obj.names[index], 'entire_input_section_bytes': len(raw),
                'exact_function_bytes': symbol['size'], 'linked_address': linked['value'],
                'all_resolved_text_padding_and_function_extents_equal': True,
                'complete_input_text_sha256': hashlib.sha256(raw).hexdigest(), 'resolved_text_sha256': hashlib.sha256(resolved).hexdigest(),
                'complete_original_symbols_and_ordered_RELA_addends_bound_to_authoritative_archive_member': True,
                'complete_ordered_exact_target_RELA_records': records, 'complete_original_PDR_records': frame_records,
                'complete_referenced_data': list(self.data.values()), 'exact_literals': list(self.literals.values())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True); parser.add_argument('--linker', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True); parser.add_argument('--negative-controls', action='store_true'); args = parser.parse_args()
    build, out = args.build.resolve(), args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'): raise SystemExit('Fresh immutable allocator binding required')
    out.mkdir(parents=True); pinned = out / 'pinned-tools'; pinned.mkdir()
    for name in ('next12_lighttable_allocator.py', 'next11_sw_checkplane_codecheck.py', 'next11_sw_linker_relas.py', 'next10_sw_bounds_codecheck.py'):
        shutil.copyfile(ROOT / 'tools/ps2' / name, pinned / name)
    game = build_binder(build / 'obj/engine_lto.o', build / 'SRB2.ELF', build / 'SRB2.map', args.linker.resolve())
    engine_roots = ('__wrap__malloc_r', 'Spill_Alloc', 'Z_TryMallocAlign', 'Z_MallocInternal.constprop.0')
    roots = {name: game.text(name) for name in engine_roots}
    map_text = game.map_path.read_text(); sdk = {}
    for function, wanted in (('malloc', 'libc_a-malloc.o'), ('_malloc_r', 'libc_a-mallocr.o')):
        archives = set(re.findall(r'(/[^\s()]+\.a)\(' + re.escape(wanted) + r'\)', map_text))
        if len(archives) != 1: raise SystemExit('One exact native libc archive/member required')
        spelling = archives.pop(); archive = Path(spelling); path = out / wanted; path.write_bytes(member(archive, wanted))
        admitted = json.loads((args.linker.resolve() / 'linker-rela-report.json').read_text())['all_pinned_linker_inputs_unchanged']
        if admitted.get(spelling) != sha(archive): raise SystemExit('SDK archive differs from independently pinned exact link input')
        sdk[function] = {'archive': spelling, 'archive_sha256': sha(archive), 'member': wanted, 'member_sha256': sha(path),
                         'binding': SDK(Elf32(path), game, spelling, wanted).section_text(function)}
    calls = sdk['malloc']['binding']['complete_ordered_exact_target_RELA_records']
    if sum(r['kind'] == 4 and r['actual_wrapped_symbol'] == '__wrap__malloc_r' for r in calls) != 1:
        raise SystemExit('Actual libc malloc does not dispatch exactly to the shipping spill wrapper')
    report = {'actual_linked_allocator_graph': 'lt_store -> libc malloc -> __wrap__malloc_r -> Spill_Alloc -> nonfatal Z_MallocInternal (Z_TryMallocAlign inlined)',
              'not_the_optional_PS2_LEAKTRACE_allocator': True, 'actual_complete_engine_function_bindings': roots,
              'actual_complete_SDK_member_bindings': sdk, 'engine_sha256': sha(game.obj.path), 'ELF_sha256': sha(game.elf.path),
              'map_sha256': sha(game.map_path), 'pinned_tool_sha256': {p.name: sha(p) for p in pinned.iterdir()}}
    if args.negative_controls:
        checked = []
        admission = sdk['malloc']; path = out / admission['member']; original = Elf32(path)
        def reject(title, raw, suffix, callback):
            altered = out / ('negative-' + title + suffix); altered.write_bytes(raw)
            try: callback(Elf32(altered))
            except SystemExit as error: checked.append({'mutation': title, 'rejected': True, 'reason': str(error), 'sha256': sha(altered)})
            else: raise SystemExit('Actual allocator mutation escaped gate: ' + title)
        for field in ('HI16_low_addend', 'symbol_size', 'frame'):
            raw = bytearray(original.data)
            if field == 'HI16_low_addend':
                sites = []
                for rela in original.sections:
                    if rela[1] != 4 or not original.names[rela[7]].startswith('.text'): continue
                    for at in range(rela[4], rela[4] + rela[5], rela[9]):
                        if struct.unpack_from('<I', raw, at + 4)[0] & 255 == 5: sites.append(at + 8)
                if not sites: raise SystemExit('One actual SDK HI16 addend required')
                at = sites[0]; struct.pack_into('<i', raw, at, struct.unpack_from('<i', raw, at)[0] + 4)
            elif field == 'frame':
                section = original.sections[original.index['.pdr']]; at = section[4] + 20
                struct.pack_into('<I', raw, at, struct.unpack_from('<I', raw, at)[0] ^ 16)
            else:
                sites = []
                for section in original.sections:
                    if section[1] != 2: continue
                    strings = original.bytes(original.sections[section[6]])
                    for at in range(section[4], section[4] + section[5], section[9]):
                        name = strings[struct.unpack_from('<I', raw, at)[0]:].split(b'\0', 1)[0]
                        if name == b'malloc': sites.append(at + 8)
                if len(sites) != 1: raise SystemExit('One actual SDK function-size field required')
                at = sites[0]; struct.pack_into('<I', raw, at, struct.unpack_from('<I', raw, at)[0] + 4)
            reject(field, raw, '.o', lambda obj: SDK(obj, game, admission['archive'], admission['member']).section_text('malloc'))
        for field, function in (('linked_malloc_call', 'malloc'), ('linked_spill_wrapper_register', '__wrap__malloc_r')):
            raw = bytearray(game.elf.data)
            if function == 'malloc':
                call = next(r for r in calls if r['kind'] == 4 and r['actual_wrapped_symbol'] == '__wrap__malloc_r')
                address = sdk['malloc']['binding']['linked_address'] + call['offset']; bit = 1
            else: address = game.maps['.text.' + function][0]; bit = 0x10000
            section = next(s for s in game.elf.sections if s[1] != 8 and s[3] <= address < s[3] + s[5]); at = section[4] + address - section[3]
            struct.pack_into('<I', raw, at, struct.unpack_from('<I', raw, at)[0] ^ bit)
            def check(elf, fn=function):
                changed = copy.copy(game); changed.elf = elf; changed.data = {}; changed.literals = {}
                if fn == 'malloc': return SDK(original, changed, admission['archive'], admission['member']).section_text('malloc')
                return changed.text(fn)
            reject(field, raw, '.ELF', check)
        report['executed_actual_allocator_binary_controls'] = checked
        report['HI16_precision'] = 'A low-bit-only HI16 addend change may preserve rounded emitted bits; its rejection is by independent complete authoritative ordered-RELA identity, not an instruction-difference claim.'
    (out / 'allocator-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Bound actual libc malloc and complete shipping PS2 spill allocator graph: ' + str(out))


if __name__ == '__main__': main()
