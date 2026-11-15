"""Exact R_CheckPlane source fixtures; EE builds need a granted global worker."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'
sys.path.insert(0, str(TOOLS))
from plane_bank_hosttest import function
from next7_render_fog_codecheck import Elf32
from next10_sw_bounds_codecheck import code, frame, relocations, symbol_tables


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, path, success=True):
    command = list(map(str, command))
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    path.write_text(' '.join(command) + '\n' + result.stdout + result.stderr)
    if success and result.returncode:
        raise SystemExit('Command failed: ' + str(path))
    return result


def prepare(out, base):
    out.mkdir(parents=True, exist_ok=True)
    baseline = (base / 'r_plane.c').read_text()
    production = (ROOT / 'src/r_plane.c').read_text()
    reset = function(baseline, 'R_ResetPlaneClip')
    if reset != function(production, 'R_ResetPlaneClip'):
        raise SystemExit('Reset source changed outside selected scan')
    bodies = {'reference': function(baseline, 'R_CheckPlane'),
              'production': function(production, 'R_CheckPlane')}
    old_scan = '''\tfor (x = intrl; x <= intrh; x++)
\t\tif (pl->top[x] != 0xffff || pl->bottom[x] != 0x0000)
\t\t\tbreak;'''
    current = bodies['production']
    guard = '#if defined(PS2_OPT_REND) && defined(NOHW) && !defined(PS2_NOOPT_CHECKPLANE_CURSOR)'
    start = current.index(guard)
    stop = current.index('#endif', start) + len('#endif')
    if current[:start] + old_scan + current[stop:] != bodies['reference']:
        raise SystemExit('Production differs outside exactly the selected scan')
    manifest = {'baseline_src': str(base), 'baseline_r_plane_sha256': sha(base/'r_plane.c'),
                'production_r_plane_sha256': sha(ROOT/'src/r_plane.c'), 'variants': {},
                'frozen_headers': {}, 'scope': 'Only NOHW Software R_CheckPlane scan changes; original Hardware/fallback and reset source',
                'exact_production_scope_guard': guard}
    for path in sorted(base.rglob('*.h')):
        relative = path.relative_to(base); target = out/'frozen-src'/relative
        target.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(path, target)
        manifest['frozen_headers'][str(relative)] = sha(target)
    prefix = '''#include "r_plane.h"
#ifdef PS2_OPT_REND
#include "ps2/ps2_rdraw.h"
#endif
extern INT32 viewwidth;
extern visplane_t *new_visplane(unsigned);
#define visplane_hash(p,l,h) ((unsigned)((p)*3+(l)+(h)*7) & VISPLANEHASHMASK)
''' + reset + '\n'
    for name, body in bodies.items():
        body = '__attribute__((noinline,noipa,externally_visible))\n' + body.replace(
            'R_CheckPlane(', name + '_R_CheckPlane(')
        (out/(name+'.inc')).write_text(body)
        (out/(name+'.c')).write_text(prefix + body + '\n')
        manifest['variants'][name] = sha(out/(name+'.inc'))
    allocator = function(baseline, 'new_visplane').replace('static visplane_t *new_visplane',
                                                         'visplane_t *new_visplane')
    (out/'allocator.inc').write_text(allocator+'\n')
    driver = (TOOLS/'next11_sw_checkplane.c').read_text().replace('NEXT11_CHECKPLANE_DECLARE',
        '\n'.join('visplane_t *'+n+'_R_CheckPlane(visplane_t *,INT32,INT32);' for n in bodies))
    driver = driver.replace('NEXT11_CHECKPLANE_VARIANTS', ','.join('VARIANT('+n+')' for n in bodies))
    (out/'driver.c').write_text(driver)
    body = (out/'production.inc').read_text()
    mutations = {'top': body.replace('0xffff', '0xfffe'),
                 'bottom': body.replace('!= 0x0000', '!= 0x0001'),
                 'output': body.replace('pl->minx = unionl;', 'pl->minx = unionl ^ 1;')}
    for name, changed in mutations.items():
        if changed == body:
            raise SystemExit('Actual source mutation missing '+name)
        (out/('production-negative-'+name+'.c')).write_text(prefix+changed+'\n')
    manifest['files'] = {path.name: sha(path) for pattern in ('*.c', '*.inc') for path in out.glob(pattern)}
    (out/'source-hashes.json').write_text(json.dumps(manifest, indent=2)+'\n')


def host(out, sanitize, negatives):
    reports = {}
    for policy, extra in [('profile', ['-DNOHW']), ('noopt-scan', ['-DNOHW','-DPS2_NOOPT_CHECKPLANE_CURSOR']),
                          ('noopt-rend', ['-DNOHW','-DPS2_NOOPT_REND']), ('noopt', ['-DNOHW','-DPS2_NOOPT']),
                          ('hardware-fallback', ['-DHWRENDER'])]:
        flags = ['gcc', '-O2', '-std=gnu17', '-fwrapv', '-DPS2_PROFILE', '-Wall', '-Wextra', '-Werror',
                 *extra, '-I'+str(out/'frozen-src'), '-I'+str(out)]
        sources = [out/'driver.c', out/'reference.c', out/'production.c']
        run([*flags, *sources, '-o', out/policy], out/(policy+'-build.log'))
        result = run([out/policy], out/(policy+'-test.log'))
        tokens={}
        for variant in ('reference','production'):
            text=run([*flags,'-E','-P',out/(variant+'.c')],out/(policy+'-'+variant+'.i')).stdout
            body=function(text,variant+'_R_CheckPlane').replace(variant+'_R_CheckPlane','R_CheckPlane')
            tokens[variant]=hashlib.sha256(re.sub(r'\s+','',body).encode()).hexdigest()
        if policy!='profile' and tokens['reference']!=tokens['production']:
            raise SystemExit('Fallback/opt-out preprocessed selected helper differs '+policy)
        reports[policy] = {'result': result.stdout.strip(), 'executed_negative_controls': [],
                           'policy_definitions':extra,'preprocessed_selected_helper_tokens':tokens,
                           'original_scan_selected':policy!='profile'}
        print(policy+': '+result.stdout.strip(), flush=True)
        if negatives:
            for name in ('top','bottom','output'):
                executable = out/(policy+'-negative-'+name)
                run([*flags, *sources[:2], out/('production-negative-'+name+'.c'), '-o', executable],
                    out/(policy+'-negative-'+name+'-build.log'))
                result = run([executable], out/(policy+'-negative-'+name+'-test.log'), False)
                if result.returncode != 1 or 'CHECKPLANE FAIL' not in result.stdout:
                    raise SystemExit('Actual source negative was not detected '+policy+':'+name)
                reports[policy]['executed_negative_controls'].append(name)
    if sanitize:
        flags = ['gcc', '-O1', '-g', '-std=gnu17', '-fwrapv', '-DPS2_PROFILE', '-DNOHW', '-Wall', '-Wextra', '-Werror',
                 '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                 '-I'+str(out/'frozen-src'), '-I'+str(out)]
        run([*flags, out/'driver.c', out/'reference.c', out/'production.c', '-o', out/'asan'], out/'asan-build.log')
        result = run([out/'asan'], out/'asan-test.log')
        if 'runtime error:' in result.stderr:
            raise SystemExit('Sanitizer diagnostics')
        reports['asan'] = {'result': result.stdout.strip(), 'no_diagnostics': not result.stderr}
        print('asan: '+result.stdout.strip(), flush=True)
    reports['source_manifest_sha256'] = sha(out/'source-hashes.json')
    (out/'host-report.json').write_text(json.dumps(reports, indent=2)+'\n')


def dependency(out, engine_path):
    engine = Elf32(engine_path); name = 'new_visplane.lto_priv.0'
    section = engine.sections[engine.index['.text.'+name]]
    raw = engine.bytes(section)
    symbols = [s for table in symbol_tables(engine).values() for s in table if s['name']==name]
    if len(symbols)!=1:
        raise SystemExit('One complete actual new_visplane required')
    gp = struct.unpack_from('<I',engine.bytes(engine.sections[engine.index['.reginfo']]),20)[0]
    labels = {'.sbss.freetail':'freetail', '.sdata.freehead':'freehead',
              'ztracemin.lto_priv.0':'next11_ztracemin', 'Z_Free':'next11_Z_Free',
              'Z_MallocInternal.constprop.0':'next11_Z_MallocInternal',
              'I_Error':'next11_I_Error', 'I_OutputMsg':'next11_I_OutputMsg'}
    literals = {}; records = []
    for record in relocations(engine,name):
        target = record['symbol']; original = target['name']; addend = record['addend']
        resolved = labels.get(original,original)
        if original.startswith('.rodata.'):
            if original not in literals:
                literals[original] = {'label':'next11_literal_'+str(len(literals)), 'section':target['section']}
            resolved = literals[original]['label']
        if target['type']==3:
            addend += target['value']+(gp if record['kind']==7 else 0)
        records.append({'offset':record['offset'], 'kind':record['kind'], 'exact_original_symbol':original,
                        'exact_original_addend':record['addend'], 'fixture_symbol':resolved, 'fixture_addend':addend})
    lines = ['.set noreorder', '.set noat', '.section .text.new_visplane,"ax",@progbits', '.balign 8',
             '.globl new_visplane', '.type new_visplane,@function', 'new_visplane:']
    lines += ['.word 0x%08x'%word for word in struct.unpack('<'+'I'*(len(raw)//4),raw)]
    for record in records:
        lines += ['.reloc new_visplane+%d,R_MIPS_%s,%s%+d'%(record['offset'],
            {4:'26',5:'HI16',6:'LO16',7:'GPREL16'}[record['kind']],record['fixture_symbol'],record['fixture_addend'])]
    lines += ['.size new_visplane,%d'%symbols[0]['size']]
    for original, entry in literals.items():
        sec = engine.sections[entry['section']]; data = engine.bytes(sec)
        lines += ['.section .rodata.'+entry['label']+',"a",@progbits', '.balign '+str(sec[8]),
                  '.globl '+entry['label'],entry['label']+':', '.byte '+','.join(map(str,data))]
        entry.update({'original_section':original,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()})
    pdr = bytes.fromhex(frame(engine,name)['frame_record_hex'])
    lines += ['.section .pdr','.balign 4','.Lnext11_pdr:',
              *['.word 0x%08x'%word for word in struct.unpack('<8I',pdr)],
              '.reloc .Lnext11_pdr,R_MIPS_32,new_visplane']
    path = out/'dependency.S'; path.write_text('\n'.join(lines)+'\n')
    manifest = {'original_engine':str(engine_path),'original_engine_sha256':sha(engine_path),
                'complete_text_bytes':len(raw),'actual_symbol_bytes':symbols[0]['size'],
                'code_sha256':hashlib.sha256(raw).hexdigest(),'complete_frame':frame(engine,name),
                'ordered_relocations':records,'complete_literals':list(literals.values()),
                'scope':'Complete actual LTO new_visplane; allocation/free/diagnostic exits are untimed capture hooks'}
    (out/'dependency.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return path


def ee(out, engine, negative):
    import build as B
    flags = [f for f in B.CFLAGS if f not in ('-MMD','-MP','-flto')]
    flags += ['-fno-lto','-Werror','-DNEXT11_CHECKPLANE_EE']
    includes = ['-I'+str(out/'frozen-src'),'-I'+str(out),
                *[f for f in B.INCS if not f.startswith('-I'+str(ROOT/'src'))]]
    objects = []
    for name in ('reference','production','driver'):
        source = out/('production-negative-bottom.c' if negative and name=='production' else name+'.c')
        target = out/(name+'.o')
        run([B.CC,*flags,*includes,'-c',source,'-o',target],out/('ee-'+name+'-build.log'))
        objects.append(target)
    source = dependency(out,engine); target=out/'dependency.o'
    run([B.CC,*flags,*includes,'-c',source,'-o',target],out/'ee-dependency-build.log');objects.append(target)
    copied = Elf32(target); actual = Elf32(engine)
    if code(copied,'new_visplane')['words']!=code(actual,'new_visplane.lto_priv.0')['words'] or frame(copied,'new_visplane')!=frame(actual,'new_visplane.lto_priv.0'):
        raise SystemExit('Complete copied dependency instruction/frame differs')
    metadata=json.loads((out/'dependency.json').read_text())
    records=relocations(copied,'new_visplane')
    expected=[(r['offset'],r['kind'],r['fixture_symbol'],r['fixture_addend']) for r in metadata['ordered_relocations']]
    observed=[(r['offset'],r['kind'],r['symbol']['name'],r['addend']) for r in records]
    if observed!=expected:
        raise SystemExit('Complete copied dependency ordered RELAs differ')
    for entry in metadata['complete_literals']:
        data=copied.bytes(copied.sections[copied.index['.rodata.'+entry['label']]])
        if len(data)!=entry['bytes'] or hashlib.sha256(data).hexdigest()!=entry['sha256']:
            raise SystemExit('Complete copied dependency literal differs')
    target=out/'memops.o';source=Path(json.loads((out/'source-hashes.json').read_text())['baseline_src'])/'ps2/ps2_memops.c'
    run([B.CC,*flags,*includes,'-c',source,'-o',target],out/'ee-memops-build.log');objects.append(target)
    command=[B.CC,*B.LDFLAGS,*objects,'-Wl,--wrap=memcpy','-Wl,--wrap=memset',
             '-Wl,-Map='+str(out/'CHECKPLANE.map'),'-o',out/'CHECKPLANE.ELF','-ldebug','-lpatches','-lm']
    run(command,out/'ee-link.log')
    driver=Elf32(out/'driver.o')
    snapshots=[symbol for table in symbol_tables(driver).values() for symbol in table
               if symbol['name']=='original' and symbol['type']==1]
    if len(snapshots)!=1:raise SystemExit('One exact complete native snapshot object required')
    report={'flags':flags,'includes':includes,'link_command':list(map(str,command)),
            'ELF_sha256':sha(out/'CHECKPLANE.ELF'),'objects':{str(p):sha(p) for p in objects},
            'dependency_source_and_exact_RELAs_checked':True,'negative':negative,
            'expected_native_cases':17026,'native_snapshot_bytes':snapshots[0]['size'],
            'actual_memory_wrapper_source_sha256':sha(source),
            'scope':'Recycled-plane scan/helper timings only; untimed exactness also exercises controlled allocation/growth'}
    (out/'ee-build-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Built '+str(out/'CHECKPLANE.ELF'),flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True);parser.add_argument('--base-src',type=Path,required=True)
    parser.add_argument('--sanitize',action='store_true');parser.add_argument('--negative-controls',action='store_true')
    parser.add_argument('--ee-only',action='store_true');parser.add_argument('--negative-ee-only',action='store_true')
    parser.add_argument('--prepare-only',action='store_true');parser.add_argument('--baseline-engine',type=Path)
    args=parser.parse_args();out=args.out.resolve();prepare(out,args.base_src.resolve())
    if args.prepare_only:return
    if args.ee_only or args.negative_ee_only:
        if not args.baseline_engine:raise SystemExit('Actual complete default-LTO engine required')
        ee(out,args.baseline_engine.resolve(),args.negative_ee_only)
    else:host(out,args.sanitize,args.negative_controls)


if __name__=='__main__':main()
