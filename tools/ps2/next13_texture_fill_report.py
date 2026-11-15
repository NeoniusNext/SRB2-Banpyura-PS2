#!/usr/bin/env python3
"""Strictly validate all384 actual row-graph timing cases and nine repetitions."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def matrix():
 rows=[]
 for cache in range(2):
  for fmt in range(2):
   for scenario in range(4):
    for width in (1,2,3,4,7,16,31,64,127,256,511,1024):
     for count in (1,64):
      rows.append((fmt,scenario,width,1 if scenario==2 else 3 if scenario==3 else 0,
         1 if scenario==2 else 2 if scenario==3 else 0,1 if scenario==1 else 15 if scenario==3 else 0,
         (4 if fmt else 1) if scenario==1 else (12 if fmt else 15) if scenario==3 else 0,count,cache))
 return rows


def parse(content,exact):
 if content.count('FILL NEXT13 DONE')!=1 or 'FILL NEXT13 FAIL' in content:raise ValueError('Missing completion or native failure')
 if re.findall(r'FILL NEXT13 PASS cases=\d+ pixels=\d+ digest=[0-9a-f]{8}',content)!=[exact]:raise ValueError('Native exact matrix count/digest changed')
 pattern=r'FILL13 TIMING row=(\d+) fmt=(\d+) scenario=(\d+) width=(\d+) dx=(\d+) dy=(\d+) source=(\d+) dst=(\d+) count=(\d+) cache=(\d+) rep=(\d+) old=(\d+) new=(\d+)'
 records=re.findall(pattern,content)
 if len(records)!=384*9 or len(records)!=content.count('FILL13 TIMING row='):raise ValueError('Missing/extra/malformed timing record')
 wanted=matrix();rows={}
 for record in records:
  row,*parameters,rep,old,new=map(int,record)
  if not 0<=row<384 or tuple(parameters)!=wanted[row] or not 0<=rep<9 or not 0<old<0x80000000 or not 0<new<0x80000000:raise ValueError('Invalid prescribed parameters/repetition/counter')
  item=rows.setdefault(row,{'parameters':parameters,'samples':{}})
  if rep in item['samples']:raise ValueError('Duplicate repetition')
  item['samples'][rep]=[old,new]
 if set(rows)!=set(range(384)) or any(set(v['samples'])!=set(range(9)) for v in rows.values()):raise ValueError('Incomplete prescribed row/repetition sets')
 for item in rows.values():
  item['old_median']=statistics.median(v[0] for v in item['samples'].values());item['new_median']=statistics.median(v[1] for v in item['samples'].values())
  item['percent']=(item['new_median']/item['old_median']-1)*100
 def total(selected):
  old=sum(v['old_median'] for v in selected);new=sum(v['new_median'] for v in selected)
  return {'rows':len(selected),'old_sum':old,'new_sum':new,'percent':(new/old-1)*100,
    'faster':sum(v['new_median']<v['old_median'] for v in selected),'ties':sum(v['new_median']==v['old_median'] for v in selected),'slower':sum(v['new_median']>v['old_median'] for v in selected)}
 values=list(rows.values())
 cohorts={'all':total(values),'index_contiguous_copy':total([v for v in values if v['parameters'][0]==0 and v['parameters'][3]==0]),
    'index_decimated':total([v for v in values if v['parameters'][0]==0 and v['parameters'][3]!=0]),
    'IA88':total([v for v in values if v['parameters'][0]==1]),'single_call':total([v for v in values if v['parameters'][7]==1]),
    'warm':total([v for v in values if v['parameters'][8]==0]),'cache_flush_requested':total([v for v in values if v['parameters'][8]==1])}
 return {'exact':exact,'rows':rows,'cohorts':cohorts,'all_rows_improve_or_tie':cohorts['all']['slower']==0}


def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--work',type=Path,required=True);p.add_argument('--host-native-report',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--negative-controls',action='store_true');a=p.parse_args()
 if a.out.exists():raise SystemExit('Fresh timing report destination required')
 a.out.mkdir(parents=True)
 exact=json.loads(a.host_native_report.read_text())['stdout'].strip();content=(a.work/'run.log').read_text()
 pre=json.loads((a.work/'run-pre.json').read_text());post=json.loads((a.work/'run-post.json').read_text())
 if post['rc'] or not post['all_frozen_inputs_unchanged'] or pre['input_sha256']!=post['input_sha256'] or post['log_sha256']!=sha(a.work/'run.log') or not post['critical_settings_unchanged'] or not post['emulator_binary_unchanged']:raise SystemExit('Native run inputs/config/log/return not preserved')
 report=parse(content,exact);controls=[]
 if a.negative_controls:
  first=re.search(r'FILL13 TIMING row=0[^\n]*',content)[0]
  changes=[content.replace(first,'',1),content+'\n'+first,content.replace('row=0 fmt=0','row=0 fmt=1',1),content.replace('FILL NEXT13 DONE','FILL NEXT13 INCOMPLETE',1),content+'\nFILL NEXT13 FAIL injected\n',content.replace('digest=ead0caa8','digest=00000000',1),content.replace('old=','old=bad',1),content.replace('rep=0','rep=1',1)]
  folder=a.out/'parser-negative-controls';folder.mkdir()
  for i,changed in enumerate(changes,1):
   (folder/(str(i)+'.log')).write_text(changed)
   try:parse(changed,exact)
   except ValueError as error:controls.append({'mutation':i,'detected':True,'reason':str(error)})
   else:raise SystemExit('Strict parser mutation escaped '+str(i))
 report.update(parser_negative_controls=controls,run_log_sha256=sha(a.work/'run.log'),fixture_sha256=sha(a.work/'FILL_NEXT13.ELF'),tool_sha256=sha(Path(__file__)))
 (a.out/'benchmark-summary.json').write_text(json.dumps(report,indent=2)+'\n')
 print(json.dumps(report['cohorts'],indent=2))


if __name__=='__main__':main()
