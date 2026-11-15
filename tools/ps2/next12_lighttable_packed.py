"""Pin the actual whole-LTO packed RGB load and its unchanged game ELF bytes."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

from next10_sw_bounds_codecheck import code
from next11_sw_checkplane_codecheck import build_binder

ROOT = Path(__file__).resolve().parents[2]


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def witness(words):
    result = []
    for i in range(1, len(words) - 2):
        a, b, c, previous = words[i], words[i + 1], words[i + 2], words[i - 1]
        if a >> 26 != 34 or b >> 26 != 38: continue
        if (a & 65535, b & 65535) != (3, 0) or (a >> 16) & 1023 != (b >> 16) & 1023: continue
        base, target = (a >> 21) & 31, (a >> 16) & 31
        if previous >> 26 or previous & 63 != 33 or (previous >> 11) & 31 != base or (previous >> 21) & 31 != 21 or (previous >> 16) & 31 != base: continue
        if c >> 26 or c & 63 != 36 or (c >> 11) & 31 != target or (c >> 21) & 31 != target: continue
        maskreg = (c >> 16) & 31
        high = any(w >> 26 == 15 and (w >> 16) & 31 == maskreg and w & 65535 == 255 for w in words[:i])
        low = any(w >> 26 == 13 and (w >> 16) & 31 == maskreg and (w >> 21) & 31 == maskreg and w & 65535 == 65535 for w in words[:i])
        parameter = any(w >> 26 == 0 and w & 63 in (33, 37) and (w >> 11) & 31 == 21 and (w >> 21) & 31 == 5 and (w >> 16) & 31 == 0 for w in words[:i])
        if high and low and parameter:
            result.append({'palette_parameter_register': 5, 'preserved_palette_register': 21,
                'formed_pointer_register': base, 'loaded_color_register': target, 'mask_register': maskreg,
                'pointer_add_offset': (i - 1) * 4, 'LWL_offset': i * 4, 'LWR_offset': (i + 1) * 4, 'AND_offset': (i + 2) * 4,
                'complete_words_hex': ['%08x' % w for w in words[i - 1:i + 3]], 'exact_RGB_mask': '0x00ffffff'})
    if len(result) != 1: raise SystemExit('One complete typed input-pointer LWL/LWR/RGB-mask witness required')
    return result[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, default=ROOT / 'build/nov15-next12/lighttable-combined-v2')
    parser.add_argument('--out', type=Path, required=True); args = parser.parse_args(); work = args.work.resolve(); out = args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'): raise SystemExit('Fresh immutable packed-load stage required')
    out.mkdir(parents=True); shutil.copyfile(Path(__file__), out / Path(__file__).name)
    build = work / 'candidate-hardware'; game = build_binder(build / 'obj/engine_lto.o', build / 'SRB2.ELF', build / 'SRB2.map', work / 'link-rela-candidate-v1')
    name = 'lt_store.lto_priv.0'; words = code(game.obj, name)['words']; actual = witness(words); controls = []
    for title, at, bit in (('LWL_byte_offset', actual['LWL_offset'], 1), ('LWR_byte_offset', actual['LWR_offset'], 1),
                           ('LWR_destination', actual['LWR_offset'], 1 << 16), ('mask_destination', actual['AND_offset'], 1 << 11)):
        changed = list(words); changed[at // 4] ^= bit
        try: witness(changed)
        except SystemExit: controls.append({'actual_word_mutation': title, 'rejected': True})
        else: raise SystemExit('Packed-load witness mutation escaped')
    objdump = Path(json.loads((build / 'build-report.json').read_text())['link_command'][0]).with_name('mips64r5900el-ps2-elf-objdump')
    result = subprocess.run([str(objdump), '-d', '--disassemble=' + name, str(build / 'SRB2.ELF')], capture_output=True, text=True)
    if result.returncode: raise SystemExit('Actual ELF disassembly failed')
    (out / 'actual-shared-store.disasm').write_text(result.stdout + result.stderr)
    abi = ROOT / 'build/nov15-next12/lighttable-v2c/policy-v1/policy-report.json'
    report = {'scope': 'Actual whole-default-LTO store instruction semantics plus separately tested source/operator coverage; no unaligned emulator fixture or CPU timing claim',
        'actual_EE_ABI': json.loads(abi.read_text())['actual_EE_ABI_witness'], 'ABI_report_sha256': sha(abi),
        'actual_unaligned_four_byte_load_and_alpha_mask': actual,
        'alignment_reason': 'LWL3/LWR0 on the same formed palette pointer preserves all four byte-alignment cases. The palette address comes from a1 through s5 and both indexed/sequential branches converge here; no aligned LW or cast is substituted for the packed input.',
        'complete_actual_ELF_binding': game.text(name), 'executed_witness_controls': controls,
        'objdump_sha256': sha(objdump), 'disassembly_sha256': sha(out / 'actual-shared-store.disasm'),
        'pinned_checker_sha256': sha(out / Path(__file__).name)}
    (out / 'packed-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Bound actual packed palette LWL/LWR load and RGB mask: ' + str(out))


if __name__ == '__main__': main()
