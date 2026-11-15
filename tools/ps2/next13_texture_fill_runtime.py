#!/usr/bin/env python3
"""Strict next12 runtime envelope around the archived official native wrapper."""
import argparse
import sys
from pathlib import Path

from next12_runtime import run_verified
from next13_texture_fill_variant import ROOT


def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--work',type=Path,required=True);p.add_argument('--timeout',type=int,default=900)
 p.add_argument('--host-report',type=Path);p.add_argument('--host-native-report',type=Path);p.add_argument('--game-binding',type=Path)
 a=p.parse_args();work=a.work.resolve()
 python=Path('/workspace/.onboarding/assets-venv/bin/python3')
 if Path(sys.executable)!=python:raise SystemExit('Exact frozen absolute Python interpreter required')
 inputs={p for p in work.rglob('*') if p.is_file()}
 inputs.update([Path(__file__).resolve(),ROOT/'tools/ps2/next13_texture_fill_run.py',ROOT/'tools/ps2/next13_texture_fill_report.py',Path('/workspace/.onboarding/environment.sh')])
 command=[str(python),'-B',str(ROOT/'tools/ps2/next13_texture_fill_run.py'),'--out',str(work),'--timeout',str(a.timeout)]
 for field in ('host_report','host_native_report','game_binding'):
  if getattr(a,field):command += ['--'+field.replace('_','-'),str(getattr(a,field).resolve())]
 result=run_verified(command,work/'runtime',inputs)
 print(result.stdout+result.stderr)
 raise SystemExit(result.returncode)


if __name__=='__main__':main()
