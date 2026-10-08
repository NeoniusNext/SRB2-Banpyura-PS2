/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE OggVorbis SOFTWARE CODEC SOURCE CODE.   *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE OggVorbis SOURCE CODE IS (C) COPYRIGHT 1994-2009             *
 * by the Xiph.Org Foundation https://xiph.org/                     *
 *                                                                  *
 ********************************************************************

 function: normalized modified discrete cosine transform
           power of two length transform only [64 <= n ]

 Original algorithm adapted long ago from _The use of multirate filter
 banks for coding of high quality digital audio_, by T. Sporer,
 K. Brandenburg and B. Edler, collection of the European Signal
 Processing Conference (EUSIPCO), Amsterdam, June 1992, Vol.1, pp
 211-214

 The below code implements an algorithm that no longer looks much like
 that presented in the paper, but the basic structure remains if you
 dig deep enough to see it.

 This module DOES NOT INCLUDE code to generate/apply the window
 function.  Everybody has their own weird favorite including me... I
 happen to like the properties of y=sin(.5PI*sin^2(x)), but others may
 vehemently disagree.

 ********************************************************************/

/* this can also be run as an integer transform by uncommenting a
   define in mdct.h; the integerization is a first pass and although
   it's likely stable for Vorbis, the dynamic range is constrained and
   roundoff isn't done (so it's noisy).  Consider it functional, but
   only a starting point.  There's no point on a machine with an FPU */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "vorbis/codec.h"
#include "mdct.h"
#include "os.h"
#include "misc.h"
#ifdef PS2_VORBIS_VU0
#include "ps2_vu0a.h"
#include <stdint.h>

/* PS2-315: the same transform on VU0 (macro mode), bit-identical to the scalar code below.  The tables of the vector stages live in the
   same allocation as `trig` (after n+n/4 floats, 16-byte aligned), so mdct_lookup and mdct_clear are unchanged (the libvorbis block.c in
   libvorbis.a allocates and frees these structures).

   FFT stages (butterfly_first / generic, blocks of B floats): table of B floats per stage = B/8 pairs of {CA, CB}, see ps2_vu0a.h. */
volatile int ps2a_vu0_busy;
volatile int ps2a_vu0_disable;   /* -novu0a (i_sound.c): scalar transform, for A/B runs in one ELF */

