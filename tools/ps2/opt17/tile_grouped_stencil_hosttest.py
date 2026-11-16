"""Actual grouped mask helper, immediate/deferred; host packet/alias proof, no emulator."""
from pathlib import Path
import argparse, hashlib, importlib.util, json, re, subprocess, tempfile
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('bounded_stencil_host',HERE/'tile_stencil_hosttest.py')
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
C=base.C[:base.C.index('int main(void){')]
C=C.replace('static void ov_flush(void){CHECK(OV.n);OV.n=0;flushes++;}', '''static int flush_probe=-1,flush_expected_high;
static void drain(void);
static void ov_flush(void){drain();CHECK(OV.n);if(flush_probe>=0)CHECK((z[flush_probe]>>24)==(u32)flush_expected_high);OV.n=0;flushes++;}''')
C+=r'''
typedef struct{tile_mask_rect_t rect;int ox,oy;}batch_t;
_Static_assert(sizeof(tile_mask_rect_t)==12,"compact rectangle size");
_Static_assert(sizeof(batch_t)==20,"caller batch stride");
static void group_expected(const batch_t*b,int n,int selected,int ended){
 int rows=(H.fbh+31)&~31,a=tile_stencil_region.clear_x0,c=tile_stencil_region.clear_x1,
     d=tile_stencil_region.clear_y0,e=tile_stencil_region.clear_y1;
 for(int y=0;y<rows;y++)for(int x=0;x<H.fbw;x++){
  int at=y*H.fbw+x;u32 high=original[at]>>24;
  if(x>=a&&x<c&&y>=d&&y<e)high=ended?0:4;
  if(selected)for(int i=0;i<n;i++){const tile_mask_rect_t*r=&b[i].rect;if(x>=r->x0&&x<r->x1&&y>=r->y0&&y<r->y1)high=0;}
  CHECK((z[at]&0xffffffu)==low[at]);CHECK((z[at]>>24)==high);
 }
 CHECK(H.sciss_x0==7&&H.sciss_y0==11&&H.sciss_x1==41&&H.sciss_y1==23);
}
static void seed_group_depth(batch_t*b,int n){
 for(int i=0;i<n;i++)for(int y=b[i].rect.y0;y<b[i].rect.y1;y++)for(int x=b[i].rect.x0;x<b[i].rect.x1;x++)z[y*H.fbw+x]|=0x01000000u;
}
static void grouped(void){
 batch_t b[4]={{{33,65,17,33},0x30000000,0x50000000},{{97,127,21,45},0x30000001,0x50000001},
               {{67,95,48,60},0x30000002,0x50000002},{{129,159,65,89},0x30000003,0x50000003}};
 H.fbw=H.vw=320;H.fbh=H.vh=300;H.z_addr=0x240000u;H.target=1;
 H.sciss_x0=7;H.sciss_y0=11;H.sciss_x1=41;H.sciss_y1=23;
 count=done=flushes=0;flush_probe=-1;tile_stencil_rectangles=tile_stencil_mask_sprites=0;tile_stencil_reset_region();
 for(int i=0;i<320*320;i++){low[i]=(u32)(i*7919u)&0xffffffu;z[i]=original[i]=low[i]|((u32)(0x70+(i&15))<<24);}
 tile_stencil_begin(33,17,159,89);drain();
 tile_stencil_group(&b[0].rect,4,sizeof b[0]);drain();
 CHECK(tile_stencil_region.rects==&b[0].rect&&tile_stencil_region.count==4&&tile_stencil_region.stride==20&&tile_stencil_rectangles==4);
 group_expected(b,4,1,0);
 seed_group_depth(b,4);flush_probe=b[3].rect.y0*H.fbw+b[3].rect.x0;flush_expected_high=1;OV.n=1;
 tile_stencil_before_pass(0);drain();CHECK(!tile_stencil_zor);group_expected(b,4,1,0);
 seed_group_depth(b,4);OV.n=1;tile_stencil_before_pass(1);drain();CHECK(tile_stencil_zor==0x01000000u);group_expected(b,4,1,0);
 seed_group_depth(b,4);OV.n=1;tile_stencil_before_pass(0);drain();CHECK(!tile_stencil_zor);group_expected(b,4,1,0);
 flush_expected_high=0;OV.n=1;tile_stencil_group_end();drain();
 CHECK(!tile_stencil_region.selected&&!tile_stencil_region.rects&&!tile_stencil_region.count&&!tile_stencil_region.stride&&!tile_stencil_zor);
 group_expected(b,4,0,0);flush_probe=-1;
 // Replacing a live group restores the previous disjoint rectangles before selecting the new cell.
 tile_stencil_group(&b[0].rect,2,sizeof b[0]);drain();group_expected(b,2,1,0);
 tile_stencil_group(&b[2].rect,2,sizeof b[0]);drain();group_expected(&b[2],2,1,0);
 CHECK(tile_stencil_rectangles==8);
 // Failure cleanup clears the saved bbox without reading borrowed coordinates.
 tile_stencil_region.rects=(const tile_mask_rect_t *)(uintptr_t)1;
 tile_stencil_end();drain();group_expected(b,4,0,1);
 CHECK(!tile_stencil_region.rects&&!tile_stencil_region.count&&!tile_stencil_region.selected&&!tile_stencil_active&&!tile_stencil_zor);
 printf("GROUPED STENCIL stride=20 rect_size=12 disjoint=4 nodepth_then_ge=exact OV_order=exact end_borrow_read=none\n");
}
int main(void){
 one(640,480,100,100,120,120);one(640,512,639,511,640,512);one(320,300,0,0,1,1);one(320,300,17,13,319,299);one(640,480,0,0,640,480);
 grouped();printf("GROUPED STENCIL COMPLETE checks=%llu\n",checks);return 0;
}
'''


