#!/usr/bin/env python3
"""Compare frozen six-node V2 proof with its later provenance-pinned replay.

This later read-only corroboration never claims to be a pre-execution pin.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path

from next13_texture_fill_provenance import freeze_checker,finish_checker


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--work',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
 if a.out.exists():raise SystemExit('Fresh proof continuity output required')
 a.out.mkdir(parents=True);checker=freeze_checker(a.out,Path(__file__));work=a.work.resolve()
 paths={
  'old_canonical':work/'final-canonical-binding-v2/final-binding.json',
  'new_canonical':work/'final-canonical-binding-v2-pinned/final-binding.json',
  'old_diagnostic':work/'final-diagnostic-binding-v2/diagnostic-binding.json',
  'new_diagnostic':work/'final-diagnostic-binding-v2-pinned/diagnostic-binding.json'}
 before={name:sha(path) for name,path in paths.items()}
 reports={name:json.loads(path.read_text()) for name,path in paths.items()}
 old=reports['old_canonical'];new=reports['new_canonical'];expected=copy.deepcopy(old);aliases=[]
 allowed={'checker_provenance','all_binding_input_bytes_unchanged','input_sha256','actual_row_selector_callers'}
 if set(new)-set(old)!=allowed or set(old)-set(new):raise SystemExit('Unexpected canonical report schema change')
 for name,node in old['actual_final_SDK_bodies'].items():
  later=new['actual_final_SDK_bodies'][name];first=Path(node['extracted_object']);second=Path(later['extracted_object'])
  if sha(first)!=node['object_sha256'] or sha(second)!=later['object_sha256'] or sha(first)!=sha(second):raise SystemExit('SDK output path alias lacks exact object-byte identity')
  expected['actual_final_SDK_bodies'][name]['extracted_object']=later['extracted_object']
  aliases.append({'SDK_member':name,'old_extraction':str(first),'new_extraction':str(second),'exact_object_sha256':sha(first)})
 if expected!={k:v for k,v in new.items() if k in old}:raise SystemExit('A canonical binding semantic/core field changed')
 if set(old['actual_final_caller_body_graph'])!={'__wrap_memcpy','__wrap_memset','fill_idx','fill_ia88','tex_upload.lto_priv.0','upload_bands'}:
  raise SystemExit('Earlier frozen V2 lacks the complete expected six-node selector closure')
 if new['actual_row_selector_callers']!=['tex_upload.lto_priv.0']:raise SystemExit('Current selector inventory differs')
 old_diag=reports['old_diagnostic'];new_diag=reports['new_diagnostic'];expected_diag=copy.deepcopy(old_diag)
 if set(new_diag)-set(old_diag)!={'checker_provenance'} or set(old_diag)-set(new_diag):raise SystemExit('Unexpected diagnostic report schema change')
 expected_diag['canonical_binding']=new_diag['canonical_binding']
 expected_diag['canonical_binding_sha256']=new_diag['canonical_binding_sha256']
 if old_diag['canonical_binding_sha256']!=before['old_canonical'] or new_diag['canonical_binding_sha256']!=before['new_canonical']:
  raise SystemExit('Diagnostic report does not pin its actual corresponding canonical report')
 old_key=str(paths['old_canonical'].resolve());new_key=str(paths['new_canonical'].resolve())
 if expected_diag['input_sha256'].pop(old_key,None)!=before['old_canonical']:raise SystemExit('Old diagnostic input pins differ')
 expected_diag['input_sha256'][new_key]=before['new_canonical']
 if expected_diag!={k:v for k,v in new_diag.items() if k in old_diag}:raise SystemExit('Diagnostic code/data/SDK/frame/core identity changed')
 after={name:sha(path) for name,path in paths.items()}
 if before!=after:raise SystemExit('Frozen proof reports changed during comparison')
 result={'scope':'Later read-only corroboration only. New provenance was not a pre-execution input to already frozen pressure runs.',
  'report_paths':{name:str(path) for name,path in paths.items()},'report_sha256':before,
  'earlier_V2_already_has_complete_six_node_selector_closure':True,
  'all_canonical_core_binding_fields_equal_after_only_exact_verified_SDK_extraction_path_aliases':True,
  'exact_verified_output_path_aliases':aliases,
  'both_diagnostic_graphs_equal_after_only_corresponding_canonical_report_reference_update':True,
  'no_canonical_or_diagnostic_ELF_body_data_RELA_frame_or_validation_scope_change':True,
  'canonical_ELF_sha256':old['final_ELF_sha256'],
  'diagnostic_ELF_sha256':{key:v['diagnostic_ELF_sha256'] for key,v in old_diag['both_diagnostic_variants'].items()},
  'all_frozen_reports_unchanged':True,'checker_provenance':finish_checker(checker)}
 (a.out/'continuity.json').write_text(json.dumps(result,indent=2)+'\n')
 print('Frozen complete V2 and later pinned replay have identical binding core; only verified output paths and added provenance differ.')


if __name__=='__main__':main()
