"""Build an actual EE32 real-zone retry witness using frozen roomy byte oracles."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
from next7_texture_retry_hosttest import ROOT,TOOLS,parts,snapshots,run

def array(name,data):
    return 'static const unsigned char '+name+'[]={\n'+',\n'.join(','.join(map(str,data[i:i+32])) for i in range(0,len(data),32))+'\n};\n'

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',required=True,type=Path);p.add_argument('--host-proof',required=True,type=Path);a=p.parse_args()
    out=a.out.resolve();out.mkdir(parents=True,exist_ok=True);host=a.host_proof.resolve()
    proof=json.loads((host/'report.json').read_text());rows=proof['baseline']['rows'];blobs=snapshots(host/'baseline/snapshots.bin')
    data=(host/'fixtures/resources.bin').read_bytes();n=struct.unpack_from('<I',data)[0];pos=4;sources=[]
    for _ in range(n):size=struct.unpack_from('<I',data,pos)[0];pos+=4;sources.append(data[pos:pos+size]);pos+=size
    header=''.join(array('ee_source'+str(i),d) for i,d in enumerate(sources))
    header+='#define EE_RESOURCE_COUNT '+str(n)+'\nstatic const unsigned char *const ee_sources[]={'+','.join('ee_source'+str(i) for i in range(n))+'};\n'
    header+='static const size_t ee_source_sizes[]={'+','.join(map(lambda d:str(len(d)),sources))+'};\n'
    cases={int(row['case']):blob for row,blob in zip(rows,blobs) if row['fail']=='0' and row['fallback']=='0'};cases[5]=cases[0]
    unique={};names={}
    for k,blob in sorted(cases.items()):
        h=hashlib.sha256(blob).hexdigest()
        if h not in unique:
            name='ee_reference'+str(len(unique));unique[h]=name;header+=array(name,blob)
        names[k]=unique[h]
    header+='static const unsigned char *const ee_expected[14]={'+','.join(names.get(i,'NULL') for i in range(14))+'};\n'
    header+='static const size_t ee_expected_sizes[14]={'+','.join(str(len(cases[i])) if i in cases else '0' for i in range(14))+'};\n'
    (out/'retry_ee_data.h').write_text(header)
    definitions=[]
    for macro,content in parts(ROOT/'src').items():
        dest=out/(macro.lower()+'.inc');dest.write_text(content);definitions+=['-D'+macro+'="'+str(dest)+'"']
    import build as B
    flags=[f for f in B.CFLAGS if f not in ('-flto','-MMD','-MP','-D_EE')]
    obj=out/'driver.o'
    run([B.CC,*flags,'-U_EE','-DNEXT7_RETRY_EE','-DRETRY_EXPECT_RETRY','-DPARANOIA','-Wno-unused-function','-Wno-unused-variable','-Wno-sign-compare','-Wno-clobbered','-I'+str(TOOLS),'-I'+str(out),*B.INCS,*definitions,'-c',TOOLS/'next7_texture_retry_hosttest.c','-o',obj],out/'build.log')
    elf=out/'RETRY.ELF';run([B.CC,*B.LDFLAGS,obj,'-o',elf,'-ldebug','-lpatches','-lm'],out/'link.log')
    report={'elf_sha256':hashlib.sha256(elf.read_bytes()).hexdigest(),'source_sha256':hashlib.sha256((ROOT/'src/r_textures.c').read_bytes()).hexdigest(),'oracle_proof':str(host),'expected_records':{k:hashlib.sha256(b).hexdigest() for k,b in cases.items()},'scope':'Actual EE32 allocator/generator functions with platform and pack-read boundaries stubbed; byte oracle is frozen roomy generator plus independent CERAIL composition; full-engine stress is separate'}
    (out/'inputs.json').write_text(json.dumps(report,indent=2)+'\n');print(elf);print(json.dumps(report,indent=2))
if __name__=='__main__':main()
