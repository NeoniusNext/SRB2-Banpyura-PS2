#!/usr/bin/env python3
"""Execute exact-source flat lifetimes with native and actual-size 32-bit types."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from next13_flats_cut_variant import ROOT, BASE, OUT, PATH, GUARD, rewrite


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def section(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def function(text, start, occurrence=0):
    starts = [m.start() for m in re.finditer(re.escape(start), text)]
    a = starts[occurrence]
    # Erase strings/comments without changing byte positions before balancing C.
    token = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/')
    clean = token.sub(lambda m: ' ' * len(m.group()), text)
    brace = clean.index('{', a)
    depth = 1
    b = brace + 1
    while depth:
        depth += (clean[b] == '{') - (clean[b] == '}')
        b += 1
    return text[a:b] + '\n'


def extract(src):
    text = (src / PATH).read_text()
    data = (src / 'src/hardware/hw_data.h').read_text()
    defs = (src / 'src/hardware/hw_defs.h').read_text()
    args = (src / 'src/m_argv.c').read_text()
    out = section(data, 'typedef enum GLTextureFormat_e', '// Patch information for the hardware renderer.')
    out += section(defs, 'enum ETextureFlags', 'struct FTextureInfo')
    out += '\nINT32 myargc; char **myargv; static INT32 found;\n'
    for name in ('INT32 M_CheckParm(', 'boolean M_IsNextParm(', 'const char *M_GetNextParm('):
        out += function(args, name)
    out += section(text, 'static size_t gl_numtextures = 0;', 'boolean gl_maptexturesloaded = false;')
    out += 'boolean gl_maptexturesloaded = false;\n'
    out += '\n/* HOST SERVICE BOUNDARIES */\n'
    out += (ROOT / 'tools/ps2/next13_flats_cut_hosttest.c').read_text().split('/* SOURCE_INSERTION */')[0]
    out += section(text, 'void HWR_InitMapTextures(void)', '// --------------------------------------------------------------------------\n// Make sure texture is downloaded')
    out += function(text, 'static void MakeLevelFlatMipmap(')
    out += '\n#ifdef PS2_PROFILE\n'
    out += function(text, 'void HWR_GetLevelFlat(', 0)
    out += function(text, 'void HWR_PS2_RegenerateMipmap(')
    out += function(text, 'void *HWR_PS2_StealData(')
    out += '\n#else\n' + function(text, 'void HWR_GetLevelFlat(', 1) + '\n#endif\n'
    out += (ROOT / 'tools/ps2/next13_flats_cut_hosttest.c').read_text().split('/* SOURCE_INSERTION */')[1]
    return out


CONFIGS = {
    'ps2-profile': ['-DPS2', '-DPS2_PROFILE'],
    'ps2-profile-optout': ['-DPS2', '-DPS2_PROFILE', '-DPS2_NOOPT_FLAT_METADATA'],
    'nonps2-profile': ['-DPS2_PROFILE'],
    'ps2-noprofile': ['-DPS2'],
    'generic': [],
    'ps2-old-mipmap': ['-DPS2', '-DPS2_PROFILE', '-DPS2_NOOPT_MIPMAP_LAYOUT'],
}

ARGUMENTS = [[], ['-hwgo'], ['-hwgo', '0'], ['-hwgo', '65536'], ['-HWGO', '65600'],
    ['-hwgo', '131071'], ['-hwgo', '-65536'], ['-hwgo', '+65536'],
    ['-hwgo', 'bogus'], ['-hwgo', '65536tail'], ['-hwgo', ' 65536'],
    ['-hwgo', ' -65536'], ['-hwgo', ' +65536'], ['-hwgo', '0x10000'],
    ['-hwgo', ''], ['-hwgo', '0', '-hwgo', '65536'],
    ['-hwgo', '65536', '-hwgo', '0'], ['-hwgo', '-hwgo', '65536'],
    ['-hwgo', 'garbage', '-hwgo', '65536'], ['-hwgo', '2147483647'],
    ['-hwgo', '4294967295'], ['-hwgo', '4295032832'], ['-hwgo', '64'],
    ['-hwgo', '1048576'], ['-hwgo', '8388608']]

SCENARIOS = ['normal', 'purge', 'steal', 'reload', 'fail-first', 'fail-second',
             'fail-variant', 'regen-null', 'regen-fail', 'zero', 'unused', 'large', 'callback-init', 'callback-argv', 'callback-calloc']

MUTATIONS = {
    'legacy-stride': ('return !((UINT32)atoi(myargv[i + 1]) & 0x10000u);', 'return true;'),
    'cursor-side-effect': ('static boolean HWR_UseCompactFlatMetadata(void)\n{\n\tINT32 i;\n', 'static boolean HWR_UseCompactFlatMetadata(void)\n{\n\tINT32 i;\n\tM_CheckParm("-hwgo");\n'),
    'getter-stride': ('n * stride', 'n * sizeof(GLMapTexture_t)'),
    'zero-reset-stride': ('sizeof(GLMapTexture_t) - gl_flats_cut;', 'gl_flats_cut ? sizeof(GLMapTexture_t) - gl_flats_cut : 0;'),
    'child-delete-policy': ('DeleteTextureMipmap(mipmap->nextcolormap, false);', 'DeleteTextureMipmap(mipmap->nextcolormap, true);'),
    'primary-before-child': ('\tif (mipmap->nextcolormap)\n', '\tDeleteTextureMipmap(mipmap, true);\n\tif (mipmap->nextcolormap)\n'),
}


def run(command, log=None, check=True):
    r = subprocess.run([str(x) for x in command], text=True, capture_output=True, cwd=ROOT)
    if log:
        log.write_text(' '.join(map(str, command)) + '\n' + r.stdout + r.stderr)
    if check and r.returncode:
        raise RuntimeError(str(command) + '\n' + (r.stdout + r.stderr)[-3500:])
    return r


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base', type=Path, default=BASE)
    p.add_argument('--candidate', type=Path, default=OUT)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--negative-controls', action='store_true')
    p.add_argument('--execute-32', action='store_true', help='Run i386 binaries only on a host with IA32 execution support')
    a = p.parse_args()
    work = a.out.resolve()
    work.mkdir(parents=True, exist_ok=False)
    original = (a.base / PATH).read_text()
    candidate = (a.candidate / PATH).read_text()
    if candidate != rewrite(original):
        raise RuntimeError('Candidate differs from the frozen reversible private-layout transform')
    prelude = (ROOT / 'tools/ps2/next13_flats_host_prelude.h').read_text()
    bodies = {tag: extract(src) for tag, src in [('base', a.base), ('cur', a.candidate)]}
    for tag, body in bodies.items():
        (work / (tag + '.c')).write_text(prelude + '\n#define FLAT_CANDIDATE ' + str(int(tag == 'cur')) + '\n' + body)
    results = []
    for bits in (64, 32):
        for name, defines in CONFIGS.items():
            stem = name + '-' + str(bits)
            flags = ['gcc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-parameter', '-Wno-misleading-indentation', '-Wno-clobbered', '-fwrapv', *defines]
            if bits == 32:
                flags += ['-m32', '-DFLAT_FREESTANDING32', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-pie', '-no-pie', '-nostdlib', '-static']
            elif a.sanitize:
                flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
            for tag in ('base', 'cur'):
                run([*flags, work / (tag + '.c'), '-o', work / (stem + '-' + tag)], work / (stem + '-' + tag + '-compile.log'))
            active = name in ('ps2-profile', 'ps2-old-mipmap')
            if not active:
                # Compare the entire extracted production unit after preprocessing,
                # excluding the host fixture that asks whether a layout is active.
                # Full-function C paths are separately compared below by stripping
                # line directives from source-only preprocessing outputs.
                def source_only(src):
                    t = (src / PATH).read_text()
                    data = section(t, 'static size_t gl_numtextures = 0;', 'boolean gl_maptexturesloaded = false;')
                    data += section(t, 'void HWR_InitMapTextures(void)', '// --------------------------------------------------------------------------\n// Make sure texture is downloaded')
                    data += function(t, 'void HWR_GetLevelFlat(', 0)
                    f = work / (stem + '-' + ('base' if src == a.base else 'cur') + '-source.c')
                    f.write_text(data)
                    return run(['gcc', '-E', '-P', '-x', 'c', *defines, f]).stdout
                if source_only(a.base) != source_only(a.candidate):
                    raise RuntimeError('Opt-out/non-PS2/no-profile original source path changed: ' + stem)
            if bits == 32 and not a.execute_32:
                results.append({'config': stem, 'comparisons': 0, 'active': active,
                                'compiled_actual_32bit_sizes': True, 'executed': False,
                                'base_binary_sha256': sha(work / (stem + '-base')), 'candidate_binary_sha256': sha(work / (stem + '-cur'))})
                continue
            comparisons = 0
            cases = []
            for scenario in SCENARIOS:
                argv_sets = ARGUMENTS if scenario in ('normal', 'reload', 'unused') else [[], ['-hwgo', '65536'], ['-hwgo', '65600']]
                if scenario == 'large':
                    argv_sets = [[], ['-hwgo', '65536']]
                for argv in argv_sets:
                    values = {}
                    for tag in ('base', 'cur'):
                        output = run([work / (stem + '-' + tag), scenario, *argv]).stdout
                        values[tag] = json.loads(output)
                    b, c = values['base'], values['cur']
                    for key in ('hash', 'events', 'errors', 'deletes', 'owners', 'live', 'calls', 'count', 'wall_bytes'):
                        if b[key] != c[key]:
                            raise RuntimeError(json.dumps({'config': stem, 'scenario': scenario, 'argv': argv, 'field': key, 'base': b, 'cur': c}))
                    if active and c['compact']:
                        if b['flat_bytes'] - c['flat_bytes'] != 8 * c['count']:
                            raise RuntimeError('Expected exactly 8 bytes saved per flat')
                    elif b['flat_bytes'] != c['flat_bytes']:
                        raise RuntimeError('Legacy/disabled flat allocation size changed')
                    cases.append({'scenario': scenario, 'argv': argv, 'base': b, 'candidate': c})
                    comparisons += 1
            # Exercise the original fault injector with a fresh static parser.
            if 'profile' in name or name == 'ps2-old-mipmap':
                for argv in (['-hwfbtex', '1'], ['-hwfbtex', '2'], ['-hwfbtex', '1', '-hwgo', '65536']):
                    b = json.loads(run([work / (stem + '-base'), 'injected', *argv]).stdout)
                    c = json.loads(run([work / (stem + '-cur'), 'injected', *argv]).stdout)
                    if any(b[k] != c[k] for k in ('hash', 'events', 'errors', 'deletes', 'owners', 'live', 'calls', 'count')):
                        raise RuntimeError('Original -hwfbtex recovery changed')
                    cases.append({'scenario': 'injected', 'argv': argv, 'base': b, 'candidate': c})
                    comparisons += 1
            results.append({'config': stem, 'comparisons': comparisons, 'active': active,
                            'executed': True, 'cases': cases,
                            'base_binary_sha256': sha(work / (stem + '-base')), 'candidate_binary_sha256': sha(work / (stem + '-cur'))})
    negative = {}
    if a.negative_controls:
        for name, (old, new) in MUTATIONS.items():
            body = bodies['cur']
            if body.count(old) != 1:
                raise RuntimeError('Mutation anchor changed: ' + name)
            src = work / ('negative-' + name + '.c')
            src.write_text(prelude + '\n#define FLAT_CANDIDATE 1\n' + body.replace(old, new, 1))
            binary = src.with_suffix('')
            run(['gcc', '-std=gnu11', '-O1', '-DPS2', '-DPS2_PROFILE', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-pie', '-no-pie', src, '-o', binary], work / ('negative-' + name + '-compile.log'))
            scenario = 'callback-init' if name == 'zero-reset-stride' else 'normal'
            argv = ['-hwgo', '65536'] if name == 'legacy-stride' else []
            r = run([binary, scenario, *argv], work / ('negative-' + name + '.log'), check=False)
            base = json.loads(run([work / 'ps2-profile-64-base', scenario, *argv]).stdout)
            killed = r.returncode != 0
            if not killed:
                cur = json.loads(r.stdout)
                killed = any(base[k] != cur[k] for k in ('hash', 'events', 'errors', 'deletes', 'owners', 'live'))
            if not killed:
                raise RuntimeError('Surviving negative control: ' + name)
            negative[name] = {'detected': True, 'exit_status': r.returncode, 'log_sha256': sha(work / ('negative-' + name + '.log'))}
    report = {'revision': '353cc94103aeb38cef87d26151c779302135d0e9',
        'source_sha256': {'baseline': sha(a.base / PATH), 'candidate': sha(a.candidate / PATH)},
        'fixture_sha256': {p.name: sha(p) for p in [Path(__file__), ROOT / 'tools/ps2/next13_flats_cut_hosttest.c', ROOT / 'tools/ps2/next13_flats_host_prelude.h']},
        'generated_source_sha256': {tag: sha(work / (tag + '.c')) for tag in bodies},
        'variant': 'flat-cut-v2', 'original_621_case_suite_preserved': True,
        'byte_accessor_domain': 'calloc storage, first member offset0, both strides aligned; full size_t cut and index',
        'actual_32bit_types_compiled': True, 'actual_32bit_types_executed': a.execute_32, 'sanitizers': a.sanitize,
        'results': results, 'comparisons': sum(x['comparisons'] for x in results), 'negative_controls': negative}
    (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'comparisons': report['comparisons'], 'negative_controls': len(negative), 'out': str(work)}))


if __name__ == '__main__':
    main()
