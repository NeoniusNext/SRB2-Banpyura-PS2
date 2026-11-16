// Keep the original GS primitive and every STQ/RGB/XYZ bit. Source cells select
// disjoint Z24-high-byte stencil rectangles, never new vertices or scissors. REGION_REPEAT masks the
// original integer texel coordinates into a rolled physical image. Indexed
// general images use128x128 with64-texel cores/32 neighbors; direct colour uses
//64x64 with32-texel cores/16 neighbors. Both occupy the same16KiB scratch.
// Thus neither sampling phase nor outside-edge coverage changes with tiling.
#define TILE_REGION_CORE (TILE_SIZE / 2)

typedef struct
{
	int x0, x1, y0, ox, oy;
} tile_run_t;

typedef struct
{
	tile_mask_rect_t rect;
	int ox, oy;
} tile_batch_t;

_Static_assert(sizeof(tile_batch_t) == 20, "bounded completed-tile batch grew");

static u32 tile_run_peak_bytes;
static u32 tile_core_fast_triangles;
static u32 tile_box_nodes, tile_pixel_fallbacks;

static u32 tile_workspace_peak(void) { return tile_run_peak_bytes; }
static u32 tile_core_fast_count(void) { return tile_core_fast_triangles; }
static u32 tile_box_count(void) { return tile_box_nodes; }
static u32 tile_pixel_fallback_count(void) { return tile_pixel_fallbacks; }

static int tile_draw_window_layout(const cv_t tri[3], const piece_t *pc, const texrec_t *source, int ox, int oy, int wide)
{
	texrec_t tile;
	piece_t part = *pc;

	if (OV.n) ov_flush();
	if (!(wide ? tile_load_wide(source, ox, oy,
		P.wrapx && !(source->region & 1), P.wrapy && !(source->region & 2), &tile)
		: tile_load_region(source, ox, oy,
		P.wrapx && !(source->region & 1), P.wrapy && !(source->region & 2), &tile)))
		return 0;
	// TW/TH and coordinate scale retain the original texture's units. The
	// repeat mask, not a change of STQ basis, limits physical VRAM addresses.
	tile.tw = source->tw > 10 ? 10 : source->tw;
	tile.th = source->th > 10 ? 10 : source->th;
	P.r = &tile;
	P.tsx = (float)source->sw / (float)(1u << tile.tw);
	P.tsy = (float)source->sh / (float)(1u << tile.th);
	P.clamp = 3ull | (3ull << 2) | ((u64)(tile.w - 1) << 4) | ((u64)(tile.h - 1) << 24);
	P.serial = H.serial - 1;
	part.ripple = 0;
	const u32 before_vertices = H.st.verts_out;
	emit_piece(tri, 3, &part);
	if (OV.n) ov_flush();
	tile_stencil_triangle_submissions += (H.st.verts_out - before_vertices) / 3u;
	tile_stencil_bbox_pixels += (u64)(H.vw) * (u64)(H.vh) * ((H.st.verts_out - before_vertices) / 3u);
	return 1;
}

static int tile_draw_window(const cv_t tri[3], const piece_t *pc, const texrec_t *source, int ox, int oy)
{
	return tile_draw_window_layout(tri, pc, source, ox, oy, 0);
}

static int tile_draw_group(const cv_t tri[3], const piece_t *pc, const texrec_t *source,
	const tile_batch_t *group, int count)
{
	tile_stencil_group_deferred(&group->rect, count, sizeof *group);
	if (!tile_draw_window(tri, pc, source, group->ox, group->oy)) return 0;
	tile_stencil_group_end();
	return 1;
}

static int tile_batch_order(const void *a, const void *b)
{
	const tile_batch_t *aa = a, *bb = b;

	if (aa->oy != bb->oy) return aa->oy < bb->oy ? -1 : 1;
	if (aa->ox != bb->ox) return aa->ox < bb->ox ? -1 : 1;
	return 0;
}

// Rectangles complete in scanline order, but their pixels are disjoint. Group
// only this original triangle's bounded batch by source window, so each window
// needs one upload/submission while preserving triangle and pass ordering.
static int tile_batch_flush(const cv_t tri[3], const piece_t *pc, const texrec_t *source,
	tile_batch_t *batch, int *count)
{
	int at = 0;

	qsort(batch, (size_t)*count, sizeof *batch, tile_batch_order);
	while (at < *count)
	{
		int end = at + 1;

		while (end < *count && batch[end].ox == batch[at].ox && batch[end].oy == batch[at].oy) end++;
		if (!tile_draw_group(tri, pc, source, &batch[at], end - at)) return 0;
		at = end;
	}
	*count = 0;
	return 1;
}

