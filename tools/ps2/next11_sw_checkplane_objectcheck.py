"""Compile full r_plane.c with frozen headers; scope and opt-out identity gate."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re

from next11_sw_checkplane import run,sha
from plane_bank_hosttest import function
from next6_render_npot_objectcheck import sections,totals,digest
from next10_sw_bounds_objectcheck import relas
from next10_sw_bounds_codecheck import frame
from next7_render_fog_codecheck import Elf32

ROOT=Path(__file__).resolve().parents[2]
NAME='R_CheckPlane'


def main():
    import build as B
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True);parser.add_argument('--base-src',type=Path,required=True)
    args=parser.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True);base=args.base_src.resolve()
    before=(base/'r_plane.c').read_text();after=(ROOT/'src/r_plane.c').read_text()
    if after.replace(function(after,NAME),function(before,NAME))!=before:
        raise SystemExit('Source changed outside complete selected helper')
    flags=[f for f in B.CFLAGS if f not in ('-flto','-MMD','-MP')]
    prefix='-I'+str(ROOT/'src')
    includes=['-I'+str(base)+f[len(prefix):] if f.startswith(prefix) else f for f in B.INCS]
    result={'source_sha256':hashlib.sha256(after.encode()).hexdigest(),
            'restoring_only_selected_helper_reconstructs_entire_frozen_file':True,
            'all_versions_use_same_frozen_baseline_headers':True,'policies':{}}
    for policy,extra in [('profile',[]),('noopt-scan',['-DPS2_NOOPT_CHECKPLANE_CURSOR']),
                          ('noopt-rend',['-DPS2_NOOPT_REND']),('noopt',['-DPS2_NOOPT']),
                          ('hardware-fallback',['-DHWRENDER','-Wno-format-truncation'])]:
        policy_flags=[f for f in flags if f!='-DNOHW'] if policy=='hardware-fallback' else flags
        if policy!='hardware-fallback' and '-DNOHW' not in policy_flags:
            raise SystemExit('Software object policy requires actual NOHW definition')
        pair={};objects={};report={}
        for version,text in [('reference',before),('candidate',after)]:
            folder=out/version;folder.mkdir(exist_ok=True);source=folder/'r_plane.c';source.write_text(text)
            target=out/(policy+'-'+version+'.o');command=[B.CC,*policy_flags,*extra,'-Werror',*includes]
            run([*command,'-c',source,'-o',target],out/(policy+'-'+version+'-build.log'))
            pair[version]=sections(target,out);objects[version]=Elf32(target)
            report[version]=totals(pair[version]);report[version]['text_rodata_digest']=digest(pair[version])
            report[version]['exact_frame']=frame(objects[version],NAME)
            if policy!='profile':
                preprocessed=run([*command,'-E','-P',source],out/(policy+'-'+version+'.i')).stdout
                tokens=re.sub(r'\s+','',function(preprocessed,NAME))
                report[version]['preprocessed_helper_tokens_sha256']=hashlib.sha256(tokens.encode()).hexdigest()
        left,right=pair['reference'],pair['candidate'];names=left.keys()|right.keys()
        changed={n for n in names if n.startswith('.text') and left.get(n)!=right.get(n)}
        allowed={'.text.'+NAME} if policy=='profile' else set()
        if changed!=allowed or relas(objects['reference'],allowed)!=relas(objects['candidate'],allowed):
            raise SystemExit('Other complete instructions or ordered RELAs changed')
        bss=[{n:size for n,(size,_) in obj.items() if n.startswith(('.bss','.sbss'))} for obj in (left,right)]
        rodata=[Counter((size,hashlib.sha256(raw).hexdigest()) for n,(size,raw) in obj.items() if n.startswith('.rodata')) for obj in (left,right)]
        if bss[0]!=bss[1] or rodata[0]!=rodata[1] or any(left.get(n)!=right.get(n) for n in names if n.startswith(('.data','.sdata'))):
            raise SystemExit('Data, complete literals or capacities changed')
        if policy!='profile' and report['reference']!=report['candidate']:
            raise SystemExit('Opt-out complete code/tokens/frame differs')
        report.update(only_changed_text_sections=sorted(changed),all_other_complete_instructions_and_ordered_RELAs_equal=True,
                      all_data_rodata_BSS_names_bytes_capacities_equal=True,
                      helper_text_bytes={v:pair[v]['.text.'+NAME][0] for v in pair})
        result['policies'][policy]=report
    (out/'object-report.json').write_text(json.dumps(result,indent=2)+'\n')
    print('Full frozen-header r_plane object scope, exact data/capacity and all opt-out policies pass',flush=True)


if __name__=='__main__':main()
