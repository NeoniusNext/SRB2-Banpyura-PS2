/* Actual integer/floating helpers compile separately with external globals. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#ifdef NEXT9_LIGHT_EE
#include <kernel.h>
#endif
#include "next9_sw_lighting.h"
next7_vid_t vid;
INT32 centerx, centery, ds_y, ds_x1, ds_x2, ds_bgofs;
INT32 dc_x, dc_yl, dc_yh;
UINT8 *topleft, *screens[5], *colormaps;
lighttable_t **planezlight;
UINT8 *ds_source, *ds_colormap, *ds_transmap, *dc_colormap;
next7_vector_t ds_slopelight;
#ifdef PS2_OPT_SLOPE
float ds_lightscale;
#else
double zeroheight = 16;
fixed_t fovtan = FRACUNIT;
#endif

typedef struct { const char *name; void (*integer)(fixed_t, fixed_t); void (*floating)(void); } variant_t;
#define VARIANT(n) { #n, n##_integer, n##_R_CalcSlopeLight }
static const variant_t variants[] = {NEXT9_LIGHT_VARIANTS};
#define NVARIANTS (sizeof variants / sizeof variants[0])
static INT32 expected[MAXVIDWIDTH];
static UINT64 cases, compared_bytes, hash = UINT64_C(1469598103934665603);

static void reset(void)
{
	unsigned i;
	for (i=0; i<MAXVIDWIDTH; i++) tiltlighting[i] = (INT32)(i * UINT32_C(0x7247af13));
}

static int compare_integer(INT32 first, INT32 length, fixed_t start, fixed_t end)
{
	size_t v, i;
	ds_x1=first; ds_x2=first+length-1;
	reset(); reference_integer(start,end); memcpy(expected,tiltlighting,sizeof expected);
	for (v=1; v<NVARIANTS; v++)
	{
		reset(); variants[v].integer(start,end);
		if (memcmp(expected,tiltlighting,sizeof expected))
		{
			printf("LIGHT FAIL variant=%s first=%ld length=%ld start=%ld end=%ld\n",variants[v].name,(long)first,(long)length,(long)start,(long)end);
			return 1;
		}
		cases++; compared_bytes+=sizeof expected;
	}
	for (i=0; i<sizeof expected/sizeof expected[0]; i+=17) hash=(hash^(UINT32)expected[i])*UINT64_C(1099511628211);
	return 0;
}

static UINT32 random_state=UINT32_C(0x5ce13821);
static UINT32 random32(void)
{
	random_state^=random_state<<13; random_state^=random_state>>17; random_state^=random_state<<5;
	return random_state;
}

static int equality(void)
{
	static const INT32 lengths[]={-17,-1,1,2,3,7,8,15,16,17,31,32,33,63,127,319,320,321,639,640};
	static const UINT32 fractions[]={0,1,32767,65535};
	static const INT32 endpoints[]={INT32_MIN,INT32_MIN+1,-65537,-65536,-1,0,1,65535,65536,47*FRACUNIT,48*FRACUNIT,INT32_MAX-1,INT32_MAX};
	UINT32 raw; size_t f,n,a,b; unsigned iteration;
	/* Every possible signed integer result of the original arithmetic shift,
	 * including all 65,536 integer values and four fractional edge patterns. */
	for(raw=0;raw<65536;raw+=
#ifdef NEXT9_LIGHT_EE
	    4093
#else
	    1
#endif
	) for(f=0;f<sizeof fractions/sizeof fractions[0];f++)
	{
		const INT32 value=(INT32)((raw<<16)|fractions[f]);
		if(compare_integer((INT32)(raw%MAXVIDWIDTH),1,value,value)) return 1;
	}
	for(n=0;n<sizeof lengths/sizeof lengths[0];n++)
	for(a=0;a<sizeof endpoints/sizeof endpoints[0];a++)
	for(b=0;b<sizeof endpoints/sizeof endpoints[0];b++)
	{
		const INT32 length=lengths[n];
		const INT32 difference=(INT32)((UINT32)endpoints[b]-(UINT32)endpoints[a]);
		if(length==-1 && difference==INT32_MIN) continue; /* Original signed division overflow. */
		if(compare_integer(0,length,endpoints[a],endpoints[b])) return 1;
		if(length>0 && length<MAXVIDWIDTH && compare_integer(MAXVIDWIDTH-length,length,endpoints[a],endpoints[b])) return 1;
	}
	for(iteration=0;iteration<
#ifdef NEXT9_LIGHT_EE
	    128
#else
	    30000
#endif
	;iteration++)
	{
		const INT32 first=(INT32)(random32()%MAXVIDWIDTH);
		const INT32 length=1+(INT32)(random32()%(MAXVIDWIDTH-first));
		const INT32 start=(INT32)random32();
		const INT32 end=(INT32)random32();
		if(compare_integer(first,length,start,end)) return 1;
	}
	printf("LIGHT EXACT cases=%llu compared_bytes=%llu hash=%016llx\n",(unsigned long long)cases,(unsigned long long)compared_bytes,(unsigned long long)hash);
	return 0;
}

