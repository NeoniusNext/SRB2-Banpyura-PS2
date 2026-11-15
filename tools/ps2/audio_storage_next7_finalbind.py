#!/usr/bin/env python3
"""Read-only binding of the measured music code to final renderer objects.

No compilation or execution. Compare complete selected section contents,
ordered relocations including explicit addends and canonical symbol targets,
referenced data sections, and the specialized 512-frame renderer ABI.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[2]
FINAL_REVISION = '2ec90684af655a9286b097224564083c4a007170'
FINAL_ELFS = {
    'software': ('build/ps2/SRB2.ELF', 'dbc708685926cf5f8368f3a29f5846a1f309bb25b9c5c3f79ab9dbd0cb267767'),
    'hardware': ('build/nov14-next7/hardware/SRB2.ELF', '9258483fa3fc655d035d9084286d339e309b6e6fa6132e5418eb4e16c17505d3'),
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


class Object:
    def __init__(self, path):
        self.path = path
        self.data = path.read_bytes()
        header = struct.unpack_from('<16sHHIIIIIHHHHHH', self.data)
        assert header[0][:7] == b'\x7fELF\x01\x01\x01' and header[2] == 8, 'expected ELF32 little-endian MIPS'
        self.headers = [struct.unpack_from('<10I', self.data, header[6] + i * header[11]) for i in range(header[12])]
        names = self.content(self.headers[header[13]])
        self.names = [self.string(names, section[0]) for section in self.headers]
        self.sections = dict(zip(self.names, self.headers))
        self.relocations = {}
        for index, section in enumerate(self.headers):
            if section[1] not in (4, 9):
                continue
            symbols = self.headers[section[6]]
            strings = self.content(self.headers[symbols[6]])
            records = self.relocations.setdefault(self.names[section[7]], [])
            for offset in range(section[4], section[4] + section[5], section[9]):
                place, info = struct.unpack_from('<II', self.data, offset)
                symbol = struct.unpack_from('<IIIBBH', self.data, symbols[4] + (info >> 8) * symbols[9])
                defining = self.names[symbol[5]] if symbol[5] < len(self.names) else 'SPECIAL:' + str(symbol[5])
                records.append({
                    'relocation_section': self.names[index], 'offset': place, 'type': info & 255,
                    'explicit_addend': struct.unpack_from('<i', self.data, offset + 8)[0] if section[1] == 4 else None,
                    'target': {'name': self.string(strings, symbol[0]), 'section': defining,
                               'value': symbol[1], 'size': symbol[2], 'info': symbol[3], 'other': symbol[4]},
                })

    @staticmethod
    def string(data, offset):
        return data[offset:].split(b'\0', 1)[0].decode()

    def content(self, section):
        return b'' if section[1] == 8 else self.data[section[4]:section[4] + section[5]]

    def signature(self, name):
        section = self.sections[name]
        return {'type': section[1], 'flags': section[2], 'bytes': section[5],
                'alignment': section[8], 'entry_bytes': section[9], 'sha256': sha(self.content(section))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, default=ROOT / 'build/nov14-next7/audio-storage-linked-hardware-final')
    parser.add_argument('--final-software', type=Path, default=ROOT / 'build/ps2/obj/engine_lto.o')
    parser.add_argument('--final-hardware', type=Path, default=ROOT / 'build/nov14-next7/hardware/obj/engine_lto.o')
    args = parser.parse_args()
    work = args.work.resolve()
    assert work.is_relative_to(ROOT / 'build')
    previous = json.loads((work / 'sw-hw-code.json').read_text())['candidate']
    names = list(previous['sections'])
    assert len(names) == 39
    source = ROOT / 'src/ps2/ps2_music.c'
    expected = json.loads((ROOT / 'build/nov14-next7/audio-storage-final-proof.json').read_text())['production_sha256']
    assert sha(source.read_bytes()) == expected
    result = {'kind': 'read-only measured-to-final music object binding', 'final_source_revision': FINAL_REVISION,
              'production_sha256': expected, 'renderers': {}}
    for renderer, measured, final in [
            ('software', work / 'candidate-software-engine.o', args.final_software.resolve()),
            ('hardware', work / 'candidate/engine_original.o', args.final_hardware.resolve())]:
        before, after = Object(measured), Object(final)
        assert sha(before.data) == previous[renderer + '_sha256']
        sections, helpers, referenced_data, other_code, externals = {}, {}, {}, set(), set()
        def targets(relocations):
            for relocation in relocations:
                target = relocation['target']['section']
                if not target:
                    externals.add(relocation['target']['name'])
                elif target in before.sections:
                    if before.sections[target][2] & 4:
                        if target not in names and target not in helpers:
                            other_code.add(target)
                    else:
                        assert before.signature(target) == after.signature(target), renderer + ' referenced data differs: ' + target
                        assert before.relocations.get(target, []) == after.relocations.get(target, []), renderer + ' data relocation differs: ' + target
                        referenced_data[target] = before.signature(target)
        for name in names:
            a, b = before.signature(name), after.signature(name)
            assert a == b, renderer + ' code section differs: ' + name
            old_relocations, new_relocations = before.relocations.get(name, []), after.relocations.get(name, [])
            assert old_relocations == new_relocations, renderer + ' relocation differs: ' + name
            sections[name] = {**a, 'ordered_relocations': old_relocations,
                              'relocation_sha256': sha(json.dumps(old_relocations, sort_keys=True).encode())}
            targets(old_relocations)
        # Audit any directly referenced engine helpers too, closing the defined
        # function graph without assuming that the selected names include them.
        while other_code:
            name = other_code.pop()
            if name in helpers:
                continue
            assert before.signature(name) == after.signature(name), renderer + ' helper differs: ' + name
            old_relocations, new_relocations = before.relocations.get(name, []), after.relocations.get(name, [])
            assert old_relocations == new_relocations, renderer + ' helper relocation differs: ' + name
            helpers[name] = {**before.signature(name), 'ordered_relocations': old_relocations}
            targets(old_relocations)
        entry = after.content(after.sections['.text.PS2_MusicRender.constprop.0'])[:8]
        assert entry == bytes.fromhex('90ffbd2700080624'), '512-frame renderer ABI entry differs'
        elf_path, expected_elf = FINAL_ELFS[renderer]
        elf = ROOT / elf_path
        assert sha(elf.read_bytes()) == expected_elf, renderer + ' final ELF pin differs'
        result['renderers'][renderer] = {
            'measured_object': str(measured.relative_to(ROOT)), 'measured_object_sha256': sha(before.data),
            'final_object': str(final.relative_to(ROOT)), 'final_object_sha256': sha(after.data),
            'final_elf': elf_path, 'final_elf_sha256': expected_elf,
            'identical_code_sections': len(sections),
            'identical_ordered_relocations': sum(len(v['ordered_relocations']) for v in sections.values()),
            'explicit_addends_compared': sum(r['explicit_addend'] is not None for v in sections.values() for r in v['ordered_relocations']),
            'renderer512_entry_bytes': entry.hex(), 'renderer512_two_argument_abi_unchanged': True,
            'sections': sections, 'additional_defined_helpers': helpers,
            'referenced_data': referenced_data, 'external_symbols': sorted(externals),
        }
    result['auditor'] = str(Path(__file__).resolve().relative_to(ROOT))
    result['auditor_sha256'] = sha(Path(__file__).read_bytes())
    output = work / 'final-object-binding.json'
    output.write_text(json.dumps(result, indent=2) + '\n')
    print('Final music binding passed: 39 identical sections and 203 identical relocations/addends per renderer; ABI and referenced data unchanged.')
    print(output.relative_to(ROOT))


if __name__ == '__main__':
    main()
