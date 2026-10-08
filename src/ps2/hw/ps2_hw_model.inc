// Models (MD3) on the GS (included by ps2_hwd.c). OPT11-MODEL (PS2-HW-264..): see docs/GATES/g1/opt11-MODEL.md.
//
// A model draws as the OpenGL driver does it (r_opengl.c DrawModelEx): the object matrix (position, angles, roll, scale; the 1/64 of the tinyframe vertices), the vertices of the two frames
// interpolated as shorts, the triangles of the mesh by index, the polygon's light (PF_ColorMapped, as sprites: the sector light through the CLUT rows / fog bands of the polygon plan), and with
// gr_modellighting the vertex shader's factor min(0.75 + max(n.y, 0), 1) (r_opengl.c / hw_shaders.h: dot(gl_Normal, (0, 1, 0)) with the model's own up component of the normal).
//
// The cost is per UNIQUE vertex (interpolation, light, transform): the OPT10 code interpolated and transformed every triangle corner (3 per vertex of a closed mesh).
// The vertices of a mesh go into a work area as FOutVector (positions are integers of 1/64 unit, the u, v of the mesh), the triangles into emit_tris as indices: its vertex cache
// (xcache) then transforms each vertex once. A mesh of more than MDL_MAXV vertices (the boss BRAK has 6061) takes the corner by corner path of OPT10.

#define MDL_MAXV 2048 // vertices of a mesh drawn through the work area
#define MDL_MAXI 3072 // indices per emit_tris call
#define MDL_CORNERS 192 // corners per call of the corner by corner path

typedef struct
{
	FOutVector v[MDL_MAXV];
	float l[MDL_MAXV]; // the light factor of each vertex (only with gr_modellighting)
	u32 idx[MDL_MAXI];
} mdlwork_t;

static mdlwork_t *mdlw; // made when the first model is drawn (zone block: a game without models does not pay for it)
static float mdl_lf[256]; // the light factor by the byte of the normal: index = c + 128
static int mdl_lf_ready;

static void mdl_init_tables(void)
{
	int c;

	for (c = -128; c < 128; c++)
	{
		// gl_Normal of a GL_BYTE normal array: (2c + 1) / 255; light = min(0.75 + max(n, 0), 1)
		const float n = (float)(2 * c + 1) * (1.0f / 255.0f);
		float l = 0.75f + (n > 0.0f ? n : 0.0f);

		mdl_lf[c + 128] = l > 1.0f ? 1.0f : l;
	}
	mdl_lf_ready = 1;
}

static int mdl_work_get(void)
{
	if (mdlw)
		return 1;
	mdlw = (mdlwork_t *)Z_TryMallocAlign(sizeof *mdlw, PU_STATIC, NULL, 6);
	if (!mdlw)
	{
		hw_limit(HW_INVALID, "no memory for the model work area; models are not drawn");
		return 0;
	}
	if (!mdl_lf_ready)
		mdl_init_tables();
	return 1;
}

// the vertices (and light factors) of one mesh at the interpolated frame fi..ni into the work area
static void mdl_interp(const mesh_t *mesh, int fi, int ni, float lerp, int light)
{
	const int nv = mesh->numVertices;
	const short *a = mesh->tinyframes[fi].vertices, *b = mesh->tinyframes[ni].vertices;
	const signed char *na = (const signed char *)mesh->tinyframes[fi].normals, *nb = (const signed char *)mesh->tinyframes[ni].normals;
	const float *uv = mesh->uvs;
	FOutVector *o = mdlw->v;
	int k;

	if (lerp == 0.0f || fi == ni)
	{
		for (k = 0; k < nv; k++, a += 3)
		{
			o[k].x = (float)a[0];
			o[k].y = (float)a[1];
			o[k].z = (float)a[2];
			o[k].s = uv[k * 2];
			o[k].t = uv[k * 2 + 1];
		}
		if (light)
			for (k = 0; k < nv; k++)
				mdlw->l[k] = mdl_lf[na[k] + 128];
		return;
	}
	for (k = 0; k < nv; k++, a += 3, b += 3)
	{
		// (short)(a + pol * (b - a)): the interpolated frame of the OpenGL driver
		o[k].x = (float)(short)((float)a[0] + lerp * (float)(b[0] - a[0]));
		o[k].y = (float)(short)((float)a[1] + lerp * (float)(b[1] - a[1]));
		o[k].z = (float)(short)((float)a[2] + lerp * (float)(b[2] - a[2]));
		o[k].s = uv[k * 2];
		o[k].t = uv[k * 2 + 1];
	}
	if (light)
		for (k = 0; k < nv; k++)
		{
			const int c = (signed char)(int)((float)na[k] + lerp * (float)(nb[k] - na[k])); // (char)(a + pol * (b - a))

			mdlw->l[k] = mdl_lf[c + 128];
		}
}