static int tile_batch_add(const cv_t tri[3], const piece_t *pc, const texrec_t *source,
	tile_batch_t *batch, int *count, const tile_run_t *run, int y1)
{
	tile_batch_t *entry = &batch[*count];

	entry->rect.x0 = run->x0; entry->rect.x1 = run->x1;
	entry->rect.y0 = (u16)run->y0; entry->rect.y1 = (u16)y1;
	entry->ox = run->ox; entry->oy = run->oy;
	if (++*count == H.vw) return tile_batch_flush(tri, pc, source, batch, count);
	return 1;
}

// Reduce only the cache-window key, never the GS coordinates. Repeat periods
// are powers of two here (NPOT repeats were cut by the existing renderer).
// Deeply clamped coordinates select a uniform edge window. Thus every finite
// input has a bounded integer key without imposing a UV or graphics limit.
static int tile_region_origin(float value, int size, int wrap)
{
	if (!isfinite(value)) return value < 0.0f ? -2 * TILE_SIZE : size + 2 * TILE_SIZE;
	if (wrap)
	{
		const int period = size < TILE_SIZE ? TILE_SIZE : size;
		// Integer floor is exact in this bounded domain. Preserve the signed
		// fmod key, including negative windows whose masked cell is zero.
		if (!(period & (period - 1)) && value > -0x1p35f && value < 0x1p35f)
		{
			const float scaled = value * (1.0f / TILE_REGION_CORE);
			int cell = (int)scaled;
			if ((float)cell > scaled) cell--;
			int key = cell & (period / TILE_REGION_CORE - 1);
			if (value < 0.0f && (key || scaled != (float)cell))
				key -= period / TILE_REGION_CORE;
			return key * TILE_REGION_CORE;
		}
		value = fmodf(value, (float)period); // huge or non-power-of-two input
	}
	else
	{
		if (value < -(float)TILE_SIZE) return -2 * TILE_SIZE;
		if (value > (float)(size + TILE_SIZE)) return size + 2 * TILE_SIZE;
	}
	const float scaled = value * (1.0f / TILE_REGION_CORE);
	int cell = (int)scaled;
	if ((float)cell > scaled) cell--;
	return cell * TILE_REGION_CORE;
}

static int tile_region_origin128(float value, int size, int wrap)
{
	if (!isfinite(value)) return value < 0.0f ? -2 * (TILE_SIZE * 2) : size + 2 * (TILE_SIZE * 2);
	if (wrap)
	{
		const int period = size < (TILE_SIZE * 2) ? (TILE_SIZE * 2) : size;
		// Integer floor is exact in this bounded domain. Preserve the signed
		// fmod key, including negative windows whose masked cell is zero.
		if (!(period & (period - 1)) && value > -0x1p35f && value < 0x1p35f)
		{
			const float scaled = value * (1.0f / (TILE_REGION_CORE * 2));
			int cell = (int)scaled;
			if ((float)cell > scaled) cell--;
			int key = cell & (period / (TILE_REGION_CORE * 2) - 1);
			if (value < 0.0f && (key || scaled != (float)cell))
				key -= period / (TILE_REGION_CORE * 2);
			return key * (TILE_REGION_CORE * 2);
		}
		value = fmodf(value, (float)period); // huge or non-power-of-two input
	}
	else
	{
		if (value < -(float)(TILE_SIZE * 2)) return -2 * (TILE_SIZE * 2);
		if (value > (float)(size + (TILE_SIZE * 2))) return size + 2 * (TILE_SIZE * 2);
	}
	const float scaled = value * (1.0f / (TILE_REGION_CORE * 2));
	int cell = (int)scaled;
	if ((float)cell > scaled) cell--;
	return cell * (TILE_REGION_CORE * 2);
}

static int tile_source_origin(float value, int size, int wrap, int extent)
{
	return extent == 128 ? tile_region_origin128(value,size,wrap) : tile_region_origin(value,size,wrap);
}

static int tile_core_origin(float lo, float hi, int size, int wrap, int extent, int *out)
{
	// Keep one complete texel of original neighbors beyond both vertex extrema.
	// The physical window contains origin-extent/4 through origin+3*extent/4-1.
	const int first = (int)ceilf(hi - (float)(extent * 3 / 4 - 2));
	const int last = (int)floorf(lo + (float)(extent / 4 - 1));
	int origin;

	if (first > last) return 0;
	origin = first + (last - first) / 2;
	if (wrap) origin %= size < extent ? extent : size;
	else if (origin < -2 * extent) origin = -2 * extent;
	else if (origin > size + 2 * extent) origin = size + 2 * extent;
	*out = origin;
	return 1;
}

