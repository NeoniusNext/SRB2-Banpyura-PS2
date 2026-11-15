#!/usr/bin/env python3
"""Exact CLUT quadword swaps, full packet/state oracle, and actual-LTO graphs."""
import argparse
import copy
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import struct
import subprocess

from audio_storage_next7_finalbind import Object
from next8_gs_codecheck import symbols
from next10_gs_hosttest import ROOT, TOOLS, dump, run, section, sha
import hw_hosttest
import next10_gs_low as graph
from next11_gs_variant import rewrite, host_emulation, INLINE_AFTER, INLINE_BEFORE

CODE = ("clut_alloc", "clut_build_into", "clut_fixed_refresh", "ramp_clut", "__wrap_memcpy", "__wrap_memset")


def prepare(work, base, current=None, prototype=False, negative=0, ee=False):
    work.mkdir(parents=True, exist_ok=True)
    current = current or base
    hw_hosttest.make_shim(work)
    (work / "hw_model.h").write_text((base / "hardware/hw_model.h").read_text().replace('#include "../doomtype.h"', ""))
    shim = (work / "shim.h").read_text()
    if ee:
        shim = shim.replace("typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;", "#include <tamtypes.h>")
    (work / "shim.h").write_text(shim)
    private = (base / "ps2/hw/ps2_hw_priv.inc").read_text()
    dbg = (base / "ps2/hw/ps2_hwd_dbg.h").read_text().replace('#include "../../doomtype.h"', '#include "shim.h"')
    (work / "ps2_hwd_dbg.h").write_text(dbg)
    common = '#ifndef NEXT11_GS_COMMON_H\n#define NEXT11_GS_COMMON_H\n#include "shim.h"\n#include "ps2_hwd_dbg.h"\n#include <stdlib.h>\n#include <string.h>\n#include <math.h>\n'
    if ee: common += '#include <kernel.h>\n'
    common += section(private, "#define GSR_PRIM", "// DMA channel 2")
    common += section(private, "typedef union\n{\n\tu64 d[2];", "static inline u32 cyc")
    common += section(private, "#define HWGIF_TAG", "#define PS2HWD_MAXPOLY")
    common += section(private, "#define NBUF", "// state of the stream")
    state = section(private, "// state of the stream", "int ps2hwd_dbg_flags;")
    state = state.replace("static struct\n{\n\tint up;", "typedef struct\n{\n\tint up;")
    state = state.replace("} H __attribute__((aligned(16)));", "} __attribute__((aligned(16))) clut_state_t;\nextern clut_state_t H;")
    common += state
    common += section((base / "ps2/hw/ps2_hw_light.inc").read_text(), "#define LIGHT_ZMAX", "// Depths (ascending)")
    draw = (base / "ps2/hw/ps2_hw_draw.inc").read_text()
    common += section(draw, "// light ramp / fog specification", "enum { LM_NONE")
    common += '''
typedef struct { int on, ready, dmac, dmac_set, chunk_open; } clut_v_t;
typedef struct { u8 prefix[12]; int open; u8 suffix[976]; } clut_vu_t;
extern clut_v_t V;
extern clut_vu_t VU;
extern int ps2hwd_dbg_flags;
extern u32 fixture_spal[256];
extern int fixture_spal_set, fixture_palette_mode;
extern u8 *fixture_lt_idx[1024];
void clut_pk_flush(void);
void clut_vu_chunk_end(void);
void clut_abort(void) __attribute__((noreturn));
#define CONS_ERROR 0
#define CONS_Alert(...) ((void)0)
#define abort clut_abort
void base_clut_build_into(u32,u32,u32,u32); void cur_clut_build_into(u32,u32,u32,u32);
void base_clut_fixed_refresh(int); void cur_clut_fixed_refresh(int);
u32 base_ramp_clut(const rampspec_t *); u32 cur_ramp_clut(const rampspec_t *);
void base_src_clut_swizzle(u32 *); void cur_src_clut_swizzle(u32 *);
void base_src_clut_entries(u32,u32,u32,u32 *); void cur_src_clut_entries(u32,u32,u32,u32 *);
void base_src_ramp_entries(const rampspec_t *,u32 *); void cur_src_ramp_entries(const rampspec_t *,u32 *);
#ifdef NEXT11_GS_EE
_Static_assert(sizeof H == 36896 && offsetof(clut_state_t,pal) == 35264, "complete production H ABI");
_Static_assert(sizeof V == 20 && sizeof VU == 992 && sizeof(rampspec_t) == 36, "complete native VU/ramp ABI");
#endif
#endif
'''
    (work / "common.h").write_text(common)
    gs = (base / "ps2/hw/ps2_hw_gs.inc").read_text()
    regs = (base / "ps2/hw/ps2_hw_regs.inc").read_text()
    pal = (base / "ps2/hw/ps2_hw_pal.inc").read_text()
    for tag, source in (("base",base),("cur",current)):
        tex = (source / "ps2/hw/ps2_hw_tex.inc").read_text()
        if tag == "cur" and prototype: tex = rewrite(tex)
        tex = tex.replace(INLINE_AFTER, INLINE_BEFORE)
        if tag == "cur" and not ee: tex = host_emulation(tex)
        if tag == "cur" and negative:
            old,new = {
                1:("entries[i + j + 8] = t;","entries[i + j + 8] = t ^ 1u;"),
                2:("memcpy((unsigned char *)p + 64, left, 16);","memcpy((unsigned char *)p + 64, right, 16);"),
                3:("ad_set(&h[3], 16ull | (16ull << 32), GSR_TRXREG);","ad_set(&h[3], 15ull | (16ull << 32), GSR_TRXREG);"),
                4:("ad_set(&d[65], 0, GSR_TEXFLUSH);","ad_set(&d[65], 0, GSR_TRXDIR);"),
                5:("H.clut_epoch++; // PS2-HW-101","H.clut_epoch += 2; // PS2-HW-101"),
                6:("if (ps2hwd_dbg_flags & 16)\n\t\treturn;","if (0)\n\t\treturn;"),
            }[negative]
            if tex.count(old) != 1: raise RuntimeError("Missing exact CLUT mutation anchor")
            tex = tex.replace(old,new,1)
        body = section(regs, "static inline void ad_set", "// FRAME_1")
        body += 'static void vu_chunk_flush(void) { if (VU.open) clut_vu_chunk_end(); }\n'
        body += section(gs, "static qw_t *pk_alloc(u32 n)", "// OPT9 (PS2-HW-32)")
        body += section(gs, "static qw_t *ad_alloc(u32 n)", "// Wait until the GS")
        body += section(tex,"static inline u32 a8_to_gs", "static inline u32 pal_hash")
        body += 'static const u32 *out_pal(void) { return fixture_spal_set && fixture_palette_mode ? fixture_spal : H.pal; }\n'
        body += section(pal,"static void pal_light_entries", "\n}")+"\n}\n"
        body += section(tex,"// Reserve a complete 16x16 CT32 CLUT transfer.","static u8 clut_hint")
        body += section(draw,"static const float ramp_zmax", "static inline float zfrag_of_w")
        body += section(draw,"static u32 spec_hash", "static void ov_flush(void)\n{")
        body = body.replace('static u32 ramp_clut', 'static u32 ramp_clut')
        body = body.replace('OV.n','fixture_ov_n').replace('ov_flush_pending();','clut_abort();')
        body = body.replace('pk_flush();','clut_pk_flush();')
        body = body.replace('lt_idx[','fixture_lt_idx[')
        body = 'static int fixture_ov_n;\n'+body
        names = ("ad_set","vu_chunk_flush","pk_alloc","ad_alloc","a8_to_gs","out_pal","pal_light_entries",
                 "clut_alloc","clut_swizzle","clut_entries","clut_palette_dependent","clut_build_into","clut_fixed_blk",
                 "clut_fixed_refresh","ramp_zmax","spec_hash","ramp_entries","ramp_clut")
        for name in sorted(names,key=len,reverse=True):
            prefix = tag + ("_src_" if name in ("clut_swizzle","clut_entries","ramp_entries") else "_")
            body = re.sub(r'\b'+name+r'\b',prefix+name,body)
        for name in ("clut_build_into","clut_fixed_refresh","ramp_clut","src_clut_swizzle","src_clut_entries","src_ramp_entries"):
            body = re.sub(r'static (void|u32) '+tag+'_'+name+r'\(',r'\1 '+tag+'_'+name+'(',body)
        if ee:
            # Only untimed source formatters/direct fallback tests are compiled.
            for name in ("clut_build_into","clut_fixed_refresh","ramp_clut"):
                body = body.replace(tag+'_'+name+'(',tag+'_source_'+name+'(')
        (work / (tag+"-production.c")).write_text('#include "common.h"\n'+body)


