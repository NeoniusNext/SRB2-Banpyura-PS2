// Host decode of an Ogg Vorbis file the way ps2_music.c does (half rate for 44.1/48 kHz, ov_read_float in blocks of 1024, PS2_FloatToS16_ref),
// printing the FNV-1a hash of the s16 stereo PCM: the same file decoded by stock libvorbis, by the vendored units (src/ps2/vorbis) and by the
// vendored units with the C twins of the VU0 kernels must give the same hash (tools/ps2/vorbis_host_check.py builds and compares them).
//   usage: vorbis_host file.ogg [seconds] [-dump out.raw]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vorbis/vorbisfile.h>
#ifdef TRUNC
#include <fenv.h>
#endif

static int16_t conv(float f)
{
	float p = f * 32768.0f;
	int t;
	float frac;
	if (p > 32768.0f) p = 32768.0f; else if (p < -32769.0f) p = -32769.0f;
	t = (int)p;
	frac = p - (float)t;
	t += (frac >= 0.5f) - (frac < -0.5f);
	return (int16_t)(t > 32767 ? 32767 : t < -32768 ? -32768 : t);
}

int main(int argc, char **argv)
{
	OggVorbis_File vf;
	vorbis_info *info;
	uint64_t h = 14695981039346656037ull;
	long frames = 0, target;
	int sec = argc > 2 ? atoi(argv[2]) : 12, half = 0, section = 0;
	const char *dump = argc > 4 && !strcmp(argv[3], "-dump") ? argv[4] : NULL;
	FILE *df = NULL;
	if (argc < 2 || ov_fopen(argv[1], &vf)) { fprintf(stderr, "cannot open\n"); return 2; }
	info = ov_info(&vf, 0);
	if (info->rate >= 44000 && !(info->rate & 1) && ov_halfrate(&vf, 1) == 0) half = 1;
	ov_pcm_seek(&vf, 0);
	target = (long)sec * 22050;
#ifdef TRUNC
	fesetround(FE_TOWARDZERO);   // after the set-up (the tables are built by libm with the default rounding); the decode loops run toward zero like VU0
#endif
	if (dump) df = fopen(dump, "wb");
	while (frames < target)
	{
		float **pcm;
		long got = ov_read_float(&vf, &pcm, 1024, &section), i;
		if (got <= 0) break;
		for (i = 0; i < got; i++)
		{
			int16_t s[2];
			size_t k;
			s[0] = conv(pcm[0][i]); s[1] = conv(info->channels == 2 ? pcm[1][i] : pcm[0][i]);
			for (k = 0; k < 4; k++) h = (h ^ ((const uint8_t *)s)[k]) * 1099511628211ull;
			if (df) fwrite(s, 4, 1, df);
		}
		frames += got;
	}
	if (df) fclose(df);
	printf("%s rate=%ld ch=%d half=%d frames=%ld fnv=%016llx\n", argv[1], info->rate, info->channels, half, frames, (unsigned long long)h);
	ov_clear(&vf);
	return 0;
}
