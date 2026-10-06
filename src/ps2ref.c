#include "doomdef.h"
#include "ps2ref.h"
#include "m_argv.h"
#include "m_random.h"
#include "g_game.h"
#include "g_demo.h"
#include "d_main.h"
#include "w_wad.h"
#include "p_local.h"
#include "deh_soc.h"
#include "p_setup.h"
#include "r_state.h"
#include "info.h"
#include "screen.h"
#include "v_video.h"
#include "z_zone.h"
#include "i_system.h"
#include "i_time.h"
#include "f_finale.h"
#include "command.h"
#include "d_event.h"
#include "keys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *tics, *frames, *numbers, *sfxlog, *allhash;
static char directory[1024];
static UINT32 seq, lastframe, lastall;
static UINT32 hash_bytes(UINT32 h, const void *p, size_t n)
{
    const UINT8 *b=p;
    while (n--) { h^=*b++; h*=16777619u; }
    return h;
}
/* Integer serialization is independent of pointer size, padding and endianness. */
static UINT32 hash_u32(UINT32 h, UINT32 v)
{
    UINT8 b[4]={(UINT8)v,(UINT8)(v>>8),(UINT8)(v>>16),(UINT8)(v>>24)};
    return hash_bytes(h,b,4);
}
static FILE *output(const char *name, const char *mode)
{
    char path[1200]; FILE *f;
    snprintf(path,sizeof(path),"%s/%s",directory,name);
    f=fopen(path,mode);
    if (!f) I_Error("PS2Ref: cannot write %s",path);
    return f;
}
static void memory(const char *name)
{
    INT32 tag; FILE *f=output(name,"wb");
    fprintf(f,"tag,bytes\n");
    for(tag=0;tag<256;tag++) { size_t n=Z_TagUsage(tag); if(n) fprintf(f,"%d,%lu\n",tag,(unsigned long)n); }
    fclose(f);
}
void PS2Ref_Init(void)
{
    INT32 p=M_CheckParm("-ps2ref");
    if(!p) return;
    if(p+1>=myargc) I_Error("-ps2ref requires an existing output directory");
    snprintf(directory,sizeof(directory),"%s",myargv[p+1]);
    tics=output("tics.csv","wb"); frames=output("frames.csv","wb");
    numbers=output("soc.tsv","wb"); sfxlog=output("sfx.csv","wb");
    fprintf(tics,"seq,leveltic,map,rng,x,y,z,momx,momy,momz,health,rings,state,mobjs,state_hash\n");
    fprintf(frames,"seq,leveltic,width,height,fnv1a32\n");
    fprintf(numbers,"expression_hex\tvalue\n");
    fprintf(sfxlog,"name,bytes,fnv1a32\n");
}
void PS2Ref_Number(const char *word, INT32 value)
{
    const unsigned char *p=(const unsigned char*)word;
    if(!numbers) return;
    while(*p) fprintf(numbers,"%02x",*p++);
    fprintf(numbers,"\t%d\n",value);
}
/* A controlled platform clock: loading/title wipes cannot skip ticks due to
 * host scheduling. Timedemo already advances gameplay one tick per frame. */
boolean PS2Ref_Clock(void)
{
    if(!tics) return false;
    if(M_CheckParm("-ps2ref-idle")) return false; // soak uses the normal hardware clock
    g_time.time++;
    g_time.timefrac=0;
    return true;
}
/* -ps2ref-scan: evaluate every string argument of every linedef/thing of the loaded map through get_number,
 * so the soc.tsv table covers strings that would otherwise only be evaluated when a special triggers. */
