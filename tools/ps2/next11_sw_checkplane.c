/* Compare complete real visplane objects, strips, guards and allocator effects. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef NEXT11_CHECKPLANE_EE
#include <kernel.h>
#endif
#include "r_plane.h"

NEXT11_CHECKPLANE_DECLARE
typedef struct { const char *name; visplane_t *(*check)(visplane_t *, INT32, INT32); } variant_t;
#define VARIANT(n) { #n, n##_R_CheckPlane }
static const variant_t variants[] = {NEXT11_CHECKPLANE_VARIANTS};
#define NVARIANTS (sizeof variants / sizeof variants[0])
#define WIDTH 640
typedef struct { visplane_t plane; UINT16 strips[2*(WIDTH+2)]; UINT8 guards[64]; } slot_t;
typedef struct { slot_t slots[4]; } arena_t;
_Static_assert(offsetof(slot_t, strips) == sizeof(visplane_t), "Coallocated real plane layout");
static arena_t arena;
visplane_t *visplanes[MAXVISPLANES];
INT32 viewwidth;
visplane_t *freetail;
visplane_t **freehead = &freetail;
static unsigned allocations, frees;
static size_t requested_bytes;
static void *freed_pointer;
static unsigned timed_region, timed_stub_calls;

static void *fixture_malloc(size_t bytes, INT32 tag, void *user)
{
	if(timed_region){timed_stub_calls++;puts("CHECKPLANE FAIL timed allocator stub");exit(1);}
	if (allocations || bytes > sizeof(slot_t)-64 || tag != 1 || user)
	{
		puts("CHECKPLANE FAIL allocator contract"); exit(1);
	}
	allocations++; requested_bytes = bytes;
	return &arena.slots[2].plane;
}
static void fixture_free(void *pointer)
{
	if(timed_region){timed_stub_calls++;puts("CHECKPLANE FAIL timed free stub");exit(1);}
	frees++; freed_pointer = pointer;
}
static void fixture_error(const char *format, INT32 width)
{
	(void)format; (void)width; puts("CHECKPLANE FAIL unexpected invalid width"); exit(1);
}
#define Z_Malloc fixture_malloc
#define Z_Free fixture_free
#define I_Error fixture_error
#define PU_STATIC 1
#ifndef NEXT11_CHECKPLANE_EE
#include "allocator.inc"
#else
/* These exits are used only by untimed allocation/growth correctness cases. */
UINT32 next11_ztracemin;
void *next11_Z_MallocInternal(size_t bytes, INT32 tag, void *user, INT32 alignment, INT32 clear)
{
	if(alignment!=4 || clear!=1){puts("CHECKPLANE FAIL native allocation arguments");exit(1);}
	return fixture_malloc(bytes,tag,user);
}
void next11_Z_Free(void *p){fixture_free(p);}
void next11_I_Error(const char *format,INT32 width){fixture_error(format,width);}
void next11_I_OutputMsg(const char *format,...)
{
	(void)format;puts("CHECKPLANE FAIL unexpected trace output");exit(1);
}
#endif
#undef Z_Malloc
#undef Z_Free
#undef I_Error

