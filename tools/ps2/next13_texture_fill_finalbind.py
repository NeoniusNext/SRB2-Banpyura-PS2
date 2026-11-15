#!/usr/bin/env python3
"""Bind a final combined game to the already executed complete row graph."""
import argparse
import hashlib
import json
from pathlib import Path

from audio_storage_next7_finalbind import Object
from next8_gs_codecheck import symbols
from next7_render_fog_codecheck import Elf32
from next10_sw_bounds_codecheck import frame
from next12_pb_scratch_codecheck import binder,sdk_memory
from next13_texture_fill_provenance import freeze_checker,finish_checker
import next10_gs_low as graph


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--native',type=Path,required=True);p.add_argument('--final',type=Path,required=True);p.add_argument('--final-relas',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
 if a.out.exists():raise SystemExit('Fresh final row-graph binding required')
 a.out.mkdir(parents=True);native=a.native.resolve();final=a.final.resolve()
 checker=freeze_checker(a.out,Path(__file__))
 tracked={native/name for name in ('cur-graph.json','FILL_NEXT13.ELF','build-report.json','native-binding.json',
   'sdk-binding/sdk-binding.json','summary/benchmark-summary.json','run.log','run-pre.json','run-post.json',
   'runtime/run-pre.json','runtime/run-post.json')}
 tracked.update(final/name for name in ('SRB2.ELF','obj/engine_lto.o','SRB2.map','build-report.json'))
 tracked.update(a.final_relas.resolve()/name for name in ('LINKER-RELAS.ELF','linker-rela-report.json'))
 original_metadata=json.loads((native/'cur-graph.json').read_text());tracked.add(Path(original_metadata['engine']))
 timed_metadata=json.loads((native/'sdk-binding/sdk-binding.json').read_text())
 tracked.update(Path(row['archive']) for row in timed_metadata['complete_fixture_SDK_memory_bodies'].values())
 input_pins={str(path.resolve()):sha(path) for path in sorted(tracked)}
 summary_path=native/'summary/benchmark-summary.json';summary=json.loads(summary_path.read_text())
 post=json.loads((native/'run-post.json').read_text());outer=json.loads((native/'runtime/run-post.json').read_text())
 if not summary['all_rows_improve_or_tie'] or post['rc'] or outer['rc'] or not post['all_frozen_inputs_unchanged'] or not outer['all_non_UI_settings_unchanged'] or not outer['emulator_binary_unchanged']:
  raise SystemExit('Only the complete accepted native matrix with preserved runtime inputs may bind final shipping code')
 if summary['fixture_sha256']!=sha(native/'FILL_NEXT13.ELF') or summary['run_log_sha256']!=sha(native/'run.log'):
  raise SystemExit('Executed native matrix no longer pins this exact fixture/log')
 original=json.loads((native/'cur-graph.json').read_text());reference=Object(Path(original['engine']))
 graph.CODE=('fill_idx','fill_ia88','__wrap_memcpy');graph.SPECIAL={}
 nodes,_=graph.bundle(a.out,final/'obj/engine_lto.o','cur')
 current=Object(final/'obj/engine_lto.o');game=Object(final/'SRB2.ELF');gs=symbols(game)
 fixture=Object(native/'FILL_NEXT13.ELF');fs=symbols(fixture);mapped=graph.map_addresses(final/'SRB2.map')
 if sha(native/'FILL_NEXT13.ELF')!=json.loads((native/'build-report.json').read_text())['elf_sha256']:raise SystemExit('Executed native ELF changed')
 if sha(final/'SRB2.ELF')!=json.loads((final/'build-report.json').read_text())['elf_sha256']:raise SystemExit('Final actual ELF/report differs')
 if set(nodes)!=set(original['nodes']):raise SystemExit('Final actual row graph node set changed')
 final_gp=json.loads((a.out/'cur-graph.json').read_text())['original_GP0'];checked={}
 for sec,node in nodes.items():
  before=original['nodes'][sec]
  def original_records(n):return [{key:r[key] for key in ('offset','type','addend','original_target')} for r in n['ordered_relocations']]
  if reference.content(reference.sections[sec])!=current.content(current.sections[sec]) or before['body_bytes']!=node['body_bytes'] or original_records(before)!=original_records(node):raise SystemExit('Final actual input row/wrapper bytes or complete ordered RELA metadata differ '+sec)
  if frame(Elf32(Path(original['engine'])),sec[6:])!=frame(Elf32(final/'obj/engine_lto.o'),sec[6:]):raise SystemExit('Final actual input row/wrapper frame differs '+sec)
  data=current.content(current.sections[sec]);records=node['ordered_relocations']
  expected=graph.relocate(data,records,lambda r:fs[r['target']]['address']+r['addend'],fs['_gp']['address'],final_gp)
  if expected!=graph.linked_span(fixture,fs,node['label'],len(data)):raise SystemExit('Final graph no longer matches executed complete native bytes '+sec)
  actual=graph.relocate(data,records,lambda r:graph.resolve_original(current,game,gs,mapped,r,{}),gs['_gp']['address'],final_gp)
  if actual!=graph.linked_span(game,gs,sec[6:],len(data)):raise SystemExit('Final complete resolved row/wrapper bytes/padding differ '+sec)
  checked[sec]={'complete_input_bytes':len(data),'body_bytes':node['body_bytes'],'ordered_original_RELA_records':records,
    'exact_frame':frame(Elf32(final/'obj/engine_lto.o'),sec[6:]),'executed_fixture_and_actual_final_resolved_bytes_checked':True}
 value=binder(final,a.final_relas.resolve());callers=set()
 for sec in current.sections:
  if not sec.startswith('.text.'):continue
  for r in current.relocations.get(sec,[]):
   if r['target']['name'] in ('fill_idx','fill_ia88','.text.fill_idx','.text.fill_ia88') or r['target']['section'] in ('.text.fill_idx','.text.fill_ia88'):
    callers.add(sec[6:])
 if not callers:raise SystemExit('Actual row selector/caller references were not discovered')
 bodies={name:value.text(name) for name in sorted(callers|{'upload_bands','fill_idx','fill_ia88','__wrap_memcpy','__wrap_memset'})}
 sdkout=a.out/'actual-sdk';sdkout.mkdir();sdk=sdk_memory(value,final,sdkout)
 timed_sdk=json.loads((native/'sdk-binding/sdk-binding.json').read_text())['complete_fixture_SDK_memory_bodies']
 for name,row in sdk.items():
  for key in ('archive_sha256','object_sha256','complete_bytes','exact_symbol_bytes','exact_input_frame'):
   if row[key]!=timed_sdk[name][key]:raise SystemExit('Final actual SDK input changed '+name+' '+key)
 result={'native':str(native),'native_ELF_sha256':sha(native/'FILL_NEXT13.ELF'),'final':str(final),
   'accepted_full384_row_native_summary_sha256':sha(summary_path),
   'final_ELF_sha256':sha(final/'SRB2.ELF'),'final_engine_sha256':sha(final/'obj/engine_lto.o'),
   'all_executed_row_wrapper_input_bytes_ordered_metadata_frames_identical_final':True,
   'rows_and_wrapper':checked,'actual_final_caller_body_graph':bodies,'complete_data':value.data,
   'complete_literals':list(value.literals.values()),'all_exact_literal_uses':value.literal_uses,'actual_final_SDK_bodies':sdk,
   'capacity_symbols':{name:value.symbols[name] for name in ('_end','_gp','_stack','_stack_size') if name in value.symbols}}
 if input_pins!={str(path.resolve()):sha(path) for path in sorted(tracked)}:raise SystemExit('Canonical/native binding input changed during verification')
 result.update(checker_provenance=finish_checker(checker),all_binding_input_bytes_unchanged=True,input_sha256=input_pins,
   actual_row_selector_callers=sorted(callers))
 (a.out/'final-binding.json').write_text(json.dumps(result,indent=2)+'\n')
 print('Final actual rows/wrapper/SDK/caller/data/literals/frames bind to executed complete native graph.')


if __name__=='__main__':main()