def host_build(work,sanitize=False,expected=0):
    binary=work/("test-sanitize" if sanitize else "test")
    flags=["-std=gnu17","-O2","-ffp-contract=off","-fwrapv","-ffunction-sections","-fdata-sections","-Wall","-Wextra","-Werror","-Wno-unused-function","-Wno-unused-variable","-Wno-unused-parameter","-DPS2_PROFILE"]
    if sanitize: flags += ["-g","-fsanitize=address,undefined","-fno-omit-frame-pointer","-no-pie"]
    run(["gcc",*flags,"-I"+str(work),work/"base-production.c",work/"cur-production.c",TOOLS/"next11_gs_hosttest.c","-Wl,--gc-sections","-o",binary,"-lm"],work/(binary.name+"-build.log"))
    return run([binary],work/(binary.name+".log"),expected)


def configure_graph(path=None):
    graph.CODE = CODE
    if path is not None:
        sections=Object(path).sections
        for name in ("clut_swizzle","clut_swizzle_active"):
            if any(re.fullmatch(r"\.text\."+name+r"(?:\.lto_priv\.\d+)?",sec) for sec in sections):graph.CODE += (name,)
    graph.SPECIAL.update({"pk_flush":"clut_pk_flush", "vu_chunk_end":"clut_vu_chunk_end"})
    graph.SDK.add("floorf")


