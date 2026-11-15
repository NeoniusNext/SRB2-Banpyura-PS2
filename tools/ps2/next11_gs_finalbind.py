#!/usr/bin/env python3
"""Read-only complete final CLUT graph binding and changed-instruction inventory."""
import argparse
import copy
import json
from pathlib import Path
import re
import shutil
import struct

from audio_storage_next7_finalbind import Object
from next8_gs_codecheck import canonical_relocations, symbols
from next10_gs_hosttest import ROOT, dump, sha
from next9_gs_finalbind import upload_graph
from next11_gs_hosttest import configure_graph, bind
import next10_gs_low as graph


def inventory(work,baseline,current):
    objects={"before":Object(baseline),"after":Object(current)}
    old,new=objects.values();report={};changed=[]
    def frames(data):
        words=struct.unpack("<"+"I"*(len(data)//4),data)
        return sorted(set(65536-(word&65535) for word in words if word>>16==0x27bd and word&0x8000))
    for sec in sorted(set(old.sections)|set(new.sections)):
        if not sec.startswith(".text."):continue
        before=old.content(old.sections[sec]) if sec in old.sections else None
        after=new.content(new.sections[sec]) if sec in new.sections else None
        oldr=canonical_relocations(old,sec) if sec in old.sections else []
        newr=canonical_relocations(new,sec) if sec in new.sections else []
        if before==after and oldr==newr:continue
        row={"before_section_bytes":len(before) if before is not None else None,"after_section_bytes":len(after) if after is not None else None,
             "instruction_bytes_changed":before!=after}
        if before!=after:
            changed.append(sec)
            for tag,obj,data in (("before",old,before),("after",new,after)):
                if data is not None:
                    row[tag+"_symbol_bytes"]=symbols(obj)[sec[6:]]["bytes"];row[tag+"_frames"]=frames(data)
        else:
            if len(oldr)!=len(newr):raise RuntimeError("Instruction-identical caller relocation count changed")
            renames=[]
            for a,b in zip(oldr,newr):
                a,b=copy.deepcopy(a),copy.deepcopy(b)
                a["target"].pop("size");b["target"].pop("size")
                if a==b:continue
                ta,tb=a["target"],b["target"]
                if not (re.fullmatch(r"CSWTCH\.\d+",ta["name"]) and re.fullmatch(r"CSWTCH\.\d+",tb["name"])):
                    raise RuntimeError("Unexpected instruction-identical caller relocation semantics changed: "+sec)
                sa,sb=ta["section"],tb["section"]
                original_sections=[s for s in old.sections if graph.canonical(s)==sa]
                current_sections=[s for s in new.sections if graph.canonical(s)==sb]
                if len(original_sections)!=1 or len(current_sections)!=1:raise RuntimeError("Ambiguous generated switch-table input section")
                raw_a,raw_b=original_sections[0],current_sections[0]
                if old.signature(raw_a)!=new.signature(raw_b) or canonical_relocations(old,raw_a)!=canonical_relocations(new,raw_b):
                    raise RuntimeError("Renamed complete generated switch table differs")
                a["target"]["name"]=b["target"]["name"];a["target"]["section"]=b["target"]["section"]
                if a!=b:raise RuntimeError("Generated table rename changes another relocation field")
                renames.append({"before":sa,"after":sb,"complete_target_bytes_and_ordered_RELAs_identical":True})
            row["only_target_symbol_size_descriptors_or_verified_generated_table_names_changed"]=True
            row["verified_generated_table_renames"]=renames
        report[sec]=row
    active = ".text.clut_swizzle_active" if ".text.clut_swizzle_active" in new.sections else ".text.clut_swizzle"
    wanted={".text.clut_build_into",".text.clut_fixed_refresh.lto_priv.0",".text.ramp_clut.lto_priv.0",active}
    if set(changed)!=wanted:raise RuntimeError("Unaccounted changed instruction sections: "+repr(changed))
    result={"baseline_engine":str(baseline),"current_engine":str(current),"sections":report,"changed_instruction_sections":changed,
            "complete_changed_section_byte_delta":sum((row["after_section_bytes"] or 0)-(row["before_section_bytes"] or 0) for row in report.values() if row["instruction_bytes_changed"])}
    expected_delta=-136 if active.endswith("_active") else -280
    if result["complete_changed_section_byte_delta"]!=expected_delta:raise RuntimeError("Unexpected complete CLUT text delta")
    if frames(new.content(new.sections[active])):raise RuntimeError("Active core gained a stack frame")
    for name in wanted-{active}:
        if frames(old.content(old.sections[name]))!=frames(new.content(new.sections[name])):raise RuntimeError("CLUT caller frame changed")
    dump(work/"changed-code-inventory.json",result)
    return result


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out",type=Path,required=True)
    ap.add_argument("--baseline-engine",type=Path,default=ROOT/"build/nov15-next11/baseline/hardware/obj/engine_lto.o")
    ap.add_argument("--baseline-elf",type=Path,default=ROOT/"build/nov15-next11/baseline/hardware/SRB2.ELF")
    ap.add_argument("--current-engine",type=Path,required=True)
    ap.add_argument("--current-elf",type=Path,required=True)
    ap.add_argument("--build-report",type=Path,required=True)
    ap.add_argument("--measurements",type=Path,default=ROOT/"build/nov15-next11/gs")
    ap.add_argument("--inventory-only",action="store_true")
    args=ap.parse_args();work=args.out.resolve();work.mkdir(parents=True,exist_ok=True)
    measured_engine=args.measurements/"candidate-hardware/obj/engine_lto.o"
    code=inventory(work,args.baseline_engine,args.current_engine if args.inventory_only else measured_engine)
    if args.inventory_only:return
    report=json.loads(args.build_report.read_text())
    if report["elf_sha256"]!=sha(args.current_elf) or report["compile_diagnostics"] or report["link_diagnostics"]:
        raise RuntimeError("Final default Hardware build report/hash/diagnostics differ")
    expected=json.loads((args.measurements/"candidate-source-freeze.json").read_text())["source_sha256"]["src/ps2/hw/ps2_hw_tex.inc"]
    inputs=[value for path,value in report["input_sha256"].items() if path.endswith("/src/ps2/hw/ps2_hw_tex.inc")]
    if inputs!=[expected] or sha(ROOT/"src/ps2/hw/ps2_hw_tex.inc")!=expected:raise RuntimeError("Final texture source differs from measured source")
    engines={"base":args.baseline_engine,"cur":args.current_engine};graphs={}
    for tag,path in engines.items():
        configure_graph(path);nodes,_=graph.bundle(work,path,tag)
        fresh=json.loads((work/(tag+"-graph.json")).read_text())
        measured=json.loads((args.measurements/"ee"/(tag+"-graph.json")).read_text())
        if fresh["nodes"]!=measured["nodes"] or fresh["original_GP0"]!=measured["original_GP0"] or fresh["preserved_local_GP_aliases"]!=measured["preserved_local_GP_aliases"]:
            raise RuntimeError("Complete final code/data/ordered RELA/GP graph differs from timed graph: "+tag)
        graphs[tag]={"code_nodes":sum(n["kind"]=="code" for n in nodes.values()),"data_nodes":sum(n["kind"]=="data" for n in nodes.values()),
                     "ordered_RELAs":sum(len(n["ordered_relocations"]) for n in nodes.values())}
    for name in ("GS_NEXT11.ELF","build-report.json"):
        shutil.copy2(args.measurements/"ee"/name,work/name)
    bind(work,engines,{"base":args.baseline_elf,"cur":args.current_elf},work/"GS_NEXT11.ELF",True)
    previous=upload_graph({"baseline":args.baseline_engine,"measured":args.baseline_engine,"current":args.current_engine})
    dump(work/"preceding-upload-retention.json",previous)
    dump(work/"final-binding-report.json",{"current_engine_sha256":sha(args.current_engine),"current_elf_sha256":sha(args.current_elf),
         "build_report_sha256":sha(args.build_report),"texture_source_sha256":expected,"graphs":graphs,
         "complete_changed_section_byte_delta":code["complete_changed_section_byte_delta"],"all_original_instructions_ordered_RELA_data_GP_literals_and_resolved_bytes_match_timed_graph":True})
    print("Final whole Hardware CLUT graph, reached SDK closure, original literals, wrappers and retained uploads bind.")


if __name__=="__main__":main()
