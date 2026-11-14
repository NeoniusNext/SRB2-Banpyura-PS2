"""Audit actual r_draw.c object controls and complete release-LTO drawer code."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'
sys.path.insert(0, str(TOOLS))
import build as B
from next6_render_npot_objectcheck import sections, totals, digest, run
from next7_render_fog_hosttest import NAMES, function
TRACKED = ['planezlight', 'ds_x1', 'ds_colormap', 'colormaps', 'ds_transmap', 'dc_colormap']


def function_info(asm, offsets=None):
    instructions = []
    for line in asm.splitlines():
        m = re.match(r'^[ \t]*([0-9a-f]+):[ \t]+[0-9a-f]{8}[ \t]+(\w+)[ \t]*(.*)$', line)
        if m:
            item = {'address': int(m[1], 16), 'opcode': m[2], 'operands': m[3]}
            if offsets is not None:
                gp = re.search(r'(-?\d+)\(gp\)', m[3])
                if gp and int(gp[1]) in offsets:
                    item['global'] = offsets[int(gp[1])]
            instructions.append(item)
        else:
            m = re.search(r'R_MIPS_GPREL16\s+(\S+)', line)
            if m and instructions and m[1] in TRACKED:
                instructions[-1]['global'] = m[1]
    loops = []
    for ins in instructions:
        target = re.search(r'([0-9a-f]+) <', ins['operands'])
        if not target or not (ins['opcode'].startswith('b') or ins['opcode'] == 'j'):
            continue
        begin, end = int(target[1], 16), ins['address']
        if begin >= end or end - begin > 0x1000:
            continue
        inside = [i for i in instructions if begin <= i['address'] <= end + 4]
        if not any(i['opcode'] == 'sb' for i in inside):
            continue
        loops.append({'begin': hex(begin), 'end': hex(end),
                      'parameter_global_loads': [i for i in inside if 'global' in i and i['opcode'] in ('lw', 'lhu')],
                      'cursor_stores': [i for i in inside if i.get('global') == 'ds_x1' and i['opcode'] == 'sw'],
                      'stack_accesses': [i for i in inside if '(sp)' in i['operands']]})
    frame = re.search(r'addiu\s+sp,sp,(-\d+)', asm)
    return {'stack_frame': -int(frame[1]) if frame else 0, 'pixel_loops': loops,
            'stack_accesses': [i for i in instructions if '(sp)' in i['operands']],
            'calls': [i for i in instructions if i['opcode'] in ('jal', 'jalr')]}


def unchanged(reference, candidate):
    names = reference.keys() | candidate.keys()
    changed = {name for name in names if name.startswith('.text') and reference.get(name) != candidate.get(name)}
    if changed != {'.text.' + name for name in NAMES}:
        raise SystemExit('Unexpected changed text sections: ' + str(sorted(changed)))
    bss = [{name: size for name, (size, _) in d.items() if name.startswith(('.bss', '.sbss'))}
           for d in (reference, candidate)]
    rodata = [Counter((size, hashlib.sha256(data).hexdigest()) for name, (size, data) in d.items()
                      if name.startswith('.rodata')) for d in (reference, candidate)]
    if bss[0] != bss[1] or rodata[0] != rodata[1]:
        raise SystemExit('BSS layout or rodata contents changed')
    if any(reference.get(name) != candidate.get(name) for name in names if name.startswith(('.data', '.sdata'))):
        raise SystemExit('Data contents changed')
    return {'only_changed_text_sections': sorted(changed), 'all_other_text_identical': True,
            'rodata_contents_multiset_identical': True, 'all_data_identical': True,
            'all_bss_names_sizes_identical': True, 'nobits_compared_as_sizes_only': True}


def object_report(work):
    report, objects = {}, {}
    for policy in ('profile', 'noopt-draw'):
        report[policy] = {}
        for variant in ('reference', 'candidate'):
            obj = work / (policy + '-' + variant + '.o')
            info = sections(obj, work)
            objects[policy + '-' + variant] = info
            data = totals(info)
            if policy == 'profile':
                data['functions'] = {}
                for name in NAMES:
                    asm = run(['mips64r5900el-ps2-elf-objdump', '-dr', '--disassemble=' + name, obj],
                              work / (variant + '-' + name + '.asm'))
                    data['functions'][name] = function_info(asm)
                    data['functions'][name]['text_bytes'] = info['.text.' + name][0]
            else:
                pre = (work / (variant + '-noopt.i')).read_text()
                bodies = re.sub(r'\s+', '', ''.join(function(pre, name) for name in NAMES))
                data['preprocessed_drawers_sha256'] = hashlib.sha256(bodies.encode()).hexdigest()
                data['code_sha256'] = digest(info)
            report[policy][variant] = data
    for key in ('code_sha256', 'preprocessed_drawers_sha256'):
        if report['noopt-draw']['reference'][key] != report['noopt-draw']['candidate'][key]:
            raise SystemExit('NOOPT_DRAW code or drawer tokens differ: ' + key)
    report['noopt-draw']['identical'] = True
    report['unchanged_sections'] = unchanged(objects['profile-reference'], objects['profile-candidate'])
    for name, data in report['profile']['candidate']['functions'].items():
        if not data['pixel_loops'] or any(loop['parameter_global_loads'] or loop['cursor_stores']
                                         for loop in data['pixel_loops']):
            raise SystemExit('Candidate object retains hot metadata traffic: ' + name)
    return report


def linked_report(elf, variant, work):
    symbols = run(['mips64r5900el-ps2-elf-nm', '-n', elf], work / (variant + '-nm.log'))
    addresses = {m[2]: int(m[1], 16) for m in re.finditer(r'^([0-9a-f]+) [A-Za-z] (\S+)$', symbols, re.M)}
    offsets = {addresses[name] - addresses['_gp']: name for name in TRACKED if name in addresses}
    report = {'sha256': hashlib.sha256(elf.read_bytes()).hexdigest(), 'functions': {}}
    for name in NAMES:
        asm = run(['mips64r5900el-ps2-elf-objdump', '-d', '--disassemble=' + name, elf],
                  work / (variant + '-' + name + '.asm'))
        report['functions'][name] = function_info(asm, offsets)
    if variant == 'candidate-elf':
        loops = [loop for data in report['functions'].values() for loop in data['pixel_loops']]
        if len(loops) != 7 or any(loop['parameter_global_loads'] or loop['cursor_stores'] or loop['stack_accesses'] for loop in loops):
            raise SystemExit('Final LTO ELF retains hot metadata traffic/stack accesses')
        report['audit'] = {'pixel_loops': len(loops), 'zero_loop_metadata_loads': True,
                           'zero_loop_cursor_stores': True, 'zero_loop_stack_accesses': True}
    return report


def concise(report):
    result = {}
    for variant, data in report.items():
        if variant not in ('profile', 'baseline-elf', 'candidate-elf'):
            result[variant] = data
        elif variant == 'profile':
            result[variant] = {v: {**{k: d[k] for k in ('text', 'rodata', 'data', 'bss')},
                                   'functions': {n: {'text_bytes': f['text_bytes'], 'frame': f['stack_frame'],
                                                     'metadata_loads': [len(x['parameter_global_loads']) for x in f['pixel_loops']],
                                                     'cursor_stores': [len(x['cursor_stores']) for x in f['pixel_loops']]}
                                                 for n, f in d['functions'].items()}}
                               for v, d in data.items()}
        else:
            result[variant] = {'sha256': data['sha256'], 'functions': {
                n: {'frame': f['stack_frame'], 'metadata_loads': [len(x['parameter_global_loads']) for x in f['pixel_loops']],
                    'cursor_stores': [len(x['cursor_stores']) for x in f['pixel_loops']]}
                for n, f in data['functions'].items()}}
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--base-src', required=True, type=Path)
    p.add_argument('--inspect-only', action='store_true')
    p.add_argument('--elf-only', action='store_true')
    p.add_argument('--baseline-elf', type=Path)
    p.add_argument('--candidate-elf', type=Path)
    a = p.parse_args()
    work = a.out.resolve(); work.mkdir(parents=True, exist_ok=True)
    if not a.inspect_only and not a.elf_only:
        flags = [f for f in B.CFLAGS if f not in ('-flto', '-MMD', '-MP')]
        for policy, extra in [('profile', []), ('noopt-draw', ['-DPS2_NOOPT_DRAW'])]:
            for variant, directory in [('reference', a.base_src.resolve()), ('candidate', ROOT / 'src')]:
                name = policy + '-' + variant
                run([B.CC, *flags, *extra, '-Werror', *B.INCS, '-c', directory / 'r_draw.c', '-o', work / (name + '.o')],
                    work / (name + '-build.log'))
                if policy == 'noopt-draw':
                    output = run([B.CC, *flags, *extra, *B.INCS, '-E', '-P', directory / 'r_draw.c'],
                                 work / (variant + '-noopt-preprocess.log'))
                    (work / (variant + '-noopt.i')).write_text(output)
    report = {} if a.elf_only else object_report(work)
    for variant, elf in [('baseline-elf', a.baseline_elf), ('candidate-elf', a.candidate_elf)]:
        if elf:
            report[variant] = linked_report(elf.resolve(), variant, work)
    filename = 'elf-report.json' if a.elf_only else 'report.json'
    (work / filename).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(concise(report), indent=2))


if __name__ == '__main__':
    main()
