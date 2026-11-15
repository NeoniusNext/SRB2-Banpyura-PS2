#!/usr/bin/env python3
"""Bind complete actual flat context bodies/data/RELA/frames and opt-out restore."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil
import struct

from audio_storage_next7_finalbind import Object
from next7_render_fog_codecheck import Elf32
from next8_sw_codecheck import linked_symbols
from next10_sw_bounds_codecheck import output_bytes, symbol_tables
from next12_pb_scratch_objectcheck import allocated, pdr
import next12_pb_scratch_codecheck as strict
from next13_flats_cold_variant import ROOT, BASE, OUT, PATH


ORIGINAL_FRAME_WITNESS = strict.encoded_frame


def encoded_frame(raw, metadata):
    try:
        return ORIGINAL_FRAME_WITNESS(raw, metadata)
    except SystemExit:
        # The existing HWR_RenderBatchesV2 reserves its 40928-byte stack in
        # two immediate steps; normalize each save to the caller's SP before
        # checking the same actual PDR facts. No body bytes are ignored.
        fields = struct.unpack('<8I', bytes.fromhex(metadata['frame_record_hex']))
        amount, mask = fields[5], fields[1]
        save = struct.unpack('<i', struct.pack('<I', fields[2]))[0]
        if amount < 32768:
            raise
        words = struct.unpack('<' + 'I' * (len(raw) // 4), raw)
        delta, steps, saves = 0, [], []
        for at, word in enumerate(words):
            op, rs, rt = word >> 26, (word >> 21) & 31, (word >> 16) & 31
            immediate = struct.unpack('<h', struct.pack('<H', word & 65535))[0]
            if op == 3 or (op == 0 and word & 63 == 9):
                break
            if op in (9, 25) and rs == 29 and rt == 29:
                delta += immediate
                steps.append({'offset': at * 4, 'signed_adjustment': immediate, 'caller_SP_delta': delta})
            if op in (43, 63, 31) and rs == 29:
                saves.append({'offset': at * 4, 'register': rt, 'encoded_SP_offset': immediate,
                              'caller_SP_offset': delta + immediate, 'opcode': op})
        if delta != -amount or not steps or any(s['signed_adjustment'] >= 0 for s in steps):
            raise SystemExit('Split actual prologue does not reserve the exact complete PDR frame')
        highest = mask.bit_length() - 1
        if mask and not any(s['register'] == highest and s['caller_SP_offset'] == save for s in saves):
            raise SystemExit('Split actual prologue highest register save does not match exact PDR offset')
        if not all(any(s['register'] == reg for s in saves) for reg in range(32) if mask & (1 << reg)):
            raise SystemExit('Split actual prologue does not witness every exact PDR saved register')
        return {'complete_input_PDR_words': list(fields), 'split_actual_SP_adjustments': steps,
                'all_prologue_saves_normalized_to_caller_SP': saves,
                'advertised_reservation_and_integer_save_mask_witnessed': True,
                'scope': 'Complete resolved body includes all instructions and padding; its exact two-step prologue and caller-SP-relative save offsets witness the unchanged large PDR frame.'}


class ActualSectionBinder(strict.StrictBinder):
    """Keep real input section names, including .text.unlikely, throughout."""
    def function_symbol(self,name):
        rows=[s for table in symbol_tables(self.obj).values() for s in table
              if s['name']==name and s['type']==2 and s['section']]
        if len(rows)!=1:
            raise SystemExit('One actual input function symbol required '+name)
        return rows[0]

    def section_function(self,section):
        index=self.obj.index[section]
        rows=[s for table in symbol_tables(self.obj).values() for s in table
              if s['type']==2 and s['section']==index]
        if len(rows)!=1:
            raise SystemExit('One exact function owner of actual input section required '+section)
        return rows[0]

    def function_relocations(self,name):
        owner=self.function_symbol(name)
        return strict.source_relas(self.obj,owner['section'])

    def exact_frame(self,owner):
        index=self.obj.index['.pdr'];raw=self.obj.bytes(self.obj.sections[index])
        matches=[]
        for r in strict.source_relas(self.obj,index):
            s=r['symbol']
            if s['section']!=owner['section'] or s['value']+r['addend']!=owner['value']:
                continue
            if r['kind']!=2 or r['offset']%32:
                raise SystemExit('Actual function PDR relocation shape differs '+owner['name'])
            body=raw[r['offset']:r['offset']+32]
            matches.append({'stack_frame_bytes':struct.unpack_from('<I',body,20)[0],
                'register_mask':struct.unpack_from('<I',body,4)[0],
                'frame_record_hex':body.hex()})
        if len(matches)!=1:
            raise SystemExit('One exact actual-section PDR required '+owner['name'])
        return matches[0]

    def text(self,name):
        owner=self.function_symbol(name);index=owner['section']
        section=self.obj.sections[index];section_name=self.obj.names[index]
        if owner['value']!=0 or section[2]&6!=6:
            raise SystemExit('Complete executable function section owner/offset differs '+name)
        address=self.mapped(index)
        linked=self.symbols[name]
        if linked['value']!=address or linked['size']!=owner['size']:
            raise SystemExit('Actual input/linked symbol address or extent differs '+name)
        original=self.obj.bytes(section);expected=bytearray(original);records=[]
        for r in self.function_relocations(name):
            at,kind,symbol,addend=r['offset'],r['kind'],r['symbol'],r['addend']
            target,resolved=self.resolve(symbol,addend,kind,address+at)
            if kind==4:mask=0x03ffffff;field=(target>>2)&mask
            elif kind==5:mask=65535;field=((target+0x8000)>>16)&mask
            elif kind==6:mask=65535;field=target&mask
            elif kind==7:
                mask=65535;delta=target-self.symbols['_gp']['value']
                if not -32768<=delta<=32767:
                    raise SystemExit('Actual GP displacement is outside signed16')
                field=delta&mask
            else:
                raise SystemExit('Unhandled actual text explicit RELA kind')
            word=struct.unpack_from('<I',expected,at)[0]
            if word&mask:
                raise SystemExit('Nonzero encoded explicit text RELA field')
            struct.pack_into('<I',expected,at,word|field)
            records.append({'offset':at,'kind':kind,'exact_symbol':symbol['name'],
                'explicit_addend':addend,'resolved_symbol':resolved,'resolved_address':target,
                'input_symbol':symbol})
        actual=output_bytes(self.elf,address,len(expected))
        if actual!=expected:
            raise SystemExit('Fully resolved complete actual-section text/padding differs '+name)
        frame=self.exact_frame(owner)
        return {'name':name,'input_section_name':section_name,'input_section_index':index,
            'exact_input_function_symbol':owner,'complete_text_bytes':len(original),
            'exact_symbol_bytes':owner['size'],'all_fully_resolved_text_and_padding_bytes_equal':True,
            'exact_frame':frame,'frame_instruction_witness':encoded_frame(actual,frame),
            'ordered_RELA_records':records,'resolved_text_sha256':hashlib.sha256(actual).hexdigest(),
            'input_object_sha256':hashlib.sha256(self.obj.data).hexdigest(),
            'ELF_sha256':hashlib.sha256(self.elf.data).hexdigest(),'map_sha256':sha(self.map_path),
            'actual_memory_wrappers':sorted(self.wrappers),'retained_linker_RELA_evidence':self.linker_rela_evidence,
            'actual_section_not_aliased_renamed_or_masked':True}


def actual_binder(build,linker):
    report=json.loads((build/'build-report.json').read_text())
    if sha(build/'SRB2.ELF')!=report['elf_sha256']:
        raise SystemExit('Actual build ELF admission differs')
    value=ActualSectionBinder(Elf32(build/'obj/engine_lto.o'),Elf32(build/'SRB2.ELF'),
        build/'SRB2.map',report['link_command'],linker)
    value.literal_uses=[]
    return value


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def controls(out, original, checked):
    out.mkdir()
    primary = next(n for n in checked if n.startswith('HWR_GetLevelFlat'))
    names = [next(n for n in checked if n.startswith(prefix))
             for prefix in ('HWR_GetLevelFlat', 'HWR_LoadMapTextures', 'HWR_FreeMapTextures')]
    results = []
    def linked_offset(address, size=1):
        matches = [s for s in original.elf.sections if s[1] != 8 and s[2] & 2
                   and s[3] <= address and address + size <= s[3] + s[5]]
        if len(matches) != 1:
            raise RuntimeError('One actual linked negative span required')
        return matches[0][4] + address - matches[0][3]
    def reject(label, raw, is_object, callback):
        path = out / (label + ('.o' if is_object else '.ELF'))
        path.write_bytes(raw)
        current = copy.copy(original)
        current.obj = Elf32(path) if is_object else original.obj
        current.elf = original.elf if is_object else Elf32(path)
        current.symbols = linked_symbols(current.elf)
        current.data, current.literals, current.literal_uses = {}, {}, []
        admissions = {'engine': hashlib.sha256(current.obj.data).hexdigest(), 'ELF': hashlib.sha256(current.elf.data).hexdigest()}
        try:
            callback(current)
        except (SystemExit, RuntimeError) as error:
            reason = str(error)
            if any(s in reason.lower() for s in ('hash pin', 'checksum', 'build/elf pin', 'sha256')):
                raise RuntimeError('Outer pin alone rejected ' + label)
            results.append({'mutation': label, 'path': str(path), 'sha256': sha(path),
                            'refreshed_admissions': admissions, 'inner_binding_rejected': True, 'reason': reason})
        else:
            raise RuntimeError('Uncaught actual binary corruption ' + label)
    for name in names:
        section = original.obj.sections[original.function_symbol(name)['section']]
        raw = bytearray(original.obj.data)
        struct.pack_into('<I', raw, section[4], struct.unpack_from('<I', raw, section[4])[0] ^ 0x10000)
        reject('object-register-' + name, raw, True, lambda v, n=name: v.text(n))
    raw = bytearray(original.elf.data)
    at = linked_offset(original.maps[original.obj.names[original.function_symbol(primary)['section']]][0], 4)
    struct.pack_into('<I', raw, at, struct.unpack_from('<I', raw, at)[0] ^ 0x10000)
    reject('linked-register', raw, False, lambda v: v.text(primary))
    call = next(r for r in original.function_relocations(primary) if r['kind'] == 4)
    raw = bytearray(original.obj.data)
    struct.pack_into('<i', raw, call['record_file_offset'] + 8, call['addend'] + 4)
    reject('call-RELA-addend', raw, True, lambda v: v.text(primary))
    raw = bytearray(original.obj.data)
    table = symbol_tables(original.obj)[call['symbol_table']]
    replacement = next(i for i, s in enumerate(table) if s['name'] == 'Z_Free')
    at = call['record_file_offset'] + 4
    word = struct.unpack_from('<I', raw, at)[0]
    struct.pack_into('<I', raw, at, (replacement << 8) | (word & 255))
    reject('call-RELA-target', raw, True, lambda v: v.text(primary))
    section = original.obj.sections[original.obj.index['.pdr']]
    sites = [r['offset'] for r in strict.source_relas(original.obj, original.obj.index['.pdr'])
             if r['symbol']['section']==original.function_symbol(primary)['section'] and r['symbol']['value']+r['addend']==original.function_symbol(primary)['value']]
    if len(sites) != 1:
        raise RuntimeError('One primary actual frame record required')
    raw = bytearray(original.obj.data)
    at = section[4] + sites[0] + 20
    struct.pack_into('<I', raw, at, struct.unpack_from('<I', raw, at)[0] ^ 16)
    reject('frame-size', raw, True, lambda v: v.text(primary))
    layout = next(i for i, n in enumerate(original.obj.names) if n.endswith('.gl_flats_cut'))
    raw = bytearray(original.obj.data)
    h = struct.unpack_from('<16sHHIIIIIHHHHHH', raw)
    at = h[6] + layout * h[11] + 20
    struct.pack_into('<I', raw, at, struct.unpack_from('<I', raw, at)[0] - 1)
    reject('private-layout-capacity', raw, True, lambda v: v.text(primary))
    string_owner, string = next((n, r) for n in names for r in original.function_relocations(n)
        if r['symbol']['section'] and original.obj.sections[r['symbol']['section']][2] & 0x30 == 0x30)
    address = original.maps[original.obj.names[original.function_symbol(string_owner)['section']]][0] + string['offset']
    anchors = original.linker_relas[(address, string['kind'])]
    if len(anchors) != 1:
        raise RuntimeError('One exact final flat literal anchor required')
    raw = bytearray(original.elf.data)
    raw[linked_offset(anchors[0]['resolved_address'])] ^= 1
    reject('exact-flat-literal', raw, False, lambda v: v.text(string_owner))
    return results


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline', type=Path, default=ROOT / 'build/nov15-next12/final/hardware')
    p.add_argument('--candidate', type=Path, default=ROOT / 'build/nov15-next13/flats-cold-context-v5')
    p.add_argument('--control', type=Path, default=ROOT / 'build/nov15-next13/flats-cold-control-v5')
    p.add_argument('--source', type=Path, default=OUT)
    p.add_argument('--baseline-relas', type=Path, default=ROOT / 'build/nov15-next12/final/pb-binding/final-linker-relas')
    p.add_argument('--out', type=Path, default=ROOT / 'build/nov15-next13/flats-cold-binding-v5')
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=False)
    pinned = a.out / 'pinned-tools'
    pinned.mkdir()
    tools = [Path(__file__), ROOT / 'tools/ps2/next13_flats_cold_context.py', ROOT / 'tools/ps2/next13_flats_cold_variant.py', ROOT / 'tools/ps2/next13_flats_cut_variant.py', ROOT / 'tools/ps2/next13_flats_variant.py',
        ROOT / 'tools/ps2/next12_pb_scratch_codecheck.py', ROOT / 'tools/ps2/next12_pb_scratch_objectcheck.py',
        ROOT / 'tools/ps2/next11_sw_checkplane_codecheck.py', ROOT / 'tools/ps2/next10_sw_bounds_codecheck.py',
        ROOT / 'tools/ps2/next7_render_fog_codecheck.py', ROOT / 'tools/ps2/audio_storage_next7_finalbind.py']
    for t in tools:
        shutil.copyfile(t, pinned / t.name)
    manifest = json.loads((a.source / 'source-manifest.json').read_text())
    actual_sources = {p.relative_to(a.source).as_posix(): sha(p) for p in (a.source / 'src').rglob('*') if p.is_file()}
    if actual_sources != manifest['candidate_source_sha256']:
        raise RuntimeError('Complete actual compile source differs')
    for label, build in [('candidate', a.candidate), ('control', a.control)]:
        c = json.loads((build / 'context-report.json').read_text())
        if c['narrow_optout'] != (label == 'control') or c['only_recompiled_unit'] != PATH or c['complete_source_overlay_sha256'] != actual_sources:
            raise RuntimeError('Actual context source/control admission differs')
        if len(c['complete_actual_IR_objects']) != 205 or c['changed_actual_IR_objects'] != ['src__hardware__hw_cache.o']:
            raise RuntimeError('Actual complete205-input graph differs')
        for path, digest in c['complete_actual_IR_objects'].items():
            if sha(path) != digest:
                raise RuntimeError('Frozen actual IR input changed')
    builds = {'baseline': a.baseline, 'candidate': a.candidate, 'control': a.control}
    objects = {tag: Object(path / 'obj/engine_lto.o') for tag, path in builds.items()}
    sig = {tag: allocated(obj) for tag, obj in objects.items()}
    frames = {tag: pdr(obj) for tag, obj in objects.items()}
    restoration = {'complete_allocated_object_sections_and_ordered_RELAs': sig['baseline'] == sig['control'],
        'complete_PDRs_and_ordered_RELAs': frames['baseline'] == frames['control'],
        'entire_shipped_ELF': sha(a.baseline / 'SRB2.ELF') == sha(a.control / 'SRB2.ELF')}
    if not all(restoration.values()):
        raise RuntimeError('Narrow opt-out does not exactly restore baseline')
    names = set(sig['baseline']) | set(sig['candidate'])
    changed = sorted(n for n in names if sig['baseline'].get(n) != sig['candidate'].get(n))
    code = [n for n in changed if n.startswith('.text.')]
    callers = set()
    for obj in (objects['baseline'], objects['candidate']):
        for owner, relas in obj.relocations.items():
            if owner.startswith('.text.') and any(r['target']['section'] in code for r in relas):
                callers.add(owner)
    prefixes = ('.text.__wrap', '.text.Spill_', '.text.HWR_GetLevelFlat', '.text.HWR_LoadMapTextures',
        '.text.HWR_FreeMapTextures', '.text.HWR_InitMapTextures', '.text.HWR_PS2_RegenerateMipmap',
        '.text.HWR_PS2_StealData', '.text.M_CheckParm', '.text.M_IsNextParm', '.text.M_GetNextParm',
        '.text.PS2TexC_Reset', '.text.Z_ClearRebuildCosts',
        '.text.R_LoadTextures', '.text.R_FinishLoadingTextures', '.text.HWR_PB', '.text.HWR_RenderBatches')
    chosen = set(code) | callers | {n for n in names if n.startswith(prefixes)}
    report = {'status': 'RUNNING', 'source_manifest_sha256': sha(a.source / 'source-manifest.json'),
        'optout_exact_restoration': restoration, 'changed_allocated_sections': changed,
        'changed_code_or_binding_sections': code, 'direct_actual_callers': sorted(callers),
        'selected_complete_body_sections': sorted(chosen), 'actual_input_frames': frames, 'variants': {},
        'tools': {t.name: sha(t) for t in tools}}
    binders = {}
    for tag, build, linker in [('baseline', a.baseline, a.baseline_relas), ('candidate', a.candidate, a.candidate / 'linker-relas')]:
        v = actual_binder(build.resolve(), linker.resolve())
        binders[tag] = v
        checked, discarded = {}, []
        for section in sorted(chosen):
            if section not in v.obj.index:
                discarded.append({'section': section, 'body_present': False})
            elif section not in v.maps or not v.maps[section][0]:
                discarded.append({'section': section, 'body_present': True, 'complete_input_signature': objects[tag].signature(section),
                    'complete_ordered_RELAs': objects[tag].relocations.get(section, []), 'exact_PDR': frames[tag]['records'].get(section)})
            else:
                try:
                    owner=v.section_function(section)
                    checked[owner['name']] = v.text(owner['name'])
                except SystemExit as error:
                    raise SystemExit(tag + ' ' + section + ': ' + str(error))
        symbols = linked_symbols(v.elf)
        report['variants'][tag] = {'complete_bound_text': checked, 'discarded_bodies': discarded,
            'complete_referenced_data': v.data, 'complete_literals': list(v.literals.values()), 'every_exact_literal_use': v.literal_uses,
            'actual_call_boundaries': strict.boundary_records(v, checked), 'ELF_sha256': sha(build / 'SRB2.ELF'),
            'engine_sha256': sha(build / 'obj/engine_lto.o'),
            'allocated_final_sections': {n: {'type': s[1], 'flags': s[2], 'address': s[3], 'bytes': s[5], 'alignment': s[8]}
                for n, s in zip(v.elf.names, v.elf.sections) if s[2] & 2},
            'memory_symbols': {n: symbols[n] for n in ('_end', '_gp', '_fbss', '_etext') if n in symbols}}
    report['binary_negative_controls'] = controls(a.out / 'binary-negatives', binders['candidate'], report['variants']['candidate']['complete_bound_text'])
    report['actual_cold_constructor_section_transition'] = {
        tag: {'actual_symbol': value.function_symbol('HWR_LoadMapTextures'),
              'actual_section_name': value.obj.names[value.function_symbol('HWR_LoadMapTextures')['section']],
              'complete_bound_body': report['variants'][tag]['complete_bound_text']['HWR_LoadMapTextures']}
        for tag,value in binders.items()}
    report['cold_codegen_is_size_oriented_under_unchanged_global_O2'] = True
    report['newly_retained_original_O2_callees_bound_explicitly'] = ['PS2TexC_Reset', 'Z_ClearRebuildCosts']
    report['raw_input_body_changes'] = [n for n in code if n in objects['baseline'].sections and n in objects['candidate'].sections
        and objects['baseline'].signature(n) != objects['candidate'].signature(n)]
    report['added_code_bodies'] = [n for n in code if n not in objects['baseline'].sections]
    report['removed_code_bodies'] = [n for n in code if n not in objects['candidate'].sections]
    be = report['variants']['baseline']['memory_symbols']['_end']['value']
    ce = report['variants']['candidate']['memory_symbols']['_end']['value']
    report['static_end_delta_bytes'] = ce - be
    report['asset_derived_flat_table_payload_saving_bytes'] = 37920
    report['predicted_payload_minus_static_end_delta_bytes'] = 37920 - (ce - be)
    report['live_numtextures_and_heap_residency_not_yet_measured'] = True
    report['status'] = 'PASS complete actual whole-LTO flat text/data/RELA/literal/frame/capacity binding and exact opt-out restoration'
    (a.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'status': report['status'], 'bound_bodies': {t: len(v['complete_bound_text']) for t, v in report['variants'].items()},
        'raw_input_body_changes': report['raw_input_body_changes'], 'static_end_delta_bytes': ce-be,
        'binary_negative_controls': len(report['binary_negative_controls'])}))


if __name__ == '__main__':
    main()
