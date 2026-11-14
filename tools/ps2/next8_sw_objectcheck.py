"""Audit isolated complete real r_draw.c controls and actual final LTO loops."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
from next8_sw_hosttest import NAMES, function, run
from next6_render_npot_objectcheck import sections, totals, digest

ROOT = Path(__file__).resolve().parents[2]


def audit_source(work, base):
    old = (base / 'r_draw8_npo2.c').read_text(); current = (ROOT / 'src/r_draw8_npo2.c').read_text()
    rebuilt = current
    for name in NAMES:
        rebuilt = rebuilt.replace(function(rebuilt, name), function(old, name))
    if rebuilt != old:
        raise SystemExit('Unexpected source changes outside selected drawer bodies')
    result = {'baseline_sha256': hashlib.sha256(old.encode()).hexdigest(), 'candidate_sha256': hashlib.sha256(current.encode()).hexdigest(),
              'selected_drawer_restore_reconstructs_baseline_byte_for_byte': True,
              'changed_functions': [name for name in NAMES if function(old,name) != function(current,name)],
              'r_draw8_c_equal': (base / 'r_draw8.c').read_bytes() == (ROOT / 'src/r_draw8.c').read_bytes(),
              'r_draw_c_equal': (base / 'r_draw.c').read_bytes() == (ROOT / 'src/r_draw.c').read_bytes()}
    (work / 'source-audit.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def function_info(asm, offsets=None):
    instructions=[]
    for line in asm.splitlines():
        match=re.match(r'^[ \t]*([0-9a-f]+):[ \t]+[0-9a-f]{8}[ \t]+(\w+)[ \t]*(.*)$',line)
        if match:
            data={'address':int(match[1],16),'opcode':match[2],'operands':match[3]}
            gp=re.search(r'(-?\d+)\(gp\)',match[3])
            if gp and offsets and int(gp[1]) in offsets: data['global']=offsets[int(gp[1])]
            instructions.append(data)
        else:
            match=re.search(r'R_MIPS_GPREL16\s+(ds_transmap|ds_flatwidth)\b',line)
            if match and instructions: instructions[-1]['global']=match[1]
    by_address={i['address']:i for i in instructions}
    if not instructions: raise SystemExit('Missing native instructions')
    successors={a:{a+4} & by_address.keys() for a in by_address}
    branches=[]
    for ins in instructions:
        address,op=ins['address'],ins['opcode']
        target=re.search(r'([0-9a-f]+) <',ins['operands'])
        if op=='jr':
            successors[address+4]=set()
        elif target and (op.startswith('b') or op=='j'):
            dest=int(target[1],16)
            successors[address+4]={dest} & by_address.keys()
            if op not in ('b','j'): successors[address+4] |= {address+8} & by_address.keys()
            branches.append((address,dest))
    entry=instructions[0]['address']; reachable=set(); todo=[entry]
    while todo:
        a=todo.pop()
        if a in reachable: continue
        reachable.add(a);todo.extend(successors[a]-reachable)
    predecessors={a:set() for a in reachable}
    for a in reachable:
        for nxt in successors[a]: predecessors[nxt].add(a)
    dominators={a:({entry} if a==entry else set(reachable)) for a in reachable}
    changed=True
    while changed:
        changed=False
        for a in sorted(reachable-{entry}):
            common=set.intersection(*(dominators[p] for p in predecessors[a])) if predecessors[a] else set()
            current={a}|common
            if current!=dominators[a]: dominators[a]=current;changed=True
    loops=[]
    for end,begin in branches:
        if begin>=end or end not in reachable or begin not in dominators[end]: continue
        # A natural-loop back edge requires its header to dominate the branch.
        # Cold jumps to earlier hot code are not loops and must not pull in
        # unrelated prologue/epilogue instructions as supposed per-pixel work.
        members={begin,end,end+4};todo=[end]
        while todo:
            a=todo.pop()
            for prev in predecessors[a]:
                if prev not in members: members.add(prev);todo.append(prev)
        inside=[by_address[a] for a in sorted(members) if a in by_address]
        if not any(i['opcode']=='sb' for i in inside): continue
        loops.append({'begin':hex(begin),'end':hex(end),'instruction_count':len(inside),
                      'parameter_global_loads':[i for i in inside if 'global' in i and i['opcode'] in ('lw','lhu')],
                      'stack_accesses':[i for i in inside if '(sp)' in i['operands']]})
    frame=re.search(r'addiu\s+sp,sp,(-\d+)',asm)
    return {'stack_frame':-int(frame[1]) if frame else 0,'pixel_loops':loops,
            'stack_accesses':[i for i in instructions if '(sp)' in i['operands']],
            'calls':[i for i in instructions if i['opcode'] in ('jal','jalr')]}


def asm_report(work, obj, variant, linked=False):
    offsets = None
    if linked:
        symbols = run(['mips64r5900el-ps2-elf-nm', '-n', obj], work / (variant + '-nm.log')).stdout
        wanted = {}
        gp = None
        for line in symbols.splitlines():
            m = re.match(r'([0-9a-f]+)\s+\w\s+(\S+)', line)
            if not m:
                continue
            if m[2] == '_gp': gp = int(m[1],16)
            if m[2] in ('ds_transmap','ds_flatwidth'): wanted[m[2]] = int(m[1],16)
        if gp is None or len(wanted) != 2:
            raise SystemExit('Missing linked gp/global symbols')
        offsets = {address-gp:name for name,address in wanted.items()}
    result = {}
    for name in NAMES:
        command = ['mips64r5900el-ps2-elf-objdump', '-d' if linked else '-dr']
        if not linked:
            command += ['-j', '.text.' + name]
        else:
            command += ['--disassemble=' + name]
        asm = run([*command,obj],work/(variant+'-'+name+'.asm')).stdout
        data = function_info(asm, offsets)
        if not data['pixel_loops']:
            raise SystemExit('No actual pixel loops: ' + name)
        for loop in data['pixel_loops']:
            loop['blend_pointer_loads'] = [v for v in loop['parameter_global_loads'] if v.get('global') == 'ds_transmap']
        data['blend_pointer_static_loads'] = sum(1 for line in asm.splitlines() if 'R_MIPS_GPREL16\tds_transmap' in line)
        result[name] = data
    return result


def compile_objects(work, base):
    import build as B
    flags = [f for f in B.CFLAGS if not f.startswith('-MMD') and f not in ('-MP','-flto')]
    report, objects = {}, {}
    for variant, src in [('reference',base),('candidate',ROOT/'src')]:
        folder = work / variant; folder.mkdir(exist_ok=True)
        for name in ('r_draw.c','r_draw8.c','r_draw8_npo2.c'):
            shutil.copyfile(src/name,folder/name)
        # Use each actual source tree's complete headers; the two controls
        # differ only in the four drawer bodies under review.
        incs = ['-I'+str(src)] + B.INCS
        for policy, extra in [('profile',[]),('noopt-draw',['-DPS2_NOOPT_DRAW'])]:
            key = policy+'-'+variant; obj=work/(key+'.o')
            run([B.CC,*flags,*extra,'-Werror',*incs,'-c',folder/'r_draw.c','-o',obj],work/(key+'-build.log'))
            info=sections(obj,work); objects[key]=info
            data=totals(info)
            if policy=='profile':
                data['functions']=asm_report(work,obj,key)
                for name in NAMES: data['functions'][name]['text_bytes']=info['.text.'+name][0]
            else:
                output=run([B.CC,*flags,*extra,*incs,'-E','-P',folder/'r_draw.c'],work/(variant+'-noopt.i')).stdout
                tokens=re.sub(r'\s+','',''.join(function(output,name) for name in NAMES))
                data['preprocessed_drawers_sha256']=hashlib.sha256(tokens.encode()).hexdigest()
                data['code_sha256']=digest(info)
            report[key]=data
    for key in ('preprocessed_drawers_sha256','code_sha256'):
        if report['noopt-draw-reference'][key] != report['noopt-draw-candidate'][key]:
            raise SystemExit('Disabled policy differs: '+key)
    before,after=objects['profile-reference'],objects['profile-candidate']
    active=json.loads((work/'source-audit.json').read_text())['changed_functions']
    names=before.keys()|after.keys()
    changed={n for n in names if n.startswith('.text') and before.get(n)!=after.get(n)}
    if changed != {'.text.'+n for n in active}:
        raise SystemExit('Unexpected changed object text: '+str(sorted(changed)))
    bss=[{n:s for n,(s,_) in obj.items() if n.startswith(('.bss','.sbss'))} for obj in (before,after)]
    if bss[0]!=bss[1]: raise SystemExit('BSS differs')
    ro=[Counter((s,hashlib.sha256(d).hexdigest()) for n,(s,d) in obj.items() if n.startswith('.rodata')) for obj in (before,after)]
    if ro[0]!=ro[1]: raise SystemExit('rodata content differs')
    if any(before.get(n)!=after.get(n) for n in names if n.startswith(('.data','.sdata'))): raise SystemExit('Data differs')
    report['controls']={'only_selected_drawer_text_sections_changed':sorted(changed),'data_rodata_bss_unchanged':True,'noopt_draw_identical':True}
    for name,data in report['profile-candidate']['functions'].items():
        if name not in active: continue
        counts=sorted(len(loop['blend_pointer_loads']) for loop in data['pixel_loops'])
        if counts != [0,1]:
            raise SystemExit('Expected one retained fallback load and zero bounded-loop loads: '+name+' '+str(counts))
    (work/'object-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k=='controls'},indent=2))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',required=True,type=Path)
    p.add_argument('--base-src',required=True,type=Path)
    p.add_argument('--compile',action='store_true')
    p.add_argument('--baseline-engine',type=Path)
    p.add_argument('--candidate-engine',type=Path)
    p.add_argument('--candidate-elf',type=Path)
    a=p.parse_args();work=a.out.resolve();work.mkdir(parents=True,exist_ok=True);base=a.base_src.resolve()
    source_audit=audit_source(work,base)
    if a.compile: compile_objects(work,base)
    linked={}
    for variant,obj in [('reference',a.baseline_engine),('candidate',a.candidate_engine),('candidate-linked',a.candidate_elf)]:
        if obj:
            linked[variant]={'file':str(obj.resolve()),'sha256':hashlib.sha256(obj.read_bytes()).hexdigest(),
                             'functions':asm_report(work,obj.resolve(),variant,variant.endswith('linked'))}
    if linked:
        (work/'native-report.json').write_text(json.dumps(linked,indent=2)+'\n')
        for variant,data in linked.items():
            if variant.startswith('candidate'):
                for name,body in data['functions'].items():
                    if name not in source_audit['changed_functions']: continue
                    counts=sorted(len(loop['blend_pointer_loads']) for loop in body['pixel_loops'])
                    if counts != [0,1]:
                        raise SystemExit('Expected native fallback/bounded blend loads [0,1]: '+name+' '+str(counts))
    print('SW source/object/native audit passed')


if __name__=='__main__': main()
