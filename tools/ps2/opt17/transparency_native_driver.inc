/* Test-only exports appended to the real driver translation unit by the
 * isolated builder. They select valid planner inputs and expose diagnostics;
 * texture admission, conversion, CLUT construction, DMA and GS remain real. */
int PS2HWD_T17Upload(GLMipmap_t *m, unsigned int level)
{
	int saved = imm_level, ri;
	imm_level = (int)level;
	ri = tex_upload(m);
	imm_level = saved;
	return ri != NOREC;
}

int PS2HWD_T17View(GLMipmap_t *m, GLMipmap_t *owner)
{
	texrec_t *r = rec_of(owner);
	return r && tex_view(m, (int)(r - H.rec)) != NOREC;
}

int PS2HWD_T17Info(GLMipmap_t *m, unsigned int *out)
{
	static u32 pixels[64 * 16] __attribute__((aligned(64)));
	texrec_t *r = rec_of(m);
	u32 cbp, loaded;
	u64 t0, t1, clamp;
	if (!r)
		return 0;
	cbp = clut_get(r->clut == 2 ? CK_KEYED : CK_OPAQUE, 0, 0);
	if (PS2HWD_ReadVram(pixels, cbp, 1, PSM_CT32, 0, 0, 64, 16))
		return 0;
	loaded = H.clut_loaded;
	tex_regs(r, 0, 0, 0, cbp, 0, &t0, &t1, &clamp);
	H.clut_loaded = loaded; /* a query does not submit this hypothetical TEX0 */
	out[0] = m->ps2_h255;
	out[1] = r->has255;
	out[2] = r->clut;
	out[3] = pixels[15 * 64 + 15]; /* index 255 is fixed by the CSM1 swap */
	out[4] = cbp;
	out[5] = (u32)t0;
	out[6] = (u32)(t0 >> 32);
	out[7] = r->dx;
	return 1;
}

void PS2HWD_T17Select(GLMipmap_t *m)
{
	texrec_t *r = rec_of(m);
	H.cur_tex = r ? (int)(r - H.rec) : NOREC;
	H.cur_missing = r == NULL;
	H.imm_tex = NULL;
}
