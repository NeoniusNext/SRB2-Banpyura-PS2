#!/usr/bin/env python3
"""Execute isolated compact-patch source, original AP88 oracle and lifecycles."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
sys.path.insert(0,str(HERE.parent))
from patch_rescue_hosttest import extract
from next13_flats_cut_hosttest import function,section


def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()


def run(cmd,log,check=True):
    r=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1'))
    log.write_text(' '.join(map(str,cmd))+'\n'+r.stdout+r.stderr)
    if check and r.returncode:raise RuntimeError(str(log)+'\n'+r.stderr[-6000:])
    return r


def make_source(root):
    cache=(root/'src/hardware/hw_cache.c').read_text()
    tex=(root/'src/ps2/hw/ps2_hw_tex.inc').read_text()
    tt=(root/'src/ps2/hw/ps2_hw_tt.inc').read_text()
    body=extract(root)
    helper=section(cache,'#ifdef HWR_PS2_PACKED_PATCHES\n// Only canonical','#ifdef PS2\n// PS2-140 (OPT10-S): the picture')
    body=body.replace('void HWR_MakePatch (',helper+'\nvoid HWR_MakePatch (',1)
    support=r'''
static void copy_ap88(const GLMipmap_t*m,UINT8*dst){
#ifdef HWR_PS2_PACKED_PATCHES
 if(HWR_PS2_IsPackedPatch(m)){for(UINT32 y=0;y<m->height;y++)for(UINT32 x=0;x<m->width;x++){UINT16 v=HWR_PS2_ReadAP88(m,x,y);size_t p=((size_t)y*m->width+x)*2;dst[p]=(UINT8)v;dst[p+1]=(UINT8)(v>>8);}return;}
#endif
 memcpy(dst,m->data,(size_t)m->width*m->height*2);
}
static int compare_ap88(const GLMipmap_t*m,const UINT8*raw){
#ifdef HWR_PS2_PACKED_PATCHES
 return HWR_PS2_PatchBitmapMatches(m,raw,(size_t)m->width*m->height*2);
#else
 return !memcmp(m->data,raw,(size_t)m->width*m->height*2);
#endif
}
static void proto_extra(GLMipmap_t*m,const UINT8*raw);
'''
    body=body.replace('static void one(patch_t *p,',support+'\nstatic void one(patch_t *p,',1)
    body=body.replace('memcpy(expected,m->data,bytes);','copy_ap88(m,expected);')
    body=body.replace('!memcmp(expected,m->data,bytes)','compare_ap88(m,expected)')
    body=body.replace('digest(expected,bytes);','digest(expected,bytes);proto_extra(m,expected);')
    body=body.replace('int main(int argc,char **argv){','static int lifecycle_main(int argc,char **argv){',1)
    driver='\ntypedef UINT8 u8;typedef UINT16 u16;typedef UINT32 u32;typedef UINT64 u64;\n'
    driver+='enum{PSM_T8=19,PSM_CT32=0};static struct{u32 pal[256];}H;static unsigned ps2hwd_fx2;\n#define FX3_NOFILL 1u\n#define HWDBG_COMPOSE 16777216u\n'
    driver+='typedef void(*rowfill_t)(void*,u32,u8*);\n'
    driver+=section(tex,'typedef struct\n{\n\tconst u8 *src;','// Without decimation uploads')
    for signature in ('static inline u32 a8_to_gs(','static inline u32 src_row(','static inline u32 src_col(','static inline u32 ap88_gather(','static void fill_ap88(','static void fill_ap88_ct32(','static void ap88_scan('):
        driver+=function(tex,signature)
    driver+='#ifdef HWR_PS2_PACKED_PATCHES\n'
    for signature in ('static void fill_patch8(','static void fill_patch32(','static void ap88_scan_patch('):driver+=function(tex,signature)
    driver+='#endif\n'
    case=section(tex,'\t\tcase GL_TEXFMT_AP_88:\n\t\t{','\t\tcase GL_TEXFMT_ALPHA_8:')
    driver+=r'''
typedef struct{int psm,clut,baked,holes;rowfill_t fill;fillctx_t c;}selection_t;
static selection_t select_patch(GLMipmap_t*m,u32 cw,u32 ch){
 const u8*data=m->data;u32 sw=m->width;int psm=0,clut=0,baked=0,holes=0;rowfill_t fill=NULL;fillctx_t fc={0},*c=&fc;
#ifdef HWR_PS2_PACKED_PATCHES
 c->patch=HWR_PS2_IsPackedPatch(m)?m:NULL;
#endif
 switch(m->format){
'''+case+r'''
 default:check(0,"only AP88 selection fixture");}
 selection_t result={psm,clut,baked,holes,fill,fc};return result;
}
'''
    driver+=section(tt,'static struct\n{','// called by hw_SetTexture')
    driver+='static const u32*out_pal(void){return H.pal;}static int pal_lookup(u32 c){for(int i=0;i<256;i++)if(H.pal[i]==c)return i;return -1;}\n'
    driver+=function(tt,'static void tt_capture(')+function(tt,'static int tt_expect(')
    return body+driver+'\n'+(HERE/'packed_patch_hosttest.c').read_text()


def target_abi(root,out):
    import build as B
    data=(root/'src/hardware/hw_data.h').read_text()
    priv=(root/'src/ps2/hw/ps2_hw_priv.inc').read_text()
    # The record type is independent of the context. Use its literal source,
    # complete actual mipmap header and target compiler for both sizes/offsets.
    record=section(priv,'typedef struct\n{\n\tGLMipmap_t *owner;','} texrec_t;')+'} texrec_t;\n'
    probe='#include <stddef.h>\n#include "'+str(root/'src/hardware/hw_data.h')+'"\ntypedef unsigned char u8;typedef unsigned short u16;typedef unsigned int u32;typedef unsigned long long u64;\n#define NCAPS 8\n'+record
    fields=('downloaded','data','nextcolormap','colormap','regen_id','ps2_twin','ps2_planfr','ps2_ihfr','ps2_imfr','ps2_full_fr','ps2_gcv','ps2_spv','height','width','ps2_pi','ps2_uw','ps2_uh','format','flags','ps2_want','ps2_ihint','ps2_nup','ps2_cost')
    expr=['sizeof(GLMipmap_t)','sizeof(GLMapTexture_t)','offsetof(GLMipmap_t,ps2_cpu)','sizeof(texrec_t)']+[f'offsetof(GLMipmap_t,{f})' for f in fields]
    probe+='const unsigned layout[] __attribute__((section(".opt17_layout"),used))={'+','.join(expr)+'};\n'
    c=out/'abi.c';c.write_text(probe);results={}
    flags=[f for f in B.CFLAGS if f not in ('-flto','-MMD','-MP','-DNOHW')]
    incs=['-I'+str(root/'src'),'-I'+str(root/'src/ps2'),'-I'+str(ROOT/'build/nov16-hwopt/candidate-v7/hardware/gen'),*B.INCS[3:]]
    for name,extra,expected in [('compact',[],(68,76,65,140)),('storage-optout',['-DPS2_NOOPT_MIPMAP_STORAGE'],(76,84,74,140)),('layout-optout',['-DPS2_NOOPT_MIPMAP_LAYOUT'],(88,96,87,140))]:
        obj=out/(name+'-abi.o');binary=out/(name+'-abi.bin')
        run([B.CC,*flags,*extra,*incs,'-c',c,'-o',obj],out/(name+'-abi-build.log'))
        run([B.DEV/'ee/bin/mips64r5900el-ps2-elf-objcopy','-O','binary','--only-section=.opt17_layout',obj,binary],out/(name+'-abi-copy.log'))
        vals=struct.unpack('<%dI'%len(expr),binary.read_bytes())
        assert vals[:4]==expected,(name,vals[:4],expected)
        results[name]={'mipmap':vals[0],'map':vals[1],'mode_offset':vals[2],'record':vals[3],'field_offsets':dict(zip(fields,vals[4:])),'object_sha256':sha(obj)}
    return results


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source-root',type=Path,default=ROOT/'build/nov17-sprite-cache/source')
    ap.add_argument('--corpus',type=Path,default=ROOT/'build/nov17-sprite-audit-v2/patch-corpus.bin')
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--negative-controls',action='store_true')
    a=ap.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False);root=a.source_root.resolve()
    source=make_source(root);c=out/'actual-source.c';c.write_text(source)
    frozen=out/'frozen-tools';frozen.mkdir()
    for p in (Path(__file__),HERE/'packed_patch_hosttest.c',HERE.parent/'patch_rescue_hosttest.py',HERE.parent/'patch_rescue_hosttest.c',HERE.parent/'next13_flats_cut_hosttest.py'):shutil.copy2(p,frozen/p.name)
    flags=['gcc','-std=gnu17','-O2','-g','-fwrapv','-DPS2','-DPS2_PROFILE','-DROTSPRITE','-DHWRENDER','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter','-Wno-misleading-indentation','-Wno-clobbered','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-fno-pie','-no-pie']
    matrices=sorted((ROOT/'build/nov16-hwopt/patch-rescue-host-v9/png-oracle/outputs').glob('*.matrix'))
    assert len(matrices)==15
    report={'source_root':str(root),'source_sha256':{str(p.relative_to(root)):sha(p) for p in sorted((root/'src').rglob('*')) if p.is_file()},'generated_sha256':sha(c),'corpus_sha256':sha(a.corpus),'frozen_tools_sha256':{p.name:sha(p) for p in frozen.iterdir()},'results':{},'negative_controls':{},'actual_EE_layout':target_abi(root,out)}
    configs=[('default',[]),('storage-optout',['-DPS2_NOOPT_MIPMAP_STORAGE']),('layout-optout',['-DPS2_NOOPT_MIPMAP_LAYOUT']),('packing-optout',['-DPS2_NOOPT_PATCH_PIXELS'])]
    for name,extra in configs:
        exe=out/name;run([*flags,*extra,c,'-o',exe,'-lm'],out/(name+'-build.log'))
        r=run([exe,a.corpus,*matrices],out/(name+'.log'))
        report['results'][name]=[json.loads(line) for line in r.stdout.splitlines() if line.startswith('{')]
        print(name,json.dumps(report['results'][name]),flush=True)
    for name in ('storage-optout','layout-optout','packing-optout'):
        for before,after in zip(report['results']['default'],report['results'][name]):
            assert before.get('hash')==after.get('hash'),(name,'reference hash')
            assert before.get('compared_bytes')==after.get('compared_bytes'),(name,'comparison scope')
    if a.negative_controls:
        mutations={
            'lost-coverage':('coverage[p >> 3] = previous | bit;','coverage[p >> 3] = previous;'),
            'mapped-transparent-index':('block[p] = index;','block[p] = ((m->flags & TF_CHROMAKEYED) && original == 255) ? 255 : index;'),
            'padding-is-opaque':('return 0x00ffu;','return 0xffffu;'),
            'stale-raw-mode':('grMipmap->ps2_cpu = 0;','grMipmap->ps2_cpu = grMipmap->ps2_cpu;'),
            'rescue-raw-data':('if (!HWR_PS2_MakePatchBitmap(m, patch, true))','if (!MakeBlockEx(m, true))'),
            'wrong-centre':('u32 sx = (1u << c->dx) >> 1;','u32 sx = 0;'),
            'opaque255-fact':('mode |= HWR_PS2_CPU_OPAQUE255;','mode |= 0;'),
            'hole-fact':('mode |= HWR_PS2_CPU_HAS_HOLES;','mode |= 0;'),
            'palette-dependent-hole':('&& ((m->ps2_cpu & HWR_PS2_CPU_HOLES255) || !(m->ps2_cpu & HWR_PS2_CPU_HAS_HOLES)))','&& 1)'),
        }
        for name,(old,new) in mutations.items():
            assert old in source,name
            f=out/('negative-'+name+'.c');f.write_text(source.replace(old,new,1));exe=f.with_suffix('')
            run([*flags,f,'-o',exe,'-lm'],out/(name+'-build.log'))
            r=run([exe,a.corpus,*matrices],out/(name+'.log'),False)
            assert r.returncode!=0,('undetected mutation',name)
            report['negative_controls'][name]={'detected':True,'exit':r.returncode,'log_sha256':sha(out/(name+'.log'))}
            print('negative',name,'detected',flush=True)
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__':main()
