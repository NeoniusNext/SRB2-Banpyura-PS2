#!/usr/bin/env python3
"""Prove metadata-only switch-table renames without masking original RELAs."""
import argparse
import hashlib
import json
from pathlib import Path
import re

from audio_storage_next7_finalbind import Object
from next13_texture_fill3_variant import ROOT


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--base',type=Path,default=ROOT/'build/nov15-next12/final/hardware')
 p.add_argument('--candidate',type=Path,required=True);p.add_argument('--binding',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
 a=p.parse_args()
 if a.out.exists():raise SystemExit('Fresh switch-alias proof required')
 left=Object(a.base/'obj/engine_lto.o');right=Object(a.candidate/'obj/engine_lto.o')
 inventory=json.loads(a.binding.read_text())['code_section_differences']
 selected=[s for s,v in inventory.items() if not v['instruction_or_padding_bytes_changed']]
 aliases={};checked={}
 for sec in selected:
  if left.content(left.sections[sec])!=right.content(right.sections[sec]):raise SystemExit('A metadata-only body changed an instruction/padding byte')
  old=left.relocations.get(sec,[]);new=right.relocations.get(sec,[])
  if len(old)!=len(new):raise SystemExit('Original ordered code RELA counts changed')
  records=[]
  for before,after in zip(old,new):
   if any(before[k]!=after[k] for k in ('offset','type','explicit_addend')):raise SystemExit('Original ordered code RELA location/kind/addend changed')
   ta=dict(before['target']);tb=dict(after['target'])
   if ta!=tb:
    key=(ta['section'],tb['section'])
    if not all(re.fullmatch(r'\.rodata\.CSWTCH\.\d+\.lto_priv\.0',s) for s in key):raise SystemExit('An unproved target changed beyond switch-table numbering')
    old_name=ta.pop('name');new_name=tb.pop('name');old_sec=ta.pop('section');new_sec=tb.pop('section')
    if ta!=tb:raise SystemExit('Switch-table target binding/type/size/value/visibility changed')
    lh=left.sections[old_sec];rh=right.sections[new_sec]
    if any(lh[k]!=rh[k] for k in (1,2,5,8,9)):raise SystemExit('Complete switch-table section type/flags/extent/alignment changed')
    ld=left.content(lh);rd=right.content(rh)
    if ld!=rd or left.relocations.get(old_sec,[]) or right.relocations.get(new_sec,[]):raise SystemExit('Complete switch-table bytes or relocation-free payload changed')
    aliases[old_sec+' -> '+new_sec]={'old_symbol':old_name,'new_symbol':new_name,'complete_bytes':len(ld),
      'full_input_sha256':hashlib.sha256(ld).hexdigest(),'type':lh[1],'flags':lh[2],'alignment':lh[8],
      'all_original_other_target_metadata':ta,'old_and_new_data_RELA_records':[],
      'complete_bytes_and_extent_and_metadata_identical':True}
   records.append({'before':before,'after':after})
  checked[sec]={'complete_code_bytes_identical':True,'ordered_original_RELA_pairs':records}
 if len(selected)!=8 or len(aliases)!=2:raise SystemExit('Unexpected metadata-only alias inventory')
 a.out.write_text(json.dumps({'baseline_engine_sha256':sha(a.base/'obj/engine_lto.o'),
   'candidate_engine_sha256':sha(a.candidate/'obj/engine_lto.o'),'binding_sha256':sha(a.binding),
   'two_complete_switch_payload_aliases':aliases,'eight_instruction_identical_code_sections':checked,
   'no_instruction_or_RELA_field_masked':True,'all_original_records_retained':True},indent=2)+'\n')
 print('Proved2 full switch payload renames across8 instruction-identical original ordered-RELA bodies.')


if __name__=='__main__':main()
