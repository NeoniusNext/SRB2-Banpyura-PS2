#!/usr/bin/env python3
"""Run the actual geometry-cache reset against the actual PS2 Zone owner contract."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from flat_child_lifetime_hosttest import function

ROOT=Path(__file__).resolve().parents[3]
HERE=Path(__file__).resolve().parent

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source-root',type=Path,default=ROOT)
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--sanitize',action='store_true')
    ap.add_argument('--expect-orphan',action='store_true',help='require the unfixed reset to reproduce the exact proper-user error')
    a=ap.parse_args();s=a.source_root.resolve();w=a.out.resolve();w.mkdir(parents=True,exist_ok=True)
    text=(s/'src/hardware/hw_gcache.inc').read_text();fn=function(text,'HWR_GCacheLevel')
    code='#include <stdint.h>\ntypedef uint8_t UINT8; static int netgame; int PS2Lua_InCall(void){return 0;} void PS2Spill_Reset(void){}\n#define max(a,b) ((a)>(b)?(a):(b))\n'
    code+='#define ZONE_HOST_SOURCE "'+str(s/'src/z_zone.c')+'"\n#define ZONE_MEM_SOURCE "'+str(s/'src/ps2/ps2_mem.c')+'"\n#define main zone_fixture_main_unused\n#include "'+str(ROOT/'tools/ps2/zone_hosttest.c')+'"\n#undef main\n'
    code+='''
#define GC_DEFAULT_KB 320u
static struct {UINT32 bypass,bypass_len,w_views,w_q,w_hit,w_full;boolean dead;UINT32 tries;void *blk;boolean on,kb_user;UINT32 kb;} gc;
static void *sticky; static unsigned detaches;
static void HWR_PBStDetach(void){sticky=NULL;detaches++;}
'''+fn+(HERE/'gcache_lifetime_hosttest.c').read_text()
    p=w/'test.c';p.write_text(code)
    cmd=['gcc','-std=gnu17','-O2','-g','-fwrapv','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter','-Wno-clobbered','-I'+str(s/'src'),'-I'+str(s/'src/ps2')]
    if a.sanitize:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
    if a.expect_orphan:cmd+=['-DEXPECT_ORPHAN']
    cmd += [str(p),'-o',str(w/'test')]
    (w/'command.json').write_text(json.dumps(cmd,indent=2)+'\n')
    b=subprocess.run(cmd,text=True,capture_output=True);(w/'build.log').write_text(b.stdout+b.stderr)
    if b.returncode:print(b.stdout+b.stderr);return 2
    r=subprocess.run([str(w/'test')],text=True,capture_output=True);o=r.stdout+r.stderr;(w/'test.log').write_text(o);print(o,end='')
    good=r.returncode==0 and 'GC LIFETIME COMPLETE' in o and 'runtime error:' not in o and 'ERROR: AddressSanitizer' not in o
    sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    (w/'result.json').write_text(json.dumps({'passed':good,'returncode':r.returncode,'sanitize':a.sanitize,'expect_orphan':a.expect_orphan,'source_sha256':{str(p.relative_to(s)):sha(p) for p in [s/'src/hardware/hw_gcache.inc',s/'src/z_zone.c',s/'src/ps2/ps2_mem.c']},'actual_reset_function_sha256':hashlib.sha256(fn.encode()).hexdigest(),'runner_sha256':sha(Path(__file__)),'fixture_sha256':sha(HERE/'gcache_lifetime_hosttest.c'),'generated_source_sha256':sha(w/'test.c'),'output':o,'emulator_used':False,'scope':'Actual HWR_GCacheLevel and real Zone; used-field state/attachment service is a fixture boundary, not full geometry replay.'},indent=2)+'\n');return int(not good)

if __name__=='__main__':raise SystemExit(main())
