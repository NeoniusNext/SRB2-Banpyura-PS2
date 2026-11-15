#!/usr/bin/env python3
"""Bind actual measured default-LTO music instructions to final engine objects.

Compare complete code/data sections and ordered relocation records, including
explicit addends and canonical symbol definitions. No compiler or emulator.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

from audio_storage_next7_finalbind import Object

ROOT=Path(__file__).resolve().parents[2]

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--measured',type=Path,default=ROOT/'build/nov15-next8/baseline/software/engine_lto.o')
    ap.add_argument('--final-object',type=Path,action='append',required=True)
    ap.add_argument('--final-elf',type=Path,action='append',help='pin the corresponding linked ELF; repeat once per final object')
    ap.add_argument('--out',type=Path,default=ROOT/'build/nov15-next8/audio/final-code-binding.json')
    a=ap.parse_args()
    if a.final_elf and len(a.final_elf)!=len(a.final_object):ap.error('--final-elf count must equal --final-object count')
    before=Object(a.measured);result={'measured':str(a.measured),'measured_sha256':digest(a.measured),'final_objects':[]}
    roots=sorted(n for n in before.names if re.fullmatch(r'\.text\.PS2_(?:Music|MIDI)\w+(?:\..*)?',n))
    assert '.text.PS2_MIDIRead.part.0' in roots and '.text.PS2_MusicRender.constprop.0' in roots
    for index,path in enumerate(a.final_object):
        after=Object(path);todo=set(roots);code={};data={};externals=set()
        while todo:
            name=todo.pop()
            if name in code or name in data:continue
            assert name in before.sections and name in after.sections,('missing section',name,path)
            assert before.signature(name)==after.signature(name),('section bytes',name,path)
            reloc=before.relocations.get(name,[])
            assert reloc==after.relocations.get(name,[]),('ordered relocation/addend/target',name,path)
            target=code if before.sections[name][2]&4 else data
            target[name]={**before.signature(name),'ordered_relocations':reloc}
            for r in reloc:
                section=r['target']['section']
                if not section or section.startswith('SPECIAL:'):externals.add(r['target']['name']);continue
                if section in before.sections and before.sections[section][2]&2:todo.add(section)
        section=after.sections['.text.PS2_MusicRender.constprop.0'];entry=after.content(section)[:8]
        assert entry==bytes.fromhex('90ffbd2700080624'),('specialized two-argument 512-frame ABI',path)
        result['final_objects'].append({'path':str(path),'sha256':digest(path),'code_sections':code,'referenced_data':data,
             'code_count':len(code),'relocation_count':sum(len(v['ordered_relocations']) for v in code.values()),
             'two_argument_512_frame_entry':entry.hex(),'external_symbols':sorted(externals)})
        if a.final_elf:result['final_objects'][-1]['elf']={'path':str(a.final_elf[index]),'sha256':digest(a.final_elf[index])}
        print(path,':',len(code),'complete code sections,',len(data),'data sections and',sum(len(v['ordered_relocations']) for v in code.values()),'ordered code relocations identical')
    result['source_sha256']={n:digest(ROOT/'src/ps2'/n) for n in ('ps2_music.c','ps2_midi.c','ps2_audio.c','ps2_pcmconv.h')}
    result['auditor_sha256']=digest(Path(__file__))
    a.out.parent.mkdir(parents=True,exist_ok=True);a.out.write_text(json.dumps(result,indent=2)+'\n')

if __name__=='__main__':main()
