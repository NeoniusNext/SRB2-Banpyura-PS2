#!/usr/bin/env python3
"""Bind the drawseg endpoint domain, unchanged vertices and backing geometry."""
import argparse
import hashlib
import json
from pathlib import Path
import re

from next10_memory_hosttest import ROOT, parts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, default=ROOT / 'build/nov15-next10/baseline-src')
    parser.add_argument('--candidate', type=Path, default=ROOT)
    parser.add_argument('--out', type=Path, default=ROOT / 'build/nov15-next10/memory/audit.json')
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    candidate = args.candidate.resolve()
    source = candidate / 'src'
    setup = (source / 'p_setup.c').read_text()
    refs, writes, endpoint_refs = [], [], []
    for path in sorted(source.rglob('*')):
        if path.suffix not in ('.c', '.h', '.S', '.s', '.inc'):
            continue
        text = path.read_text(errors='strict')
        for n, line in enumerate(text.splitlines(), 1):
            if re.search(r'\b(?:leftpos|rightpos)\b', line):
                item = {'source': str(path.relative_to(candidate)), 'line': n, 'text': line.strip()}
                endpoint_refs.append(item)
                if path.name == 'r_defs.h':
                    assert line.strip() in ('vector2_t leftpos, rightpos;',
                        'vertex_t leftpos, rightpos; // Used for rendering FOF walls with slopes'), item
                else:
                    assert path.name == 'r_segs.c', item
                    assert not re.search(r'\b(?:leftpos|rightpos)\b(?!\.[xy]\b)', line), item
            if not re.search(r'\b(?:floorzset|ceilingzset)\b', line):
                continue
            item = {'source': str(path.relative_to(candidate)), 'line': n, 'text': line.strip()}
            refs.append(item)
            if re.search(r'(?:floorzset|ceilingzset)\s*=(?!=)', line):
                assert path.name == 'p_setup.c', item
                assert re.search(r'=\s*(?:true|false);\s*$', line), item
                writes.append(item)
            assert not re.search(r'&\s*[^;]*(?:floorzset|ceilingzset)', line), item
    assert len(writes) == 4
    assert sum('false;' in r['text'] for r in writes) == 2
    assert sum('true;' in r['text'] for r in writes) == 2
    lua = (source / 'lua_maplib.c').read_text()
    script = (source / 'lua_script.c').read_text()
    poly = (source / 'p_polyobj.c').read_text()
    segs = (source / 'r_segs.c').read_text()
    zone = (source / 'z_zone.c').read_text()
    retained = {
        'readonly_lua_registration': 'LUA_RegisterUserdataMetatable(L, META_VERTEX, vertex_get, NULL, vertex_num);' in lua,
        'lua_archives_indices': 'P_WriteUINT16(save_p, vertex - vertexes);' in script,
        'lua_unarchives_typed_indices': 'LUA_PushUserdata(gL, &vertexes[P_ReadUINT16(save_p)], META_VERTEX);' in script,
        'lua_invalidates_each_typed_element': 'LUA_InvalidateUserdata(&vertexes[i]);' in script,
        'main_vertex_calloc_sizeof': 'vertexes  = Z_Calloc(numvertexes * sizeof (*vertexes), PU_LEVEL, NULL);' in setup,
        'extra_vertex_realloc_sizeof': 'vertexes = Z_Realloc(vertexes, numvertexes*sizeof(*vertexes), PU_LEVEL, NULL);' in setup,
        'writer_copy_sizeof': 'memcpy(wvertexes, vertexes, numvertexes * sizeof(*vertexes));' in setup,
        'poly_original_array_sizeof': 'po->numVerticesAlloc * sizeof(vertex_t)' in poly,
        'poly_temporary_full_copy': 'po->tmpVerts[i] = *(po->vertices[i]);' in poly,
        'drawseg_growth_sizeof': 'drawsegs = Z_Realloc(drawsegs, newmax*sizeof (*drawsegs), PU_STATIC, NULL);' in segs,
        'zone_inplace_growth_zeroed': 'memset((uint8_t *)ptr + copysize, 0, size - copysize);' in zone,
        'zone_moved_growth_zeroed': 'memset((char*)rez+copysize, 0x00, size-copysize);' in zone,
    }
    assert all(retained.values()), retained
    before, after = parts(args.baseline.resolve()), parts(candidate)
    for key in before:
        if key != 'MEMORY_DRAWSEG':
            assert before[key] == after[key], key + ' implementation differs from frozen source'
    assert (args.baseline / 'src/z_zone.c').read_bytes() == (source / 'z_zone.c').read_bytes()
    assert (args.baseline / 'src/ps2/ps2_mem.c').read_bytes() == (source / 'ps2/ps2_mem.c').read_bytes()
    geometry = []
    capacity, backing = 16 << 20, (16 << 20) + 65536 + 32
    for residue in range(0, 65536, 64):
        prefix = ((residue + 16 + 65535) & ~65535) - residue
        suffix = backing - prefix - capacity
        assert prefix >= 16 and suffix >= 16
        geometry.append((prefix, suffix))
    hashes = {str(p.relative_to(candidate)): hashlib.sha256(p.read_bytes()).hexdigest() for p in
              [source / n for n in ('r_defs.h', 'p_setup.c', 'p_slopes.c', 'p_polyobj.c', 'lua_maplib.c',
                                    'lua_script.c', 'r_segs.c', 'z_zone.c', 'ps2/ps2_mem.c')]}
    report = {'source_sha256': hashes, 'all_flag_references': refs, 'all_named_flag_writes': writes,
              'all_endpoint_references': endpoint_refs,
              'endpoint_ref_domain': 'Every nondeclaration reference is r_segs.c .x or .y; no full-object copy, whole-object address escape or height/flag read.',
              'flag_write_domain': [0, 1], 'retained_source_contracts': retained,
              'all_extracted_consumers_equal_frozen_source': True,
              'persistent_vertex_declaration_equal_frozen_source': before['MEMORY_TYPES'] == after['MEMORY_TYPES'],
              'single_backing_capacity': capacity, 'backing_bytes': backing,
              'all_64aligned_base_residues': len(geometry),
              'minimum_prefix_guard': min(p for p, _ in geometry),
              'minimum_suffix_guard': min(s for _, s in geometry),
              'manual_source_audit': 'No positional drawseg initializer, raw drawseg serialization/comparison, fixed stride/offset ASM or binary plugin consumer. All drawseg arrays use sizeof/typed arithmetic; growth repairs ds_p/curdrawsegs/firstseg indices. Both endpoints use only full fixed_t x/y. Persistent vertex_t, its heights/flags, polyobjects, Lua/map/network/save types remain byte-identical. Non-PS2 header preprocessing equality is checked by host proof.'}
    args.out.write_text(json.dumps(report, indent=2) + '\n')
    print('All drawseg endpoint uses are typed XY only; persistent vertices/consumer bodies unchanged; 1024 backing residues bounded.')


if __name__ == '__main__':
    main()
