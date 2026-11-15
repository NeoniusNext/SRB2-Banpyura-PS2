#!/usr/bin/env python3
"""Embed and bind complete actual default-LTO dec_level code/data for native execution."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

from next13_texture_variant import ROOT
from next13_texture_hosttest import section
from audio_storage_next7_finalbind import Object
from next8_gs_codecheck import symbols
from next10_sw_bounds_codecheck import frame
from next7_render_fog_codecheck import Elf32
import next10_gs_low as graph


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(command,log):
    r=subprocess.run([str(x) for x in command],capture_output=True,text=True,cwd=ROOT)
    log.write_text(' '.join(map(str,command))+'\n'+r.stdout+r.stderr)
    if r.returncode:raise SystemExit('Native build failed '+str(log)+'\n'+(r.stdout+r.stderr)[-2000:])


def prepare(work,builds,source):
    if work.exists():raise SystemExit('Fresh native preparation directory required')
    work.mkdir(parents=True)
    graph.CODE=('dec_level',);graph.SPECIAL={}
    graphs={}
    for tag,build in builds.items():
        graphs[tag],_=graph.bundle(work,build/'obj/engine_lto.o',tag)
        if len(graphs[tag])!=2 or [n['kind'] for n in graphs[tag].values()]!=['code','data']:
            raise SystemExit('Actual helper graph is no longer exactly code plus complete H')
        node=next(n for n in graphs[tag].values() if n['kind']=='data')
        if node['bytes']!=36896 or node['ordered_relocations']:raise SystemExit('Unexpected complete actual H state shape')
    common='''#ifndef NEXT13_TEXTURE_COMMON_H
#define NEXT13_TEXTURE_COMMON_H
#include <stdint.h>
#include <stddef.h>
typedef uint8_t u8; typedef uint32_t u32;
typedef struct { u32 pal[256]; } texture_state_t;
extern texture_state_t H;
typedef void (*texture_fn)(const u8 *, u32, u32, u32, u8 *);
void base_dec_level(const u8 *,u32,u32,u32,u8 *);
void cur_dec_level(const u8 *,u32,u32,u32,u8 *);
void truth_dec_level(const u8 *,u32,u32,u32,u8 *);
_Static_assert(sizeof(u32)==4,"actual unsigned32 score arithmetic");
#endif
'''
    (work/'common.h').write_text(common)
    names={tag:next(n['label'] for n in nodes.values() if n['kind']=='data') for tag,nodes in graphs.items()}
    (work/'native-state.h').write_text(''.join('extern unsigned char '+n+'[];\n' for n in names.values())+
        'static unsigned char *const native_state[2]={'+names['base']+','+names['cur']+'};\n')
    text=(source/'ps2/hw/ps2_hw_tex.inc').read_text()
    truth=section(text,'static void dec_level_ref(','// The same as dec_level_ref').replace('static void dec_level_ref(','void truth_dec_level(',1)
    (work/'truth.c').write_text('#include "common.h"\n'+truth)
    shutil.copyfile(ROOT/'tools/ps2/next13_texture_hosttest.c',work/'host-fixture.inc')
    shutil.copyfile(ROOT/'tools/ps2/next13_texture_native.c',work/'native-fixture.c')
    pinned=work/'pinned-tools';pinned.mkdir()
    for name in ('next13_texture_native.py','next13_texture_native.c','next13_texture_hosttest.c','next10_gs_low.py','next8_gs_codecheck.py','audio_storage_next7_finalbind.py'):
        shutil.copyfile(ROOT/'tools/ps2'/name,pinned/name)
    (work/'native-inputs.json').write_text(json.dumps({'builds':{t:str(p) for t,p in builds.items()},
       'source':str(source),'source_texture_sha256':sha(source/'ps2/hw/ps2_hw_tex.inc'),
       'input_sha256':{str(p):sha(p) for p in sorted(work.rglob('*')) if p.is_file()},
       'actual_input_frames':{t:frame(Elf32(b/'obj/engine_lto.o'),'dec_level') for t,b in builds.items()}},indent=2)+'\n')


def build(work,builds):
    import build as B
    flags=[f for f in B.CFLAGS if f not in ('-MMD','-MP','-flto')]
    flags+=['-fno-lto','-Werror','-DNEXT13_TEXTURE_EE','-I'+str(work)]
    objects=[];reports={}
    for tag,current in builds.items():
        source=work/(tag+'-graph.S');target=source.with_suffix('.o')
        run([B.CC,*flags,*B.INCS,'-c',source,'-o',target],work/(tag+'-graph-build.log'))
        data=json.loads((work/(tag+'-graph.json')).read_text())
        graph.restore_section_symbol_types(target,data['nodes'],data['original_GP0'])
        reports[tag]=graph.verify_object(current/'obj/engine_lto.o',target,data['nodes']);objects.append(target)
    for name in ('truth','native-fixture'):
        target=work/(name+'.o')
        run([B.CC,*flags,*B.INCS,'-c',work/(name+'.c'),'-o',target],work/(name+'-build.log'));objects.append(target)
    binary=work/'TEXTURE_NEXT13.ELF'
    run([B.CC,*B.LDFLAGS,*objects,'-Wl,-Map='+str(work/'TEXTURE_NEXT13.map'),'-o',binary,'-ldebug','-lpatches','-lm'],work/'fixture-link.log')
    (work/'build-report.json').write_text(json.dumps({'elf_sha256':sha(binary),'flags':flags,
       'all_complete_actual_nodes':reports,'native_fixture_sha256':sha(work/'native-fixture.c'),
       'host_fixture_sha256':sha(work/'host-fixture.inc'),'no_compiled_source_helper_is_timed':True},indent=2)+'\n')
    print('Built actual-code native '+str(binary))


def bind(work,builds,out):
    if out.exists():raise SystemExit('Fresh native binding report required')
    fixture=Object(work/'TEXTURE_NEXT13.ELF');fs=symbols(fixture)
    if sha(work/'TEXTURE_NEXT13.ELF')!=json.loads((work/'build-report.json').read_text())['elf_sha256']:raise SystemExit('Fixture changed')
    result={}
    for tag,build in builds.items():
        g=json.loads((work/(tag+'-graph.json')).read_text());obj=Object(build/'obj/engine_lto.o');elf=Object(build/'SRB2.ELF');es=symbols(elf)
        if sha(build/'obj/engine_lto.o')!=g['engine_sha256']:raise SystemExit('Actual graph engine changed')
        report=json.loads((build/'build-report.json').read_text())
        if sha(build/'SRB2.ELF')!=report['elf_sha256']:raise SystemExit('Actual whole game ELF changed')
        mapped=graph.map_addresses(build/'SRB2.map');nodes={}
        for sec,node in g['nodes'].items():
            if node['kind']=='data':
                if node['bytes']!=36896 or fs[node['label']]['bytes']!=36896 or fs[node['label']]['address']%16 or mapped[sec]['bytes']!=36896:
                    raise SystemExit('Complete actual/fixture H capacity or alignment differs')
                nodes[sec]={'original_and_fixture_complete_BSS_capacity':36896,'palette_byte_offset':35264,
                     'actual_map_address':mapped[sec]['address'],'fixture_address':fs[node['label']]['address']}
                continue
            data=obj.content(obj.sections[sec]);records=node['ordered_relocations']
            if any(r['type'] not in (5,6) or r['original_target']['section'] not in g['nodes'] or
                g['nodes'][r['original_target']['section']]['kind']!='data' for r in records):
                raise SystemExit('Unexpected helper code/literal/SDK boundary')
            expected=graph.relocate(data,records,lambda r:fs[r['target']]['address']+r['addend'],fs['_gp']['address'],g['original_GP0'])
            if expected!=graph.linked_span(fixture,fs,node['label'],len(data)):raise SystemExit('Complete resolved fixture code+padding differs')
            original=graph.relocate(data,records,lambda r:graph.resolve_original(obj,elf,es,mapped,r,{}),es['_gp']['address'],g['original_GP0'])
            if original!=graph.linked_span(elf,es,sec[6:],len(data)):raise SystemExit('Complete resolved actual game code+padding differs')
            nodes[sec]={'complete_original_section_bytes':len(data),'symbol_body_bytes':node['body_bytes'],
                'full_input_sha256':hashlib.sha256(data).hexdigest(),'ordered_original_RELA_records':records,
                'fully_resolved_actual_and_fixture_bytes_checked':True,'actual_input_frame':frame(Elf32(build/'obj/engine_lto.o'),'dec_level'),
                'actual_helper_has_no_external_callees':True}
        result[tag]={'build':str(build),'ELF_sha256':sha(build/'SRB2.ELF'),'engine_sha256':sha(build/'obj/engine_lto.o'),'nodes':nodes}
    out.write_text(json.dumps({'fixture_sha256':sha(work/'TEXTURE_NEXT13.ELF'),'variants':result},indent=2)+'\n')
    print('Bound complete actual-LTO native code/data/ordered RELAs/padding/frames.')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base',type=Path,default=ROOT/'build/nov15-next12/final/hardware')
    p.add_argument('--candidate',type=Path,required=True)
    p.add_argument('--source',type=Path,default=ROOT/'build/nov15-next12/final/source/src')
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--prepare',action='store_true');p.add_argument('--build',action='store_true');p.add_argument('--bind',action='store_true')
    a=p.parse_args();builds={'base':a.base.resolve(),'cur':a.candidate.resolve()};work=a.out.resolve()
    if a.prepare:prepare(work,builds,a.source.resolve())
    if a.build:build(work,builds)
    if a.bind:bind(work,builds,work/'native-binding.json')


if __name__=='__main__':main()