// Positive-Q projective coordinates stay between the original vertex extrema.
// Test RAW unwrapped extrema before choosing one rolled64/128-texel window;
// equal wrapped keys alone could hide a whole repeat. Inclusive core boundaries
// and wider nonaligned ranges are safe when the explicit neighbor margin fits.
static int tile_one_core(const cv_t tri[3], const piece_t *pc, const texrec_t *source, int *ox, int *oy)
{
	float min_u = 0.0f, max_u = 0.0f, min_v = 0.0f, max_v = 0.0f;
	const int extent = tile_general_extent(source);
	int i;

	for (i = 0; i < 3; i++)
	{
		const float u = (tri[i].a[0] - pc->sb) * (float)source->sw;
		const float v = (tri[i].a[1] - pc->tb) * (float)source->sh;

		// This is only fast-path eligibility. Other coordinates use the full
		// classifier, including huge values and non-positive/unknown Q.
		if (!(tri[i].w > 0.0f) || !isfinite(tri[i].w)
			|| !isfinite(u) || !isfinite(v) || fabsf(u) >= 0x1p20f || fabsf(v) >= 0x1p20f)
			return 0;
		const float q = 1.0f / tri[i].w;
		if (!(q > 0.0f) || !isfinite(q)) return 0;
		if (i == 0) { min_u = max_u = u; min_v = max_v = v; }
		else
		{
			if (u < min_u) min_u = u;
			if (u > max_u) max_u = u;
			if (v < min_v) min_v = v;
			if (v > max_v) max_v = v;
		}
	}
	return tile_core_origin(min_u, max_u, source->w, P.wrapx && !(source->region & 1), extent, ox)
		&& tile_core_origin(min_v, max_v, source->h, P.wrapy && !(source->region & 2), extent, oy);
}

// The borrowed window is optional and is tested only after V8 rejects one
// ordinary window. Its raw positive-Q bounds use the same neighbor margin.
static int tile_one_wide(const cv_t tri[3], const piece_t *pc, const texrec_t *source, int *ox, int *oy)
{
	float min_u = 0.0f, max_u = 0.0f, min_v = 0.0f, max_v = 0.0f;
	int width, height;
	int i;

	if (!tile_wide_dimensions(source, &width, &height)) return 0;
	for (i = 0; i < 3; i++)
	{
		const float u = (tri[i].a[0] - pc->sb) * (float)source->sw;
		const float v = (tri[i].a[1] - pc->tb) * (float)source->sh;

		// This is only fast-path eligibility. Other coordinates use the full
		// classifier, including huge values and non-positive/unknown Q.
		if (!(tri[i].w > 0.0f) || !isfinite(tri[i].w)
			|| !isfinite(u) || !isfinite(v) || fabsf(u) >= 0x1p20f || fabsf(v) >= 0x1p20f)
			return 0;
		const float q = 1.0f / tri[i].w;
		if (!(q > 0.0f) || !isfinite(q)) return 0;
		if (i == 0) { min_u = max_u = u; min_v = max_v = v; }
		else
		{
			if (u < min_u) min_u = u;
			if (u > max_u) max_u = u;
			if (v < min_v) min_v = v;
			if (v > max_v) max_v = v;
		}
	}
	return tile_core_origin(min_u, max_u, source->w, P.wrapx && !(source->region & 1), width, ox)
		&& tile_core_origin(min_v, max_v, source->h, P.wrapy && !(source->region & 2), height, oy);
}

typedef struct
{
	int x[3], y[3];
	float rq[3], us[3], vs[3], inv_det;
	int64_t det;
} tile_plane_t;

typedef struct { int x0, x1, y0, y1; } tile_box_t;

static void tile_plane_edges(const tile_plane_t *p, int x, int y, int64_t *n0, int64_t *n1, int64_t *n2)
{
	const int fx = (2048 + x) * 16, fy = (2048 + y) * 16;

	*n1 = (int64_t)(fx - p->x[0]) * (p->y[2] - p->y[0])
		- (int64_t)(p->x[2] - p->x[0]) * (fy - p->y[0]);
	*n2 = (int64_t)(p->x[1] - p->x[0]) * (fy - p->y[0])
		- (int64_t)(fx - p->x[0]) * (p->y[1] - p->y[0]);
	*n0 = p->det - *n1 - *n2;
}

// Match the software int64 cast, whose signed32 inputs round to nearest-even.
// EE cvt.s.w can truncate inexact values, so use it only for exact integers.
static float tile_i64_to_float(int64_t value)
{
	int32_t n = (int32_t)value;

	if ((int64_t)n != value) return (float)value;
	if (n >= -16777216 && n <= 16777216) return (float)n;
	const uint32_t sign = (uint32_t)n & 0x80000000u;
	const uint32_t mag = n < 0 ? 0u - (uint32_t)n : (uint32_t)n;
	// Here floor(log2(mag)) is 24..31: at most eight low bits are lost.
	unsigned shift = 1;
	uint32_t top = mag >> 24;
	if (top >= 16u) { shift += 4; top >>= 4; }
	if (top >= 4u) { shift += 2; top >>= 2; }
	if (top >= 2u) shift++;
	uint32_t mant = mag >> shift;
	const uint32_t rem = mag & ((1u << shift) - 1u), half = 1u << (shift - 1u);
	if (rem > half || (rem == half && (mant & 1u))) mant++;
	// Addition carries a rounded 24-bit mantissa into the next exponent.
	const uint32_t bits = sign + ((shift + 150u) << 23) + (mant - 0x800000u);
	union { uint32_t u; float f; } out = {bits};
	return out.f;
}

