// PS2 GS hardware renderer driver: matrices, viewport, clipping (included by ps2_hwd.c).
// Matrices are column-major like OpenGL's: m[col * 4 + row], v' = M * v. The calls below reproduce what r_opengl.c does
// with glRotatef/glTranslatef/glScalef/GLPerspective so the engine's FTransform means the same thing here.

#define FAR_CLIPPING_PLANE 32768.0f
#define NZCLIP_DEFAULT 0.9f
#define FOV_2D 90.0f // r_opengl.c keeps `fov` = 90 for GClipRect
#ifndef BASEVIDHEIGHT
#define BASEVIDHEIGHT 200
#endif

static void m_identity(float *m)
{
	int i;

	for (i = 0; i < 16; i++)
		m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
}

static void m_mul(float *o, const float *a, const float *b) // o = a * b
{
	float t[16];
	int c, r;

	for (c = 0; c < 4; c++)
		for (r = 0; r < 4; r++)
			t[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1] + a[2 * 4 + r] * b[c * 4 + 2] + a[3 * 4 + r] * b[c * 4 + 3];
	memcpy(o, t, sizeof t);
}

static void m_translate(float *m, float x, float y, float z) // m = m * T
{
	float t[16];

	m_identity(t);
	t[12] = x;
	t[13] = y;
	t[14] = z;
	m_mul(m, m, t);
}

static void m_scale(float *m, float x, float y, float z)
{
	float t[16];

	m_identity(t);
	t[0] = x;
	t[5] = y;
	t[10] = z;
	m_mul(m, m, t);
}

static void m_rotate(float *m, float deg, float x, float y, float z)
{
	float t[16], rad = deg * (3.14159265358979f / 180.0f), c = cosf(rad), s = sinf(rad), len = sqrtf(x * x + y * y + z * z), ic;

	if (len < 1e-12f)
		return;
	x /= len;
	y /= len;
	z /= len;
	ic = 1.0f - c;
	m_identity(t);
	t[0] = x * x * ic + c;
	t[1] = y * x * ic + z * s;
	t[2] = z * x * ic - y * s;
	t[4] = x * y * ic - z * s;
	t[5] = y * y * ic + c;
	t[6] = z * y * ic + x * s;
	t[8] = x * z * ic + y * s;
	t[9] = y * z * ic - x * s;
	t[10] = z * z * ic + c;
	m_mul(m, m, t);
}

// GLPerspective of r_opengl.c: m = m * P
static void m_perspective(float *m, float fovy, float aspect, float zn, float zf)
{
	float p[16], radians = fovy / 2.0f * (3.14159265358979f / 180.0f), sine = sinf(radians), dz = zf - zn, cot;

	if (fabsf(dz) < 1.0E-36f || sine == 0.0f || aspect == 0.0f)
		return;
	cot = cosf(radians) / sine;
	memset(p, 0, sizeof p);
	p[0] = cot / aspect;
	p[5] = cot;
	p[10] = -(zf + zn) / dz;
	p[11] = -1.0f;
	p[14] = -2.0f * zn * zf / dz;
	m_mul(m, m, p);
}

static void xf_update(void)
{
	if (H.mat_dirty)
	{
		m_mul(H.mvp, H.proj, H.mv);
		H.mat_dirty = 0;
	}
}

// r_opengl.c GClipRect: the projection of 2D drawing (and of screen textures) with the near plane the engine asks for
static void xf_set_2d(float nearclip)
{
	H.near_plane = nearclip;
	m_identity(H.proj);
	m_perspective(H.proj, FOV_2D, 1.0f, H.near_plane, FAR_CLIPPING_PLANE);
	H.mat_dirty = 1;
}

// r_opengl.c SetTransform: model view, then the projection with the field of view of the transform
static void xf_set_transform(const FTransform *stransform)
{
	float used_fov = FOV_2D;
	int shearing = 0;

	m_identity(H.mv);
	if (stransform)
	{
		used_fov = stransform->fovxangle;
		if (stransform->mirror)
			m_scale(H.mv, -stransform->scalex, stransform->scaley, -stransform->scalez);
		else if (stransform->flip)
			m_scale(H.mv, stransform->scalex, -stransform->scaley, -stransform->scalez);
		else
			m_scale(H.mv, stransform->scalex, stransform->scaley, -stransform->scalez);
		if (stransform->roll)
			m_rotate(H.mv, stransform->rollangle, 0.0f, 0.0f, 1.0f);
		m_rotate(H.mv, stransform->anglex, 1.0f, 0.0f, 0.0f);
		m_rotate(H.mv, stransform->angley + 270.0f, 0.0f, 1.0f, 0.0f);
		m_translate(H.mv, -stransform->x, -stransform->z, -stransform->y);
		H.split_special = stransform->splitscreen; // the OpenGL driver keeps this in a static: it outlives the call
		shearing = stransform->shearing;
	}
	else
	{
		m_scale(H.mv, 1.0f, 1.0f, -1.0f);
	}
	m_identity(H.proj);
	if (shearing)
	{
		float fdy = stransform->viewaiming * 2.0f;

		if (stransform->flip)
			fdy *= -1.0f;
		m_translate(H.proj, 0.0f, -fdy / (float)BASEVIDHEIGHT, 0.0f);
	}
	if (H.split_special)
	{
		used_fov = (float)(atanf(tanf(used_fov * 3.14159265358979f / 360.0f) * 0.8f) * 360.0f / 3.14159265358979f);
		m_perspective(H.proj, used_fov, 2.0f, H.near_plane, FAR_CLIPPING_PLANE);
	}
	else
	{
		m_perspective(H.proj, used_fov, 1.0f, H.near_plane, FAR_CLIPPING_PLANE);
	}
	H.mat_dirty = 1;
}

