"""Read-only audit of final release-LTO generator and recovery helper frames."""
import argparse
import hashlib
import json
from pathlib import Path
import re
from next6_render_npot_objectcheck import run


def inspect(elf, label, out):
    symbols = run(['mips64r5900el-ps2-elf-nm', '-S', '--defined-only', elf],
                  out / (label + '-symbols.log'))
    names = ('R_GenerateTexture', 'R_CompositeRetryRead', 'R_TryCompositeLump',
             'WPack_CanReadWithoutAlloc', 'R_TryReadLump')
    result = {'elf': str(elf), 'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(),
              'functions': {}}
    for name in names:
        match = re.search(r'^([0-9a-f]+) ([0-9a-f]+) [Tt] (' + name
                          + r'(?:\.lto_priv\.\d+)?)$', symbols, re.M)
        if not match:
            continue
        actual = match[3]
        asm = run(['mips64r5900el-ps2-elf-objdump', '-d', '--disassemble=' + actual, elf],
                  out / (label + '-' + name + '.asm'))
        frame = re.search(r'addiu\s+sp,sp,(-\d+)', asm)
        calls = sorted(set(re.findall(r'\bjal\s+[0-9a-f]+ <([^>]+)>', asm)))
        result['functions'][name] = {'symbol': actual, 'address': match[1],
                                     'text_bytes': int(match[2], 16),
                                     'frame_bytes': -int(frame[1]) if frame else 0,
                                     'direct_call_targets': calls}
    assert 'R_GenerateTexture' in result['functions']
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--baseline-elf', required=True, type=Path)
    p.add_argument('--pre-retry-elf', required=True, type=Path)
    p.add_argument('--candidate-elf', required=True, type=Path)
    a = p.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    result = {label: inspect(path.resolve(), label, out) for label, path in
              [('baseline', a.baseline_elf), ('pre-retry', a.pre_retry_elf),
               ('candidate', a.candidate_elf)]}
    before, after = (result[n]['functions']['R_GenerateTexture'] for n in ('pre-retry', 'candidate'))
    result['generator_change_from_pre_retry'] = {
        key: after[key] - before[key] for key in ('text_bytes', 'frame_bytes')}
    result['scope'] = ('Actual linked LTO function sizes/individual frames and direct calls; '
                       'not a whole-thread worst-case stack or whole-game timing proof')
    (out / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