typedef struct
{
	arena_t arena;
	visplane_t *buckets[MAXVISPLANES], *tail;
	visplane_t **head;
	unsigned allocations, frees;
	size_t requested_bytes;
	void *freed_pointer;
} snapshot_t;
static snapshot_t original, expected, actual;
static UINT64 cases, compared_bytes;
static UINT32 random_state = UINT32_C(0x8d52f3a1);
static UINT32 random32(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}
static void capture(snapshot_t *s)
{
	memset(s,0,sizeof *s);
	memcpy(&s->arena,&arena,sizeof arena); memcpy(s->buckets,visplanes,sizeof visplanes);
	s->tail=freetail; s->head=freehead; s->allocations=allocations; s->frees=frees;
	s->requested_bytes=requested_bytes; s->freed_pointer=freed_pointer;
}
static void restore(const snapshot_t *s)
{
	memcpy(&arena,&s->arena,sizeof arena); memcpy(visplanes,s->buckets,sizeof visplanes);
	freetail=s->tail; freehead=s->head; allocations=s->allocations; frees=s->frees;
	requested_bytes=s->requested_bytes; freed_pointer=s->freed_pointer;
}
static void plane_setup(visplane_t *p, INT32 width)
{
	p->next=NULL; p->height=(fixed_t)random32(); p->viewx=(fixed_t)random32();
	p->viewy=(fixed_t)random32(); p->viewz=(fixed_t)random32();
	p->viewangle=random32(); p->plangle=random32(); p->picnum=(INT32)random32();
	p->lightlevel=(INT32)random32(); p->extra_colormap=NULL;
	p->top=(UINT16 *)(p+1)+1; p->bottom=p->top+width+2; p->clipwidth=width;
	p->high=(INT32)random32(); p->low=(INT32)random32();
	p->xoffs=(INT64)((UINT64)random32()<<32 | random32());
	p->yoffs=(INT64)((UINT64)random32()<<32 | random32());
	p->xscale=(fixed_t)random32(); p->yscale=(fixed_t)random32();
	p->sector=NULL; p->ffloor=NULL; p->polyobj=NULL; p->slope=NULL; p->portalsector=NULL;
}
static INT32 current_start,current_stop;
static void checkplane_setup(INT32 width, INT32 first, INT32 length, unsigned mode,
                  unsigned alias, unsigned recycle, unsigned ffloor)
{
	unsigned i;
	memset(&arena,0xa6,sizeof arena); memset(visplanes,0,sizeof visplanes);
	for (i=0;i<4;i++)plane_setup(&arena.slots[i].plane,WIDTH);
	viewwidth=width; allocations=0; frees=0; requested_bytes=0; freed_pointer=NULL;
	visplane_t *p=&arena.slots[0].plane;
	p->minx=first; p->maxx=first+length-1;
	current_start=0; current_stop=width-1;
	if(ffloor)p->ffloor=(ffloor_t *)&arena.slots[3];
	if(recycle)
	{
		freetail=&arena.slots[1].plane;
		freetail->clipwidth=recycle==2?width-1:WIDTH;
		freetail->next=recycle==3?&arena.slots[3].plane:NULL;
	}
	else freetail=NULL;
	freehead=&freetail;
	for(i=0;i<WIDTH;i++){p->top[i]=65535;p->bottom[i]=0;}
	if(alias==1)p->bottom=p->top;
	if(alias==2)p->bottom=p->top+1;
	if(alias==3)p->top=p->bottom+1;
	if(length>0 && first<width && first+length>0 && mode)
	{
		INT32 at=first;
		if(mode==3 || mode==4)at+=length/2;
		if(mode==5 || mode==6)at+=length-1;
		if(at>=0 && at<width)
		{
			if(mode&1)p->top[at]=(UINT16)(random32()%65535);
			else p->bottom[at]=(UINT16)(1+random32()%65535);
		}
	}
	if(mode==7)for(i=0;i<WIDTH;i++){p->top[i]=(UINT16)random32();p->bottom[i]=(UINT16)random32();}
	/* Existing bucket heads are observable even when a new node is inserted. */
	for(i=0;i<MAXVISPLANES;i++)if((i&7)==5)visplanes[i]=&arena.slots[3].plane;
	capture(&original);
}
static int compare(void)
{
	size_t v;
	restore(&original);
	visplane_t *want=reference_R_CheckPlane(&arena.slots[0].plane,current_start,current_stop);
	capture(&expected);
	for(v=1;v<NVARIANTS;v++)
	{
		restore(&original);
		visplane_t *got=variants[v].check(&arena.slots[0].plane,current_start,current_stop);
		capture(&actual);
		if(want!=got || memcmp(&expected,&actual,sizeof expected))
		{
			printf("CHECKPLANE FAIL variant=%s width=%ld min=%ld max=%ld start=%ld stop=%ld\n",
			       variants[v].name,(long)viewwidth,(long)original.arena.slots[0].plane.minx,
			       (long)original.arena.slots[0].plane.maxx,(long)current_start,(long)current_stop);
			return 1;
		}
		cases++;compared_bytes+=sizeof expected;
	}
	return 0;
}
#ifdef NEXT11_CHECKPLANE_EE
static UINT32 now(void){UINT32 value;__asm__ volatile("mfc0 %0,$9":"=r"(value));return value;}
static INT32 timed_min,timed_max;
static unsigned timed_hash;
__attribute__((noinline,noipa))
static void timed_call(visplane_t *(*check)(visplane_t *,INT32,INT32))
{
	visplane_t *p=&arena.slots[0].plane,*recycled=&arena.slots[1].plane;
	p->minx=timed_min;p->maxx=timed_max;
	recycled->next=NULL;recycled->clipwidth=WIDTH;
	freetail=recycled;freehead=&freetail;visplanes[timed_hash]=NULL;
	(void)check(p,current_start,current_stop);
}
static int benchmark(void)
{
	static const INT32 lengths[]={-1,0,1,2,3,7,8,16,31,32,33,100,320,640};
	unsigned cache,n,mode,ffloor,rep,order,i;size_t v;
	for(cache=0;cache<2;cache++)for(ffloor=0;ffloor<2;ffloor++)
	for(mode=0;mode<7;mode++)for(n=0;n<sizeof lengths/sizeof lengths[0];n++)
	{
		const INT32 length=lengths[n],width=length>0?length:1;
		const unsigned calls=cache?32:2048;
		checkplane_setup(width,0,length,mode,0,1,ffloor);
		current_start=0;current_stop=length-1;
		if(compare())return 1;
		restore(&original);
		timed_min=0;timed_max=length-1;
		visplane_t *p=&arena.slots[0].plane;
		timed_hash=ffloor?MAXVISPLANES-1:(unsigned)(p->picnum*3+p->lightlevel+p->height*7)&VISPLANEHASHMASK;
		for(v=1;v<NVARIANTS;v++)
		{
			UINT32 best[2]={UINT32_MAX,UINT32_MAX};
			for(rep=0;rep<5;rep++)for(order=0;order<2;order++)
			{
				const unsigned which=(rep+order)&1;
				visplane_t *(*check)(visplane_t *,INT32,INT32)=which?variants[v].check:reference_R_CheckPlane;
				UINT32 start,elapsed=0;
				timed_region=1;
				if(cache)
				{
					for(i=0;i<calls;i++)
					{
						FlushCache(0);FlushCache(2);
						const int enabled=DIntr();
						start=now();timed_call(check);elapsed+=now()-start;
						if(enabled)EIntr();
					}
				}
				else
				{
					timed_call(check);const int enabled=DIntr();start=now();
					for(i=0;i<calls;i++)timed_call(check);
					elapsed=now()-start;if(enabled)EIntr();
				}
				timed_region=0;
				if(elapsed<best[which])best[which]=elapsed;
			}
			printf("CHECKPLANEBENCH variant=%s n=%ld mode=%u ffloor=%u cache=%s calls=%u before=%lu after=%lu\n",
			       variants[v].name,(long)length,mode,ffloor,cache?"cold":"warm",calls,
			       (unsigned long)best[0],(unsigned long)best[1]);fflush(stdout);
		}
	}
	printf("CHECKPLANE TIMED allocation_or_free_stubs=%u\n",timed_stub_calls);
	puts("CHECKPLANE DONE");return timed_stub_calls?1:0;
}
#endif
static int equality(void)
{
#ifdef NEXT11_CHECKPLANE_EE
	static const INT32 widths[]={1,7,31,320,640};
	static const INT32 lengths[]={-17,0,1,2,3,7,16,32,33,100,320,640};
#else
	static const INT32 widths[]={1,7,16,31,319,320,512,640};
	static const INT32 lengths[]={-17,-1,0,1,2,3,7,8,15,16,17,31,32,33,63,127,319,320,321,639,640};
#endif
	unsigned w,n,mode,alias,recycle,ffloor,i;
	for(w=0;w<sizeof widths/sizeof widths[0];w++)
	for(n=0;n<sizeof lengths/sizeof lengths[0];n++)
	for(mode=0;mode<8;mode++)for(alias=0;alias<4;alias++)
	for(recycle=0;recycle<4;recycle++)for(ffloor=0;ffloor<2;ffloor++)
	{
		INT32 length=lengths[n];if(length>widths[w])continue;
		checkplane_setup(widths[w],0,length,mode,alias,recycle,ffloor);if(compare())return 1;
		if(length>0 && length<widths[w])
		{checkplane_setup(widths[w],widths[w]-length,length,mode,alias,recycle,ffloor);if(compare())return 1;}
	}
	for(i=0;i<
#ifdef NEXT11_CHECKPLANE_EE
	    128
#else
	    16000
#endif
	;i++)
	{
		INT32 width=1+(INT32)(random32()%WIDTH),first=(INT32)(random32()%(unsigned)width);
		INT32 length=(INT32)(random32()%(unsigned)(width-first+2))-1;
		checkplane_setup(width,first,length,random32()%8,random32()%4,random32()%4,random32()&1);
		current_start=(INT32)(random32()%(unsigned)width);
		current_stop=(INT32)(random32()%(unsigned)width);
		if(compare())return 1;
	}
	for(i=0;i<65536;i+=
#ifdef NEXT11_CHECKPLANE_EE
	    257
#else
	    1
#endif
	)
	{
		checkplane_setup(1,0,1,0,0,1,0);arena.slots[0].plane.top[0]=(UINT16)i;
		capture(&original);if(compare())return 1;
		checkplane_setup(1,0,1,0,0,1,1);arena.slots[0].plane.bottom[0]=(UINT16)i;
		capture(&original);if(compare())return 1;
	}
	/* No overlap must not form strip pointers for these original empty states. */
	checkplane_setup(640,INT32_MAX,0,0,0,0,0);if(compare())return 1;
	checkplane_setup(640,640,0,0,0,0,0);if(compare())return 1;
	printf("CHECKPLANE EXACT cases=%llu compared_bytes=%llu\n",
	       (unsigned long long)cases,(unsigned long long)compared_bytes);
	return 0;
}
int main(void)
{
	if(equality())return 1;
#ifdef NEXT11_CHECKPLANE_EE
	return benchmark();
#else
	return 0;
#endif
}
