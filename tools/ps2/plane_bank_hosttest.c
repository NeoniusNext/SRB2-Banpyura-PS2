#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "m_fixed.h"

#define MAXVIDWIDTH 640
#define MAXVISPLANES 513
#define MAXFFLOORS 40
#define VISPLANEHASHMASK 511
#define PU_STATIC 1
typedef UINT32 angle_t;
typedef struct sector_s sector_t;
typedef struct extracolormap_s extracolormap_t;
typedef struct ffloor_s ffloor_t;
typedef struct polyobj_s polyobj_t;
typedef struct pslope_s pslope_t;
typedef struct sectorportal_s sectorportal_t;
#include "plane_type.inc"

typedef struct block_s { size_t size; struct block_s *next; } block_t;
static block_t *blocks;
static size_t live, peak, allocations, releases;
static int checks, failures;
static void check(int ok, const char *what)
{
	checks++;
	if (!ok)
	{
		if (failures < 8) fprintf(stderr, "FAIL %s\n", what);
		failures++;
	}
}
static void I_Error(const char *format, ...)
{
	va_list args;
	va_start(args, format); vfprintf(stderr, format, args); va_end(args);
	exit(2);
}
static void *guard_malloc(size_t size)
{
	block_t *block = malloc(sizeof (*block) + 32 + size + 32);
	UINT8 *payload;
	if (!block) I_Error("host allocator failure");
	block->size = size; block->next = blocks; blocks = block;
	payload = (UINT8 *)(block + 1) + 32;
	memset(block + 1, 0xA7, 32);
	memset(payload, 0xCD, size);
	memset(payload + size, 0xA7, 32);
	live += size; allocations++; if (live > peak) peak = live;
	return payload;
}
static void guard_check(void)
{
	block_t *block;
	for (block = blocks; block; block = block->next)
	{
		const UINT8 *payload = (UINT8 *)(block + 1) + 32;
		int i;
		for (i = 0; i < 32; i++)
			check(payload[i - 32] == 0xA7 && payload[block->size + (size_t)i] == 0xA7, "allocation canary");
	}
}
static void guard_free(void *payload)
{
	block_t **slot = &blocks;
	while (*slot && (UINT8 *)((*slot) + 1) + 32 != payload) slot = &(*slot)->next;
	if (!*slot) I_Error("invalid free");
	{
		block_t *block = *slot;
		*slot = block->next; live -= block->size; releases++; free(block);
	}
}
static void *zone_malloc(size_t size, int tag, void *owner)
{
	check(tag == PU_STATIC && owner == NULL, "bank lifetime");
	return guard_malloc(size);
}
static INT32 viewwidth, viewheight;
static visplane_t *visplanes[MAXVISPLANES], *freetail;
static visplane_t **freehead = &freetail;
static INT16 floorclip[MAXVIDWIDTH], ceilingclip[MAXVIDWIDTH];
static fixed_t frontscale[MAXVIDWIDTH];
static struct { INT16 f_clip[MAXVIDWIDTH], c_clip[MAXVIDWIDTH]; } ffloor[MAXFFLOORS];
static void R_ClearFFloorClipArrays(void) {}
#define visplane_hash(picnum,lightlevel,height) ((unsigned)((picnum)*3+(lightlevel)+(height)*7) & VISPLANEHASHMASK)
#define Z_Malloc zone_malloc
#define Z_Free guard_free
#define malloc guard_malloc
#include "planes.inc"
#undef malloc

static void init_plane(visplane_t *plane, int id)
{
	plane->height = id * FRACUNIT; plane->picnum = id; plane->lightlevel = id & 255;
	plane->minx = viewwidth; plane->maxx = -1;
	plane->xoffs = id; plane->yoffs = -id; plane->xscale = plane->yscale = FRACUNIT;
	plane->extra_colormap = NULL; plane->ffloor = NULL;
	plane->viewx = 1; plane->viewy = 2; plane->viewz = 3; plane->viewangle = 4;
	plane->plangle = 5; plane->sector = NULL; plane->polyobj = NULL;
	plane->slope = NULL; plane->portalsector = NULL;
	R_ResetPlaneClip(plane);
}
int main(int argc, char **argv)
{
	static const int widths[] = {320, 320, 640, 319, 1, 160, 640, 640, 320, 512, 2, 640};
	FILE *dump;
	int frame, i, x;
	if (argc != 2) return 2;
	dump = fopen(argv[1], "wb"); if (!dump) return 2;
	viewheight = 224;
	for (frame = 0; frame < (int)(sizeof widths / sizeof widths[0]); frame++)
	{
		viewwidth = widths[frame];
		R_ClearPlanes();
		for (i = 0; i < 1200; i++)
		{
			visplane_t *plane = new_visplane((unsigned)i % MAXVISPLANES), *split;
#ifdef PLANE_DYNAMIC
			check(plane->clipwidth >= viewwidth, "recycled plane capacity");
			if (plane->clipwidth < viewwidth)
			{
				fclose(dump);
				return 1;
			}
#endif
			init_plane(plane, i);
			for (x = 0; x < viewwidth; x++)
			{
				check(plane->top[x] == 65535 && plane->bottom[x] == 0, "clip initialization");
				plane->top[x] = (UINT16)((x + i) % 111);
				plane->bottom[x] = (UINT16)(plane->top[x] + 7);
			}
			plane->top[-1] = plane->top[viewwidth] = 65535;
			plane->bottom[-1] = plane->bottom[viewwidth] = 0;
			check(plane->top[viewwidth] == 65535, "separate top and bottom pads");
			plane->minx = 0; plane->maxx = viewwidth - 1;
			R_PlaneBounds(plane);
			check(plane->high >= 0 && plane->low <= 117, "plane bounds");
			split = R_CheckPlane(plane, 0, viewwidth - 1);
			check(split != plane && split->height == plane->height
				&& split->xoffs == plane->xoffs && split->viewangle == plane->viewangle, "overlap split metadata");
			for (x = 0; x < viewwidth; x++)
				check(split->top[x] == 65535 && split->bottom[x] == 0, "split initialization");
			R_ExpandPlane(split, 0, viewwidth - 1);
			check(split == R_CheckPlane(split, 0, viewwidth - 1), "empty plane reuse");
			if (fwrite(plane->top - 1, sizeof (*plane->top), (size_t)viewwidth + 2, dump) != (size_t)viewwidth + 2
				|| fwrite(plane->bottom - 1, sizeof (*plane->bottom), (size_t)viewwidth + 2, dump) != (size_t)viewwidth + 2)
				return 2;
		}
		guard_check();
		printf("PL frame=%d width=%d live=%zu peak=%zu allocs=%zu frees=%zu\n", frame, viewwidth, live, peak, allocations, releases);
	}
	fclose(dump);
	guard_check();
	while (blocks) guard_free((UINT8 *)(blocks + 1) + 32);
	check(live == 0, "cleanup balance");
	printf("PL DONE checks=%d failures=%d peak=%zu\n", checks, failures, peak);
	return failures ? 1 : 0;
}
