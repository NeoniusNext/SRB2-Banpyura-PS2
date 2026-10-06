// EE check of PS2-41: ov_read() (libvorbisfile converts float -> int16 in double) against ov_read_float() + PS2_FloatToS16,
// on a real Ogg file, in the emulator. Prints OGGCONV lines: sample mismatches (must be 0) and COP0 cycles per output frame.
// usage (PCSX2, gameargs): host:<file.ogg> [seconds]   see tools/ps2/build_ogg_test.py / run via tools/ps2/run_pcsx2.py
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define OV_EXCLUDE_STATIC_CALLBACKS
#include <vorbis/vorbisfile.h>
#include "../../src/ps2/ps2_pcmconv.h"

static unsigned char *data;
static size_t size, pos;

static size_t Rd(void *dst, size_t sz, size_t cnt, void *u)
{
	size_t n = sz * cnt;
	(void)u;
	if (n > size - pos) n = size - pos;
	memcpy(dst, data + pos, n);
	pos += n;
	return sz ? n / sz : 0;
}
static int Sk(void *u, ogg_int64_t off, int whence)
{
	ogg_int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (ogg_int64_t)pos : (ogg_int64_t)size;
	(void)u;
	if (off < -base || off > (ogg_int64_t)size - base) return -1;
	pos = (size_t)(base + off);
	return 0;
}
static long Tl(void *u) { (void)u; return (long)pos; }

static inline unsigned cop0(void) { unsigned v; __asm__ volatile("mfc0 %0,$9" : "=r"(v)); return v; }

int main(int argc, char **argv)
{
	FILE *f;
	OggVorbis_File vf;
	ov_callbacks cb = { Rd, Sk, NULL, Tl };
	vorbis_info *info;
	size_t frames_wanted, a_frames = 0, b_frames = 0, i, mismatches = 0;
	int16_t *A, *B;
	unsigned t0, ta, tb, tc;
	float **pcm;
	int section = 0, ch;
	int seconds = 4, k;
	const char *path = NULL;

	setvbuf(stdout, NULL, _IONBF, 0);
	for (k = 0; k < argc; k++) // PCSX2 -gameargs: argv[0] is already the first argument
	{
		if (strstr(argv[k], ".ogg")) path = argv[k];
		else if (atoi(argv[k]) > 0) seconds = atoi(argv[k]);
	}
	if (!path || !(f = fopen(path, "rb"))) { puts("OGGCONV FAIL open"); return 1; }
	fseek(f, 0, SEEK_END); size = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
	data = malloc(size);
	if (!data || fread(data, 1, size, f) != size) { puts("OGGCONV FAIL read"); return 1; }
	fclose(f);
	if (ov_open_callbacks(NULL, &vf, NULL, 0, cb)) { puts("OGGCONV FAIL ov_open"); return 1; }
	info = ov_info(&vf, 0);
	ch = info->channels;
	if (info->rate >= 44000 && !(info->rate & 1)) ov_halfrate(&vf, 1);
	frames_wanted = (size_t)seconds * 22050;
	A = malloc(frames_wanted * 2 * sizeof *A + 8192);
	B = malloc(frames_wanted * 2 * sizeof *B + 8192);
	if (!A || !B) { puts("OGGCONV FAIL memory"); return 1; }
	printf("OGGCONV file=%s bytes=%u channels=%d rate=%ld seconds=%d\n", argv[1], (unsigned)size, ch, info->rate, seconds);

	// pass A: the library's own conversion
	t0 = cop0();
	while (a_frames < frames_wanted)
	{
		static char raw[1024 * 4];
		long got = ov_read(&vf, raw, 1024 * ch * 2, 0, 2, 1, &section);
		long n;
		if (got <= 0) break;
		n = got / (ch * 2);
		for (i = 0; i < (size_t)n; i++)
		{
			A[(a_frames + i) * 2] = ((int16_t *)raw)[i * ch];
			A[(a_frames + i) * 2 + 1] = ((int16_t *)raw)[i * ch + (ch == 2)];
		}
		a_frames += (size_t)n;
	}
	ta = cop0() - t0;

	ov_pcm_seek(&vf, 0);
	// pass B: ov_read_float + PS2_FloatToS16
	t0 = cop0();
	while (b_frames < a_frames)
	{
		long got = ov_read_float(&vf, &pcm, 1024, &section);
		long i2;
		if (got <= 0) break;
		for (i2 = 0; i2 < got; i2++)
		{
			B[(b_frames + (size_t)i2) * 2] = PS2_FloatToS16(pcm[0][i2]);
			B[(b_frames + (size_t)i2) * 2 + 1] = PS2_FloatToS16(pcm[ch == 2][i2]);
		}
		b_frames += (size_t)got;
	}
	tb = cop0() - t0;

	ov_pcm_seek(&vf, 0);
	// pass C: decode only (ov_read_float without conversion)
	{
		size_t c_frames = 0;
		t0 = cop0();
		while (c_frames < a_frames)
		{
			long got = ov_read_float(&vf, &pcm, 1024, &section);
			if (got <= 0) break;
			c_frames += (size_t)got;
		}
		tc = cop0() - t0;
	}

	for (i = 0; i < (a_frames < b_frames ? a_frames : b_frames) * 2; i++)
		if (A[i] != B[i]) mismatches++;
	printf("OGGCONV frames A=%u B=%u mismatches=%u (must be 0, frames equal)\n", (unsigned)a_frames, (unsigned)b_frames, (unsigned)mismatches);
	printf("OGGCONV cycles/frame: ov_read=%u ov_read_float+conv=%u decode_only=%u\n",
		(unsigned)(a_frames ? ta / a_frames : 0), (unsigned)(b_frames ? tb / b_frames : 0), (unsigned)(a_frames ? tc / a_frames : 0));
	puts(mismatches == 0 && a_frames == b_frames && a_frames ? "OGGCONV PASS" : "OGGCONV FAIL");
	ov_clear(&vf);
	return mismatches == 0 && a_frames == b_frames ? 0 : 1;
}
