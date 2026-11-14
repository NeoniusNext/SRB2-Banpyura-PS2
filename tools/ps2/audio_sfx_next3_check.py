#!/usr/bin/env python3
"""Compare SFX/mixer changes with a complete saved source tree, never git HEAD.

--ee builds the paired target mixer ELF without starting an emulator. The host
also checks actual I_GetSfx on synthetic and stock PCM, and executes the actual
zone allocator with live neighbors, owners, red zones and allocation peaks.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def run(command, *, env=None, expected=0):
    result = subprocess.run([str(x) for x in command], capture_output=True, text=True, env=env)
    if result.returncode != expected:
        print(result.stdout + result.stderr)
        raise RuntimeError('unexpected status %d: %s' % (result.returncode, ' '.join(str(x) for x in command[:12])))
    return result.stdout + result.stderr


def fixtures(out, assets):
    paths, stock = [], set()
    folder = out / 'fixtures'
    folder.mkdir(exist_ok=True)
    def put(name, data, real=False):
        path = folder / name
        path.write_bytes(data)
        paths.append(path)
        if real:
            stock.add(str(path))
    def chunk(name, payload, pad=True):
        return name + struct.pack('<I', len(payload)) + payload + (b'\0' if pad and len(payload) & 1 else b'')
    for bits in (8, 16):
        for channels in (1, 2):
            for count in (1, 2, 3, 7, 511, 512, 513):
                for layout in range(4):
                    samples = bytes((i * 73 + (i >> 4)) & 255 for i in range(count * channels * (bits // 8)))
                    rate = (11025, 22050, 44100, 96000)[layout]
                    fmt = struct.pack('<HHIIHH', 1, channels, rate, rate * channels * (bits // 8), channels * (bits // 8), bits)
                    body = b'WAVE'
                    if layout == 1:
                        body += chunk(b'JUNK', b'x' * 63)
                    if layout == 2:
                        body += chunk(b'data', samples) + chunk(b'fmt ', fmt)
                    else:
                        body += chunk(b'fmt ', fmt) + chunk(b'data', samples, pad=layout != 3)
                    if layout == 3:
                        body += chunk(b'LIST', b'INFO' + bytes(range(64)))
                    body += chunk(b'JUNK', b'end' * 13)
                    put('synthetic-%d-%d-%d-%d.wav' % (bits, channels, count, layout), b'RIFF' + struct.pack('<I', len(body)) + body)
    for count in (1, 2, 3, 511, 512, 513, 4096):
        for tail in (0, 1, 17, 64):
            put('synthetic-dmx-%d-%d.dmx' % (count, tail), struct.pack('<HHI', 3, 22050, count)
                + bytes((i * 47) & 255 for i in range(count)) + b't' * tail)
    with zipfile.ZipFile(assets / 'srb2.pk3') as archive:
        for name in sorted(archive.namelist()):
            if not name.lower().startswith('sounds/') or name.endswith('/'):
                continue
            data = archive.read(name)
            valid = len(data) >= 12 and data[:4] == b'RIFF' and data[8:12] == b'WAVE'
            valid |= len(data) >= 8 and data[:2] == b'\3\0' and 0 < struct.unpack_from('<I', data, 4)[0] <= len(data) - 8
            if valid:
                put('stock-' + Path(name).name, data, True)
    return paths, stock


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--out', type=Path, default=ROOT / 'build/audio-sfx-next3-20261114')
    parser.add_argument('--assets', type=Path, default=Path('/workspace/.onboarding/srb2-assets'))
    parser.add_argument('--sdk', type=Path, default=Path(os.environ.get('PS2SDK', '/workspace/.onboarding/ps2dev-v2/ps2dev/ps2sdk')))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--ee', action='store_true')
    args = parser.parse_args()
    out = args.out.resolve()
    if not out.is_relative_to(ROOT / 'build'):
        parser.error('--out must be under the repository build directory')
    out.mkdir(parents=True, exist_ok=True)
    reference = args.baseline.resolve() / 'src/ps2'
    if (reference / 'ps2_audio.h').read_bytes() != (ROOT / 'src/ps2/ps2_audio.h').read_bytes():
        parser.error('reference/candidate mixer public headers differ')
    log = ['baseline=' + str(args.baseline.resolve()), 'reference normalization: none']
    inc = ['-I' + str(ROOT / 'src/ps2'), '-I' + str(ROOT / 'tools/ps2'), '-I' + str(args.sdk / 'ee/include'),
           '-I' + str(args.sdk / 'common/include')]
    flags = ['-O2', '-g', '-std=gnu17', '-Wall', '-Wextra', '-Werror', *inc]
    ldflags = []
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie']
        ldflags += ['-no-pie']
    names = sorted(set(re.findall(r'\b(PS2_\w+)\s*\(', (ROOT / 'src/ps2/ps2_audio.h').read_text())))
    mixer_sources = []
    for label, src in [('reference', reference), ('candidate', ROOT / 'src/ps2')]:
        for name in ('ps2_audio.c', 'i_sound.c'):
            log.append(label + ' ' + name + ' sha256=' + hashlib.sha256((src / name).read_bytes()).hexdigest())
        text = (src / 'ps2_audio.c').read_text()
        if label == 'reference':
            text = ''.join('#define %s %s\n' % (n, n.replace('PS2_', 'Ref_', 1)) for n in names) + text
        copied = out / (label + '_ps2_audio.c')
        copied.write_text(text)
        mixer_sources.append(copied)
    fixture = ROOT / 'tools/ps2/audio_sfx_next3_hosttest.c'
    exe = out / 'mixer'
    run(['gcc', *flags, *mixer_sources, fixture, '-o', exe, *ldflags])
    env = os.environ.copy()
    if args.sanitize:
        env['PS2_AUDIO_NO_BENCH'] = '1'
    result = run([exe], env=env)
    (out / 'mixer.log').write_text(result)
    print('\n'.join(line for line in result.splitlines() if line.startswith('SFX3 DONE')))
    log.append(result)
    for control, expression in [('--negative-control', '!memcmp(oa, ob, sizeof oa)'),
                                 ('--state-negative-control', '!memcmp(&a, &b, sizeof a)')]:
        result = run([exe, control], env=env, expected=1)
        if expression not in result:
            raise RuntimeError('wrong mixer negative-control failure: ' + result)
        (out / (control[2:] + '.log')).write_text(result)
        log.append(control + ' detected')

    paths, stock = fixtures(out, args.assets)
    backend = (ROOT / 'tools/ps2/audio_backend_hosttest.c').read_text()
    read_anchor = '{ CHECK(l < 8 && off <= lengths[l] && n <= lengths[l]-off); memcpy(dst,lumpdata[l]+off,n); return n; }'
    assert backend.count(read_anchor) == 1
    backend = backend.replace(read_anchor, '{ probe_reads++; probe_read_hash = (((probe_read_hash ^ l) * UINT64_C(1099511628211) ^ off)'
                              ' * UINT64_C(1099511628211) ^ n) * UINT64_C(1099511628211); '
                              + read_anchor[2:])
    peak_anchor = '*p = n; allocated += n; return p+1;'
    assert backend.count(peak_anchor) == 1
    backend = backend.replace(peak_anchor, '*p = n; allocated += n; if (allocated > probe_peak) probe_peak = allocated; return p+1;')
    reports = []
    for label, src in [('reference', reference), ('candidate', ROOT / 'src/ps2')]:
        work = out / label
        work.mkdir(exist_ok=True)
        source = re.sub(r'^#include "(?:\.\./[^"]+|ps2_boot\.h)"\n', '', (src / 'i_sound.c').read_text(), flags=re.M)
        (work / 'i_sound_host.c').write_text(source)
        (work / 'audio_backend_probe.c').write_text(backend)
        exe = work / 'cache'
        production = [ROOT / ('src/ps2/' + n) for n in ('ps2_audio.c', 'ps2_athread.c', 'ps2_music.c', 'ps2_midi.c')]
        # Old backend stubs intentionally leave target-only helpers unused.
        run(['gcc', *flags, '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-unused-variable', '-Wno-sign-compare', '-I' + str(work),
             *(['-DSFX_CACHE_COMPACT'] if label == 'candidate' else []), *production,
             ROOT / 'tools/ps2/audio_sfx_cache_next3_hosttest.c', '-o', exe, *ldflags, '-lm', '-lpthread'])
        result = run([exe, *paths], env=env)
        (work / 'cache.log').write_text(result)
        print('\n'.join(line for line in result.splitlines() if line.startswith('CACHE3 DONE')))
        log.append(result)
        rows = []
        for line in result.splitlines():
            if line.startswith('CACHE3 file='):
                rows.append(dict(part.split('=', 1) for part in line.split()[1:]))
        reports.append(rows)
        if label == 'candidate':
            for control, expression in [('--negative-control', '!memcmp(data + descriptor.offset'),
                                         ('--read-negative-control', 'probe_reads == 1 && probe_read_hash == expected_reads')]:
                result = run([exe, control, paths[0]], env=env, expected=1)
                if expression not in result:
                    raise RuntimeError('wrong cache negative-control failure: ' + result)
                (work / (control[2:] + '.log')).write_text(result)
                log.append('cache ' + control + ' detected')
    assert len(reports[0]) == len(paths) == len(reports[1])
    for before, after in zip(*reports):
        assert {k: v for k, v in before.items() if k != 'retained'} == {k: v for k, v in after.items() if k != 'retained'}, (before, after)
    corpus = [row for row in reports[1] if row['file'] in stock]
    sizes = out / 'stock-zone-sizes.txt'
    sizes.write_text(''.join('%s %s\n' % (row['original'], row['retained']) for row in corpus))
    (out / 'cache-report.json').write_text(json.dumps({'files': len(paths), 'stock_files': len(corpus),
        'stock_original': sum(int(row['original']) for row in corpus), 'stock_retained': sum(int(row['retained']) for row in corpus),
        'rows': reports[1]}, indent=2) + '\n')
    zone = out / 'zone'
    zone_flags = ['-Wno-unused-function', '-Wno-clobbered', '-Wno-array-bounds', '-DPARANOIA', '-I' + str(ROOT / 'src')]
    run(['gcc', *flags, *zone_flags, ROOT / 'tools/ps2/audio_sfx_zone_next3_hosttest.c', '-o', zone, *ldflags])
    result = run([zone, sizes], env=env)
    (out / 'zone.log').write_text(result)
    print(result, end=''); log.append(result)
    mutant = out / 'zone-copy-mutant.c'
    text = (ROOT / 'src/z_zone.c').read_text()
    anchor = '&& ZA_Resize(ptr, size))'
    assert text.count(anchor) == 1
    mutant.write_text(text.replace(anchor, '&& 0 && ZA_Resize(ptr, size))'))
    exe = out / 'zone-negative'
    run(['gcc', *flags, *zone_flags, '-DZONE_HOST_SOURCE="' + str(mutant) + '"',
         ROOT / 'tools/ps2/audio_sfx_zone_next3_hosttest.c', '-o', exe, *ldflags])
    result = run([exe], env=env, expected=1)
    if 'SFX shrink preserves address and retargets owner' not in result:
        raise RuntimeError('wrong allocator negative-control failure: ' + result)
    (out / 'zone-negative.log').write_text(result); log.append('zone forced-copy negative control detected')

    if args.ee:
        work = out / 'ee'
        work.mkdir(exist_ok=True)
        ee_flags = ['-O2', '-std=gnu17', '-G0', '-Wall', '-Wextra', '-Werror', '-D_EE', '-DPS2', *inc]
        objects = []
        for source in [*mixer_sources, fixture]:
            obj = work / (source.stem + '.o')
            run(['mips64r5900el-ps2-elf-gcc', *ee_flags, '-c', source, '-o', obj])
            objects.append(obj)
            if source in mixer_sources:
                obj.with_suffix('.dis').write_text(run(['mips64r5900el-ps2-elf-objdump', '-dr', obj]))
        result = run(['mips64r5900el-ps2-elf-size', *objects[:2]])
        print(result, end=''); log.append(result)
        elf = work / 'AUDIO_SFX_NEXT3.ELF'
        run(['mips64r5900el-ps2-elf-gcc', '-T' + str(args.sdk / 'ee/startup/linkfile'), '-L' + str(args.sdk / 'ee/lib'),
             '-Wl,-zmax-page-size=128', '-Wl,--defsym,_stack_size=0x60000', *objects, '-o', elf, '-ldebug', '-lpatches', '-lm'])
        log.append('EE ELF (not run by driver)=' + str(elf))
        log.append('EE ELF sha256=' + hashlib.sha256(elf.read_bytes()).hexdigest())
        print('EE synthetic ELF (not executed):', elf)
        corpus_header = work / 'zone-corpus.h'
        corpus_header.write_text('static const size_t sfx_zone_corpus[][2] = {\n' + ''.join(
            '{%s, %s},\n' % (row['original'], row['retained']) for row in corpus) + '};\n')
        # The zone core has no EE service dependencies. Keep the existing host
        # test's boundary and execute its actual 32-bit target data layout.
        zone_flags_ee = [flag for flag in ee_flags if flag not in ('-D_EE', '-DPS2')] + zone_flags + ['-Wno-format',
            '-DSFX_ZONE_CORPUS="' + str(corpus_header) + '"']
        obj = work / 'zone.o'
        run(['mips64r5900el-ps2-elf-gcc', *zone_flags_ee, '-c', ROOT / 'tools/ps2/audio_sfx_zone_next3_hosttest.c', '-o', obj])
        elf = work / 'AUDIO_SFX_ZONE_NEXT3.ELF'
        run(['mips64r5900el-ps2-elf-gcc', '-T' + str(args.sdk / 'ee/startup/linkfile'), '-L' + str(args.sdk / 'ee/lib'),
             '-Wl,-zmax-page-size=128', '-Wl,--defsym,_stack_size=0x60000', obj, '-o', elf, '-ldebug', '-lpatches', '-lm'])
        log.append('EE zone ELF (not run by driver)=' + str(elf))
        log.append('EE zone ELF sha256=' + hashlib.sha256(elf.read_bytes()).hexdigest())
        print('EE zone ELF (not executed):', elf)
    (out / 'report.txt').write_text('\n'.join(log))
    print('Report:', out / 'report.txt')


if __name__ == '__main__':
    main()
