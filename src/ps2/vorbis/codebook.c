/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE OggVorbis SOFTWARE CODEC SOURCE CODE.   *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE OggVorbis SOURCE CODE IS (C) COPYRIGHT 1994-2015             *
 * by the Xiph.Org Foundation https://xiph.org/                     *
 *                                                                  *
 ********************************************************************

 function: basic codebook pack/unpack/code/decode operations

 ********************************************************************/

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ogg/ogg.h>
#include "vorbis/codec.h"
#include "codebook.h"
#include "scales.h"
#include "misc.h"
#include "os.h"

/* packs the given codebook into the bitstream **************************/

int vorbis_staticbook_pack(const static_codebook *c,oggpack_buffer *opb){
  long i,j;
  int ordered=0;

  /* first the basic parameters */
  oggpack_write(opb,0x564342,24);
  oggpack_write(opb,c->dim,16);
  oggpack_write(opb,c->entries,24);

  /* pack the codewords.  There are two packings; length ordered and
     length random.  Decide between the two now. */

  for(i=1;i<c->entries;i++)
    if(c->lengthlist[i-1]==0 || c->lengthlist[i]<c->lengthlist[i-1])break;
  if(i==c->entries)ordered=1;

  if(ordered){
    /* length ordered.  We only need to say how many codewords of
       each length.  The actual codewords are generated
       deterministically */

    long count=0;
    oggpack_write(opb,1,1);  /* ordered */
    oggpack_write(opb,c->lengthlist[0]-1,5); /* 1 to 32 */

    for(i=1;i<c->entries;i++){
      char this=c->lengthlist[i];
      char last=c->lengthlist[i-1];
      if(this>last){
        for(j=last;j<this;j++){
          oggpack_write(opb,i-count,ov_ilog(c->entries-count));
          count=i;
        }
      }
    }
    oggpack_write(opb,i-count,ov_ilog(c->entries-count));

  }else{
    /* length random.  Again, we don't code the codeword itself, just
       the length.  This time, though, we have to encode each length */
    oggpack_write(opb,0,1);   /* unordered */

    /* algortihmic mapping has use for 'unused entries', which we tag
       here.  The algorithmic mapping happens as usual, but the unused
       entry has no codeword. */
    for(i=0;i<c->entries;i++)
      if(c->lengthlist[i]==0)break;

    if(i==c->entries){
      oggpack_write(opb,0,1); /* no unused entries */
      for(i=0;i<c->entries;i++)
        oggpack_write(opb,c->lengthlist[i]-1,5);
    }else{
      oggpack_write(opb,1,1); /* we have unused entries; thus we tag */
      for(i=0;i<c->entries;i++){
        if(c->lengthlist[i]==0){
          oggpack_write(opb,0,1);
        }else{
          oggpack_write(opb,1,1);
          oggpack_write(opb,c->lengthlist[i]-1,5);
        }
      }
    }
  }

  /* is the entry number the desired return value, or do we have a
     mapping? If we have a mapping, what type? */
  oggpack_write(opb,c->maptype,4);
  switch(c->maptype){
  case 0:
    /* no mapping */
    break;
  case 1:case 2:
    /* implicitly populated value mapping */
    /* explicitly populated value mapping */

    if(!c->quantlist){
      /* no quantlist?  error */
      return(-1);
    }

    /* values that define the dequantization */
    oggpack_write(opb,c->q_min,32);
    oggpack_write(opb,c->q_delta,32);
    oggpack_write(opb,c->q_quant-1,4);
    oggpack_write(opb,c->q_sequencep,1);

    {
      int quantvals;
      switch(c->maptype){
      case 1:
        /* a single column of (c->entries/c->dim) quantized values for
           building a full value list algorithmically (square lattice) */
        quantvals=_book_maptype1_quantvals(c);
        break;
      case 2:
        /* every value (c->entries*c->dim total) specified explicitly */
        quantvals=c->entries*c->dim;
        break;
      default: /* NOT_REACHABLE */
        quantvals=-1;
      }

      /* quantized values */
      for(i=0;i<quantvals;i++)
        oggpack_write(opb,labs(c->quantlist[i]),c->q_quant);

    }
    break;
  default:
    /* error case; we don't have any other map types now */
    return(-1);
  }

  return(0);
}

