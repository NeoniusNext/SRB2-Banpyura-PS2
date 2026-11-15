#!/usr/bin/env python3
"""Execute the unchanged complete public n<3 rejection before PB entry sinks."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

import next12_pb_scratch_hosttest as full
import next13_geometry_hosttest as host
import next13_geometry_variant as variant


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline',type=Path,default=variant.WORK/'geometry-baseline-v1')
    p.add_argument('--candidate',type=Path,default=variant.WORK/'geometry-source-v2')
    p.add_argument('--out',type=Path,default=variant.WORK/'geometry-public-v1')
    a=p.parse_args();base,cur,out=(x.resolve() for x in (a.baseline,a.candidate,a.out))
    variant.validate(base);variant.validate(cur,'v2')
    name='src/hardware/hw_batching.c'
    if (base/name).read_bytes()!=(cur/name).read_bytes():raise RuntimeError('Public caller source differs')
    text=(base/name).read_text()
    body=host.function(text,'void HWR_ProcessPolygon(')
    slow=host.function(text,'static void HWR_ProcessPolygonSlow(')
    if 'if (iNumPts < 3)\n\t\treturn;' not in body or 'if (iNumPts < 3)\n        return;' not in slow:
        raise RuntimeError('Original public/private entry size guards differ')
    if out.exists() or not out.is_relative_to(variant.WORK):raise RuntimeError('Fresh next13 output required')
    out.mkdir(parents=True);(out/'tool-snapshot').mkdir()
    for tool in (Path(__file__),variant.ROOT/'tools/ps2/next13_geometry_hosttest.py'):
        shutil.copyfile(tool,out/'tool-snapshot'/tool.name)
    old=full.hw_hosttest.ROOT;full.hw_hosttest.ROOT=base
    try:full.hw_hosttest.make_shim(out)
    finally:full.hw_hosttest.ROOT=old
    (out/'hw_model.h').write_text((base/'src/hardware/hw_model.h').read_text().replace('#include "../doomtype.h"',''))
    shutil.copyfile(base/'src/ps2/hw/ps2_hw_fx2.h',out/'ps2_hw_fx2.h')
    prefix='''#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "shim.h"
#include "ps2_hw_fx2.h"
#define HWR_GO_NOPB 0x10000u
static unsigned calls[3];
static int currently_batching,hwr_sprite_batch,hwr_grec_on,hwr_ph_on,hwr_sprite_shadow;
static unsigned hwr_geo_off,hwr_fr_off;
int ps2hwd_fx2;
static int HWR_PBFast(const FSurfaceInfo *s,const FOutVector *v,FUINT n,FBITFIELD f,int target,boolean horizon,UINT32 spr)
{(void)s;(void)v;(void)n;(void)f;(void)target;(void)horizon;(void)spr;calls[0]++;return 0;}
static void HWR_PBAdd(const FSurfaceInfo *s,const FOutVector *v,FUINT n,FBITFIELD f,int target,boolean horizon)
{(void)s;(void)v;(void)n;(void)f;(void)target;(void)horizon;calls[1]++;}
static void HWR_ProcessPolygonSlow(FSurfaceInfo *s,FOutVector *v,FUINT n,FBITFIELD f,int target,boolean horizon)
{(void)s;(void)v;(void)n;(void)f;(void)target;(void)horizon;calls[2]++;}
'''
    suffix='''
int main(void)
{
    unsigned n,mode,pointers,cases=0;FSurfaceInfo surface;FOutVector vertices[3];
    memset(&surface,0xE7,sizeof surface);memset(vertices,0xA3,sizeof vertices);
    for(mode=0;mode<256;mode++)for(pointers=0;pointers<2;pointers++)for(n=0;n<3;n++) {
        currently_batching=mode&1;hwr_sprite_batch=(mode>>1)&1;hwr_grec_on=(mode>>2)&1;
        hwr_ph_on=(mode>>3)&1;hwr_geo_off=(mode&16)?HWR_GO_NOPB:0;hwr_sprite_shadow=(mode>>5)&1;
        hwr_fr_off=(mode&64)?64:0;ps2hwd_fx2=(mode&128)?FX3_NOSPR|FX3_NOSTREAM:0;
        HWR_ProcessPolygon(pointers?&surface:NULL,pointers?vertices:NULL,n,~(FBITFIELD)0,INT_MIN,1);
        if(calls[0]||calls[1]||calls[2]) {
            fprintf(stderr,"PUBLIC REJECTION FAILED n=%u mode=%u pointers=%u fast=%u add=%u slow=%u\\n",n,mode,pointers,calls[0],calls[1],calls[2]);return 1;
        }
        cases++;
    }
    printf("PUBLIC REJECTION PASS cases=%u PB-entry-calls=%u slow-calls=%u\\n",cases,calls[0]+calls[1],calls[2]);return 0;
}
'''
    report={'complete_public_function_sha256':hashlib.sha256(body.encode()).hexdigest(),
        'actual_unchanged_caller_source_sha256':sha(base/name),'private_slow_guard_source_sha256':hashlib.sha256(slow.encode()).hexdigest(),
        'scope':'Complete unchanged HWR_ProcessPolygon source, entry sinks replace PB/slow callees only; native graph remains a separate proof',
        'runs':{}}
    for tag,mutate,sanitize in [('regular',False,False),('ASan_UBSan',False,True),('negative-remove-size-guard',True,False)]:
        source=body
        if mutate:
            source=source.replace('if (iNumPts < 3)\n\t\treturn;','if (0)\n\t\treturn;',1)
        c=out/(tag+'.c');c.write_text(prefix+source+suffix);binary=out/tag
        flags=['-std=gnu17','-O2','-g','-DPS2','-DPS2_PROFILE','-Wall','-Wextra','-Werror','-I'+str(out)]
        if sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
        command=['gcc',*flags,c,'-o',binary]
        full.run(command,out/(tag+'-build.log'))
        result=subprocess.run([str(binary)],capture_output=True,text=True)
        (out/(tag+'-run.log')).write_text(result.stdout+result.stderr)
        if result.returncode!=(1 if mutate else 0):raise RuntimeError('Public guard run failed: '+tag)
        if mutate and not result.stderr.startswith('PUBLIC REJECTION FAILED '):raise RuntimeError('Guard negative escaped')
        report['runs'][tag]={'source_sha256':sha(c),'binary_sha256':sha(binary),'command':list(map(str,command)),
            'returncode':result.returncode,'stdout':result.stdout.strip(),'stderr':result.stderr.strip()}
    report['status']='PASS public n<3 rejection unchanged; no PBPut helper-domain claim for those inputs'
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'status':report['status'],'report':str(out/'report.json'),'runs':{k:v['returncode'] for k,v in report['runs'].items()}},indent=2))


if __name__=='__main__':main()