static void scan_map(void)
{
    size_t i; int k;
    for(i=0;i<numlines;i++) for(k=0;k<NUMLINESTRINGARGS;k++)
        if(lines[i].stringargs[k]) (void)get_number(lines[i].stringargs[k]);
    for(i=0;i<nummapthings;i++) for(k=0;k<NUMMAPTHINGSTRINGARGS;k++)
        if(mapthings[i].stringargs[k]) (void)get_number(mapthings[i].stringargs[k]);
}
void PS2Ref_Tic(void)
{
    thinker_t *th; UINT32 h=2166136261u, n=0; INT32 i;
    mobj_t *mo; player_t *p;
    if(tics && gamestate==GS_LEVEL && M_CheckParm("-ps2ref-scan")) { scan_map(); PS2Ref_End(); }
    if(!tics || !demoplayback || gamestate!=GS_LEVEL) return;
    p=&players[consoleplayer]; mo=p->mo;
    if(!mo) I_Error("PS2Ref: demo has no player mobj");
    h=hash_u32(h,P_GetRandSeed());
    for(i=0;i<MAXPLAYERS;i++) if(playeringame[i]) {
        player_t *q=&players[i]; mobj_t *m=q->mo;
        h=hash_u32(h,i); h=hash_u32(h,q->rings); h=hash_u32(h,q->score);
        h=hash_u32(h,q->pflags); h=hash_u32(h,q->playerstate); h=hash_u32(h,q->lives);
        if(m) { h=hash_u32(h,m->x); h=hash_u32(h,m->y); h=hash_u32(h,m->z); h=hash_u32(h,m->angle); }
    }
    for(th=thlist[THINK_MOBJ].next;th!=&thlist[THINK_MOBJ];th=th->next) {
        mobj_t *m=(mobj_t*)th; n++;
        h=hash_u32(h,th->removing); h=hash_u32(h,m->type);
        h=hash_u32(h,m->x); h=hash_u32(h,m->y); h=hash_u32(h,m->z);
        h=hash_u32(h,m->momx); h=hash_u32(h,m->momy); h=hash_u32(h,m->momz);
        h=hash_u32(h,m->angle); h=hash_u32(h,m->health); h=hash_u32(h,m->tics);
        h=hash_u32(h,m->state ? (UINT32)(m->state-states) : UINT32_MAX);
        h=hash_u32(h,m->flags); h=hash_u32(h,m->flags2); h=hash_u32(h,m->eflags);
    }
    { /* -ps2ref-mobjs FROM TO: per-mobj dump of those level tics (mobjs-<tic>.csv), to locate state_hash differences */
        INT32 q=M_CheckParm("-ps2ref-mobjs");
        if(q && q+2<myargc && (INT32)leveltime>=atoi(myargv[q+1]) && (INT32)leveltime<=atoi(myargv[q+2])) {
            char name[64]; FILE *f; UINT32 k=0;
            snprintf(name,sizeof(name),"mobjs-%u.csv",(unsigned)leveltime);
            f=output(name,"wb");
            fprintf(f,"idx,type,x,y,z,momx,momy,momz,angle,health,tics,state,flags,flags2,eflags\n");
            for(th=thlist[THINK_MOBJ].next;th!=&thlist[THINK_MOBJ];th=th->next) {
                mobj_t *m=(mobj_t*)th;
                fprintf(f,"%u,%d,%d,%d,%d,%d,%d,%d,%u,%d,%d,%d,%u,%u,%u\n",k++,(INT32)m->type,m->x,m->y,m->z,m->momx,m->momy,m->momz,
                    (unsigned)m->angle,m->health,(INT32)m->tics,m->state?(INT32)(m->state-states):-1,(unsigned)m->flags,(unsigned)m->flags2,(unsigned)m->eflags);
            }
            fclose(f);
        }
    }
    seq++;
    fprintf(tics,"%u,%u,%d,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%08x\n",
        seq,leveltime,gamemap,P_GetRandSeed(),mo->x,mo->y,mo->z,mo->momx,mo->momy,mo->momz,
        mo->health,p->rings,mo->state?(INT32)(mo->state-states):-1,n,h);
    if(seq==1) memory("memory-level.csv");
}
/* -ps2ref-lumps: after W_InitMultipleFiles write the lump table of every loaded file (lumps.tsv) and quit.
 * Tab separated: wadnum lumpnum name longname fullname size - the PC build (pk3) and the PS2 build (packs) must agree. */
