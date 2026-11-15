#!/usr/bin/env python3
"""Build only hw_cache replacement IR in the complete actual205-input EE graph.

Requires the root's granted serial EE compiler worker. No emulator is run.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct

from next10_sw_bounds_context import archive_generated
from next12_pb_scratch_context import run, compare_lto_diagnostics
from next11_sw_linker_relas import verify
from next7_render_fog_codecheck import Elf32
from audio_storage_next7_finalbind import Object
from next13_flats_variant import ROOT, BASE, OUT, PATH, rewrite


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def lto_options(path):
    obj = Object(path)
    sections = [s for s in obj.sections if s.startswith('.gnu.lto_.opts')]
    if len(sections) != 1:
        raise RuntimeError('One whole LTO option section required')
    return obj.content(obj.sections[sections[0]])


def check_diagnostics(build, out, stdout, stderr, canonical_ir, objects, original):
    report = compare_lto_diagnostics(build / 'lto.log', stdout, stderr, canonical_ir, objects, original, out)
    if report['all_normalized_diagnostics_identical']:
        return report
    before = (build / 'lto.log').read_text()
    invocation = report['exact_recorded_invocation_metadata']
    if invocation['removed']:
        key = invocation['exact_structured_report_field'].split('.')[0]
        line = ' '.join(map(str, original[key]['lto_command'])) + '\n'
        if not before.startswith(line):
            raise RuntimeError('Canonical diagnostic invocation differs')
        before = before[len(line):]
    counts = re.findall(r'using serial compilation of (\d+) LTRANS jobs', before)
    other = re.findall(r'using serial compilation of (\d+) LTRANS jobs', stderr)
    if len(counts) != 1 or len(other) != 1:
        raise RuntimeError('Unexpected actual LTO diagnostic difference')
    paths = {}
    for row in report['byte_identical_input_paths']:
        for spelling in (row['canonical_path'], row['copied_path']):
            paths[spelling] = row['identity']
            if Path(spelling).is_relative_to(ROOT):
                paths[str(Path(spelling).relative_to(ROOT))] = row['identity']
    for row in report['checked_diagnostic_aliases']:
        paths[row['diagnostic_spelling']] = row['identity']
    def normalize(value):
        for spelling, identity in sorted(paths.items(), key=lambda x: len(x[0]), reverse=True):
            value = re.sub(r'(?<![\w./-])' + re.escape(spelling) + r'(?=[:\s\'"\)]|$)', lambda m: identity, value)
        return value.replace(str(ROOT) + '/', '')
    expected = before.replace('using serial compilation of ' + counts[0] + ' LTRANS jobs',
                              'using serial compilation of ' + other[0] + ' LTRANS jobs')
    if stdout or normalize(expected) != normalize(stderr):
        raise RuntimeError('New actual LTO diagnostics beyond explicit partition count')
    report['all_diagnostics_except_recorded_partition_count_identical'] = True
    report['partition_count'] = {'baseline': int(counts[0]), 'candidate': int(other[0])}
    (out / 'lto-diagnostics.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, default=ROOT / 'build/nov15-next12/final/hardware')
    p.add_argument('--baseline', type=Path, default=BASE)
    p.add_argument('--candidate', type=Path, default=OUT)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--optout', action='store_true')
    a = p.parse_args()
    build, base, candidate, out = (v.resolve() for v in (a.build, a.baseline, a.candidate, a.out))
    if out.exists() or not out.is_relative_to(ROOT / 'build/nov15-next13'):
        raise RuntimeError('Fresh isolated pass13 destination required')
    manifest = json.loads((candidate / 'source-manifest.json').read_text())
    sources = {p.relative_to(base).as_posix(): sha(p) for p in (base / 'src').rglob('*') if p.is_file()}
    overlay = {p.relative_to(candidate).as_posix(): sha(p) for p in (candidate / 'src').rglob('*') if p.is_file()}
    if len(sources) != 602 or sources != manifest['source_sha256'] or overlay != manifest['candidate_source_sha256']:
        raise RuntimeError('Complete602 source freeze differs')
    if [p for p in sources if sources[p] != overlay[p]] != [PATH] or (candidate / PATH).read_text() != rewrite((base / PATH).read_text()):
        raise RuntimeError('Only isolated private flat layout changes are allowed')
    original = json.loads((build / 'build-report.json').read_text())
    if not original['hardware'] or len(original['sources']) != 203 or sha(build / 'SRB2.ELF') != 'efc963c7876a5f2a1c395baf5ba9982b68c6db1e56811d7a5e8a690ba840ab20':
        raise RuntimeError('Exact actual default Hardware context required')
    frozen = {str(p): sha(p) for p in (build / 'build-report.json', build / 'obj/engine_lto.o', build / 'SRB2.ELF', build / 'SRB2.map')}
    inputs = {}
    for name, digest in original['input_sha256'].items():
        path = Path(name)
        if path.is_relative_to(ROOT / 'src'):
            if sha(base / path.relative_to(ROOT)) != digest:
                raise RuntimeError('Canonical source dependency differs: ' + name)
        else:
            inputs[name] = digest
    out.mkdir(parents=True)
    (out / 'obj').mkdir()
    pinned = out / 'pinned-tools'
    pinned.mkdir()
    for name in ('next13_flats_context.py', 'next13_flats_variant.py', 'next12_pb_scratch_context.py', 'next10_sw_bounds_context.py', 'next11_sw_linker_relas.py'):
        shutil.copyfile(ROOT / 'tools/ps2' / name, pinned / name)
    generated = archive_generated(out, inputs, [build / 'gen/config.h'])
    generated_paths = {e['original_path']: e['archive_path'] for e in generated['details']['entries']}
    directories = {str(Path(old).parent): str(Path(new).parent) for old, new in generated_paths.items()}
    for name, digest in inputs.items():
        actual = Path(generated_paths.get(name, name))
        if sha(actual) != digest:
            raise RuntimeError('Canonical external/generated dependency differs: ' + name)
    flags = [f for f in original['flags'] if f not in ('-MMD', '-MP')]
    prefix = '-I' + str(ROOT / 'src')
    flags = ['-I' + str(candidate / 'src') + f[len(prefix):] if f.startswith(prefix) else f for f in flags]
    flags = ['-I' + directories.get(f[2:], f[2:]) if f.startswith('-I') else f for f in flags]
    if not {'-O2', '-G8', '-flto', '-Werror', '-DPS2', '-DPS2_PROFILE', '-DHWRENDER'} <= set(flags):
        raise RuntimeError('Actual default compile flags required')
    rules_path = ROOT / 'tools/ps2/opt_units.txt'
    rules = {}
    for line in rules_path.read_text().splitlines():
        parts = line.split('#', 1)[0].split()
        if len(parts) > 1:
            rules[parts[0]] = parts[1:]
    if rules.get(PATH, []) or any(f.startswith('-O') and f != '-O2' for f in flags):
        raise RuntimeError('hw_cache actual default O2 has no per-unit override')
    if a.optout:
        flags += ['-DPS2_NOOPT_FLAT_METADATA']
    compiler = original['link_command'][0]
    objects, canonical_ir = [], {}
    for unit in original['sources']:
        name = unit.replace('/', '__').replace('.c', '.o')
        old, new = build / 'obj' / name, out / 'obj' / name
        canonical_ir[str(old)] = sha(old)
        shutil.copyfile(old, new)
        objects.append(new)
    for name in ('ps2_hw_vu1_vsm.o', 'ps2_hw_vu1_spr_vsm.o'):
        old, new = build / 'obj' / name, out / 'obj' / name
        canonical_ir[str(old)] = sha(old)
        shutil.copyfile(old, new)
        objects.append(new)
    replacement = out / 'obj/src__hardware__hw_cache.o'
    command = [compiler, *flags, '-MMD', '-MF', out / 'hw_cache.d', '-c', candidate / PATH, '-o', replacement]
    stdout, stderr, compile_seconds = run(command, out / 'compile.log', 120)
    if stdout or stderr:
        raise RuntimeError('Actual changed-unit compile produced diagnostics')
    before_options = lto_options(build / 'obj' / replacement.name)
    after_options = lto_options(replacement)
    if before_options != after_options:
        raise RuntimeError('Entire changed-unit .gnu.lto_.opts bytes differ from actual canonical IR')
    (out / 'actual-unit-options.json').write_text(json.dumps({'baseline_IR': str(build / 'obj' / replacement.name),
        'candidate_IR': str(replacement), 'all_original_option_section_bytes_identical': True,
        'option_bytes_hex': before_options.hex(), 'opt_units_sha256': sha(rules_path), 'actual_default_unit_override': []}, indent=2) + '\n')
    engine = out / 'obj/engine_lto.o'
    lto = [compiler, '-r', '-nostdlib', '-flto=1', '-flinker-output=nolto-rel', '-O2', '-ffunction-sections', '-fdata-sections',
           *[f for f in flags if f.startswith('-G') or f == '-mno-abicalls'], *objects, '-o', engine]
    stdout, stderr, lto_seconds = run(lto, out / 'lto.log', 600)
    diagnostics = check_diagnostics(build, out, stdout, stderr, canonical_ir, objects, original)
    link = list(original['link_command'])
    engine_sites = [i for i, v in enumerate(link) if v.endswith('/engine_lto.o')]
    map_sites = [i for i, v in enumerate(link) if v.startswith('-Wl,-Map=')]
    if len(engine_sites) != 1 or len(map_sites) != 1:
        raise RuntimeError('One complete actual engine and map required')
    link[engine_sites[0]] = str(engine)
    link[link.index('-o') + 1] = str(out / 'SRB2.ELF')
    link[map_sites[0]] = '-Wl,-Map=' + str(out / 'SRB2.map')
    stdout, stderr, link_seconds = run(link, out / 'link.log', 120)
    if '\n'.join(s for s in (stdout + stderr).splitlines() if 'linking abicalls files with non-abicalls files' not in s).strip():
        raise RuntimeError('New actual final-link diagnostic')
    companion = out / 'linker-relas'
    companion.mkdir()
    emitted = list(link)
    emitted[emitted.index('-o') + 1] = str(companion / 'LINKER-RELAS.ELF')
    emitted[map_sites[0]] = '-Wl,-Map=' + str(companion / 'LINKER-RELAS.map')
    emitted.append('-Wl,--emit-relocs')
    stdout, stderr, reloc_seconds = run(emitted, companion / 'link.log', 120)
    if '\n'.join(s for s in (stdout + stderr).splitlines() if 'linking abicalls files with non-abicalls files' not in s).strip():
        raise RuntimeError('New exact retained-RELA diagnostic')
    checked = verify(Elf32(out / 'SRB2.ELF'), Elf32(companion / 'LINKER-RELAS.ELF'))
    checked.update(original_ELF=str(out / 'SRB2.ELF'), original_ELF_sha256=sha(out / 'SRB2.ELF'),
                   original_engine=str(engine), original_engine_sha256=sha(engine),
                   original_map=str(out / 'SRB2.map'), original_map_sha256=sha(out / 'SRB2.map'),
                   diagnostic_ELF_sha256=sha(companion / 'LINKER-RELAS.ELF'), exact_link_command=emitted)
    (companion / 'linker-rela-report.json').write_text(json.dumps(checked, indent=2) + '\n')
    # Compile actual EE ABI constants from the exact native project headers.
    probe = out / 'abi.c'
    probe.write_text('#include <stddef.h>\n#include "hardware/hw_data.h"\n'
        '_Static_assert(sizeof(GLMipmap_t)==76,"actual EE mipmap ABI");\n'
        '_Static_assert(sizeof(GLMapTexture_t)==84,"public wall ABI stays intact");\n'
        'const unsigned int next13_flat_abi[]={sizeof(GLMipmap_t),sizeof(GLMapTexture_t),'
        '_Alignof(GLMipmap_t),offsetof(GLMapTexture_t,mipmap),offsetof(GLMapTexture_t,scaleX),offsetof(GLMapTexture_t,scaleY)};\n')
    abi_obj = out / 'abi.o'
    probe_flags = [f for f in flags if f != '-flto']
    stdout, stderr, abi_seconds = run([compiler, *probe_flags, '-fno-lto', '-c', probe, '-o', abi_obj], out / 'abi-compile.log', 120)
    if stdout or stderr:
        raise RuntimeError('Actual EE ABI probe diagnostics')
    obj = Object(abi_obj)
    abi_data = obj.content(obj.sections['.rodata.next13_flat_abi'])
    abi = struct.unpack('<6I', abi_data)
    if abi != (76, 84, 4, 0, 76, 80):
        raise RuntimeError('Unexpected actual EE ABI constants')
    actual_inputs = {}
    for name, digest in original['input_sha256'].items():
        path = Path(name)
        actual = candidate / path.relative_to(ROOT) if path.is_relative_to(ROOT / 'src') else Path(generated_paths.get(name, name))
        actual_inputs[str(actual)] = sha(actual)
        expected = overlay[path.relative_to(ROOT).as_posix()] if path.is_relative_to(ROOT / 'src') else digest
        if actual_inputs[str(actual)] != expected:
            raise RuntimeError('Frozen actual compile dependency changed')
    for name, digest in {**frozen, **canonical_ir}.items():
        if sha(name) != digest:
            raise RuntimeError('Immutable canonical artifact changed')
    changed = [Path(old).name for old, new in zip(canonical_ir, objects) if canonical_ir[old] != sha(new)]
    if changed != ['src__hardware__hw_cache.o']:
        raise RuntimeError('Another actual complete IR input changed')
    context = {'status': 'COMPLETE actual default205-input isolated flat metadata context',
        'canonical_build': str(build), 'canonical_artifacts': frozen, 'canonical_IR_objects': canonical_ir,
        'complete_actual_IR_objects': {str(v): sha(v) for v in objects}, 'changed_actual_IR_objects': changed,
        'all_other204_complete_IR_inputs_identical': True, 'complete_baseline_source_sha256': sources,
        'complete_source_overlay_sha256': overlay, 'only_recompiled_unit': PATH, 'narrow_optout': a.optout,
        'generated_input_archive': generated, 'compile_command': list(map(str, command)), 'lto_command': list(map(str, lto)),
        'link_command': link, 'emit_relocs_command': emitted, 'compile_seconds': compile_seconds,
        'lto_seconds': lto_seconds, 'link_seconds': link_seconds, 'reloc_link_seconds': reloc_seconds,
        'lto_diagnostics': diagnostics, 'actual_unit_options_identical': True, 'actual_EE_ABI': list(abi),
        'actual_EE_ABI_object_sha256': sha(abi_obj), 'actual_EE_ABI_probe_seconds': abi_seconds,
        'engine_sha256': sha(engine), 'ELF_sha256': sha(out / 'SRB2.ELF'),
        'baseline_ELF_byte_identical': sha(build / 'SRB2.ELF') == sha(out / 'SRB2.ELF'),
        'pinned_tools': {p.name: sha(p) for p in pinned.iterdir()}}
    report = dict(original)
    report.update(flags=flags, compiled=1, link_command=link, input_sha256=actual_inputs,
                  elf_sha256=context['ELF_sha256'], elf_bytes=(out / 'SRB2.ELF').stat().st_size,
                  flats_context_control=context)
    (out / 'context-report.json').write_text(json.dumps(context, indent=2) + '\n')
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'out': str(out), 'ELF_sha256': context['ELF_sha256'], 'actual_EE_ABI': list(abi),
                      'optout': a.optout, 'baseline_ELF_byte_identical': context['baseline_ELF_byte_identical'], 'lto_seconds': lto_seconds}))


if __name__ == '__main__':
    main()
