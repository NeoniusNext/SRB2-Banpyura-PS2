#!/usr/bin/env python3
"""Run the bound row graph via run_pcsx2 with immutable evidence and full settings."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from next13_texture_fill_variant import ROOT
from next11_gs_run import critical_settings


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);p.add_argument('--timeout',type=int,default=900)
 p.add_argument('--host-report',type=Path);p.add_argument('--host-native-report',type=Path);p.add_argument('--game-binding',type=Path)
 a=p.parse_args();work=a.out.resolve()
 if (work/'run-pre.json').exists():raise SystemExit('Fresh native run evidence required')
 binding=json.loads((work/'native-binding.json').read_text());build=json.loads((work/'build-report.json').read_text())
 elf=work/'FILL_NEXT13.ELF'
 if binding['fixture_sha256']!=sha(elf) or build['elf_sha256']!=sha(elf):raise SystemExit('Actual bound native ELF changed')
 inputs={p for p in work.rglob('*') if p.is_file()}
 for tag,v in binding['variants'].items():
  base=Path(v['build'])
  if sha(base/'SRB2.ELF')!=v['ELF_sha256'] or sha(base/'obj/engine_lto.o')!=v['engine_sha256']:raise SystemExit('Actual bound game changed')
  inputs.update([base/'SRB2.ELF',base/'obj/engine_lto.o',base/'SRB2.map',base/'build-report.json'])
 freeze=work.parent/'candidate-source-freeze.json';frozen=json.loads(freeze.read_text());source=Path(frozen['source_root'])
 if any(sha(source/name)!=digest for name,digest in frozen['source_sha256'].items()):raise SystemExit('Complete602 candidate source changed')
 inputs.add(freeze);inputs.update(source/name for name in frozen['source_sha256'])
 for name in ('next13_texture_fill_run.py','next13_texture_fill_native.py','next13_texture_fill_native.c','next13_texture_fill_hosttest.c','next13_texture_fill_report.py','next13_texture_fill_sdkbind.py','next13_texture_fill_codecheck.py','run_pcsx2.py','next11_gs_run.py'):
  inputs.add(ROOT/'tools/ps2'/name)
 inputs.update([a.game_binding or work.parent/'code-binding-v3/code-binding.json',
                a.host_report or work.parent/'host-v4/host-report.json',
                a.host_native_report or work.parent/'host-native-matrix-v2/report.json'])
 inputs.add(Path('/workspace/.onboarding/emulator-env.sh'))
 before={str(p):sha(p) for p in sorted(inputs)}
 archive=work/'pinned-run-inputs';archived={}
 for name,digest in before.items():
  path=Path(name);rel=path.relative_to(ROOT) if path.is_relative_to(ROOT) else Path('external')/name.lstrip('/')
  dest=archive/rel;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,dest);archived[str(dest.relative_to(work))]=digest
 (work/'pinned-run-inputs.json').write_text(json.dumps(archived,indent=2)+'\n')
 profile=Path(os.environ.get('SRB2_PCSX2_ROOT','/workspace/.onboarding/pcsx2'))
 configs=sorted(profile.glob('slot*/**/*.ini'));runtimes=sorted(set(p for p in profile.glob('slot*/usr/bin/pcsx2-qt') if p.is_file()))
 critical={str(p):critical_settings(p) for p in configs};runtime={str(p):sha(p) for p in runtimes}
 if not any(p.name=='PCSX2.ini' for p in configs):raise SystemExit('Standard complete emulator profile required')
 for path in configs:
  dest=work/'run-config-before'/path.relative_to(profile);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,dest)
 command=[sys.executable,'-B',str(ROOT/'tools/ps2/run_pcsx2.py'),'--elf',str(elf),'--log',str(work/'run.log'),'--until','FILL NEXT13 DONE','--timeout',str(a.timeout)]
 pre={'command':command,'input_sha256':before,'critical_settings':critical,'emulator_binary_sha256':runtime,
      'config_sha256':{str(p):sha(p) for p in configs},'emulator_environment':{k:v for k,v in os.environ.items() if k.startswith(('PCSX2','SRB2_PCSX2'))}}
 (work/'run-pre.json').write_text(json.dumps(pre,indent=2)+'\n')
 result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True);(work/'orchestration.log').write_text(result.stdout+result.stderr)
 after={str(p):sha(p) for p in sorted(inputs)};critical_after={str(p):critical_settings(p) for p in configs};runtime_after={str(p):sha(p) for p in runtimes}
 for path in configs:
  dest=work/'run-config-after'/path.relative_to(profile);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,dest)
 post={'rc':result.returncode,'input_sha256':after,'all_frozen_inputs_unchanged':before==after,
    'log_sha256':sha(work/'run.log') if (work/'run.log').exists() else None,'critical_settings':critical_after,
    'critical_settings_unchanged':critical==critical_after,'emulator_binary_sha256':runtime_after,'emulator_binary_unchanged':runtime==runtime_after}
 (work/'run-post.json').write_text(json.dumps(post,indent=2)+'\n')
 print((result.stdout+result.stderr)[-2500:])
 if before!=after or critical!=critical_after or runtime!=runtime_after:raise SystemExit('Pinned actual native inputs/emulator configuration changed')
 raise SystemExit(result.returncode)


if __name__=='__main__':main()