static size_t ps2a_fft_floats(int log2n,int n){
  size_t f=0;
  int points=n>>1,stages=log2n-5,i;
  if(--stages>0)f+=points;
  for(i=1;--stages>0;i++)f+=points>>i;
  return f;
}
/* layout of the table area: [FFT stages][ROT1: n floats][ROT2: n floats][BR: 12 words per iteration, n/16 iterations] */
static size_t ps2a_tables_floats(int log2n,int n){
  return ps2a_fft_floats(log2n,n)+(size_t)n*2+(size_t)(n/16)*12;
}
static DATA_TYPE *ps2a_vt(const mdct_lookup *l){
  return (DATA_TYPE *)(((uintptr_t)(l->trig+l->n+l->n/4)+15)&~(uintptr_t)15);
}
static void ps2a_build_stage(DATA_TYPE *vt,const DATA_TYPE *trig,int B,int s){
  int ti=4<<s,k;
  for(k=0;k<B/4;k+=2){
    DATA_TYPE *c=vt+(k>>1)*8;
    int p0=B/4-1-k,p1=p0-1;
    c[0]=trig[p0*ti];   c[1]=-trig[p0*ti+1]; c[2]=trig[p1*ti];   c[3]=-trig[p1*ti+1];
    c[4]=trig[p0*ti+1]; c[5]=trig[p0*ti];    c[6]=trig[p1*ti+1]; c[7]=trig[p1*ti];
  }
}
/* ROT1 constants of ps2_vu0a.h: n/4 complex numbers, pairs of {CA, CB} */
static void ps2a_build_rot1(DATA_TYPE *tab,const DATA_TYPE *trig,int n){
  int n4=n>>2,n8=n>>3,q,h;
  for(q=0;q<n4/2;q++){
    DATA_TYPE *c=tab+q*8;
    for(h=0;h<2;h++){
      int m=2*q+h;
      if(m<n8){
        DATA_TYPE t0=trig[n4+2*(n8-1-m)],t1=trig[n4+2*(n8-1-m)+1];
        c[2*h]=-t0; c[2*h+1]=t1; c[4+2*h]=-t1; c[4+2*h+1]=-t0;
      }else{
        int s=m-n8;
        DATA_TYPE u=trig[n4-2*s-1],v=trig[n4-2*s-2];
        c[2*h]=u; c[2*h+1]=v; c[4+2*h]=v; c[4+2*h+1]=-u;
      }
    }
  }
}
/* ROT2 constants: per group of four complex numbers {T1, T0, -T0, -T1} (four lanes each), see ps2_vu0a.h */
static void ps2a_build_rot2(DATA_TYPE *tab,const DATA_TYPE *trig,int n){
  int n2=n>>1,n16=n>>4,g,l;
  for(g=0;g<n16;g++){
    DATA_TYPE *c=tab+g*16;
    for(l=0;l<4;l++){
      DATA_TYPE t0=trig[n2+2*(4*g+l)],t1=trig[n2+2*(4*g+l)+1];
      c[l]=t1; c[4+l]=t0; c[8+l]=-t0; c[12+l]=-t1;
    }
  }
}
/* BR constants: per iteration {CA, CB, four byte offsets of the gathered complex numbers}, see ps2_vu0a.h */
static void ps2a_build_br(DATA_TYPE *tab,const DATA_TYPE *trig,const int *bitrev,int n){
  int it;
  for(it=0;it<n/16;it++){
    DATA_TYPE *c=tab+it*12;
    const DATA_TYPE *T=trig+n+4*it;
    c[0]=T[0]; c[1]=T[1]; c[2]=T[2]; c[3]=T[3];
    c[4]=T[1]; c[5]=-T[0]; c[6]=T[3]; c[7]=-T[2];
    ((int *)c)[8]=bitrev[4*it]*4; ((int *)c)[9]=bitrev[4*it+1]*4; ((int *)c)[10]=bitrev[4*it+2]*4; ((int *)c)[11]=bitrev[4*it+3]*4;
  }
}
static void ps2a_build_tables(mdct_lookup *l){
  int points=l->n>>1,stages=l->log2n-5,i;
  DATA_TYPE *vt=ps2a_vt(l);
  if(--stages>0){
    ps2a_build_stage(vt,l->trig,points,0);
    vt+=points;
  }
  for(i=1;--stages>0;i++){
    ps2a_build_stage(vt,l->trig,points>>i,i);
    vt+=points>>i;
  }
  ps2a_build_rot1(vt,l->trig,l->n);
  ps2a_build_rot2(vt+l->n,l->trig,l->n);
  ps2a_build_br(vt+2*l->n,l->trig,l->bitrev,l->n);
}
static DATA_TYPE *ps2a_rot1_tab(const mdct_lookup *l){
  return ps2a_vt(l)+ps2a_fft_floats(l->log2n,l->n);
}
static DATA_TYPE *ps2a_rot2_tab(const mdct_lookup *l){
  return ps2a_rot1_tab(l)+l->n;
}
static DATA_TYPE *ps2a_br_tab(const mdct_lookup *l){
  return ps2a_rot1_tab(l)+2*l->n;
}
#endif

/* build lookups for trig functions; also pre-figure scaling and
   some window function algebra. */

