#!/usr/bin/env python3
"""Prove try-only AP88 rescue against complete pre-purge production pixels."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

from next13_flats_cut_hosttest import function, section
import strip_pics

ROOT=Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command,log=None,check=True,env=None):
    r=subprocess.run(list(map(str,command)),cwd=ROOT,capture_output=True,text=True,env=env)
    if log:log.write_text(" ".join(map(str,command))+"\n"+r.stdout+r.stderr)
    if check and r.returncode:raise RuntimeError(str(command)+"\n"+(r.stdout+r.stderr)[-5000:])
    return r


def extract(root):
    data=(root/'src/hardware/hw_data.h').read_text()
    defs=(root/'src/hardware/hw_defs.h').read_text()
    rd=(root/'src/r_defs.h').read_text()
    tex=(root/'src/r_textures.h').read_text()
    pichead=(root/'src/r_picformats.h').read_text()
    picture=(root/'src/r_picformats.c').read_text()
    patch=(root/'src/r_patch.c').read_text()
    rotation=(root/'src/r_patchrotation.c').read_text()
    fixed=(root/'src/m_fixed.h').read_text()
    cache=(root/'src/hardware/hw_cache.c').read_text()
    dtype=(root/'src/doomtype.h').read_text()
    fixture=(ROOT/'tools/ps2/patch_rescue_hosttest.c').read_text()
    preamble,rest=fixture.split('/* TYPES */')
    services,suffix=rest.split('/* FUNCTIONS */')
    types=section(dtype,'typedef struct\n{\n\tUINT8 red;','typedef union FColorRGBA RGBA_t;')+'typedef union FColorRGBA RGBA_t;\n'
    types+=section(data,'typedef enum GLTextureFormat_e','// Patch information for the hardware renderer.')
    types+=section(data,'struct GLPatch_s','typedef struct GLPatch_s GLPatch_t;')+'typedef struct GLPatch_s GLPatch_t;\n'
    types+=section(defs,'enum ETextureFlags','struct FTextureInfo')
    types+=section(rd,'// posts are runs of non masked source pixels','typedef struct\n{\n\tunsigned topdelta;')
    types+=section(rd,'typedef struct\n{\n\tunsigned topdelta;','//\n// OTHER TYPES')
    types+=section(rd,'#ifdef ROTSPRITE\ntypedef struct\n{\n\tINT32 angles;','#if defined(_MSC_VER)\n#pragma pack(1)')
    types+=section(rd,'typedef struct\n{\n\tINT16 width;          // bounding box size','typedef enum\n{\n\tRF_HORIZONTALFLIP')
    types+=section(tex,'typedef struct\n{\n\t// Block origin','// texture type')
    types+=section(pichead,'typedef enum\n{\n\tPICFMT_NONE','void *Picture_Convert(')
    source=preamble+types+services
    for name in ('FixedMul','FixedInt','FixedFloor','FixedCeil'):
        source+=function(fixed,'FUNCMATH FUNCINLINE static ATTRINLINE fixed_t '+name+'(')
    source+=function(patch,'patch_t *Patch_Create(')
    source+=section(patch,'#define PATCH_ALIGN','patch_t *Patch_CreateFromDoomPatch(')
    for name in ('patch_t *Patch_CreateFromDoomPatch(','void Patch_CalcDataSizes(','void Patch_MakeColumns(','boolean Patch_IsEvictable('):source+=function(patch,name)
    source+=function(patch,'static void Patch_FreeData(')
    source+=function(patch,'void Patch_Free(')
    for name in ('void HWR_FreeTextureData(','void HWR_FreeTexture(','void HWR_FreeTextureColormaps('):source+=function(cache,name)
    source+=section(picture,'static void *ReadPixelFunc_Patch(','/** Converts a picture to a flat.')
    for name in ('void *Picture_GetPatchPixel(','INT32 Picture_FormatBPP(','boolean Picture_IsPatchFormat(','boolean Picture_IsInternalPatchFormat(','boolean Picture_IsDoomPatchFormat(','boolean Picture_IsFlatFormat('):source+=function(picture,name)
    source+=function(rotation,'static void RotatedPatch_CalculateDimensions(')
    source+=function(rotation,'void RotatedPatch_DoRotation(')
    for name in ('static INT32 format2bpp(','static void HWR_DrawColumnInCache(','static void HWR_DrawPatchInCache(','static UINT8 *MakeBlockEx(','static UINT8 *MakeBlock(','void HWR_MakePatch '):source+=function(cache,name)
    source+=section(cache,'#if defined(PS2) && !defined(PS2_NOOPT_PATCH_RESCUE)','void HWR_PS2_RegenerateMipmap(')
    source+=function(cache,'void HWR_PS2_RegenerateMipmap(')
    return source+suffix


def png_oracle(out, assets):
    work=out/'png-oracle';work.mkdir()
    strip_pics.write_stub_sources(work)
    compat=work/'compat.c';compat.write_text('#include <ctype.h>\nchar *strupr(char*s){for(char*p=s;*p;p++)*p=toupper((unsigned char)*p);return s;}\nchar *strlwr(char*s){for(char*p=s;*p;p++)*p=tolower((unsigned char)*p);return s;}\n')
    pngroot=Path('/workspace/.onboarding/libpng-host')
    flags=['gcc','-std=gnu17','-O2','-fwrapv','-DNDEBUG','-DNOHW','-DNOMD5','-DCMAKECONFIG','-DUNIXCOMMON','-DNOEXECINFO','-DHAVE_PNG','-I'+str(ROOT/'src'),'-I'+str(ROOT/'build/host-gen'),'-I'+str(work),'-I'+str(pngroot/'include/libpng16'),'-I'+str(pngroot/'include')]
    exe=work/'png2matrix'
    run([*flags,ROOT/'tools/ps2/strip_pics_host.c',ROOT/'src/r_picformats.c',compat,pngroot/'lib/libpng16.a','-lz','-lm','-o',exe],work/'build.log')
    inputs=work/'inputs';inputs.mkdir();outputs=work/'outputs';outputs.mkdir()
    with zipfile.ZipFile(assets/'srb2.pk3') as z:
        palette=work/'PLAYPAL';palette.write_bytes(z.read('PLAYPAL'))
        pngs=[]
        for name in z.namelist():
            raw=z.read(name)
            if raw.startswith(b'\x89PNG\r\n\x1a\n'):
                p=inputs/(str(len(pngs))+'.png');p.write_bytes(raw);pngs.append(p)
    run([exe,palette,outputs,*pngs],work/'run.log')
    matrices=sorted(outputs.glob('*.matrix'))
    if not matrices:raise RuntimeError('No real PNG oracle cases')
    return matrices,{'binary_sha256':sha(exe),'inputs':{str(p.relative_to(out)):sha(p) for p in [palette,*pngs]},'matrices':{str(p.relative_to(out)):sha(p) for p in matrices},'libpng_sha256':sha(pngroot/'lib/libpng16.a')}


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source-root',type=Path,default=ROOT)
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--assets',type=Path,default=Path('/workspace/.onboarding/srb2-assets'))
    ap.add_argument('--negative-controls',action='store_true')
    a=ap.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
    source_root=a.source_root.resolve()
    source=extract(source_root);(out/'actual-source.c').write_text(source)
    fixture=ROOT/'tools/ps2/patch_rescue_hosttest.c'
    frozen=out/'frozen-tools';frozen.mkdir()
    for f in (Path(__file__),fixture,ROOT/'tools/ps2/next13_flats_cut_hosttest.py',ROOT/'tools/ps2/strip_pics.py',ROOT/'tools/ps2/strip_pics_host.c'):shutil.copy2(f,frozen/f.name)
    baseline=subprocess.check_output(['git','show','604cfd150:src/hardware/hw_cache.c'],cwd=ROOT).decode()
    current=(source_root/'src/hardware/hw_cache.c').read_text()
    for name in ('static INT32 format2bpp(','static void HWR_DrawColumnInCache(','static void HWR_DrawPatchInCache(','static UINT8 *MakeBlockEx(','static UINT8 *MakeBlock(','void HWR_MakePatch '):
        if function(baseline,name)!=function(current,name):raise RuntimeError('Normal patch recipe changed: '+name)
    matrices,png=png_oracle(out,a.assets)
    flags=['gcc','-std=gnu17','-O2','-g','-fwrapv','-DPS2','-DPS2_PROFILE','-DROTSPRITE','-DHWRENDER','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter','-Wno-misleading-indentation','-Wno-clobbered','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-fno-pie','-no-pie']
    source_inputs={name:sha(source_root/name) for name in ('src/hardware/hw_cache.c','src/hardware/hw_data.h','src/hardware/hw_defs.h','src/doomtype.h','src/r_defs.h','src/r_textures.h','src/r_picformats.h','src/r_picformats.c','src/r_patch.c','src/r_patchrotation.c','src/m_fixed.h')}
    before=out/'baseline-regenerate.c';before.write_text(function(baseline,'void HWR_PS2_RegenerateMipmap(').replace('&gl_textures[m->regen_id]','HWR_MapTexture((size_t)m->regen_id, true)'))
    after=out/'candidate-regenerate.c';after.write_text(function(current,'void HWR_PS2_RegenerateMipmap('))
    identities={}
    for name,defines in [('rescue-optout',['-DPS2','-DPS2_PROFILE','-DPS2_NOOPT_PATCH_RESCUE']),('nonps2-profile',['-DPS2_PROFILE'])]:
        texts=[run(['gcc','-E','-P',*defines,f]).stdout for f in (before,after)]
        if texts[0]!=texts[1]:raise RuntimeError('Inactive regeneration path changed: '+name)
        identities[name]=True
    report={'source_inputs':source_inputs,'frozen_tools_sha256':{f.name:sha(f) for f in frozen.iterdir()},'inactive_regeneration_exact_to_pre_rescue_paged_accessor':identities,'source_root':str(source_root),'source_sha256':sha(source_root/'src/hardware/hw_cache.c'),'header_sha256':sha(source_root/'src/hardware/hw_data.h'),'generated_sha256':sha(out/'actual-source.c'),'normal_MakePatch_exact_to_baseline':True,'png_oracle':png,'results':{},'negative_controls':{}}
    env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1')
    for name,extra in [('default',[]),('storage-optout',['-DPS2_NOOPT_MIPMAP_STORAGE']),('optout',['-DPS2_NOOPT_PATCH_RESCUE'])]:
        exe=out/name;run([*flags,*extra,out/'actual-source.c','-o',exe,'-lm'],out/(name+'-build.log'))
        result=run([exe,*matrices],out/(name+'.log'),env=env)
        report['results'][name]=json.loads(result.stdout)
    for key in ('cases','compared_bytes','hash'):
        if report['results']['default'][key]!=report['results']['storage-optout'][key]:raise RuntimeError('Header opt-out changes rescue pixels')
    if a.negative_controls:
        mutations={'mandatory-allocation':('if (!MakeBlockEx(m, true))','if (!MakeBlockEx(m, false))'),'wrong-colormap':('HWR_DrawPatchInCache(m, m->width, m->height, patch->width, patch->height, patch);','GLColormap_t *saved=m->colormap; m->colormap=NULL; HWR_DrawPatchInCache(m, m->width, m->height, patch->width, patch->height, patch); m->colormap=saved;'),'wrong-tag-range':('Z_IterateTags(PU_PATCH, PU_PATCH_ROTATED, HWR_PS2_FindPatchSource);','Z_IterateTags(PU_PATCH, PU_HUDGFX, HWR_PS2_FindPatchSource);'),'allocation-in-iterator':('const patch_t *patch = mem;','const patch_t *patch = mem; (void)Z_Malloc(1,PU_STATIC,NULL);'),'missing-size-guard':('|| (size_t)m->width * m->height > (size_t)0x7FFFFFF0u / 2u','|| false'),'wrong-parent':('if (mipmap == ps2_patch_source.mipmap)','if (mipmap == ps2_patch_source.mipmap && !mipmap->colormap)')}
        for name,(before,after) in mutations.items():
            if before not in source:raise RuntimeError('Missing mutation anchor '+name)
            f=out/('negative-'+name+'.c');f.write_text(source.replace(before,after,1));exe=f.with_suffix('')
            run([*flags,f,'-o',exe,'-lm'],f.with_suffix('.build.log'))
            result=run([exe],f.with_suffix('.log'),False,env)
            if result.returncode==0:raise RuntimeError('Undetected source corruption: '+name)
            report['negative_controls'][name]={'detected':True,'exit':result.returncode,'log_sha256':sha(f.with_suffix('.log'))}
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'out':str(out),'results':report['results'],'negative_controls':len(report['negative_controls'])}))


if __name__=='__main__':main()
