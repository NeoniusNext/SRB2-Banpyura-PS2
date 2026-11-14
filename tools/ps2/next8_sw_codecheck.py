"""Require complete affine-NPOT fixture/default-LTO instruction/RELA identity.

Only relocation fields and symbol spelling normalize; register allocation,
relative branches, instruction scheduling, constants, stack instructions and
alignment padding remain exact. Both relocation targets and explicit addends
must match. This gate binds target kernel measurements to executed game code.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from next7_render_fog_codecheck import Elf32
from next8_sw_hosttest import NAMES


def linked_symbols(elf):
    result={}
    for section in elf.sections:
        if section[1]!=2: continue
        names=elf.bytes(elf.sections[section[6]])
        for offset in range(section[4],section[4]+section[5],section[9]):
            sym=struct.unpack_from('<IIIBBH',elf.data,offset)
            name=names[sym[0]:].split(b'\0',1)[0].decode()
            if name: result[name]={'value':sym[1],'size':sym[2],'section':sym[5]}
    return result


def bind_linked(engine, executable):
    symbols=linked_symbols(executable)
    functions={}
    for name in NAMES:
        code=engine.instructions(name,True)
        symbol=symbols[name];section=executable.sections[symbol['section']]
        offset=symbol['value']-section[3]
        if offset<0 or offset+code['text_bytes']>section[5]:
            raise SystemExit('Linked function/padding outside text section: '+name)
        actual=executable.bytes(section)[offset:offset+code['text_bytes']]
        expected=list(code['words'])
        for location,(kind,target,encoded,addend) in code['relocations'].items():
            if addend is None or encoded!=0:
                raise SystemExit('Expected explicit zero-field MIPS RELA for linked proof')
            if target not in symbols:
                raise SystemExit('Linked relocation target symbol missing: '+target)
            address=(symbols[target]['value']+addend)&0xffffffff
            if kind==4:
                value=(address>>2)&0x3ffffff
            elif kind==5:
                value=((address+0x8000)>>16)&0xffff
            elif kind==6:
                value=address&0xffff
            elif kind==7:
                displacement=address-symbols['_gp']['value']
                if not -32768<=displacement<=32767:
                    raise SystemExit('Resolved GP displacement outside signed16')
                value=displacement&0xffff
            else:
                raise SystemExit('Unsupported final-link relocation '+str(kind))
            expected[location//4] |= value
        expected=struct.pack('<'+'I'*len(expected),*expected)
        if expected!=actual:
            differences=[hex(i) for i,(x,y) in enumerate(zip(expected,actual)) if x!=y]
            raise SystemExit('Actual linked bytes differ after exact RELA resolution '+name+': '+str(differences))
        functions[name]={'all_linked_bytes_equal_after_exact_RELA_resolution':True,'body_plus_padding_bytes':len(actual),
                         'symbol_body_bytes':symbol['size'],'checked_relocations':len(code['relocations'])}
    return {'elf':str(executable.path),'elf_sha256':hashlib.sha256(executable.data).hexdigest(),'functions':functions}


def negative_controls(out, fixture, engine, executable):
    name='R_DrawTranslucentSpan_NPO2_8'
    controls=[]
    code=engine.instructions(name,True)
    symbol=linked_symbols(executable)[name]
    section=executable.sections[symbol['section']]
    linked_start=section[4]+symbol['value']-section[3]
    pointer_location=next(location for location,reloc in code['relocations'].items() if reloc[1]=='ds_transmap')
    for title,location,bit in [('linked_register',4,0x10000),('linked_pointer',pointer_location,1)]:
        data=bytearray(executable.data)
        value=struct.unpack_from('<I',data,linked_start+location)[0]
        struct.pack_into('<I',data,linked_start+location,value^bit)
        path=out/('negative-'+title+'.ELF');path.write_bytes(data)
        try:
            bind_linked(engine,Elf32(path))
        except SystemExit as error:
            if not str(error).startswith('Actual linked bytes differ after exact RELA resolution'):
                raise
            controls.append({'mutation':title,'rejected':True,'file':str(path),'sha256':hashlib.sha256(data).hexdigest()})
        else:
            raise SystemExit('Missed linked-byte negative: '+title)
    index=engine.index['.text.'+name]
    text=engine.sections[index]
    reloc_section=next(section for section in engine.sections if section[1]==4 and section[7]==index)
    records=range(reloc_section[4],reloc_section[4]+reloc_section[5],reloc_section[9])
    record=next(offset for offset in records if struct.unpack_from('<I',engine.data,offset)[0]==pointer_location)
    names=engine.symbols[reloc_section[6]]
    other=next(i for i,value in enumerate(names) if value=='ds_colormap' or value.endswith('.ds_colormap'))
    for title in ('object_register','object_RELA_target','object_RELA_addend'):
        data=bytearray(engine.data)
        if title=='object_register':
            value=struct.unpack_from('<I',data,text[4]+4)[0]
            struct.pack_into('<I',data,text[4]+4,value^0x10000)
        elif title=='object_RELA_target':
            info=struct.unpack_from('<I',data,record+4)[0]
            struct.pack_into('<I',data,record+4,(other<<8)|(info&255))
        else:
            value=struct.unpack_from('<I',data,record+8)[0]
            struct.pack_into('<I',data,record+8,value^1)
        path=out/('negative-'+title+'.o');path.write_bytes(data)
        if fixture.instructions('candidate_'+name,False)==Elf32(path).instructions(name,True):
            raise SystemExit('Missed complete-code negative: '+title)
        controls.append({'mutation':title,'rejected':True,'file':str(path),'sha256':hashlib.sha256(data).hexdigest()})
    return controls


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--fixture-object', required=True, type=Path)
    p.add_argument('--baseline-engine', type=Path)
    p.add_argument('--candidate-engine', type=Path)
    p.add_argument('--baseline-elf', type=Path)
    p.add_argument('--candidate-elf', type=Path)
    p.add_argument('--negative-controls', action='store_true')
    a = p.parse_args(); out = a.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    fixture = Elf32(a.fixture_object.resolve())
    inputs_path = fixture.path.parent / 'source-hashes.json'
    inputs = json.loads(inputs_path.read_text())
    for variant in ('reference', 'candidate'):
        actual_hash = hashlib.sha256((fixture.path.parent / (variant + '.inc')).read_bytes()).hexdigest()
        if actual_hash != inputs[variant]['extracted_sha256']:
            raise SystemExit('Frozen extracted source hash changed: ' + variant)
    for name, key in [('driver.c', 'driver_sha256'), ('next8_sw_stub.h', 'stub_sha256')]:
        file = fixture.path.parent / name
        if file.exists() and hashlib.sha256(file.read_bytes()).hexdigest() != inputs[key]:
            raise SystemExit('Frozen fixture input hash changed: ' + name)
    report = {'fixture_object': str(fixture.path), 'fixture_sha256': hashlib.sha256(fixture.data).hexdigest(),
              'frozen_input_hashes': inputs,
              'normalization': 'Relocation fields and symbol spelling only; complete instruction/register/branch/addend/padding identity', 'versions': {}}
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
            data['functions'][name] = {'identical_complete_instructions_and_relocations': equal,
                                      'fixture_text_bytes': expected['text_bytes'], 'engine_text_bytes': actual['text_bytes'],
                                      'relocation_count': len(expected['relocations']),
                                      'explicit_RELA_addends': sum(v[3] is not None for v in expected['relocations'].values()),
                                      'differences': differences}
            if not equal:
                failures.append(variant + ':' + name)
        linked_path = a.baseline_elf if variant == 'reference' else a.candidate_elf
        if linked_path: data['actual_linked_bytes'] = bind_linked(engine, Elf32(linked_path.resolve()))
        report['versions'][variant] = data
    # These are the exact bounded setup/loop windows in the pinned baseline.
    # Complete matching above still covers the windows themselves. This extra
    # comparison proves that zero/short spans and every fallback execute the
    # original instructions and global relocations, including cold stack work.
    unchanged = {}
    for name, (begin, end) in {
        'R_DrawTranslucentSpan_NPO2_8': (0x1b4, 0x224),
        'R_DrawWaterSpan_NPO2_8': (0x1dc, 0x250),
    }.items():
        reference = fixture.instructions('reference_' + name, False)
        candidate = fixture.instructions('candidate_' + name, False)
        regions_equal = (reference['text_bytes'] == candidate['text_bytes'] and
                         reference['words'][:begin // 4] == candidate['words'][:begin // 4] and
                         reference['words'][end // 4:] == candidate['words'][end // 4:])
        relocs = [{k: v for k, v in item['relocations'].items() if k < begin or k >= end}
                  for item in (reference, candidate)]
        equal = regions_equal and relocs[0] == relocs[1]
        unchanged[name] = {'identical_all_instructions_relocations_outside_bounded_window': equal,
                           'bounded_window_begin': hex(begin), 'bounded_window_end_exclusive': hex(end),
                           'unchanged_prefix_bytes': begin, 'unchanged_suffix_bytes': reference['text_bytes'] - end}
        if not equal:
            failures.append('cold/fallback:' + name)
    report['unchanged_short_and_fallback_code'] = unchanged
    if a.negative_controls:
        if not a.candidate_engine or not a.candidate_elf:
            raise SystemExit('Binary gate negatives require candidate engine and ELF')
        report['five_actual_binary_gate_negative_controls'] = negative_controls(out, fixture, Elf32(a.candidate_engine.resolve()), Elf32(a.candidate_elf.resolve()))
    report['all_compared_functions_match'] = not failures
    (out / 'code-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k:v for k,v in report.items() if k != 'versions'}, indent=2))
    if failures:
        raise SystemExit('Complete native/fixture stream mismatch: ' + ', '.join(failures))


if __name__ == '__main__':
    main()
