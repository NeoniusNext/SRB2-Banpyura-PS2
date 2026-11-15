/* Appended after complete literal engine/driver functions by the runner. */
static unsigned long long upload_bytes,upload_cases,compact_bytes,reference_bytes,corpus_cases;

static void check_upload(GLMipmap_t*m,GLMipmap_t*raw,u32 cw,u32 ch,u32 dx,u32 dy,u32 w,u32 h){
 selection_t a=select_patch(m,cw,ch),b=select_patch(raw,cw,ch);
 check(a.psm==b.psm&&a.clut==b.clut&&a.baked==b.baked&&a.holes==b.holes,"identical literal AP88 format/CLUT/baked/holes selection");
 a.c.src=m->data;b.c.src=raw->data;a.c.sw=b.c.sw=m->width;a.c.sh=b.c.sh=m->height;
 a.c.dx=b.c.dx=dx;a.c.dy=b.c.dy=dy;a.c.W2=b.c.W2=w;a.c.H2=b.c.H2=h;
 size_t bytes=(size_t)w*(a.psm==PSM_T8?1:4);u8*left=malloc(bytes+32),*right=malloc(bytes+32);check(left&&right,"upload row buffers");
 memset(left,0xa5,bytes+32);memset(right,0xa5,bytes+32);
 for(u32 y=0;y<h;y++){
  a.fill(&a.c,y,left+16);b.fill(&b.c,y,right+16);
  check(!memcmp(left,right,bytes+32),"all native P8/CT32 upload texels and guards match old AP88 source");upload_bytes+=bytes;
 }
 free(left);free(right);upload_cases++;
}

static void proto_extra(GLMipmap_t*m,const UINT8*bytes){
 GLMipmap_t raw=*m;raw.data=(void*)bytes;raw.ps2_cpu=0;
 u32 dx=m->width>1?1:0,dy=m->height>1?1:0;
 check_upload(m,&raw,m->ps2_uw,m->ps2_uh,0,0,m->ps2_uw,m->ps2_uh);
 check_upload(m,&raw,m->width,m->height,dx,0,m->width>>dx,m->height);
 check_upload(m,&raw,m->width,m->height,0,dy,m->width,m->height>>dy);
 check_upload(m,&raw,m->width,m->height,dx,dy,m->width>>dx,m->height>>dy);
}

