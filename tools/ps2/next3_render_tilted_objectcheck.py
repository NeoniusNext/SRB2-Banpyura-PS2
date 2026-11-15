"""Compile real frozen/current r_draw.c and prove the NOOPT_DRAW code identity."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'
sys.path.insert(0, str(TOOLS))
import build as B
from next3_render_tilted_hosttest import NAMES, function


def run(cmd, log):
    result = subprocess.run(list(map(str, cmd)), capture_output=True, text=True, cwd=ROOT)
    log.write_text(' '.join(map(str, cmd)) + '\n' + result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit(f'Command failed: {log}\n{result.stdout}{result.stderr}')
    return result.stdout


def sections(path, out):
    dump = run(['mips64r5900el-ps2-elf-objdump', '-h', path], out / (path.stem + '-sections.log'))
    data = path.read_bytes()
    result = {}
    for m in re.finditer(r'^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)', dump, re.M):
        name, size, offset = m[1], int(m[2], 16), int(m[3], 16)
        result[name] = (size, data[offset:offset+size])
    return result


def totals(items):
    return {kind: sum(size for name, (size, _) in items.items() if name.startswith(prefixes))
            for kind, prefixes in [('text', ('.text',)), ('rodata', ('.rodata',)),
                                   ('data', ('.data', '.sdata')), ('bss', ('.bss', '.sbss'))]}


def digest(items):
    h = hashlib.sha256()
    for name, (size, data) in sorted(items.items()):
        if name.startswith(('.text', '.rodata')):
            h.update(name.encode() + b'\0' + size.to_bytes(8, 'little') + data)
    return h.hexdigest()



def linked_report(elf, variant, work):
    symbols = run(['mips64r5900el-ps2-elf-nm', '-n', elf], work / (variant + '-nm.log'))
    addresses = {m[2]: int(m[1], 16) for m in re.finditer(r'^([0-9a-f]+) [A-Za-z] (\S+)$', symbols, re.M)}
    tracked = ['nflatxshift', 'nflatyshift', 'nflatmask', 'ds_transmap', 'planezlight',
               'ds_colormap', 'colormaps', 'ds_x1']
    offsets = {addresses[name] - addresses['_gp']: name for name in tracked if name in addresses}
    report = {'sha256': hashlib.sha256(elf.read_bytes()).hexdigest(), 'functions': {}}
    for func in NAMES:
        asm = run(['mips64r5900el-ps2-elf-objdump', '-d', '--disassemble=' + func, elf],
                  work / (variant + '-' + func + '.asm'))
        instructions = []
        for m in re.finditer(r'^\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(\w+)\s*(.*)$', asm, re.M):
            instructions.append((int(m[1], 16), m[2], m[3]))
        loops = []
        for address, op, operands in instructions:
            target = re.search(r'([0-9a-f]+) <', operands)
            if op not in ('bne', 'bnez', 'beq', 'beqz', 'bgez', 'bgtz', 'blez', 'bltz') or not target:
                continue
            begin = int(target[1], 16)
            if begin >= address or address - begin > 0x240:
                continue
            inside = [(at, name, args) for at, name, args in instructions if begin <= at <= address + 4]
            if not any(name == 'sb' for _, name, _ in inside):
                continue
            reads = []
            for at, name, args in inside:
                gp = re.search(r'(-?\d+)\(gp\)', args)
                if name == 'lw' and gp and int(gp[1]) in offsets:
                    reads.append({'address': hex(at), 'global': offsets[int(gp[1])]})
            loops.append({'begin': hex(begin), 'end': hex(address), 'bytes': address - begin + 8,
                          'parameter_global_loads': reads})
        stack = re.search(r'addiu\s+sp,sp,(-\d+)', asm)
        report['functions'][func] = {'stack_frame': -int(stack[1]) if stack else 0,
                                     'pixel_loops': loops}
    return report

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--base-src', required=True, type=Path)
    p.add_argument('--baseline-elf', type=Path)
    p.add_argument('--candidate-elf', type=Path)
    p.add_argument('--elf-only', action='store_true')
    args = p.parse_args()
    work = args.out.resolve(); work.mkdir(parents=True, exist_ok=True)
    flags = [x for x in B.CFLAGS if x not in ['-flto', '-MMD', '-MP']]
    if args.elf_only:
        report = {}
        for variant, elf in [('baseline-elf', args.baseline_elf), ('candidate-elf', args.candidate_elf)]:
            if elf:
                report[variant] = linked_report(elf, variant, work)
        (work / 'elf-report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({v: {name: {'stack_frame': data['stack_frame'],
                                      'pixel_loops': len(data['pixel_loops']),
                                      'parameter_loads': [len(loop['parameter_global_loads']) for loop in data['pixel_loops']]}
                              for name, data in elf['functions'].items()} for v, elf in report.items()}, indent=2))
        return
    report = {}
    objects = {}
    for policy, extra in [('profile', []), ('noopt-draw', ['-DPS2_NOOPT_DRAW'])]:
        report[policy] = {}
        for variant, directory in [('reference', args.base_src.resolve()), ('candidate', ROOT / 'src')]:
            name = policy + '-' + variant
            obj = work / (name + '.o')
            run([B.CC, *flags, *extra, '-Werror', *B.INCS, '-c', directory / 'r_draw.c', '-o', obj],
                work / (name + '-build.log'))
            info = sections(obj, work)
            objects[name] = info
            report[policy][variant] = totals(info)
            if policy == 'profile':
                for func in NAMES:
                    run(['mips64r5900el-ps2-elf-objdump', '-dr', '--disassemble=' + func, obj],
                        work / (variant + '-' + func + '.asm'))
            else:
                pre = work / (variant + '-noopt.i')
                output = run([B.CC, *flags, *extra, *B.INCS, '-E', '-P', directory / 'r_draw.c'],
                             work / (variant + '-noopt-preprocess.log'))
                pre.write_text(output)
                normalized = re.sub(r'\s+', '', output)
                drawers = re.sub(r'\s+', '', ''.join(function(output, name) for name in NAMES))
                report[policy][variant]['preprocessed_sha256'] = hashlib.sha256(normalized.encode()).hexdigest()
                report[policy][variant]['preprocessed_drawers_sha256'] = hashlib.sha256(drawers.encode()).hexdigest()
                report[policy][variant]['code_sha256'] = digest(info)
        if policy == 'noopt-draw':
            if report[policy]['reference']['code_sha256'] != report[policy]['candidate']['code_sha256']:
                raise SystemExit('NOOPT_DRAW object code/rodata differs')
            if report[policy]['reference']['preprocessed_drawers_sha256'] != report[policy]['candidate']['preprocessed_drawers_sha256']:
                raise SystemExit('NOOPT_DRAW drawer preprocessed tokens differ')
            report[policy]['identical'] = True
    for variant, elf in [('baseline-elf', args.baseline_elf), ('candidate-elf', args.candidate_elf)]:
        if elf:
            report[variant] = linked_report(elf, variant, work)
    (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