/* unpacks a codebook from the packet buffer into the codebook struct,
   readies the codebook auxiliary structures for decode *************/
static_codebook *vorbis_staticbook_unpack(oggpack_buffer *opb){
  long i,j;
  static_codebook *s=_ogg_calloc(1,sizeof(*s));
  s->allocedp=1;

  /* make sure alignment is correct */
  if(oggpack_read(opb,24)!=0x564342)goto _eofout;

  /* first the basic parameters */
  s->dim=oggpack_read(opb,16);
  s->entries=oggpack_read(opb,24);
  if(s->entries==-1)goto _eofout;

  if(ov_ilog(s->dim)+ov_ilog(s->entries)>24)goto _eofout;

  /* codeword ordering.... length ordered or unordered? */
  switch((int)oggpack_read(opb,1)){
  case 0:{
    long unused;
    /* allocated but unused entries? */
    unused=oggpack_read(opb,1);
    if((s->entries*(unused?1:5)+7)>>3>opb->storage-oggpack_bytes(opb))
      goto _eofout;
    /* unordered */
    s->lengthlist=_ogg_malloc(sizeof(*s->lengthlist)*s->entries);

    /* allocated but unused entries? */
    if(unused){
      /* yes, unused entries */

      for(i=0;i<s->entries;i++){
        if(oggpack_read(opb,1)){
          long num=oggpack_read(opb,5);
          if(num==-1)goto _eofout;
          s->lengthlist[i]=num+1;
        }else
          s->lengthlist[i]=0;
      }
    }else{
      /* all entries used; no tagging */
      for(i=0;i<s->entries;i++){
        long num=oggpack_read(opb,5);
        if(num==-1)goto _eofout;
        s->lengthlist[i]=num+1;
      }
    }

    break;
  }
  case 1:
    /* ordered */
    {
      long length=oggpack_read(opb,5)+1;
      if(length==0)goto _eofout;
      s->lengthlist=_ogg_malloc(sizeof(*s->lengthlist)*s->entries);

      for(i=0;i<s->entries;){
        long num=oggpack_read(opb,ov_ilog(s->entries-i));
        if(num==-1)goto _eofout;
        if(length>32 || num>s->entries-i ||
           (num>0 && (num-1)>>(length-1)>1)){
          goto _errout;
        }
        if(length>32)goto _errout;
        for(j=0;j<num;j++,i++)
          s->lengthlist[i]=length;
        length++;
      }
    }
    break;
  default:
    /* EOF */
    goto _eofout;
  }

  /* Do we have a mapping to unpack? */
  switch((s->maptype=oggpack_read(opb,4))){
  case 0:
    /* no mapping */
    break;
  case 1: case 2:
    /* implicitly populated value mapping */
    /* explicitly populated value mapping */

    s->q_min=oggpack_read(opb,32);
    s->q_delta=oggpack_read(opb,32);
    s->q_quant=oggpack_read(opb,4)+1;
    s->q_sequencep=oggpack_read(opb,1);
    if(s->q_sequencep==-1)goto _eofout;

    {
      int quantvals=0;
      switch(s->maptype){
      case 1:
        quantvals=(s->dim==0?0:_book_maptype1_quantvals(s));
        break;
      case 2:
        quantvals=s->entries*s->dim;
        break;
      }

      /* quantized values */
      if(((quantvals*s->q_quant+7)>>3)>opb->storage-oggpack_bytes(opb))
        goto _eofout;
      s->quantlist=_ogg_malloc(sizeof(*s->quantlist)*quantvals);
      for(i=0;i<quantvals;i++)
        s->quantlist[i]=oggpack_read(opb,s->q_quant);

      if(quantvals&&s->quantlist[quantvals-1]==-1)goto _eofout;
    }
    break;
  default:
    goto _errout;
  }

  /* all set */
  return(s);

 _errout:
 _eofout:
  vorbis_staticbook_destroy(s);
  return(NULL);
}

/* returns the number of bits ************************************************/
int vorbis_book_encode(codebook *book, int a, oggpack_buffer *b){
  if(a<0 || a>=book->c->entries)return(0);
  oggpack_write(b,book->codelist[a],book->c->lengthlist[a]);
  return(book->c->lengthlist[a]);
}

