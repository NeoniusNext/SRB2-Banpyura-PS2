"""Complete composite generator in the real arena, including every retry failure."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import subprocess
import zipfile
ROOT=Path(__file__).resolve().parents[2]
TOOLS=ROOT/'tools/ps2'

def section(s,a,b):
    i=s.index(a);return s[i:s.index(b,i)]

def parts(base):
    rd=(base/'r_defs.h').read_text(); rt=(base/'r_textures.h').read_text(); rp=(base/'r_patch.c').read_text(); tex=(base/'r_textures.c').read_text()
    types=section(rd,'// posts are runs of non masked source pixels','//\n//')
    types+=section(rd,'typedef struct\n{\n\tINT16 width, height;','// Possible alpha types for a patch.')
    types+=section(rt,'typedef struct\n{\n\t// Block origin','// all loaded and prepared textures')
    patch=section(rp,'patch_t *Patch_Create(','#ifdef PS2_PROFILE')
    patch+=section(rp,'#define PATCH_ALIGN','// The zone may drop an embedded patch')
    patch+=section(rp,'void Patch_CalcDataSizes(','//\n// Frees patches with a tag range.')
    renderer=section(tex,'INT32 numtextures = 0;','// Painfully simple texture id')
    renderer+=section(tex,'static void R_DrawColumnInCache(','#if defined(PS2) && defined(PS2_PROFILE)\n// PS2-180')
    policy='\n'.join(line for line in rd.splitlines() if re.match(r'^#define TEXTURE_255_IS_TRANSPARENT\b',line))+'\n'
    assert policy.strip()=='#define TEXTURE_255_IS_TRANSPARENT'
    policy+='#ifdef RETRY_COUNTED_PROFILE\n#undef TEXTURE_255_IS_TRANSPARENT\n#endif\n'
    return {'RETRY_POLICY':policy,'RETRY_TYPES':types,'RETRY_PATCH':patch,'RETRY_RENDERER':renderer}

def run(cmd,log,negative=False):
    p=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,cwd=ROOT)
    log.write_text(' '.join(map(str,cmd))+'\n'+p.stdout+p.stderr)
    if p.returncode and not negative: raise SystemExit(p.stdout+p.stderr+'\n'+str(log))
    return p

def snapshots(path):
    data=path.read_bytes(); records=[]; pos=0
    while pos<len(data):
        start=pos; fallback,w,h=struct.unpack_from('<III',data,pos);pos+=12
        if not fallback:
            pos+=8+w*h
            for _ in range(w):
                posts=struct.unpack_from('<I',data,pos)[0];pos+=4+12*posts
        assert pos<=len(data)
        records.append(data[start:pos])
    assert pos==len(data)
    return records

def cerail_oracle(sources,counted):
    width,height=256,2048; pixels=bytearray([0 if counted else 255])*(width*height)
    placements=[(i,0,i*256) for i in range(8)]+[(8,i*64,0) for i in range(4)]
    for n,ox,oy in placements:
        d=sources[n];w,h=struct.unpack_from('<hh',d)
        for x in range(w):
            off=struct.unpack_from('<I',d,8+4*x)[0];previous=-1
            while d[off]!=255:
                top,length=d[off],d[off+1];top=top+previous if top<=previous else top;previous=top
                for y in range(length):
                    xx,yy=x+ox,top+y+oy
                    if 0<=xx<width and 0<=yy<height:pixels[xx*height+yy]=d[off+3+y]
                off+=length+4
    return struct.pack('<5I',0,width,height,int(counted),0)+pixels+b''.join(struct.pack('<4I',1,0,height,0) for _ in range(width))

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',required=True,type=Path);p.add_argument('--base-src',required=True,type=Path);p.add_argument('--sanitize',action='store_true');p.add_argument('--counted',action='store_true');p.add_argument('--negative-controls',action='store_true');p.add_argument('--mutation',choices=['bound','capacity','counter','cleanup','pixel']);a=p.parse_args()
    out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
    fixture=out/'fixtures';fixture.mkdir(exist_ok=True)
    with zipfile.ZipFile('/workspace/.onboarding/srb2-assets/srb2.pk3') as z:
        names=['CESTONE'+str(i) for i in range(2,10)]+['CERAIL']; data=[z.read(next(f for f in z.namelist() if Path(f).stem.upper()==n)) for n in names]
    (fixture/'resources.bin').write_bytes(struct.pack('<I',len(data))+b''.join(struct.pack('<I',len(d))+d for d in data))
    outputs={}; reports={}
    for name,base,extra in [('baseline',a.base_src,[]),('candidate',ROOT/'src',['-DRETRY_EXPECT_RETRY']),('old-switch',ROOT/'src',['-DPS2_NOOPT_TEXRETRY'])]:
        work=out/name;work.mkdir(exist_ok=True);defs=[]
        extra=extra+(['-DRETRY_COUNTED_PROFILE'] if a.counted else [])
        for macro,content in parts(base).items():
            if name=='candidate' and macro=='RETRY_RENDERER' and a.mutation:
                edits={'bound':('if (!composite_retried && retry_eligible)','if (retry_eligible)'),
                       'capacity':('retry_capacity = blocksize;','retry_capacity = total_pixels;'),
                       'counter':('texturememory -= retry_capacity;','texturememory -= 0;'),
                       'cleanup':('\t\t\ttemp_posts = NULL;','\t\t\t/* omitted required reset */'),
                       'pixel':('post.topdelta = source[0];','post.topdelta = source[0] + 1;')}
                old,new=edits[a.mutation];assert content.count(old)==1,(a.mutation,content.count(old));content=content.replace(old,new,1)
            dest=work/(macro.lower()+'.inc');dest.write_text(content);defs+=['-D'+macro+'="'+str(dest)+'"']
        binary=work/'test'
        flags=['cc','-std=gnu17','-O2','-g','-Wall','-Wextra','-Wno-unused-function','-Wno-clobbered','-Wno-sign-compare','-Wno-unused-variable','-fwrapv','-DPARANOIA','-I'+str(ROOT/'src'),'-I'+str(TOOLS),*defs,*extra]
        run([*flags,TOOLS/'next7_texture_retry_hosttest.c','-o',binary],work/'build.log')
        result=run([binary,fixture,work/'snapshots.bin'],work/'run.log');outputs[name]=result.stdout
        rows=[dict(re.findall(r'(\w+)=([\da-f]+)',line)) for line in result.stdout.splitlines() if line.startswith('RETRY case=')]
        for bad in range(1,5):
            invalid=run([binary,fixture,work/('invalid'+str(bad)+'.bin'),str(bad)],work/('invalid'+str(bad)+'.log'),True)
            assert invalid.returncode==1 and 'original lump validation' in invalid.stderr,(name,bad,invalid.stderr)
        reports[name]={'rows':rows,'source_sha256':hashlib.sha256((base/'r_textures.c').read_bytes()).hexdigest()}
        print(name,len(rows),result.stdout.splitlines()[-1],flush=True)
    before,after,old=(reports[n]['rows'] for n in ('baseline','candidate','old-switch'))
    assert before==old,'opt-out must retain every baseline allocation/state/output result'
    assert len(before)==len(after)==135
    for b,c in zip(before,after):
        if c['case']=='5':
            assert b['fallback']=='1' and c['fallback']=='0' and c['finals']=='2' and c['hash']==before[0]['hash'],('natural fragmentation witness',b,c)
        elif c['fail']=='0': assert b==c,('normal-success allocation/output/counter differs',b,c)
        elif c['case'] in ('2','3','4'): assert b==c,('ineligible path changed',b,c)
        else:
            assert int(c['finals'])<=2
            reference=next(r for r in before if r['case']==c['case'] and r['fail']=='0')
            assert c['bytes']==reference['bytes'],('memory counter charged once',c)
            if c['case'] in ('0','1') and c['fail']=='1' and c['afail']=='0' and c['rfail']=='0' and c['change']=='0': assert c['fallback']=='0' and c['finals']=='2'
            if c['case']=='0' and (1<=int(c['afail'])<=17 or 1<=int(c['rfail'])<=24 or int(c['change'])): assert c['fallback']=='1'
            if c['fallback']=='0': assert c['hash']==reference['hash'],('retry output differs',c)
    bs=snapshots(out/'baseline/snapshots.bin');cs=snapshots(out/'candidate/snapshots.bin');os=snapshots(out/'old-switch/snapshots.bin')
    assert bs==os and len(bs)==len(before)
    assert bs[0]==cerail_oracle(data,a.counted),'independent real-patch pixel/post oracle'
    compared=0
    for row,blob in zip(after,cs):
        if row['fallback']=='0':
            case='0' if row['case']=='5' else row['case']
            reference=next(i for i,r in enumerate(before) if r['case']==case and r['fail']=='0')
            assert blob==bs[reference],('complete normalized byte difference',row)
            compared+=len(blob)
    reports['complete_compared_bytes']=compared
    reports['independent_cerail_pixels_posts_match']=True
    reports['assertions']={'normal_success_outputs_allocation_sequence_and_counters_identical':True,'old_switch_all_rows_identical':True,'bounded_retry':True}
    if a.sanitize:
        work=out/'candidate';binary=work/'asan';defs=['-D'+m+'="'+str(work/(m.lower()+'.inc'))+'"' for m in parts(ROOT/'src')]
        flags=['cc','-std=gnu17','-O1','-g','-fwrapv','-DPARANOIA','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-I'+str(ROOT/'src'),'-I'+str(TOOLS),*defs,*(['-DRETRY_COUNTED_PROFILE'] if a.counted else [])]
        run([*flags,TOOLS/'next7_texture_retry_hosttest.c','-o',binary],work/'asan-build.log');r=run([binary,fixture,work/'asan-snapshots.bin'],work/'asan-run.log')
        assert 'runtime error:' not in r.stderr and r.stdout==outputs['candidate']
        assert (work/'asan-snapshots.bin').read_bytes()==(work/'snapshots.bin').read_bytes()
        reports['asan']=True;print('asan PASS',flush=True)
    if a.negative_controls:
        negative=[]
        for mutation in ('bound','capacity','counter','cleanup','pixel'):
            work=out/('negative-'+mutation)
            cmd=[sys.executable,'-B',str(Path(__file__)), '--base-src',str(a.base_src.resolve()),'--out',str(work),'--mutation',mutation]
            if a.counted:cmd+=['--counted']
            bad=run(cmd,out/('negative-'+mutation+'.log'),True)
            assert bad.returncode and (work/'candidate/test').is_file() and (work/'candidate/run.log').is_file(),(mutation,bad.stdout,bad.stderr)
            assert 'error:' not in (work/'candidate/build.log').read_text(),mutation+' failed to compile'
            assert 'RETRY case=' in (work/'candidate/run.log').read_text(),mutation+' did not execute actual mutation'
            negative.append(mutation)
        reports['negative_controls_detected']=negative;print('negative controls PASS: '+','.join(negative),flush=True)
    (out/'report.json').write_text(json.dumps(reports,indent=2)+'\n')

if __name__=='__main__':main()