void mdct_init(mdct_lookup *lookup,int n){
  int   *bitrev=_ogg_malloc(sizeof(*bitrev)*(n/4));
#ifdef PS2_VORBIS_VU0
  DATA_TYPE *T=_ogg_malloc(sizeof(*T)*(n+n/4)+16+sizeof(DATA_TYPE)*ps2a_tables_floats((int)rint(log((float)n)/log(2.f)),n));
#else
  DATA_TYPE *T=_ogg_malloc(sizeof(*T)*(n+n/4));
#endif

  int i;
  int n2=n>>1;
  int log2n=lookup->log2n=rint(log((float)n)/log(2.f));
  lookup->n=n;
  lookup->trig=T;
  lookup->bitrev=bitrev;

/* trig lookups... */

  for(i=0;i<n/4;i++){
    T[i*2]=FLOAT_CONV(cos((M_PI/n)*(4*i)));
    T[i*2+1]=FLOAT_CONV(-sin((M_PI/n)*(4*i)));
    T[n2+i*2]=FLOAT_CONV(cos((M_PI/(2*n))*(2*i+1)));
    T[n2+i*2+1]=FLOAT_CONV(sin((M_PI/(2*n))*(2*i+1)));
  }
  for(i=0;i<n/8;i++){
    T[n+i*2]=FLOAT_CONV(cos((M_PI/n)*(4*i+2))*.5);
    T[n+i*2+1]=FLOAT_CONV(-sin((M_PI/n)*(4*i+2))*.5);
  }

  /* bitreverse lookup... */

  {
    int mask=(1<<(log2n-1))-1,i,j;
    int msb=1<<(log2n-2);
    for(i=0;i<n/8;i++){
      int acc=0;
      for(j=0;msb>>j;j++)
        if((msb>>j)&i)acc|=1<<j;
      bitrev[i*2]=((~acc)&mask)-1;
      bitrev[i*2+1]=acc;

    }
  }
  lookup->scale=FLOAT_CONV(4.f/n);
#ifdef PS2_VORBIS_VU0
  ps2a_build_tables(lookup);
#endif
}

/* 8 point butterfly (in place, 4 register) */
STIN void mdct_butterfly_8(DATA_TYPE *x){
  REG_TYPE r0   = x[6] + x[2];
  REG_TYPE r1   = x[6] - x[2];
  REG_TYPE r2   = x[4] + x[0];
  REG_TYPE r3   = x[4] - x[0];

           x[6] = r0   + r2;
           x[4] = r0   - r2;

           r0   = x[5] - x[1];
           r2   = x[7] - x[3];
           x[0] = r1   + r0;
           x[2] = r1   - r0;

           r0   = x[5] + x[1];
           r1   = x[7] + x[3];
           x[3] = r2   + r3;
           x[1] = r2   - r3;
           x[7] = r1   + r0;
           x[5] = r1   - r0;

}

/* 16 point butterfly (in place, 4 register) */
STIN void mdct_butterfly_16(DATA_TYPE *x){
  REG_TYPE r0     = x[1]  - x[9];
  REG_TYPE r1     = x[0]  - x[8];

           x[8]  += x[0];
           x[9]  += x[1];
           x[0]   = MULT_NORM((r0   + r1) * cPI2_8);
           x[1]   = MULT_NORM((r0   - r1) * cPI2_8);

           r0     = x[3]  - x[11];
           r1     = x[10] - x[2];
           x[10] += x[2];
           x[11] += x[3];
           x[2]   = r0;
           x[3]   = r1;

           r0     = x[12] - x[4];
           r1     = x[13] - x[5];
           x[12] += x[4];
           x[13] += x[5];
           x[4]   = MULT_NORM((r0   - r1) * cPI2_8);
           x[5]   = MULT_NORM((r0   + r1) * cPI2_8);

           r0     = x[14] - x[6];
           r1     = x[15] - x[7];
           x[14] += x[6];
           x[15] += x[7];
           x[6]  = r0;
           x[7]  = r1;

           mdct_butterfly_8(x);
           mdct_butterfly_8(x+8);
}

