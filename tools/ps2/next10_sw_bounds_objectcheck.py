"""Compile real r_plane.c in matching current-header contexts and check scope."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct
from next3_render_tilted_hosttest import function,run
from next6_render_npot_objectcheck import sections,totals,digest
from next7_render_fog_codecheck import Elf32

ROOT=Path(__file__).resolve().parents[2]
NAME='R_PlaneBounds'


def relas(elf,excluded):
    result=[]
    for section in elf.sections:
        name=elf.names[section[7]]
        if section[1]!=4 or not name.startswith('.text') or name in excluded:continue
        records=[]
        for at in range(section[4],section[4]+section[5],section[9]):
            location,info,addend=struct.unpack_from('<IIi',elf.data,at)
            target=elf.symbols[section[6]][info>>8]
            if re.fullmatch(r'\.rodata\.CSWTCH\.\d+',target):
                index=elf.index[target];literal=elf.sections[index]
                if any(s[7]==index and s[1] in (4,9) for s in elf.sections):
                    raise SystemExit('Generated switch data has unhandled RELAs')
                target=('complete_generated_switch_literal',literal[5],literal[8],elf.bytes(literal).hex())
            records.append((location,info&255,target,addend))
        result.append((name,records))
    return result


def main():
    import build as B
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,required=True);p.add_argument('--base-src',type=Path,required=True)
    a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True);base=a.base_src.resolve()
    before=(base/'r_plane.c').read_text();after=(ROOT/'src/r_plane.c').read_text()
    if after.replace(function(after,NAME),function(before,NAME))!=before:
        raise SystemExit('Unexpected source change outside exact bounds helper')
    report={'source_sha256':hashlib.sha256(after.encode()).hexdigest(),
            'restoring_only_bounds_helper_reconstructs_entire_frozen_file':True,
            'both_versions_use_current_headers_including_other_pass10_layout_changes':True,
            'generated_CSWTCH_symbol_numbers_normalize_to_complete_literal_bytes_and_alignment':True,'policies':{}}
    flags=[f for f in B.CFLAGS if not f.startswith('-MMD') and f not in ('-MP','-flto')]
    for policy,extra in [('profile',[]),('noopt-bounds',['-DPS2_NOOPT_PLANEBOUNDS_MMI']),
                          ('noopt-rend',['-DPS2_NOOPT_REND']),('noopt',['-DPS2_NOOPT'])]:
        pair={};objects={};data={}
        for version,source in [('reference',before),('candidate',after)]:
            folder=out/version;folder.mkdir(exist_ok=True)
            path=folder/'r_plane.c';path.write_text(source)
            obj=out/(policy+'-'+version+'.o')
            command=[B.CC,*flags,*extra,'-Werror',*B.INCS]
            run([*command,'-c',path,'-o',obj],out/(policy+'-'+version+'-build.log'))
            pair[version]=sections(obj,out);objects[version]=Elf32(obj)
            data[version]=totals(pair[version]);data[version]['text_rodata_digest']=digest(pair[version])
            if policy!='profile':
                preprocessed=run([*command,'-E','-P',path],out/(policy+'-'+version+'.i')).stdout
                tokens=re.sub(r'\s+','',function(preprocessed,NAME))
                data[version]['preprocessed_helper_tokens_sha256']=hashlib.sha256(tokens.encode()).hexdigest()
        left,right=pair['reference'],pair['candidate'];names=left.keys()|right.keys()
        changed={n for n in names if n.startswith('.text') and left.get(n)!=right.get(n)}
        allowed={'.text.'+NAME} if policy=='profile' else set()
        if changed!=allowed:raise SystemExit('Unexpected changed complete text sections '+str(sorted(changed)))
        if relas(objects['reference'],allowed)!=relas(objects['candidate'],allowed):
            raise SystemExit('Other text ordered RELAs differ')
        bss=[{n:size for n,(size,_) in obj.items() if n.startswith(('.bss','.sbss'))} for obj in (left,right)]
        rodata=[Counter((size,hashlib.sha256(raw).hexdigest()) for n,(size,raw) in obj.items() if n.startswith('.rodata')) for obj in (left,right)]
        if bss[0]!=bss[1] or rodata[0]!=rodata[1] or any(left.get(n)!=right.get(n) for n in names if n.startswith(('.data','.sdata'))):
            raise SystemExit('Data, rodata or BSS changed')
        if policy!='profile' and data['reference']!=data['candidate']:
            raise SystemExit('Disabled policy differs')
        data.update({'only_changed_text_sections':sorted(changed),'all_other_text_and_ordered_RELAs_identical':True,
                     'data_rodata_bss_identical':True,'bounds_text_bytes':{v:pair[v]['.text.'+NAME][0] for v in pair}})
        report['policies'][policy]=data
    (out/'object-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Real matching-context r_plane objects: scope, code/RELAs, data and disabled policies pass')


if __name__=='__main__':main()