static void synthetic_extra(void){
 /* Deliberate opaque255, translated original255/alpha0, and sparse NPOT edges. */
 for(unsigned keyed=0;keyed<2;keyed++)for(unsigned mapped=0;mapped<3;mapped++){
  patch_t*p=flat_source(13,17,0);GLPatch_t g={0};GLMipmap_t m={0};GLColormap_t cm={0};
  p->pixels[0]=255;p->pixels[1]=7;
  for(unsigned i=0;i<256;i++)cm.data[i]=(UINT8)(mapped==1?255-i:(i%3?255:73));cm.data[255]=100;
  if(mapped)m.colormap=&cm;HWR_MakePatch(p,&g,&m,false);m.flags=keyed?TF_CHROMAKEYED:0;
  HWR_MakePatch(p,&g,&m,true);GLMipmap_t ref=m;ref.data=NULL;MakeBlock(&ref);HWR_DrawPatchInCache(&ref,ref.width,ref.height,p->width,p->height,p);
  check(compare_ap88(&m,ref.data),"mapped/index255 synthetic complete AP88 match");
  for(unsigned epoch=0;epoch<3;epoch++){
   for(unsigned i=0;i<256;i++)H.pal[i]=(i*0x10203u)^(epoch*0x8931u);if(epoch==1)H.pal[100]=H.pal[255];
   proto_extra(&m,ref.data);
  }
  ps2hwd_dbg_flags=HWDBG_COMPOSE;HWR_MakePatch(p,&g,&m,true);ps2hwd_dbg_flags=0;
  check(compare_ap88(&m,ref.data),"COMPOSE uses virtual bytes and clears raw clone mode");
  Z_Free(ref.data);Z_Free(m.data);Patch_Free(p);
 }
 /* Overlapping posts must clear previous coverage, and a later flag change
    must not turn the cached byte invariant into a palette-dependent fact. */
 for(unsigned opaque=0;opaque<2;opaque++){
  patch_t*p=Patch_Create(1,4);p->columns=Z_Calloc(sizeof(column_t),PU_PATCH_DATA,NULL);
  p->posts=Z_Calloc(2*sizeof(post_t),PU_PATCH_DATA,NULL);p->pixels=Z_Calloc(4,PU_PATCH_DATA,NULL);
  p->columns[0]=(column_t){2,p->posts,p->pixels};p->posts[0]=(post_t){1,2,0};p->posts[1]=(post_t){1,2,2};
  p->pixels[0]=1;p->pixels[1]=2;p->pixels[2]=255;p->pixels[3]=3;
  GLPatch_t g={0};GLMipmap_t m={0};GLColormap_t cm={0};for(unsigned i=0;i<256;i++)cm.data[i]=(UINT8)i;cm.data[255]=100;cm.data[3]=opaque?255:42;m.colormap=&cm;
  HWR_MakePatch(p,&g,&m,false);m.flags=TF_CHROMAKEYED;HWR_MakePatch(p,&g,&m,true);
  GLMipmap_t raw=m;raw.data=NULL;MakeBlock(&raw);HWR_DrawPatchInCache(&raw,raw.width,raw.height,p->width,p->height,p);
  check(compare_ap88(&m,raw.data),"overlapping transparent post clears old coverage and preserves index100");
  m.flags=raw.flags=0;H.pal[100]=H.pal[255];proto_extra(&m,raw.data);H.pal[100]^=0x010101u;proto_extra(&m,raw.data);
  Z_Free(raw.data);Z_Free(m.data);Patch_Free(p);
 }
 /* The normal try/malloc decision is based on OLD logical AP88 size. */
 for(unsigned large=0;large<2;large++){
  patch_t*p=flat_source(large?129:31,large?129:31,0);GLMipmap_t m={0};GLPatch_t g={0};
  unsigned tc=try_calls,mc=malloc_calls,fc=fatal_calls,gc=guard_calls;fail_try=1;HWR_MakePatch(p,&g,&m,true);
  if(large)check(!m.data&&try_calls==tc+1&&malloc_calls==mc,"old large AP88 request remains try even when compact payload is below64KiB");
  else check(m.data&&try_calls==tc&&malloc_calls==mc+1,"old small AP88 request retains mandatory allocation route");
  check(fatal_calls==fc&&guard_calls==gc,"normal try failure adds no fatal or guard call");fail_try=0;
  if(m.data)Z_Free(m.data);HWR_MakePatch(p,&g,&m,true);check(m.data!=NULL,"retry encodes after failed normal request");
  if(large){fail_try=1;HWR_MakePatch(p,&g,&m,true);check(!m.data&&m.ps2_cpu==0,"failed rebuild invalidates old owner and mode");fail_try=0;}
  if(m.data)Z_Free(m.data);Patch_Free(p);
 }
 /* The existing makebitmap=false entry permits NULL after initialization. */
 GLMipmap_t m={0};GLPatch_t g={0};m.width=m.height=8;m.format=GL_TEXFMT_AP_88;
 HWR_MakePatch(NULL,&g,&m,false);check(!m.data,"initialized null-patch metadata-only call");
 /* Unusual posts extending into logical padding must retain raw storage. */
 patch_t*p=flat_source(7,9,0);p->posts[0].topdelta=9;p->posts[0].length=1;
 HWR_MakePatch(p,&g,&m,false);m.width=m.height=16;m.ps2_uw=7;m.ps2_uh=9;
#ifdef HWR_PS2_PACKED_PATCHES
 check(!HWR_PS2_CanPackPatch(p,&m),"noncanonical post extent retains existing raw path");
#endif
 Patch_Free(p);
 /* Generic allocations always clear stale compact bits, including failure. */
 m.width=m.height=8;m.ps2_cpu=3;m.format=GL_TEXFMT_ALPHA_8;MakeBlock(&m);check(m.ps2_cpu==0,"raw alpha allocation clears compact mode");Z_Free(m.data);
 m.ps2_cpu=3;m.format=GL_TEXFMT_AP_88;fail_try=1;check(!MakeBlockEx(&m,true)&&m.ps2_cpu==0,"failed raw allocation clears stale mode");fail_try=0;
}