/* the 'eliminate the decode tree' optimization actually requires the
   codewords to be MSb first, not LSb.  This is an annoying inelegancy
   (and one of the first places where carefully thought out design
   turned out to be wrong; Vorbis II and future Ogg codecs should go
   to an MSb bitpacker), but not actually the huge hit it appears to
   be.  The first-stage decode table catches most words so that
   bitreverse is not in the main execution path. */

static ogg_uint32_t bitreverse(ogg_uint32_t x){
  x=    ((x>>16)&0x0000ffff) | ((x<<16)&0xffff0000);
  x=    ((x>> 8)&0x00ff00ff) | ((x<< 8)&0xff00ff00);
  x=    ((x>> 4)&0x0f0f0f0f) | ((x<< 4)&0xf0f0f0f0);
  x=    ((x>> 2)&0x33333333) | ((x<< 2)&0xcccccccc);
  return((x>> 1)&0x55555555) | ((x<< 1)&0xaaaaaaaa);
}

STIN long decode_packed_entry_number(codebook *book, oggpack_buffer *b){
  int  read=book->dec_maxlength;
  long lo,hi;
  long lok;

#if defined(PS2_VORBIS_FAST)
  /* PS2-311: the first-stage table lookup without the two calls into libogg (oggpack_look, oggpack_adv). The look-ahead of dec_firsttablen
     (<= 8) bits is the little-endian word at ptr shifted by endbit, and a hit advances by its code length (<= 8): the same bits, the same position
     as the generic path below.
     PS2-313: (1) the last 8 bytes of the packet are no longer left to the generic path: the word is assembled from the bytes that are there (the
     rest zero) and used only for as many bits as remain (oggpack_look fails when fewer than `bits` remain); (2) a code longer than the first-stage
     table is bisected here on the same look-ahead bits (dec_maxlength of them, which must remain), without oggpack_look / oggpack_adv. Everything
     else (fewer bits left than needed, the end-of-packet cases and their return values) takes the generic path. */
  {
    long avail=b->storage-b->endbyte;
    if(avail>0 && b->ptr){
      ogg_uint64_t w;
      long entry;
      long remain;
      if(avail>=8){
        __builtin_memcpy(&w,b->ptr,8);
        remain=64-b->endbit;
      }else{
        long k;
        w=0;
        for(k=0;k<avail;k++)w|=(ogg_uint64_t)b->ptr[k]<<(8*k);
        remain=avail*8-b->endbit;
      }
      w>>=b->endbit;
      if(remain>=book->dec_firsttablen){
        entry=book->dec_firsttable[w&((1UL<<book->dec_firsttablen)-1)];
        if(!(entry&0x80000000UL)){
          int bits=b->endbit+book->dec_codelengths[entry-1];
          b->ptr+=bits>>3;
          b->endbyte+=bits>>3;
          b->endbit=bits&7;
          return(entry-1);
        }else if(remain>=read){
          lo=(entry>>15)&0x7fff;
          hi=book->used_entries-(entry&0x7fff);
          {
            ogg_uint32_t testword=bitreverse((ogg_uint32_t)(w&(read>=32?0xffffffffUL:((1UL<<read)-1))));
            int bbits;
            while(hi-lo>1){
              long p=(hi-lo)>>1;
              long test=book->codelist[lo+p]>testword;
              lo+=p&(test-1);
              hi-=p&(-test);
            }
            if(book->dec_codelengths[lo]<=read){
              bbits=b->endbit+book->dec_codelengths[lo];
              b->ptr+=bbits>>3; b->endbyte+=bbits>>3; b->endbit=bbits&7;
              return(lo);
            }
            bbits=b->endbit+read;
            b->ptr+=bbits>>3; b->endbyte+=bbits>>3; b->endbit=bbits&7;
            return(-1);
          }
        }
      }
    }
  }
#endif

  lok = oggpack_look(b,book->dec_firsttablen);

  if (lok >= 0) {
    long entry = book->dec_firsttable[lok];
    if(entry&0x80000000UL){
      lo=(entry>>15)&0x7fff;
      hi=book->used_entries-(entry&0x7fff);
    }else{
      oggpack_adv(b, book->dec_codelengths[entry-1]);
      return(entry-1);
    }
  }else{
    lo=0;
    hi=book->used_entries;
  }

  /* Single entry codebooks use a firsttablen of 1 and a
     dec_maxlength of 1.  If a single-entry codebook gets here (due to
     failure to read one bit above), the next look attempt will also
     fail and we'll correctly kick out instead of trying to walk the
     underformed tree */

  lok = oggpack_look(b, read);

  while(lok<0 && read>1)
    lok = oggpack_look(b, --read);
  if(lok<0)return -1;

  /* bisect search for the codeword in the ordered list */
  {
    ogg_uint32_t testword=bitreverse((ogg_uint32_t)lok);

    while(hi-lo>1){
      long p=(hi-lo)>>1;
      long test=book->codelist[lo+p]>testword;
      lo+=p&(test-1);
      hi-=p&(-test);
      }

    if(book->dec_codelengths[lo]<=read){
      oggpack_adv(b, book->dec_codelengths[lo]);
      return(lo);
    }
  }

  oggpack_adv(b, read);

  return(-1);
}