def native_tables(work, graphs):
    graph.tables(work, graphs)
    operations=[]
    for tag,nodes in graphs.items():
        wanted={"spal_set":None,"spal":None,"lt_idx":None,"pMasterPalette":None,
                "cv_glpaletterendering":None,"cv_glshaders":None,"gl_shadersavailable":None}
        for sec,node in nodes.items():
            name=graph.canonical(sec.split(".",2)[2]) if sec.startswith((".bss.",".sbss.",".data.")) else ""
            if name in wanted: wanted[name]=node["label"]
        if any(value is None for value in wanted.values()):raise RuntimeError("Missing actual palette data nodes")
        operations += ["*(u32 *)"+wanted["spal_set"]+" = (u32)screen;",
                       "memcpy("+wanted["spal"]+", palette, 1024);",
                       "((u8 **)"+wanted["lt_idx"]+")[1] = table;",
                       "*(void **)"+wanted["pMasterPalette"]+" = (void *)1;",
                       "*(u32 *)("+wanted["cv_glpaletterendering"]+" + 24) = (u32)mode;",
                       "*(u32 *)("+wanted["cv_glshaders"]+" + 24) = 1;",
                       "*"+wanted["gl_shadersavailable"]+" = 1;"]
    (work/"native-palette.h").write_text("static void native_palette_setup(int mode, int screen, const u32 *palette, u8 *table) {\n"+"\n".join(operations)+"\n}\n")


