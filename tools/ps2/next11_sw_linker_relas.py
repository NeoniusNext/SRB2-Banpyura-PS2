"""Replay an exact link with retained RELAs, preserving every allocated byte.

This diagnostic ELF only exposes GNU ld's exact merged-string mapping. It is
never shipped or executed and needs one granted serial compiler/link worker.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from next7_render_fog_codecheck import Elf32
from next10_sw_bounds_codecheck import symbol_tables

ROOT=Path(__file__).resolve().parents[2]


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def allocated(elf):
    return {name:{'type':s[1],'flags':s[2],'address':s[3],'bytes':s[5],'alignment':s[8],
                  'sha256':None if s[1]==8 else hashlib.sha256(elf.bytes(s)).hexdigest()}
            for name,s in zip(elf.names,elf.sections) if s[2]&2}


def defined_symbols(elf):
    return {(s['name'],s['value'],s['size'],s['type'],s['binding'])
            for table in symbol_tables(elf).values() for s in table if s['section'] and s['name']}


def verify(original,diagnostic):
    before,after=allocated(original),allocated(diagnostic)
    extra={name:entry for name,entry in after.items() if name not in before}
    if any(after.get(name)!=entry for name,entry in before.items()) or any(entry['bytes'] for entry in extra.values()):
        raise SystemExit('Diagnostic --emit-relocs changed an allocated byte/section/address/capacity')
    old,new=defined_symbols(original),defined_symbols(diagnostic)
    if not old<=new:raise SystemExit('Diagnostic --emit-relocs changed an original defined symbol address/size')
    header=lambda data:tuple(__import__('struct').unpack_from('<16sHHIIIIIHHHHHH',data)[i] for i in (1,2,3,4,7))
    if header(original.data)!=header(diagnostic.data):raise SystemExit('Diagnostic ELF class/type/entry/ABI changed')
    return {'every_allocated_section_byte_address_size_capacity_and_alignment_equal':True,
            'allocated_sections':allocated(original),'all_original_defined_symbols_equal':True,
            'additional_zero_byte_zero_capacity_sections':extra,
            'original_defined_symbol_count':len(old),'diagnostic_additional_defined_symbols':len(new-old)}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,required=True);parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();build=args.build.resolve();out=args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT/'build'):raise SystemExit('Fresh diagnostic output under build/ required')
    out.mkdir(parents=True)
    report_path=build/'build-report.json';report=json.loads(report_path.read_text())
    engine=build/'obj/engine_lto.o';elf=build/'SRB2.ELF';map_path=build/'SRB2.map'
    if sha(elf)!=report['elf_sha256']:raise SystemExit('Canonical exact ELF/report pin differs')
    inputs={str(p):sha(p) for p in (report_path,engine,elf,map_path,Path(report['link_command'][0]))}
    # Map LOAD/member records identify all SDK archives/objects selected by ld.
    for name in re.findall(r'(/[^\s()]+\.(?:a|o))',map_path.read_text()):
        path=Path(name)
        if path.is_file() and not name.endswith('/engine_lto.o'):inputs[str(path)]=sha(path)
    command=list(report['link_command']);sites=[i for i,arg in enumerate(command) if arg.endswith('/engine_lto.o')]
    if len(sites)!=1:raise SystemExit('One exact canonical engine input required')
    command[sites[0]]=str(engine);command[command.index('-o')+1]=str(out/'LINKER-RELAS.ELF')
    sites=[i for i,arg in enumerate(command) if arg.startswith('-Wl,-Map=')]
    if len(sites)!=1:raise SystemExit('One exact canonical map output required')
    command[sites[0]]='-Wl,-Map='+str(out/'LINKER-RELAS.map');command.append('-Wl,--emit-relocs')
    result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True)
    (out/'link.log').write_text(' '.join(command)+'\n'+result.stdout+result.stderr)
    if result.returncode:raise SystemExit('Diagnostic retained-RELA link failed')
    diagnostics='\n'.join(line for line in (result.stdout+result.stderr).splitlines()
                          if 'linking abicalls files with non-abicalls files' not in line).strip()
    if diagnostics:raise SystemExit('Diagnostic retained-RELA link has new diagnostics')
    if inputs!={name:sha(Path(name)) for name in inputs}:raise SystemExit('Original pinned linker inputs changed')
    diagnostic=out/'LINKER-RELAS.ELF';checked=verify(Elf32(elf),Elf32(diagnostic))
    checked.update(original_ELF=str(elf),original_ELF_sha256=sha(elf),original_engine=str(engine),
                   original_engine_sha256=sha(engine),original_map=str(map_path),original_map_sha256=sha(map_path),
                   diagnostic_ELF_sha256=sha(diagnostic),exact_link_command=command,
                   all_pinned_linker_inputs_unchanged=inputs)
    (out/'linker-rela-report.json').write_text(json.dumps(checked,indent=2)+'\n')
    print('Retained exact linker RELAs; all original allocated bytes and symbols preserved: '+str(out),flush=True)


if __name__=='__main__':main()
