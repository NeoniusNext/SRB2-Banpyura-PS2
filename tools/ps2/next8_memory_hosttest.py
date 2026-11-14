#!/usr/bin/env python3
"""Actual sprite-frame loader/copies in the real arena, frozen vs reordered."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

from verify_pack import decode

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'
PAKNAMES = ('SRB2.PAK', 'ZONES.PAK', 'CHARS.PAK', 'MUSIC.PAK')


def section(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


def parts(root):
    definitions = (root / 'src/r_defs.h').read_text()
    things = (root / 'src/r_things.c').read_text()
    rotation = (root / 'src/r_patchrotation.c').read_text()
    headers = (root / 'src/r_things.h').read_text()
    limits = (root / 'src/ps2/ps2_limits.c').read_text()
    types = section(definitions, '// posts are runs of non masked source pixels', '//\n// OTHER TYPES')
    types += section(definitions, '#ifdef ROTSPRITE\ntypedef struct\n{\n\tINT32 angles;', '#if defined(_MSC_VER)')
    types += definitions[definitions.index('typedef enum\n{\n\tSRF_SINGLE'):].rsplit('\n#endif', 1)[0]
    helpers = section(headers, 'FUNCMATH FUNCINLINE static ATTRINLINE char R_Frame2Char', '#endif //__R_THINGS__')
    loader = section(things, 'static void R_InstallSpriteLump(', '// Install a single sprite, given its identifying name')
    loader += section(things, '#ifdef PS2_PROFILE\n// PS2-LOAD-4:', 'static void AddShortSpriteDefs(')
    growth = section(limits, '\t\tspriteframe_t *grown = Z_Malloc(', '\n\t}\n\n\tps2_fulllimits = true;')
    rotating = section(rotation, 'patch_t *Patch_GetRotatedSprite(', '\nvoid Patch_Rotate(')
    rotating += section(rotation, 'rotsprite_t *RotatedPatch_Create(', '\nstatic void RotatedPatch_CalculateDimensions(')
    zone = (TOOLS / 'zone_hosttest.c').read_text().split('static void alignment_and_cycles(void)', 1)[0] + '\n#endif\n'
    zone = zone.replace('#include "../../src/z_zone.c"', '#include "' + str(ROOT / 'src/z_zone.c') + '"')
    zone = zone.replace('#include "../../src/ps2/ps2_mem.c"', '#include "' + str(ROOT / 'src/ps2/ps2_mem.c') + '"')
    backing = '\tif (arena_raw)\n\t\taligned_release(arena_raw);\n\tarena_raw = memalign(64, bytes + 65536 + arena_skew);'
    assert zone.count(backing) == 1
    arena_helper = '''#ifdef __mips__
static unsigned char ee_backing[(16u << 20) + 65536 + 32] __attribute__((aligned(64)));
static int ee_backing_ready;
static void memory_backing_check(void)
{
    if (!ee_backing_ready) return;
    for (const unsigned char *p = ee_backing; p < arena_mem; p++)
        if (*p != 0xA6) check(0, "EE arena prefix canary");
    for (const unsigned char *p = arena_mem + (16u << 20); p < ee_backing + sizeof ee_backing; p++)
        if (*p != 0xA6) check(0, "EE arena suffix canary");
    check(1, "complete EE prefix/suffix canaries");
}
#else
static void memory_backing_check(void) {}
#endif

'''
    zone = zone.replace('static void arena_reset(size_t bytes, int twosided, int prefer, int redzone)',
                        arena_helper + 'static void arena_reset(size_t bytes, int twosided, int prefer, int redzone)')
    zone = zone.replace(backing, '\tmemory_backing_check();\n#ifdef __mips__\n'
                        '\tcheck(bytes == (16u << 20) && !arena_skew, "fixed EE arena backing bounds");\n'
                        '\tarena_raw = ee_backing + 16;\n#else\n' + backing + '\n#endif')
    guard = '''#ifdef __mips__
    check(arena_mem >= ee_backing + 16 && arena_mem + bytes <= ee_backing + sizeof ee_backing - 16,
        "EE arena has complete declared capacity and exterior guards");
    memset(ee_backing, 0xA6, arena_mem - ee_backing);
    memset(arena_mem + bytes, 0xA6, ee_backing + sizeof ee_backing - (arena_mem + bytes));
    ee_backing_ready = 1;
#endif
'''
    zone = zone.replace('\tmemset(arena_mem, 0xCD, bytes);', guard + '\tmemset(arena_mem, 0xCD, bytes);')
    return {'MEMORY_ZONE': zone, 'MEMORY_TYPES': types, 'MEMORY_HELPERS': helpers, 'MEMORY_LOADER': loader,
            'MEMORY_GROWTH': growth, 'MEMORY_ROTATION': rotating}


def stock_manifest(destination, pakdir):
    info = (ROOT / 'src/info.c').read_text()
    sprite_names = re.findall(r'^\s*"([^"]+)"', section(info, '\n{\n\t"NULL",', '\n};'), re.M)
    player_names = re.findall(r'^\s*"([^"]+)"', section(info, '\n{\n\t"STND",', '\n};'), re.M)
    packs, groups, names_to_id = [], [], {}
    for pakname in PAKNAMES:
        with (pakdir / pakname).open('rb') as stream:
            h = struct.unpack('<4s11I', stream.read(48))
            count, table, poolpos, poolsize, block = h[4], h[5], h[6], h[7], h[10]
            stream.seek(table)
            entries = [struct.unpack('<6I', stream.read(24)) for _ in range(count)]
            stream.seek(poolpos)
            pool = stream.read(poolsize)
            rows = []
            for position, disk, size, fullname, longname, codec in entries:
                full = pool[fullname:pool.index(0, fullname)].decode('ascii')
                long = pool[longname:pool.index(0, longname)].decode('ascii')
                rows.append({'name': long[:8], 'longname': long, 'fullname': full,
                             'size': size, 'header': [0, 0, 0, 0], 'good': 0,
                             'source': (position, disk, size, codec, block)})
            packs.append(rows)
    def append_group(pack, start, end, name, key):
        if key not in names_to_id:
            names_to_id[key] = len(names_to_id)
        groups.append({'pack': pack, 'start': start, 'end': end, 'name': name, 'id': names_to_id[key], 'key': key})
    for pack, rows in enumerate(packs):
        sprite_ids = [i for i, row in enumerate(rows) if row['fullname'].lower().startswith('sprites/')]
        if sprite_ids:
            start, end = sprite_ids[0], sprite_ids[-1] + 1
            assert all(row['fullname'].lower().startswith('sprites/') for row in rows[start:end])
            for name in sprite_names:
                append_group(pack, start, end, name, 'global:' + name)
        assert not any(row['fullname'].lower().startswith('longsprites/') for row in rows), 'add long stock groups if present'
    for pack, rows in enumerate(packs):
        for skin, marker in enumerate(i for i, row in enumerate(rows) if row['name'].startswith('S_SKIN')):
            start = marker + 1
            end = next((i for i in range(start, len(rows)) if rows[i]['name'] in ('S_END', 'S_START')
                        or rows[i]['name'].startswith(('S_SKIN', 'P_SKIN'))), len(rows))
            supermarker = next((i for i in range(start, end) if rows[i]['name'] == 'S_SUPER'), end)
            for mode, first, last in [('super', supermarker + 1, end), ('normal', start, supermarker)]:
                if first >= last:
                    continue
                for name in player_names:
                    append_group(pack, first, last, name, f'skin:{pack}:{skin}:{mode}:{name}')
    needed = {(group['pack'], index) for group in groups for index in range(group['start'], group['end'])
              if len(packs[group['pack']][index]['name']) in (6, 8)
              and packs[group['pack']][index]['name'][:4] == group['name']}
    for pack, pakname in enumerate(PAKNAMES):
        with (pakdir / pakname).open('rb') as stream:
            for index, row in enumerate(packs[pack]):
                if (pack, index) not in needed or row['size'] <= 8:
                    continue
                data = decode(stream, *row['source'])
                if data.startswith(b'\x89SRPIC\r\n'):
                    data = data[8:]
                assert len(data) >= 8 and not data.startswith(b'\x89PNG'), row['fullname']
                row['header'] = struct.unpack_from('<4h', data)
                row['good'] = 1
    def string(value):
        encoded = value.encode('ascii')
        return struct.pack('<H', len(encoded)) + encoded
    with destination.open('wb') as stream:
        stream.write(struct.pack('<III', len(packs), len(groups), len(names_to_id)))
        for rows in packs:
            stream.write(struct.pack('<I', len(rows)))
            for row in rows:
                stream.write(string(row['name']) + string(row['longname']))
                stream.write(struct.pack('<II4h', row['size'], row['good'], *row['header']))
        for group in groups:
            stream.write(struct.pack('<4I', group['pack'], group['start'], group['end'], group['id']))
            stream.write(string(group['name']))
    return {'packs': [len(pack) for pack in packs], 'groups': groups, 'definition_ids': names_to_id,
            'stock_header_reads': len(needed), 'manifest_sha256': hashlib.sha256(destination.read_bytes()).hexdigest(),
            'pack_sha256': {name: hashlib.sha256((pakdir / name).read_bytes()).hexdigest()
                            for name in PAKNAMES}}


def run(command, log, negative=False):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True, timeout=180)
    log.write_text(result.stdout + result.stderr)
    if not negative:
        assert result.returncode == 0, str(log) + ': ' + result.stdout[-1000:] + result.stderr[-4000:]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, default=ROOT / 'build/nov15-next8/baseline-src')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/nov15-next8/memory/host')
    parser.add_argument('--pak', type=Path, default=ROOT / 'build/pak')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--negative-controls', action='store_true')
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    interfaces = {}
    for label, root in [('baseline', args.baseline.resolve()), ('current', ROOT)]:
        source = '#include "doomdef.h"\n#include "r_defs.h"\n'
        result = subprocess.run(['cc', '-E', '-P', '-DNOHW', '-I' + str(root / 'src'),
                                 '-I' + str(ROOT / 'build/host-gen'), '-xc', '-'],
                                input=source, text=True, capture_output=True)
        (args.out / (label + '-nonps2.i')).write_text(result.stdout)
        (args.out / (label + '-nonps2.log')).write_text(result.stderr)
        assert result.returncode == 0, result.stderr
        interfaces[label] = result.stdout
    assert interfaces['baseline'] == interfaces['current'], 'non-PS2 header interface differs'
    manifest = args.out / 'stock.bin'
    stock = stock_manifest(manifest, args.pak)
    (args.out / 'stock.json').write_text(json.dumps(stock, indent=2) + '\n')
    reports, snapshots, sources = {}, {}, {}
    variants = [('baseline', args.baseline.resolve(), []), ('current', ROOT, []),
                ('old-switch', ROOT, ['-DPS2_NOOPT_SPRFRAME_LAYOUT'])]
    if args.sanitize:
        variants.append(('asan', ROOT, ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']))
    if args.negative_controls:
        variants += [(mutation, ROOT, []) for mutation in ('negative-flip', 'negative-lump', 'negative-copy', 'negative-rotation')]
    for label, root, flags in variants:
        work = args.out / label
        work.mkdir(exist_ok=True)
        sourceparts = parts(root)
        mutations = {'negative-flip': ('MEMORY_LOADER', 'sprtemp[frame].flip = flipped ? 0xFFFF : 0;', 'sprtemp[frame].flip = flipped ? 0x7FFF : 0;'),
                     'negative-lump': ('MEMORY_LOADER', 'sprtemp[frame].lumppat[rotation] = lumppat;', 'sprtemp[frame].lumppat[rotation] = lumppat ^ 0x80000000u;'),
                     'negative-copy': ('MEMORY_LOADER', 'maxframe*sizeof (spriteframe_t));', '(maxframe-1)*sizeof (spriteframe_t));'),
                     'negative-rotation': ('MEMORY_ROTATION', 'sprite->rotated[spriteangle] = rotsprite;', 'sprite->rotated[(spriteangle+1)&15] = rotsprite;')}
        if label in mutations:
            macro, before, after = mutations[label]
            assert sourceparts[macro].count(before) == 1, label
            sourceparts[macro] = sourceparts[macro].replace(before, after, 1)
        defines = []
        for macro, content in sourceparts.items():
            path = work / (macro.lower() + '.inc')
            path.write_text(content)
            defines.append('-D' + macro + '="' + str(path) + '"')
        executable = work / 'test'
        command = ['cc', '-std=gnu17', '-O1' if label == 'asan' else '-O2', '-g', '-fwrapv',
                   '-Wall', '-Wextra', '-Wno-unused-function', '-Wno-sign-compare', '-Wno-unused-variable',
                   '-DPARANOIA', '-I' + str(ROOT / 'src'), '-I' + str(TOOLS),
                   *defines, *flags, TOOLS / 'next8_memory_hosttest.c', '-o', executable]
        run(command, work / 'build.log')
        snapshot = work / 'snapshot.bin'
        result = run([executable, manifest, snapshot], work / 'run.log', label.startswith('negative-'))
        if label.startswith('negative-'):
            assert 'MEMORY8 BEGIN' in result.stdout, label + ' did not execute'
            assert result.returncode or snapshot.read_bytes() != snapshots['baseline'], label + ' escaped literal comparison'
            reports[label] = {'detected': True, 'returncode': result.returncode}
            print(label, 'detected', flush=True)
            continue
        assert 'runtime error:' not in result.stderr and 'ERROR: AddressSanitizer' not in result.stderr
        tail = re.search(r'MEMORY8 DONE cases=(\d+) checks=(\d+) stockframes=(\d+) stockroots=(\d+) sizeof=(\d+) align=(\d+)', result.stdout)
        assert tail, result.stdout[-1000:]
        reports[label] = dict(zip(['cases', 'checks', 'stockframes', 'stockroots', 'sizeof', 'align'], map(int, tail.groups())))
        reports[label]['rows'] = [dict(re.findall(r'(\w+)=([\da-f]+)', line)) for line in result.stdout.splitlines() if line.startswith('MEMORY8 case=')]
        reports[label]['stock_definition_counts'] = dict((int(i), int(n)) for i, n in re.findall(r'^MEMORY8 stockdef=(\d+) frames=(\d+)$', result.stdout, re.M))
        snapshots[label] = snapshot.read_bytes()
        sources[label] = {macro: hashlib.sha256(content.encode()).hexdigest() for macro, content in sourceparts.items()}
        print(label, tail.group(), flush=True)
    assert snapshots['baseline'] == snapshots['current'] == snapshots['old-switch']
    if args.sanitize:
        assert snapshots['asan'] == snapshots['current']
    assert reports['old-switch'] == reports['baseline']
    for before, after in zip(reports['baseline']['rows'], reports['current']['rows']):
        assert before.keys() == after.keys()
        for key in before:
            if key in ('used', 'peak', 'request'):
                assert int(after[key]) <= int(before[key]), (before, after)
            else:
                assert before[key] == after[key], (before, after)
    frames_by_key = {key: reports['current']['stock_definition_counts'][index]
                     for key, index in stock['definition_ids'].items()}
    scoped = {}
    for scope in ('global', 'skin'):
        counts = [count for key, count in frames_by_key.items() if key.startswith(scope + ':') and count]
        minimum = lambda stride: sum((16 + count * stride + 15) & ~15 for count in counts)
        scoped[scope] = {'frames': sum(counts), 'nonempty_roots': len(counts),
                         'EE_requested_before': sum(counts) * 108, 'EE_requested_after': sum(counts) * 104,
                         'EE_minimum_blocks_before': minimum(108), 'EE_minimum_blocks_after': minimum(104)}
    report = {'results': reports, 'compared_literal_bytes': len(snapshots['baseline']),
              'snapshot_sha256': hashlib.sha256(snapshots['current']).hexdigest(),
              'source_sha256': sources, 'stock_manifest_sha256': stock['manifest_sha256'],
              'stock_EE_frame_payload_saving': 4 * reports['current']['stockframes'],
              'host_pointer_bytes': struct.calcsize('P'), 'complete_literal_outputs_equal': True,
              'loader_replay_scopes': scoped, 'stock_frame_counts_by_key': frames_by_key,
              'nonps2_header_interface_equal': True,
              'nonps2_header_interface_sha256': hashlib.sha256(interfaces['current'].encode()).hexdigest()}
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Literal bytes equal:', report['compared_literal_bytes'], 'stock EE payload saving:', report['stock_EE_frame_payload_saving'], flush=True)


if __name__ == '__main__':
    main()
