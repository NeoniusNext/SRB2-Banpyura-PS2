// OPT11-MODEL (PS2-HW-262): the skin colour blend of a model texture in palette index space (included by hw_md2.c in place of the RGBA version).
// hw_md2.c's HWR_CreateBlendedTexture works on two RGBA images (256 KB each for 256 x 256, in a zone with 2.5 MB to spare). Here the base texture is the 8 bit indices of the
// pack (the colour of a texel is the entry of the texture palette), the blend map is read from MODELS.PAK for the moment of the blend (sparse runs: alpha, brightness, average of the
// channels of the texels that have a blend alpha) and the result is 8 bit indices again (the nearest palette colour by the rule of the palette rendering shader, as the rest of the
// texture): 64 KB per colour instead of three 256 KB images. The decision per texel is the original's, line by line; a texel the algorithm leaves as it was keeps its index.
// Not exact where the colour of a texel was mixed with a blend alpha between 0 and 255 (the original mixes the colour of the file, here the palette colour it was made into).

#define SETBRIGHTNESS(brightness,r,g,b) \
	brightness = (UINT8)(((1063*(UINT16)(r))/5000) + ((3576*(UINT16)(g))/5000) + ((361*(UINT16)(b))/5000))

static boolean HWR_CreateBlendedTexture(GLMipmap_t *basemip, const ps2_blend_t *bl, GLMipmap_t *grMipmap, INT32 skinnum, skincolornum_t color)
{
	const UINT16 w = basemip->width, h = basemip->height;
	const UINT32 size = (UINT32)w * h;
	const RGBA_t *pal = HWR_GetTexturePalette();
	const UINT8 *src;
	UINT8 *out;
	RGBA_t blendcolor;
	UINT16 translation[16]; // First the color index
	UINT8 cutoff[16]; // Brightness cutoff before using the next color
	UINT8 translen = 0;
	UINT8 i;
	UINT32 n, ri = 0, pxi = 0, runpos = ~0u, runend = 0;
	boolean holes = false;

	blendcolor = V_GetColor(0); // initialize
	memset(translation, 0, sizeof(translation));
	memset(cutoff, 0, sizeof(cutoff));

	if (grMipmap->width == 0)
	{
		grMipmap->width = w;
		grMipmap->height = h;
		grMipmap->flags = TF_CHROMAKEYED;
		grMipmap->format = GL_TEXFMT_P_8;
	}
	if (grMipmap->data)
	{
		Z_Free(grMipmap->data);
		grMipmap->data = NULL;
	}
	if (!basemip->data || (bl && (bl->w != w || bl->h != h)))
		return false;
	out = PS2Models_TryAlloc(size, PU_HWRMODELTEXTURE_UNLOCKED, &grMipmap->data, 4);
	if (!out)
		return false;
	src = basemip->data; // used this frame: the allocation above did not take it (checked again below)
	if (!src)
	{
		Z_Free(out);
		return false;
	}
	memcpy(out, src, size);

	if (bl && bl->nruns)
	{
		runpos = bl->run[0];
		runend = runpos + (bl->run[1] & 0xFFFF);
	}

	// TC_METALSONIC includes an actual skincolor translation, on top of its flashing.
	if (skinnum == TC_METALSONIC)
		color = SKINCOLOR_COBALT;

	if (color != SKINCOLOR_NONE && color < numskincolors)
	{
		UINT8 numdupes = 1;

		translation[translen] = skincolors[color].ramp[0];
		cutoff[translen] = 255;

		for (i = 1; i < 16; i++)
		{
			if (translation[translen] == skincolors[color].ramp[i])
			{
				numdupes++;
				continue;
			}

			if (translen > 0)
			{
				cutoff[translen] = cutoff[translen-1] - (256 / (16 / numdupes));
			}

			numdupes = 1;
			translen++;

			translation[translen] = (UINT16)skincolors[color].ramp[i];
		}

		translen++;
	}

	for (n = 0; n < size; n++)
	{
		const UINT8 idx = src[n];
		UINT8 ba = 0, blum = 0, bavg = 0;
		RGBA_t image, cur;
		boolean skip = false;

		if (bl)
		{
			while (ri < bl->nruns && n >= runend)
			{
				ri++;
				if (ri < bl->nruns)
				{
					runpos = bl->run[ri * 2];
					runend = runpos + (bl->run[ri * 2 + 1] & 0xFFFF);
				}
				else
					runpos = ~0u;
			}
			if (ri < bl->nruns && n >= runpos)
			{
				const UINT8 *q = bl->px + (size_t)pxi * 4;

				ba = q[0];
				blum = q[1];
				bavg = q[2];
				pxi++;
			}
		}
		if (idx == 255) // a hole stays a hole in every case (the original keeps the alpha of the image)
		{
			holes = true;
			out[n] = 255;
			continue;
		}
		image.s.red = pal[idx].s.red;
		image.s.green = pal[idx].s.green;
		image.s.blue = pal[idx].s.blue;
		image.s.alpha = 255;
		cur = image;

		if (skinnum == TC_ALLWHITE)
		{
			// Turn everything white
			cur.s.red = cur.s.green = cur.s.blue = 255;
		}
		else
		{
			// Everything below requires a blend image
			if (!bl)
			{
				skip = true;
			}
			// Metal Sonic dash mode
			else if (skinnum == TC_DASHMODE)
			{
				UINT8 ialpha = 255 - ba, balpha = ba;
				RGBA_t icolor = image, bcolor;

				memset(&bcolor, 0x00, sizeof(RGBA_t));

				if (ba)
				{
					bcolor.s.blue = 0;
					bcolor.s.red = 255;
					bcolor.s.green = bavg;
				}

				if (image.s.alpha && image.s.red > image.s.green << 1) // this is pretty arbitrary, but it works well for Metal Sonic
				{
					icolor.s.red = image.s.blue;
					icolor.s.blue = image.s.red;
				}

				cur.s.red = (ialpha * icolor.s.red + balpha * bcolor.s.red)/255;
				cur.s.green = (ialpha * icolor.s.green + balpha * bcolor.s.green)/255;
				cur.s.blue = (ialpha * icolor.s.blue + balpha * bcolor.s.blue)/255;
			}
			else if (translen <= 0 || (skinnum != TC_RAINBOW && ba == 0)) // all settings that use skincolors: nothing to do (ba == 0: for metal sonic blend)
			{
				skip = true;
			}
			else
			{
				UINT16 brightness;
				RGBA_t nextcolor;
				UINT8 firsti = 0, secondi, mul = 0, mulmax = 1;
				INT32 r, g, b;
				boolean ignore = false;

				if (skinnum == TC_RAINBOW)
				{
					UINT16 imagebright, blendbright = blum;
					SETBRIGHTNESS(imagebright,image.s.red,image.s.green,image.s.blue);
					// slightly dumb average between the blend image color and base image colour, usually one or the other will be fully opaque anyway
					brightness = (imagebright*(255-ba))/255 + (blendbright*ba)/255;
				}
				else
					brightness = blum;

				// Calculate a sort of "gradient" for the skincolor
				// Rainbow needs to find the closest match to the textures themselves, instead of matching brightnesses to other colors.
				// Ensue horrible mess.
				if (skinnum == TC_RAINBOW)
				{
					UINT16 brightdif = 256;
					UINT8 colorbrightnesses[16];
					INT32 compare, m, d;

					// Ignore pure white & pitch black
					if (brightness > 253 || brightness < 2)
					{
						skip = true;
						ignore = true;
						secondi = 0;
					}
					else
					{
						for (i = 0; i < translen; i++)
						{
							RGBA_t tempc = V_GetColor(translation[i]);
							SETBRIGHTNESS(colorbrightnesses[i], tempc.s.red, tempc.s.green, tempc.s.blue); // store brightnesses for comparison
						}

						for (i = 0; i < translen; i++)
						{
							if (brightness > colorbrightnesses[i]) // don't allow greater matches (because calculating a makeshift gradient for this is already a huge mess as is)
								continue;

							compare = abs((INT16)(colorbrightnesses[i]) - (INT16)(brightness));

							if (compare < brightdif)
							{
								brightdif = (UINT16)compare;
								firsti = i; // best matching color that's equal brightness or darker
							}
						}

						secondi = firsti+1; // next color in line
						if (secondi >= translen)
						{
							m = (INT16)brightness; // - 0;
							d = (INT16)colorbrightnesses[firsti]; // - 0;
						}
						else
						{
							m = (INT16)brightness - (INT16)colorbrightnesses[secondi];
							d = (INT16)colorbrightnesses[firsti] - (INT16)colorbrightnesses[secondi];
						}

						if (m >= d)
							m = d-1;

						mulmax = 16;

						// calculate the "gradient" multiplier based on how close this color is to the one next in line
						if (m <= 0 || d <= 0)
							mul = 0;
						else
							mul = (mulmax-1) - ((m * mulmax) / d);
					}
				}
				else
				{
					// Just convert brightness to a skincolor value, use distance to next position to find the gradient multipler
					firsti = 0;

					for (i = 1; i < translen; i++)
					{
						if (brightness >= cutoff[i])
							break;
						firsti = i;
					}

					secondi = firsti+1;

					mulmax = cutoff[firsti];
					if (secondi < translen)
						mulmax -= cutoff[secondi];

					mul = cutoff[firsti] - brightness;
				}

				if (!ignore)
				{
					blendcolor = V_GetColor(translation[firsti]);

					if (secondi >= translen)
						mul = 0;

					if (mul > 0) // If it's 0, then we only need the first color.
					{
						nextcolor = V_GetColor(translation[secondi]);

						// Find difference between points
						r = (INT32)(nextcolor.s.red - blendcolor.s.red);
						g = (INT32)(nextcolor.s.green - blendcolor.s.green);
						b = (INT32)(nextcolor.s.blue - blendcolor.s.blue);

						// Find the gradient of the two points
						r = ((mul * r) / mulmax);
						g = ((mul * g) / mulmax);
						b = ((mul * b) / mulmax);

						// Add gradient value to color
						blendcolor.s.red += r;
						blendcolor.s.green += g;
						blendcolor.s.blue += b;
					}

					if (skinnum == TC_RAINBOW)
					{
						UINT32 tempcolor;
						UINT16 colorbright;

						SETBRIGHTNESS(colorbright,blendcolor.s.red,blendcolor.s.green,blendcolor.s.blue);
						if (colorbright == 0)
							colorbright = 1; // no dividing by 0 please

						tempcolor = (brightness * blendcolor.s.red) / colorbright;
						tempcolor = min(255, tempcolor);
						cur.s.red = (UINT8)tempcolor;

						tempcolor = (brightness * blendcolor.s.green) / colorbright;
						tempcolor = min(255, tempcolor);
						cur.s.green = (UINT8)tempcolor;

						tempcolor = (brightness * blendcolor.s.blue) / colorbright;
						tempcolor = min(255, tempcolor);
						cur.s.blue = (UINT8)tempcolor;
					}
					else
					{
						// Color strength depends on image alpha
						INT32 tempcolor;

						tempcolor = ((image.s.red * (255-ba)) / 255) + ((blendcolor.s.red * ba) / 255);
						tempcolor = min(255, tempcolor);
						cur.s.red = (UINT8)tempcolor;

						tempcolor = ((image.s.green * (255-ba)) / 255) + ((blendcolor.s.green * ba) / 255);
						tempcolor = min(255, tempcolor);
						cur.s.green = (UINT8)tempcolor;

						tempcolor = ((image.s.blue * (255-ba)) / 255) + ((blendcolor.s.blue * ba) / 255);
						tempcolor = min(255, tempcolor);
						cur.s.blue = (UINT8)tempcolor;
					}
				}
			}

			if (skip)
				cur = image;

			// *Now* we can do Metal Sonic's flashing
			if (skinnum == TC_METALSONIC)
			{
				// Blend dark blue into white
				if (cur.s.alpha > 0 && cur.s.red == 0 && cur.s.green == 0 && cur.s.blue < 255 && cur.s.blue > 31)
				{
					// Sal: Invert non-blue
					cur.s.red = cur.s.green = (255 - cur.s.blue);
					cur.s.blue = 255;
				}
			}
			else if (skinnum == TC_BOSS)
			{
				// Turn everything below a certain threshold white
				if ((image.s.red == image.s.green) && (image.s.green == image.s.blue) && image.s.blue < 127)
				{
					// Lactozilla: Invert the colors
					cur.s.red = cur.s.green = cur.s.blue = (255 - image.s.blue);
				}
			}
		}

		if (cur.s.red == image.s.red && cur.s.green == image.s.green && cur.s.blue == image.s.blue)
			out[n] = idx;
		else
			out[n] = PS2Models_QuantizeOne(cur.s.red, cur.s.green, cur.s.blue);
	}
	grMipmap->flags = holes ? TF_CHROMAKEYED : 0;
	return true;
}