def native_build(work,engines,base,current):
    import build as B
    configure_graph()
    prepare(work,base,current,ee=True)
    graphs={}
    for tag,path in engines.items():
        configure_graph(path)
        graphs[tag],_=graph.bundle(work,path,tag)
        obj=Object(path)
        for sec,node in graphs[tag].items():
            if node["kind"]!="code":continue
            for record in node["ordered_relocations"]:
                target=record["original_target"];target_sec=target["section"]
                if target_sec in obj.sections and obj.sections[target_sec][2]&4 and target_sec not in graphs[tag]:
                    if graph.canonical(target["name"]) not in ("pk_flush","vu_chunk_end") and not graph.canonical(target_sec).startswith(".text.ov_flush"):
                        raise RuntimeError("Unclosed actual executable callee: "+target_sec)
    native_tables(work,graphs)
    flags=[f for f in B.CFLAGS if f not in ("-MMD","-MP","-flto")]
    flags += ["-fno-lto","-Werror","-Wno-unused-function","-Wno-unused-variable","-DNEXT11_GS_EE","-I"+str(work)]
    objects=[];reports={}
    for tag,path in engines.items():
        source=work/(tag+"-graph.S");target=source.with_suffix(".o")
        run([B.CC,*flags,*B.INCS,"-c",source,"-o",target],work/(tag+"-graph-build.log"))
        metadata=json.loads((work/(tag+"-graph.json")).read_text())
        graph.restore_section_symbol_types(target,graphs[tag],metadata["original_GP0"])
        reports[tag]=graph.verify_object(path,target,graphs[tag]);objects.append(target)
        target=work/(tag+"-production.o")
        run([B.CC,*flags,*B.INCS,"-c",work/(tag+"-production.c"),"-o",target],work/(tag+"-production-build.log"));objects.append(target)
    fixture=work/"fixture.o"
    run([B.CC,*flags,*B.INCS,"-c",TOOLS/"next11_gs_hosttest.c","-o",fixture],work/"fixture-build.log")
    binary=work/"GS_NEXT11.ELF"
    run([B.CC,*B.LDFLAGS,*objects,fixture,"-Wl,-Map="+str(work/"GS_NEXT11.map"),"-o",binary,"-ldebug","-lpatches","-lm"],work/"fixture-link.log")
    dump(work/"build-report.json",{"elf_sha256":sha(binary),"fixture_sha256":sha(TOOLS/"next11_gs_hosttest.c"),"flags":flags,"all_complete_embedded_nodes":reports})
    print("Built "+str(binary))


