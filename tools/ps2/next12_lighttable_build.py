"""Full default Hardware builds from frozen602 source and only light-table edits.

Uses one granted EE worker, jobs1. No emulator. Reference/candidate source and
output path lengths match; both use the exact official baseline config bytes.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
ALLOWED={'src/hardware/hw_cache.c','src/ps2/hw/ps2_hw_pal.inc','src/ps2/hw/ps2_hwd.c',
         'src/ps2/hw/ps2_hwd.h','src/ps2/hw/ps2_hwd_null.c'}


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def sources(tree):
    return {str(path.relative_to(tree)):sha(path) for path in sorted((tree/'src').rglob('*')) if path.is_file()}


def prepare(work,baseline,candidate,pb_overlay=False):
    before=sources(baseline)
    if len(before)!=602:raise SystemExit('Complete official602 source snapshot required')
    for variant,source in [('reference',baseline),('candidate',candidate)]:
        target=work/(variant+'-source')
        if target.exists():raise SystemExit('Fresh isolated full-build source directory required')
        shutil.copytree(source/'src',target/'src')
        tools=target/'tools/ps2';tools.mkdir(parents=True)
        for name in ('build.py','sources.txt','sources_hw.txt','opt_units.txt'):
            shutil.copyfile(ROOT/'tools/ps2'/name,tools/name)
        current=sources(target);changed={n for n in before if before[n]!=current[n]}
        allowed=ALLOWED|({'src/hardware/hw_pbatch.inc'} if pb_overlay else set())
        if set(current)!=set(before) or changed!=(allowed if variant=='candidate' else set()):
            raise SystemExit('Unexpected isolated source scope '+str(changed))
        if variant=='candidate' and pb_overlay and current['src/hardware/hw_pbatch.inc']!='08e3ce0c810ba69f957ab8aaa0082e28bcdb1e3f7a3939f8d0da1e8133e2e350':
            raise SystemExit('Only the tested complete PB scratch overlay is permitted')
        manifest={'variant':variant,'source_commit':'85bd9716b1ed114d46f95831c64a87623038eeaa',
                  'source_files':len(current),'source_sha256':current,'only_baseline_differences':sorted(changed),
                  'tested_PB_scratch_overlay':pb_overlay and variant=='candidate',
                  'build_tools_sha256':{n:sha(tools/n) for n in ('build.py','sources.txt','sources_hw.txt','opt_units.txt')}}
        (work/(variant+'-source.json')).write_text(json.dumps(manifest,indent=2)+'\n')
    if len(str(work/'reference-source'))!=len(str(work/'candidate-source')):
        raise SystemExit('Equal-length isolated source paths required')


def build(work,variant,config):
    source=work/(variant+'-source');output=work/(variant+'-hardware')
    manifest=json.loads((work/(variant+'-source.json')).read_text())
    if manifest['source_sha256']!=sources(source):raise SystemExit('Frozen full-build source changed')
    for name,digest in manifest['build_tools_sha256'].items():
        if sha(source/'tools/ps2'/name)!=digest:raise SystemExit('Frozen default build tool changed')
    overrides={k:v for k,v in os.environ.items() if k.startswith('SRB2_PS2_') and k not in ('SRB2_PS2_HW','SRB2_PS2_OUT')}
    if overrides:raise SystemExit('No experiment build overrides permitted '+str(overrides))
    config_bytes=config.read_bytes();digest=sha(config)
    output.mkdir(parents=True,exist_ok=True)
    environment=dict(os.environ,SRB2_PS2_HW='1',SRB2_PS2_OUT=str(output))
    command=[sys.executable,'-B',str(Path(__file__).resolve()),'--execute',variant,'--work',str(work),'--config',str(config)]
    result=subprocess.run(command,cwd=ROOT,env=environment,capture_output=True,text=True)
    (work/(variant+'-full-build.log')).write_text(' '.join(command)+'\n'+result.stdout+result.stderr)
    print((result.stdout+result.stderr)[-2200:],flush=True)
    if result.returncode:raise SystemExit('Complete default Hardware build failed '+variant)
    if config.read_bytes()!=config_bytes or (output/'gen/config.h').read_bytes()!=config_bytes:
        raise SystemExit('Pinned baseline generated configuration changed')
    if manifest['source_sha256']!=sources(source):raise SystemExit('Frozen source changed during full build')
    report_path=output/'build-report.json';report=json.loads(report_path.read_text())
    report['lighttable_isolated_source']={'manifest':str(work/(variant+'-source.json')),
        'manifest_sha256':sha(work/(variant+'-source.json')),'source':manifest,
        'baseline_generated_config':str(config),'baseline_generated_config_sha256':digest,
        'scope':'Complete default Hardware flags/features/203 C +2 VU inputs; only version metadata config is byte-frozen to official baseline'}
    report_path.write_text(json.dumps(report,indent=2)+'\n')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work',type=Path,default=ROOT/'build/nov15-next12/lighttable')
    parser.add_argument('--base',type=Path,default=ROOT/'build/nov15-next12/baseline-src')
    parser.add_argument('--candidate',type=Path,default=ROOT/'build/nov15-next12/lighttable-src')
    parser.add_argument('--config',type=Path,default=ROOT/'build/nov15-next12/baseline/hardware/gen/config.h')
    parser.add_argument('--prepare-only',action='store_true');parser.add_argument('--build',choices=('reference','candidate','both'))
    parser.add_argument('--pb-overlay',action='store_true',help='Permit only the exact tested08e3 PB file as the sixth candidate change')
    parser.add_argument('--execute',choices=('reference','candidate'),help=argparse.SUPPRESS)
    args=parser.parse_args();work=args.work.resolve();work.mkdir(parents=True,exist_ok=True)
    if args.execute:
        source=work/(args.execute+'-source');script=source/'tools/ps2/build.py'
        spec=importlib.util.spec_from_file_location('lighttable_default_build',script);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        data=args.config.resolve().read_bytes()
        def pinned_config():
            module.GEN.mkdir(parents=True,exist_ok=True);(module.GEN/'config.h').write_bytes(data)
        module.gen_config=pinned_config;sys.argv=[str(script),'--jobs','1'];module.main();return
    if not args.build:
        prepare(work,args.base.resolve(),args.candidate.resolve(),args.pb_overlay)
        if args.prepare_only:return
    for variant in ('reference','candidate') if args.build in (None,'both') else (args.build,):
        build(work,variant,args.config.resolve())


if __name__=='__main__':main()
