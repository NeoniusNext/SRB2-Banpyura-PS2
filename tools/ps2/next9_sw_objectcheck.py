"""Prove scope, complete real-r_draw objects and exact disabled policies."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
from next7_render_fog_hosttest import function,run
from next7_render_fog_codecheck import Elf32
from next6_render_npot_objectcheck import sections,totals,digest

ROOT=Path(__file__).resolve().parents[2]


def relas(elf,excluded):
    result=[]
    for section in elf.sections:
        if section[1]!=4 or not elf.names[section[7]].startswith('.text') or elf.names[section[7]] in excluded:continue
        values=[]
        for at in range(section[4],section[4]+section[5],section[9]):
            location,info,addend=struct.unpack_from('<IIi',elf.data,at)
            values.append((location,info&255,elf.symbols[section[6]][info>>8],addend))
        result.append((elf.names[section[7]],values))
    return result


def main():
    import build as B
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',required=True,type=Path)
    p.add_argument('--base-src',required=True,type=Path)
    a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True);base=a.base_src.resolve()
    before=(base/'r_draw.c').read_text();after=(ROOT/'src/r_draw.c').read_text()
    if after.replace(function(after,'R_CalcTiltedLighting'),function(before,'R_CalcTiltedLighting'))!=before:
        raise SystemExit('Unexpected production changes outside integer clamp helper')
    for name in ('r_draw8.c','r_draw8_npo2.c','r_plane.c','r_segs.c'):
        if (base/name).read_bytes()!=(ROOT/'src'/name).read_bytes():raise SystemExit('Unexpected source change: '+name)
    report={'source_sha256':hashlib.sha256(after.encode()).hexdigest(),'restoring_only_integer_helper_reconstructs_baseline_byte_for_byte':True,'policies':{}}
    flags=[f for f in B.CFLAGS if not f.startswith('-MMD') and f not in ('-MP','-flto')]
    for policy,extra in [('profile',[]),('noopt-draw',['-DPS2_NOOPT_DRAW']),('noopt',['-DPS2_NOOPT'])]:
        pair={};objects={};data={}
        for version,src in [('reference',base),('candidate',ROOT/'src')]:
            folder=out/version;folder.mkdir(exist_ok=True)
            for name in ('r_draw.c','r_draw8.c','r_draw8_npo2.c'):shutil.copyfile(src/name,folder/name)
            obj=out/(policy+'-'+version+'.o')
            command=[B.CC,*flags,*extra,'-Werror','-I'+str(src),*B.INCS]
            run([*command,'-c',folder/'r_draw.c','-o',obj],out/(policy+'-'+version+'-build.log'))
            pair[version]=sections(obj,out);objects[version]=Elf32(obj)
            data[version]=totals(pair[version])
            data[version]['text_rodata_digest']=digest(pair[version])
            if policy!='profile':
                text=run([*command,'-E','-P',folder/'r_draw.c'],out/(policy+'-'+version+'.i')).stdout
                tokens=re.sub(r'\s+','',''.join(function(text,name) for name in ('R_CalcTiltedLighting','R_CalcSlopeLight')))
                data[version]['preprocessed_helper_tokens_sha256']=hashlib.sha256(tokens.encode()).hexdigest()
        left,right=pair['reference'],pair['candidate'];names=left.keys()|right.keys()
        changed={name for name in names if name.startswith('.text') and left.get(name)!=right.get(name)}
        allowed={'.text.R_CalcSlopeLight'} if policy=='profile' else set()
        if changed!=allowed:raise SystemExit('Unexpected changed complete text sections: '+str(sorted(changed)))
        if relas(objects['reference'],allowed)!=relas(objects['candidate'],allowed):raise SystemExit('Unchanged/caller text RELAs differ')
        bss=[{n:s for n,(s,_) in obj.items() if n.startswith(('.bss','.sbss'))} for obj in (left,right)]
        rodata=[Counter((s,hashlib.sha256(b).hexdigest()) for n,(s,b) in obj.items() if n.startswith('.rodata')) for obj in (left,right)]
        if bss[0]!=bss[1] or rodata[0]!=rodata[1] or any(left.get(n)!=right.get(n) for n in names if n.startswith(('.data','.sdata'))):
            raise SystemExit('Data, rodata or BSS changed')
        if policy!='profile' and data['reference']!=data['candidate']:raise SystemExit('Disabled policy differs')
        data['changed_text_sections']=sorted(changed)
        data['all_other_text_and_ordered_RELA_streams_identical']=True
        data['data_rodata_bss_unchanged']=True
        report['policies'][policy]=data
    (out/'object-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Actual r_draw scope, text/RELAs, memory and both disabled policies pass')


if __name__=='__main__':main()