static void tile_plane_uv(const tile_plane_t *p, int64_t n1, int64_t n2,
	float *u, float *v, float *q, float *qa, float *ua, float *va)
{
	const float b1 = tile_i64_to_float(n1) * p->inv_det, b2 = tile_i64_to_float(n2) * p->inv_det;
	const float iq = (*q = p->rq[0] + b1 * (p->rq[1] - p->rq[0]) + b2 * (p->rq[2] - p->rq[0])) > 0.0f ? 1.0f / *q : 0.0f;

	*u = (p->us[0] + b1 * (p->us[1] - p->us[0]) + b2 * (p->us[2] - p->us[0])) * iq;
	*v = (p->vs[0] + b1 * (p->vs[1] - p->vs[0]) + b2 * (p->vs[2] - p->vs[0])) * iq;
	// Absolute expression sums bound cancellation across this affine box.
	*qa = fabsf(p->rq[0]) + fabsf(b1) * (fabsf(p->rq[1]) + fabsf(p->rq[0])) + fabsf(b2) * (fabsf(p->rq[2]) + fabsf(p->rq[0]));
	*ua = fabsf(p->us[0]) + fabsf(b1) * (fabsf(p->us[1]) + fabsf(p->us[0])) + fabsf(b2) * (fabsf(p->us[2]) + fabsf(p->us[0]));
	*va = fabsf(p->vs[0]) + fabsf(b1) * (fabsf(p->vs[1]) + fabsf(p->vs[0])) + fabsf(b2) * (fabsf(p->vs[2]) + fabsf(p->vs[0]));
}

// 1: every sample uses one raw core; 2: all samples lie outside one triangle
// edge and retain the old outside key0; 0: subdivide or use the old pixel key.
static int tile_box_classify(const tile_plane_t *p, const tile_box_t *b, const texrec_t *source, int *ox, int *oy)
{
	int64_t edges[4][3];
	int outside[3] = {1,1,1};
	float min_u=0.0f,max_u=0.0f,min_v=0.0f,max_v=0.0f,min_q=0.0f,max_qa=0.0f,max_ua=0.0f,max_va=0.0f;
	const int extent = tile_general_extent(source), core = extent / 2;
	const float inv_core = extent == 128 ? (1.0f / 64.0f) : (1.0f / 32.0f);
	int i, j;

	tile_box_nodes++;
	for(i=0;i<4;i++)
	{
		tile_plane_edges(p, (i&1)?b->x1-1:b->x0, (i&2)?b->y1-1:b->y0, &edges[i][0], &edges[i][1], &edges[i][2]);
		for(j=0;j<3;j++) if(p->det>0 ? edges[i][j]>=0 : edges[i][j]<=0) outside[j]=0;
	}
	if(outside[0]||outside[1]||outside[2]) { *ox=*oy=0; return 2; }
	for(i=0;i<4;i++)
	{
		float u,v,q,qa,ua,va;
		tile_plane_uv(p,edges[i][1],edges[i][2],&u,&v,&q,&qa,&ua,&va);
		if(!(q>0.0f)||!isfinite(q)||!isfinite(u)||!isfinite(v)||fabsf(u)>=0x1p20f||fabsf(v)>=0x1p20f) return 0;
		if(!i) { min_u=max_u=u; min_v=max_v=v; min_q=q; }
		else { if(u<min_u)min_u=u;if(u>max_u)max_u=u;if(v<min_v)min_v=v;if(v>max_v)max_v=v;if(q<min_q)min_q=q; }
		if(qa>max_qa)max_qa=qa;
		if(ua>max_ua)max_ua=ua;
		if(va>max_va)max_va=va;
	}
	const float bu=floorf(min_u*inv_core)*core;
	const float bv=floorf(min_v*inv_core)*core;
	if(max_u>bu+core||max_v>bv+core) return 0;
	// Reject ill-conditioned extrapolation. A conservative 64-ulp expression
	// allowance must stay within four texels, leaving >10 original halo texels
	// beyond both extrema even for bilinear filtering. Ambiguity just subdivides.
	const float qe=0x1p-17f*max_qa, room=4.0f*(min_q-qe);
	if(!(room>0.0f)||0x1p-17f*max_ua+hw_maxf(fabsf(min_u),fabsf(max_u))*qe>room
		||0x1p-17f*max_va+hw_maxf(fabsf(min_v),fabsf(max_v))*qe>room) return 0;
	*ox=tile_source_origin(bu,source->w,P.wrapx&&!(source->region&1),extent);
	*oy=tile_source_origin(bv,source->h,P.wrapy&&!(source->region&2),extent);
	return 1;
}

