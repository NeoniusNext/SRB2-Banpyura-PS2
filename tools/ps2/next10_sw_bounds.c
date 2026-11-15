/* Complete real visplane metadata and strips are checked, including aliases. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef NEXT10_BOUNDS_EE
#include <kernel.h>
#endif
#include "r_plane.h"
NEXT10_BOUNDS_DECLARE
typedef struct { const char *name; void (*bounds)(visplane_t *); } variant_t;
#define VARIANT(n) { #n, n##_R_PlaneBounds }
static const variant_t variants[] = {NEXT10_BOUNDS_VARIANTS};
#define NVARIANTS (sizeof variants / sizeof variants[0])
#define WIDTH 640
typedef union { visplane_t plane; UINT16 words[sizeof(visplane_t)/2]; } plane_bytes_t;
static plane_bytes_t original, expected, actual;
static UINT16 strips[2*(WIDTH+2)], saved[2*(WIDTH+2)];
static UINT64 cases, compared_bytes;
static UINT32 random_state = UINT32_C(0x93b6e1a7);

static UINT32 random32(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

static void bounds_setup(INT32 first, INT32 length, unsigned mode, unsigned alias)
{
	unsigned i;
	memset(&original, 0xa6, sizeof original);
	original.plane.minx = first; original.plane.maxx = first+length-1;
	original.plane.top = strips+1;
	original.plane.bottom = strips+WIDTH+3;
	original.plane.clipwidth = WIDTH;
	for(i=0;i<2*(WIDTH+2);i++)
	{
		UINT16 value;
		switch(mode)
		{
			case 0: value=0; break;
			case 1: value=65535; break;
			case 2: value=32768; break;
			case 3: value=(UINT16)i; break;
			case 4: value=(UINT16)(65535-i); break;
			case 5: value=(i&1)?65535:0; break;
			default: value=(UINT16)random32(); break;
		}
		strips[i]=value;
	}
	/* All strips may share live contents; only their addresses stay metadata. */
	if(alias==1) original.plane.bottom=original.plane.top;
	if(alias==2) original.plane.bottom=original.plane.top+1;
	if(alias==3) original.plane.top=original.plane.bottom+1;
	memcpy(saved,strips,sizeof strips);
}

static int compare(void)
{
	size_t v;
	expected=original; reference_R_PlaneBounds(&expected.plane);
	for(v=1;v<NVARIANTS;v++)
	{
		actual=original; variants[v].bounds(&actual.plane);
		if(memcmp(&expected,&actual,sizeof expected) || memcmp(saved,strips,sizeof strips))
		{
			printf("BOUNDS FAIL variant=%s first=%ld max=%ld expected=%ld,%ld actual=%ld,%ld\n",
			       variants[v].name,(long)original.plane.minx,(long)original.plane.maxx,
			       (long)expected.plane.high,(long)expected.plane.low,(long)actual.plane.high,(long)actual.plane.low);
			return 1;
		}
		cases++; compared_bytes+=sizeof expected+sizeof strips;
	}
	return 0;
}

static int equality(void)
{
	static const INT32 lengths[]={-17,-1,0,1,2,3,7,8,15,16,17,31,32,33,63,127,319,320,321,639,640};
	unsigned n, mode, alias, i;
	for(n=0;n<sizeof lengths/sizeof lengths[0];n++)
	for(mode=0;mode<7;mode++) for(alias=0;alias<4;alias++)
	{
		bounds_setup(0,lengths[n],mode,alias); if(compare())return 1;
		if(lengths[n]>0 && lengths[n]<WIDTH)
		{
			bounds_setup(WIDTH-lengths[n],lengths[n],mode,alias); if(compare())return 1;
		}
	}
	/* Every UINT16 value crosses both signed-halfword boundaries in each slot. */
	for(i=0;i<65536;i+=
#ifdef NEXT10_BOUNDS_EE
	    257
#else
	    1
#endif
	)
	{
		bounds_setup(0,2,0,0);
		strips[1]=(UINT16)i; strips[2]=(UINT16)(65535-i);
		strips[WIDTH+3]=(UINT16)i; strips[WIDTH+4]=(UINT16)(65535-i);
		memcpy(saved,strips,sizeof strips); if(compare())return 1;
	}
	for(i=0;i<
#ifdef NEXT10_BOUNDS_EE
	    128
#else
	    12000
#endif
	;i++)
	{
		const INT32 first=(INT32)(random32()%WIDTH);
		const INT32 length=1+(INT32)(random32()%(WIDTH-first));
		const unsigned alias=random32()%4;
		bounds_setup(first,length,6,alias); if(compare())return 1;
	}
	printf("BOUNDS EXACT cases=%llu compared_bytes=%llu\n",(unsigned long long)cases,(unsigned long long)compared_bytes);
	return 0;
}

#ifdef NEXT10_BOUNDS_EE
static UINT32 now(void)
{
	UINT32 value; __asm__ volatile("mfc0 %0,$9" : "=r"(value)); return value;
}

static int benchmark(void)
{
	static const INT32 lengths[]={-1,0,1,2,3,7,8,16,31,32,33,100,320,640};
	size_t v, n; unsigned mode, rep, order, i, cache;
	for(cache=0;cache<
#ifdef NEXT10_BOUNDS_COLD
	    2
#else
	    1
#endif
	;cache++) for(mode=0;mode<7;mode++) for(n=0;n<sizeof lengths/sizeof lengths[0];n++)
	{
		const unsigned calls=cache?32:2048;
		bounds_setup(0,lengths[n],mode,0); if(compare())return 1;
		for(v=1;v<NVARIANTS;v++)
		{
			UINT32 best[2]={UINT32_MAX,UINT32_MAX};
			for(rep=0;rep<5;rep++) for(order=0;order<2;order++)
			{
				const unsigned variant=(rep+order)&1;
				void (*bounds)(visplane_t *)=variant?variants[v].bounds:reference_R_PlaneBounds;
				UINT32 start, elapsed;
				actual=original;
				if(cache)
				{
					elapsed=0;
					for(i=0;i<calls;i++)
					{
						FlushCache(0); FlushCache(2);
						const int interrupts_enabled=DIntr();
						start=now(); bounds(&actual.plane); elapsed+=now()-start;
						if(interrupts_enabled)EIntr();
					}
				}
				else
				{
					bounds(&actual.plane);
					const int interrupts_enabled=DIntr();
					start=now();
					for(i=0;i<calls;i++)bounds(&actual.plane);
					elapsed=now()-start;
					if(interrupts_enabled)EIntr();
				}
				if(elapsed<best[variant])best[variant]=elapsed;
			}
			printf("BOUNDSBENCH variant=%s n=%ld mode=%u cache=%s calls=%u before=%lu after=%lu\n",variants[v].name,
			       (long)lengths[n],mode,cache?"cold":"warm",calls,(unsigned long)best[0],(unsigned long)best[1]); fflush(stdout);
		}
	}
	puts("BOUNDS DONE"); return 0;
}
#endif

int main(void)
{
	if(equality())return 1;
#ifdef NEXT10_BOUNDS_EE
	return benchmark();
#else
	return 0;
#endif
}
