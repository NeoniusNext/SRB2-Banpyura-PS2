#!/usr/bin/env python3
"""Record the operand-selection argument and complete bound scalar FP order.

This is an instruction-semantics argument, not a native raw-bit execution or
an assertion that host IEEE arithmetic implements the EE. No tools compile,
link or execute EE code here.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct

from audio_storage_next7_finalbind import Object
import next13_geometry_variant as variant


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,default=variant.WORK/'geometry-fp-proof-v2')
    a=p.parse_args();out=a.out.resolve()
    if out.exists() or not out.is_relative_to(variant.WORK):raise RuntimeError('Fresh isolated next13 output required')
    names=('HWR_PBAdd.lto_priv.0','HWR_PBSprQuad','HWR_ProcessPolygon.part.0','HWR_ProcessPolygonSlow')
    binding_path=variant.WORK/'geometry-codecheck-v2/report.json';binding=json.loads(binding_path.read_text())
    rows={}
    for role,path in [('baseline',variant.ROOT/'build/nov15-next12/final/hardware'),('candidate',variant.WORK/'geometry-context-v2')]:
        positive=binding['variants'][role]
        if sha(path/'obj/engine_lto.o')!=positive['engine_sha256'] or sha(path/'SRB2.ELF')!=positive['ELF_sha256']:
            raise RuntimeError('Exact bound actual native graph differs')
        obj=Object(path/'obj/engine_lto.o');rows[role]={}
        for name in names:
            raw=obj.content(obj.sections['.text.'+name]);instructions=struct.unpack('<'+'I'*(len(raw)//4),raw)
            control=[];fp=[]
            for i,w in enumerate(instructions):
                if w>>26!=17:continue
                if ((w>>21)&31) in (2,6):control.append({'offset':i*4,'word_hex':f'{w:08x}'})
                if ((w>>21)&31)==16:fp.append({'offset':i*4,'word_hex':f'{w:08x}','function':w&63,
                    'fs':(w>>11)&31,'ft':(w>>16)&31,'fd':(w>>6)&31})
            if control:raise RuntimeError('An actual body accesses FCSR; revisit the flag proof')
            rows[role][name]={'complete_input_body_sha256':hashlib.sha256(raw).hexdigest(),'bytes':len(raw),
                'FCSR_control_register_instructions':control,'ordered_scalar_FP_instructions':fp,
                'resolved_body_sha256':positive['complete_bound_text'][name]['resolved_text_sha256']}
    report={'kind':__doc__,'status':'FORMAL operand-selection proof and actual-bound-body FP/control inventory; not a native raw-bit execution',
        'source_domain':'Every reachable private PBPut call has n >= 3. Public n<3 rejection is independently executed; no private-helper n0 claim for v2.',
        'value_proof':'MIN.S/MAX.S copies either fs or ft. With identical 32-bit operand patterns, either comparison outcome copies exactly the same pattern; no IEEE ordering, NaN, denormal or zero normalization assumption is used.',
        'flags_proof':'MIN.S/MAX.S clears only O/U. At least vertex1 min/max remains before the same final extent SUBs; no FCSR reader/writer exists in any of the four complete actual native body extents. Their complete linked disassemblies retain s-extent SUB followed by t-extent SUB. Removed clears therefore do not change function-boundary O/U or sticky flags. The original manual lists only coprocessor-unusable as a MIN/MAX exception; usable COP1 is the renderer execution contract.',
        'architecture_reference':{'url':'https://raw.githubusercontent.com/ninjadynamics/PS2Docs/main/EE_Core_Instruction_Set_Manual.pdf',
            'sha256':'104205afc6f5a5071168d8bf693a6c8b4db9a02e1aa66eb51a828e8951f5c069','bytes':1820607,
            'version':'6.0 April 2002','printed_pages':{'MAX.S':363,'MIN.S':365,'SUB.S':377},
            'scope':'Original SCE archival document checked temporarily during this audit; full manual/text not vendored'},
        'complete_actual_bodies':rows,'checker_sha256':sha(Path(__file__)),
        'proof_inputs':{name:{'path':str(path),'sha256':sha(path)} for name,path in {
            'actual_binding':binding_path,'host':variant.WORK/'geometry-host-v3/report.json',
            'public_guard':variant.WORK/'geometry-public-v1/report.json','instruction_inventory':variant.WORK/'geometry-inventory-v2/report.json'}.items()}}
    out.mkdir(parents=True);shutil.copyfile(Path(__file__),out/Path(__file__).name)
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'status':report['status'],'report':str(out/'report.json')},indent=2))


if __name__=='__main__':main()