#undef SETBRIGHTNESS

static void HWR_GetBlendedTexture(md2_t *md2, patch_t *patch, INT32 skinnum, const UINT8 *colormap, skincolornum_t color)
{
	GLPatch_t *grPatch = patch->hardware;
	GLMipmap_t *grMipmap, *newMipmap;
	ps2_blend_t bl;
	boolean haveblend = false, ok;
	char rel[48];

	if (colormap == colormaps || colormap == NULL)
	{
		// Don't do any blending
		HWD.pfnSetTexture(grPatch->mipmap);
		return;
	}
	strlcpy(rel, md2->filename, sizeof rel);
	FIL_ForceExtension(rel, "_blend.png");
	if (!grPatch->mipmap->data) // purged: the base indices again (the GS may still hold the picture: the texture stays downloaded)
	{
		char brel[48];
		const UINT32 dl = grPatch->mipmap->downloaded;

		strlcpy(brel, md2->filename, sizeof brel);
		FIL_ForceExtension(brel, ".png");
		if (!PS2Models_LoadTexture(brel, grPatch->mipmap))
		{
			grPatch->mipmap->downloaded = dl;
			HWD.pfnSetTexture(grPatch->mipmap);
			return;
		}
		grPatch->mipmap->downloaded = dl;
	}

	// search for the mipmap
	// skip the first (no colormap translated)
	for (grMipmap = grPatch->mipmap; grMipmap->nextcolormap; )
	{
		grMipmap = grMipmap->nextcolormap;
		if (grMipmap->colormap && grMipmap->colormap->source == colormap)
		{
			if (grMipmap->downloaded && grMipmap->data && !memcmp(grMipmap->colormap->data, colormap, 256 * sizeof(UINT8)))
			{
				HWD.pfnSetTexture(grMipmap); // found the colormap, set it to the correct texture
				return;
			}
			// (the blended picture was purged, or the colormap was changed under the same pointer)
			if (!md2->noblendfile)
				haveblend = PS2Models_LoadBlend(rel, &bl);
			md2->noblendfile = !haveblend && !PS2Models_HasBlend(rel);
			M_Memcpy(grMipmap->colormap->data, colormap, 256 * sizeof(UINT8));
			ok = HWR_CreateBlendedTexture(grPatch->mipmap, haveblend ? &bl : NULL, grMipmap, skinnum, color);
			if (haveblend)
				PS2Models_FreeBlend(&bl);
			if (!ok)
			{
				HWD.pfnSetTexture(grPatch->mipmap);
				return;
			}
			HWD.pfnUpdateTexture(grMipmap);
			return;
		}
	}

	// If here, the blended texture has not been created
	// So we create it
	newMipmap = calloc(1, sizeof (*newMipmap));
	if (newMipmap == NULL) // no room for it: the plain texture
	{
		HWD.pfnSetTexture(grPatch->mipmap);
		return;
	}
	newMipmap->colormap = Z_Calloc(sizeof(*newMipmap->colormap), PU_HWRPATCHCOLMIPMAP, NULL);
	newMipmap->colormap->source = colormap;
	M_Memcpy(newMipmap->colormap->data, colormap, 256 * sizeof(UINT8));
	if (!md2->noblendfile)
		haveblend = PS2Models_LoadBlend(rel, &bl);
	md2->noblendfile = !haveblend && !PS2Models_HasBlend(rel);
	ok = HWR_CreateBlendedTexture(grPatch->mipmap, haveblend ? &bl : NULL, newMipmap, skinnum, color);
	if (haveblend)
		PS2Models_FreeBlend(&bl);
	if (!ok)
	{
		Z_Free(newMipmap->colormap);
		free(newMipmap);
		HWD.pfnSetTexture(grPatch->mipmap);
		return;
	}
	grMipmap->nextcolormap = newMipmap; // chained only now: a failed one is not left in the list
	HWD.pfnSetTexture(newMipmap);
}