/* Decode side is specced and easier, because we don't need to find
   matches using different criteria; we simply read and map.  There are
   two things we need to do 'depending':

   We may need to support interleave.  We don't really, but it's
   convenient to do it here rather than rebuild the vector later.

   Cascades may be additive or multiplicitive; this is not inherent in
   the codebook, but set in the code using the codebook.  Like
   interleaving, it's easiest to do it here.
   addmul==0 -> declarative (set the value)
   addmul==1 -> additive
   addmul==2 -> multiplicitive */

/* returns the [original, not compacted] entry number or -1 on eof *********/
long vorbis_book_decode(codebook *book, oggpack_buffer *b){
  if(book->used_entries>0){
    long packed_entry=decode_packed_entry_number(book,b);
    if(packed_entry>=0)
      return(book->dec_index[packed_entry]);
  }

  /* if there's no dec_index, the codebook unpacking isn't collapsed */
  return(-1);
}

/* returns 0 on OK or -1 on eof *************************************/
/* decode vector / dim granularity gaurding is done in the upper layer */
long vorbis_book_decodevs_add(codebook *book,float *a,oggpack_buffer *b,int n){
  if(book->used_entries>0){
    int step=n/book->dim;
    long *entry = alloca(sizeof(*entry)*step);
    float **t = alloca(sizeof(*t)*step);
    int i,j,o;

    for (i = 0; i < step; i++) {
      entry[i]=decode_packed_entry_number(book,b);
      if(entry[i]==-1)return(-1);
      t[i] = book->valuelist+entry[i]*book->dim;
    }
    for(i=0,o=0;i<book->dim;i++,o+=step)
      for (j=0;o+j<n && j<step;j++)
        a[o+j]+=t[j][i];
  }
  return(0);
}

/* decode vector / dim granularity gaurding is done in the upper layer */
long vorbis_book_decodev_add(codebook *book,float *a,oggpack_buffer *b,int n){
  if(book->used_entries>0){
    int i,j,entry;
    float *t;

    for(i=0;i<n;){
      entry = decode_packed_entry_number(book,b);
      if(entry==-1)return(-1);
      t     = book->valuelist+entry*book->dim;
      for(j=0;i<n && j<book->dim;)
        a[i++]+=t[j++];
    }
  }
  return(0);
}

/* unlike the others, we guard against n not being an integer number
   of <dim> internally rather than in the upper layer (called only by
   floor0) */
long vorbis_book_decodev_set(codebook *book,float *a,oggpack_buffer *b,int n){
  if(book->used_entries>0){
    int i,j,entry;
    float *t;

    for(i=0;i<n;){
      entry = decode_packed_entry_number(book,b);
      if(entry==-1)return(-1);
      t     = book->valuelist+entry*book->dim;
      for (j=0;i<n && j<book->dim;){
        a[i++]=t[j++];
      }
    }
  }else{
    int i;

    for(i=0;i<n;){
      a[i++]=0.f;
    }
  }
  return(0);
}

