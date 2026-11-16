"""Fault-only actual watchdog unwind, with hardware registers/IRQs stubbed."""
import argparse,hashlib,json,pathlib,re,subprocess
r=pathlib.Path(__file__).resolve().parents[3]
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--source-root',type=pathlib.Path,default=r)
ap.add_argument('--out',type=pathlib.Path,required=True)
ap.add_argument('--negative',action='store_true')
ap.add_argument('--sanitize',action='store_true')
a=ap.parse_args()
s=a.source_root.resolve()/'src/ps2/hw/ps2_hw_gs.inc'
t=s.read_text();start=t.index('static void wd_recover(');beg=t.index('{',start);d=1;end=beg+1
while d:d+=(t[end]=='{')-(t[end]=='}');end+=1
fn=t[start:end].replace('__asm__ volatile("sync.l");','hw_sync();').replace('*(volatile u32 *)0x10003000','gif_ctrl')
if a.negative:fn=fn.replace('const int interrupted_stencil = tile_stencil_active;','const int interrupted_stencil = 0;')
w=a.out.resolve();w.mkdir(parents=True,exist_ok=True)
prefix='''#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <setjmp.h>
typedef uint32_t u32;
static struct { struct { u32 timeouts,wd_recoveries; } st; u32 flips,dma_busy,q_n,q_head,pending,wr,run,done_seq,q_seq,clut_loaded,finish_pending,pend,pend_done; struct { u32 valid; } gsr; } H;
static struct { int on; } V;
static u32 d2,d1,vif1,csr,gif_ctrl;
#define D2_CHCR (&d2)
#define D1_CHCR (&d1)
#define VIF1_FBRST (&vif1)
#define GS_CSR (&csr)
#define WD_MAX_RECOVERIES 6
#define HWDBG_NOZC 2097152u
static u32 ps2hwd_dbg_flags,wd_last_flips=~0u;
static int tile_stencil_active,seq,released,aborted,failed,fellback,diagnosed,synced,irq_on;
static jmp_buf escape;
static void wd_diag(const char *s) { (void)s; diagnosed=++seq; }
static u32 DIntr(void) { return 1; }
static void EIntr(void) { irq_on=1; }
static void hw_sync(void) { synced=++seq; }
static void val_reset(void) {}
static void I_OutputMsg(const char *s) { (void)s; }
static void rel_release_all(void) { released=++seq; }
static void PS2HWD_Abort(void) { aborted=++seq; tile_stencil_active=0; }
static void hw_failure(const char *s) { (void)s; failed=++seq; }
static void fault_abort(void) { fellback=++seq; longjmp(escape,1); }
#define abort fault_abort
'''
body='''
static unsigned checks,failures;
#define WC(c) do { checks++; if(!(c)) { failures++; printf("WATCHDOG FAIL %s\\n",#c); } } while(0)
int main(void) {
 for(volatile int active=0;active<2;active++) {
  H=(typeof(H)){0};H.flips=17;H.dma_busy=H.q_n=H.q_head=H.pending=H.gsr.valid=H.clut_loaded=H.finish_pending=H.pend=1;H.q_seq=38;
  V.on=1;d1=d2=9;vif1=csr=gif_ctrl=0;tile_stencil_active=active;wd_last_flips=~0u;seq=released=aborted=failed=fellback=diagnosed=synced=irq_on=0;
  int escaped=setjmp(escape);if(!escaped)wd_recover("fixture timeout");
  WC(H.st.timeouts==1&&H.st.wd_recoveries==1);WC(d1==0&&d2==0&&vif1==1&&gif_ctrl==1);WC(synced>diagnosed&&released>synced);WC(H.done_seq==38&&!H.dma_busy&&!H.q_n&&!H.q_head&&!H.pending&&!H.gsr.valid&&!H.clut_loaded&&!H.finish_pending);WC(irq_on&&csr==2&&H.pend_done==1);
  if(active) {WC(escaped==1&&aborted>released&&failed>aborted&&fellback>failed);WC(!tile_stencil_active);}
  else {WC(!escaped&&!aborted&&!failed&&!fellback);}
 }
 printf("WATCHDOG COMPLETE checks=%u failed=%u\\n",checks,failures);return failures!=0;
}
'''
(w/'test.c').write_text(prefix+fn+body);cmd=['gcc','-std=gnu17','-O2','-Wall','-Wextra','-Werror',str(w/'test.c'),'-o',str(w/'test')]
if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
p=subprocess.run(cmd,text=True,capture_output=True);(w/'build.log').write_text(p.stdout+p.stderr)
if p.returncode:print(p.stdout+p.stderr);raise SystemExit(2)
p=subprocess.run([str(w/'test')],text=True,capture_output=True);(w/'test.log').write_text(p.stdout+p.stderr);print(p.stdout+p.stderr);(w/'result.json').write_text(json.dumps({'source':str(s),'source_sha256':hashlib.sha256(s.read_bytes()).hexdigest(),'negative':a.negative,'sanitize':a.sanitize,'rc':p.returncode,'output':p.stdout,'stderr':p.stderr},indent=2)+'\n');matched=(p.returncode==0 and 'WATCHDOG COMPLETE checks=13 failed=0' in p.stdout) if not a.negative else (p.returncode==1 and 'WATCHDOG FAIL' in p.stdout)
matched=matched and 'runtime error:' not in p.stderr and 'ERROR: AddressSanitizer' not in p.stderr
raise SystemExit(int(not matched))
