#!/usr/bin/env python3
"""Link unchanged complete production LTO engine objects into a codec gate.

Only symbol metadata is changed in object copies. The existing specialized
PS2_MusicRender.constprop.0 clone has the two-argument 512-frame ABI documented
by the sixth-pass production-code gate. No engine function is recompiled.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import zipfile

from audio_refill_next6_linked_check import ROOT, allocated_sections, run


def read_rows(path):
    text = path.read_text()
    rows = [dict(re.findall(r'(\w+)=([\da-f.]+)', line)) for line in text.splitlines() if 'LINKED7 ROW ' in line]
    done = re.search(r'LINKED7 DONE rows=(\d+)', text)
    if not done or len(rows) != int(done[1]):
        raise SystemExit('incomplete linked gate: ' + str(path))
    return rows


def summarize(out):
    before = read_rows(out / 'baseline/run.log')
    after = read_rows(out / 'candidate/run.log')
    if len(before) != len(after):
        raise SystemExit('linked row counts differ')
    rows = []
    for a, b in zip(before, after):
        if {key: value for key, value in a.items() if key != 'count'} != {key: value for key, value in b.items() if key != 'count'}:
            raise SystemExit('linked PCM/public-state/read signature differs: ' + str((a, b)))
        row = {key: value for key, value in a.items() if key != 'count'}
        row['baseline_count'] = int(a['count']); row['candidate_count'] = int(b['count'])
        row['reduction_pct'] = (int(a['count']) - int(b['count'])) * 100 / int(a['count'])
        rows.append(row)
    groups = {}
    for name, selected in [('all', rows),
            ('native_wav', [r for r in rows if r['codec'] == '0' and r['speed'] == '0.000']),
            ('general_wav', [r for r in rows if r['codec'] == '0' and r['speed'] != '0.000']),
            ('ogg', [r for r in rows if r['codec'] in ('1', '2', '3')]),
            ('midi', [r for r in rows if r['codec'] in ('4', '5', '6')]),
            ('mp3', [r for r in rows if r['codec'] in ('7', '8')])]:
        if not selected:
            groups[name] = {'rows': 0, 'timing_available': False}
            continue
        a = sum(r['baseline_count'] for r in selected); b = sum(r['candidate_count'] for r in selected)
        groups[name] = {'rows': len(selected), 'baseline_count': a, 'candidate_count': b,
                        'reduction_pct': (a - b) * 100 / a,
                        'slower': sum(r['candidate_count'] > r['baseline_count'] for r in selected),
                        'min_pct': min(r['reduction_pct'] for r in selected), 'max_pct': max(r['reduction_pct'] for r in selected)}
    report = {'groups': groups, 'rows': rows, 'inputs': json.loads((out / 'inputs.json').read_text())}
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(groups, indent=2))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, default=ROOT / 'build/nov14-next7/audio-storage-linked')
    ap.add_argument('--baseline-object', type=Path, default=ROOT / 'build/nov14-next7/baseline/software/engine_lto.o')
    ap.add_argument('--candidate-object', type=Path)
    ap.add_argument('--link-report', type=Path, default=ROOT / 'build/nov14-next7/baseline/software/build-report.json')
    ap.add_argument('--assets', type=Path, default=Path('/workspace/.onboarding/srb2-assets/music.pk3'))
    ap.add_argument('--summarize', action='store_true')
    a = ap.parse_args()
    out = a.out.resolve()
    if not out.is_relative_to(ROOT / 'build'):
        ap.error('--out must be under build')
    if a.summarize:
        summarize(out); return
    for label, path in [('baseline object', a.baseline_object), ('candidate object', a.candidate_object),
                        ('production link report', a.link_report), ('music archive', a.assets)]:
        if path is None or not path.is_file():
            ap.error(label + ' is missing: ' + str(path))
    out.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'tools/ps2/audio_storage_next7_linked.c'
    sdk = Path(os.environ['PS2SDK'])
    command = json.loads(a.link_report.read_text())['link_command']
    inputs = {'link_report': str(a.link_report.resolve()), 'link_report_sha256': hashlib.sha256(a.link_report.read_bytes()).hexdigest(),
              'driver_sha256': hashlib.sha256(source.read_bytes()).hexdigest(), 'codecs': []}
    assets = []
    with zipfile.ZipFile(a.assets) as archive:
        for magic, first in [(b'OggS', 1), (b'MThd', 4)]:
            songs = sorted((len(data), name) for name in archive.namelist() if not name.endswith('/')
                           and (data := archive.read(name)).startswith(magic))
            for i, selected in enumerate((0, len(songs) // 2, len(songs) - 1)):
                name = songs[selected][1]
                assets.append((first + i, name, archive.read(name), 2, 44100 if magic == b'OggS' else 22050))
    for channels in (1, 2):
        dest = out / ('tone%d.mp3' % channels)
        run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i',
             'sine=frequency=337:sample_rate=44100:duration=3.25', '-ac', channels,
             '-codec:a', 'libmp3lame', '-b:a', '128k', dest])
        assets.append((channels + 6, dest.name, dest.read_bytes(), channels, 22050))
    with (out / 'linked_codec7.h').open('w') as header:
        for codec, name, data, channels, rate in assets:
            header.write('static const unsigned char codec7_%d[] = {%s};\n' % (codec, ','.join(str(value) for value in data)))
            inputs['codecs'].append({'id': codec, 'name': name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
        header.write('static const struct { const unsigned char *bytes; size_t length; unsigned codec,channels,rate; } linked_inputs7[] = {\n')
        for codec, name, data, channels, rate in assets:
            header.write('{codec7_%d,sizeof codec7_%d,%d,%d,%d},\n' % (codec, codec, codec, channels, rate))
        header.write('};\n')
    driver = out / 'driver.o'
    flags = ['-O2', '-G0', '-std=gnu17', '-Wall', '-Wextra', '-Werror', '-D_EE', '-DPS2',
             '-I' + str(ROOT / 'src/ps2'), '-I' + str(out), '-I' + str(sdk / 'ee/include'), '-I' + str(sdk / 'common/include')]
    run(['mips64r5900el-ps2-elf-gcc', *flags, '-c', source, '-o', driver])
    for kind, original in [('baseline', a.baseline_object), ('candidate', a.candidate_object)]:
        work = out / kind; work.mkdir(exist_ok=True)
        saved = work / 'engine_original.o'; shutil.copyfile(original, saved)
        exposed = work / 'engine_exposed.o'
        section = '.text.PS2_MusicRender.constprop.0'
        disassembly = run(['mips64r5900el-ps2-elf-objdump', '-dr', '-j', section, saved])
        (work / 'render512.dis').write_text(disassembly)
        # Fail closed if the preceding clone's two-argument ABI entry changes.
        # It overwrites a2 with 2048 output bytes before consuming incoming a2.
        if not re.search(r'^\s*4:\s+24060800\s+li\s+a2,2048\s*$', disassembly, re.M):
            raise SystemExit('specialized renderer ABI entry changed; inspect ' + str(work / 'render512.dis'))
        run(['mips64r5900el-ps2-elf-objcopy', '--redefine-sym=PS2_MusicRender.constprop.0=Engine_MusicRender512', saved, exposed])
        run(['mips64r5900el-ps2-elf-objcopy', '--globalize-symbol=Engine_MusicRender512', exposed])
        before = allocated_sections(saved)
        if before != allocated_sections(exposed):
            raise SystemExit('allocated section bytes changed: ' + kind)
        inputs[kind] = {'object': str(original.resolve()), 'sha256': hashlib.sha256(saved.read_bytes()).hexdigest(),
                        'exposed_sha256': hashlib.sha256(exposed.read_bytes()).hexdigest(), 'identical_allocated_sections': len(before),
                        'render512_two_argument_entry_verified': True,
                        'function_section_bytes': {name: before[name][2] for name in
                            (section, '.text.Decode', '.text.PS2_MusicOpen') if name in before}}
        link = []
        for arg in command:
            if str(arg).endswith('/engine_lto.o'):
                link.extend([str(exposed), str(driver), '-Wl,--wrap=main'])
            elif str(arg).endswith('/SRB2.ELF'):
                link.append(str(work / 'AUDIO_LINKED7.ELF'))
            elif str(arg).startswith('-Wl,-Map='):
                link.append('-Wl,-Map=' + str(work / 'link.map'))
            else:
                link.append(arg)
        (work / 'link.log').write_text(run(link))
        inputs[kind]['link_command'] = link
        inputs[kind]['elf_sha256'] = hashlib.sha256((work / 'AUDIO_LINKED7.ELF').read_bytes()).hexdigest()
        print(kind, 'preserved sections', len(before), 'ELF', inputs[kind]['elf_sha256'])
    (out / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')


if __name__ == '__main__':
    main()