static void full_corpus(const char*path){
 FILE*f=fopen(path,"rb");check(f!=NULL,"open immutable corpus");u8 magic[8],rgb[768];u32 n;
 check(fread(magic,8,1,f)==1&&!memcmp(magic,"OPT17SPR",8),"corpus marker");check(fread(&n,4,1,f)==1&&fread(rgb,768,1,f)==1,"corpus header");
 for(u32 i=0;i<256;i++)H.pal[i]=rgb[i*3]|((u32)rgb[i*3+1]<<8)|((u32)rgb[i*3+2]<<16);
 for(u32 r=0;r<n;r++){
  u32 header[13];check(fread(header,sizeof header,1,f)==1,"case header");
  u8*data=malloc(header[0]);check(data&&fread(data,header[0],1,f)==1,"case full payload");
  patch_t*p=Patch_CreateFromDoomPatch((softwarepatch_t*)data);free(data);
  for(unsigned mapping=0;mapping<3;mapping++){
   GLPatch_t g={0};GLMipmap_t m={0};GLColormap_t cm={0};
   for(unsigned i=0;i<256;i++)cm.data[i]=(UINT8)(mapping==1?255-i:(i%7?(i*37u)^0xa5u:255));cm.data[255]=100;
   if(mapping)m.colormap=&cm;HWR_MakePatch(p,&g,&m,true);
   check(m.data!=NULL,"normal compact allocation");size_t bytes=(size_t)m.width*m.height*2;
#ifdef HWR_PS2_PACKED_PATCHES
   check(HWR_PS2_IsPackedPatch(&m),"every canonical corpus source encoded");
   size_t payload=(size_t)p->width*p->height;payload+=(payload+7u)>>3;
   check(blocks[block_of(m.data)].n==payload,"one tight indices+coverage allocation, no AP88 staging");
#else
   size_t payload=bytes;check(m.ps2_cpu==0,"opt-out retains raw storage");
#endif
   GLMipmap_t raw=m;raw.data=NULL;MakeBlock(&raw);HWR_DrawPatchInCache(&raw,raw.width,raw.height,p->width,p->height,p);
   check(compare_ap88(&m,raw.data),"entire corpus mapped AP88 image directly matches old drawer");compared_bytes+=bytes;corpus_cases++;
   if(!mapping){
    compact_bytes+=payload;reference_bytes+=bytes;
    check_upload(&m,&raw,p->width,p->height,0,0,p->width,p->height);
    u32 dx=m.width>=4?2:(m.width>=2?1:0),dy=m.height>=4?2:(m.height>=2?1:0);
    check_upload(&m,&raw,m.width,m.height,dx,dy,m.width>>dx,m.height>>dy);
    tt_capture(&m);check(TT.have&&TT.w==m.width&&TT.h==m.height&&TT.bpp==2,"unchanged TT logical capture scope");
    check(!memcmp(TT.buf,raw.data,bytes),"TT snapshot contains entire independent original AP88 image");
    Z_Free(m.data);check(!m.data,"zone invalidates tight image owner");TT.m=NULL;TT.rpsm=select_patch(&raw,p->width,p->height).psm;
    u32 rgbout=0;int expected=((u8*)raw.data)[1]==255;
    check(tt_expect(0,0,&rgbout)==expected,"TT expectation remains independent after source purge");
   }
   Z_Free(raw.data);if(m.data)Z_Free(m.data);
  }
  Patch_Free(p);
  if((r%1000)==0)fprintf(stderr,"corpus %u/%u\n",r,n);
 }
 check(fgetc(f)==EOF,"no untested corpus tail");fclose(f);
}

int main(int argc,char**argv){
 check(argc>=2,"corpus and PNG matrices supplied");
 int result=lifecycle_main(argc-1,argv+1);check(result==0,"preceding full rescue lifecycle fixture");
 synthetic_extra();full_corpus(argv[1]);free(TT.buf);TT.buf=NULL;
 for(size_t i=0;i<block_high;i++)check(!blocks[i].live,"no prototype source/image leaks");
 printf("{\"corpus_cases\":%llu,\"compared_bytes\":%llu,\"upload_cases\":%llu,\"upload_bytes\":%llu,\"compact_bytes\":%llu,\"reference_bytes\":%llu,\"checks\":%u,\"hash\":%u}\n",corpus_cases,compared_bytes,upload_cases,upload_bytes,compact_bytes,reference_bytes,checks,hash);return 0;
}