/* PS2-312: vorbis_book_decodevv_add with a bin limit and loops without the per-element channel bookkeeping. The stream is interleaved
   (ch values per frame i); with `lim` = the number of bins the transform will read (half-rate decoding uses the lower half only), frames
   i >= lim are decoded (bits consumed, entries looked up) but not added. For i < lim every sum is the same float operation on the same
   operands as in vorbis_book_decodevv_add. */
long vorbis_book_decodevv_add_lim(codebook *book,float **a,long offset,int ch,
                                  oggpack_buffer *b,int n,long lim){
  long i,j,entry;
  int dim;
  if(book->used_entries>0){
    int m=(offset+n)/ch;
    dim=book->dim;
    i=offset/ch;
    if(ch==2 && !(dim&1)){
      float *a0=a[0],*a1=a[1];
      while(i<m){
        entry = decode_packed_entry_number(book,b);
        if(entry==-1)return(-1);
        {
          const float *t = book->valuelist+entry*dim;
          for(j=0;j<dim && i<m;j+=2,i++)
            if(i<lim){
              a0[i]+=t[j];
              a1[i]+=t[j+1];
            }
        }
      }
    }else if(ch==1){
      float *a0=a[0];
      while(i<m){
        entry = decode_packed_entry_number(book,b);
        if(entry==-1)return(-1);
        {
          const float *t = book->valuelist+entry*dim;
          for(j=0;j<dim && i<m;j++,i++)
            if(i<lim)a0[i]+=t[j];
        }
      }
    }else{
      int chptr=0;
      while(i<m){
        entry = decode_packed_entry_number(book,b);
        if(entry==-1)return(-1);
        {
          const float *t = book->valuelist+entry*dim;
          for (j=0;i<m && j<dim;j++){
            if(i<lim)a[chptr][i]+=t[j];
            chptr++;
            if(chptr==ch){
              chptr=0;
              i++;
            }
          }
        }
      }
    }
  }
  return(0);
}

long vorbis_book_decodevv_add(codebook *book,float **a,long offset,int ch,
                              oggpack_buffer *b,int n){

  long i,j,entry;
  int chptr=0;
  if(book->used_entries>0){
    int m=(offset+n)/ch;
    for(i=offset/ch;i<m;){
      entry = decode_packed_entry_number(book,b);
      if(entry==-1)return(-1);
      {
        const float *t = book->valuelist+entry*book->dim;
        for (j=0;i<m && j<book->dim;j++){
          a[chptr++][i]+=t[j];
          if(chptr==ch){
            chptr=0;
            i++;
          }
        }
      }
    }
  }
  return(0);
}

/* PS2-313 ---------------------------------------------------------------------------------------------------------------------------------- */
ps2_fastbook *ps2_fastbook_build(const codebook *book){
  /* The table is indexed by up to 10 bits of look-ahead (the books of the stream have first tables of only 5..8 bits: a third of the symbols of a
     stream at normal quality were longer than that and went through the bisect of the general path).  Same construction as vorbis_book_init_decode:
     every codeword of length <= tlen fills all its suffix extensions. */
  int tlen,i;
  unsigned n,j;
  ps2_fastbook *fb;
  if(book->used_entries<=0 || !book->dec_codelengths || !book->codelist || book->dec_maxlength<1)return(NULL);
  if(book->valuelist && (unsigned long)book->used_entries*(unsigned long)book->dim*sizeof(float)>=(1UL<<26))return(NULL);
  if(!book->valuelist && (!book->dec_index || book->entries>=(1L<<26)))return(NULL);
  tlen=book->dec_maxlength<10?book->dec_maxlength:10;
  n=1u<<tlen;
  fb=_ogg_malloc(sizeof(*fb)+sizeof(ogg_uint32_t)*n);
  if(!fb)return(NULL);
  memset(fb->ft,0,sizeof(ogg_uint32_t)*n);
  fb->mask=n-1;
  fb->dim=book->dim;
  if(book->used_entries==1 && book->dec_maxlength==1){
    /* the single entry book: one bit, always entry 0 (the table of the general path is {1,1}) */
    for(i=0;i<(int)n;i++)fb->ft[i]=1;
    if(book->valuelist){
      const float *t=book->valuelist;
      long q;
      int z=1;
      for(q=0;q<book->dim;q++)if(t[q]!=0.f)z=0;
      for(i=0;i<(int)n;i++)fb->ft[i]=(z?16u:0u)|1u;
    }else{
      for(i=0;i<(int)n;i++)fb->ft[i]=((ogg_uint32_t)book->dec_index[0]<<5)|1u;
    }
    return(fb);
  }
  for(i=0;i<book->used_entries;i++){
    int len=book->dec_codelengths[i];
    if(len<=tlen){
      ogg_uint32_t orig=bitreverse(book->codelist[i]),word;
      if(book->valuelist){
        const float *t=book->valuelist+(long)i*book->dim;
        long q;
        int z=1;
        for(q=0;q<book->dim;q++)if(t[q]!=0.f)z=0;
        word=((ogg_uint32_t)((long)i*book->dim*(long)sizeof(float))<<5)|(z?16u:0u)|(ogg_uint32_t)len;
      }else{
        word=((ogg_uint32_t)book->dec_index[i]<<5)|(ogg_uint32_t)len;
      }
      for(j=0;j<(1u<<(tlen-len));j++)fb->ft[orig|(j<<len)]=word;
    }
  }
  return(fb);
}