static void tile_pixel_key(const tile_plane_t *p, int x, int y, const texrec_t *source, int *ox, int *oy)
{
	int64_t n0,n1,n2;

	tile_pixel_fallbacks++;
	tile_plane_edges(p,x,y,&n0,&n1,&n2);
	*ox=*oy=0;
	if(p->det>0 ? (n0>=0&&n1>=0&&n2>=0) : (n0<=0&&n1<=0&&n2<=0))
	{
		float u,v,q,qa,ua,va;
		tile_plane_uv(p,n1,n2,&u,&v,&q,&qa,&ua,&va);
		const int extent=tile_general_extent(source);
		*ox=tile_source_origin(u,source->w,P.wrapx&&!(source->region&1),extent);
		*oy=tile_source_origin(v,source->h,P.wrapy&&!(source->region&2),extent);
	}
}

typedef struct
{
	tile_batch_t *batch;
	int *count, *previous, *current;
	int previous_n, current_n, previous_at;
} tile_row_batch_t;

// Retain only indices into the existing bounded batch. A flush consumes every
// borrowed rectangle before its indices are discarded. Equal horizontal spans
// on adjacent rows can therefore extend one disjoint rectangle in place.
static int tile_row_add(const cv_t tri[3], const piece_t *pc, const texrec_t *source,
	tile_row_batch_t *rows, int x0, int x1, int y, int ox, int oy)
{
	int index;
	tile_batch_t *entry;

	while (rows->previous_at < rows->previous_n
		&& rows->batch[rows->previous[rows->previous_at]].rect.x0 < x0)
		rows->previous_at++;
	if (rows->previous_at < rows->previous_n)
	{
		index = rows->previous[rows->previous_at];
		entry = &rows->batch[index];
		if (entry->rect.x0 == x0 && entry->rect.x1 == x1 && entry->rect.y1 == y
			&& entry->ox == ox && entry->oy == oy)
		{
			entry->rect.y1 = (u16)(y + 1);
			rows->current[rows->current_n++] = index;
			rows->previous_at++;
			return 1;
		}
	}
	index = *rows->count;
	entry = &rows->batch[index];
	entry->rect.x0 = x0; entry->rect.x1 = x1;
	entry->rect.y0 = (u16)y; entry->rect.y1 = (u16)(y + 1);
	entry->ox = ox; entry->oy = oy;
	if (++*rows->count == H.vw)
	{
		if (!tile_batch_flush(tri, pc, source, rows->batch, rows->count)) return 0;
		rows->previous_n = rows->current_n = rows->previous_at = 0;
	}
	else rows->current[rows->current_n++] = index;
	return 1;
}

// The guarded GS vertices bound every positive row-edge slope below 2^21.
// Keep the numerator wide through ceil construction: a value just above 2^32
// must retain the original exact 64-bit division instead of wrapping to zero.
static int64_t tile_row_div_positive(int64_t numerator, uint32_t denominator)
{
	if ((uint64_t)numerator <= UINT32_MAX) return (uint32_t)numerator / denominator;
	return numerator / denominator;
}

// Exact inclusive edge inequalities locate the same inside samples used by
// tile_pixel_key. Everything before/after the interval retains outside key 0.
static void tile_row_inside(const tile_plane_t *p, int x0, int x1, int y, int *left, int *right)
{
	int64_t n[3], d[3];
	int lo = x0, hi = x1;

	tile_plane_edges(p, x0, y, &n[0], &n[1], &n[2]);
	d[1] = (int64_t)(p->y[2] - p->y[0]) * 16;
	d[2] = -(int64_t)(p->y[1] - p->y[0]) * 16;
	d[0] = -d[1] - d[2];
	for (int i = 0; i < 3; i++)
	{
		if (p->det < 0) { n[i] = -n[i]; d[i] = -d[i]; }
		if (d[i] > 0 && n[i] < 0)
		{
			const int64_t distance = tile_row_div_positive(-n[i] + d[i] - 1, (uint32_t)d[i]);
			if (distance >= x1 - x0) { lo = hi = x0; break; }
			if (x0 + (int)distance > lo) lo = x0 + (int)distance;
		}
		else if (d[i] <= 0)
		{
			if (n[i] < 0) { lo = hi = x0; break; }
			if (d[i] < 0)
			{
				const int64_t distance = tile_row_div_positive(n[i], (uint32_t)-d[i]);
				if (distance < x1 - x0 && x0 + (int)distance + 1 < hi)
					hi = x0 + (int)distance + 1;
			}
		}
	}
	if (lo >= hi) lo = hi = x0;
	*left = lo; *right = hi;
}