#ifdef NEXT9_LIGHT_EE
static UINT32 now(void)
{
	UINT32 value; __asm__ volatile("mfc0 %0,$9" : "=r"(value)); return value;
}

static void parameters(int length,unsigned mode)
{
	static const float starts[]={-100,-1,-0.25f,0,0.25f,1,24,47,47.75f,48,100,24,24,32760,-32760};
	static const float steps[]={0,0,0,0,0,0,0,0,0,0,0,0.125f,-0.125f,32,-32};
	vid.width=640; vid.height=400;
	centerx=centery=ds_y=ds_x1=0; ds_x2=length-1;
	ds_slopelight.x=steps[mode]; ds_slopelight.y=0; ds_slopelight.z=starts[mode];
#ifdef PS2_OPT_SLOPE
	ds_lightscale=1;
#endif
}

static int benchmark(void)
{
	/* Native zero-length checks preserve the observed complete LTO binary:
	 * both helpers branch out before division. Host C excludes that original
	 * undefined division input; its integer-domain proof is separate. */
	static const int lengths[]={-1,0,1,2,3,7,8,16,31,32,33,100,320,640};
	size_t v,n; unsigned mode,rep,order,i; const unsigned calls=2048;
	for(mode=0;mode<15;mode++) for(n=0;n<sizeof lengths/sizeof lengths[0];n++)
	{
		parameters(lengths[n],mode); reset(); reference_R_CalcSlopeLight(); memcpy(expected,tiltlighting,sizeof expected);
		for(v=1;v<NVARIANTS;v++)
		{
			UINT32 best[2]={UINT32_MAX,UINT32_MAX};
			reset(); variants[v].floating();
			if(memcmp(expected,tiltlighting,sizeof expected)) {printf("LIGHT TARGET FAIL variant=%s n=%d mode=%u\n",variants[v].name,lengths[n],mode);return 1;}
			for(rep=0;rep<5;rep++) for(order=0;order<2;order++)
			{
				const unsigned variant=(rep+order)&1;
				void(*light)(void)=variant?variants[v].floating:reference_R_CalcSlopeLight;
				UINT32 start,elapsed;
				light();
				const int interrupts_enabled=DIntr();
				start=now();
				for(i=0;i<calls;i++) light();
				elapsed=now()-start;
				if(interrupts_enabled) EIntr();
				if(elapsed<best[variant]) best[variant]=elapsed;
			}
			printf("LIGHTBENCH variant=%s n=%d mode=%u calls=%u before=%lu after=%lu\n",variants[v].name,lengths[n],mode,calls,(unsigned long)best[0],(unsigned long)best[1]); fflush(stdout);
		}
	}
	puts("LIGHT DONE");return 0;
}
#endif

int main(void)
{
#ifdef NEXT9_LIGHT_EE
	if(equality()) return 1;
	return benchmark();
#else
	return equality();
#endif
}
