"""Freeze the exact host checker and its loaded repository dependencies."""
import hashlib
import json
from pathlib import Path
import shutil
import sys


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def freeze_checker(out,checker):
 checker=Path(checker).resolve();project=checker.parents[2]
 files={checker}
 for module in tuple(sys.modules.values()):
  name=getattr(module,'__file__',None)
  if not name:continue
  path=Path(name).resolve()
  if path.is_file() and path.suffix=='.py' and path.is_relative_to(project/'tools/ps2'):files.add(path)
 before={str(path):sha(path) for path in sorted(files)}
 archive=out/'checker-snapshot';entries=[]
 for name,digest in before.items():
  source=Path(name);dest=archive/source.relative_to(project)
  dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,dest)
  if sha(dest)!=digest:raise SystemExit('Checker snapshot copy differs '+name)
  entries.append({'original_path':name,'snapshot_path':str(dest.resolve()),'sha256':digest})
 executable=Path(sys.executable).resolve()
 report={'checker_original_path':str(checker),'checker_sha256':before[str(checker)],
   'all_loaded_repository_Python_dependencies':entries,'Python_executable':str(executable),
   'Python_executable_sha256':sha(executable),'Python_version':sys.version,
   'archived_before_binding_checks':True}
 path=out/'checker-provenance.json';path.write_text(json.dumps(report,indent=2)+'\n')
 return {'report':report,'manifest':str(path.resolve()),'source_pins':before}


def finish_checker(state):
 for name,digest in state['source_pins'].items():
  if sha(Path(name))!=digest:raise SystemExit('Loaded host checker source changed during binding '+name)
 for entry in state['report']['all_loaded_repository_Python_dependencies']:
  if sha(Path(entry['snapshot_path']))!=entry['sha256']:raise SystemExit('Frozen host checker snapshot changed')
 return {'checker_sha256':state['report']['checker_sha256'],'snapshot_manifest':state['manifest'],
   'snapshot_manifest_sha256':sha(Path(state['manifest'])),
   'all_loaded_repository_checker_sources_unchanged':True,
   'all_frozen_checker_source_bytes_unchanged':True,
   'dependency_count':len(state['source_pins'])}
