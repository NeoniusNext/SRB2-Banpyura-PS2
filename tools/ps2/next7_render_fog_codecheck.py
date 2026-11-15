"""Compare complete fixture/native drawer instruction streams after relocations.

Only MIPS relocation fields are normalized, with symbol names and addends
retained. All non-relocation bits (registers, opcodes, constants, relative branch
distances, stack setup) must match exactly. The fixture matches real viddef_t
layout and the complete five-element screens array, including GP eligibility.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
from next7_render_fog_hosttest import NAMES


class Elf32:
    def __init__(self, path):
        self.path = path
        self.data = path.read_bytes()
        h = struct.unpack_from('<16sHHIIIIIHHHHHH', self.data)
        if h[0][:7] != b'\x7fELF\x01\x01\x01':
            raise SystemExit('Expected little-endian ELF32: ' + str(path))
        self.sections = [struct.unpack_from('<IIIIIIIIII', self.data, h[6] + i * h[11]) for i in range(h[12])]
        strings = self.bytes(self.sections[h[13]])
        self.names = [strings[s[0]:].split(b'\0', 1)[0].decode() for s in self.sections]
        self.index = {name: i for i, name in enumerate(self.names)}
        self.symbols = {}
        for i, section in enumerate(self.sections):
            if section[1] != 2:
                continue
            names = self.bytes(self.sections[section[6]])
            symbols = []
            for offset in range(section[4], section[4] + section[5], section[9]):
                sym = struct.unpack_from('<IIIBBH', self.data, offset)
                name = names[sym[0]:].split(b'\0', 1)[0].decode()
                if not name and (sym[3] & 15) == 3:
                    name = self.names[sym[5]]
                symbols.append(name)
            self.symbols[i] = symbols

    def bytes(self, section):
        return self.data[section[4]:section[4] + section[5]]

    def instructions(self, name, production, constant_pool=False):
        index = self.index['.text.' + name]
        code = self.bytes(self.sections[index])
        if len(code) % 4:
            raise SystemExit('Unaligned native text section: ' + name)
        words = list(struct.unpack('<' + 'I' * (len(code) // 4), code))
        relocs = {}
        for section in self.sections:
            if section[1] not in (4, 9) or section[7] != index:
                continue
            for offset in range(section[4], section[4] + section[5], section[9]):
                location, info = struct.unpack_from('<II', self.data, offset)
                explicit_addend = struct.unpack_from('<i', self.data, offset + 8)[0] if section[1] == 4 else None
                kind, symbol = info & 255, self.symbols[section[6]][info >> 8]
                normalized = re.sub(r'\.lto_priv\.\d+$', '', symbol)
                normalized = re.sub(r'^(?:\.(?:text|bss|sbss|sdata)\.)?(?:reference_|candidate_)?', '', normalized)
                word = words[location // 4]
                if kind in (5, 6, 7):
                    addend = word & 65535
                    words[location // 4] &= 0xffff0000
                elif kind == 4:
                    addend = word & 0x03ffffff
                    words[location // 4] &= 0xfc000000
                else:
                    raise SystemExit('Unexpected text relocation ' + str(kind) + ': ' + name)
                if constant_pool and kind == 7 and normalized == '.sdata':
                    # A partial MIPS link biases local-section GPREL addends by
                    # the object's recorded gp. Compare the addressed float
                    # literal bytes, retaining its exact load instruction.
                    if word >> 26 != 49 or explicit_addend is None:
                        raise SystemExit('Unexpected helper literal relocation')
                    gp = struct.unpack_from('<I', self.bytes(self.sections[self.index['.reginfo']]), 20)[0]
                    pool = self.bytes(self.sections[self.index['.sdata']])
                    position = explicit_addend + gp
                    if position < 0 or position + 4 > len(pool):
                        raise SystemExit('Helper literal outside constant pool')
                    normalized = 'float32_bytes:' + pool[position:position + 4].hex()
                    explicit_addend = 0
                relocs[location] = (kind, normalized, addend, explicit_addend)
        if not relocs:
            raise SystemExit('Missing expected function relocations: ' + name)
        # Function sections have zero alignment padding. Keep its exact size,
        # rather than silently ignoring padding or changed instructions.
        return {'words': words, 'relocations': relocs, 'text_bytes': len(code)}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--fixture-object', required=True, type=Path)
    p.add_argument('--baseline-engine', type=Path)
    p.add_argument('--candidate-engine', type=Path)
    a = p.parse_args()
    out = a.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    fixture = Elf32(a.fixture_object.resolve())
    report = {'fixture_object': str(fixture.path), 'fixture_sha256': hashlib.sha256(fixture.data).hexdigest(),
              'normalization': 'Only relocation fields and symbol suffix/prefix spelling; all instruction bits, relocation targets and addends exact',
              'helper_literal_normalization': 'Resolve local .sdata GPREL references with object gp, compare all four literal bytes and retain load instruction',
              'versions': {}}
    failures = []
    for variant, path in [('reference', a.baseline_engine), ('candidate', a.candidate_engine)]:
        if path is None:
            continue
        engine = Elf32(path.resolve())
        data = {'object': str(engine.path), 'object_sha256': hashlib.sha256(engine.data).hexdigest(), 'functions': {}}
        for name in NAMES:
            expected = fixture.instructions(variant + '_' + name, False)
            actual = engine.instructions(name, True)
            equal = expected == actual
            differences = []
            for i, (before, after) in enumerate(zip(expected['words'], actual['words'])):
                if before != after:
                    differences.append({'offset': hex(i * 4), 'fixture_word': hex(before), 'engine_word': hex(after)})
            if expected['relocations'] != actual['relocations']:
                differences.append({'fixture_relocations': expected['relocations'], 'engine_relocations': actual['relocations']})
            data['functions'][name] = {'identical_instruction_and_relocation_stream': equal,
                                       'fixture_text_bytes': expected['text_bytes'], 'engine_text_bytes': actual['text_bytes'],
                                       'relocation_count': len(expected['relocations']),
                                       'rela_entries_with_explicit_addends': sum(v[3] is not None for v in expected['relocations'].values()),
                                       'differences': differences}
            if not equal:
                failures.append(variant + ': ' + name)
        # The real slope-light helper is common to both drawers, but also
        # belongs to each timed call. Verify its complete instructions and
        # exact pooled literal bytes, not merely the call target's spelling.
        helper_names = [n[len('.text.'):] for n in engine.names
                        if re.fullmatch(r'\.text\.R_CalcSlopeLight(?:\.lto_priv\.\d+)?', n)]
        if len(helper_names) != 1:
            raise SystemExit('Expected one actual slope-light helper')
        expected = fixture.instructions('R_CalcSlopeLight', False, True)
        actual = engine.instructions(helper_names[0], True, True)
        equal = expected == actual
        data['lighting_helper'] = {'identical_complete_instructions_and_literal_bytes': equal,
                                   'fixture_text_bytes': expected['text_bytes'], 'engine_text_bytes': actual['text_bytes'],
                                   'relocation_count': len(expected['relocations']),
                                   'literal_bytes': sorted({r[1] for r in expected['relocations'].values()
                                                            if r[1].startswith('float32_bytes:')})}
        if not equal:
            failures.append(variant + ': R_CalcSlopeLight')
        report['versions'][variant] = data
    report['all_compared_drawers_identical_after_documented_relocations'] = not failures
    (out / 'code-report.json').write_text(json.dumps(report, indent=2) + '\n')
    if failures:
        raise SystemExit('Native/fixture instruction differences: ' + ', '.join(failures))
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
