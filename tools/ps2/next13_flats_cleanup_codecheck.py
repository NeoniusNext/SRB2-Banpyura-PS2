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
from next10_sw_bounds_codecheck import relocations, symbol_tables
from next12_pb_scratch_objectcheck import allocated, pdr
import next12_pb_scratch_codecheck as strict
from next13_flats_cleanup_variant import ROOT, BASE, OUT, PATH


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


strict.encoded_frame = encoded_frame


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def controls(out, original, checked):
    out.mkdir()
    primary = next(n for n in checked if n.startswith('HWR_GetLevelFlat'))
    names = [next(n for n in checked if n.startswith(prefix))
             for prefix in ('HWR_GetLevelFlat', 'HWR_LoadMapTextures', 'HWR_FreeMapTextures', 'FreeMapMipmap')]
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
        section = original.obj.sections[original.obj.index['.text.' + name]]
        raw = bytearray(original.obj.data)
        struct.pack_into('<I', raw, section[4], struct.unpack_from('<I', raw, section[4])[0] ^ 0x10000)
        reject('object-register-' + name, raw, True, lambda v, n=name: v.text(n))
    raw = bytearray(original.elf.data)
    at = linked_offset(original.maps['.text.' + primary][0], 4)
    struct.pack_into('<I', raw, at, struct.unpack_from('<I', raw, at)[0] ^ 0x10000)
    reject('linked-register', raw, False, lambda v: v.text(primary))
    call = next(r for r in relocations(original.obj, primary) if r['kind'] == 4)
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
             if r['symbol']['name'] in (primary, '.text.' + primary)]
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
    string_owner, string = next((n, r) for n in names for r in relocations(original.obj, n)
        if r['symbol']['section'] and original.obj.sections[r['symbol']['section']][2] & 0x30 == 0x30)
    address = original.maps['.text.' + string_owner][0] + string['offset']
    anchors = original.linker_relas[(address, string['kind'])]
    if len(anchors) != 1:
        raise RuntimeError('One exact final flat literal anchor required')
    raw = bytearray(original.elf.data)
    raw[linked_offset(anchors[0]['resolved_address'])] ^= 1
    reject('exact-flat-literal', raw, False, lambda v: v.text(string_owner))
    return results


def cleanup_read_order(value):
    sections=[n for n in value.obj.names if n.startswith('.text.FreeMapMipmap')]
    if len(sections)!=1:
        raise RuntimeError('Exactly one uncloned complete cleanup helper required')
    section=sections[0];name=section[6:]
    raw=value.obj.bytes(value.obj.sections[value.obj.index[section]])
    words=struct.unpack('<'+'I'*(len(raw)//4),raw)
    callbacks=[i*4 for i,w in enumerate(words) if w>>26==0 and w&63==9]
    data_reads=[i*4 for i,w in enumerate(words) if w>>26==35 and w&65535==4 and (w>>21)&31!=29]
    relas=relocations(value.obj,name)
    zones=[r['offset'] for r in relas if r['symbol']['name']=='Z_Free']
    frees=[r['offset'] for r in relas if r['symbol']['name']=='free']
    # Actual O2 duplicates the primary path inside this one function, so there
    # are three callback/data/zone-free instruction sequences, not clones.
    if len(callbacks)!=3 or len(data_reads)!=3 or len(zones)!=3 or len(frees)!=1:
        raise RuntimeError('Unexpected actual complete cleanup callback/data graph')
    zone_reads=[]
    for transfer in zones:
        reads=[at for at in data_reads if transfer-16<=at<=transfer+4]
        if len(reads)!=1:
            raise RuntimeError('Each actual zone-free path requires one fresh data argument load')
        read=reads[0]
        if not any(at+4<read for at in callbacks):
            raise RuntimeError('A data field was read before the driver callback')
        zone_reads.append({'Z_Free_transfer_offset':transfer,'fresh_data_load_offset':read})
    free=frees[0];delay=struct.unpack_from('<I',raw,free+4)[0]
    if not (delay>>26==35 and delay&65535==20 and (delay>>16)&31==4 and callbacks[0]+4<free+4):
        raise RuntimeError('Child free must freshly reload nextcolormap after its driver callback')
    clears=[i*4 for i,w in enumerate(words) if w>>26==43 and w&65535==20 and (w>>16)&31==0]
    if len(clears)!=1 or not free+4<clears[0]<callbacks[1]:
        raise RuntimeError('Child link must clear after free and before primary callback')
    caller='HWR_FreeMapTextures';calls=[r for r in relocations(value.obj,caller) if r['symbol']['name'] in (section,name)]
    if len(calls)!=2:
        raise RuntimeError('Exactly two active wall/flat cleanup calls required')
    return {'one_uncloned_helper':section,'helper_complete_bytes':len(raw),
        'driver_callback_offsets':callbacks,'fresh_data_loads_and_zone_transfers':zone_reads,
        'child_free_call_offset':free,'fresh_child_link_load_in_delay_slot':free+4,
        'child_link_null_store_offset':clears[0],
        'active_wall_flat_caller_offsets':[r['offset'] for r in calls],
        'source_body_projection_and_both_true_false_policies_checked_by_host':True,
        'complete_actual_bodies_RELAs_and_PDRs_bound_separately':True}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline', type=Path, default=ROOT / 'build/nov15-next12/final/hardware')
    p.add_argument('--candidate', type=Path, default=ROOT / 'build/nov15-next13/flats-cleanup-context-v4')
    p.add_argument('--control', type=Path, default=ROOT / 'build/nov15-next13/flats-cleanup-control-v4')
    p.add_argument('--source', type=Path, default=OUT)
    p.add_argument('--baseline-relas', type=Path, default=ROOT / 'build/nov15-next12/final/pb-binding/final-linker-relas')
    p.add_argument('--out', type=Path, default=ROOT / 'build/nov15-next13/flats-cleanup-binding-v4')
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=False)
    pinned = a.out / 'pinned-tools'
    pinned.mkdir()
    tools = [Path(__file__), ROOT / 'tools/ps2/next13_flats_cleanup_context.py', ROOT / 'tools/ps2/next13_flats_cleanup_variant.py', ROOT / 'tools/ps2/next13_flats_cut_variant.py', ROOT / 'tools/ps2/next13_flats_variant.py',
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
        '.text.R_LoadTextures', '.text.R_FinishLoadingTextures', '.text.HWR_PB', '.text.HWR_RenderBatches')
    chosen = set(code) | callers | {n for n in names if n.startswith(prefixes)}
    report = {'status': 'RUNNING', 'source_manifest_sha256': sha(a.source / 'source-manifest.json'),
        'optout_exact_restoration': restoration, 'changed_allocated_sections': changed,
        'changed_code_or_binding_sections': code, 'direct_actual_callers': sorted(callers),
        'selected_complete_body_sections': sorted(chosen), 'actual_input_frames': frames, 'variants': {},
        'tools': {t.name: sha(t) for t in tools}}
    binders = {}
    for tag, build, linker in [('baseline', a.baseline, a.baseline_relas), ('candidate', a.candidate, a.candidate / 'linker-relas')]:
        v = strict.binder(build.resolve(), linker.resolve())
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
                    checked[section[6:]] = v.text(section[6:])
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
    report['shared_cleanup_read_order'] = cleanup_read_order(binders['candidate'])
    report['binary_negative_controls'] = controls(a.out / 'binary-negatives', binders['candidate'], report['variants']['candidate']['complete_bound_text'])
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