#if defined(_EE) && defined(PS2_VORBIS_FAST)
/* PS2-313: the hot loop of the fast residue decode in assembly.  Runs symbols while the first-stage table hits, the packet has at least 8 more
   bytes and the whole vector (HALF pairs) lies before `alim`; leaves at the first symbol that needs the general path (the caller decodes it).
   The operations on the data are the C ones (one add.s per element, same order); nothing but the speed differs.
   a0 / a1: pointers to the current element of the two channels, bp: bit position. */
#define PS2_RUN_HEAD \
  "1:\n" \
  "sltu %[t0], %[bp], %[bplim]\n" \
  "beqz %[t0], 9f\n" \
  " srl %[t1], %[bp], 3\n" \
  "addu %[t1], %[t1], %[base]\n" \
  "ldl %[t2], 7(%[t1])\n" \
  "ldr %[t2], 0(%[t1])\n" \
  "andi %[t3], %[bp], 7\n" \
  "dsrlv %[t2], %[t2], %[t3]\n" \
  "and %[t2], %[t2], %[mask]\n" \
  "sll %[t2], %[t2], 2\n" \
  "addu %[t2], %[t2], %[ft]\n" \
  "lw %[t4], 0(%[t2])\n" \
  "beqz %[t4], 9f\n" \
  " andi %[t5], %[t4], 15\n" \
  "addu %[bp], %[bp], %[t5]\n" \
  "andi %[t6], %[t4], 16\n" \
  "bnez %[t6], 2f\n" \
  " srl %[t7], %[t4], 5\n" \
  "addu %[t7], %[t7], %[vl]\n"
#define PS2_RUN_TAIL(STEP) \
  "2:\n" \
  "addiu %[a0], %[a0], " STEP "\n" \
  "addiu %[a1], %[a1], " STEP "\n" \
  "sltu %[t0], %[a0], %[alim]\n" \
  "bnez %[t0], 1b\n" \
  " nop\n" \
  "9:\n"
#define PS2_RUN_OPERANDS \
  : [a0] "+&r"(a0), [a1] "+&r"(a1), [bp] "+&r"(bp), \
    [t0] "=&r"(t0), [t1] "=&r"(t1), [t2] "=&r"(t2), [t3] "=&r"(t3), [t4] "=&r"(t4), [t5] "=&r"(t5), [t6] "=&r"(t6), [t7] "=&r"(t7) \
  : [base] "r"(base), [ft] "r"(ft), [mask] "r"(mask), [vl] "r"(vl), [alim] "r"(alim), [bplim] "r"(bplim) \
  : "memory", "$f0", "$f1", "$f2", "$f3", "$f4", "$f5", "$f6", "$f7", "$f8", "$f9", "$f10", "$f11", "$f12", "$f13", "$f14", "$f15"