void PS2Ref_Lumps(void)
{
    UINT16 w, l; FILE *f;
    if(!tics || !M_CheckParm("-ps2ref-lumps")) return;
    f=output("lumps.tsv","wb");
    fprintf(f,"wadnum\tlumpnum\tname\tlongname\tfullname\tsize\n");
    for(w=0;w<numwadfiles;w++) for(l=0;l<wadfiles[w]->numlumps;l++) {
        const lumpinfo_t *li=&wadfiles[w]->lumpinfo[l];
        fprintf(f,"%u\t%u\t%s\t%s\t%s\t%lu\n",(unsigned)w,(unsigned)l,li->name,li->longname,li->fullname,(unsigned long)li->size);
    }
    fclose(f);
    PS2Ref_End();
}
/* -ps2ref-title N: dump the indexed frame of the title screen at its 35th, 70th ... N-th rendered frame, then quit. */
static void title_frame(void)
{
    static UINT32 count;
    UINT32 limit=(UINT32)atoi(M_GetNextParm());
    UINT32 h; char name[64]; FILE *f; size_t size;
    if(gamestate!=GS_TITLESCREEN) return;
    count++;
    if(count%35) return;
    size=(size_t)vid.width*vid.height; h=hash_bytes(2166136261u,screens[0],size);
    fprintf(frames,"%u,%u,%d,%d,%08x\n",count,count,vid.width,vid.height,h);
    snprintf(name,sizeof(name),"title-%06u.idx",count); f=output(name,"wb");
    if(fwrite(screens[0],1,size,f)!=size) I_Error("PS2Ref: short frame write");
    fclose(f);
    if(count>=limit) PS2Ref_End();
}
/* Real-clock title soak. Configure rollingdemos Off; do not modify title/game logic. */
static void idle_frame(void)
{
    static precise_t start, next;
    static UINT32 count, lastcount, countmin=UINT32_MAX, countmax;
    static UINT64 countsum;
    precise_t now=I_GetPreciseTime(), precision=I_GetPrecisePrecision();
    UINT32 seconds, duration, delta=0;
    char name[64];
    INT32 parm=M_CheckParm("-ps2ref-idle");
    if(parm+1>=myargc) I_Error("-ps2ref-idle requires seconds");
    duration=(UINT32)atoi(myargv[parm+1]);
    if(!duration) I_Error("-ps2ref-idle requires positive seconds");
    if(gamestate!=GS_TITLESCREEN) I_Error("PS2Ref idle left the title screen");
#ifdef PS2
    UINT32 cop0;
    __asm__ volatile("mfc0 %0, $9" : "=r"(cop0));
    if(count) {
        delta=cop0-lastcount;
        if(delta<countmin) countmin=delta;
        if(delta>countmax) countmax=delta;
        countsum+=delta;
    }
    lastcount=cop0;
#else
    (void)lastcount;
    (void)delta;
#endif
    count++;
    if(!start) { start=now; next=now; }
    seconds=(UINT32)((now-start)/precision);
    if(now>=next || seconds>=duration) {
        snprintf(name,sizeof(name),"memory-idle-%03u.csv",seconds);
        memory(name);
        CONS_Printf("G1 idle seconds=%u frames=%u zone_payload=%lu cop0_framegap_min=%u max=%u mean=%u\n",
            seconds,count,(unsigned long)Z_TotalUsage(),count>1?countmin:0,countmax,
            count>1?(UINT32)(countsum/(count-1)):0);
        next=now+60*precision;
    }
    if(seconds>=duration) PS2Ref_End();
}
#ifndef PS2
/* OPT10-HF (PC only): picture reference for the PS2 hardware renderer. -ps2ref-shot SPEC takes the engine screenshot (OpenGL: glReadPixels of the
 * frame just drawn, <home>/screenshots) at the same frames as the PS2 -vidshot: items t35 (35th title frame), l70 (70th level frame), f200 (200th frame),
 * k300 (first frame with leveltime >= 300; K300 only in a level started after the previous shot), w5 (5th frame of a wipe), each with an optional
 * =command after the shot ('~' is a space: k300=map~2). -ps2ref-keys 120:enter,130:down,...: key presses by frame number (the keys of -vidkeys).
 * The game quits after the last shot (line "PS2SHOT COMPLETE"). Every shot prints "PS2SHOT <tag> leveltime=<n> n=<ordinal>". */