/* 32 point butterfly (in place, 4 register) */
STIN void mdct_butterfly_32(DATA_TYPE *x){
  REG_TYPE r0     = x[30] - x[14];
  REG_TYPE r1     = x[31] - x[15];

           x[30] +=         x[14];
           x[31] +=         x[15];
           x[14]  =         r0;
           x[15]  =         r1;

           r0     = x[28] - x[12];
           r1     = x[29] - x[13];
           x[28] +=         x[12];
           x[29] +=         x[13];
           x[12]  = MULT_NORM( r0 * cPI1_8  -  r1 * cPI3_8 );
           x[13]  = MULT_NORM( r0 * cPI3_8  +  r1 * cPI1_8 );

           r0     = x[26] - x[10];
           r1     = x[27] - x[11];
           x[26] +=         x[10];
           x[27] +=         x[11];
           x[10]  = MULT_NORM(( r0  - r1 ) * cPI2_8);
           x[11]  = MULT_NORM(( r0  + r1 ) * cPI2_8);

           r0     = x[24] - x[8];
           r1     = x[25] - x[9];
           x[24] += x[8];
           x[25] += x[9];
           x[8]   = MULT_NORM( r0 * cPI3_8  -  r1 * cPI1_8 );
           x[9]   = MULT_NORM( r1 * cPI3_8  +  r0 * cPI1_8 );

           r0     = x[22] - x[6];
           r1     = x[7]  - x[23];
           x[22] += x[6];
           x[23] += x[7];
           x[6]   = r1;
           x[7]   = r0;

           r0     = x[4]  - x[20];
           r1     = x[5]  - x[21];
           x[20] += x[4];
           x[21] += x[5];
           x[4]   = MULT_NORM( r1 * cPI1_8  +  r0 * cPI3_8 );
           x[5]   = MULT_NORM( r1 * cPI3_8  -  r0 * cPI1_8 );

           r0     = x[2]  - x[18];
           r1     = x[3]  - x[19];
           x[18] += x[2];
           x[19] += x[3];
           x[2]   = MULT_NORM(( r1  + r0 ) * cPI2_8);
           x[3]   = MULT_NORM(( r1  - r0 ) * cPI2_8);

           r0     = x[0]  - x[16];
           r1     = x[1]  - x[17];
           x[16] += x[0];
           x[17] += x[1];
           x[0]   = MULT_NORM( r1 * cPI3_8  +  r0 * cPI1_8 );
           x[1]   = MULT_NORM( r1 * cPI1_8  -  r0 * cPI3_8 );

           mdct_butterfly_16(x);
           mdct_butterfly_16(x+16);

}

/* N point first stage butterfly (in place, 2 register) */
STIN void mdct_butterfly_first(DATA_TYPE *T,
                                        DATA_TYPE *x,
                                        int points){

  DATA_TYPE *x1        = x          + points      - 8;
  DATA_TYPE *x2        = x          + (points>>1) - 8;
  REG_TYPE   r0;
  REG_TYPE   r1;

  do{

               r0      = x1[6]      -  x2[6];
               r1      = x1[7]      -  x2[7];
               x1[6]  += x2[6];
               x1[7]  += x2[7];
               x2[6]   = MULT_NORM(r1 * T[1]  +  r0 * T[0]);
               x2[7]   = MULT_NORM(r1 * T[0]  -  r0 * T[1]);

               r0      = x1[4]      -  x2[4];
               r1      = x1[5]      -  x2[5];
               x1[4]  += x2[4];
               x1[5]  += x2[5];
               x2[4]   = MULT_NORM(r1 * T[5]  +  r0 * T[4]);
               x2[5]   = MULT_NORM(r1 * T[4]  -  r0 * T[5]);

               r0      = x1[2]      -  x2[2];
               r1      = x1[3]      -  x2[3];
               x1[2]  += x2[2];
               x1[3]  += x2[3];
               x2[2]   = MULT_NORM(r1 * T[9]  +  r0 * T[8]);
               x2[3]   = MULT_NORM(r1 * T[8]  -  r0 * T[9]);

               r0      = x1[0]      -  x2[0];
               r1      = x1[1]      -  x2[1];
               x1[0]  += x2[0];
               x1[1]  += x2[1];
               x2[0]   = MULT_NORM(r1 * T[13] +  r0 * T[12]);
               x2[1]   = MULT_NORM(r1 * T[12] -  r0 * T[13]);

    x1-=8;
    x2-=8;
    T+=16;

  }while(x2>=x);
}

