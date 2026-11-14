#!/usr/bin/env python3
"""Link a small driver to exact already-generated complete-engine audio code."""
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


def run(args):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(result.stdout + result.stderr)
    return result.stdout + result.stderr


def allocated_sections(path):
    data = path.read_bytes()
    header = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
    assert header[0][:7] == b'\x7fELF\x01\x01\x01'
    sections = [struct.unpack_from('<IIIIIIIIII', data, header[6] + i * header[11]) for i in range(header[12])]
    strings = sections[header[13]]
    names = data[strings[4]:strings[4] + strings[5]]
    result = {}
    for section in sections:
        if not section[2] & 2:
            continue
        name = names[section[0]:].split(b'\0', 1)[0].decode()
        contents = b'' if section[1] == 8 else data[section[4]:section[4] + section[5]]
        result[name] = (section[1], section[2], section[5], hashlib.sha256(contents).hexdigest())
    return result


def summarize(out):
    versions = {}
    for kind in ('baseline', 'candidate'):
        log = (out / kind / 'run.log').read_text()
        rows = []
        for line in log.splitlines():
            if 'LINKED6 ROW ' not in line:
                continue
            values = dict(re.findall(r'(\w+)=([\da-f.]+)', line))
            rows.append(values)
        done = re.search(r'LINKED6 DONE rows=(\d+)', log)
        if not done or len(rows) != int(done[1]):
            raise SystemExit('incomplete linked screen: ' + kind)
        versions[kind] = rows
    if len(versions['baseline']) != len(versions['candidate']):
        raise SystemExit('row counts differ')
    rows = []
    for before, after in zip(versions['baseline'], versions['candidate']):
        if {key: value for key, value in before.items() if key != 'count'} != {key: value for key, value in after.items() if key != 'count'}:
            raise SystemExit('linked PCM/public-state/read signature differs: ' + str((before, after)))
        row = {key: value for key, value in before.items() if key != 'count'}
        row['baseline_count'] = int(before['count'])
        row['candidate_count'] = int(after['count'])
        row['reduction_pct'] = (row['baseline_count'] - row['candidate_count']) * 100 / row['baseline_count']
        rows.append(row)
    groups = {}
    for name, selected in [('all', rows), ('native_wav', [row for row in rows if row['codec'] == '0' and row['speed'] == '0.000']),
                           ('general_wav', [row for row in rows if row['codec'] == '0' and row['speed'] != '0.000']),
                           ('ogg', [row for row in rows if row['codec'] != '0'])]:
        a = sum(row['baseline_count'] for row in selected)
        b = sum(row['candidate_count'] for row in selected)
        groups[name] = {'rows': len(selected), 'baseline_count': a, 'candidate_count': b,
                        'reduction_pct': (a - b) * 100 / a, 'slower': sum(row['candidate_count'] > row['baseline_count'] for row in selected),
                        'min_pct': min(row['reduction_pct'] for row in selected), 'max_pct': max(row['reduction_pct'] for row in selected)}
    report = {'groups': groups, 'rows': rows, 'inputs': json.loads((out / 'inputs.json').read_text())}
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(groups, indent=2))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, default=ROOT / 'build/audio-refill-next6-linked')
    ap.add_argument('--baseline-object', type=Path, default=ROOT / 'build/audio-refill-next6-linked/baseline/engine_original.o')
    ap.add_argument('--candidate-object', type=Path, default=ROOT / 'build/audio-refill-next6-linked/candidate/engine_original.o')
    ap.add_argument('--link-report', type=Path, default=ROOT / 'build/audio-refill-next6-linked/candidate/build-report.original.json')
    ap.add_argument('--assets', type=Path, default=Path('/workspace/.onboarding/srb2-assets/music.pk3'))
    ap.add_argument('--summarize', action='store_true')
    args = ap.parse_args()
    out = args.out.resolve()
    if not out.is_relative_to(ROOT / 'build'):
        ap.error('--out must be under build')
    if args.summarize:
        summarize(out)
        return
    for label, path in [('baseline object', args.baseline_object), ('candidate object', args.candidate_object),
                        ('production link report', args.link_report), ('music archive', args.assets)]:
        if not path.is_file():
            ap.error(label + ' is missing: ' + str(path))
    out.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'tools/ps2/audio_refill_next6_linked.c'
    sdk = Path(os.environ['PS2SDK'])
    report = json.loads(args.link_report.read_text())
    command = report['link_command']
    inputs = {'link_report': str(args.link_report.resolve()), 'link_report_sha256': hashlib.sha256(args.link_report.read_bytes()).hexdigest(),
              'driver_sha256': hashlib.sha256(source.read_bytes()).hexdigest(), 'codecs': []}
    with zipfile.ZipFile(args.assets) as archive:
        songs = sorted((len(data), name, data) for name in archive.namelist() if not name.endswith('/')
                       and (data := archive.read(name)).startswith(b'OggS'))
        chosen = [songs[0], songs[len(songs) // 2], songs[-1]]
    header = []
    for i, (length, name, data) in enumerate(chosen):
        header.append('static const unsigned char ogg%d[] = {%s};' % (i, ','.join(str(value) for value in data)))
        inputs['codecs'].append({'id': i + 1, 'name': name, 'bytes': length, 'sha256': hashlib.sha256(data).hexdigest()})
    header.append('static const struct { const unsigned char *bytes; size_t length; } linked_ogg[] = {' +
                  ','.join('{ogg%d,sizeof ogg%d}' % (i, i) for i in range(len(chosen))) + '};')
    header.append('#define LINKED_OGG_COUNT %d' % len(chosen))
    (out / 'linked_ogg.h').write_text('\n'.join(header) + '\n')
    driver = out / 'driver.o'
    flags = ['-O2', '-G0', '-std=gnu17', '-Wall', '-Wextra', '-Werror', '-D_EE', '-DPS2',
             '-I' + str(ROOT / 'src/ps2'), '-I' + str(out), '-I' + str(sdk / 'ee/include'), '-I' + str(sdk / 'common/include')]
    run(['mips64r5900el-ps2-elf-gcc', *flags, '-c', source, '-o', driver])
    for kind, original in [('baseline', args.baseline_object), ('candidate', args.candidate_object)]:
        work = out / kind
        work.mkdir(exist_ok=True)
        exposed = work / 'engine_exposed.o'
        run(['mips64r5900el-ps2-elf-objcopy',
             '--redefine-sym=PS2_MusicRender.constprop.0=Engine_MusicRender512', original, exposed])
        run(['mips64r5900el-ps2-elf-objcopy', '--globalize-symbol=Engine_MusicRender512', exposed])
        before = allocated_sections(original)
        if before != allocated_sections(exposed):
            raise SystemExit('allocated section bytes changed: ' + kind)
        inputs[kind] = {'object': str(original.resolve()), 'sha256': hashlib.sha256(original.read_bytes()).hexdigest(),
                        'exposed_sha256': hashlib.sha256(exposed.read_bytes()).hexdigest(),
                        'identical_allocated_sections': len(before)}
        cmd = []
        for arg in command:
            if str(arg).endswith('/engine_lto.o'):
                cmd += [str(exposed), str(driver), '-Wl,--wrap=main']
            elif str(arg).endswith('/SRB2.ELF'):
                cmd.append(str(work / 'AUDIO_LINKED6.ELF'))
            elif str(arg).startswith('-Wl,-Map='):
                cmd.append('-Wl,-Map=' + str(work / 'link.map'))
            else:
                cmd.append(arg)
        output = run(cmd)
        (work / 'link.log').write_text(output)
        inputs[kind]['link_command'] = cmd
        inputs[kind]['elf_sha256'] = hashlib.sha256((work / 'AUDIO_LINKED6.ELF').read_bytes()).hexdigest()
        print(kind, 'preserved sections', len(before), 'ELF', inputs[kind]['elf_sha256'])
    (out / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')


if __name__ == '__main__':
    main()
