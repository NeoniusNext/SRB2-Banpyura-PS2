#!/usr/bin/env python3
"""Reconstruct actual SDK memory archive bodies in both games and the native kernel."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil

from next13_texture_fill_variant import ROOT
from next12_pb_scratch_codecheck import binder,sdk_memory
from next7_render_fog_codecheck import Elf32
from next8_sw_codecheck import linked_symbols


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--work',type=Path,required=True);p.add_argument('--game-binding',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
 a=p.parse_args();work=a.work.resolve()
 if a.out.exists():raise SystemExit('Fresh complete SDK binding required')
 a.out.mkdir(parents=True)
 bound=json.loads(a.game_binding.read_text());native=json.loads((work/'native-binding.json').read_text())
 original=bound['variants']['base'];build=Path(original['build'])
 value=binder(build,ROOT/'build/nov15-next12/final/pb-binding/final-linker-relas')
 value=copy.copy(value);value.elf=Elf32(work/'FILL_NEXT13.ELF');value.symbols=linked_symbols(value.elf)
 value.wrappers=set();value.data={};value.literals={};value.linker_relas={};value.linker_rela_evidence=None;value.literal_uses=[]
 sdkmap=a.out/'fixture-map';sdkmap.mkdir();shutil.copyfile(work/'FILL_NEXT13.map',sdkmap/'SRB2.map')
 sdkout=a.out/'actual-sdk';sdkout.mkdir()
 fixture=sdk_memory(value,sdkmap,sdkout)
 for tag,v in bound['variants'].items():
  if native['variants'][tag]['ELF_sha256']!=v['ELF_sha256'] or native['variants'][tag]['engine_sha256']!=v['engine_sha256']:raise SystemExit('Different actual game bound by kernel and caller graph')
  for name,row in v['actual_SDK_memory_bodies'].items():
   current=fixture[name]
   for field in ('archive_sha256','object_sha256','complete_bytes','exact_symbol_bytes','exact_input_frame'):
    if row[field]!=current[field]:raise SystemExit('Actual full SDK input mismatch '+name+' '+field)
   def original_records(entry):return [{k:r[k] for k in ('offset','kind','exact_symbol','explicit_addend')} for r in entry['ordered_RELA_records']]
   if original_records(row)!=original_records(current):raise SystemExit('Original ordered SDK RELA semantics differ '+name)
 result={'fixture_sha256':sha(work/'FILL_NEXT13.ELF'),'game_binding_sha256':sha(a.game_binding),
   'native_binding_sha256':sha(work/'native-binding.json'),'complete_fixture_SDK_memory_bodies':fixture,
   'all_original_archive_object_bytes_frames_and_ordered_RELA_equal_both_actual_games':True,
   'all_fixture_code_padding_and_referenced_data_fully_resolved_independently':True,
   'scope':'Only memcpy is reachable by a timed row; memset/memmove complete bodies are retained/used outside timing.'}
 (a.out/'sdk-binding.json').write_text(json.dumps(result,indent=2)+'\n')
 print('Complete actual SDK archive bodies, ordered RELAs, frames and resolved kernel bytes bind.')


if __name__=='__main__':main()