static void ps2_run1(float **pa0,float **pa1,unsigned long *pbp,const unsigned char *base,const ogg_uint32_t *ft,unsigned mask,
                     const char *vl,const float *alim,unsigned long bplim){
  float *a0=*pa0,*a1=*pa1;
  unsigned long bp=*pbp;
  long t0,t1,t2,t3,t4,t5,t6,t7;
  __asm__ volatile(
    ".set push\n.set noreorder\n"
    PS2_RUN_HEAD
    "lwc1 $f0, 0(%[t7])\n"
    "lwc1 $f1, 4(%[t7])\n"
    "lwc1 $f2, 0(%[a0])\n"
    "lwc1 $f3, 0(%[a1])\n"
    "add.s $f2, $f2, $f0\n"
    "add.s $f3, $f3, $f1\n"
    "swc1 $f2, 0(%[a0])\n"
    "swc1 $f3, 0(%[a1])\n"
    PS2_RUN_TAIL("4")
    ".set pop\n"
    PS2_RUN_OPERANDS);
  *pa0=a0; *pa1=a1; *pbp=bp;
}
static void ps2_run2(float **pa0,float **pa1,unsigned long *pbp,const unsigned char *base,const ogg_uint32_t *ft,unsigned mask,
                     const char *vl,const float *alim,unsigned long bplim){
  float *a0=*pa0,*a1=*pa1;
  unsigned long bp=*pbp;
  long t0,t1,t2,t3,t4,t5,t6,t7;
  __asm__ volatile(
    ".set push\n.set noreorder\n"
    PS2_RUN_HEAD
    "lwc1 $f0, 0(%[t7])\n"
    "lwc1 $f1, 4(%[t7])\n"
    "lwc1 $f2, 8(%[t7])\n"
    "lwc1 $f3, 12(%[t7])\n"
    "lwc1 $f4, 0(%[a0])\n"
    "lwc1 $f5, 4(%[a0])\n"
    "lwc1 $f6, 0(%[a1])\n"
    "lwc1 $f7, 4(%[a1])\n"
    "add.s $f4, $f4, $f0\n"
    "add.s $f6, $f6, $f1\n"
    "add.s $f5, $f5, $f2\n"
    "add.s $f7, $f7, $f3\n"
    "swc1 $f4, 0(%[a0])\n"
    "swc1 $f6, 0(%[a1])\n"
    "swc1 $f5, 4(%[a0])\n"
    "swc1 $f7, 4(%[a1])\n"
    PS2_RUN_TAIL("8")
    ".set pop\n"
    PS2_RUN_OPERANDS);
  *pa0=a0; *pa1=a1; *pbp=bp;
}
static void ps2_run4(float **pa0,float **pa1,unsigned long *pbp,const unsigned char *base,const ogg_uint32_t *ft,unsigned mask,
                     const char *vl,const float *alim,unsigned long bplim){
  float *a0=*pa0,*a1=*pa1;
  unsigned long bp=*pbp;
  long t0,t1,t2,t3,t4,t5,t6,t7;
  __asm__ volatile(
    ".set push\n.set noreorder\n"
    PS2_RUN_HEAD
    "lwc1 $f0, 0(%[t7])\n"
    "lwc1 $f1, 4(%[t7])\n"
    "lwc1 $f2, 8(%[t7])\n"
    "lwc1 $f3, 12(%[t7])\n"
    "lwc1 $f4, 16(%[t7])\n"
    "lwc1 $f5, 20(%[t7])\n"
    "lwc1 $f6, 24(%[t7])\n"
    "lwc1 $f7, 28(%[t7])\n"
    "lwc1 $f8, 0(%[a0])\n"
    "lwc1 $f9, 4(%[a0])\n"
    "lwc1 $f10, 8(%[a0])\n"
    "lwc1 $f11, 12(%[a0])\n"
    "lwc1 $f12, 0(%[a1])\n"
    "lwc1 $f13, 4(%[a1])\n"
    "lwc1 $f14, 8(%[a1])\n"
    "lwc1 $f15, 12(%[a1])\n"
    "add.s $f8, $f8, $f0\n"
    "add.s $f12, $f12, $f1\n"
    "add.s $f9, $f9, $f2\n"
    "add.s $f13, $f13, $f3\n"
    "add.s $f10, $f10, $f4\n"
    "add.s $f14, $f14, $f5\n"
    "add.s $f11, $f11, $f6\n"
    "add.s $f15, $f15, $f7\n"
    "swc1 $f8, 0(%[a0])\n"
    "swc1 $f12, 0(%[a1])\n"
    "swc1 $f9, 4(%[a0])\n"
    "swc1 $f13, 4(%[a1])\n"
    "swc1 $f10, 8(%[a0])\n"
    "swc1 $f14, 8(%[a1])\n"
    "swc1 $f11, 12(%[a0])\n"
    "swc1 $f15, 12(%[a1])\n"
    PS2_RUN_TAIL("16")
    ".set pop\n"
    PS2_RUN_OPERANDS);
  *pa0=a0; *pa1=a1; *pbp=bp;
}
#endif

