#!/usr/bin/env python3
"""Read-only source/actual-frame closure for the existing -zreport1 observer."""
import ast
import hashlib
import json
from pathlib import Path
import re
import shutil
import next12_pb_scratch_codecheck as strict

ROOT=Path(__file__).resolve().parents[2]
WORK=ROOT/'build/nov15-next13'

def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def function(text,name):
    m=re.search(r'^.*\b'+re.escape(name)+r'\([^;]*\)\n\{\n',text,re.M)
    if not m:raise RuntimeError('Function source absent:'+name)
    end=text.index('\n}\n',m.end())+3
    return text[m.start():end]

def main():
    out=WORK/'final/ram-observer-binding-v2';out.mkdir(parents=True,exist_ok=False)
    names=['src/ps2/ps2_mem.h','src/ps2/ps2_mem.c','src/z_zone.c','src/p_setup.c','src/d_main.c','src/ps2/i_system.c','src/ps2/ps2_hwfb.c']
    source={n:(ROOT/n).read_text() for n in names}
    assert all(sha(ROOT/n)==sha(ROOT/'build/nov15-next12/final/source'/n) for n in names)
    extracts={}
    for path,functions in {'src/ps2/ps2_mem.c':['PS2Mem_Report','ZA_NoteEvict','ZA_Stats','ZA_ResetPeak','PS2Mem_TagName'],
        'src/z_zone.c':['Z_AdvanceFrame','Z_NextFrame','Z_FreeTags','Z_ReportCosts','Z_FlushCache'],
        'src/ps2/i_system.c':['I_OutputMsg'],'src/ps2/ps2_hwfb.c':['PS2HWFB_Report']}.items():
        for name in functions:extracts[name]={'path':path,'complete_source':function(source[path],name)}
    assert 'char stackbuf[1024]' in extracts['I_OutputMsg']['complete_source']
    assert 'if ((size_t)len >= sizeof stackbuf)' in extracts['I_OutputMsg']['complete_source']
    assert 'fwrite(txt, 1, (size_t)len, stdout)' in extracts['I_OutputMsg']['complete_source']
    assert 'static char hwfb_last[160]' in source['src/ps2/ps2_hwfb.c']
    assert 'PS2Mem_Line("level-exit");\n\t\tZA_ResetPeak();' in extracts['Z_FreeTags']['complete_source']
    assert 'za_peak = za_used;' in extracts['ZA_ResetPeak']['complete_source']
    assert 'PS2Mem_Report(0);' in extracts['Z_AdvanceFrame']['complete_source']
    assert 'Z_AdvanceFrame();\n\tPS2Mem_Frame();' in extracts['Z_NextFrame']['complete_source']
    assert 'Z_NextFrame();' in source['src/d_main.c']
    assert re.findall(r'ZA_NoteEvict\(([^;]+)\);',source['src/z_zone.c'])==['ZA_SIZE(block)','ZA_SIZE(block)','ZA_SIZE(b)','ZA_SIZE(b)','ZA_SIZE(b)']
    formats=[]
    token=re.compile(r'%(?:[-+ #0]*)(\d+)?(?:\.\d+)?(?:ll|l|h|z)?([diuoxXsc%])')
    for name in ('PS2Mem_Report','Z_ReportCosts','PS2HWFB_Report'):
        text=extracts[name]['complete_source']
        assert 'CONS_' not in text and not re.search(r'\b(?:malloc|realloc|calloc|free)\s*\(',text)
        for raw in re.findall(r'I_OutputMsg\(\s*("(?:[^"\\]|\\.)*")',text):
            fmt=ast.literal_eval(raw)
            def replacement(m):
                # Report string args: tag names/string literals are bounded;
                # largest is the NUL-terminated static160-byte fallback text.
                width=int(m[1] or 0)
                return 'S'*max(width,160) if m[2]=='s' else 'P' if m[2]=='%' else 'N'*max(width,11)
            bounded=token.sub(replacement,fmt)
            assert '%' not in bounded.replace('fragmentation ',''),fmt
            assert len(bounded.encode())<1024
            formats.append({'producer':name,'format':fmt,'conservative_max_formatted_bytes_including_newline':len(bounded.encode()),'numeric_domain':'EE32-bit unsigned long/int; width padded; all%s conservatively160 bytes'})
    final_path=WORK/'final/ram-binding-v1/report.json';final=json.loads(final_path.read_text())
    assert final['ELF_sha256']=='7e4764fb609a1b3665936c0594ad4358c4bf1891545f6d4c48da13f5a0e7a9af'
    actual_names=['PS2Mem_Report','Z_NextFrame','D_SRB2Loop','Z_ArenaStart.part.0','Z_FreeTags','P_LoadLevel','ZA_Stats','ZA_InitMem','PS2Mem_Frame','PS2Mem_Line','PS2Mem_Checkpoint','I_OutputMsg','Z_ReportCosts','PS2HWFB_Report','Z_FlushCache','Z_EvictLRU','Z_EvictLRUWalk','Z_MakeRoomWalk','Z_MallocInternal.constprop.0']
    baseline=strict.binder(ROOT/'build/nov15-next12/final/hardware',ROOT/'build/nov15-next12/final/pb-binding/final-linker-relas')
    left={n:baseline.text(n) for n in actual_names}
    right={n:final['complete_actual_all_up_bound_text'][n] for n in actual_names}
    for role,graph in [('baseline',left),('candidate',right)]:
        assert graph['PS2Mem_Report']['exact_frame']['stack_frame_bytes']==3296
        assert graph['I_OutputMsg']['exact_frame']['stack_frame_bytes']==1360
        routes={n:[r['exact_symbol'] for r in row['ordered_RELA_records'] if r['kind']==4] for n,row in graph.items()}
        assert 'PS2Mem_Report' in routes['Z_NextFrame'] and 'PS2Mem_Frame' in routes['Z_NextFrame']
        assert 'Z_NextFrame' in routes['D_SRB2Loop']
        assert 'Z_FlushCache' in routes['P_LoadLevel'] and 'Z_FreeTags' in routes['P_LoadLevel']
        assert 'I_OutputMsg' in routes['PS2Mem_Report'] and not any(n.startswith('CONS_') for n in routes['PS2Mem_Report'])
    graph={'baseline':{'complete_bound_bodies':left,'complete_data':baseline.data,'complete_literals':list(baseline.literals.values()),'all_literal_uses':baseline.literal_uses},
        'candidate':{'complete_bound_bodies':right,'authority':{'path':str(final_path),'sha256':sha(final_path),'full89body_graph_referenced_data_and_literals_in_authority':True}}}
    (out/'actual-graphs.json').write_text(json.dumps(graph,indent=2)+'\n')
    shutil.copyfile(__file__,out/Path(__file__).name)
    report={'status':'PASS source/actual canonical and baseline direct reporting/frame/caller closure; native admission separate',
        'source_sha256':{n:sha(ROOT/n) for n in names},'complete_source_extracts':extracts,'complete_report_format_bounds':formats,
        'actual_graphs':{'path':str(out/'actual-graphs.json'),'sha256':sha(out/'actual-graphs.json'),'bound_bodies_each':len(actual_names)},
        'actual_PS2Mem_Report_frame_bytes':3296,'actual_I_OutputMsg_frame_bytes':1360,
        'caller_scope':'Actual D_SRB2Loop→Z_NextFrame (inlined Z_AdvanceFrame)→PS2Mem_Report then PS2Mem_Frame; arena startup and final report routes also fully bound. P_LoadLevel includes setup; actual Z_FreeTags includes localpeak reset.',
        'counter_scope':'All5 source producer sites pass ZA_SIZE, physical allocated extent including header/rounding/absorbed tail. Actual inlined producer bodies and all count/byte readers bound. Counts reset at InitMem; peakreset does not reset eviction totals. No exactvictim identity or residual cause claim.',
        'observer_scope':'Report and its direct print helpers use I_OutputMsg, no CONS/HUD rendering. All actual reportlines must separately be <1024 in raw checker. Bounded lines avoid I_OutputMsg explicit dynamic-long-line branch; no universal SDK stdio allocation-free assertion. Report3296-byte stack frame and I_OutputMsg1360-byte frame are real; whole highwater remains native-only.',
        'peak_scope':'With-zreport enabled existing Z_FreeTags emits level-exit then ZA_ResetPeak: peak is since-reset; gpeak remains global. No inference from these matched observer runs to old/default-observer stack/peak.',
        'counter_admission_authorities':{str(p):sha(p) for p in [WORK/'review/eviction-accounting-review-v1/report.json',WORK/'review/eviction-policy-v2-classification.json']},
        'tool_sha256':sha(__file__),'no_compiler_linker_or_emulator_invoked':True}
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(str(out/'report.json'),sha(out/'report.json'))

if __name__=='__main__':main()
