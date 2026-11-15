#!/usr/bin/env python3
"""Execute PBPut bit/alias and complete PB/planner/packet source replays.

Host GCC only. The existing full PB harness is reused at its current pinned
source revision, with no scratch-layout change or excluded output fields.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

import next12_pb_scratch_hosttest as full
import next13_geometry_variant as variant

ROOT = variant.ROOT
TOOLS = ROOT / 'tools/ps2'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def function(text, signature, last=False):
    start = text.rindex(signature) if last else text.index(signature)
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        if text[end] == '{': depth += 1
        if text[end] == '}': depth -= 1
        end += 1
    return text[start:end] + '\n'


def build_bits(work, baseline, candidate, sanitize=False, negative=None, version='v1'):
    work.mkdir(parents=True, exist_ok=True)
    # The shim copies the actual engine vertex/surface definitions. Its wider
    # host pointer/long ABI is reported separately from the target ABI proof.
    old = full.hw_hosttest.ROOT
    full.hw_hosttest.ROOT = baseline
    try: full.hw_hosttest.make_shim(work)
    finally: full.hw_hosttest.ROOT = old
    (work / 'hw_model.h').write_text((baseline / 'src/hardware/hw_model.h').read_text().replace('#include "../doomtype.h"', ''))
    (work / 'geometry-result.h').write_text('''#pragma once
#include <stdint.h>
enum { GEOMETRY_POOL_WORDS = 2176 };
typedef struct { uint32_t pool[GEOMETRY_POOL_WORDS], entries[8], plan[8], state[11]; } geometry_result_t;
''')
    objects = []
    for tag, source in (('baseline', baseline), ('candidate', candidate)):
        text = (source / variant.SOURCE).read_text()
        structures = full.section(text, 'typedef union\n', '\nstatic hwr_qw_t *pb_pool;')
        body = function(text, 'static inline __attribute__((always_inline)) void HWR_PBPut(')
        if negative and tag == 'candidate':
            if negative == 'omit-first-copy': body = body.replace('HWR_PBC(0);', '/* unsafe negative: omitted vertex zero copy */', 1)
            elif negative == 'omit-last-reduction': body = body.replace('HWR_PBV(3);', 'HWR_PBC(3);', 1)
            else: raise RuntimeError('Unknown negative')
        portable = ''.join(function(text, sig, True) for sig in (
            'static inline float hwr_minf(', 'static inline float hwr_maxf(', 'static inline float hwr_i2f('))
        wrapper = '''
void geometry_TAG(const uint32_t *input,uint32_t n,uint32_t extent,uint32_t want,uint32_t overlap,uint32_t initial,geometry_result_t *result)
{
    static const uint32_t offsets[8]={0,0,1,3,8,9,16,31};
    FSurfaceInfo surf; hwr_pbk_t bucket; const FOutVector *vertices;
    unsigned i; uint32_t *poolwords=(uint32_t *)(void *)pb_pool;
    memset(&surf,0,sizeof surf);memset(&bucket,0,sizeof bucket);
    memset(pb_ent,0xB7,8*sizeof(uint32_t));memset(pb_plan,0xE3,8*sizeof(uint32_t));
    for(i=0;i<GEOMETRY_POOL_WORDS;i++)poolwords[i]=0xA5C31917u+i*0x9E3779B9u;
    if(overlap) {memcpy(poolwords+offsets[overlap],input,1250*sizeof(uint32_t));vertices=(const FOutVector *)(const void *)(poolwords+offsets[overlap]);}
    else vertices=(const FOutVector *)(const void *)input;
    pb_n=1;pb_wr=2;pb_nplan=1;bucket.needext=extent;bucket.planwant=want;
    bucket.cnt=initial&1;bucket.tail=0;bucket.head=0x37481256u;surf.LightInfo.light_level=initial*43u;
    feclearexcept(FE_ALL_EXCEPT);
    if(initial&2)feraiseexcept(FE_OVERFLOW|FE_UNDERFLOW|FE_INVALID);
    HWR_PBPut(&bucket,17,&surf,vertices,n,2+2*n);
    result->state[0]=(uint32_t)fetestexcept(FE_ALL_EXCEPT);
    memcpy(result->pool,pb_pool,sizeof result->pool);memcpy(result->entries,pb_ent,sizeof result->entries);
    memcpy(result->plan,pb_plan,sizeof result->plan);
    result->state[1]=pb_n;result->state[2]=pb_wr;result->state[3]=pb_nplan;
    result->state[4]=bucket.cnt;result->state[5]=bucket.tail;result->state[6]=bucket.head;
    result->state[7]=bucket.needext;result->state[8]=bucket.planwant;
    result->state[9]=sizeof(FOutVector);result->state[10]=sizeof(FUINT);
}
'''.replace('geometry_TAG', 'geometry_' + tag)
        header = '''#include <fenv.h>
#include <stdint.h>
#include <string.h>
#include "shim.h"
#include "geometry-result.h"
#define HWR_PB_END 0xFFFFFFFFu
'''
        globals_ = '''
static hwr_qw_t geometry_pool[GEOMETRY_POOL_WORDS/4];
static hwr_qw_t *pb_pool=geometry_pool;
static hwr_pbe_t geometry_ent[4],*pb_ent=geometry_ent;
static UINT32 geometry_plan[8],*pb_plan=geometry_plan;
static UINT32 pb_n,pb_wr,pb_nplan;
#define HWR_PB_PLAN_BLK(i) pb_plan[2u*(i)]
#define HWR_PB_PLAN_BUCKET(i) pb_plan[2u*(i)+1u]
'''
        path = work / (tag + '.c')
        path.write_text(header + structures + globals_ + portable + body + wrapper)
        objects.append(path)
    binary = work / 'bits'
    flags = ['-std=gnu17','-O2','-g','-fstrict-aliasing','-ffp-contract=off','-frounding-math','-fno-fast-math',
             '-Wall','-Wextra','-Werror','-I' + str(work)]
    if sanitize: flags += ['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
    if version=='v2': flags += ['-DNEXT13_GEOMETRY_REACHABLE_ONLY']
    command = ['gcc',*flags,*objects,TOOLS/'next13_geometry_bits.c','-o',binary,'-lm']
    full.run(command,work/'build.log')
    code = subprocess.run([str(binary)],capture_output=True,text=True,
                           env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
    (work/'run.log').write_text(code.stdout+code.stderr)
    return {'command':list(map(str,command)),'binary_sha256':sha(binary),'returncode':code.returncode,
            'stdout':code.stdout.strip(),'stderr':code.stderr.strip(),
            'source_files':{p.name:sha(p) for p in work.iterdir() if p.suffix in ('.c','.h')}}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline',type=Path,default=variant.WORK/'geometry-baseline-v1')
    p.add_argument('--candidate',type=Path,default=variant.WORK/'geometry-source-v1')
    p.add_argument('--out',type=Path,default=variant.WORK/'geometry-host-v1')
    p.add_argument('--negative-controls',action='store_true')
    p.add_argument('--version',choices=('v1','v2'),default='v1')
    a = p.parse_args()
    baseline,candidate,out=a.baseline.resolve(),a.candidate.resolve(),a.out.resolve()
    variant.validate(baseline);variant.validate(candidate,a.version)
    if out.exists() or not out.is_relative_to(variant.WORK) or out==variant.WORK:
        raise RuntimeError('Use a fresh isolated next13 output directory')
    out.mkdir(parents=True)
    snapshot=out/'tool-snapshot'
    snapshot.mkdir()
    for tool in (Path(__file__),TOOLS/'next13_geometry_bits.c',TOOLS/'next13_geometry_variant.py'):
        (snapshot/tool.name).write_bytes(tool.read_bytes())
    report={'status':'RUNNING','baseline':str(baseline),'candidate':str(candidate),
            'tools':{p.name:sha(p) for p in [Path(__file__),TOOLS/'next13_geometry_bits.c',
                TOOLS/'next13_geometry_variant.py',TOOLS/'next12_pb_scratch_hosttest.py',
                TOOLS/'next12_pb_scratch_hosttest.c',TOOLS/'hw_hosttest.py',TOOLS/'hg_hosttest.c']},
            'compiler':subprocess.check_output(['gcc','--version'],text=True).splitlines()[0],
            'bits':{},'full':{},'negative_controls':{}}
    for tag,sanitize in [('regular',False),('ASan_UBSan',True)]:
        result=build_bits(out/('bits-'+tag),baseline,candidate,sanitize,version=a.version)
        report['bits'][tag]=result
        if result['returncode'] or not result['stdout'].startswith('PBPUT BITS PASS '):
            (out/'failed-report.json').write_text(json.dumps(report,indent=2)+'\n')
            raise RuntimeError('Bit/alias replay failed: '+tag)
    for tag,source in [('baseline',baseline),('candidate',candidate)]:
        work=out/('full-'+tag)
        prepared=full.prepare(work,baseline,source,'candidate')
        regular=full.build_run(work,'candidate')
        sanitized=full.build_run(work,'candidate',True)
        report['full'][tag]={'prepared':prepared,'regular':regular,'ASan_UBSan':sanitized}
        for stream in ('events','packets','memory'):
            if Path(regular['outputs'][stream]['path']).read_bytes()!=Path(sanitized['outputs'][stream]['path']).read_bytes():
                raise RuntimeError('Complete sanitizer stream differs: '+tag+'/'+stream)
    report['complete_stream_equal']={stream:Path(report['full']['baseline']['regular']['outputs'][stream]['path']).read_bytes()==
        Path(report['full']['candidate']['regular']['outputs'][stream]['path']).read_bytes() for stream in ('events','packets','memory')}
    if not all(report['complete_stream_equal'].values()):raise RuntimeError('Complete PB/planner/packet replay differs')
    if a.negative_controls:
        for negative in ('omit-first-copy','omit-last-reduction'):
            result=build_bits(out/('negative-'+negative),baseline,candidate,False,negative,a.version)
            report['negative_controls'][negative]=result
            if result['returncode']!=1 or not result['stderr'].startswith('PBPUT mismatch '):
                raise RuntimeError('Unsafe production-source mutation escaped: '+negative)
    variant.validate(baseline);variant.validate(candidate,a.version)
    report['status']='PASS; host component equality only, target code/cost and integrated gate pending'
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'status':report['status'],'report':str(out/'report.json'),
        'bits':{k:v['stdout'] for k,v in report['bits'].items()},'complete_stream_equal':report['complete_stream_equal'],
        'negative_controls':len(report['negative_controls'])},indent=2))


if __name__=='__main__':main()
