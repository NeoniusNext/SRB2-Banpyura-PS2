#!/usr/bin/env python3
"""Read-only stock patch/flat compression census with literal-source C oracle.

Use the assets virtualenv (lz4). Outputs are isolated under build/nov17-*.
No game, emulator, production edits, quality-policy changes or VRAM saving claim.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zlib

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))
import cook
import lz4.block
from patch_rescue_hosttest import extract
from next13_flats_cut_hosttest import function, section

MARKER = b'\x89SRPIC\r\n'
BLOCK32 = ((0,1,4,5,16,17,20,21),(2,3,6,7,18,19,22,23),(8,9,12,13,24,25,28,29),(10,11,14,15,26,27,30,31))
BLOCK16 = ((0,2,8,10),(1,3,9,11),(4,6,12,14),(5,7,13,15),(16,18,24,26),(17,19,25,27),(20,22,28,30),(21,23,29,31))


def sha(data):
    return hashlib.sha256(data).hexdigest()


def align(n, a):
    return (n+a-1)//a*a


def footprint(fmt, w, h):
    x, y = w-1, h-1
    if fmt == 'P8':
        tbw = (w+127)//128*2
        return ((y>>6)*((tbw+1)>>1)+(x>>7))*32+BLOCK32[(y>>4)&3][(x>>4)&7]+1
    if fmt == 'P4':
        tbw = (w+127)//128*2
        return ((y>>7)*((tbw+1)>>1)+(x>>7))*32+BLOCK16[(y>>4)&7][(x>>5)&3]+1
    tbw = (w+63)//64
    return ((y>>5)*tbw+(x>>6))*32+BLOCK32[(y>>3)&3][(x>>3)&7]+1


def packed_size(data):
    """Explicit 8-byte per-buffer model header; retain raw if it is smaller."""
    c = lz4.block.compress(data, mode='high_compression', compression=12, store_size=False)
    assert lz4.block.decompress(c, uncompressed_size=len(data)) == data
    return min(len(data), len(c)+8)


def decode_patch(data):
    """Validate full posts, then mirror literal Patch_MakeColumns order."""
    if len(data)<13:
        return None
    w,h,left,top = struct.unpack_from('<hhhh', data)
    if not (0<w<=16384 and 0<h<=16384) or 8+4*w>len(data):
        return None
    offsets = struct.unpack_from('<%dI'%w, data, 8)
    if any(p<8+4*w or p>=len(data) for p in offsets):
        return None
    columns, posts, chunks = [], [], []
    index = bytearray(b'\xff')*(w*h)
    alpha = bytearray(w*h)
    pixel_count = 0
    try:
        for x,p in enumerate(offsets):
            first_post, first_pixel, prev, colbytes = len(posts), pixel_count, 0, 0
            while data[p] != 255:
                delta,n = data[p],data[p+1]
                pos = delta+prev if delta<=prev else delta
                prev = pos
                if p+4+n>len(data) or pos+n>h:
                    return None
                pixels = data[p+3:p+3+n]
                posts.append((pos,n,colbytes))
                chunks.append(pixels)
                index[pos*w+x:(pos+n)*w+x:w] = pixels
                alpha[pos*w+x:(pos+n)*w+x:w] = b'\xff'*n
                pixel_count += n
                colbytes += n
                p += n+4
            columns.append((len(posts)-first_post,first_post,first_pixel))
    except (IndexError, ValueError):
        return None
    pixels = b''.join(chunks)
    metadata = b''.join(struct.pack('<III',*c) for c in columns+posts)
    pw,ph = 1<<(w-1).bit_length(),1<<(h-1).bit_length()
    ap88 = bytearray(b'\xff\x00')*(pw*ph)
    for y in range(h):
        off = y*pw*2
        ap88[off:off+w*2:2] = index[y*w:(y+1)*w]
        ap88[off+1:off+w*2+1:2] = alpha[y*w:(y+1)*w]
    return dict(w=w,h=h,left=left,top=top,pw=pw,ph=ph,columns=columns,
                posts=posts,pixels=pixels,metadata=metadata,index=index,alpha=alpha,ap88=ap88)


def image_metrics(d, pal):
    w,h,pw,ph = (d[k] for k in ('w','h','pw','ph'))
    holes = d['alpha'].count(0)
    # A transparent stock background is always index255. Only post pixels are opaque.
    opaque = set(d['index'])
    opaque255 = d['index'].count(255)>holes
    if not opaque255:
        opaque.discard(255)
    basefmt = 'CT32' if holes and opaque255 else 'P8'
    output = set(d['index'])
    p4_band = next(iter(output))>>4 if len({i>>4 for i in output})==1 else None
    rgba = {pal[i]+b'\xff' for i in opaque}
    if holes:
        rgba.add(pal[255]+b'\x00')
    states = len(opaque)+bool(holes)
    direct = w<=1024 and h<=1024 and w%8==0
    # Prove the tight byte representation against ALL modeled AP88 bytes.
    # No-hole images may keep opaque255. Padding alpha is implicit in the
    # already stored real extent. Binary holes use255 only without opaque255.
    if not (holes and opaque255):
        recovered=bytearray(b'\xff\x00')*(pw*ph)
        amap=bytes(0 if i==255 and holes else 255 for i in range(256))
        for y in range(h):
            off=y*pw*2;idx=d['index'][y*w:(y+1)*w]
            recovered[off:off+w*2:2]=idx
            recovered[off+1:off+w*2+1:2]=idx.translate(amap)
        assert recovered==d['ap88'], 'tight P8 model loses AP88 data'
    row = {
        'width':w,'height':h,'left':d['left'],'top':d['top'],'pow2_width':pw,'pow2_height':ph,
        'area':w*h,'opaque_pixels':w*h-holes,'holes':holes,'opaque255':opaque255,
        'opaque_palette_indices':len(opaque),'index_alpha_states':states,'rgba_colors':len(rgba),
        'canonical_pixels':len(d['pixels']),'canonical_posts':len(d['posts']),
        'canonical_columns':len(d['columns']),'max_post_top':max((p[0] for p in d['posts']),default=0),
        'max_post_length':max((p[1] for p in d['posts']),default=0),
        'max_post_data_offset':max((p[2] for p in d['posts']),default=0),
        'max_column_pixels':max((sum(p[1] for p in d['posts'][c[1]:c[1]+c[0]]) for c in d['columns']),default=0),
        'canonical_ee_bytes':align(48+12*w+12*len(d['posts']),16)+len(d['pixels']),
        'canonical_pixels_lz4_model_bytes':packed_size(d['pixels']),
        'canonical_post8_model_ee_bytes':align(48+12*w+8*len(d['posts']),16)+len(d['pixels']),
        'canonical_post6_model_ee_bytes':align(48+12*w+6*len(d['posts']),16)+len(d['pixels']),
        'ap88_pow2_bytes':len(d['ap88']),'ap88_cropped_bytes':w*h*2,
        'ap88_lz4_model_bytes':packed_size(d['ap88']),
        'cpu_p8_exact_eligible':not(holes and opaque255),
        'cpu_p8_or_p9_model_bytes':pw*ph+(align(pw*ph,8)//8 if holes and opaque255 else 0),
        'cpu_tight_p8_or_p9_model_bytes':w*h+(align(w*h,8)//8 if holes and opaque255 else 0),
        'cpu_tight_always_coverage_model_bytes':w*h+align(w*h,8)//8,
        'gs_fullsize_legal':w<=1024 and h<=1024,
        'current_native_format':basefmt,'current_native_payload':w*h*(4 if basefmt=='CT32' else 1),
        'current_native_span_bytes':footprint(basefmt,w,h)*256,
        'shared_band':p4_band,'p4_transfer_legal':direct,
        'shared_p4_eligible':direct and basefmt=='P8' and p4_band is not None,
        'custom_p4_index_states_eligible':direct and states<=16,
        'custom_p4_rgba_colors_eligible':direct and len(rgba)<=16,
        'custom_p8_index_states_eligible':states<=256,
        'custom_p8_rgba_colors_eligible':len(rgba)<=256,
        'p4_span_bytes':footprint('P4',w,h)*256,'p8_span_bytes':footprint('P8',w,h)*256,
        'image_sha256':sha(struct.pack('<II',w,h)+d['index']+d['alpha']),
        'canonical_sha256':sha(struct.pack('<hhhh',w,h,d['left'],d['top'])+d['metadata']+d['pixels']),
    }
    return row


ORACLE_MAIN = r'''
static u32 crc_field(u32 crc,u32 v){return (u32)crc32(crc,(const Bytef*)&v,4);}
int main(int argc,char **argv){
 check(argc==2,"corpus path");FILE*f=fopen(argv[1],"rb");check(f!=NULL,"open corpus");
 unsigned char magic[8];check(fread(magic,8,1,f)==1&&!memcmp(magic,"OPT17SPR",8),"corpus marker");
 u32 count;check(fread(&count,4,1,f)==1,"corpus count");u8 rgb[768];check(fread(rgb,768,1,f)==1,"palette");
 for(u32 i=0;i<256;i++){H.pal[i]=rgb[i*3]|((u32)rgb[i*3+1]<<8)|((u32)rgb[i*3+2]<<16);palette[i].rgba=H.pal[i]|0xff000000u;}
 unsigned long long bytes=0,texels=0,posts_all=0,pixels_all=0;
 for(u32 r=0;r<count;r++){
  u32 v[13];check(fread(v,sizeof v,1,f)==1,"case header");
  u8*raw=malloc(v[0]);check(raw&&fread(raw,v[0],1,f)==1,"complete patch");
  check(Picture_CheckIfDoomPatch((softwarepatch_t*)raw,v[0]),"actual patch check");
  size_t np=0,ns=0;Patch_CalcDataSizes((softwarepatch_t*)raw,&np,&ns);
  check(np==v[4]&&ns==v[5],"canonical counts");
  patch_t*p=Patch_CreateFromDoomPatch((softwarepatch_t*)raw);free(raw);
#ifdef OPT17_CORRUPT_POST
  if(ns)p->posts[0].topdelta^=1;
#endif
  check(p->width==v[6]&&p->height==v[7],"actual dimensions");
  check(crc32(0,p->pixels,(uInt)np)==v[2],"entire canonical pixel stream");
  u32 mc=0;
  for(unsigned x=0;x<(unsigned)p->width;x++){
   column_t*c=&p->columns[x];mc=crc_field(mc,c->num_posts);mc=crc_field(mc,(u32)(c->posts-p->posts));mc=crc_field(mc,(u32)(c->pixels-p->pixels));
  }
  for(size_t i=0;i<ns;i++){mc=crc_field(mc,p->posts[i].topdelta);mc=crc_field(mc,p->posts[i].length);mc=crc_field(mc,(u32)p->posts[i].data_offset);}
  check(mc==v[3],"every normalized column and post field");
  GLPatch_t g={0};GLMipmap_t m={0};HWR_MakePatch(p,&g,&m,true);
#ifdef OPT17_CORRUPT_AP88
  ((u8*)m.data)[0]^=1;
#endif
  size_t n=(size_t)m.width*m.height*2;
  check(crc32(0,m.data,(uInt)n)==v[1],"entire actual AP88 bitmap");
  int partial=0,hole=0,opaque255=0,hole_rgb=0;
  ap88_scan(m.data,m.width,(u32)p->width,(u32)p->height,H.pal[255],0,&partial,&hole,&opaque255,&hole_rgb);
  check(!partial&&!hole_rgb&&hole==(int)v[8]&&opaque255==(int)v[9],"literal upload classification");
  int p2=0,h2=0,o2=0,r2=0;
  if(!(m.width&3)){ap88_scan(m.data,m.width,(u32)p->width,(u32)p->height,H.pal[255],1,&p2,&h2,&o2,&r2);check(p2==partial&&h2==hole&&o2==opaque255&&r2==hole_rgb,"fast scan equivalence");}
  u32 w=(u32)p->width,h=(u32)p->height;
  check(tex_footprint(PSM_T8,w,h,((w+127)/128)*2)==v[10],"P8 physical range");
  check(tex_footprint(PSM_CT32,w,h,(w+63)/64)==v[11],"CT32 physical range");
  check(tex_footprint(PSM_T4,w,h,((w+127)/128)*2)==v[12],"P4 physical range");
  bytes+=n;texels+=(unsigned long long)w*h;posts_all+=ns;pixels_all+=np;
  Z_Free(m.data);Patch_Free(p);
 }
 check(fgetc(f)==EOF,"no unvalidated corpus tail");fclose(f);
 for(size_t i=0;i<block_high;i++)check(!blocks[i].live,"no oracle leaks");
 printf("{\"patches\":%u,\"compared_ap88_bytes\":%llu,\"cropped_texels\":%llu,\"canonical_posts\":%llu,\"canonical_pixels\":%llu,\"checks\":%u}\n",count,bytes,texels,posts_all,pixels_all,checks);return 0;
}
'''


def oracle(source, corpus, out, sanitize, negative_controls):
    tex=(source/'src/ps2/hw/ps2_hw_tex.inc').read_text()
    pic=(source/'src/r_picformats.c').read_text()
    generated=extract(source).split('int main(int argc,char **argv)')[0]
    generated+='\n#include <zlib.h>\ntypedef UINT8 u8;typedef UINT16 u16;typedef UINT32 u32;typedef UINT64 u64;\n'
    generated+='enum{PSM_CT32=0,PSM_CT16=2,PSM_CT16S=10,PSM_T8=19,PSM_T4=20,PAGE_BLOCKS=32};static struct{u32 pal[256];}H;\n'
    generated+=section(tex,'static const u8 blk_32_8','// ---- record table and LRU ----')
    generated+=function(tex,'static void ap88_scan(')
    generated+=function(pic,'boolean Picture_CheckIfDoomPatch(')
    generated+=ORACLE_MAIN
    c=out/'actual-source-oracle.c';c.write_text(generated)
    command=['gcc','-std=gnu17','-O2','-g','-fwrapv','-DPS2','-DPS2_PROFILE','-DROTSPRITE','-DHWRENDER',
             '-Wno-unused-function','-Wno-unused-variable',c,'-o',out/'oracle','-lm','-lz']
    if sanitize:command+=['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-fno-pie','-no-pie']
    r=subprocess.run(list(map(str,command)),capture_output=True,text=True)
    (out/'oracle-build.log').write_text(' '.join(map(str,command))+'\n'+r.stdout+r.stderr)
    if r.returncode:raise RuntimeError(r.stderr)
    r=subprocess.run([out/'oracle',corpus],capture_output=True,text=True)
    (out/'oracle-run.log').write_text(r.stdout+r.stderr)
    if r.returncode:raise RuntimeError(r.stderr)
    controls={}
    if negative_controls:
        for name in ('POST','AP88'):
            exe=out/('negative-'+name.lower())
            cmd=[str(v) for v in command]
            cmd[cmd.index('-o')+1]=str(exe);cmd+=['-DOPT17_CORRUPT_'+name]
            b=subprocess.run(cmd,capture_output=True,text=True)
            (out/('negative-'+name.lower()+'-build.log')).write_text(' '.join(cmd)+'\n'+b.stdout+b.stderr)
            if b.returncode:raise RuntimeError(b.stderr)
            t=subprocess.run([exe,corpus],capture_output=True,text=True)
            (out/('negative-'+name.lower()+'.log')).write_text(t.stdout+t.stderr)
            if t.returncode!=101 or 'Patch rescue proof failed:' not in t.stderr:raise RuntimeError('undetected negative control '+name)
            controls[name.lower()]={'detected':True,'exit':t.returncode,'log_sha256':sha((out/('negative-'+name.lower()+'.log')).read_bytes())}
    return dict(result=json.loads(r.stdout),sanitizers=sanitize,negative_controls=controls,generated_sha256=sha(c.read_bytes()),binary_sha256=sha((out/'oracle').read_bytes()))


def ee_abi(source,out):
    import build as B
    probe=HERE/'patch_abi.c'
    flags=[f for f in B.CFLAGS if f not in ('-flto','-MMD','-MP','-DNOHW')]
    gen=source.parent/'hardware/gen'
    incs=['-I'+str(source/'src'),'-I'+str(source/'src/ps2'),'-I'+str(gen),*B.INCS[3:]]
    cmd=list(map(str,[B.CC,*flags,*incs,'-c',probe,'-o',out/'abi.o']))
    r=subprocess.run(cmd,capture_output=True,text=True)
    (out/'abi-build.log').write_text(' '.join(cmd)+'\n'+r.stdout+r.stderr)
    if r.returncode:raise RuntimeError(r.stderr)
    subprocess.run([str(B.DEV/'ee/bin/mips64r5900el-ps2-elf-objcopy'),'-O','binary','--only-section=.opt17_patch_abi',out/'abi.o',out/'abi.bin'],check=True)
    vals=struct.unpack('<11I',(out/'abi.bin').read_bytes())
    return dict(zip(('patch_t','column_t','post_t','column_align','post_align','patch_pixels_offset','patch_columns_offset','patch_posts_offset','post_top_offset','post_length_offset','post_data_offset'),vals))


def summarize(rows):
    numeric=('area','opaque_pixels','holes','canonical_pixels','canonical_posts','canonical_columns','canonical_ee_bytes',
             'canonical_pixels_lz4_model_bytes','canonical_post8_model_ee_bytes','canonical_post6_model_ee_bytes',
             'ap88_pow2_bytes','ap88_cropped_bytes','ap88_lz4_model_bytes','cpu_p8_or_p9_model_bytes',
             'cpu_tight_p8_or_p9_model_bytes','cpu_tight_always_coverage_model_bytes',
             'current_native_payload','current_native_span_bytes')
    result={'count':len(rows),**{k:sum(r[k] for r in rows) for k in numeric},
            'native_formats':dict(Counter(r['current_native_format'] for r in rows)),
            'with_holes':sum(bool(r['holes']) for r in rows),'with_opaque255':sum(r['opaque255'] for r in rows)}
    result['gs_fullsize_legal']=sum(r['gs_fullsize_legal'] for r in rows)
    result['with_holes_and_opaque255']=sum(bool(r['holes']) and r['opaque255'] for r in rows)
    for key in ('cpu_p8_exact_eligible','shared_p4_eligible','custom_p4_index_states_eligible','custom_p4_rgba_colors_eligible','custom_p8_index_states_eligible','custom_p8_rgba_colors_eligible'):
        selected=[r for r in rows if r[key]]
        entry={'count':len(selected),'ap88_bytes':sum(r['ap88_pow2_bytes'] for r in selected)}
        if 'p4' in key:
            entry.update(current_span_bytes=sum(r['current_native_span_bytes'] for r in selected),p4_span_bytes=sum(r['p4_span_bytes'] for r in selected),
                         image_span_reduction=sum(r['current_native_span_bytes']-r['p4_span_bytes'] for r in selected),
                         image_span_reduction_after_one_generic_clut_per_image=sum(max(0,r['current_native_span_bytes']-r['p4_span_bytes']-1024) for r in selected))
        result[key]=entry
    for key in ('max_post_top','max_post_length','max_post_data_offset','max_column_pixels'):
        result[key]=max((r[key] for r in rows),default=0)
    for digest,size,label in [('canonical_sha256','canonical_ee_bytes','identical_canonical'),('image_sha256','ap88_pow2_bytes','identical_ap88_images')]:
        groups=defaultdict(list)
        for r in rows:groups[r[digest]].append(r)
        result[label]={'groups_with_duplicates':sum(len(g)>1 for g in groups.values()),'duplicate_instances':sum(len(g)-1 for g in groups.values()),
                       'payload_opportunity_bytes':sum(sum(r[size] for r in g[1:]) for g in groups.values())}
    return result


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source-root',type=Path,default=ROOT/'build/nov16-hwopt/candidate-v7/source')
    ap.add_argument('--packs',type=Path,default=ROOT/'build/pak')
    ap.add_argument('--out',type=Path,default=ROOT/'build/nov17-sprite-audit')
    ap.add_argument('--sanitize',action='store_true')
    ap.add_argument('--negative-controls',action='store_true')
    ap.add_argument('--ee-abi',action='store_true')
    a=ap.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
    source=a.source_root.resolve();packs=a.packs.resolve()
    frozen=out/'frozen-tools';frozen.mkdir()
    for path in (Path(__file__),HERE/'patch_abi.c',HERE.parent/'patch_rescue_hosttest.py',HERE.parent/'patch_rescue_hosttest.c',HERE.parent/'next13_flats_cut_hosttest.py',HERE.parent/'cook.py',HERE.parent/'strip_pics.py'):
        shutil.copy2(path,frozen/path.name)
    reader=cook.PackReader(packs/'SRB2.PAK');allpal=reader.get(reader.names.index(b'PLAYPAL'));palraw=allpal[:768];pal=[palraw[i:i+3] for i in range(0,768,3)]
    rows,flats,fades,skipped=[],[],[],Counter();packhash={};corpus=out/'patch-corpus.bin'
    with corpus.open('wb') as f:
        f.write(b'OPT17SPR'+struct.pack('<I',0)+palraw)
        for name in ('SRB2.PAK','CHARS.PAK','ZONES.PAK','MUSIC.PAK'):
            path=packs/name;packhash[name]=sha(path.read_bytes());p=cook.PackReader(path)
            for i,n in enumerate(p.names):
                if n.endswith(b'/') or name=='MUSIC.PAK':continue
                raw=p.get(i);category=n.split(b'/')[0].decode('utf8','replace')
                if category=='Flats':
                    side=int(len(raw)**0.5)
                    if side in (16,32,64,128,256,512,1024) and side*side==len(raw) and not raw.startswith(MARKER):
                        colors=set(raw);bands={c>>4 for c in colors};band=next(iter(bands)) if len(bands)==1 else None
                        flats.append(dict(pack=name,index=i,name=n.decode('utf8','replace'),width=side,height=side,bytes=len(raw),colors=len(colors),has255=255 in colors,band=band,
                                          p8_span_bytes=footprint('P8',side,side)*256,p4_span_bytes=footprint('P4',side,side)*256,sha256=sha(raw)))
                        continue
                if category=='Fades':
                    fades.append(dict(pack=name,index=i,name=n.decode('utf8','replace'),bytes=len(raw),values=len(set(raw)),min=min(raw,default=0),max=max(raw,default=0)))
                    continue
                cooked=raw.startswith(MARKER);patch=raw[8:] if cooked else raw
                d=decode_patch(patch)
                if d is None:
                    skipped[name+':'+category]+=1
                    continue
                row=image_metrics(d,pal);row.update(pack=name,index=i,name=n.decode('utf8','replace'),category=category,cooked_png=cooked,decoded_lump_bytes=len(raw),decoded_lump_sha256=sha(raw))
                rows.append(row)
                header=[len(patch),zlib.crc32(d['ap88']),zlib.crc32(d['pixels']),zlib.crc32(d['metadata']),len(d['pixels']),len(d['posts']),d['w'],d['h'],int(bool(row['holes'])),int(row['opaque255']),footprint('P8',d['w'],d['h']),footprint('CT32',d['w'],d['h']),footprint('P4',d['w'],d['h'])]
                f.write(struct.pack('<13I',*header));f.write(patch)
            print(name,'patches',len(rows),'flats',len(flats),flush=True)
        f.seek(8);f.write(struct.pack('<I',len(rows)))
    srcfiles=('src/r_patch.c','src/r_defs.h','src/r_picformats.c','src/hardware/hw_cache.c','src/hardware/hw_data.h','src/ps2/hw/ps2_hw_tex.inc','src/ps2/hw/ps2_hw_spr.inc','src/ps2/hw/ps2_hw_priv.inc','src/ps2/hw/ps2_hw_pal.inc')
    bycat={}
    for cat in sorted({r['pack']+':'+r['category'] for r in rows}):bycat[cat]=summarize([r for r in rows if r['pack']+':'+r['category']==cat])
    sprite_rows=[r for r in rows if r['category']=='Sprites' or r['pack']=='CHARS.PAK']
    aliases=[]
    for k in range(len(allpal)//768):
        p=allpal[k*768:(k+1)*768]
        aliases.append({'bank':k,'index255_rgb':p[765:768].hex(),'aliases':[i for i in range(255) if p[i*3:i*3+3]==p[765:768]]})
    flatselected=[r for r in flats if r['band'] is not None]
    flatreport={'count':len(flats),'bytes':sum(r['bytes'] for r in flats),'single_band_count':len(flatselected),'single_band_bytes':sum(r['bytes'] for r in flatselected),
                'single_band_without255_count':sum(not r['has255'] for r in flatselected),'single_band_physical_span_reduction':sum(r['p8_span_bytes']-r['p4_span_bytes'] for r in flatselected),
                'at_most_16_colors_count':sum(r['colors']<=16 for r in flats)}
    report={'scope':'Complete stock decoded PAK source census. Each patch modeled as ordinary primary AP88 hardware patch with real-extent crop enabled and no explicit/automatic texture cap. Texture-folder patches are also composition inputs; their hypothetical standalone image totals are not actual usage. Native payload/spans are full-size arithmetic before tex_plan and are not valid full-size sampling claims for oversized pictures. No simultaneous residency, frame memory, feature equivalence, GPU validation or implementation claim.',
            'source_root':str(source),'source_sha256':{n:sha((source/n).read_bytes()) for n in srcfiles},'tool_sha256':sha(Path(__file__).read_bytes()),'pack_sha256':packhash,
            'canonical_ee_abi':ee_abi(source,out) if a.ee_abi else {'assumed_patch_t':48,'assumed_column_t':12,'assumed_post_t':12},
            'all_patches':summarize(rows),'sprites_and_characters':summarize(sprite_rows),'by_category':bycat,'raw_flats':flatreport,'fades':{'count':len(fades),'bytes':sum(r['bytes'] for r in fades)},'rejected_nonpatch_by_category':dict(skipped),'PLAYPAL_opaque255_aliases':aliases,
            'stock_rgba_sources':'No native RGBA pictures in these four cooked PAKs. Fifteen PNGs already contain canonical indexed Doom patches. External model packs, add-ons, generated/rotated patches and runtime colormap variants are not this corpus.',
            'oracle':oracle(source,corpus,out,a.sanitize,a.negative_controls),'corpus_sha256':sha(corpus.read_bytes()),
            'frozen_tool_sha256':{p.name:sha(p.read_bytes()) for p in frozen.iterdir()}}
    (out/'rows.json').write_text(json.dumps({'patches':rows,'flats':flats,'fades':fades},indent=2)+'\n')
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'report':str(out/'report.json'),'all_patches':report['all_patches'],'raw_flats':flatreport,'oracle':report['oracle']},indent=2))


if __name__=='__main__':main()