DEFERRED_C = r'''
static void deferred(void){
 batch_t b[4]={{{33,65,17,33},0x30000000,0x50000000},{{97,127,21,45},0x30000001,0x50000001},
               {{67,95,48,60},0x30000002,0x50000002},{{129,159,65,89},0x30000003,0x50000003}};
 H.fbw=H.vw=320;H.fbh=H.vh=300;H.z_addr=0x240000u;H.target=1;
 H.sciss_x0=7;H.sciss_y0=11;H.sciss_x1=41;H.sciss_y1=23;
 count=done=flushes=0;flush_probe=-1;OV.n=0;tile_stencil_rectangles=tile_stencil_mask_sprites=0;tile_stencil_reset_region();
 for(int i=0;i<320*320;i++){low[i]=(u32)(i*7919u)&0xffffffu;z[i]=original[i]=low[i]|((u32)(0x70+(i&15))<<24);}
 tile_stencil_begin(33,17,159,89);drain();
 const unsigned before=count;
 tile_stencil_group_deferred(&b[0].rect,4,sizeof b[0]);drain();
 CHECK(count==before); // deferred selection must not submit the redundant initial zero paint
 CHECK(tile_stencil_region.rects==&b[0].rect&&tile_stencil_region.count==4&&tile_stencil_region.stride==20&&tile_stencil_rectangles==4);
 group_expected(b,4,0,0); // every selected pixel is still high4 until its first pass
 tile_stencil_before_pass(1);drain();
 CHECK(tile_stencil_zor==0x01000000u&&count>before);group_expected(b,4,1,0);
 const unsigned activation=count-before;
 seed_group_depth(b,4);flush_probe=b[3].rect.y0*H.fbw+b[3].rect.x0;flush_expected_high=1;OV.n=1;
 tile_stencil_before_pass(0);drain();CHECK(!tile_stencil_zor);group_expected(b,4,1,0);
 seed_group_depth(b,4);OV.n=1;tile_stencil_before_pass(1);drain();CHECK(tile_stencil_zor==0x01000000u);group_expected(b,4,1,0);
 flush_expected_high=0;OV.n=1;tile_stencil_group_end();drain();group_expected(b,4,0,0);
 CHECK(!tile_stencil_region.rects&&!tile_stencil_region.selected&&!tile_stencil_region.count);flush_probe=-1;
 // A source failure before first activation needs only bbox cleanup, never borrowed coordinates.
 const unsigned unselected=count;tile_stencil_group_deferred(&b[0].rect,4,sizeof b[0]);CHECK(count==unselected);
 tile_stencil_region.rects=(const tile_mask_rect_t *)(uintptr_t)1;
 tile_stencil_end();drain();group_expected(b,4,0,1);
 CHECK(!tile_stencil_active&&!tile_stencil_zor&&!tile_stencil_region.rects);
 printf("DEFERRED STENCIL initial_mask_sprites=0 first_activation_sprites=%u NoDepth_Zw1_then_GE_OV=exact group_end=all4 failure_before_activation=exact\n",activation);
}
'''

def function_text(text,name):
 m=re.search(r'^(?:static\s+)?(?:inline\s+)?\w+[ *]+'+re.escape(name)+r'\([^;{}]*\)\n\{',text,re.M)
 if not m:raise AssertionError('missing function '+name)
 start=text.index('{',m.start());depth=0
 for i in range(start,len(text)):
  depth+=(text[i]=='{')-(text[i]=='}')
  if not depth:return text[m.start():i+1]
 raise AssertionError('unterminated function '+name)

