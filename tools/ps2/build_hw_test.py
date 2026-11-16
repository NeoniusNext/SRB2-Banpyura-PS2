"""Build the GS renderer driver hardware test: tools/ps2/hw_test.c + the real src/ps2/hw/ps2_hwd.c.

usage: SRB2_PS2_OUT=<dir> python tools/ps2/build_hw_test.py
Output: <dir>/HW_TEST.ELF (+ hwt/*.o, hwt/build.log). Same flags and tool environment as tools/ps2/build.py;
Engine stand-ins come from gs_list_ee_stubs.c, shared with the GS-list fixture.
Any compiler output (warnings included) fails the build.
"""
import subprocess
import sys
import hashlib
import json
import shlex
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build as B  # noqa: E402  (constants of the engine build; main() is not run on import)

OUTDIR = B.OUT / 'hwt'
SOURCES = ['tools/ps2/hw_test.c', 'src/ps2/hw/ps2_hwd.c', 'tools/ps2/gs_list_ee_stubs.c']
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
    for vname in ('ps2_hw_vu1', 'ps2_hw_vu1_spr'):  # the VU1 microcode of the polygon program and of the sprite program (the driver refers to their symbols)
        vsm = B.ROOT / ('src/ps2/hw/vu1/%s.vsm' % vname)
        if vsm.is_file():
            vobj = OUTDIR / ('%s_vsm.o' % vname)
            vcmd = [str(B.DEV / ('dvp/bin/dvp-as' + B.EXE)), str(vsm), '-o', str(vobj)]
            commands.append(vcmd)
            pv = subprocess.run(vcmd, env=B.ENV, capture_output=True, text=True, cwd=B.ROOT)
            if pv.returncode:
                print('FAIL dvp-as', (pv.stdout + pv.stderr)[-3000:])
                return 1
            objs.append(str(vobj))
    (OUTDIR / 'build.log').write_text('\n'.join(log), encoding='utf-8')
    if bad:
        print('\n'.join(log))
        return 1
    elf = B.OUT / 'HW_TEST.ELF'
    cmd = [str(B.CC)] + B.LDFLAGS + objs + ['-o', str(elf), '-Wl,-Map=' + str(OUTDIR / 'hw_test.map')] + LIBS
    commands.append(cmd)
    p = subprocess.run(cmd, env=B.ENV, capture_output=True, text=True, cwd=B.ROOT)
    (OUTDIR / 'link.log').write_text(p.stdout + p.stderr, encoding='utf-8')
    rest = [l for l in (p.stdout + p.stderr).splitlines() if l.strip() and 'linking abicalls files with non-abicalls files' not in l]  # (dvp-as objects)
    if p.returncode or rest:
        print(p.stdout + p.stderr)
        return 1
    inputs = {B.ROOT / s for s in SOURCES}
    inputs.update((B.ROOT / 'src/ps2/hw').glob('*.inc'))
    inputs.update((B.ROOT / 'src/ps2/hw').glob('*.h'))
    inputs.update((B.ROOT / 'src/ps2/hw/vu1').glob('*.vsm'))
    inputs.update([B.ROOT / 'tools/ps2/hw_test_ext.inc', Path(__file__), B.ROOT / 'tools/ps2/build.py'])
    # -MMD records transitive project/SDK headers. Retain them too: an explicit
    # .inc inventory alone does not bind the actual types/macros of this build.
    for src in SOURCES:
        dep = (OUTDIR / (Path(src).stem + '.o')).with_suffix('.d')
        primary = dep.read_text().replace('\\\n', ' ').splitlines()[0]
        if ': ' not in primary:
            raise RuntimeError('malformed dependency file: ' + str(dep))
        for name in shlex.split(primary.split(': ', 1)[1]):
            path = Path(name)
            inputs.add(path if path.is_absolute() else B.ROOT / path)
    inputs = {f.resolve() for f in inputs}
    root = B.ROOT.resolve()
    (OUTDIR / 'build-report.json').write_text(json.dumps({
        'commands': commands, 'elf_bytes': elf.stat().st_size,
        'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(),
        'source_sha256': {str(f.relative_to(root) if f.is_relative_to(root) else f): hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(inputs)},
        'compile_diagnostics': 0, 'link_diagnostics': 0,
        'known_sdk_abi_warning_lines': sum('linking abicalls files with non-abicalls files' in line for line in (p.stdout + p.stderr).splitlines()),
    }, indent=2), encoding='utf-8')
    print('built', elf, elf.stat().st_size, 'bytes; compile diagnostics=0; link diagnostics excluding known SDK ABI warning=0')
    return 0


if __name__ == '__main__':
    sys.exit(main())
