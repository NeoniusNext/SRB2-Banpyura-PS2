#!/usr/bin/env python3
"""Bind both fixed-local-RNG copies to the complete canonical row/SDK graph.

Read-only: no compiler, emulator, source edit or ELF patch is performed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from next7_render_fog_codecheck import Elf32
from next8_sw_codecheck import linked_symbols
from next10_sw_bounds_codecheck import output_bytes
from next12_local_rng import stub_words,verify_patch
from next13_texture_fill_provenance import freeze_checker,finish_checker


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--canonical-binding',type=Path,required=True)
 p.add_argument('--diagnostics',type=Path,required=True)
 p.add_argument('--out',type=Path,required=True)
 a=p.parse_args()
 if a.out.exists():raise SystemExit('Fresh diagnostic graph-binding destination required')
 a.out.mkdir(parents=True)
 checker=freeze_checker(a.out,Path(__file__))
 binding=json.loads(a.canonical_binding.read_text());build=Path(binding['final'])
 report_path=a.diagnostics/'patch-report.json';report=json.loads(report_path.read_text())
 original_path=build/'SRB2.ELF';original=Elf32(original_path);symbols=linked_symbols(original)
 if sha(original_path)!=binding['final_ELF_sha256'] or sha(build/'obj/engine_lto.o')!=binding['final_engine_sha256']:
  raise SystemExit('Canonical binding no longer pins exact original ELF/engine')
 expected={'elf':original_path,'engine':build/'obj/engine_lto.o','map':build/'SRB2.map'}
 if not report['diagnostic_only'] or set(report['variants'])!={'01b2d3f5','9e3779b9'}:
  raise SystemExit('Both requested diagnostic-only seed copies are required')
 for key,path in expected.items():
  if report['original_inputs'][key]['sha256']!=sha(path):raise SystemExit('Diagnostic origin differs from canonical '+key)
 initializer=symbols['M_RandomSeedFromOS'];seeder=symbols['M_RandomSeed']
 header=original.sections[initializer['section']]
 start=header[4]+initializer['value']-header[3];size=initializer['size']
 pins={a.canonical_binding.resolve(),report_path.resolve(),*expected.values(),*(Path(v['ELF']) for v in report['variants'].values())}
 before={str(path):sha(path) for path in sorted(pins)}
 result={}
 for key,variant in report['variants'].items():
  path=Path(variant['ELF']);parsed=Elf32(path)
  if sha(path)!=variant['ELF_sha256'] or int(key,16)!=variant['seed_uint32']:
   raise SystemExit('Diagnostic copy/seed pin changed')
  payload=struct.pack('<10I',*stub_words(variant['seed_uint32'],seeder['value']))
  verify_patch(original.data,parsed.data,start,size,payload)
  if linked_symbols(parsed)!=symbols:raise SystemExit('Diagnostic defined symbols/addresses/extent changed')
  def span(address,count,label):
   if address<initializer['value']+size and initializer['value']<address+count:
    raise SystemExit('Bound graph unexpectedly overlaps diagnostic initializer '+label)
   old=output_bytes(original,address,count);new=output_bytes(parsed,address,count)
   if old!=new:raise SystemExit('A complete canonical graph span changed '+label)
   return {'address':address,'complete_bytes':count,'resolved_sha256':hashlib.sha256(new).hexdigest()}
  code={}
  for name,node in binding['actual_final_caller_body_graph'].items():
   checked=span(symbols[name]['value'],node['complete_text_bytes'],name)
   if checked['resolved_sha256']!=node['resolved_text_sha256']:
    raise SystemExit('Canonical full resolved code no longer equals its original binding '+name)
   code[name]={**checked,'original_exact_input_frame':node['exact_frame'],
     'all_original_ordered_RELA_and_frame_instruction_witnesses_reused_unchanged':True}
  data={}
  for name,node in binding['complete_data'].items():
   address=node['linked_address'];count=node['complete_bytes']
   if node['type']==8:
    found=[s for s in parsed.sections if s[1]==8 and s[3]<=address and address+count<=s[3]+s[5]]
    if len(found)!=1 or address%node['alignment']:raise SystemExit('Diagnostic complete BSS capacity/alignment changed '+name)
    data[name]={'address':address,'complete_BSS_capacity':count,'alignment':node['alignment']}
   else:
    checked=span(address,count,name)
    if checked['resolved_sha256']!=node['resolved_sha256']:raise SystemExit('Diagnostic data initializer differs from canonical binding '+name)
    data[name]=checked
  literals=[]
  for node in binding['complete_literals']:
   value=bytes.fromhex(node['complete_literal_hex']);address=node['linked_address']
   checked=span(address,len(value),node['input_section'])
   if output_bytes(parsed,address,len(value))!=value or (address-node['offset'])%node['input_alignment']:
    raise SystemExit('Diagnostic complete literal bytes/alignment changed')
   literals.append({**checked,'original_exact_retained_RELA_anchor':node['retained_exact_linker_RELA_anchor']})
  sdk={}
  for name,node in binding['actual_final_SDK_bodies'].items():
   checked=span(node['linked_address'],node['complete_bytes'],'SDK '+name)
   if checked['resolved_sha256']!=node['resolved_sha256']:raise SystemExit('Diagnostic actual SDK body/padding changed '+name)
   sdk[name]={**checked,'archive_sha256':node['archive_sha256'],'object_sha256':node['object_sha256'],
      'original_exact_input_frame':node['exact_input_frame'],'all_original_ordered_SDK_RELA_and_data_checks_reused':True}
  result[key]={'diagnostic_ELF':str(path),'diagnostic_ELF_sha256':sha(path),'seed':variant['seed_uint32'],
    'initializer_file_offset':start,'only_authorized_initializer_symbol_bytes':size,
    'every_other_ELF_file_byte_identical':True,'all_original_symbols_addresses_sizes_identical':True,
    'complete_caller_row_wrapper_graph':code,'complete_data_capacities_and_initializers':data,
    'complete_original_literals':literals,'complete_actual_SDK_bodies':sdk}
 after={str(path):sha(path) for path in sorted(pins)}
 if before!=after:raise SystemExit('Immutable canonical or diagnostic inputs changed during read-only binding')
 (a.out/'diagnostic-binding.json').write_text(json.dumps({'canonical_binding':str(a.canonical_binding.resolve()),
   'checker_provenance':finish_checker(checker),
   'canonical_binding_sha256':sha(a.canonical_binding),'canonical_ELF_sha256':sha(original_path),
   'diagnostic_patch_report_sha256':sha(report_path),'all_inputs_unchanged':True,'input_sha256':before,
   'both_diagnostic_variants':result,'scope':'Code/data/ABI identity only; no game/image/pressure acceptance or new performance run.'},indent=2)+'\n')
 print('Both diagnostic copies preserve the complete canonical timed row/caller/data/literal/SDK/frame graph.')


if __name__=='__main__':main()