def audit_call_paths(draw,tile_draw,tile,tex):
 emit=function_text(draw,'emit_piece')
 overlay_start=emit.index('if (P.lmode == LM_RAMP && P.npass >= 1 && !P.pass[0].ramp)')
 fog_start=emit.index('else if (P.lmode == LM_RAMP)',overlay_start)
 main=emit[:overlay_start];overlay=emit[overlay_start:fog_start];fog=emit[fog_start:]
 def precedes(part,guard,targets,label):
  if guard not in part:raise AssertionError(label+' missing before_pass')
  at=part.index(guard)
  for target in targets:
   if target not in part or at>=part.index(target):raise AssertionError(label+' activation follows '+target)
 precedes(main,'tile_stencil_before_pass(',['pass_setup(','emit_reglist_fan(','o = pk_alloc(','ws_pack(','put_vertex('],'ordinary sampled/nontexture pass')
 precedes(overlay,'tile_stencil_before_pass(0)',['ov_room(','emit_overlay('],'GE light overlay')
 precedes(fog,'tile_stencil_before_pass((P.flags & PF_NoDepthTest) != 0)',['state_flush(','o = pk_alloc(','put_vertex('],'fog ramp pass')
 group=function_text(tile_draw,'tile_draw_group')
 if not(group.index('tile_stencil_group_deferred(')<group.index('tile_draw_window(')<group.index('tile_stencil_group_end(')):raise AssertionError('group borrow/draw/end order')
 window=function_text(tile_draw,'tile_draw_window');prefix=window[:window.index('emit_piece(')]
 if prefix.count('tile_load_region(')!=1:raise AssertionError('unexpected upload path')
 if re.search(r'\b(?:pk_alloc|put_vertex|state_flush|sprite_tex|emit_[a-z_]+)\s*\(',prefix):raise AssertionError('draw before emit_piece')
 load=function_text(tile,'tile_load_impl')
 if 'upload_bands(' not in load or re.search(r'\b(?:pk_alloc|put_vertex|state_flush|sprite_tex|emit_[a-z_]+)\s*\(',load):raise AssertionError('tile upload performs a draw')
 upload=function_text(tex,'upload_bands')
 if 'HWGIF_TAG(nqw, 1, 0, 0, 2, 0)' not in upload or 'ad_set(&h[4], 0, GSR_TRXDIR)' not in upload or 'PRIM_' in upload:raise AssertionError('upload is not IMAGE-only')
 return ['ordinary sampled and nontexture passes activate before state/vertices','GE light overlay activates before queue packing','fog-ramp pass activates before state/vertices','borrow -> IMAGE-only tile upload -> emit_piece -> group_end']


def variants(s):
 yield 'normal',s,True
 a=s.index('static void tile_stencil_before_pass(');b=s.index('static inline void tile_stencil_unselect(',a)
 yield 'negative-before-pass-first-only',s[:a]+s[a:b].replace('i < tile_stencil_region.count','i < 1',1)+s[b:],False
 yield 'negative-stride-12',s.replace('tile_stencil_region.stride = stride;','tile_stencil_region.stride = sizeof(tile_mask_rect_t); (void)stride;',1),False
 a=s.index('static void tile_stencil_group_end(');b=s.index('\nstatic ',a+1)
 yield 'negative-group-end-first-only',s[:a]+s[a:b].replace('i < tile_stencil_region.count','i < 1',1)+s[b:],False
 if 'static void tile_stencil_group_deferred(' in s:
  yield 'negative-deferred-initial-paint',s.replace('(void)tile_stencil_borrow_group(rects, n, stride);','tile_stencil_group(rects, n, stride);',1),False
  mutation=s.replace('static void tile_stencil_group_deferred(', 'static int tile_stencil_skip_first_activation;\nstatic void tile_stencil_group_deferred(',1)
  mutation=mutation.replace('(void)tile_stencil_borrow_group(rects, n, stride);','(void)tile_stencil_borrow_group(rects, n, stride); tile_stencil_skip_first_activation=1;',1)
  a=mutation.index('static void tile_stencil_before_pass(');at=mutation.index('\tif (tile_stencil_region.selected)',a)
  mutation=mutation[:at]+'\tif (tile_stencil_skip_first_activation) { tile_stencil_skip_first_activation=0; tile_stencil_zor=depth_always?0x01000000u:0; return; }\n'+mutation[at:]
  yield 'negative-deferred-first-activation',mutation,False