extern boolean takescreenshot;
static void shot_keys(INT32 framen)
{
    INT32 q=M_CheckParm("-ps2ref-keys"); const char *p;
    if(!q || q+1>=myargc) return;
    for(p=myargv[q+1];*p;) {
        INT32 n=0, key=0; char name[12]; const char *kn=name; size_t len=0; boolean down=true, up=true; event_t ev;
        while(*p>='0' && *p<='9') n=n*10+(*p++-'0');
        if(*p==':') p++;
        while(*p && *p!=',' && len<sizeof name-1) name[len++]=*p++;
        name[len]='\0';
        while(*p && *p!=',') p++;
        if(*p==',') p++;
        if(n!=framen) continue;
        if(*kn=='+') up=false, kn++; else if(*kn=='-') down=false, kn++;
        if(!strcmp(kn,"enter")) key=KEY_ENTER; else if(!strcmp(kn,"esc")) key=KEY_ESCAPE; else if(!strcmp(kn,"up")) key=KEY_UPARROW;
        else if(!strcmp(kn,"down")) key=KEY_DOWNARROW; else if(!strcmp(kn,"left")) key=KEY_LEFTARROW; else if(!strcmp(kn,"right")) key=KEY_RIGHTARROW;
        else if(!strcmp(kn,"bs")) key=KEY_BACKSPACE; else if(!strcmp(kn,"space")) key=KEY_SPACE; else if(!strcmp(kn,"tab")) key=KEY_TAB;
        else if(!strcmp(kn,"console")) key='`'; else if(!strcmp(kn,"f1")) key=KEY_F1; else if(!strcmp(kn,"f2")) key=KEY_F2;
        else if(!strcmp(kn,"f10")) key=KEY_F10; else if(!strcmp(kn,"f11")) key=KEY_F11;
        else if(kn[0]>='a' && kn[0]<='z' && !kn[1]) key=kn[0];
        else I_Error("-ps2ref-keys: unknown key '%s'",kn);
        memset(&ev,0,sizeof ev); ev.key=key;
        if(down) { ev.type=ev_keydown; D_PostEvent(&ev); }
        if(up) { ev.type=ev_keyup; D_PostEvent(&ev); }
    }
}
static void shot_frame(void)
{
    static boolean parsed, quitnext; static INT32 titlen, leveln, anyn, wipen, left, done, knext; static boolean klow=true; static char spec[1536];
    INT32 kord=0; const char *p;
    if(quitnext) { CONS_Printf("PS2SHOT COMPLETE %d\n",(int)done); I_Quit(); }
    if(!parsed) {
        parsed=true;
        if(M_CheckParm("-ps2ref-shot") && M_IsNextParm()) {
            snprintf(spec,sizeof spec,"%s",M_GetNextParm());
            for(p=spec;*p;) { left+=(*p=='t'||*p=='l'||*p=='f'||*p=='k'||*p=='K'||*p=='w'); while(*p && *p!=',') p++; if(*p==',') p++; }
        }
    }
    anyn++;
    shot_keys(anyn);
    if(!left) return;
    if(gamestate==GS_LEVEL && leveltime<20) klow=true;
    if(WipeInAction) wipen++; else if(gamestate==GS_TITLESCREEN) titlen++; else if(gamestate==GS_LEVEL) leveln++;
    for(p=spec;*p;) {
        const char kind=*p++; INT32 n=0, hit; char cmd[64]; size_t cl=0;
        while(*p>='0' && *p<='9') n=n*10+(*p++-'0');
        cmd[0]='\0';
        if(*p=='=') { for(p++;*p && *p!=',' && cl<sizeof cmd-2;p++) cmd[cl++]=*p=='~'?' ':*p; cmd[cl++]='\n'; cmd[cl]='\0'; }
        while(*p && *p!=',') p++;
        if(*p==',') p++;
        hit=(kind=='w' && WipeInAction && n==wipen)
            || (!WipeInAction && ((kind=='t' && n==titlen) || (kind=='l' && n==leveln) || (kind=='f' && n==anyn)))
            || (!WipeInAction && (kind=='k' || kind=='K') && kord++==knext && gamestate==GS_LEVEL && (INT32)leveltime>=n && (klow || kind=='k'));
        if(hit && (kind=='k' || kind=='K')) { knext++; klow=false; }
        if(!hit) continue;
        takescreenshot=true;
        CONS_Printf("PS2SHOT %c%d leveltime=%d n=%d gamestate=%d\n",kind,(int)n,(int)leveltime,(int)++done,(int)gamestate);
        if(cmd[0]) COM_BufAddText(cmd);
        if(done>=left) quitnext=true;
    }
}
#endif
void PS2Ref_Frame(void)
{
    UINT32 h; char name[64]; FILE *f; size_t size;
#ifndef PS2
    shot_frame();
#endif
    if(frames && M_CheckParm("-ps2ref-idle")) { idle_frame(); return; }
    if(frames && M_CheckParm("-ps2ref-title") && M_IsNextParm()) { title_frame(); return; }
    if(frames && seq && seq!=lastall && demoplayback && gamestate==GS_LEVEL && M_CheckParm("-ps2ref-hashall"))
    { /* -ps2ref-hashall: FNV-1a of every rendered level frame in allhash.csv (frame-exact comparison of two builds) */
        if(!allhash) { allhash=output("allhash.csv","wb"); fprintf(allhash,"seq,fnv1a32\n"); }
        fprintf(allhash,"%u,%08x\n",seq,hash_bytes(2166136261u,screens[0],(size_t)vid.width*vid.height));
        lastall=seq;
    }
    { /* -ps2ref-every N: dump every N-th frame instead of every 35th (equivalence statistics over many frames) */
        INT32 q=M_CheckParm("-ps2ref-every"); UINT32 every=(q && q+1<myargc && atoi(myargv[q+1])>0) ? (UINT32)atoi(myargv[q+1]) : 35;
        if(!frames || !seq || seq==lastframe || seq%every || !demoplayback || gamestate!=GS_LEVEL) return;
    }
    if(vid.width!=320 || vid.height!=200 || vid.bpp!=1) I_Error("PS2Ref: expected 320x200x8");
    size=(size_t)vid.width*vid.height; h=hash_bytes(2166136261u,screens[0],size);
    fprintf(frames,"%u,%u,%d,%d,%08x\n",seq,leveltime,vid.width,vid.height,h);
    snprintf(name,sizeof(name),"frame-%06u.idx",seq); f=output(name,"wb");
    if(fwrite(screens[0],1,size,f)!=size) I_Error("PS2Ref: short frame write");
    fclose(f); lastframe=seq;
}
void PS2Ref_Sfx(const char *name,const void *data,size_t size)
{
    char path[96]; FILE *f;
    if(!sfxlog || !name || !data) return;
    snprintf(path,sizeof(path),"sfx-%s.pcm",name); f=output(path,"wb");
    if(fwrite(data,1,size,f)!=size) I_Error("PS2Ref: short PCM write");
    fclose(f);
    fprintf(sfxlog,"%s,%lu,%08x\n",name,(unsigned long)size,hash_bytes(2166136261u,data,size));
}
void PS2Ref_End(void)
{
    FILE *done;
    if(!tics) return;
    memory("memory-end.csv");
    fclose(tics); fclose(frames); fclose(numbers); fclose(sfxlog); if(allhash) fclose(allhash);
    tics=frames=numbers=sfxlog=NULL;
    done=output("complete.txt","wb"); fprintf(done,"complete tics=%u\n",seq); fclose(done);
    I_Quit();
}
