#!/usr/bin/env python3
"""Bit-exact differential and CPU benchmark of int64-to-float conversion paths."""
import argparse,hashlib,json,os,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);p.add_argument('--sanitize',action='store_true');p.add_argument('--ee-only',action='store_true');a=p.parse_args()
 out=a.out.resolve();out.mkdir(parents=True,exist_ok=True);functions=Path(__file__).with_name('tile_i64f_functions.c');fixture=Path(__file__).with_suffix('.c');commands=[]
 proof={'functions':str(functions),'functions_sha256':sha(functions),'fixture':str(fixture),'fixture_sha256':sha(fixture),'scope':'Separate non-LTO current compiler int64 cast, direct int32 diagnostic, exact signed32 bit constructor and exactly representable-only cast. No GS or whole-game performance claim.'};(out/'source-proof.json').write_text(json.dumps(proof,indent=2)+'\n')
 if a.ee_only:
  os.environ['SRB2_PS2_OUT']=str(out/'sdkcfg');sys.path.insert(0,str(ROOT/'tools/ps2'));import build as B
  B.gen_config();flags=[f for f in B.CFLAGS if f not in ('-flto','-MMD','-MP')]+['-fno-lto','-DTILE_I64F_EE','-Werror'];objects=[]
  for source in (functions,fixture):
   obj=out/(source.stem+'.o');cmd=[str(B.CC),*flags,*B.INCS,'-c',str(source),'-o',str(obj)];commands.append(cmd);r=subprocess.run(cmd,capture_output=True,text=True,env=B.ENV);(out/(source.stem+'-build.log')).write_text(r.stdout+r.stderr)
   if r.returncode or r.stdout or r.stderr:raise RuntimeError(r.stdout+r.stderr)
   objects.append(obj)
  elf=out/'TILE_I64F.ELF';cmd=[str(B.CC),*B.LDFLAGS,*map(str,objects),'-o',str(elf),'-ldebug','-lpatches','-lm'];commands.append(cmd);r=subprocess.run(cmd,capture_output=True,text=True,env=B.ENV);(out/'link.log').write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(r.stdout+r.stderr)
  (out/'ee-build-report.json').write_text(json.dumps({'elf':str(elf),'elf_sha256':sha(elf),'source_proof':proof,'commands':commands},indent=2)+'\n');print(elf);return 0
 flags=['-std=gnu17','-O2','-g','-Wall','-Wextra','-Werror','-fno-lto']
 if a.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
 cmd=['cc',*flags,str(functions),str(fixture),'-o',str(out/'test')];r=subprocess.run(cmd,capture_output=True,text=True);(out/'build.log').write_text(r.stdout+r.stderr)
 if r.returncode:raise RuntimeError(r.stdout+r.stderr)
 r=subprocess.run([str(out/'test')],capture_output=True,text=True);(out/'test.log').write_text(r.stdout+r.stderr);passed=r.returncode==0 and 'failures=0' in r.stdout and not r.stderr
 (out/'results.json').write_text(json.dumps({'passed':passed,'returncode':r.returncode,'sanitize':a.sanitize,'source_proof':proof,'output':r.stdout,'stderr':r.stderr},indent=2)+'\n');print(r.stdout+r.stderr);return int(not passed)
if __name__=='__main__':raise SystemExit(main())
