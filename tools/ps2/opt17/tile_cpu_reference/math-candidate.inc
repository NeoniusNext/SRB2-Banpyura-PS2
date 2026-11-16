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

static u32 tile_run_peak_bytes;

static u32 tile_workspace_peak(void) { return tile_run_peak_bytes; }

static int tile_draw_run(const cv_t tri[3], const piece_t *pc, const texrec_t *source,
	const tile_run_t *run, int y1)
{
	texrec_t tile;
	piece_t part = *pc;

	if (OV.n) ov_flush(); // the queued overlay belongs to the previous scissor
	tile_stencil_select(run->x0, run->y0, run->x1, y1);
	if (!tile_load_region(source, run->ox, run->oy,
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
	tile_stencil_unselect(run->x0, run->y0, run->x1, y1);
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

static void tile_emit_triangle(const cv_t tri[3], const piece_t *pc, int source_ri)
{
	const texrec_t source = H.rec[source_ri];
	const float saved_tsx = P.tsx, saved_tsy = P.tsy;
	const u64 saved_clamp = P.clamp;
	const u32 sx0 = H.sciss_x0, sx1 = H.sciss_x1, sy0 = H.sciss_y0, sy1 = H.sciss_y1;
	float px[3], py[3], rq[3], us[3], vs[3], inv_det;
	int64_t det;
	int x0, x1, y0, y1, i, y, which = 0, prev_n = 0;

	if (H.vw <= 0 || H.vw > 1024) return;
	// Allocated only while this path runs. Each scanline has at most one run
	// per output pixel. Two sorted rows permit bounded vertical coalescing.
	tile_run_t rows[2][H.vw];
	const u32 workbytes = (u32)sizeof rows;
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
	for (y = y0; y < y1; y++)
	{
		tile_run_t *cur = rows[which], *prev = rows[1 - which];
		int n = 0, p = 0, x;
		const int fx0 = (2048 + x0) * 16, fy = (2048 + y) * 16;
		const int64_t step1 = 16ll * ((int)py[2] - (int)py[0]);
		const int64_t step2 = -16ll * ((int)py[1] - (int)py[0]);
		int64_t edge1 = (int64_t)(fx0 - (int)px[0]) * ((int)py[2] - (int)py[0])
			- (int64_t)((int)px[2] - (int)px[0]) * (fy - (int)py[0]);
		int64_t edge2 = (int64_t)((int)px[1] - (int)px[0]) * (fy - (int)py[0])
			- (int64_t)(fx0 - (int)px[0]) * ((int)py[1] - (int)py[0]);
		for (x = x0; x < x1; x++, edge1 += step1, edge2 += step2)
		{
			const int64_t n1 = edge1, n2 = edge2;
			const int64_t n0 = det - n1 - n2;
			const int inside = det > 0 ? (n0 >= 0 && n1 >= 0 && n2 >= 0) : (n0 <= 0 && n1 <= 0 && n2 <= 0);
			int ox = 0, oy = 0;
			// Closed integer edge tests include every possible GS sample. Outside
			// pixels still submit the original primitive; the GS rejects them.
			// Avoid extrapolating Q toward zero outside the actual triangle.
			if (inside)
			{
				const float b1 = (float)n1 * inv_det, b2 = (float)n2 * inv_det;
				const float q = rq[0] + b1 * (rq[1] - rq[0]) + b2 * (rq[2] - rq[0]);
				const float iq = q > 0.0f ? 1.0f / q : 0.0f;
				const float u = (us[0] + b1 * (us[1] - us[0]) + b2 * (us[2] - us[0])) * iq;
				const float v = (vs[0] + b1 * (vs[1] - vs[0]) + b2 * (vs[2] - vs[0])) * iq;
				ox = tile_region_origin(u, source.w, P.wrapx && !(source.region & 1));
				oy = tile_region_origin(v, source.h, P.wrapy && !(source.region & 2));
			}
			if (n && cur[n - 1].ox == ox && cur[n - 1].oy == oy)
				cur[n - 1].x1 = x + 1;
			else
			{
				cur[n].x0 = x; cur[n].x1 = x + 1; cur[n].y0 = y;
				cur[n].ox = ox; cur[n].oy = oy; n++;
			}
		}
		for (i = 0; i < n; i++)
		{
			while (p < prev_n && prev[p].x0 < cur[i].x0)
			{
				if (!tile_draw_run(tri, pc, &source, &prev[p++], y)) goto failed;
			}
			if (p < prev_n && prev[p].x0 == cur[i].x0)
			{
				if (prev[p].x1 == cur[i].x1 && prev[p].ox == cur[i].ox && prev[p].oy == cur[i].oy)
					cur[i].y0 = prev[p].y0;
				else if (!tile_draw_run(tri, pc, &source, &prev[p], y)) goto failed;
				p++;
			}
		}
		while (p < prev_n)
			if (!tile_draw_run(tri, pc, &source, &prev[p++], y)) goto failed;
		prev_n = n;
		which = 1 - which;
	}
	for (i = 0; i < prev_n; i++)
		if (!tile_draw_run(tri, pc, &source, &rows[1 - which][i], y1)) goto failed;
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
