#!/usr/bin/env python3
"""Read-only dependency, linker-input and actual-object inventory for all-up probe.

This records exposure and graph scope. It does not admit any combined shipping
result or infer a fit by adding component deltas. No compiler/emulator runs.
"""
import argparse
import json
from pathlib import Path
import re
import shlex
import shutil

from audio_storage_next7_finalbind import Object
from next12_pb_scratch_finalbind import object_delta,sha
from next12_pb_scratch_objectcheck import pdr
from next13_combined_context import ROOT,WORK,UNITS


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--context',type=Path,default=WORK/'combined-v1')
    p.add_argument('--out',type=Path,default=WORK/'combined-inventory-v1')
    a=p.parse_args();context,out=a.context.resolve(),a.out.resolve()
    if out.exists() or not out.is_relative_to(WORK):raise RuntimeError('Fresh isolated pass13 inventory required')
    freeze=json.loads((context/'input-freeze.json').read_text());report=json.loads((context/'context-report.json').read_text())
    if sha(context/'input-freeze.json')!=report['input_freeze_sha256'] or sha(context/'SRB2.ELF')!=report['ELF_sha256']:
        raise RuntimeError('Frozen all-up input/output pin differs')
    expected={str((context/'snapshot'/name).resolve()):digest for name,digest in freeze['all602_combined_source_sha256'].items()}
    expected.update({str(Path(row['original']).resolve()):row['sha256'] for row in freeze['external_inputs'].values()})
    for row in freeze['generated_input_archive']['details']['entries']:
        expected[str(Path(row['archive_path']).resolve())]=row['archive_sha256']
    dependencies={}
    for unit in UNITS:
        dep=context/(Path(unit).stem+'.d');text=dep.read_text().replace('\\\n',' ')
        target,body=text.split(':',1);paths=sorted(set(Path(x).resolve() for x in shlex.split(body)))
        rows=[]
        for path in paths:
            digest=sha(path)
            if expected.get(str(path))!=digest:raise RuntimeError('Actual dependency was absent/different in before-target freeze: '+str(path))
            rows.append({'actual_path':str(path),'sha256':digest,'present_in_before_target_freeze':True})
        dependencies[unit]={'dependency_file_sha256':sha(dep),'actual_object_target':target.strip(),
            'actual_dependency_count':len(rows),'complete_dependency_rows':rows}
    actual_link_inputs={}
    for name in sorted(set(re.findall(r'(/[^\s()]+\.(?:a|o))',(context/'SRB2.map').read_text()))):
        path=Path(name)
        if not path.is_file() or name.endswith('/engine_lto.o'):continue
        row=freeze['link_inputs_and_compiler'].get(name)
        if row is None or sha(path)!=row['sha256']:raise RuntimeError('Actual selected SDK link input was not frozen before target run: '+name)
        actual_link_inputs[name]={'sha256':sha(path),'before_target_snapshot':row['snapshot']}
    for path,digest in report['complete_actual_IR_objects'].items():
        if sha(Path(path))!=digest:raise RuntimeError('Actual complete combined IR input differs')
    if len(report['complete_actual_IR_objects'])!=205 or not report['all_other201_complete_IR_inputs_identical']:
        raise RuntimeError('Complete four-replacement actual205 graph required')
    baseline=Path(report['baseline_origin']['build']);objects={'baseline':Object(baseline/'obj/engine_lto.o'),
        'candidate':Object(context/'obj/engine_lto.o')};delta=object_delta(objects['baseline'],objects['candidate'])
    instruction_changes=[];binding_only=[];other_changes=[]
    for name in delta['complete_changed_allocated_sections']:
        a_obj,b_obj=objects['baseline'],objects['candidate']
        if name in a_obj.sections and name in b_obj.sections and a_obj.signature(name)==b_obj.signature(name):binding_only.append(name)
        elif name in delta['changed_code_sections_or_bindings']:instruction_changes.append(name)
        else:other_changes.append(name)
    out.mkdir(parents=True);shutil.copyfile(Path(__file__),out/Path(__file__).name)
    result={'status':'COMPLETE read-only actual all-up dependency/object exposure inventory; shipping gates remain pending',
        'context_report_sha256':sha(context/'context-report.json'),'ELF_sha256':report['ELF_sha256'],
        'actual_changed_unit_dependencies':dependencies,'all_actual_changed_unit_dependencies_were_frozen_before_target_run':True,
        'actual_selected_external_link_inputs':actual_link_inputs,'all_actual_link_inputs_were_frozen_before_target_run':True,
        'changed_allocated_sections':delta['complete_changed_allocated_sections'],
        'changed_code_sections_or_bindings':delta['changed_code_sections_or_bindings'],
        'actual_instruction_body_changes_or_dispositions':instruction_changes,'binding_only_changes':binding_only,
        'other_actual_allocated_changes':other_changes,'direct_actual_callers':delta['direct_actual_callers'],
        'actual_static_footprint':json.loads((context/'static-footprint.json').read_text()),
        'checker_sha256':sha(Path(__file__)),'no_compiler_or_emulator_invoked':True,'shipping_admission':False}
    (out/'report.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'status':result['status'],'report':str(out/'report.json'),
        'dependency_counts':{unit:row['actual_dependency_count'] for unit,row in dependencies.items()},
        'actual_selected_external_link_inputs':len(actual_link_inputs),
        'changed_allocated_sections':len(delta['complete_changed_allocated_sections']),
        'changed_instruction_bodies_or_dispositions':len(instruction_changes),'binding_only_changes':len(binding_only)},indent=2))


if __name__=='__main__':main()
