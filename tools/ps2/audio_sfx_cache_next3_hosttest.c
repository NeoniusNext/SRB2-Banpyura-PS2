/* Use the existing backend boundary with real I_GetSfx and mixer functions. */
#include <stdint.h>
#include <stddef.h>
static uint64_t probe_reads, probe_read_hash;
static size_t probe_peak;
#define main BackendRegressionMain
#include "audio_backend_probe.c"
#undef main

static uint64_t Hash(uint64_t h, const void *data, size_t n)
{
	const uint8_t *p = data;
	while (n--) h = (h ^ *p++) * UINT64_C(1099511628211);
	return h;
}
static size_t MemoryRead(void *user, size_t off, void *dst, size_t n)
{
	memcpy(dst, (const uint8_t *)user + off, n); return n;
}
int main(int argc, char **argv)
{
	static const unsigned pans[] = {0, 64, 128, 255}, pitches[] = {64, 93, 128, 255};
	size_t total_before = 0, total_after = 0, files = 0;
	int arg, negative = 0, read_negative = 0;
	CHECK(!BackendRegressionMain(1, argv));
	fail_transfer = 0; zone_limit = 0;
	I_StartupSound(); CHECK(sound_started && !allocated);
	for (arg = 1; arg < argc; arg++)
	{
		FILE *fp;
		uint8_t *data;
		size_t n, payload, retained, c;
		ps2_pcm descriptor;
		ps2_audio_input in;
		cached_sample *s;
		uint64_t digest = UINT64_C(1469598103934665603), expected_reads;
		if (!strcmp(argv[arg], "--negative-control")) { negative = 1; continue; }
		if (!strcmp(argv[arg], "--read-negative-control")) { read_negative = 1; continue; }
		fp = fopen(argv[arg], "rb"); CHECK(fp && !fseek(fp, 0, SEEK_END));
		n = (size_t)ftell(fp); CHECK(n && n <= SFX_MAX); rewind(fp);
		data = malloc(n); CHECK(data && fread(data, 1, n, fp) == n); fclose(fp);
		in.user = data; in.size = n; in.read_at = MemoryRead;
		CHECK(PS2_ParsePCM(&in, &descriptor, 1));
		payload = (size_t)descriptor.frames * descriptor.channels * (descriptor.bits / 8);
		lumpdata[6] = data; lengths[6] = n;
		S_sfx[6].name = argv[arg]; S_sfx[6].lumpnum = 6;
		fail_alloc = 1; CHECK(!I_GetSfx(&S_sfx[6]) && !allocated); fail_alloc = 0;
		probe_peak = 0; probe_reads = 0; probe_read_hash = UINT64_C(1469598103934665603);
		s = I_GetSfx(&S_sfx[6]); CHECK(s && S_sfx[6].data == s && S_sfx[6].length == n);
		CHECK(I_GetSfx(&S_sfx[6]) == s);
		if (negative) { s->data[s->sample.pcm.offset] ^= 1; negative = 0; }
		if (read_negative) { probe_read_hash ^= 1; read_negative = 0; }
		CHECK(s->sample.pcm.frames == descriptor.frames && s->sample.pcm.rate == descriptor.rate
			&& s->sample.pcm.bits == descriptor.bits && s->sample.pcm.channels == descriptor.channels);
		CHECK(!memcmp(data + descriptor.offset, s->sample.bytes + s->sample.pcm.offset, payload));
#ifdef SFX_CACHE_COMPACT
		retained = payload + (descriptor.offset & 15);
		CHECK(s->sample.pcm.offset == (descriptor.offset & 15));
#else
		retained = n;
		CHECK(s->sample.pcm.offset == descriptor.offset);
#endif
		CHECK(s->sample.bytes == s->data && s->bytes == retained && sample_bytes == retained);
		CHECK(allocated == sizeof *s + retained && probe_peak == sizeof *s + n);
		/* The initial pointer is 64-byte aligned in the real zone. Keeping the
		 * old offset modulo 16 preserves all 2/4/8/16-byte source alignments. */
		CHECK((s->sample.pcm.offset & 15) == (descriptor.offset & 15));
		expected_reads = ((UINT64_C(1469598103934665603) ^ 6) * UINT64_C(1099511628211)
			^ 0) * UINT64_C(1099511628211);
		expected_reads = (expected_reads ^ n) * UINT64_C(1099511628211);
		CHECK(probe_reads == 1 && probe_read_hash == expected_reads);
		digest = Hash(digest, data + descriptor.offset, payload);
		for (c = 0; c < 4; c++)
		{
			ps2_mixer mix;
			int16_t out[1024];
			unsigned round, v;
			PS2_MixerInit(&mix);
			for (v = 0; v < 4; v++) CHECK(PS2_MixerStart(&mix, &s->sample, (int)v,
				(unsigned)(255 - v * 63), pans[(c + v) & 3], pitches[c]) >= 0);
			for (round = 0; round < 3; round++)
			{
				PS2_MixerRender(&mix, out, NULL, 512, 32768);
				digest = Hash(digest, out, sizeof out);
				for (v = 0; v < 4; v++)
				{
					uint32_t state[] = {mix.voices[v].sample != NULL, mix.voices[v].frame,
						mix.voices[v].fraction, mix.voices[v].step};
					digest = Hash(digest, state, sizeof state);
				}
			}
		}
		{
			int h = I_StartSound(6, 255, 128, 128, 0, 0);
			CHECK(h >= 0 && I_SoundIsPlaying(h));
			I_FreeSfx(&S_sfx[6]); CHECK(!S_sfx[6].data && !I_SoundIsPlaying(h) && !allocated && !sample_bytes);
		}
		printf("CACHE3 file=%s original=%zu retained=%zu payload=%zu digest=%016llx reads=%llu readhash=%016llx peak=%zu\n",
			argv[arg], n, retained, payload, (unsigned long long)digest, (unsigned long long)probe_reads,
			(unsigned long long)probe_read_hash, probe_peak);
		total_before += n; total_after += retained; files++; free(data);
	}
	I_ShutdownSound(); CHECK(!allocated);
	printf("CACHE3 DONE files=%zu original=%zu retained=%zu saved=%zu\n", files, total_before, total_after, total_before - total_after);
	return 0;
}
