#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m_fixed.h"

#define MAXVIDWIDTH 640
#define MAXFFLOORS 40
#define PU_STATIC 1
typedef struct seg_s seg_t;
#include "vertex_type.inc"
#include "drawseg_type.inc"

static INT32 viewwidth;
static drawseg_t *drawsegs, *ds_p, *curdrawsegs, *firstseg;
static size_t maxdrawsegs;
static size_t candidatebytes, referencebytes, allocations, checks;
static int failures;
static void check(int ok, const char *what)
{
	++checks;
	if (!ok) { fprintf(stderr, "FAIL %s\n", what); ++failures; exit(1); }
}
static void I_Error(const char *message) { fprintf(stderr, "%s\n", message); exit(2); }
static void *Z_Realloc(void *old, size_t size, int tag, void *owner)
{
	void *p = calloc(1, size);
	size_t oldsize = old == drawsegs ? maxdrawsegs * sizeof (*drawsegs) : 0;
	size_t i;
	if (old && old != drawsegs)
		for (i = 0; i < maxdrawsegs; ++i)
			if (drawsegs[i].frontscale == old) oldsize = (size_t)drawsegs[i].frontscalewidth * sizeof (fixed_t);
	if (old != drawsegs || (old == NULL && size != 128 * sizeof (*drawsegs))) allocations++;
	check(tag == PU_STATIC && !owner, "growth tag/owner");
	if (!p) I_Error("growth failed");
	if (old) memcpy(p, old, oldsize);
	free(old);
	candidatebytes += size - oldsize;
	return p;
}
#include "drawseg_functions.inc"

int main(void)
{
	static const INT32 widths[] = {320, 640, 1, 319, 160, 512, 640, 320};
	inline_drawseg_t *reference = NULL;
	size_t refcapacity = 0, frame, slot, x;
	for (frame = 0; frame < 32; ++frame)
	{
		const size_t width = (size_t)widths[frame % (sizeof widths / sizeof widths[0])];
		const size_t total = frame == 0 ? 288 : 288 + frame * 19;
		viewwidth = (INT32)width;
		ds_p = drawsegs; curdrawsegs = drawsegs; firstseg = NULL;
		for (slot = 0; slot < total; ++slot)
		{
			const size_t firstindex = firstseg ? (size_t)(firstseg - drawsegs) : 0;
			const boolean hadfirst = firstseg != NULL;
			grow_drawsegs();
			check(ds_p == drawsegs + slot, "write cursor retained during moving realloc");
			check(curdrawsegs == drawsegs, "portal cursor slot zero retained");
			if (hadfirst) check(firstseg == drawsegs + firstindex, "firstseg including slot zero retained");
			if (maxdrawsegs != refcapacity)
			{
				inline_drawseg_t *newref = calloc(maxdrawsegs, sizeof (*newref));
				if (!newref) I_Error("reference growth failed");
				if (reference) memcpy(newref, reference, refcapacity * sizeof (*reference));
				free(reference); reference = newref; refcapacity = maxdrawsegs;
				referencebytes = refcapacity * sizeof (*reference);
			}
			/* Keep slot zero firstseg through initial growth; later frames reuse many slots. */
			if ((frame == 0 && !slot) || (frame && slot % (7 + frame % 5) == 0))
			{
				R_AllocDrawSegFrontScale(ds_p);
				firstseg = ds_p;
				for (x = 0; x < MAXVIDWIDTH; ++x)
					check((x < (size_t)firstseg->frontscalewidth ? firstseg->frontscale[x] : 0) == reference[slot].frontscale[x], "initial zero and stale columns retained");
			}
			/* Write sparse ranges; unmodified columns must survive shrink/expand and reuse. */
			if (firstseg)
			{
				const size_t first = (size_t)(firstseg - drawsegs);
				const size_t begin = (slot * 17) % width;
				const size_t end = begin + 4 < width ? begin + 4 : width;
				for (x = begin; x < end; ++x)
					firstseg->frontscale[x] = reference[first].frontscale[x] = (fixed_t)(frame * 104729 + slot * 89 + x);
			}
			++ds_p;
		}
		/* A polyobject plane can read a non-owner slot: ensure zeros or old values. */
		for (slot = 0; slot < total; slot += 23)
		{
			R_AllocDrawSegFrontScale(drawsegs + slot);
			check(drawsegs[slot].frontscalewidth >= viewwidth, "polyobject reader has current view capacity");
			for (x = 0; x < width; ++x)
				check(drawsegs[slot].frontscale[x] == reference[slot].frontscale[x], "non-owner polyobject initial and retained values");
		}
		for (slot = 0; slot < total; ++slot)
		for (x = 0; x < MAXVIDWIDTH; ++x)
			check((x < (size_t)drawsegs[slot].frontscalewidth ? drawsegs[slot].frontscale[x] : 0) == reference[slot].frontscale[x], "whole slot scale bytes equivalent");
		if (!frame)
			printf("DRAWSEG first_frame count=%zu capacity=%zu ref=%zu candidate=%zu allocations=%zu\n", total, maxdrawsegs, referencebytes, candidatebytes, allocations);
	}
	printf("DRAWSEG checks=%zu failures=%d ref_struct=%zu candidate_struct=%zu ref=%zu candidate=%zu scale_allocations=%zu\n",
		checks, failures, sizeof (*reference), sizeof (*drawsegs), referencebytes, candidatebytes, allocations);
	for (slot = 0; slot < maxdrawsegs; ++slot) free(drawsegs[slot].frontscale);
	free(drawsegs); free(reference);
	return failures ? 1 : 0;
}