/* N/stage point generic N stage butterfly (in place, 2 register) */
STIN void mdct_butterfly_generic(DATA_TYPE *T,
                                          DATA_TYPE *x,
                                          int points,
                                          int trigint){

  DATA_TYPE *x1        = x          + points      - 8;
  DATA_TYPE *x2        = x          + (points>>1) - 8;
  REG_TYPE   r0;
  REG_TYPE   r1;

  do{

               r0      = x1[6]      -  x2[6];
               r1      = x1[7]      -  x2[7];
               x1[6]  += x2[6];
               x1[7]  += x2[7];
               x2[6]   = MULT_NORM(r1 * T[1]  +  r0 * T[0]);
               x2[7]   = MULT_NORM(r1 * T[0]  -  r0 * T[1]);

               T+=trigint;

               r0      = x1[4]      -  x2[4];
               r1      = x1[5]      -  x2[5];
               x1[4]  += x2[4];
               x1[5]  += x2[5];
               x2[4]   = MULT_NORM(r1 * T[1]  +  r0 * T[0]);
               x2[5]   = MULT_NORM(r1 * T[0]  -  r0 * T[1]);

               T+=trigint;

               r0      = x1[2]      -  x2[2];
               r1      = x1[3]      -  x2[3];
               x1[2]  += x2[2];
               x1[3]  += x2[3];
               x2[2]   = MULT_NORM(r1 * T[1]  +  r0 * T[0]);
               x2[3]   = MULT_NORM(r1 * T[0]  -  r0 * T[1]);

               T+=trigint;

               r0      = x1[0]      -  x2[0];
               r1      = x1[1]      -  x2[1];
               x1[0]  += x2[0];
               x1[1]  += x2[1];
               x2[0]   = MULT_NORM(r1 * T[1]  +  r0 * T[0]);
               x2[1]   = MULT_NORM(r1 * T[0]  -  r0 * T[1]);

               T+=trigint;
    x1-=8;
    x2-=8;

  }while(x2>=x);
}

STIN void mdct_butterflies(mdct_lookup *init,
                             DATA_TYPE *x,
                             int points,
                             int vu){

  DATA_TYPE *T=init->trig;
  int stages=init->log2n-5;
  int i,j;

#ifdef PS2_VORBIS_VU0
  if(vu){
    /* PS2-315: blocks of B floats, H = upper half, L = lower half (x1 and x2 of the scalar functions), tables built in mdct_init */
    DATA_TYPE *tab=ps2a_vt(init);
    if(--stages>0){
      ps2a_fft_pairs_dispatch(vu,x+(points>>1),x,tab,points>>3);
      tab+=points;
    }
    for(i=1;--stages>0;i++){
      int B=points>>i;
      for(j=0;j<(1<<i);j++)
        ps2a_fft_pairs_dispatch(vu,x+B*j+(B>>1),x+B*j,tab,B>>3);
      tab+=B;
    }
  }else
#endif
  {
  if(--stages>0){
    mdct_butterfly_first(T,x,points);
  }

  for(i=1;--stages>0;i++){
    for(j=0;j<(1<<i);j++)
      mdct_butterfly_generic(T,x+(points>>i)*j,points>>i,4<<i);
  }
  }

  for(j=0;j<points;j+=32)
    mdct_butterfly_32(x+j);

}

void mdct_clear(mdct_lookup *l){
  if(l){
    if(l->trig)_ogg_free(l->trig);
    if(l->bitrev)_ogg_free(l->bitrev);
    memset(l,0,sizeof(*l));
  }
}

STIN void mdct_bitreverse(mdct_lookup *init,
                            DATA_TYPE *x){
  int        n       = init->n;
  int       *bit     = init->bitrev;
  DATA_TYPE *w0      = x;
  DATA_TYPE *w1      = x = w0+(n>>1);
  DATA_TYPE *T       = init->trig+n;

  do{
    DATA_TYPE *x0    = x+bit[0];
    DATA_TYPE *x1    = x+bit[1];

    REG_TYPE  r0     = x0[1]  - x1[1];
    REG_TYPE  r1     = x0[0]  + x1[0];
    REG_TYPE  r2     = MULT_NORM(r1     * T[0]   + r0 * T[1]);
    REG_TYPE  r3     = MULT_NORM(r1     * T[1]   - r0 * T[0]);

              w1    -= 4;

              r0     = HALVE(x0[1] + x1[1]);
              r1     = HALVE(x0[0] - x1[0]);

              w0[0]  = r0     + r2;
              w1[2]  = r0     - r2;
              w0[1]  = r1     + r3;
              w1[3]  = r3     - r1;

              x0     = x+bit[2];
              x1     = x+bit[3];

              r0     = x0[1]  - x1[1];
              r1     = x0[0]  + x1[0];
              r2     = MULT_NORM(r1     * T[2]   + r0 * T[3]);
              r3     = MULT_NORM(r1     * T[3]   - r0 * T[2]);

              r0     = HALVE(x0[1] + x1[1]);
              r1     = HALVE(x0[0] - x1[0]);

              w0[2]  = r0     + r2;
              w1[0]  = r0     - r2;
              w0[3]  = r1     + r3;
              w1[1]  = r3     - r1;

              T     += 4;
              bit   += 4;
              w0    += 4;

  }while(w0<w1);
}