def floor_binding(work,binaries,fixture):
    import build as B
    library=B.DEV/"ee/mips64r5900el-ps2-elf/lib/libm.a"
    source=work/"sdk-floorf.o"
    source.write_bytes(subprocess.check_output([str(B.DEV/"ee/bin/mips64r5900el-ps2-elf-ar"),"p",str(library),"libm_a-sf_floor.o"]))
    original=Object(source);body=original.content(original.sections[".text"])
    constants=original.content(original.sections[".rodata.cst4"])
    records=original.relocations[".text"]
    if len(body)!=320 or len(constants)!=12 or len(records)!=8 or symbols(original)["floorf"]["bytes"]!=316:
        raise RuntimeError("Actual SDK floorf object/constant/RELA shape changed")
    words=struct.unpack("<"+"I"*(316//4),body[:316])
    if any(word>>26 in (2,3) or word&0xfc00003f==9 for word in words):
        raise RuntimeError("SDK floorf has a nonleaf branch/call")
    result={}
    for tag,path in dict(binaries,fixture=fixture).items():
        elf=Object(path);table=symbols(elf);actual=graph.linked_span(elf,table,"floorf",320)
        def at(address,count):
            for header in elf.headers:
                if header[1]!=8 and header[3]<=address and address+count<=header[3]+header[5]:
                    return elf.content(header)[address-header[3]:address-header[3]+count]
            raise RuntimeError("SDK constant address absent")
        concrete={}
        for addend in (0,4,8):
            matching=[r for r in records if r["explicit_addend"]==addend]
            highs=[r for r in matching if r["type"]==5];lows=[r for r in matching if r["type"]==6]
            if len(highs)!=len(lows) or not highs:raise RuntimeError("Missing original SDK HI/LO constant pair")
            addresses=[]
            for high,low in zip(highs,lows):
                hw=struct.unpack_from("<I",actual,high["offset"])[0];lw=struct.unpack_from("<I",actual,low["offset"])[0]
                address=(((hw&65535)<<16)+struct.unpack("<h",struct.pack("<H",lw&65535))[0])&0xffffffff
                if at(address,4)!=constants[addend:addend+4] or address%4:raise RuntimeError("SDK floorf concrete constant differs")
                addresses.append(address)
            if len(set(addresses))!=1:raise RuntimeError("Same SDK constant resolved inconsistently")
            concrete[addend]=addresses[0]
        normalized=[{"offset":r["offset"],"type":r["type"],"addend":r["explicit_addend"]} for r in records]
        expected=graph.relocate(body,normalized,lambda r:concrete[r["addend"]],table["_gp"]["address"],0)
        if expected!=actual:raise RuntimeError("SDK floorf complete resolved instructions/padding differ")
        result[tag]={"elf_sha256":sha(path),"complete_code_and_padding_bytes":320,"symbol_bytes":316,
                     "ordered_RELA":records,"three_exact_constant_bytes_hex":constants.hex(),"concrete_constant_addresses":concrete,
                     "all_resolved_original_bytes_identical":True,"stack_frame_bytes":16}
    dump(work/"sdk-floorf-binding.json",{"archive":str(library),"archive_sha256":sha(library),"member_sha256":sha(source),"versions":result})


def bind(work,engines,binaries,fixture,negative=False):
    configure_graph()
    data_literals={}; literal_report={}
    for tag,path in engines.items():
        obj=Object(path);elf=Object(binaries[tag]);mapped=graph.map_addresses(binaries[tag].with_suffix(".map"))
        metadata=json.loads((work/(tag+"-graph.json")).read_text());found={};details=[]
        for sec,node in metadata["nodes"].items():
            if node["kind"]!="data" or obj.sections[sec][1]==8 or obj.sections[sec][2]&32:continue
            mapping=mapped.get(sec)
            if mapping is None or mapping["bytes"]!=obj.sections[sec][5]:raise RuntimeError("Missing complete original data map entry")
            for record in node["ordered_relocations"]:
                target=record["original_target"];target_sec=target["section"]
                if target_sec not in obj.sections or not obj.sections[target_sec][2]&32:continue
                if record["type"]!=2:raise RuntimeError("Unexpected data-to-literal relocation")
                address=mapping["address"]+record["offset"];value=None
                for header in elf.headers:
                    if header[1]!=8 and header[3]<=address and address+4<=header[3]+header[5]:
                        value=struct.unpack_from("<I",elf.content(header),address-header[3])[0]
                if value is None:raise RuntimeError("Actual literal data pointer absent")
                at=target["value"]+record["addend"]
                source=obj.content(obj.sections[target_sec])
                if not 0<=at<len(source):raise RuntimeError("Original data literal offset outside section")
                literal=source[at:].split(b"\0",1)[0]+b"\0";candidates=graph.literal_candidates(elf,literal)
                alignment=obj.sections[target_sec][8]
                if value not in candidates or value%alignment!=at%alignment:raise RuntimeError("Actual complete data literal/alignment differs")
                key=(target_sec,at)
                if key in found and found[key]["address"]!=value:raise RuntimeError("Identical input literal resolved inconsistently")
                found[key]={"address":value}
                details.append({"owner_section":sec,"R32_offset":record["offset"],"source_section":target_sec,"source_offset":at,
                                "concrete_address":value,"exact_literal_hex":literal.hex(),"identical_merge_candidates":candidates,"alignment":alignment})
        data_literals[str(path)]=found;literal_report[tag]=details
    original_resolve=graph.resolve_original
    def resolved(obj,elf,syms,mapped,record,literals):
        target=record["original_target"];key=(target["section"],target["value"]+record["addend"])
        extended=dict(data_literals.get(str(obj.path),{}));extended.update(literals)
        return original_resolve(obj,elf,syms,mapped,record,extended)
    graph.resolve_original=resolved
    try: graph.bind(work,engines,binaries,fixture,negative)
    finally: graph.resolve_original=original_resolve
    dump(work/"merged-data-literal-binding.json",literal_report)
    floor_binding(work,binaries,fixture)
    inputs=set(engines.values())|set(binaries.values())|{fixture}
    inputs.update(path.with_suffix(".map") for path in binaries.values())
    inputs.update(path.parent/"build-report.json" for path in binaries.values())
    dump(work/"binding-inputs.json",{str(path.resolve()):sha(path) for path in sorted(inputs)})


def audit_source(work,baseline,current):
    before=(baseline/"ps2/hw/ps2_hw_tex.inc").read_text();after=(current/"ps2/hw/ps2_hw_tex.inc").read_text()
    if rewrite(after,True)!=before:raise RuntimeError("CLUT reversal does not restore entire texture source")
    unchanged={}
    for path in sorted((baseline/"ps2/hw").iterdir()):
        if path.is_file() and path.name!="ps2_hw_tex.inc":
            if path.read_bytes()!=(current/"ps2/hw"/path.name).read_bytes():raise RuntimeError("Unexpected GS source edit")
            unchanged[path.name]=sha(path)
    if after.count("clut_swizzle(e);")!=1 or (current/"ps2/hw/ps2_hw_draw.inc").read_text().count("clut_swizzle(e);")!=1:
        raise RuntimeError("Unexpected complete CLUT caller set")
    dump(work/"source-audit.json",{"whole_texture_reversal_identical":True,"all_other_GS_sources_unchanged":unchanged,
         "baseline_tex_sha256":sha(baseline/"ps2/hw/ps2_hw_tex.inc"),"candidate_tex_sha256":sha(current/"ps2/hw/ps2_hw_tex.inc"),
         "source_callers":["clut_build_into","ramp_clut"],"payload_source":"clut_alloc returns clut_alloc's pk_alloc(72) result h[6].w",
         "alignment_chain":"gs_setup memalign(64) -> H.ring[cur] qw_t pointer -> pk_alloc integer qword offset -> h[6] payload offset96",
         "debug_identity_before_pointer_access":True,"non_quadword_word_aligned_scalar_fallback_retained":True})


def timing_matrix():
    cases=[(0,k,0) for k in (1,2,3,4,5,6,8,9)]+[(1,0,1),(1,0,0),(1,1,0)]+[(2,k,0) for k in range(1,6)]+[(2,1,1)]
    rows=[]
    for cache in range(2):
        for identity in range(2):
            for ci,(family,kind,flag) in enumerate(cases):
                for length in range(1 if family==2 else 3):
                    count=1 if length==0 else (2 if family==1 else 8) if length==1 else (16 if family==1 else 64)
                    rows.append({"case":ci,"family":family,"kind":kind,"flag":flag,"identity":identity,"cache":cache,"count":count})
    if len(rows)!=156:raise RuntimeError("Invalid prescribed timing matrix")
    return rows


def parse_timing(content,exact):
    if content.count("GS NEXT11 DONE")!=1 or "GS NEXT11 FAIL" in content:raise RuntimeError("Missing completion or native failure")
    if re.findall(r"GS NEXT11 EXACT cases=\d+ packets=\d+ digest=[0-9a-f]{8}",content)!=[exact]:raise RuntimeError("Exact corpus digest/count differs")
    pattern=r"GS11 TIMING row=(\d+) case=(\d+) family=(\d+) kind=(\d+) flag=(\d+) identity=(\d+) cache=(\d+) count=(\d+) rep=(\d+) old=(\d+) new=(\d+)"
    samples=re.findall(pattern,content);matrix=timing_matrix()
    if len(samples)!=len(matrix)*9 or len(samples)!=content.count("GS11 TIMING row="):raise RuntimeError("Missing/malformed/extra timing rows")
    rows={}
    for raw in samples:
        row,ci,family,kind,flag,identity,cache,count,rep,old,new=map(int,raw)
        params={"case":ci,"family":family,"kind":kind,"flag":flag,"identity":identity,"cache":cache,"count":count}
        if not 0<=row<len(matrix) or params!=matrix[row] or not 0<=rep<9 or not 0<old<0x80000000 or not 0<new<0x80000000:
            raise RuntimeError("Prescribed row/parameter/repetition/counter differs")
        entry=rows.setdefault(row,dict(params,samples={}))
        if rep in entry["samples"]:raise RuntimeError("Duplicate row/repetition")
        entry["samples"][rep]=[old,new]
    if set(rows)!=set(range(156)) or any(set(r["samples"])!=set(range(9)) for r in rows.values()):raise RuntimeError("Incomplete prescribed rows")
    for entry in rows.values():
        entry["old_median"]=statistics.median(v[0] for v in entry["samples"].values());entry["new_median"]=statistics.median(v[1] for v in entry["samples"].values())
        entry["percent"]=(entry["new_median"]/entry["old_median"]-1)*100
    def total(selected):
        old=sum(r["old_median"] for r in selected);new=sum(r["new_median"] for r in selected)
        return {"rows":len(selected),"old_sum":old,"new_sum":new,"percent":(new/old-1)*100,
                "faster":sum(r["new_median"]<r["old_median"] for r in selected),"ties":sum(r["new_median"]==r["old_median"] for r in selected),
                "slower":sum(r["new_median"]>r["old_median"] for r in selected)}
    values=list(rows.values())
    cohorts={"all":total(values),"aligned_production_swizzle":total([r for r in values if not r["identity"] and not(r["family"]==2 and r["flag"]) and not(r["family"]==1 and r["flag"])]),
             "debug_identity":total([r for r in values if r["identity"]]),"short_single_call":total([r for r in values if r["count"]==1]),
             "warm":total([r for r in values if not r["cache"]]),"cache_flush_requested":total([r for r in values if r["cache"]]),
             "ramp_cache_hit":total([r for r in values if r["family"]==2 and r["flag"]]),"fixed_no_refresh":total([r for r in values if r["family"]==1 and r["flag"]])}
    return {"exact":exact,"rows":rows,"cohorts":cohorts}


def summarize(work,log,host_report,negative=False):
    host=json.loads(host_report.read_text());build=json.loads((work/"build-report.json").read_text())
    if host["fixture_sha256"]!=build["fixture_sha256"] or host["fixture_sha256"]!=sha(TOOLS/"next11_gs_hosttest.c"):raise RuntimeError("Host/native fixture hashes differ")
    outputs=list(host["results"].values())
    if set(host["results"])!={"ordinary","sanitized"} or any(value!=outputs[0] for value in outputs):raise RuntimeError("Host/sanitizer proof incomplete")
    if [r["mutation"] for r in host["controls"]]!=list(range(1,7)) or any("GS NEXT11 FAIL" not in r["result"] for r in host["controls"]):raise RuntimeError("Executed source controls incomplete")
    exacts=re.findall(r"GS NEXT11 EXACT cases=\d+ packets=\d+ digest=[0-9a-f]{8}",outputs[0])
    if len(exacts)!=1:raise RuntimeError("Missing host exact corpus")
    pre=json.loads((work/"run-pre.json").read_text());post=json.loads((work/"run-post.json").read_text())
    if post["rc"] or not post["all_frozen_inputs_unchanged"] or pre["input_sha256"]!=post["input_sha256"] or post["log_sha256"]!=sha(log):raise RuntimeError("Native wrapper/input provenance failed")
    if not post["critical_settings_unchanged"] or not post["emulator_binary_unchanged"] or pre["critical_settings"]!=post["critical_settings"]:raise RuntimeError("Native configuration/binary changed")
    content=log.read_text();report=parse_timing(content,exacts[0]);controls=[]
    if negative:
        first=re.search(r"GS11 TIMING row=0[^\n]*",content)[0]
        changes=[content.replace(first,"",1),content+"\n"+first,content.replace("row=0 case=0","row=0 case=1",1),
                 content.replace("GS NEXT11 DONE","GS NEXT11 INCOMPLETE",1),content+"\nGS NEXT11 FAIL injected\n",
                 content.replace(exacts[0],exacts[0].replace("digest=", "digest=0"),1),content.replace("old=","old=bad",1),content.replace("rep=0","rep=1",1)]
        directory=work/"parser-negative-controls";directory.mkdir(exist_ok=True)
        for i,changed in enumerate(changes,1):
            (directory/(str(i)+".log")).write_text(changed)
            try:parse_timing(changed,exacts[0])
            except RuntimeError as error:controls.append({"mutation":i,"rejected":str(error)})
            else:raise RuntimeError("Executed report-parser corruption escaped")
    report["parser_negative_controls"]=controls
    report["scope"]="Actual full-LTO CLUT builders and complete reached integer/SDK graph; functional fallback tested separately, no fallback-speed or physical-console/FPS percentage claim"
    dump(work/"benchmark-summary.json",report)
    print(json.dumps(report["cohorts"],indent=2))


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out",type=Path,default=ROOT/"build/nov15-next11/gs/host")
    ap.add_argument("--baseline-src",type=Path,default=ROOT/"build/nov15-next11/baseline-src/src")
    ap.add_argument("--current-src",type=Path)
    ap.add_argument("--prototype",action="store_true")
    ap.add_argument("--sanitize",action="store_true")
    ap.add_argument("--negative-controls",action="store_true")
    ap.add_argument("--build",action="store_true")
    ap.add_argument("--bind",action="store_true")
    ap.add_argument("--summarize",type=Path)
    ap.add_argument("--host-report",type=Path,default=ROOT/"build/nov15-next11/gs/host/host-report.json")
    ap.add_argument("--baseline-engine",type=Path,default=ROOT/"build/nov15-next11/baseline/hardware/obj/engine_lto.o")
    ap.add_argument("--current-engine",type=Path,default=ROOT/"build/nov15-next11/gs/candidate-hardware/obj/engine_lto.o")
    ap.add_argument("--baseline-elf",type=Path,default=ROOT/"build/nov15-next11/baseline/hardware/SRB2.ELF")
    ap.add_argument("--current-elf",type=Path,default=ROOT/"build/nov15-next11/gs/candidate-hardware/SRB2.ELF")
    args=ap.parse_args();work=args.out.resolve()
    if args.summarize:
        summarize(work,args.summarize,args.host_report,args.negative_controls);return
    if args.bind:
        bind(work,{"base":args.baseline_engine,"cur":args.current_engine},{"base":args.baseline_elf,"cur":args.current_elf},work/"GS_NEXT11.ELF",args.negative_controls)
        audit_source(work,args.baseline_src,args.current_src or ROOT/"build/nov15-next11/gs/measured-source/src")
        return
    if args.build:
        native_build(work,{"base":args.baseline_engine,"cur":args.current_engine},args.baseline_src,args.current_src or ROOT/"build/nov15-next11/gs/measured-source/src")
        return
    prepare(work,args.baseline_src,args.current_src,args.prototype)
    if args.current_src and not args.prototype: audit_source(work,args.baseline_src,args.current_src)
    results={"ordinary":host_build(work)}
    if args.sanitize: results["sanitized"]=host_build(work,True)
    if any(value != results["ordinary"] for value in results.values()): raise RuntimeError("Sanitizer/ordinary CLUT corpus differs")
    controls=[]
    if args.negative_controls:
        for negative in range(1,7):
            directory=work/("negative-"+str(negative))
            prepare(directory,args.baseline_src,args.current_src,args.prototype,negative)
            result=host_build(directory,expected=1)
            if "GS NEXT11 FAIL" not in result:raise RuntimeError("CLUT source mutation escaped")
            controls.append({"mutation":negative,"result":result.strip()})
    dump(work/"host-report.json",{"fixture_sha256":sha(TOOLS/"next11_gs_hosttest.c"),"results":results,"controls":controls,
         "current_source_root":str(args.current_src) if args.current_src else None,
         "source_sha256":{tag:sha(work/(tag+"-production.c")) for tag in ("base","cur")},"common_sha256":sha(work/"common.h")})
    print(results["ordinary"])


if __name__ == "__main__": main()
