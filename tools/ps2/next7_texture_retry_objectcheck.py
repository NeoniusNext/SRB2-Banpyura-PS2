"""Compile actual composite generator, measure code/frames, prove opt-out identity."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/ps2'))
import build as B
from next6_render_npot_objectcheck import sections,totals,digest,run

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',required=True,type=Path);p.add_argument('--base-file',required=True,type=Path);a=p.parse_args()
    out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
    # Both source versions intentionally use the same current shared headers:
    # isolate this recovery change from the earlier location-layout candidate.
    reference=out/'reference-r_textures.c';reference.write_bytes(a.base_file.read_bytes())
    flags=[f for f in B.CFLAGS if f not in ('-flto','-MMD','-MP')]
    results={};items={}
    for name,source,extra in [('reference',reference,[]),('candidate',ROOT/'src/r_textures.c',[]),('old-switch',ROOT/'src/r_textures.c',['-DPS2_NOOPT_TEXRETRY'])]:
        obj=out/(name+'.o');run([B.CC,*flags,*extra,'-Werror',*B.INCS,'-c',source,'-o',obj],out/(name+'-build.log'))
        info=sections(obj,out);items[name]=info;data=totals(info);data['code_sha256']=digest(info);data['source_sha256']=hashlib.sha256(source.read_bytes()).hexdigest()
        asm=run(['mips64r5900el-ps2-elf-objdump','-dr','--disassemble=R_GenerateTexture',obj],out/(name+'-generator.asm'))
        frame=re.search(r'addiu\s+sp,sp,(-\d+)',asm);data['generator_frame']=-int(frame[1]) if frame else 0;data['generator_text_bytes']=info['.text.R_GenerateTexture'][0]
        data['recovery_helpers']={n:v[0] for n,v in info.items() if n.startswith('.text.R_') and any(k in n for k in ('CompositeRetry','TryComposite'))}
        results[name]=data
    assert digest(items['reference'])==digest(items['old-switch']),'opt-out complete code/rodata identity failed'
    assert all(items['reference'].get(n)==items['old-switch'].get(n) for n in items['reference'].keys()|items['old-switch'].keys() if n.startswith(('.data','.sdata','.bss','.sbss'))),'opt-out storage identity failed'
    for kind,prefix in [('data',('.data','.sdata')),('bss',('.bss','.sbss'))]:
        assert results['reference'][kind]==results['candidate'][kind],kind+' storage changed'
    results['old_switch_complete_code_rodata_storage_identical']=True
    (out/'report.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results,indent=2))
if __name__=='__main__':main()
