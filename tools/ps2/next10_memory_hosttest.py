#!/usr/bin/env python3
"""Real drawseg XY storage/growth and unchanged map-vertex consumers in the arena."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

from next8_memory_hosttest import parts as sprite_parts
from next9_memory_hosttest import function, section
from verify_pack import decode

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'
FIELDS = ('x', 'y', 'floorzset', 'ceilingzset', 'floorz', 'ceilingz')


def parts(root):
    defs = (root / 'src/r_defs.h').read_text()
    setup = (root / 'src/p_setup.c').read_text()
    lua = (root / 'src/lua_maplib.c').read_text()
    poly = (root / 'src/p_polyobj.c').read_text()
    math = (root / 'src/m_fixed.h').read_text()
    slopes = (root / 'src/p_slopes.c').read_text()
    segs = (root / 'src/r_segs.c').read_text()
    ray = (root / 'src/r_slopeq.h').read_text()
    hashsrc = (root / 'src/ps2/ps2_loadprof.h').read_text()
    vertex = section(defs, 'typedef struct\n{\n\tfixed_t x, y;', '// Forward of linedefs')
    udmf = section(setup, '\tfor (i = 0, vt = vertexes;', '\n\tTextmapReleasePos(&vertexBlocks);')
    udmf = 'static void actual_textmap_vertices(void)\n{\nUINT32 i; vertex_t *vt;\n' + udmf + '\n}\n'
    hashloop = section(setup, '\tfor (i = 0; i < numvertexes; i++)\n\t{\n\t\tconst vertex_t *v', '\n\tPS2LP_HashPrint(')
    hashing = section(hashsrc, 'typedef struct { UINT32 h[2]; } ps2lp_hash_t;', '\nvoid PS2LP_HashPrint(')
    hashing += '\n#define LH(v) PS2LP_H32(&hs, (UINT32)(v))\n'
    hashing += 'static ps2lp_hash_t actual_vertex_hash(void)\n{\nsize_t i; ps2lp_hash_t hs = {{2166136261u, 0x811C9DC5u ^ 0xA5A5A5A5u}};\n'
    hashing += hashloop + '\nreturn hs;\n}\n'
    drawseg = section(defs, 'typedef struct drawseg_s', '\n#ifdef ROTSPRITE')
    growth = section(segs, '\tif (ds_p == drawsegs+maxdrawsegs)', '\n\tsidedef = curline->sidedef;')
    growth = 'static void actual_drawseg_growth(void)\n{\n' + growth + '\n}\n'
    intersections = section(segs, '\tif (frontsector->hasslope || (backsector && backsector->hasslope))', '\n\tPS2SUB_E(3);')
    intersections = 'static void actual_drawseg_intersections(INT32 start, INT32 stop)\n{\nvertex_t segleft = {0}, segright = {0};\n' + intersections + '\n}\n'
    rayhelpers = ''.join(function(ray, n) for n in ('RQ_Clz64', 'RQ_Mul128', 'RQ_MAdd128', 'RQ_Div128', 'RQ_MulAddDiv', 'RQ_RayHitsSeg'))
    return {'MEMORY_ZONE': sprite_parts(root)['MEMORY_ZONE'], 'MEMORY_TYPES': vertex,
            'MEMORY_DRAWSEG': drawseg, 'MEMORY_DRAWGROWTH': growth,
            'MEMORY_DRAWLIFE': function(segs, 'R_ReleaseDrawSegScales') + function(segs, 'R_AllocDrawSegFrontScale'),
            'MEMORY_DRAWSTORE': intersections, 'MEMORY_RAY': rayhelpers,
            'MEMORY_MATH': ''.join(function(math, n) for n in ('FixedToFloat', 'PS2_FloatToI32', 'FloatToFixed',
                                                              'FixedToDouble', 'PS2_DoubleToI32', 'DoubleToFixed')),
            'MEMORY_LOAD': ''.join(function(setup, n) for n in ('P_LoadVertices', 'ParseTextmapVertexParameter', 'P_LoadExtraVertices')),
            'MEMORY_TEXTMAP': udmf, 'MEMORY_HASH': hashing,
            'MEMORY_SLOPE': function(slopes, 'SpawnVertexSlopes'),
            'MEMORY_LUA': section(lua, 'enum vertex_e', '\n};') + '\n};\n' + function(lua, 'vertex_get') + function(lua, 'vertex_num'),
            'MEMORY_POLY': function(poly, 'Polyobj_addVertex') + function(poly, 'Polyobj_rotatePoint')}


def stock_manifest(destination, pakdir):
    """Preserve literal traditional coordinates or ordered UDMF vertex parameters.

    Archive/decompression/token boundaries are outside this layout change; all
    replayed values still pass the actual production vertex loaders/defaults.
    """
    maps, packs = [], {}
    with destination.open('wb') as output:
        output.write(b'\0' * 4)
        for pakname in ('SRB2.PAK', 'ZONES.PAK', 'CHARS.PAK', 'MUSIC.PAK'):
            path = pakdir / pakname
            packs[pakname] = hashlib.sha256(path.read_bytes()).hexdigest()
            with path.open('rb') as stream:
                h = struct.unpack('<4s11I', stream.read(48))
                stream.seek(h[5])
                entries = [struct.unpack('<6I', stream.read(24)) for _ in range(h[4])]
                stream.seek(h[6])
                pool = stream.read(h[7])
                for e in entries:
                    name = pool[e[3]:pool.index(0, e[3])].decode('ascii')
                    if not name.lower().startswith('maps/') or not name.lower().endswith('.wad'):
                        continue
                    data = decode(stream, e[0], e[1], e[2], e[5], h[10])
                    magic, count, directory = struct.unpack_from('<4sII', data)
                    assert magic in (b'IWAD', b'PWAD'), name
                    lumps = [struct.unpack_from('<II8s', data, directory + 16*i) for i in range(count)]
                    byname = {n.rstrip(b'\0'): data[p:p+s] for p, s, n in lumps}
                    def string(s):
                        raw = s.encode('ascii')
                        return struct.pack('<I', len(raw)) + raw
                    output.write(string(pakname + ':' + name))
                    if b'TEXTMAP' in byname:
                        text = byname[b'TEXTMAP'].decode('ascii')
                        # Strings are kept atomic so comment tokens inside them
                        # are not mistaken for real comments.
                        cleaned = re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*[\s\S]*?\*/',
                                         lambda m: m.group() if m.group().startswith('"') else ' ', text)
                        blocks = re.findall(r'\bvertex\s*\{([^{}]*)\}', cleaned)
                        assert blocks, name
                        output.write(struct.pack('<II', 1, len(blocks)))
                        for block in blocks:
                            params = [(k, v.strip()) for k, v in re.findall(r'(\w+)\s*=\s*([^;]+);', block)
                                      if k in ('x', 'y', 'zfloor', 'zceiling')]
                            assert {'x', 'y'} <= {p[0] for p in params}, name
                            output.write(struct.pack('<I', len(params)))
                            for k, v in params:
                                output.write(string(k) + string(v))
                        n, kind, raw = len(blocks), 'udmf', byname[b'TEXTMAP']
                    else:
                        raw = byname[b'VERTEXES']
                        assert len(raw) % 4 == 0
                        n, kind = len(raw) // 4, 'doom'
                        output.write(struct.pack('<II', 0, n) + raw)
                    maps.append({'pack': pakname, 'name': name, 'kind': kind, 'vertices': n,
                                 'source_lump_sha256': hashlib.sha256(raw).hexdigest(),
                                 'EE_payload_before': n*20, 'EE_payload_after': n*20,
                                 'EE_payload_saving': 0})
        output.seek(0)
        output.write(struct.pack('<I', len(maps)))
    return {'maps': maps, 'pack_sha256': packs,
            'manifest_sha256': hashlib.sha256(destination.read_bytes()).hexdigest()}


def run(command, log, negative=False):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True, timeout=240)
    log.write_text(result.stdout + result.stderr)
    if not negative:
        assert result.returncode == 0, str(log) + ': ' + result.stdout[-1000:] + result.stderr[-4000:]
    return result


def rows(text):
    return [dict(re.findall(r'(\w+)=([\da-f]+)', line)) for line in text.splitlines() if 'MEMORY10 case=' in line]


def compare_rows(before, after):
    assert len(before) == len(after)
    for old, new in zip(before, after):
        assert old['case'] == new['case'] and old['kind'] == new['kind']
        # Moving growth and several live roots can alter alignment/placement;
        # only pure array/loader/stock cases have a component peak ordering.
        if old['kind'] in ('0', '1', '2', '6', '7'):
            for key in ('request', 'used', 'peak'):
                if key in old:
                    assert int(new[key]) <= int(old[key]), (old, new)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, default=ROOT / 'build/nov15-next10/baseline-src')
    parser.add_argument('--candidate', type=Path, default=ROOT)
    parser.add_argument('--out', type=Path, default=ROOT / 'build/nov15-next10/memory/host')
    parser.add_argument('--pak', type=Path, default=ROOT / 'build/pak')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--negative-controls', action='store_true')
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    interfaces = {}
    for label, root in [('baseline', args.baseline.resolve()), ('current', args.candidate.resolve())]:
        result = subprocess.run(['cc', '-E', '-P', '-DNOHW', '-I' + str(root / 'src'),
                                 '-I' + str(ROOT / 'build/host-gen'), '-xc', '-'],
                                input='#include "doomdef.h"\n#include "r_defs.h"\n', text=True, capture_output=True)
        (args.out / (label + '-nonps2.i')).write_text(result.stdout)
        (args.out / (label + '-nonps2.log')).write_text(result.stderr)
        assert result.returncode == 0, result.stderr
        interfaces[label] = result.stdout
    assert interfaces['baseline'] == interfaces['current'], 'non-PS2 header interface changed'
    manifest = args.out / 'stock.bin'
    stock = stock_manifest(manifest, args.pak)
    (args.out / 'stock.json').write_text(json.dumps(stock, indent=2) + '\n')
    variants = [('baseline', args.baseline.resolve(), []), ('current', args.candidate.resolve(), []),
                ('old-switch', args.candidate.resolve(), ['-DPS2_NOOPT_DRAWSEG_XY'])]
    if args.sanitize:
        variants.append(('asan', args.candidate.resolve(), ['-fsanitize=address,undefined', '-fno-sanitize=shift-base',
                         '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']))
    mutations = {
        'negative-xy': ('MEMORY_DRAWSTORE', 'ds_p->rightpos.y = segright.y;', 'ds_p->rightpos.y = segright.y ^ 0x80000000u;'),
        'negative-pointer': ('MEMORY_DRAWGROWTH', 'curdrawsegs = drawsegs + curpos;', 'curdrawsegs = drawsegs + (curpos ? curpos - 1 : 0);'),
        'negative-first': ('MEMORY_DRAWGROWTH', 'firstseg = drawsegs + firstpos;', 'firstseg = drawsegs;'),
        'negative-width': ('MEMORY_DRAWLIFE', 'ds->frontscalewidth = viewwidth;', 'ds->frontscalewidth = viewwidth - 1;'),
        'negative-copy': ('FIXTURE', 'M_Memcpy(copy, values, count * sizeof *copy);',
                          'M_Memcpy(copy, values, (count ? count - 1 : 0) * sizeof *copy);')}
    if args.negative_controls:
        variants += [(name, args.candidate.resolve(), []) for name in mutations]
    reports, snapshots, sources = {}, {}, {}
    fixture_path = TOOLS / 'next10_memory_hosttest.c'
    for label, root, flags in variants:
        work = args.out / label
        work.mkdir(exist_ok=True)
        sourceparts, fixture = parts(root), fixture_path.read_text()
        if label in mutations:
            key, old, new = mutations[label]
            if key == 'FIXTURE':
                assert fixture.count(old) == 1
                fixture = fixture.replace(old, new, 1)
            else:
                assert sourceparts[key].count(old) == 1, label
                sourceparts[key] = sourceparts[key].replace(old, new, 1)
        defines, hashes = [], {}
        for macro, content in sourceparts.items():
            path = work / (macro.lower() + '.inc')
            path.write_text(content)
            defines.append('-D' + macro + '="' + str(path) + '"')
            hashes[macro] = hashlib.sha256(content.encode()).hexdigest()
        source = work / 'fixture.c'
        source.write_text(fixture)
        binary = work / 'fixture'
        run(['cc', '-std=gnu17', '-O2', '-g', '-fwrapv', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-function', '-Wno-unused-variable', '-Wno-sign-compare', '-Wno-clobbered', '-DPARANOIA',
             '-I' + str(TOOLS), '-I' + str(ROOT / 'src'), *flags, *defines, source, '-o', binary], work / 'build.log')
        result = run([binary, manifest, work / 'snapshot.bin'], work / 'run.log', negative=label in mutations)
        if label in mutations:
            assert 'MEMORY10 BEGIN' in result.stdout, label + ' did not execute'
            assert result.returncode or (work / 'snapshot.bin').read_bytes() != snapshots['baseline'], label + ' escaped'
            reports[label] = {'mutation_detected': True, 'exit_code': result.returncode}
            print(label, 'detected', flush=True)
            continue
        assert 'runtime error:' not in result.stderr and 'ERROR: AddressSanitizer' not in result.stderr, result.stderr
        tail = re.search(r'MEMORY10 DONE cases=(\d+) checks=(\d+) stockmaps=(\d+) stockvertices=(\d+) sizeof=(\d+) align=(\d+) drawseg=(\d+)', result.stdout)
        assert tail, result.stdout[-2000:]
        reports[label] = dict(zip(('cases', 'checks', 'stockmaps', 'stockvertices', 'sizeof', 'align', 'drawseg'), map(int, tail.groups())))
        reports[label]['rows'] = rows(result.stdout)
        snapshots[label] = (work / 'snapshot.bin').read_bytes()
        sources[label] = hashes
        print(label, tail.group(), flush=True)
    assert snapshots['baseline'] == snapshots['current'] == snapshots['old-switch'], 'literal vertex/consumer snapshots differ'
    if args.sanitize:
        assert snapshots['asan'] == snapshots['baseline']
    assert reports['baseline'] == reports['old-switch'], 'opt-out state/allocator rows differ'
    compare_rows(reports['baseline']['rows'], reports['current']['rows'])
    assert reports['baseline']['sizeof'] == reports['current']['sizeof'] == 20
    assert reports['baseline']['drawseg'] - reports['current']['drawseg'] == 24
    report = {'results': reports, 'source_sha256': sources,
              'fixture_sha256': hashlib.sha256(fixture_path.read_bytes()).hexdigest(),
              'literal_compared_bytes': len(snapshots['baseline']),
              'snapshot_sha256': hashlib.sha256(snapshots['baseline']).hexdigest(),
              'stock_manifest_sha256': stock['manifest_sha256'], 'stock_maps': stock['maps'],
              'nonps2_preprocessed_interface_equal': True, 'old_switch_all_rows_equal': True,
              'all_literal_snapshots_equal': True,
              'sanitizer_scope': 'ASan/UBSan; only shift-base disabled for unchanged traditional signed coordinate <<16 conversion, with negative/extreme loader values retained.',
              'scope': 'Actual drawseg type/growth/pointer repair/intersection statements/frontscale lifecycle and real allocator; all drawseg fields and endpoint full bits retained. Persistent map vertex type unchanged; actual loaders/defaults/extra nodes, slope selection, Lua getters, polyobject growth/copies and loadhash also replayed. Tokenizer, Lua VM, slope geometry, angle-table services and archive boundaries controlled. EE/full game checks separate.'}
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Literal snapshot bytes:', report['literal_compared_bytes'], report['snapshot_sha256'], flush=True)


if __name__ == '__main__':
    main()
