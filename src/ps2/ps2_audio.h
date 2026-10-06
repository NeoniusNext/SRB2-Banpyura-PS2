// Bounded, platform-independent PCM mixer and random-access audio input.
#ifndef PS2_AUDIO_H
#define PS2_AUDIO_H
#include <stddef.h>
#include <stdint.h>

#define PS2_AUDIO_RATE 22050
#define PS2_AUDIO_CHANNELS 64
#define PS2_AUDIO_BLOCK 512

typedef struct
{
	void *user;
	size_t size;
	size_t (*read_at)(void *user, size_t offset, void *dst, size_t bytes);
} ps2_audio_input;

typedef struct
{
	size_t offset;
	uint32_t frames, rate;
	unsigned bits, channels;
} ps2_pcm;

typedef struct
{
	ps2_pcm pcm;
	const uint8_t *bytes;
} ps2_sample;

typedef struct
{
	const ps2_sample *sample;
	uint32_t frame, fraction, step, generation;
	unsigned volume, pan;
} ps2_voice;

typedef struct
{
	ps2_voice voices[PS2_AUDIO_CHANNELS];
	unsigned volume;
} ps2_mixer;

size_t PS2_AudioRead(const ps2_audio_input *in, size_t off, void *dst, size_t n);
int PS2_ParsePCM(const ps2_audio_input *in, ps2_pcm *pcm, int allow_dmx);
int16_t PS2_PCMValue(const uint8_t *p, unsigned bits);
void PS2_MixerInit(ps2_mixer *m);
int PS2_MixerStart(ps2_mixer *m, const ps2_sample *s, int channel,
	unsigned volume, unsigned pan, unsigned pitch);
int PS2_MixerPlaying(const ps2_mixer *m, int handle);
void PS2_MixerStop(ps2_mixer *m, int handle);
void PS2_MixerParams(ps2_mixer *m, int handle, unsigned volume, unsigned pan, unsigned pitch);
void PS2_MixerForget(ps2_mixer *m, const ps2_sample *s);
// music may be NULL or equal to out. Gains are 0..32768. Output is interleaved signed stereo.
void PS2_MixerRender(ps2_mixer *m, int16_t *out, const int16_t *music,
	size_t frames, unsigned music_gain);
#endif
