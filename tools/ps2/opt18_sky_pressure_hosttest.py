#!/usr/bin/env python3
"""Check SKY-only pressure admission, protected closures and actual queued reuse."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(HERE/'opt17'))
from tile_host_common import ROOT,prepare,compile_run,replace_once


def function(text,name):
    start=text.index('static void '+name+'(');at=text.index('{',start);depth=0
    for end in range(at,len(text)):
        depth+=(text[end]=='{')-(text[end]=='}')
        if not depth:return text[start:end+1]
    raise RuntimeError('Unterminated wrapper')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--sanitize',action='store_true')
    p.add_argument('--negative',choices=['context','protected','atomic','paired','shared','memo','return','unwind'])
    a=p.parse_args();source,work=a.source_root.resolve(),a.out.resolve();hashes=prepare(source,work)
    base=(work/'base_hosttest.c').read_text().split('int main(int argc, char **argv)')[0]
    base=replace_once(base,'void HWR_PS2_FreeData(void *data) { free(data); }',
        'static unsigned sky_cpu_frees;\nvoid HWR_PS2_FreeData(void *data) { sky_cpu_frees++; free(data); }')
    (work/'base_hosttest.c').write_text(base)
    tex=work/'ps2_hw_tex.inc'
    tex.write_text(replace_once(tex.read_text(),'static void weights_prepare(void)\n{',
        'static unsigned sky_weights_calls;\nstatic void weights_prepare(void)\n{\n    sky_weights_calls++;'))
    driver=(source/'src/ps2/hw/ps2_hwd.c').read_text()
    if driver.count('tile_sky_pressure_reset();')!=2:raise RuntimeError('Init/shutdown reset coverage changed')
    (work/'sky-wrapper.inc').write_text(function(driver,'hw_RenderSkyDome').replace('gl_sky_t','test_sky_t'))
    controls={
        'context':('if (tile_cache.sky_pressure != 1 || tile_cache.sky_failed) return 0;','if (tile_cache.sky_failed) return 0;','world borrow cannot pressure-evict'),
        'protected':('!tile_sky_pressure_allowed(image)','0','protected closure refuses pressure borrow'),
        'atomic':('if (at == victim_n) victims[victim_n++] = image;','if (at == victim_n) {victims[victim_n++] = image; tex_drop(image,1);}','protected mixed-prefix preflight is atomic'),
        'paired':('if (r->pnext != NOREC)','if (0 && r->pnext != NOREC)','protected closure refuses pressure borrow'),
        'shared':('|| r->wgt == WEIGHT_STAY || H.cur_tex == image','|| 0 || H.cur_tex == image','protected closure refuses pressure borrow'),
        'memo':('if (tile_cache.sky_pressure != 1 || tile_cache.sky_failed) return 0;','if (tile_cache.sky_pressure != 1) return 0;','failed prefix memo avoids repeated weight scans'),
        'return':('(void)tile_cache_return_borrow(); // old readers precede ordinary reuse in the same GIF stream','/* deliberately omit end-of-dome return */','end dome restores permanent16KiB before ordinary allocation'),
        'unwind':('tile_sky_pressure_revoke();','/* deliberately skip revoke before external callback */','nonlocal callback sees revoked permission with active renderer'),
    }
    if a.negative:
        path=work/('ps2_hw_priv.inc' if a.negative=='unwind' else 'ps2_hw_tile.inc');old,new,_=controls[a.negative];text=replace_once(path.read_text(),old,new)
        if a.negative=='context':text=replace_once(text,'if (tile_cache.sky_pressure != 1) return 0;','/* deliberately bypass the post-flush permission recheck too */')
        if a.negative=='shared':text=replace_once(text,'|| H.cur_tex == v || (batch_phase == 1','|| 0 || (batch_phase == 1')
        path.write_text(text)
    fixture=HERE/'opt18_sky_pressure_hosttest.c';ran=compile_run(source,work,fixture,a.sanitize)
    output=ran.stdout+ran.stderr
    matched=ran.returncode==0 and 'SKYPRESS COMPLETE' in output
    if a.negative:matched=ran.returncode==1 and controls[a.negative][2] in output
    matched=matched and 'runtime error:' not in output and 'ERROR: AddressSanitizer' not in output
    sha=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
    (work/'result.json').write_text(json.dumps(dict(scope=__doc__,source_root=str(source),source_sha256=hashes,
        driver_sha256=sha(source/'src/ps2/hw/ps2_hwd.c'),fixture_sha256=sha(fixture),runner_sha256=sha(Path(__file__)),
        sanitize=a.sanitize,negative=a.negative,returncode=ran.returncode,matched_expectation=matched,
        emulator_used=False,output=output),indent=2)+'\n')
    return int(not matched)


if __name__=='__main__':raise SystemExit(main())
