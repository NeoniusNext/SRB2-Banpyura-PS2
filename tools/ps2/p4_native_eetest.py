#!/usr/bin/env python3
"""Build targeted P4 native GS checks from a frozen candidate source tree.

Copies all production source into a fresh stage, adds explicit diagnostic-only
record/transport queries to that copy, and reuses the official native HW_TEST
startup/readback implementation and common EE/VU flags. Does not run PCSX2.
The ordinary suite is retained; pass p4only to run the focused exact P8/P4 suite.
"""
import argparse
import hashlib
import importlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'

HOOKS = r'''
/* P4 native fixture diagnostics only; absent from production source/ELF. */
int P4Test_Query(const GLMipmap_t *m, unsigned int *out)
{
    const texrec_t *r = rec_of(m);
    if (!r) return 0;
    out[0] = r->psm; out[1] = r->csa; out[2] = r->w; out[3] = r->h;
    out[4] = r->nblk; out[5] = r->tbw; out[6] = r->share != NOREC;
    out[7] = r->blk;
    return 1;
}
int P4Test_SetVU(int on)
{
    PS2HWD_Sync();
    V.on = on != 0;
    vu_plan_valid = 0; VU.consts_ok = 0; H.gsr.valid = 0; H.serial++;
    return V.on && V.ready;
}
unsigned int P4Test_VUPolys(void) { return G.s_vu; }
int P4Test_RowMode(void) { return P.pal && VU.rowmode; }
int P4Test_RebootVU(void)
{
    PS2HWD_Sync();
    if (VU.open || V.chunk_open) return 0;
    vu_plan_valid = 0; VU.consts_ok = 0; H.gsr.valid = 0; H.serial++;
    return vu1_boot();
}
void P4Test_ReadPlanes(unsigned int *out)
{
    unsigned int i;
    PS2HWD_Sync();
    for (i = 0; i < 48; i++) out[i] = VU1_DATA[20 * 4 + i];
}
void P4Test_Guard(float *out)
{
    xf_update();
    out[0] = (float)H.guard_x / 1024.0f;
    out[1] = (float)H.guard_y / 1024.0f;
    out[2] = H.near_plane; out[3] = FAR_CLIPPING_PLANE;
}
int P4Test_Crosses(const FOutVector *v, int n, int plane)
{
    int i, outside = 0;
    const float gx = (float)H.guard_x / 1024.0f, gy = (float)H.guard_y / 1024.0f;
    xf_update();
    for (i = 0; i < n; i++) {
        cv_t c;
        xform_vertex(&v[i], NULL, &c);
        outside += !!(outcode(&c, gx, gy) & (1 << plane));
    }
    return outside > 0 && outside < n;
}
'''


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise RuntimeError('Expected one fixture insertion: ' + before)
    return text.replace(before, after)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, required=True, help='Frozen root containing src/')
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    source, out = a.source.resolve(), a.out.resolve()
    if not out.is_relative_to(ROOT / 'build') or out == ROOT / 'build' or out.exists():
        p.error('Use a fresh child of repository build/')
    if 'PSM_T4' not in (source / 'src/ps2/hw/ps2_hw_tex.inc').read_text():
        p.error('Frozen P4 candidate source required')
    out.mkdir(parents=True)
    pinned = out / 'pinned-tools'
    pinned.mkdir()
    for name in ('p4_native_eetest.py', 'p4_native_cases.inc', 'hw_test.c', 'hw_test_ext.inc',
                 'gs_list_ee_stubs.c', 'build.py'):
        shutil.copyfile(TOOLS / name, pinned / name)
    frozen = out / 'source'
    shutil.copytree(source / 'src', frozen / 'src', symlinks=False)
    original = {str(path.relative_to(frozen)): sha(path) for path in sorted(frozen.rglob('*')) if path.is_file()}
    driver = frozen / 'src/ps2/hw/ps2_hwd.c'
    driver.write_text(driver.read_text() + HOOKS)
    fixture = (pinned / 'hw_test.c').read_text()
    fixture = replace_once(fixture, 'boolean HWR_ShouldUsePaletteRendering(void) { return 0; }',
                           'static int p4test_palette_mode;\nboolean HWR_ShouldUsePaletteRendering(void) { return p4test_palette_mode; }')
    # The native fixture owns these allocations. Its cache-release hook is a
    # no-op, so a P8 fallback may borrow the fixture data without losing it.
    fixture = replace_once(fixture, '(void)m; (void)newuser;\n\treturn NULL;',
                           '*newuser = m->data;\n\treturn m->data;')
    fixture = replace_once(fixture, '#include "hw_test_ext.inc"',
                           '#include "' + str(pinned / 'hw_test_ext.inc') + '"\n'
                           '#include "' + str(pinned / 'p4_native_cases.inc') + '"')
    fixture = replace_once(fixture, 'int main(int argc, char **argv)', 'int standard_main(int argc, char **argv)')
    fixture += '\nint main(int argc, char **argv) { g_argc = argc; g_argv = argv; return arg("p4only") ? p4test_main(argc, argv) : standard_main(argc, argv); }\n'
    obj = out / 'hwt'
    obj.mkdir()
    fixture_path = obj / 'hw_test.c'
    fixture_path.write_text(fixture)
    os.environ['SRB2_PS2_OUT'] = str(out)
    B = importlib.import_module('build')
    B.gen_config()
    flags = list(dict.fromkeys([f for f in B.CFLAGS if f != '-DNOHW'] + ['-DHWRENDER', '-Werror']))
    incs = ['-I' + str(frozen / 'src'), '-I' + str(frozen / 'src/ps2'), *B.INCS[2:]]
    sources = [fixture_path, driver, pinned / 'gs_list_ee_stubs.c']
    commands, objects, inputs = [], [], set()

    def run(command, log, abi_warning=False):
        commands.append(list(map(str, command)))
        result = subprocess.run(command, cwd=ROOT, env=B.ENV, text=True, capture_output=True)
        output = result.stdout + result.stderr
        log.write_text(output)
        unexpected = [line for line in output.splitlines() if line.strip() and not
                      (abi_warning and 'linking abicalls files with non-abicalls files' in line)]
        if result.returncode or unexpected:
            raise RuntimeError(str(log) + '\n' + output)

    for s in sources:
        target = obj / (s.stem + '.o')
        run([B.CC, *flags, *incs, '-c', s, '-o', target], obj / (s.stem + '-build.log'))
        objects.append(target)
        dep = target.with_suffix('.d')
        primary = dep.read_text().replace('\\\n', ' ').splitlines()[0]
        for name in shlex.split(primary.split(': ', 1)[1]):
            path = Path(name)
            inputs.add(path.resolve() if path.is_absolute() else (ROOT / path).resolve())
    for name in ('ps2_hw_vu1', 'ps2_hw_vu1_spr'):
        s = frozen / ('src/ps2/hw/vu1/' + name + '.vsm')
        target = obj / (name + '_vsm.o')
        run([B.DEV / ('dvp/bin/dvp-as' + B.EXE), s, '-o', target], obj / (name + '-build.log'))
        objects.append(target)
        inputs.add(s)
    elf = out / 'HW_TEST.ELF'
    run([B.CC, *B.LDFLAGS, *objects, '-o', elf, '-Wl,-Map=' + str(obj / 'hw_test.map'),
         '-lgskit', '-ldmakit', '-ldebug', '-lpatches', '-lm'], obj / 'link.log', True)
    inputs.update([Path(__file__), TOOLS / 'p4_native_cases.inc', TOOLS / 'build.py',
                   TOOLS / 'hw_test.c', TOOLS / 'hw_test_ext.inc', driver])
    inputs.update(path for path in pinned.iterdir() if path.is_file())
    report = {'source': str(source), 'ELF_sha256': sha(elf), 'ELF_bytes': elf.stat().st_size,
              'commands': commands, 'common_flags': flags, 'include_roots': incs,
              'all_original_source_sha256': original,
              'input_sha256': {str(path): sha(path) for path in sorted(inputs)},
              'diagnostic_production_copy_edits': ['Appended record/transport read-only queries and explicit symmetric scalar transport switch to copied ps2_hwd.c'],
              'compile_diagnostics': 0, 'link_diagnostics_except_known_sdk_abi_warning': 0,
              'scope': 'Native GS P4 isolated fixture, explicit full-size mode. Not the full game LTO code or physical PS2 timing.'}
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Built ' + str(elf) + ' (' + str(elf.stat().st_size) + ' bytes); diagnostics=0')


if __name__ == '__main__':
    main()