// Crossing estimates never certify pixels: all resulting intervals still pass
// tile_box_classify, with its original rounding/cancellation and halo bound.
static int tile_row_next(const tile_plane_t *p, int x, int end, int y,
	int core, float qdx, float udx, float vdx)
{
	int64_t n0, n1, n2;
	float u, v, q, qa, ua, va;
	int length = end - x;

	tile_plane_edges(p, x, y, &n0, &n1, &n2);
	tile_plane_uv(p, n1, n2, &u, &v, &q, &qa, &ua, &va);
	if (!(q > 0.0f) || !isfinite(q) || !isfinite(u) || !isfinite(v)
		|| fabsf(u) >= 0x1p20f || fabsf(v) >= 0x1p20f) return x + 1;
	for (int axis = 0; axis < 2; axis++)
	{
		const float value = axis ? v : u, slope = axis ? vdx : udx;
		const float direction = slope - value * qdx;
		int cell = (int)(value * (1.0f / core));
		if ((float)(cell * core) > value) cell--;
		const float boundary = (float)((cell + (direction > 0.0f)) * core);
		const float denominator = slope - boundary * qdx;
		if (direction != 0.0f && denominator != 0.0f)
		{
			const float distance = (boundary - value) * q / denominator;
			if (distance >= 0.0f && distance < (float)length)
			{
				const int next = (int)distance + 1;
				if (next < length) length = next;
			}
		}
	}
	return x + length;
}

// A projective coordinate along a fixed row is a ratio of affine functions.
// With positive Q its derivative has constant sign, so the next raw U/V core
// crossing predicts a whole interval. Unsupported or ill-conditioned planes
// keep the original two-dimensional classifier below.
static int tile_emit_rows(const cv_t tri[3], const piece_t *pc, const texrec_t *source,
	const tile_plane_t *p, const tile_box_t *bounds, tile_batch_t *batch, int *count)
{
	const float b1dx = 16.0f * ((float)p->y[2] - (float)p->y[0]) * p->inv_det;
	const float b2dx = -16.0f * ((float)p->y[1] - (float)p->y[0]) * p->inv_det;
	const float qdx = b1dx * (p->rq[1] - p->rq[0]) + b2dx * (p->rq[2] - p->rq[0]);
	const float udx = b1dx * (p->us[1] - p->us[0]) + b2dx * (p->us[2] - p->us[0]);
	const float vdx = b1dx * (p->vs[1] - p->vs[0]) + b2dx * (p->vs[2] - p->vs[0]);
	const int core = tile_general_extent(source) / 2;

	if (!isfinite(qdx) || !isfinite(udx) || !isfinite(vdx)) return 0;
	for (int i = 0; i < 3; i++)
		if (!(p->rq[i] > 0.0f) || !isfinite(p->rq[i])
			|| p->x[i] < 0 || p->x[i] > 65535 || p->y[i] < 0 || p->y[i] > 65535) return 0;
	for (int i = 0; i < 4; i++)
	{
		int64_t n0, n1, n2;
		float u, v, q, qa, ua, va;
		tile_plane_edges(p, i & 1 ? bounds->x1 - 1 : bounds->x0,
			i & 2 ? bounds->y1 - 1 : bounds->y0, &n0, &n1, &n2);
		tile_plane_uv(p, n1, n2, &u, &v, &q, &qa, &ua, &va);
		if (!(q > 0.0f) || !isfinite(q) || !isfinite(u) || !isfinite(v)
			|| fabsf(u) >= 0x1p20f || fabsf(v) >= 0x1p20f) return 0;
		// Do not turn old ill-conditioned two-dimensional fallback into a
		// viewport-sized sweep of individual pixels.
		const float qe = 0x1p-17f * qa, room = 4.0f * (q - qe);
		if (!(room > 0.0f) || 0x1p-17f * ua + fabsf(u) * qe > room
			|| 0x1p-17f * va + fabsf(v) * qe > room) return 0;
	}
	int ox, oy;
	if (tile_box_classify(p, bounds, source, &ox, &oy))
	{
		// Cropping can leave one source core even when raw vertex extrema
		// needed multiple cores. Preserve the old whole-box certificate before
		// introducing row boundaries or an additional outside-key0 group.
		const tile_run_t run = {bounds->x0, bounds->x1, bounds->y0, ox, oy};
		return tile_batch_add(tri, pc, source, batch, count, &run, bounds->y1) ? 1 : -1;
	}
	int references[2 * H.vw];
	tile_row_batch_t rows = {batch, count, references, references + H.vw, 0, 0, 0};
	for (int y = bounds->y0; y < bounds->y1; y++)
	{
		int left, right;
		tile_row_inside(p, bounds->x0, bounds->x1, y, &left, &right);
		if (bounds->x0 < left && !tile_row_add(tri, pc, source, &rows, bounds->x0, left, y, 0, 0)) return -1;
		for (int x = left; x < right;)
		{
			int end = tile_row_next(p, x, right, y, core, qdx, udx, vdx), ox, oy;
			tile_box_t box = {x, end, y, y + 1};
			int kind = tile_box_classify(p, &box, source, &ox, &oy);
			// Roundoff in the crossing estimate can include the next core's
			// first sample. Retry one sample shorter, then shrink conservatively.
			if (!kind && end > x + 1) { box.x1 = --end; kind = tile_box_classify(p, &box, source, &ox, &oy); }
			while (!kind && end > x + 1)
			{
				box.x1 = end = x + (end - x) / 2;
				kind = tile_box_classify(p, &box, source, &ox, &oy);
			}
			if (!kind) tile_pixel_key(p, x, y, source, &ox, &oy);
			if (!tile_row_add(tri, pc, source, &rows, x, end, y, ox, oy)) return -1;
			x = end;
		}
		if (right < bounds->x1 && !tile_row_add(tri, pc, source, &rows, right, bounds->x1, y, 0, 0)) return -1;
		int *swap = rows.previous; rows.previous = rows.current; rows.current = swap;
		rows.previous_n = rows.current_n; rows.current_n = rows.previous_at = 0;
	}
	return 1;
}

