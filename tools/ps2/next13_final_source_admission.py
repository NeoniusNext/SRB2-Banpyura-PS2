#!/usr/bin/env python3
"""Admit the exact selected602 source and prepare canonical build commands.

Read-only preparation: never executes build.py, compiler, linker or emulator.
With --revision, verifies every actual source file against that exact Git tree
and requires current HEAD/source cleanliness for a ROOT canonical build.
Without it, records the isolated selected source and pending source commit.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

from next13_geometry_combined_bind import ROOT,WORK,SELECTED,sources
import next13_geometry_variant as baseline_variant


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def git(*args):return subprocess.check_output(['git',*args],cwd=ROOT,text=True).strip()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,default=WORK/'combined-classic-v1/snapshot')
    p.add_argument('--revision',help='Full intended committed Git revision for canonical release admission')
    p.add_argument('--out',type=Path,default=WORK/'final-source-preparation-v1')
    p.add_argument('--final-root',type=Path,default=WORK/'final')
    a=p.parse_args();source,out,final_root=(x.resolve() for x in (a.source,a.out,a.final_root))
    if out.exists() or not out.is_relative_to(WORK):raise RuntimeError('Fresh isolated source-admission output required')
    baseline={name:hashlib.sha256(data).hexdigest() for name,data in baseline_variant.baseline_files().items()}
    expected={**baseline,**SELECTED};actual=sources(source)
    if len(actual)!=602 or actual!=expected:raise RuntimeError('Actual source is not the exact selected four-overlay602 tree')
    commit=None
    if a.revision:
        if not re.fullmatch(r'[0-9a-f]{40}',a.revision):raise RuntimeError('A full explicit immutable Git revision is required')
        raw=subprocess.check_output(['git','ls-tree','-rz','--full-tree',a.revision,'--','src'],cwd=ROOT)
        blobs={}
        for row in raw.split(b'\0'):
            if not row:continue
            metadata,name=row.split(b'\t',1);mode,kind,blob=metadata.decode().split()
            if mode!='100644' or kind!='blob':raise RuntimeError('Exact regular source files required')
            blobs[name.decode()]=blob
        if set(blobs)!=set(actual):raise RuntimeError('Committed complete602 source set differs')
        for name,blob in blobs.items():
            body=(source/name).read_bytes()
            if hashlib.sha1(b'blob '+str(len(body)).encode()+b'\0'+body).hexdigest()!=blob:
                raise RuntimeError('Actual source differs from committed blob: '+name)
        head=git('rev-parse','HEAD')
        if head!=a.revision:raise RuntimeError('Canonical generated metadata must use the admitted current HEAD')
        if source==ROOT and git('status','--porcelain','--','src'):raise RuntimeError('Canonical production source has uncommitted changes')
        commit={'revision':a.revision,'short_revision':git('rev-parse','--short',a.revision),
            'branch':git('rev-parse','--abbrev-ref','HEAD'),'subject':git('log','-1','--format=%s',a.revision),
            'all602_actual_files_match_exact_committed_blobs':True,'current_HEAD_matches_admitted_revision':True}
    tools={}
    for name in ('build.py','sources.txt','sources_hw.txt','opt_units.txt'):
        path=ROOT/'tools/ps2'/name
        original=subprocess.check_output(['git','show','353cc94103aeb38cef87d26151c779302135d0e9:tools/ps2/'+name],cwd=ROOT)
        if path.read_bytes()!=original:raise RuntimeError('Existing official default build policy changed: '+name)
        tools[name]={'path':str(path),'sha256':sha(path)}
    baseline_reports={}
    for role,count in [('software',189),('hardware',203)]:
        path=ROOT/'build/nov15-next12/final'/role/'build-report.json';record=json.loads(path.read_text())
        if record['compiled']!=count or len(record['sources'])!=count or record['diagnostic_build']:
            raise RuntimeError('Clean original full-build precedent differs')
        baseline_reports[role]={'report_path':str(path),'report_sha256':sha(path),'actual_recorded_command':record['command'],
            'complete_C_inputs_compiled':count,'additional_VU_inputs':2 if role=='hardware' else 0,
            'actual_default_flags':record['flags'],'actual_link_command':record['link_command']}
    python='/workspace/.onboarding/assets-venv/bin/python3'
    commands={role:{'environment':{'SRB2_PS2_HW':'1' if role=='hardware' else '0','SRB2_PS2_OUT':str(final_root/role)},
        'working_directory':str(ROOT),'argv':[python,'-B','tools/ps2/build.py','--jobs','1'],
        'requires_explicit_EE_worker_grant':True,'requires_fresh_output_directory':True,
        'no_prof_hwdetail_optout_or_feature_cut_flags':True} for role in ('software','hardware')}
    out.mkdir(parents=True);shutil.copyfile(Path(__file__),out/Path(__file__).name)
    (out/'pinned-build-tools').mkdir()
    for name in tools:shutil.copyfile(ROOT/'tools/ps2'/name,out/'pinned-build-tools'/name)
    report={'status':'CANONICAL SOURCE ADMITTED; builds not executed' if commit else 'SELECTED ISOLATED SOURCE ADMITTED; canonical source commit/build pending',
        'source':str(source),'source_files':602,'source_sha256':expected,
        'baseline_source_revision':baseline_variant.REVISION,'only_baseline_differences':sorted(SELECTED),
        'canonical_source_commit':commit,'official_build_tools_unchanged':tools,'baseline_clean_build_precedent':baseline_reports,
        'proposed_canonical_build_commands':commands,'setup_script':{'path':'/workspace/.onboarding/environment.sh',
            'sha256':sha(Path('/workspace/.onboarding/environment.sh'))},
        'generated_config_scope':'Official build.py gen_config reads actual admitted HEAD/branch/subject. Canonical metadata is a separate source from the frozen d4 exposure config; inspect its actual resulting layout and bind its own full ELF.',
        'compiler_linker_emulator_invoked':False,'checker_sha256':sha(Path(__file__))}
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'status':report['status'],'report':str(out/'report.json'),'source_files':602,
        'only_baseline_differences':sorted(SELECTED),'canonical_commit':commit['revision'] if commit else None,
        'builds_started':False},indent=2))


if __name__=='__main__':main()
