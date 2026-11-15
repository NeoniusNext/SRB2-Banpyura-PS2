#!/usr/bin/env python3
"""Actual full-width slope metadata, lifetime, pool, save and consumer proof."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from next8_memory_hosttest import parts as sprite_parts
from next9_memory_hosttest import section
from next11_memory_replay import validate_candidate

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'


def function(source, name):
    match = re.search(r'^[^\n;{}]*\b' + re.escape(name) + r'\s*\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    depth = 0
    tokens = re.finditer(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', source[match.start():])
    for token in tokens:
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if not depth:
                return source[match.start():match.start() + token.end()] + '\n'
    raise AssertionError(name)


def parts(root):
    definitions = (root / 'src/r_defs.h').read_text()
    fixed = (root / 'src/m_fixed.h').read_text()
    vector = (root / 'src/m_vector.h').read_text()
    slopes = (root / 'src/p_slopes.c').read_text()
    mobj = (root / 'src/p_mobj.c').read_text()
    save = (root / 'src/p_saveg.c').read_text()
    lua = (root / 'src/lua_maplib.c').read_text()
    plane = (root / 'src/r_plane.c').read_text()
    cache = (root / 'src/hardware/hw_gcache.inc').read_text()
    zone = sprite_parts(root)['MEMORY_ZONE']
    # The shared old allocator fixture used int for boolean. Production uses
    # stdbool/C23 bool, which both actual header and target ABI checks confirm.
    assert zone.count('typedef int boolean;') == 1
    zone = zone.replace('typedef int boolean;', 'typedef _Bool boolean;')
    zone = zone.replace(str(ROOT / 'src/z_zone.c'), str(root / 'src/z_zone.c'))
    zone = zone.replace(str(ROOT / 'src/ps2/ps2_mem.c'), str(root / 'src/ps2/ps2_mem.c'))
    types = section(fixed, 'typedef struct\n{\n\tfixed_t x;', '\nvector2_t *FV2_Load')
    types += section(fixed, 'typedef struct\n{\n\tfixed_t x, y, z;', '\nvector3_t *FV3_Load')
    types += section(vector, 'typedef struct', '\nvoid DVector3_Load')
    types += section(definitions, 'typedef struct pslope_s', '\ntypedef enum')
    blocks = [m.start() for m in re.finditer(re.escape('\tif (diff2 & MD2_FLOORSPRITESLOPE)\n\t{'), save)]
    assert len(blocks) == 2
    wrappers = []
    for name, start in zip(('actual_save_floor', 'actual_load_floor'), blocks):
        end = save.index('\n\tif (diff2 & MD2_DRAWONLYFORPLAYER)', start)
        wrappers.append('static void ' + name + '(save_t *save_p, mobj_t *mobj, UINT32 diff2)\n{\n'
                        + save[start:end] + '\n}\n')
    return {'MEMORY_ZONE': zone, 'MEMORY_TYPES': types,
            'MEMORY_FIXED': function(fixed, 'FixedMul'),
            'MEMORY_SLOPES': '#define MEMORY_SOURCE_BYTE_POOL ' + str(int('PS2_NOOPT_SLOPE_POOL_BYTES' in function(slopes, 'Slope_Add'))) + '\n'
                + ''.join(function(slopes, n) for n in ('Slope_Add', 'P_SlopeById', 'P_GetSlopeZAt', 'P_GetZAt')),
            'MEMORY_FLOOR': ''.join(function(mobj, n) for n in ('P_CreateFloorSpriteSlope', 'P_RemoveFloorSpriteSlope')),
            'MEMORY_SAVE': function(save, 'SaveSlope') + ''.join(wrappers),
            'MEMORY_LUA': section(lua, 'enum slope_e {', '\n};') + '\n};\n' + function(lua, 'slope_get'),
            'MEMORY_RENDER': function(plane, 'R_GetSlopeZAt') + function(cache, 'gc_slope')}


def run(command, log, negative=False):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True, timeout=300)
    log.write_text(result.stdout + result.stderr)
    if not negative:
        assert result.returncode == 0, str(log) + ': ' + result.stdout[-1000:] + result.stderr[-4000:]
    return result


def rows(text):
    return [dict(re.findall(r'(\w+)=([\da-f]+)', line)) for line in text.splitlines() if 'MEMORY11 case=' in line]


def compare_rows(before, after):
    assert len(before) == len(after)
    for old, new in zip(before, after):
        assert old['case'] == new['case'] and old['kind'] == new['kind'] and old['n'] == new['n']
        # These pure arrays, byte-budget pool construction and standalone allocations
        # have no competing cache work. Whole-game placement is a separate gate.
        if old['kind'] in ('0', '1', '2', '3'):
            for key in ('request', 'used', 'peak'):
                assert int(new[key]) <= int(old[key]), (old, new)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, default=ROOT / 'build/nov15-next11/baseline-src')
    parser.add_argument('--candidate', type=Path, default=ROOT / 'build/nov15-next11/memory/byte-pool/candidate-src')
    parser.add_argument('--out', type=Path)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--negative-controls', action='store_true')
    args = parser.parse_args()
    stage = validate_candidate(args.candidate)
    args.out = (args.out or ROOT / ('build/nov15-next11/memory/replay-' + stage + '/host')).resolve()
    if not args.out.is_relative_to(ROOT / 'build') or args.out == ROOT / 'build':
        parser.error('--out must be an isolated child of build/')
    if (args.out / 'report.json').exists():
        parser.error('Completed host proof exists; choose a fresh --out to preserve its evidence')
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
    variants = [('baseline', args.baseline.resolve(), []), ('current', args.candidate.resolve(), []),
                ('old-switch', args.candidate.resolve(), ['-DPS2_NOOPT_SLOPE_LAYOUT']),
                ('pool-old', args.candidate.resolve(), ['-DPS2_NOOPT_SLOPE_POOL_BYTES'])]
    if args.sanitize:
        variants.append(('asan', args.candidate.resolve(), ['-fsanitize=address,undefined',
                         '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']))
    mutations = {
        'negative-flag': ('MEMORY_SLOPES', 'ret->flags = flags;', 'ret->flags = flags & 0x7f;'),
        'negative-id': ('MEMORY_SLOPES', 'ret->id = slopecount;', 'ret->id = slopecount & 0x7fff;'),
        'negative-next': ('MEMORY_SLOPES', 'ret->next = slopelist;', 'ret->next = NULL;'),
        'negative-save': ('MEMORY_SAVE', 'P_WriteAngle(save_p, slope->xydirection);',
                          'P_WriteAngle(save_p, slope->xydirection ^ 0x80000000u);'),
        'negative-copy': ('FIXTURE', 'M_Memcpy(copy, values, count * sizeof *copy);',
                          'M_Memcpy(copy, values, (count ? count - 1 : 0) * sizeof *copy);'),
        'negative-quota': ('MEMORY_SLOPES', 'const unsigned perchunk = 8192 / stride ? (unsigned)(8192 / stride) : 1;',
                           'const unsigned perchunk = 8192 / stride ? (unsigned)(8192 / stride) + 1 : 1;')}
    if stage == 'header-only':
        del mutations['negative-quota']  # The historical constructor had no byte-budget expression.
    if args.negative_controls:
        variants += [(name, args.candidate.resolve(), []) for name in mutations]
    reports, snapshots, sources = {}, {}, {}
    fixture_path = TOOLS / 'next11_memory_hosttest.c'
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
            defines.append('-D' + macro + '=\"' + str(path) + '\"')
            hashes[macro] = hashlib.sha256(content.encode()).hexdigest()
        source = work / 'fixture.c'
        source.write_text(fixture)
        binary = work / 'fixture'
        run(['cc', '-std=gnu17', '-O2', '-g', '-fwrapv', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-function', '-Wno-unused-variable', '-Wno-sign-compare', '-Wno-clobbered', '-DPARANOIA',
             '-I' + str(TOOLS), '-I' + str(ROOT / 'src'), *flags, *defines, source, '-o', binary], work / 'build.log')
        result = run([binary, work / 'snapshot.bin'], work / 'run.log', negative=label in mutations)
        if label in mutations:
            assert 'MEMORY11 BEGIN' in result.stdout, label + ' did not execute'
            assert result.returncode or (work / 'snapshot.bin').read_bytes() != snapshots['baseline'], label + ' escaped'
            reports[label] = {'mutation_detected': True, 'exit_code': result.returncode}
            print(label, 'detected', flush=True)
            continue
        assert 'runtime error:' not in result.stderr and 'ERROR: AddressSanitizer' not in result.stderr, result.stderr
        tail = re.search(r'MEMORY11 DONE cases=(\d+) checks=(\d+) sizeof=(\d+) align=(\d+) stride=(\d+) slots=(\d+)', result.stdout)
        assert tail, result.stdout[-2000:]
        reports[label] = dict(zip(('cases', 'checks', 'sizeof', 'align', 'stride', 'slots'), map(int, tail.groups())))
        reports[label]['rows'] = rows(result.stdout)
        snapshots[label] = (work / 'snapshot.bin').read_bytes()
        sources[label] = hashes
        print(label, tail.group(), flush=True)
    assert snapshots['baseline'] == snapshots['current'] == snapshots['old-switch'] == snapshots['pool-old'], 'literal slope/consumer snapshots differ'
    if args.sanitize:
        assert snapshots['asan'] == snapshots['baseline']
    assert reports['baseline'] == reports['old-switch'], 'opt-out state/allocator rows differ'
    compare_rows(reports['baseline']['rows'], reports['current']['rows'])
    compare_rows(reports['baseline']['rows'], reports['pool-old']['rows'])
    assert reports['baseline']['sizeof'] == 128 and reports['current']['sizeof'] == 120
    report = {'candidate_stage': stage, 'results': reports, 'source_sha256': sources,
              'fixture_sha256': hashlib.sha256(fixture_path.read_bytes()).hexdigest(),
              'literal_compared_bytes': len(snapshots['baseline']),
              'snapshot_sha256': hashlib.sha256(snapshots['baseline']).hexdigest(),
              'nonps2_preprocessed_interface_equal': True, 'old_switch_all_rows_equal': True,
              'all_literal_snapshots_equal': True,
              'scope': 'Actual slope/vector types, real zone/pool and Slope_Add/id lookup, unchanged8KiB chunk budget with complete naturally aligned slots, full-bit fixed-count field/pointer/double/copy corpus including72/73/74 boundaries. Actual constructor failure after common4672=64x73 boundary preserves every original record/pointer/list/count and retries exactly one new record; complete logical fields are compared literally. Separate per-capacity local-pool epoch/failure invariants assert exact private chunk/used/epoch publication without claiming differing private capacities equal. Standalone floor-sprite replace/free/zero, actual saved floor-sprite write/read blocks and Lua getter, fixed/double height queries and hardware slope fingerprint consumer retained. Old layout returns64 slots; narrow pool optout retains112-byte/64-slot counterfactual onEE. Save byte transport, Lua VM and hash sink are controlled service boundaries. No geometry, interpolation or renderer algorithm changes; full-game images/cache/process checks separate.'}
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Literal snapshot bytes:', report['literal_compared_bytes'], report['snapshot_sha256'], flush=True)


if __name__ == '__main__':
    main()