// engine viewport (top-left origin, engine pixels) -> frame buffer pixels, scissor and the guard band
static void viewport_set(INT32 minx, INT32 miny, INT32 maxx, INT32 maxy)
{
	float cx, cy, hw, hh, gx, gy, a, b;
	int x0, y0, x1, y1;

	H.vpx = (float)minx * H.sx;
	H.vpy = (float)miny * H.sy;
	H.vpw = (float)(maxx - minx) * H.sx;
	H.vph = (float)(maxy - miny) * H.sy;
	x0 = (int)floorf(H.vpx + 0.001f);
	y0 = (int)floorf(H.vpy + 0.001f);
	x1 = (int)ceilf(H.vpx + H.vpw - 0.001f) - 1;
	y1 = (int)ceilf(H.vpy + H.vph - 0.001f) - 1;
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > H.vw - 1)
		x1 = H.vw - 1;
	if (y1 > H.vh - 1)
		y1 = H.vh - 1;
	if (x1 < x0)
		x1 = x0;
	if (y1 < y0)
		y1 = y0;
	H.sciss_x0 = (u32)x0;
	H.sciss_y0 = (u32)y0;
	H.sciss_x1 = (u32)x1;
	H.sciss_y1 = (u32)y1;
	// vertices are clipped so that their frame buffer coordinates stay inside +-1900 (the GS holds -2048..2047.94)
	cx = H.vpx + H.vpw * 0.5f;
	cy = H.vpy + H.vph * 0.5f;
	hw = H.vpw * 0.5f;
	hh = H.vph * 0.5f;
	if (hw < 1.0f)
		hw = 1.0f;
	if (hh < 1.0f)
		hh = 1.0f;
	a = (1900.0f + cx) / hw;
	b = (1900.0f - cx) / hw;
	gx = a < b ? a : b;
	a = (1900.0f + cy) / hh;
	b = (1900.0f - cy) / hh;
	gy = a < b ? a : b;
	if (gx < 1.0f)
		gx = 1.0f;
	if (gy < 1.0f)
		gy = 1.0f;
	H.guard_x = (int)(gx * 1024.0f);
	H.guard_y = (int)(gy * 1024.0f);
}

// ---- clipping ----

typedef struct
{
	float x, y, z, w;
	float a[6]; // s, t and, for vertex coloured geometry, r, g, b, a
} __attribute__((aligned(16))) cv_t;

// signed distance of a vertex to clip plane p (>= 0 = inside); planes: 0 left, 1 right, 2 bottom, 3 top, 4 near, 5 far
static inline float plane_dist(const cv_t *v, int p, float gx, float gy)
{
	switch (p)
	{
		case 0: return v->x + gx * v->w;
		case 1: return gx * v->w - v->x;
		case 2: return v->y + gy * v->w;
		case 3: return gy * v->w - v->y;
		case 4: return v->z + v->w;
		default: return v->w - v->z;
	}
}

// outcode of a clip space vertex: bit p set = outside plane p
static inline int outcode(const cv_t *v, float gx, float gy)
{
	float w = v->w;

	return (v->x < -gx * w ? 1 : 0) | (v->x > gx * w ? 2 : 0) | (v->y < -gy * w ? 4 : 0) | (v->y > gy * w ? 8 : 0) | (v->z < -w ? 16 : 0) | (v->z > w ? 32 : 0);
}

// Sutherland-Hodgman against the planes in `mask`; returns the vertex count (0 = nothing left); the result is in *res
static int clip_poly(cv_t *a, cv_t *b, int n, int na, int mask, cv_t **res)
{
	cv_t *in = a, *out = b, *t;
	float gx = (float)H.guard_x * (1.0f / 1024.0f), gy = (float)H.guard_y * (1.0f / 1024.0f);
	int p, i, k;

	for (p = 0; p < 6 && n > 0; p++)
	{
		int m = 0;

		if (!(mask & (1 << p)))
			continue;
		for (i = 0; i < n; i++)
		{
			const cv_t *v0 = &in[i], *v1 = &in[i + 1 == n ? 0 : i + 1];
			float d0 = plane_dist(v0, p, gx, gy), d1 = plane_dist(v1, p, gx, gy);

			if (d0 >= 0.0f)
				out[m++] = *v0;
			if ((d0 >= 0.0f) != (d1 >= 0.0f))
			{
				float tt = d0 / (d0 - d1);
				cv_t *o = &out[m++];

				o->x = v0->x + (v1->x - v0->x) * tt;
				o->y = v0->y + (v1->y - v0->y) * tt;
				o->z = v0->z + (v1->z - v0->z) * tt;
				o->w = v0->w + (v1->w - v0->w) * tt;
				for (k = 0; k < na; k++)
					o->a[k] = v0->a[k] + (v1->a[k] - v0->a[k]) * tt;
			}
		}
		n = m;
		t = in;
		in = out;
		out = t;
	}
	*res = in;
	return n;
}
