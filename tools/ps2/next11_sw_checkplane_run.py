"""Stage/pin exact native bytes and all non-UI emulator settings before/after."""
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

from next10_gs_run import critical_settings
from next11_sw_checkplane_report import report,negative_controls

ROOT=Path(__file__).resolve().parents[2]


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    import run_pcsx2 as R
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input',type=Path,required=True);parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--negative',action='store_true');parser.add_argument('--timeout',type=int,default=600)
    args=parser.parse_args();source=args.input.resolve();out=args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT/'build'):
        raise SystemExit('Fresh staged directory under build/ required')
    if int(os.environ.get('SRB2_PCSX2_SLOTS','0'))!=1:
        raise SystemExit('Explicit serialized one-emulator configuration required')
    manifest=json.loads((source/'source-hashes.json').read_text())
    for name,digest in manifest['files'].items():
        if sha(source/name)!=digest:raise SystemExit('Compiled source changed '+name)
    for name,digest in manifest['frozen_headers'].items():
        if sha(source/'frozen-src'/name)!=digest:raise SystemExit('Frozen header changed '+name)
    build=json.loads((source/'ee-build-report.json').read_text())
    if sha(source/'CHECKPLANE.ELF')!=build['ELF_sha256']:
        raise SystemExit('Compiled ELF/build pin mismatch')
    for name,digest in build['objects'].items():
        if sha(Path(name))!=digest:raise SystemExit('Compiled native object changed '+name)
    files=sorted(path for path in source.rglob('*') if path.is_file() and path.suffix in
                 ('.h','.c','.inc','.S','.o','.ELF','.map','.json','.log'))
    before={str(path.relative_to(source)):sha(path) for path in files}
    out.mkdir(parents=True)
    for path in files:
        target=out/path.relative_to(source);target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,target)
    if before!={name:sha(out/name) for name in before}:
        raise SystemExit('Staged source/object/ELF bytes differ')
    tools=[Path(__file__).resolve(),ROOT/'tools/ps2/next11_sw_checkplane.py',
           ROOT/'tools/ps2/next11_sw_checkplane.c',ROOT/'tools/ps2/next11_sw_checkplane_report.py',
           ROOT/'tools/ps2/next10_gs_run.py',ROOT/'tools/ps2/run_pcsx2.py',Path('/workspace/.onboarding/emulator-env.sh')]
    tool_hashes={str(path):sha(path) for path in tools}
    for path in tools:
        target=out/'pinned-tools'/path.name;target.parent.mkdir(exist_ok=True);shutil.copyfile(path,target)
    profile=Path(os.environ.get('SRB2_PCSX2_ROOT','/workspace/.onboarding/pcsx2'))
    configs=sorted(profile.glob('slot*/**/*.ini'))
    if not any(path.name=='PCSX2.ini' for path in configs):raise SystemExit('Complete standard profile missing')
    critical_before={str(path):critical_settings(path) for path in configs}
    binaries=set(path for path in profile.glob('slot*/usr/bin/pcsx2-qt') if path.is_file())
    binaries.add(Path(R.PCSX2).resolve())
    binaries={str(path):sha(path) for path in sorted(binaries)}
    for path in configs:
        target=out/'run-config-before'/path.relative_to(profile);target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,target)
    command=[sys.executable,'-B',str(ROOT/'tools/ps2/run_pcsx2.py'),'--elf',str(out/'CHECKPLANE.ELF'),
             '--log',str(out/'run.log'),'--until','CHECKPLANE FAIL' if args.negative else 'CHECKPLANE DONE',
             '--timeout',str(args.timeout)]
    started=time.time();result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True);finished=time.time()
    (out/'runner.log').write_text(' '.join(command)+'\n'+result.stdout+result.stderr)
    after={name:sha(out/name) for name in before}
    if before!=after or before!={str(path.relative_to(source)):sha(path) for path in files}:
        raise SystemExit('Original or staged compiled bytes changed during execution')
    critical_after={str(path):critical_settings(path) for path in configs if path.exists()}
    for path in configs:
        if path.exists():
            target=out/'run-config-after'/path.relative_to(profile);target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,target)
    if critical_before!=critical_after or binaries!={name:sha(Path(name)) for name in binaries}:
        raise SystemExit('Emulator critical settings or binaries changed')
    if tool_hashes!={str(path):sha(path) for path in tools}:
        raise SystemExit('Executed runner/proof tool changed')
    if result.returncode:raise SystemExit('PCSX2 runner did not reach required outcome')
    if args.negative:
        text=(out/'run.log').read_text();failures=re.findall(r'CHECKPLANE FAIL[^\r\n]*',text)
        if len(failures)!=1 or 'variant=production' not in failures[0] or 'CHECKPLANE DONE' in text or 'CHECKPLANE EXACT' in text:
            raise SystemExit('Actual native source mutation not uniquely detected before completion')
        validation={'actual_bottom_sample_mutation_detected':True,'native_failure':failures[0]}
    else:
        validation=report(out);validation['parser_negative_controls']=negative_controls(out)
    execution={'compiled_input_directory':str(source),'executed_ELF':str(out/'CHECKPLANE.ELF'),
               'inputs_before':before,'inputs_after':after,'all_original_and_staged_inputs_unchanged':True,
               'runner_command':command,'runner_returncode':result.returncode,'run_started_unix':started,'run_finished_unix':finished,
               'proof_tools_before_and_after_sha256':tool_hashes,'emulator_binaries_before_and_after_sha256':binaries,
               'critical_settings_before':critical_before,'critical_settings_after':critical_after,
               'every_non_UI_setting_unchanged':True,'full_config_before_and_after_archived':True,
               'run_log_sha256':sha(out/'run.log'),'runner_log_sha256':sha(out/'runner.log'),
               'expected_outcome':'actual source mutation rejected' if args.negative else 'complete exactness and 392 paired cost rows',
               'validation_passed':True}
    (out/'run-manifest.json').write_text(json.dumps(execution,indent=2)+'\n')
    validation['staged_run_manifest_sha256']=sha(out/'run-manifest.json')
    (out/('negative-result.json' if args.negative else 'report.json')).write_text(json.dumps(validation,indent=2)+'\n')
    print(json.dumps(validation.get('summary',validation),indent=2),flush=True)


if __name__=='__main__':main()
