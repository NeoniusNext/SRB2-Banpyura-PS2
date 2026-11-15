"""Freeze and test real light-table producers and palette/CLUT/cache consumers.

Host-only. Every stage is fresh and contains its exact source/tool bytes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[2]


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def function(text,name):
    match=re.search(r'(?m)^(?:static\s+)?(?:inline\s+)?[A-Za-z_][\w \t*]*\b'+re.escape(name)+r'\s*\([^;{}]*\)\s*(?://[^\n]*\n\s*)?\{',text)
    if not match:raise ValueError('Complete function missing '+name)
    at=match.end();depth=1
    while depth:
        depth+=(text[at]=='{')-(text[at]=='}');at+=1
    return text[match.start():at]


def select_private_policy(text,noopt=False):
    """Select only the bounded new opt-out guards; retain every other token."""
    output=[]; selected=None; nested=0
    for line in text.splitlines(keepends=True):
        match=re.fullmatch(r'#if (!?)defined\(PS2_NOOPT_LIGHTTABLE_INDEXED\)\s*\n',line)
        if match:
            if selected is not None:raise ValueError('Nested private opt-out guard')
            selected=(not noopt) if match[1] else noopt
        elif selected is not None and nested and line.strip()=='#endif':
            nested-=1
            if selected:output.append(line)
        elif selected is not None and line.lstrip().startswith('#if'):
            nested+=1
            if selected:output.append(line)
        elif selected is not None and not nested and line.strip()=='#else':selected=not selected
        elif selected is not None and not nested and line.strip()=='#endif':selected=None
        elif selected is None or selected:output.append(line)
    if selected is not None:raise ValueError('Unterminated private opt-out guard')
    return ''.join(output)


def run(command,log,success=True):
    command=list(map(str,command));result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True)
    log.write_text(' '.join(command)+'\n'+result.stdout+result.stderr)
    if success and result.returncode:raise SystemExit('Host fixture command failed '+str(log))
    return result


def prepare(out,base,current):
    if out.exists():raise SystemExit('Fresh immutable host stage required')
    out.mkdir(parents=True)
    for name,source in [('reference-src',base),('candidate-src',current)]:shutil.copytree(source,out/name)
    pinned=out/'pinned-tools';pinned.mkdir()
    for name in ('next12_lighttable_hosttest.py','next12_lighttable_hosttest.c'):
        shutil.copyfile(ROOT/'tools/ps2'/name,pinned/name)
    base=out/'reference-src';current=out/'candidate-src'
    cache=(base/'hardware/hw_cache.c').read_text();curcache=(current/'hardware/hw_cache.c').read_text()
    driver=(base/'ps2/hw/ps2_hwd.c').read_text();curdriver=(current/'ps2/hw/ps2_hwd.c').read_text()
    pal=(base/'ps2/hw/ps2_hw_pal.inc').read_text();curpal=(current/'ps2/hw/ps2_hw_pal.inc').read_text()
    optout_restored='#if defined(PS2_NOOPT_LIGHTTABLE_INDEXED)\nstatic u32 lt_store' in curpal
    selected_pal=select_private_policy(curpal)
    selected_driver=select_private_policy(curdriver)
    shared_store='static u32 lt_store_indexed(' not in curpal
    tex=(base/'ps2/hw/ps2_hw_tex.inc').read_text();priv=(base/'ps2/hw/ps2_hw_priv.inc').read_text()
    gc=(base/'hardware/hw_gcache.inc').read_text();main=(base/'hardware/hw_main.c').read_text()
    draw=(base/'ps2/hw/ps2_hw_draw.inc').read_text()
    mode_start=draw.index('\tif (palette_mode() != pal_last_mode)')
    mode_end=draw.index('\n\t}',mode_start)+3
    (out/'source-mode.inc').write_text(draw[mode_start:mode_end]+'\n')
    for name in ('HWR_CreateLightTable','HWR_UpdateLightTable','HWR_ClearLightTables','HWR_GetTexturePalette'):
        if function(cache,name)!=function(curcache,name):raise SystemExit('Generic engine API changed '+name)
    for name in ('hw_CreateLightTable','hw_UpdateLightTable','hw_ClearLightTables','hw_SetScreenPalette'):
        current_body=function(selected_driver,name)
        if shared_store:
            current_body=current_body.replace('lt_store(0, hw_lighttable, NULL)','lt_store(0, hw_lighttable)').replace('lt_store(id, hw_lighttable, NULL)','lt_store(id, hw_lighttable)')
        if function(driver,name)!=current_body:raise SystemExit('Generic driver API changed beyond private optional-index argument '+name)
        if optout_restored and function(driver,name)!=function(select_private_policy(curdriver,True),name):
            raise SystemExit('Original generic driver opt-out body differs '+name)
    for name in (('lt_clear','pal_light_entries','out_pal','pal_row_of') if shared_store else ('lt_store','lt_clear','pal_light_entries','out_pal','pal_row_of')):
        if function(pal,name)!=function(curpal,name):raise SystemExit('Generic store/consumer changed '+name)
    state=priv[:priv.index('\nint ps2hwd_dbg_flags;')]
    state=state.replace('static struct\n{\n\tint up;','typedef struct\n{\n\tint up;',1)
    state=state.replace('} H __attribute__((aligned(16)));','} __attribute__((aligned(16))) lighttable_state_t;\nextern lighttable_state_t H;',1)
    if 'extern lighttable_state_t H;' not in state:raise SystemExit('Real complete H declaration not found')
    common='''#ifndef NEXT12_LIGHTTABLE_COMMON_H
#define NEXT12_LIGHTTABLE_COMMON_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "doomdef.h"
#include "hardware/hw_glob.h"
#include "hardware/hw_main.h"
#include "hardware/hw_drv.h"
#include "r_data.h"
#include "v_video.h"
#include "z_zone.h"
#include "ps2/hw/ps2_hwd.h"
#include "ps2/hw/ps2_hwd_dbg.h"
typedef UINT8 u8; typedef UINT16 u16; typedef UINT32 u32; typedef UINT64 u64;
typedef struct GSGLOBAL GSGLOBAL;
'''+state+'''
#define LT_MAX 1024
extern u8 *lt_idx[LT_MAX],lt_gen[LT_MAX];
extern u32 spal[256],pal_key[1024];
extern u8 pal_val[1024],clut_hint[256];
extern int spal_set,pal_last_mode,ps2hwd_dbg_flags;
typedef struct { UINT32 a,b; } gcfp_t;
UINT32 reference_HWR_GetLightTableID(extracolormap_t *);
UINT32 candidate_HWR_GetLightTableID(extracolormap_t *);
#endif
'''
    (out/'common.h').write_text(common)
    pieces=[]
    pieces.append(priv[priv.index('enum\n{\n\tHW_SHADER'):priv.index('\nstatic void (*resource_error_handler)')])
    pieces.append(function(main,'HWR_ShouldUsePaletteRendering'))
    pieces.append(function(cache,'HWR_GetTexturePalette'))
    pieces += [function(pal,n) for n in ('palette_mode','out_pal','lt_ok','pal_row_of')]
    pieces += [function(tex,n) for n in ('pal_hash','pal_lookup','pal_build','a8_to_gs','clut_slot')]
    pieces.append(function(pal,'pal_light_entries'))
    start=tex.index('// Reserve a complete 16x16 CT32 CLUT transfer.')
    stop=tex.index('// ---- upload of one texture ----',start)
    pieces.append(tex[start:stop].replace('static u8 clut_hint[256];','u8 clut_hint[256];'))
    pieces += [function(gc,n) for n in ('gcw','gc_init_fp','gc_cmap')]
    pieces.append(function(tex,'palette_set'))
    pieces += [function(driver,n) for n in ('hw_SetTexturePalette','hw_SetScreenPalette')]
    store=function(selected_pal,'lt_store' if shared_store else 'lt_store_indexed')
    rgb_mask='pal_lookup(c.rgba & 0x00FFFFFFu)' in store
    if rgb_mask:
        rgb=store.split('const RGBA_t c = t[indices ? indices[i] : i];',1)[1].split('miss +=',1)[0]
        rgb=re.sub(r'int k = pal_lookup\(([^;]*)\);',r'return (\1);',rgb)
        (out/'rgb-expression.inc').write_text('__attribute__((noinline,noipa))\nstatic u32 fixture_actual_rgb(const RGBA_t *input)\n{\nconst RGBA_t c=*input;\n'+rgb+'}\n')
    rgba=function(pal,'lt_store')
    stores=rgba+'\n'+store+'\n'
    if shared_store:
        stores=rgba.replace('lt_store(', 'reference_lt_store(',1)+'\n'+store.replace('lt_store(', 'candidate_lt_store(',1)+'''
static u32 lt_store(u32 id,const RGBA_t *t,const u8 *indices)
{
    if(active_variant)return candidate_lt_store(id,t,indices);
    CHECK(indices==NULL);
    return reference_lt_store(id,t);
}
'''
    if optout_restored:
        fallback=function(select_private_policy(curpal,True),'lt_store')
        if fallback!=rgba:raise SystemExit('Original complete private opt-out body differs')
        default=stores[stores.index('static u32 candidate_lt_store'):]
        stores=rgba.replace('lt_store(', 'reference_lt_store(',1)+'''
#if defined(PS2_NOOPT_LIGHTTABLE_INDEXED)
'''+fallback.replace('lt_store(', 'candidate_fallback_lt_store(',1)+'''
static u32 lt_store(u32 id,const RGBA_t *t)
{
    return active_variant ? candidate_fallback_lt_store(id,t) : reference_lt_store(id,t);
}
#else
'''+default+'#endif\n'
    (out/'stores.inc').write_text('#define malloc fixture_driver_malloc\n#define free fixture_driver_free\n'+stores+function(pal,'lt_clear')+'\n#undef malloc\n#undef free\n')
    pieces += [function(curdriver if shared_store else driver,n) for n in ('hw_CreateLightTable','hw_UpdateLightTable','hw_ClearLightTables')]
    indexed_bodies='\n\n'.join(function(curdriver,n) for n in ('PS2HWD_CreateIndexedLightTable','PS2HWD_UpdateIndexedLightTable'))
    pieces.append(('#if !defined(PS2_NOOPT_LIGHTTABLE_INDEXED)\n'+indexed_bodies+'\n#endif') if optout_restored else indexed_bodies)
    pieces += [function(cache,n) for n in ('HWR_CreateLightTable','HWR_UpdateLightTable','HWR_ClearLightTables')]
    # Stores need palette lookup; the following actual helpers need stores.
    split=next(i for i,p in enumerate(pieces) if p.startswith('static UINT32 hw_CreateLightTable'))
    (out/'source-before.inc').write_text('\n\n'.join(pieces[:split])+'\n')
    (out/'source-after.inc').write_text('\n\n'.join(pieces[split:])+'\n')
    for variant,text in [('reference',cache),('candidate',curcache)]:
        body=function(text,'HWR_GetLightTableID').replace('HWR_GetLightTableID(',variant+'_HWR_GetLightTableID(',1)
        (out/(variant+'.c')).write_text('#include "common.h"\n__attribute__((noinline,noipa,externally_visible))\n'+body+'\n')
    (out/'driver.c').write_text((pinned/'next12_lighttable_hosttest.c').read_text())
    tested_store=store.replace('lt_store(', 'candidate_lt_store(',1) if shared_store else store
    mutations={
        'direct-index':tested_store.replace('ix[i] = (u8)(k < 0 ? 0 : k);','ix[i] = indices ? indices[i] : (u8)(k < 0 ? 0 : k);' if shared_store else 'ix[i] = t[i];'),
        'driver-forward':tested_store.replace('const RGBA_t c = t[indices ? indices[i] : i];','RGBA_t c = t[indices ? indices[i] : i]; if(indices)c.rgba = H.pal[indices[i]];') if shared_store else tested_store.replace('const RGBA_t c = palette[t[i]];','RGBA_t c; c.rgba = H.pal[t[i]];'),
        'miss-255':tested_store.replace('ix[i] = (u8)(k < 0 ? 0 : k);','ix[i] = (u8)(k < 0 ? 255 : k);'),
        'generation':tested_store.replace('lt_gen[id]++;','lt_gen[id] += 2;'),
        'debug-rgb':tested_store.replace('c1.s.red, c1.s.green, c1.s.blue','c0.s.red, c0.s.green, c0.s.blue'),
    }
    if rgb_mask:
        mutations.update({'rgb-alpha':tested_store.replace('c.rgba & 0x00FFFFFFu','c.rgba'),
                          'rgb-red-bit':tested_store.replace('c.rgba & 0x00FFFFFFu','c.rgba & 0x00FFFFFEu')})
    for name,changed in mutations.items():
        if changed==tested_store:raise SystemExit('Actual source mutation missing '+name)
        (out/('stores-negative-'+name+'.inc')).write_text((out/'stores.inc').read_text().replace(tested_store,changed))
        (out/('driver-negative-'+name+'.c')).write_text((out/'driver.c').read_text().replace('"stores.inc"','"stores-negative-'+name+'.inc"'))
    api=function(curdriver,'PS2HWD_UpdateIndexedLightTable');changed=api.replace('ov_flush_all();','(void)0;')
    if changed==api:raise SystemExit('Update flush mutation missing')
    (out/'source-after-negative-flush.inc').write_text((out/'source-after.inc').read_text().replace(api,changed))
    (out/'driver-negative-flush.c').write_text((out/'driver.c').read_text().replace('"source-after.inc"','"source-after-negative-flush.inc"'))
    getter=(out/'candidate.c').read_text();changed=getter.replace('else if (colormap->gl_lighttable.needs_update)','else')
    if changed==getter:raise SystemExit('Temporal source mutation missing')
    (out/'candidate-negative-temporal.c').write_text(changed)
    manifest={'baseline':str(base),'candidate':str(current),'generic_engine_APIs_and_consumers_source_identical':True,
              'generic_driver_wrappers_only_add_private_NULL_index_argument':shared_store,
              'single_shared_private_store':shared_store,
              'original_private_store_and_wrappers_restored_under_optout':optout_restored,
              'packed_RGBA_word_mask_and_portable_fallback':rgb_mask,
              'negative_controls':list(mutations)+['flush','temporal'],
              'actual_complete_H_declaration':True,'files':{str(p.relative_to(out)):sha(p) for p in sorted(out.rglob('*')) if p.is_file()}}
    (out/'source-hashes.json').write_text(json.dumps(manifest,indent=2)+'\n')


def host(out,sanitize,negatives):
    reports={}
    manifest=json.loads((out/'source-hashes.json').read_text())
    policies=[('ps2',[]),('noopt',['-DPS2_NOOPT_LIGHTTABLE_INDEXED']),('nonps2',['-UPS2'])]
    if manifest['packed_RGBA_word_mask_and_portable_fallback']:policies.append(('portable-rgb',['-U__BYTE_ORDER__']))
    for policy,extra in policies:
        flags=['gcc','-O2','-std=gnu17','-fwrapv','-DPS2','-DPS2_PROFILE','-DHWRENDER','-DNDEBUG',
               '-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable',*extra,
               *(['-DNEXT12_LIGHTTABLE_RGB_MASK'] if manifest['packed_RGBA_word_mask_and_portable_fallback'] else []),
               '-I'+str(out/'candidate-src'),'-I'+str(out)]
        sources=[out/'driver.c',out/'reference.c',out/'candidate.c']
        run([*flags,*sources,'-lm','-o',out/policy],out/(policy+'-build.log'))
        result=run([out/policy],out/(policy+'-test.log'));print(policy+': '+result.stdout.strip(),flush=True)
        reports[policy]={'result':result.stdout.strip(),'flags':flags,'source_mutations_rejected':[]}
        if negatives and policy=='ps2':
            for name in manifest['negative_controls']:
                driver=out/('driver.c' if name=='temporal' else 'driver-negative-'+name+'.c')
                candidate=out/('candidate-negative-temporal.c' if name=='temporal' else 'candidate.c')
                executable=out/('negative-'+name)
                run([*flags,driver,out/'reference.c',candidate,'-lm','-o',executable],out/('negative-'+name+'-build.log'))
                result=run([executable],out/('negative-'+name+'-test.log'),False)
                if result.returncode!=1 or 'LIGHTTABLE FAIL' not in result.stdout:
                    raise SystemExit('Executed actual source control not detected '+name)
                reports[policy]['source_mutations_rejected'].append(name)
    if sanitize:
        flags=['gcc','-O1','-g','-std=gnu17','-fwrapv','-DPS2','-DPS2_PROFILE','-DHWRENDER','-DNDEBUG',
               '-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable',
               '-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie',
               *(['-DNEXT12_LIGHTTABLE_RGB_MASK'] if manifest['packed_RGBA_word_mask_and_portable_fallback'] else []),
               '-I'+str(out/'candidate-src'),'-I'+str(out)]
        run([*flags,out/'driver.c',out/'reference.c',out/'candidate.c','-lm','-o',out/'asan'],out/'asan-build.log')
        result=run([out/'asan'],out/'asan-test.log')
        if result.stderr:raise SystemExit('Sanitizer diagnostics')
        reports['asan']={'result':result.stdout.strip(),'no_diagnostics':True};print('asan: '+result.stdout.strip(),flush=True)
    reports['source_manifest_sha256']=sha(out/'source-hashes.json')
    reports['pinned_tool_sha256']={p.name:sha(p) for p in (out/'pinned-tools').iterdir()}
    (out/'host-report.json').write_text(json.dumps(reports,indent=2)+'\n')


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--base-src',type=Path,default=ROOT/'build/nov15-next12/lighttable/reference-source/src')
    parser.add_argument('--candidate-src',type=Path,default=ROOT/'build/nov15-next12/lighttable/candidate-source/src')
    parser.add_argument('--sanitize',action='store_true');parser.add_argument('--negative-controls',action='store_true')
    args=parser.parse_args();out=args.out.resolve();prepare(out,args.base_src.resolve(),args.candidate_src.resolve());host(out,args.sanitize,args.negative_controls)


if __name__=='__main__':main()