// the corner by corner path (OPT10): used for meshes the work area cannot hold and for models in the float format
static void mdl_mesh_corners(const mesh_t *mesh, int fi, int ni, float lerp, int tiny, int light)
{
	int start;
	u32 k;

	for (k = 0; k < MDL_CORNERS; k++)
		mdlw->idx[k] = k;
	for (start = 0; start < mesh->numTriangles * 3; start += MDL_CORNERS)
	{
		int n = mesh->numTriangles * 3 - start, valid = 1, c;

		if (n > MDL_CORNERS)
			n = MDL_CORNERS;
		for (c = 0; c < n; c++)
		{
			const int vi = tiny ? mesh->indices[start + c] : start + c;
			float xyz[3];
			int j;

			if (vi < 0 || vi >= mesh->numVertices)
			{
				valid = 0;
				break;
			}
			for (j = 0; j < 3; j++)
			{
				if (tiny)
				{
					const short *a = mesh->tinyframes[fi].vertices, *b = mesh->tinyframes[ni].vertices;

					xyz[j] = (float)(short)(a[vi * 3 + j] + lerp * (b[vi * 3 + j] - a[vi * 3 + j]));
				}
				else
				{
					const float *a = mesh->frames[fi].vertices, *b = mesh->frames[ni].vertices;

					xyz[j] = a[vi * 3 + j] + lerp * (b[vi * 3 + j] - a[vi * 3 + j]);
				}
			}
			mdlw->v[c].x = xyz[0];
			mdlw->v[c].y = xyz[1];
			mdlw->v[c].z = xyz[2];
			mdlw->v[c].s = mesh->uvs[vi * 2];
			mdlw->v[c].t = mesh->uvs[vi * 2 + 1];
			if (light)
			{
				if (tiny)
				{
					const signed char *na = (const signed char *)mesh->tinyframes[fi].normals, *nb = (const signed char *)mesh->tinyframes[ni].normals;

					mdlw->l[c] = mdl_lf[(signed char)(int)((float)na[vi] + lerp * (float)(nb[vi] - na[vi])) + 128];
				}
				else
					mdlw->l[c] = 1.0f;
			}
		}
		if (!valid)
		{
			hw_limit(HW_INVALID, "model index out of range; batch rejected");
			continue;
		}
		emit_tris(mdlw->v, mdlw->idx, (u32)n, light ? mdlw->l : NULL);
		G.md_slow++;
	}
}

