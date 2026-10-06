"""Build the GS renderer driver hardware test: tools/ps2/hw_test.c + the real src/ps2/hw/ps2_hwd.c.

usage: SRB2_PS2_OUT=<dir> python tools/ps2/build_hw_test.py
Output: <dir>/HW_TEST.ELF (+ hwt/*.o, hwt/build.log). Same flags and tool environment as tools/ps2/build.py;
any compiler output (warnings included) from the two files fails the build.
"""
import subprocess
import sys
import hashlib
import json
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build as B  # noqa: E402  (constants of the engine build; main() is not run on import)

OUTDIR = B.OUT / 'hwt'
SOURCES = ['tools/ps2/hw_test.c', 'src/ps2/hw/ps2_hwd.c']
LIBS = ['-lgskit', '-ldmakit', '-ldebug', '-lpatches', '-lm']


def main():
    OUTDIR.mkdir(parents=True, exist_ok=True)
    B.gen_config()
    cflags = list(dict.fromkeys([f for f in B.CFLAGS if f != '-DNOHW'] + ['-DHWRENDER']))
    objs, log, bad, commands = [], [], False, []
    for src in SOURCES:
        obj = OUTDIR / (Path(src).stem + '.o')
        cmd = [str(B.CC)] + cflags + B.INCS + ['-c', str(B.ROOT / src), '-o', str(obj)]
        commands.append(cmd)
        p = subprocess.run(cmd, env=B.ENV, capture_output=True, text=True, cwd=B.ROOT)
        out = (p.stdout + p.stderr).strip()
        log.append(f'=== {src} (rc={p.returncode})\n{out}')
        if p.returncode or out:
            bad = True
        objs.append(str(obj))
    (OUTDIR / 'build.log').write_text('\n'.join(log), encoding='utf-8')
    if bad:
        print('\n'.join(log))
        return 1
    elf = B.OUT / 'HW_TEST.ELF'
    cmd = [str(B.CC)] + B.LDFLAGS + objs + ['-o', str(elf), '-Wl,-Map=' + str(OUTDIR / 'hw_test.map')] + LIBS
    commands.append(cmd)
    p = subprocess.run(cmd, env=B.ENV, capture_output=True, text=True, cwd=B.ROOT)
    (OUTDIR / 'link.log').write_text(p.stdout + p.stderr, encoding='utf-8')
    if p.returncode or (p.stdout + p.stderr).strip():
        print(p.stdout + p.stderr)
        return 1
    inputs = [B.ROOT / s for s in SOURCES] + list((B.ROOT / 'src/ps2/hw').glob('*.inc'))
    inputs += [B.ROOT / 'src/ps2/hw/ps2_hwd.h', B.ROOT / 'src/ps2/hw/ps2_hwd_dbg.h', Path(__file__), B.ROOT / 'tools/ps2/build.py']
    (OUTDIR / 'build-report.json').write_text(json.dumps({
        'commands': commands, 'elf_bytes': elf.stat().st_size,
        'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(),
        'source_sha256': {str(f.relative_to(B.ROOT)): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs},
        'compile_diagnostics': 0, 'link_diagnostics': 0,
    }, indent=2), encoding='utf-8')
    print('built', elf, elf.stat().st_size, 'bytes; compile/link output: none')
    return 0


if __name__ == '__main__':
    sys.exit(main())