static void mdct_backward_x(mdct_lookup *init, DATA_TYPE *in, DATA_TYPE *out, int vu);
void mdct_backward(mdct_lookup *init, DATA_TYPE *in, DATA_TYPE *out){
#ifdef PS2_VORBIS_VU0
  /* vu: 0 scalar code, 1 vector structure with the C twins of the kernels, 2 with VU0 */
  if(!ps2a_vu0_disable && !((((uintptr_t)in)|((uintptr_t)out))&15)){
#ifdef PS2A_MODEL
    mdct_backward_x(init,in,out,1);
    return;
#else
    ps2a_vu0_saved sv;
    if(ps2a_vu0_enter(&sv)){
      mdct_backward_x(init,in,out,2);
      ps2a_vu0_leave(&sv);
      return;
    }
#endif
  }
#endif
  mdct_backward_x(init,in,out,0);
}

static void mdct_backward_x(mdct_lookup *init, DATA_TYPE *in, DATA_TYPE *out, int vu){
  int n=init->n;
  int n2=n>>1;
  int n4=n>>2;

  /* rotate */

  DATA_TYPE *iX = in+n2-7;
  DATA_TYPE *oX = out+n2+n4;
  DATA_TYPE *T  = init->trig+n4;

#ifdef PS2_VORBIS_VU0
  if(vu){
    /* PS2-315: n/8 complex numbers from the odd elements (lower half), n/8 from the even ones (upper half) */
    const DATA_TYPE *rt=ps2a_rot1_tab(init);
    ps2a_rot1_lo_dispatch(vu,in,out+n2,rt,n>>4);
    ps2a_rot1_hi_dispatch(vu,in+n2-4,out+n2+n4,rt+(n>>1),n>>4);
  }else
#endif
  {
  do{
    oX         -= 4;
    oX[0]       = MULT_NORM(-iX[2] * T[3] - iX[0]  * T[2]);
    oX[1]       = MULT_NORM (iX[0] * T[3] - iX[2]  * T[2]);
    oX[2]       = MULT_NORM(-iX[6] * T[1] - iX[4]  * T[0]);
    oX[3]       = MULT_NORM (iX[4] * T[1] - iX[6]  * T[0]);
    iX         -= 8;
    T          += 4;
  }while(iX>=in);

  iX            = in+n2-8;
  oX            = out+n2+n4;
  T             = init->trig+n4;

  do{
    T          -= 4;
    oX[0]       =  MULT_NORM (iX[4] * T[3] + iX[6] * T[2]);
    oX[1]       =  MULT_NORM (iX[4] * T[2] - iX[6] * T[3]);
    oX[2]       =  MULT_NORM (iX[0] * T[1] + iX[2] * T[0]);
    oX[3]       =  MULT_NORM (iX[0] * T[0] - iX[2] * T[1]);
    iX         -= 8;
    oX         += 4;
  }while(iX>=in);
  }

  mdct_butterflies(init,out+n2,n2,vu);
#ifdef PS2_VORBIS_VU0
  if(vu)
    ps2a_bitrev_dispatch(vu,out+n2,out,out+n2,ps2a_br_tab(init),n>>4);
  else
#endif
  mdct_bitreverse(init,out);

  /* roatate + window */

#ifdef PS2_VORBIS_VU0
  if(vu){
    /* PS2-315: out = [ rev(u) | -u | rev(v) | v ]; u goes through the upper half as the scalar code does */
    int n16=n>>4;
    ps2a_rot2_a_dispatch(vu,out,out+n2,out+n2+n4,ps2a_rot2_tab(init),n16);
    ps2a_rev_neg_dispatch(vu,out+n2,out+n4-4,out+n4,n16);
    ps2a_rev_dispatch(vu,out+n2+n4,out+n2+n4-4,n16);
  }else
#endif
  {
    DATA_TYPE *oX1=out+n2+n4;
    DATA_TYPE *oX2=out+n2+n4;
    DATA_TYPE *iX =out;
    T             =init->trig+n2;

    do{
      oX1-=4;

      oX1[3]  =  MULT_NORM (iX[0] * T[1] - iX[1] * T[0]);
      oX2[0]  = -MULT_NORM (iX[0] * T[0] + iX[1] * T[1]);

      oX1[2]  =  MULT_NORM (iX[2] * T[3] - iX[3] * T[2]);
      oX2[1]  = -MULT_NORM (iX[2] * T[2] + iX[3] * T[3]);

      oX1[1]  =  MULT_NORM (iX[4] * T[5] - iX[5] * T[4]);
      oX2[2]  = -MULT_NORM (iX[4] * T[4] + iX[5] * T[5]);

      oX1[0]  =  MULT_NORM (iX[6] * T[7] - iX[7] * T[6]);
      oX2[3]  = -MULT_NORM (iX[6] * T[6] + iX[7] * T[7]);

      oX2+=4;
      iX    +=   8;
      T     +=   8;
    }while(iX<oX1);

    iX=out+n2+n4;
    oX1=out+n4;
    oX2=oX1;

    do{
      oX1-=4;
      iX-=4;

      oX2[0] = -(oX1[3] = iX[3]);
      oX2[1] = -(oX1[2] = iX[2]);
      oX2[2] = -(oX1[1] = iX[1]);
      oX2[3] = -(oX1[0] = iX[0]);

      oX2+=4;
    }while(oX2<iX);

    iX=out+n2+n4;
    oX1=out+n2+n4;
    oX2=out+n2;
    do{
      oX1-=4;
      oX1[0]= iX[3];
      oX1[1]= iX[2];
      oX1[2]= iX[1];
      oX1[3]= iX[0];
      iX+=4;
    }while(oX1>oX2);
  }
}