static void draw_model(model_t *model, INT32 frameIndex, float duration, float tics,
	INT32 nextFrameIndex, FTransform *pos, float hscale, float vscale,
	UINT8 flipped, UINT8 hflipped, FSurfaceInfo *surf)
{
	float saved_mv[16], object_mv[16], lerp = 0.0f;
	const u32 t0 = cyc();
	u32 flags;
	int mi;
	const int light_on = H.model_light && !(ps2hwd_model_ab & 2);

	if (!H.up || !model || !pos || !surf || frameIndex < 0 || nextFrameIndex < -1)
		return;
	if (!mdl_work_get())
		return;
	G.md_draws++;
	if (duration > 0.0f && tics >= 0.0f)
		lerp = clampf((duration - tics) / duration, 0.0f, 1.0f);
	memcpy(saved_mv, H.mv, sizeof saved_mv);
	m_translate(H.mv, pos->x, pos->z, pos->y);
	m_rotate(H.mv, pos->anglez, 0.0f, 0.0f, -1.0f);
	m_rotate(H.mv, pos->anglex, 1.0f, 0.0f, 0.0f);
	m_rotate(H.mv, pos->angley, 0.0f, -1.0f, 0.0f);
	if (pos->roll)
	{
		m_translate(H.mv, pos->centerx, pos->centery, 0.0f);
		m_rotate(H.mv, pos->rollangle, pos->rollx, 0.0f, pos->rollz);
		m_translate(H.mv, -pos->centerx, -pos->centery, 0.0f);
	}
	m_scale(H.mv, hscale * 0.5f, vscale * (flipped ? -0.5f : 0.5f), hscale * (hflipped ? -0.5f : 0.5f));
	memcpy(object_mv, H.mv, sizeof object_mv);
	flags = (u32)surf->PolyFlags | PF_Modulated;
	// PS2-HW-126: the engine selects SHADER_MODEL (HWD.pfnSetShader) and the OpenGL driver always runs it, so hw_md2.c does not put PF_ColorMapped into the surface flags;
	// begin_draw lights only PF_ColorMapped polygons: a model drew at full brightness whatever the sector light, colormap or RF_FULLDARK said.
	if (H.shaders_on && H.shader >= 0 && H.shader <= 5)
		flags |= PF_ColorMapped;
	if (surf->PolyFlags & (PF_Additive | PF_Subtractive | PF_ReverseSubtract | PF_Multiplicative))
		flags |= PF_Occlude;
	else if (surf->PolyColor.s.alpha == 255)
		flags |= PF_Occlude | PF_Masked;
	for (mi = 0; mi < model->numMeshes; mi++)
	{
		const mesh_t *mesh = &model->meshes[mi];
		int fi, ni, tiny = mesh->tinyframes != NULL, light;

		if (mesh->numFrames <= 0 || mesh->numTriangles < 0 || mesh->numTriangles > 0x1FFFFFFF
			|| !mesh->uvs || (tiny ? !mesh->indices : !mesh->frames))
		{
			hw_limit(HW_INVALID, "invalid model mesh; draw rejected");
			continue;
		}
		fi = frameIndex % mesh->numFrames;
		ni = nextFrameIndex == -1 ? fi : nextFrameIndex % mesh->numFrames;
		if (tiny ? (!mesh->tinyframes[fi].vertices || !mesh->tinyframes[ni].vertices)
			: (!mesh->frames[fi].vertices || !mesh->frames[ni].vertices))
		{
			hw_limit(HW_INVALID, "model frame vertices unavailable; mesh rejected");
			continue;
		}
		light = light_on && tiny && mesh->tinyframes[fi].normals && mesh->tinyframes[ni].normals;
		memcpy(H.mv, object_mv, sizeof H.mv);
		if (tiny)
			m_scale(H.mv, 1.0f / 64.0f, 1.0f / 64.0f, 1.0f / 64.0f);
		H.mat_dirty = 1;
		if (!begin_draw(flags, surf))
			break;
		P.cull = (flipped ^ hflipped ^ pos->flip ^ pos->mirror) ? -1 : 1;
		if (light)
		{
			P.vcol = 1; // the vertices carry their light factor (a[2]); the polygon's colour modulates it (put_vertex format 6)
			P.vlight = 1;
		}
		G.md_meshes++;
		G.md_tris += (u32)mesh->numTriangles;
		if (tiny && mesh->numVertices <= MDL_MAXV && !(ps2hwd_model_ab & 1))
		{
			int start, total = mesh->numTriangles * 3;

			G.md_verts += (u32)mesh->numVertices;
			mdl_interp(mesh, fi, ni, lerp, light);
			for (start = 0; start < total; start += MDL_MAXI)
			{
				const int n = total - start < MDL_MAXI ? total - start : MDL_MAXI;
				const unsigned short *ix = mesh->indices + start;
				int k;

				for (k = 0; k < n; k++)
					mdlw->idx[k] = ix[k];
				emit_tris(mdlw->v, mdlw->idx, (u32)n, light ? mdlw->l : NULL);
			}
		}
		else
		{
			mdl_mesh_corners(mesh, fi, ni, lerp, tiny, light);
		}
		P.vcol = 0;
		P.vlight = 0;
	}
	P.cull = 0;
	memcpy(H.mv, saved_mv, sizeof H.mv);
	H.mat_dirty = 1;
	G.md_cyc += cyc() - t0;
}
