#!/usr/bin/env python3
"""Compare the fourth-pass half-rate mixer with the complete frozen preceding source.

No reference normalization. Each mixer is a separate translation unit, without
LTO. --ee builds, but does not execute, the paired EE test/benchmark ELF.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re

from audio_sfx_next3_check import ROOT, fixtures, run


def check_ee_objects(work):
    def instructions(path):
        functions, key = {}, None
        for line in path.read_text().splitlines():
            match = re.match(r'^[0-9a-f]+ <(.+)>:$', line)
            if match:
                key = match[1]
                functions[key] = []
            elif key and re.match(r'^\s+[0-9a-f]+:\s+[0-9a-f]{8}\s', line):
                assembly = re.split(r'\s+[0-9a-f]{8}\s+', line, maxsplit=1)[1]
                functions[key].append(re.sub(r'\b[0-9a-f]+(?= <)', '@', assembly))
        return functions
    before = instructions(work / 'reference_ps2_audio.dis')
    after = instructions(work / 'candidate_ps2_audio.dis')
    names = ['ClipMMI', 'MusicMMI', 'MixNativeMMI', 'MixDec2Mono8', 'MixDec2Mono16', 'MixDec2Voice',
             'Mix8Mono', 'Mix8Stereo', 'Mix16Mono', 'Mix16Stereo', 'Mix16MonoOdd', 'Mix16StereoOdd']
    report = {name: {'instructions_before': len(before[name]), 'instructions_after': len(after[name]),
                     'same_instructions_registers_and_branch_offsets': before[name] == after[name]}
              for name in names}
    assert all(row['same_instructions_registers_and_branch_offsets'] for row in report.values()), report
    for name, functions in [('Ref_MixerRender', before), ('PS2_MixerRender', after),
                            ('MixHalfVoice', after), ('MixHalfMMI', after)]:
        frame = next((line for line in functions[name] if re.match(r'addiu\s+sp,sp,-', line)), None)
        report[name] = {'stack_allocation': frame}
    (work / 'object-check.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', required=True, type=Path)
    ap.add_argument('--out', type=Path, default=ROOT / 'build/audio-half-next4-20261114')
    ap.add_argument('--assets', type=Path, default=Path('/workspace/.onboarding/srb2-assets'))
    ap.add_argument('--sdk', type=Path, default=Path(os.environ.get('PS2SDK', '/workspace/.onboarding/ps2dev-v2/ps2dev/ps2sdk')))
    ap.add_argument('--sanitize', action='store_true')
    ap.add_argument('--ee', action='store_true')
    a = ap.parse_args()
    out = a.out.resolve()
    if not out.is_relative_to(ROOT / 'build'):
        ap.error('--out must be under the repository build directory')
    out.mkdir(parents=True, exist_ok=True)
    reference = a.baseline.resolve() / 'src/ps2'
    candidate = ROOT / 'src/ps2'
    hashes = {}
    for name in ('ps2_audio.h', 'ps2_music.h', 'ps2_midi.h'):
        if (reference / name).read_bytes() != (candidate / name).read_bytes():
            ap.error('public headers differ: ' + name)
    for name in ('i_sound.c', 'ps2_music.c', 'ps2_midi.c', 'ps2_athread.c'):
        if (reference / name).read_bytes() != (candidate / name).read_bytes():
            ap.error('this focused proof requires unchanged surrounding audio code: ' + name)
    hashes['fixtures'] = {name: hashlib.sha256((ROOT / 'tools/ps2' / name).read_bytes()).hexdigest()
                          for name in ('audio_half_next4_check.py', 'audio_half_next4_hosttest.c',
                                       'audio_sfx_next3_hosttest.c', 'audio_sfx_cache_next3_hosttest.c',
                                       'audio_backend_hosttest.c', 'audio_backend_stubs.h')}
    log = ['baseline=' + str(a.baseline.resolve()), 'reference normalization: none',
           'Both backend boundaries use the same handstub lumpinfo_t; no real w_wad.h ABI is assumed.']
    inc = ['-I' + str(candidate), '-I' + str(ROOT / 'tools/ps2'), '-I' + str(a.sdk / 'ee/include'),
           '-I' + str(a.sdk / 'common/include')]
    flags = ['-O2', '-g', '-std=gnu17', '-Wall', '-Wextra', '-Werror', *inc]
    ldflags = []
    if a.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie']
        ldflags += ['-no-pie']
    names = sorted(set(re.findall(r'\b(PS2_\w+)\s*\(', (candidate / 'ps2_audio.h').read_text())))
    sources = []
    for label, src in [('reference', reference), ('candidate', candidate)]:
        hashes[label] = {name: hashlib.sha256((src / name).read_bytes()).hexdigest()
                         for name in ('ps2_audio.c', 'i_sound.c', 'ps2_music.c', 'ps2_midi.c', 'ps2_athread.c')}
        text = (src / 'ps2_audio.c').read_text()
        if label == 'reference':
            text = ''.join('#define %s %s\n' % (n, n.replace('PS2_', 'Ref_', 1)) for n in names) + text
        copied = out / (label + '_ps2_audio.c')
        copied.write_text(text)
        sources.append(copied)
    fixture = ROOT / 'tools/ps2/audio_half_next4_hosttest.c'
    exe = out / 'mixer'
    run(['gcc', *flags, *sources, fixture, '-o', exe, *ldflags])
    env = os.environ.copy()
    if a.sanitize:
        env['PS2_AUDIO_NO_BENCH'] = '1'
    result = run([exe], env=env)
    (out / 'mixer.log').write_text(result)
    print('\n'.join(line for line in result.splitlines() if line.startswith(('SFX3 DONE', 'HALF4'))))
    log.append(result)
    for control, expression in [('--half-negative-control', '!memcmp(oa, ob, sizeof oa)'),
                                ('--half-state-negative-control', '!memcmp(&a, &b, sizeof a)')]:
        result = run([exe, control], env=env, expected=1)
        if expression not in result:
            raise RuntimeError('wrong half-rate negative-control failure: ' + result)
        (out / (control[2:] + '.log')).write_text(result)
        log.append(control + ' detected')

    paths, stock = fixtures(out, a.assets)
    backend = (ROOT / 'tools/ps2/audio_backend_hosttest.c').read_text()
    anchor = '{ CHECK(l < 8 && off <= lengths[l] && n <= lengths[l]-off); memcpy(dst,lumpdata[l]+off,n); return n; }'
    assert backend.count(anchor) == 1
    backend = backend.replace(anchor, '{ probe_reads++; probe_read_hash = (((probe_read_hash ^ l) * UINT64_C(1099511628211) ^ off)'
                              ' * UINT64_C(1099511628211) ^ n) * UINT64_C(1099511628211); ' + anchor[2:])
    anchor = '*p = n; allocated += n; return p+1;'
    assert backend.count(anchor) == 1
    backend = backend.replace(anchor, '*p = n; allocated += n; if (allocated > probe_peak) probe_peak = allocated; return p+1;')
    reports = []
    for label, src in [('reference', reference), ('candidate', candidate)]:
        work = out / label
        work.mkdir(exist_ok=True)
        production = re.sub(r'^#include "(?:\.\./[^"]+|ps2_boot\.h)"\n', '', (src / 'i_sound.c').read_text(), flags=re.M)
        (work / 'i_sound_host.c').write_text(production)
        (work / 'audio_backend_probe.c').write_text(backend)
        exe = work / 'cache'
        run(['gcc', *flags, '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-unused-variable', '-Wno-sign-compare',
             '-I' + str(work), '-DSFX_CACHE_COMPACT',
             *[src / name for name in ('ps2_audio.c', 'ps2_athread.c', 'ps2_music.c', 'ps2_midi.c')],
             ROOT / 'tools/ps2/audio_sfx_cache_next3_hosttest.c', '-o', exe, *ldflags, '-lm', '-lpthread'])
        result = run([exe, *paths], env=env)
        (work / 'cache.log').write_text(result)
        print('\n'.join(line for line in result.splitlines() if line.startswith('CACHE3 DONE')))
        rows = [dict(part.split('=', 1) for part in line.split()[1:])
                for line in result.splitlines() if line.startswith('CACHE3 file=')]
        reports.append(rows)
        log.append(result)
        if label == 'candidate':
            for control, expression in [('--negative-control', '!memcmp(data + descriptor.offset'),
                                        ('--read-negative-control', 'probe_reads == 1 && probe_read_hash == expected_reads')]:
                result = run([exe, control, paths[0]], env=env, expected=1)
                if expression not in result:
                    raise RuntimeError('wrong cache negative-control failure: ' + result)
                (work / (control[2:] + '.log')).write_text(result)
                log.append('cache ' + control + ' detected')
    assert len(reports[0]) == len(paths) == len(reports[1])
    assert reports[0] == reports[1], 'payload, PCM/private-state digest, retention or ordered read trace differs'
    (out / 'cache-report.json').write_text(json.dumps({'files': len(paths), 'stock_files': len(stock), 'rows': reports[1]}, indent=2) + '\n')

    if a.ee:
        work = out / 'ee'
        work.mkdir(exist_ok=True)
        ee_flags = ['-O2', '-std=gnu17', '-G0', '-Wall', '-Wextra', '-Werror', '-D_EE', '-DPS2', *inc]
        objects = []
        for src in [*sources, fixture]:
            obj = work / (src.stem + '.o')
            run(['mips64r5900el-ps2-elf-gcc', *ee_flags, '-c', src, '-o', obj])
            objects.append(obj)
            if src in sources:
                obj.with_suffix('.dis').write_text(run(['mips64r5900el-ps2-elf-objdump', '-dr', obj]))
        result = run(['mips64r5900el-ps2-elf-size', *objects[:2]])
        print(result, end=''); log.append(result)
        log.append(json.dumps(check_ee_objects(work), indent=2))
        elf = work / 'AUDIO_HALF_NEXT4.ELF'
        run(['mips64r5900el-ps2-elf-gcc', '-T' + str(a.sdk / 'ee/startup/linkfile'), '-L' + str(a.sdk / 'ee/lib'),
             '-Wl,-zmax-page-size=128', '-Wl,--defsym,_stack_size=0x60000', *objects, '-o', elf, '-ldebug', '-lpatches', '-lm'])
        hashes['ee_elf'] = hashlib.sha256(elf.read_bytes()).hexdigest()
        print('EE ELF (not executed):', elf)
    log.append(json.dumps(hashes, indent=2))
    (out / 'source-sha256.json').write_text(json.dumps(hashes, indent=2) + '\n')
    (out / 'report.txt').write_text('\n'.join(log))
    print('Report:', out / 'report.txt')


if __name__ == '__main__':
    main()
