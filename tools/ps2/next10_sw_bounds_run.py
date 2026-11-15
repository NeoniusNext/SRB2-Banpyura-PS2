"""Stage exact existing SW fixture bytes and pin them before/after PCSX2.

No compilation. Uses only run_pcsx2.py and requires the serialized emulator
slot. The original build and prior run evidence remain untouched.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
from next10_sw_bounds_report import report,negative_controls

ROOT=Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def settings(text):
    keys=('EnableExtraMemory','EnableEECache','EECycleRate','EECycleSkip')
    result={}
    for key in keys:
        values=re.findall(r'^'+key+r'\s*=\s*([^\n\r]+)',text,re.M)
        if len(values)!=1:raise SystemExit('Missing/ambiguous exact emulator setting '+key)
        result[key]=values[0].strip()
    return result


def main():
    import run_pcsx2 as R
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--negative',action='store_true');a=p.parse_args()
    source=a.input.resolve();out=a.out.resolve()
    if not out.is_relative_to(ROOT/'build') or out.exists():
        raise SystemExit('A fresh staged run directory under build/ is required')
    if int(os.environ.get('SRB2_PCSX2_SLOTS','0'))!=1:
        raise SystemExit('Explicit single emulator slot configuration required')
    manifest=json.loads((source/'source-hashes.json').read_text())
    for name in ('functions.c','driver.c'):
        if sha(source/name)!=manifest[name]:raise SystemExit('Frozen source mismatch '+name)
    for name,digest in manifest['variants'].items():
        if sha(source/(name+'.inc'))!=digest:raise SystemExit('Frozen extracted source mismatch '+name)
    for name,digest in manifest['current_header_snapshot'].items():
        if sha(source/'frozen-src'/name)!=digest:raise SystemExit('Frozen header mismatch '+name)
    out.mkdir(parents=True)
    files=['source-hashes.json','functions.o','driver.o','BOUNDS.ELF',
           'ee-functions-build.log','ee-driver-build.log','ee-link.log']
    files+=sorted(path.name for pattern in ('*.c','*.inc') for path in source.glob(pattern))
    files+=['frozen-src/'+name for name in manifest['current_header_snapshot']]
    files=sorted(set(files))
    originals={name:sha(source/name) for name in files}
    for name in files:
        path=out/name;path.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source/name,path)
    before={name:sha(out/name) for name in files}
    if originals!=before:raise SystemExit('Staged bytes differ from compiled fixture')
    executable=Path(R.PCSX2).resolve()
    emulator_binaries=[executable]
    actual=Path(R.PCSX2).parent/'usr/bin/pcsx2-qt'
    if actual.exists() and actual.resolve()!=executable:emulator_binaries.append(actual.resolve())
    binary_hashes={str(path):sha(path) for path in emulator_binaries}
    configs=[Path(R.PCSX2).parent/'usr/bin/inis/PCSX2.ini',executable.parent/'inis/PCSX2.ini']
    configs=list({path.resolve() for path in configs if path.is_file()})
    if len(configs)!=1:raise SystemExit('Expected one selected emulator config')
    config=configs[0];config_before=config.read_text();critical=settings(config_before)
    if critical!={'EnableExtraMemory':'false','EnableEECache':'false','EECycleRate':'0','EECycleSkip':'0'}:
        raise SystemExit('Unexpected standard EE timing configuration')
    (out/'emulator-config-before.ini').write_text(config_before)
    tool=ROOT/'tools/ps2/run_pcsx2.py';tool_before=sha(tool)
    command=[sys.executable,str(tool),'--elf',str(out/'BOUNDS.ELF'),'--log',str(out/'run.log'),
             '--until','BOUNDS FAIL' if a.negative else 'BOUNDS DONE','--timeout','60' if a.negative else '180']
    started=time.time()
    result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True)
    finished=time.time()
    (out/'runner.log').write_text(' '.join(command)+'\n'+result.stdout+result.stderr)
    after={name:sha(out/name) for name in files}
    if before!=after or originals!={name:sha(source/name) for name in files}:
        raise SystemExit('Compiled or executed input bytes changed during run')
    if binary_hashes!={name:sha(Path(name)) for name in binary_hashes} or tool_before!=sha(tool):
        raise SystemExit('Emulator binary or runner changed during run')
    config_after=config.read_text();(out/'emulator-config-after.ini').write_text(config_after)
    if critical!=settings(config_after):raise SystemExit('Critical emulator settings changed during run')
    if result.returncode:raise SystemExit('PCSX2 runner did not reach required outcome')
    log=(out/'run.log').read_text()
    if a.negative:
        failures=re.findall(r'(BOUNDS FAIL[^\n]*)',log)
        if len(failures)!=1 or 'variant=production' not in failures[0] or 'BOUNDS DONE' in log or 'BOUNDS EXACT' in log:
            raise SystemExit('Actual MMI mutation was not uniquely detected before completion')
        validation={'actual_MMI_minimum_changed_to_maximum':True,'mutation_detected':True,'native_failure':failures[0]}
        (out/'negative-result.json').write_text(json.dumps(validation,indent=2)+'\n')
    else:
        validation=report(out);validation['parser_negative_controls']=negative_controls(out)
        (out/'report.json').write_text(json.dumps(validation,indent=2)+'\n')
    execution={'compiled_input_directory':str(source),'executed_ELF':str(out/'BOUNDS.ELF'),
        'inputs_before':before,'inputs_after':after,'all_original_and_staged_inputs_unchanged':True,
        'runner_command':command,'runner_returncode':result.returncode,'run_started_unix':started,'run_finished_unix':finished,
        'runner_tool_sha256':tool_before,'emulator_binaries_before_and_after_sha256':binary_hashes,
        'config_path':str(config),'config_before_sha256':sha(out/'emulator-config-before.ini'),
        'config_after_sha256':sha(out/'emulator-config-after.ini'),
        'complete_config_bytes_unchanged':config_before==config_after,'critical_settings_before_and_after':critical,
        'run_log_sha256':sha(out/'run.log'),'runner_log_sha256':sha(out/'runner.log'),
        'expected_outcome':'actual MMI mutation detected' if a.negative else 'complete exactness and 196 timing rows',
        'validation_passed':True}
    (out/'run-manifest.json').write_text(json.dumps(execution,indent=2)+'\n')
    validation['staged_run_manifest_sha256']=sha(out/'run-manifest.json')
    (out/('negative-result.json' if a.negative else 'report.json')).write_text(json.dumps(validation,indent=2)+'\n')
    print('Staged inputs and exact execution outcome pinned: '+str(out))


if __name__=='__main__':main()
