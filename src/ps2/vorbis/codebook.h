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

 function: basic shared codebook operations

 ********************************************************************/

#ifndef _V_CODEBOOK_H_
#define _V_CODEBOOK_H_

#include <ogg/ogg.h>

/* This structure encapsulates huffman and VQ style encoding books; it
   doesn't do anything specific to either.

   valuelist/quantlist are nonNULL (and q_* significant) only if
   there's entry->value mapping to be done.

   If encode-side mapping must be done (and thus the entry needs to be
   hunted), the auxiliary encode pointer will point to a decision
   tree.  This is true of both VQ and huffman, but is mostly useful
   with VQ.

*/

typedef struct static_codebook{
  long   dim;           /* codebook dimensions (elements per vector) */
  long   entries;       /* codebook entries */
  char  *lengthlist;    /* codeword lengths in bits */

  /* mapping ***************************************************************/
  int    maptype;       /* 0=none
                           1=implicitly populated values from map column
                           2=listed arbitrary values */

  /* The below does a linear, single monotonic sequence mapping. */
  long     q_min;       /* packed 32 bit float; quant value 0 maps to minval */
  long     q_delta;     /* packed 32 bit float; val 1 - val 0 == delta */
  int      q_quant;     /* bits: 0 < quant <= 16 */
  int      q_sequencep; /* bitflag */

  long     *quantlist;  /* map == 1: (int)(entries^(1/dim)) element column map
                           map == 2: list of dim*entries quantized entry vals
                        */
  int allocedp;
} static_codebook;

typedef struct codebook{
  long dim;           /* codebook dimensions (elements per vector) */
  long entries;       /* codebook entries */
  long used_entries;  /* populated codebook entries */
  const static_codebook *c;

  /* for encode, the below are entry-ordered, fully populated */
  /* for decode, the below are ordered by bitreversed codeword and only
     used entries are populated */
  float        *valuelist;  /* list of dim*entries actual entry values */
  ogg_uint32_t *codelist;   /* list of bitstream codewords for each entry */

  int          *dec_index;  /* only used if sparseness collapsed */
  char         *dec_codelengths;
  ogg_uint32_t *dec_firsttable;
  int           dec_firsttablen;
  int           dec_maxlength;

  /* The current encoder uses only centered, integer-only lattice books. */
  int           quantvals;
  int           minval;
  int           delta;
} codebook;

extern void vorbis_staticbook_destroy(static_codebook *b);
extern int vorbis_book_init_encode(codebook *dest,const static_codebook *source);
extern int vorbis_book_init_decode(codebook *dest,const static_codebook *source);
extern void vorbis_book_clear(codebook *b);

extern float *_book_unquantize(const static_codebook *b,int n,int *map);
extern float *_book_logdist(const static_codebook *b,float *vals);
extern float _float32_unpack(long val);
extern long   _float32_pack(float val);
extern int  _best(codebook *book, float *a, int step);
extern long _book_maptype1_quantvals(const static_codebook *b);

extern int vorbis_book_besterror(codebook *book,float *a,int step,int addmul);
extern long vorbis_book_codeword(codebook *book,int entry);
extern long vorbis_book_codelen(codebook *book,int entry);



extern int vorbis_staticbook_pack(const static_codebook *c,oggpack_buffer *b);
extern static_codebook *vorbis_staticbook_unpack(oggpack_buffer *b);

extern int vorbis_book_encode(codebook *book, int a, oggpack_buffer *b);

extern long vorbis_book_decode(codebook *book, oggpack_buffer *b);
extern long vorbis_book_decodevs_add(codebook *book, float *a,
                                     oggpack_buffer *b,int n);
extern long vorbis_book_decodev_set(codebook *book, float *a,
                                    oggpack_buffer *b,int n);
extern long vorbis_book_decodev_add(codebook *book, float *a,
                                    oggpack_buffer *b,int n);
extern long vorbis_book_decodevv_add(codebook *book, float **a,
                                     long off,int ch,
                                    oggpack_buffer *b,int n);
/* PS2-312: the same with a bin limit: frames at or beyond `lim` are decoded (the bit position advances) but not added */
extern long vorbis_book_decodevv_add_lim(codebook *book, float **a,
                                         long offset,int ch,
                                         oggpack_buffer *b,int n,long lim);

/* PS2-313: first-stage Huffman table with everything the residue loop needs in one word:
   ft[code bits] = (offset << 5) | (zero vector << 4) | code length, 0 = longer code (general path).
   offset = byte offset of the entry's vector in valuelist, or, for a book without values, the entry number (dec_index). */
typedef struct ps2_fastbook{
  unsigned mask;
  long dim;
  ogg_uint32_t ft[1];
} ps2_fastbook;
extern ps2_fastbook *ps2_fastbook_build(const codebook *book);
struct vorbis_dsp_state;
extern ps2_fastbook *ps2_fastbook_get(struct vorbis_dsp_state *vd,codebook *book);
extern long ps2_book_decode(struct vorbis_dsp_state *vd,codebook *book,oggpack_buffer *b);   /* = vorbis_book_decode, faster for valueless books */
/* the two-channel interleaved residue decode (res2_inverse) with the fast table; same result as vorbis_book_decodevv_add_lim */
extern long ps2_book_decodevv2_add(const ps2_fastbook *fb,codebook *book,float **a,long offset,
                                   oggpack_buffer *b,int n,long lim);



#endif