static void tile_emit_triangle(const cv_t tri[3], const piece_t *pc, int source_ri)
{
	const texrec_t source = H.rec[source_ri];
	const float saved_tsx = P.tsx, saved_tsy = P.tsy;
	const u64 saved_clamp = P.clamp;
	const u32 sx0 = H.sciss_x0, sx1 = H.sciss_x1, sy0 = H.sciss_y0, sy1 = H.sciss_y1;
	float px[3], py[3], rq[3], us[3], vs[3], inv_det;
	int64_t det;
	int x0, x1, y0, y1, i, batch_n = 0;

	if (H.vw <= 0 || H.vw > 1024) return;
	if (tile_one_core(tri, pc, &source, &x0, &y0))
	{
		if (!tile_draw_window(tri, pc, &source, x0, y0))
			hw_failure("lossless single-core texture source unavailable");
		else
			tile_core_fast_triangles++;
		P.r = &H.rec[source_ri];
		P.tsx = saved_tsx; P.tsy = saved_tsy; P.clamp = saved_clamp;
		H.sciss_x0 = sx0; H.sciss_x1 = sx1; H.sciss_y0 = sy0; H.sciss_y1 = sy1;
		P.serial = H.serial - 1;
		return;
	}
	for (i = 0; i < 3; i++)
	{
		rq[i] = 1.0f / tri[i].w;
		px[i] = (float)(int)(P.ox + tri[i].x * rq[i] * P.kx);
		py[i] = (float)(int)(P.oy + tri[i].y * rq[i] * P.ky);
		us[i] = (tri[i].a[0] - pc->sb) * (float)source.sw * rq[i];
		vs[i] = (tri[i].a[1] - pc->tb) * (float)source.sh * rq[i];
	}
	det = (int64_t)((int)px[1] - (int)px[0]) * ((int)py[2] - (int)py[0])
		- (int64_t)((int)px[2] - (int)px[0]) * ((int)py[1] - (int)py[0]);
	if (!det) return;
	inv_det = 1.0f / (float)det;
	x0 = (int)floorf(hw_minf(px[0], hw_minf(px[1], px[2])) * (1.0f / 16.0f)) - 2048;
	x1 = (int)floorf(hw_maxf(px[0], hw_maxf(px[1], px[2])) * (1.0f / 16.0f)) - 2048 + 1;
	y0 = (int)floorf(hw_minf(py[0], hw_minf(py[1], py[2])) * (1.0f / 16.0f)) - 2048;
	y1 = (int)floorf(hw_maxf(py[0], hw_maxf(py[1], py[2])) * (1.0f / 16.0f)) - 2048 + 1;
	if (x0 < (int)sx0) x0 = (int)sx0;
	if (x1 > (int)sx1 + 1) x1 = (int)sx1 + 1;
	if (y0 < (int)sy0) y0 = (int)sy0;
	if (y1 > (int)sy1 + 1) y1 = (int)sy1 + 1;
	if (x0 < 0) x0 = 0;
	if (x1 > H.vw) x1 = H.vw;
	if (y0 < 0) y0 = 0;
	if (y1 > H.vh) y1 = H.vh;
	if (x0 >= x1 || y0 >= y1) return;
	{
		int wx, wy;

		// Keep the fallback's empty-box rejection before borrowing or uploading.
		// Only free adjacent blocks may extend the already-required 16 KiB image.
		if (tile_one_wide(tri, pc, &source, &wx, &wy)
			&& tile_cache_get() != NOREC && tile_cache_try_borrow())
		{
			if (!tile_draw_window_layout(tri, pc, &source, wx, wy, 1))
				hw_failure("lossless borrowed-window texture source unavailable");
			else
				tile_core_fast_triangles++;
			P.r = &H.rec[source_ri];
			P.tsx = saved_tsx; P.tsy = saved_tsy; P.clamp = saved_clamp;
			H.sciss_x0 = sx0; H.sciss_x1 = sx1; H.sciss_y0 = sy0; H.sciss_y1 = sy1;
			P.serial = H.serial - 1;
			return;
		}
	}
	// Iterative binary subdivision has bounded workspace. Unsupported depth
	// falls back to individual original pixel keys, without a graphics limit.
	tile_batch_t batch[H.vw];
	tile_box_t pending[32];
	const u32 workbytes = (u32)(sizeof pending + sizeof batch + 2 * H.vw * sizeof(int));
	if (workbytes > tile_run_peak_bytes) tile_run_peak_bytes = workbytes;
	tile_stencil_begin(x0, y0, x1, y1);
	tile_plane_t plane;
	for(i=0;i<3;i++) { plane.x[i]=(int)px[i];plane.y[i]=(int)py[i];plane.rq[i]=rq[i];plane.us[i]=us[i];plane.vs[i]=vs[i]; }
	plane.inv_det=inv_det;plane.det=det;
	tile_box_t box={x0,x1,y0,y1};int top=0;
	const int rows = tile_emit_rows(tri, pc, &source, &plane, &box, batch, &batch_n);
	if (rows < 0) goto failed;
	if (rows > 0) goto flush;
	for(;;)
	{
		int ox,oy;
		const int kind=tile_box_classify(&plane,&box,&source,&ox,&oy);
		if(kind || (box.x1-box.x0==1 && box.y1-box.y0==1))
		{
			if(!kind)tile_pixel_key(&plane,box.x0,box.y0,&source,&ox,&oy);
			tile_run_t run={box.x0,box.x1,box.y0,ox,oy};
			if(!tile_batch_add(tri,pc,&source,batch,&batch_n,&run,box.y1))goto failed;
		}
		else if(top<(int)(sizeof pending/sizeof pending[0]))
		{
			tile_box_t right=box;
			if(box.x1-box.x0>=box.y1-box.y0 && box.x1-box.x0>1) { const int mid=box.x0+(box.x1-box.x0)/2;box.x1=mid;right.x0=mid; }
			else { const int mid=box.y0+(box.y1-box.y0)/2;box.y1=mid;right.y0=mid; }
			pending[top++]=right;continue;
		}
		else
		{
			// The supported GS layouts never exhaust 32 DFS entries. Keep an
			// exact bounded fallback for any larger legal box as well.
			for(int y=box.y0;y<box.y1;y++)for(int x=box.x0;x<box.x1;x++)
			{
				tile_pixel_key(&plane,x,y,&source,&ox,&oy);
				tile_run_t run={x,x+1,y,ox,oy};
				if(!tile_batch_add(tri,pc,&source,batch,&batch_n,&run,y+1))goto failed;
			}
		}
		if(!top)break;
		box=pending[--top];
	}
flush:
	if(!tile_batch_flush(tri,pc,&source,batch,&batch_n))goto failed;
	goto done;
failed:
	tile_stencil_end();
	hw_failure("lossless texture tile allocation/source unavailable");
done:
	tile_stencil_end();
	P.r = &H.rec[source_ri];
	P.tsx = saved_tsx; P.tsy = saved_tsy; P.clamp = saved_clamp;
	H.sciss_x0 = sx0; H.sciss_x1 = sx1; H.sciss_y0 = sy0; H.sciss_y1 = sy1;
	P.serial = H.serial - 1;
}

static void tile_emit_piece(const cv_t *v, int cnt, const piece_t *pc)
{
	const int source_ri = (int)(P.r - H.rec);
	int k;
	for (k = 1; k + 1 < cnt; k++)
	{
		cv_t tri[3];
		if (pc->ripple)
		{
			const int first = k - 1;
			tri[0] = v[first]; tri[1] = v[first + 1 + (first & 1)]; tri[2] = v[first + 2 - (first & 1)];
		}
		else { tri[0] = v[0]; tri[1] = v[k]; tri[2] = v[k + 1]; }
		tile_emit_triangle(tri, pc, source_ri);
	}
}