void mdct_forward(mdct_lookup *init, DATA_TYPE *in, DATA_TYPE *out){
  int n=init->n;
  int n2=n>>1;
  int n4=n>>2;
  int n8=n>>3;
  DATA_TYPE *w=alloca(n*sizeof(*w)); /* forward needs working space */
  DATA_TYPE *w2=w+n2;

  /* rotate */

  /* window + rotate + step 1 */

  REG_TYPE r0;
  REG_TYPE r1;
  DATA_TYPE *x0=in+n2+n4;
  DATA_TYPE *x1=x0+1;
  DATA_TYPE *T=init->trig+n2;

  int i=0;

  for(i=0;i<n8;i+=2){
    x0 -=4;
    T-=2;
    r0= x0[2] + x1[0];
    r1= x0[0] + x1[2];
    w2[i]=   MULT_NORM(r1*T[1] + r0*T[0]);
    w2[i+1]= MULT_NORM(r1*T[0] - r0*T[1]);
    x1 +=4;
  }

  x1=in+1;

  for(;i<n2-n8;i+=2){
    T-=2;
    x0 -=4;
    r0= x0[2] - x1[0];
    r1= x0[0] - x1[2];
    w2[i]=   MULT_NORM(r1*T[1] + r0*T[0]);
    w2[i+1]= MULT_NORM(r1*T[0] - r0*T[1]);
    x1 +=4;
  }

  x0=in+n;

  for(;i<n2;i+=2){
    T-=2;
    x0 -=4;
    r0= -x0[2] - x1[0];
    r1= -x0[0] - x1[2];
    w2[i]=   MULT_NORM(r1*T[1] + r0*T[0]);
    w2[i+1]= MULT_NORM(r1*T[0] - r0*T[1]);
    x1 +=4;
  }


  mdct_butterflies(init,w+n2,n2,0);
  mdct_bitreverse(init,w);

  /* roatate + window */

  T=init->trig+n2;
  x0=out+n2;

  for(i=0;i<n4;i++){
    x0--;
    out[i] =MULT_NORM((w[0]*T[0]+w[1]*T[1])*init->scale);
    x0[0]  =MULT_NORM((w[0]*T[1]-w[1]*T[0])*init->scale);
    w+=2;
    T+=2;
  }
}