def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--source-root',type=Path,default=HERE.parents[2]);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--expect-helper-sha256');a=ap.parse_args();helper_path=a.source_root.resolve()/'src/ps2/hw/ps2_hw_tile_stencil.inc';outdir=a.out.resolve();outdir.mkdir(parents=True,exist_ok=True)
 original=helper_path.read_text();digest=hashlib.sha256(original.encode()).hexdigest()
 if a.expect_helper_sha256 and digest!=a.expect_helper_sha256:raise RuntimeError('helper hash mismatch '+digest)
 deferred_api='static void tile_stencil_group_deferred(' in original
 test_c=C
 call_path_proof=None
 if deferred_api:
  test_c=C.replace('int main(void){',DEFERRED_C+'\nint main(void){',1).replace(' grouped();printf(', ' grouped();deferred();printf(',1)
  hw=helper_path.parent
  paths={name:hw/name for name in ['ps2_hw_draw.inc','ps2_hw_tile_draw.inc','ps2_hw_tile.inc','ps2_hw_tex.inc']}
  texts={name:path.read_text() for name,path in paths.items()}
  covered=audit_call_paths(texts['ps2_hw_draw.inc'],texts['ps2_hw_tile_draw.inc'],texts['ps2_hw_tile.inc'],texts['ps2_hw_tex.inc'])
  controls=[]
  guards=['tile_stencil_before_pass((P.flags & PF_NoDepthTest) || (ps2hwd_dbg_flags & 1));','tile_stencil_before_pass(0);','tile_stencil_before_pass((P.flags & PF_NoDepthTest) != 0);']
  for label,guard in zip(['ordinary','overlay','fog-ramp'],guards):
   emitted=function_text(texts['ps2_hw_draw.inc'],'emit_piece');assert guard in emitted
   changed=emitted.replace(guard,'/* negative: activation omitted */',1)
   mutation=texts['ps2_hw_draw.inc'].replace(emitted,changed,1)
   try:audit_call_paths(mutation,texts['ps2_hw_tile_draw.inc'],texts['ps2_hw_tile.inc'],texts['ps2_hw_tex.inc'])
   except AssertionError as error:controls.append({'path':label,'rejected':True,'reason':str(error)})
   else:raise AssertionError('call-path negative escaped: '+label)
  call_path_proof={'covered':covered,'negative_controls':controls,'source_sha256':{name:hashlib.sha256(path.read_bytes()).hexdigest() for name,path in paths.items()},'kind':'static actual-source ordering and IMAGE upload audit; dynamic helper tests below'}
 results=[]
 for name,helper,expected in variants(original):
  with tempfile.TemporaryDirectory(prefix='nov17-grouped-stencil-') as tmp:
   binary=str(Path(tmp)/'test')
   command=['cc','-std=gnu17','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-x','c','-','-o',binary]
   r=subprocess.run(command,input=test_c.replace('/* ACTUAL_HELPER */',helper),text=True,capture_output=True)
   if r.returncode:raise RuntimeError(r.stdout+r.stderr)
   r=subprocess.run([binary],text=True,capture_output=True);out=r.stdout+r.stderr
   passed=r.returncode==0 and 'GROUPED STENCIL COMPLETE' in out and 'runtime error:' not in out
   failure=re.search(r'^FAIL .+$',out,re.M)
   valid=passed if expected else r.returncode==1 and bool(failure) and 'runtime error:' not in out
   log=outdir/('grouped-stencil-'+name+'.log');log.write_text(out)
   results.append({'variant':name,'expected_pass':expected,'passed':passed,'matched_expectation':valid,'returncode':r.returncode,'failure':failure.group(0) if failure else None,'tested_helper_sha256':hashlib.sha256(helper.encode()).hexdigest(),'log':str(log)})
 normal=Path(results[0]['log']).read_text();complete=re.search(r'COMPLETE checks=(\d+)',normal)
 report={'helper':str(helper_path),'helper_sha256':digest,'deferred_api':deferred_api,'call_path_proof':call_path_proof,'fixture':str(Path(__file__).resolve()),'fixture_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'checks':int(complete.group(1)) if complete else None,'all_matched_expectation':all(r['matched_expectation'] for r in results),'sanitizers':['address','undefined'],'results':results,'emulator_used':False,'helper_edited_by_fixture':False,'scope':['actual compact strided rect subobjects:12-byte rect,20-byte parent record','four disjoint rectangles retain their holes and source-cell mask','NoDepth followed by GE resets high byte0 across every selected rectangle','pending OV flush precedes repaint or release of borrowed rects','group_end restores all selected rectangles to4 and clears the borrow','triangle failure cleanup uses only saved bbox, even when borrowed pointer is poisoned','legacy single-select path passes unchanged bounded-mask checks']}
 path=outdir/'result.json';path.write_text(json.dumps(report,indent=2)+'\n')
 print(json.dumps({'report':str(path),'helper_sha256':digest,'checks':report['checks'],'all_matched_expectation':report['all_matched_expectation']},indent=2))
 return int(not report['all_matched_expectation'])
if __name__=='__main__':raise SystemExit(main())
