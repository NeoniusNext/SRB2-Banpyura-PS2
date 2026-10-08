// CPU model interpolation and bounded triangle staging; GS has no OpenGL VBOs.
static void draw_model(model_t *model, INT32 frameIndex, float duration, float tics,
	INT32 nextFrameIndex, FTransform *pos, float hscale, float vscale,
	UINT8 flipped, UINT8 hflipped, FSurfaceInfo *surf)
{
	static FOutVector vertices[192];
	static u32 indices[192];
	float saved_mv[16], object_mv[16], lerp = 0.0f;
	u32 flags;
	int mi, k;

	if (!H.up || !model || !pos || !surf || frameIndex < 0 || nextFrameIndex < -1)
		return;
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
	for (k = 0; k < 192; k++)
		indices[k] = (u32)k;
	for (mi = 0; mi < model->numMeshes; mi++)
	{
		mesh_t *mesh = &model->meshes[mi];
		int fi, ni, start, tiny = mesh->tinyframes != NULL;
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
		memcpy(H.mv, object_mv, sizeof H.mv);
		if (tiny)
			m_scale(H.mv, 1.0f / 64.0f, 1.0f / 64.0f, 1.0f / 64.0f);
		H.mat_dirty = 1;
		if (!begin_draw(flags, surf))
			break;
		P.cull = (flipped ^ hflipped ^ pos->flip ^ pos->mirror) ? -1 : 1;
		for (start = 0; start < mesh->numTriangles * 3; start += 192)
		{
			int n = mesh->numTriangles * 3 - start, valid = 1;
			if (n > 192)
				n = 192;
			for (k = 0; k < n; k++)
			{
				int vi = tiny ? mesh->indices[start + k] : start + k, j;
				float xyz[3];
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
				vertices[k].x = xyz[0]; vertices[k].y = xyz[1]; vertices[k].z = xyz[2];
				vertices[k].s = mesh->uvs[vi * 2]; vertices[k].t = mesh->uvs[vi * 2 + 1];
			}
			if (!valid)
			{
				hw_limit(HW_INVALID, "model index out of range; batch rejected");
				continue;
			}
			emit_tris(vertices, indices, (u32)n, NULL);
		}
	}
	P.cull = 0;
	memcpy(H.mv, saved_mv, sizeof H.mv);
	H.mat_dirty = 1;
}
