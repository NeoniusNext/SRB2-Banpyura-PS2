// Keep the original GS primitive and every STQ/RGB/XYZ bit. Source cells select
// disjoint Z24-high-byte stencil rectangles, never new vertices or scissors. REGION_REPEAT masks the
// original integer texel coordinates into a rolled 64 x 64 image. A 32-texel
// cell has sixteen original neighboring texels on each side inside that image.
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

static int tile_draw_window(const cv_t tri[3], const piece_t *pc, const texrec_t *source, int ox, int oy)
{
	texrec_t tile;
	piece_t part = *pc;

	if (OV.n) ov_flush();
	if (!tile_load_region(source, ox, oy,
		P.wrapx && !(source->region & 1), P.wrapy && !(source->region & 2), &tile))
		return 0;
	// TW/TH and coordinate scale retain the original texture's units. The
	// repeat mask, not a change of STQ basis, limits physical VRAM addresses.
	tile.tw = source->tw > 10 ? 10 : source->tw;
	tile.th = source->th > 10 ? 10 : source->th;
	P.r = &tile;
	P.tsx = (float)source->sw / (float)(1u << tile.tw);
	P.tsy = (float)source->sh / (float)(1u << tile.th);
	P.clamp = 3ull | (3ull << 2) | ((u64)(TILE_SIZE - 1) << 4) | ((u64)(TILE_SIZE - 1) << 24);
	P.serial = H.serial - 1;
	part.ripple = 0;
	const u32 before_vertices = H.st.verts_out;
	emit_piece(tri, 3, &part);
	if (OV.n) ov_flush();
	tile_stencil_triangle_submissions += (H.st.verts_out - before_vertices) / 3u;
	tile_stencil_bbox_pixels += (u64)(H.vw) * (u64)(H.vh) * ((H.st.verts_out - before_vertices) / 3u);
	return 1;
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

static int tile_core_origin(float lo, float hi, int size, int wrap, int *out)
{
	// Keep one complete texel of original neighbors beyond both vertex extrema.
	// The physical window contains origin-16 through origin+47 inclusively.
	const int first = (int)ceilf(hi - (float)(TILE_SIZE * 3 / 4 - 2));
	const int last = (int)floorf(lo + (float)(TILE_SIZE / 4 - 1));
	int origin;

	if (first > last) return 0;
	origin = first + (last - first) / 2;
	if (wrap) origin %= size < TILE_SIZE ? TILE_SIZE : size;
	else if (origin < -2 * TILE_SIZE) origin = -2 * TILE_SIZE;
	else if (origin > size + 2 * TILE_SIZE) origin = size + 2 * TILE_SIZE;
	*out = origin;
	return 1;
}

// Positive-Q projective coordinates stay between the original vertex extrema.
// Test those RAW unwrapped extrema before choosing one rolled 64-texel window;
// equal wrapped keys alone could hide a whole repeat. Inclusive 0..32 cores
// and wider nonaligned ranges are safe when the explicit neighbor margin fits.
static int tile_one_core(const cv_t tri[3], const piece_t *pc, const texrec_t *source, int *ox, int *oy)
{
	float min_u = 0.0f, max_u = 0.0f, min_v = 0.0f, max_v = 0.0f;
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
	return tile_core_origin(min_u, max_u, source->w, P.wrapx && !(source->region & 1), ox)
		&& tile_core_origin(min_v, max_v, source->h, P.wrapy && !(source->region & 2), oy);
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
	const float bu=floorf(min_u*(1.0f/TILE_REGION_CORE))*TILE_REGION_CORE;
	const float bv=floorf(min_v*(1.0f/TILE_REGION_CORE))*TILE_REGION_CORE;
	if(max_u>bu+TILE_REGION_CORE||max_v>bv+TILE_REGION_CORE) return 0;
	// Reject ill-conditioned extrapolation. A conservative 64-ulp expression
	// allowance must stay within four texels, leaving >10 original halo texels
	// beyond both extrema even for bilinear filtering. Ambiguity just subdivides.
	const float qe=0x1p-17f*max_qa, room=4.0f*(min_q-qe);
	if(!(room>0.0f)||0x1p-17f*max_ua+hw_maxf(fabsf(min_u),fabsf(max_u))*qe>room
		||0x1p-17f*max_va+hw_maxf(fabsf(min_v),fabsf(max_v))*qe>room) return 0;
	*ox=tile_region_origin(bu,source->w,P.wrapx&&!(source->region&1));
	*oy=tile_region_origin(bv,source->h,P.wrapy&&!(source->region&2));
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
		*ox=tile_region_origin(u,source->w,P.wrapx&&!(source->region&1));
		*oy=tile_region_origin(v,source->h,P.wrapy&&!(source->region&2));
	}
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
	// Iterative binary subdivision has bounded workspace. Unsupported depth
	// falls back to individual original pixel keys, without a graphics limit.
	tile_batch_t batch[H.vw];
	tile_box_t pending[32];
	const u32 workbytes = (u32)(sizeof pending + sizeof batch);
	if (workbytes > tile_run_peak_bytes) tile_run_peak_bytes = workbytes;
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
	tile_stencil_begin(x0, y0, x1, y1);
	tile_plane_t plane;
	for(i=0;i<3;i++) { plane.x[i]=(int)px[i];plane.y[i]=(int)py[i];plane.rq[i]=rq[i];plane.us[i]=us[i];plane.vs[i]=vs[i]; }
	plane.inv_det=inv_det;plane.det=det;
	tile_box_t box={x0,x1,y0,y1};int top=0;
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