/* The interleaved two-channel decode of vorbis_book_decodevv_add_lim (ch == 2, even dim) with the bit reader in registers and no per-element
   tests when the whole vector lies inside the transformed bins.  Bits are consumed exactly as by decode_packed_entry_number (a code longer than the
   first table, and the last 8 bytes of the packet, take that function).  Vectors that are all zero are not added (adding +-0 changes at most the
   sign of a zero).  On the EE the loop over the symbols that hit the first table is assembly (ps2_run1/2/4). */
long ps2_book_decodevv2_add(const ps2_fastbook *fb,codebook *book,float **a,long offset,
                            oggpack_buffer *b,int n,long lim){
  long i=offset>>1,m=(offset+n)>>1;
  const long dim=fb->dim,half=dim>>1;
  const long mi=m<lim?m:lim;   /* a vector is added without per-element tests while it ends at or before mi */
  float *a0=a[0],*a1=a[1];
  const char *vl=(const char *)book->valuelist;
  const unsigned char *base=b->buffer;
  const unsigned mask=fb->mask;
  const ogg_uint32_t *ft=fb->ft;
  unsigned long bp=(unsigned long)b->endbyte*8+(unsigned long)b->endbit;
  const unsigned long bplim=b->storage>=8?(unsigned long)(b->storage-7)*8:0;
  if(book->used_entries<=0)return(0);
  while(i<m){
    ogg_uint32_t e=0;
    long off;
    int zero=0;
#if defined(_EE) && defined(PS2_VORBIS_FAST)
    if(i+half<=mi && (half==1 || half==2 || half==4)){
      float *p0=a0+i,*p1=a1+i;
      const float *alim=a0+(mi-half+1);   /* another vector starts while p0 < alim */
      if(half==1)ps2_run1(&p0,&p1,&bp,base,ft,mask,vl,alim,bplim);
      else if(half==2)ps2_run2(&p0,&p1,&bp,base,ft,mask,vl,alim,bplim);
      else ps2_run4(&p0,&p1,&bp,base,ft,mask,vl,alim,bplim);
      i=p0-a0;
      if(i>=m)break;
    }
#endif
    if(bp<bplim){
      ogg_uint64_t w;
      __builtin_memcpy(&w,base+(bp>>3),8);
      e=ft[(unsigned)(w>>(bp&7))&mask];
    }
    if(e){
      bp+=e&15;
      zero=(e>>4)&1;
      off=(long)(e>>5);
    }else{
      long entry;
      b->endbyte=(long)(bp>>3); b->endbit=(int)(bp&7); b->ptr=b->buffer+b->endbyte;
      entry=decode_packed_entry_number(book,b);
      bp=(unsigned long)b->endbyte*8+(unsigned long)b->endbit;
      if(entry==-1){
        return(-1);
      }
      off=entry*dim*(long)sizeof(float);
    }
    if(i+half<=mi){
      if(!zero){
        const float *t=(const float *)(vl+off);
        long k;
        for(k=0;k<half;k++){
          a0[i+k]+=t[2*k];
          a1[i+k]+=t[2*k+1];
        }
      }
      i+=half;
    }else{
      const float *t=(const float *)(vl+off);
      long j;
      for(j=0;j<dim && i<m;j+=2,i++)
        if(i<lim){
          a0[i]+=t[j];
          a1[i]+=t[j+1];
        }
    }
  }
  b->endbyte=(long)(bp>>3); b->endbit=(int)(bp&7); b->ptr=b->buffer+b->endbyte;
  return(0);
}
