// Incremental WAV/Vorbis/MP3 decoding; source may be memory or a pack lump.
#include "ps2_music.h"
#include "ps2_midi.h"
#include "ps2_pcmconv.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <malloc.h>
#ifdef PS2_AUDIO_VORBIS
#define OV_EXCLUDE_STATIC_CALLBACKS
#include <vorbis/vorbisfile.h>
#endif
#ifdef PS2_AUDIO_MP3
#include <mpg123.h>
#endif

#define DECODE_FRAMES 1024
struct ps2_music
{
	ps2_audio_input in;
	ps2_music_type type;
	ps2_pcm pcm;
	ps2_midi *midi;
#ifdef PS2_AUDIO_VORBIS
	OggVorbis_File ogg;
	int ogg_open, ogg_half;
#endif
#ifdef PS2_AUDIO_MP3
	mpg123_handle *mp3;
#endif
	size_t io_pos;
	uint64_t decoded, position;
	uint32_t rate, length_ms, loop_ms, fraction, step;
	unsigned channels, playing, paused, looping, error;
	size_t buffered, cursor;
	unsigned skipped;                  // PS2-317: the position moved without decoding; the decoder is behind (resynchronised by PS2_MusicRender)
	int16_t buffer[DECODE_FRAMES * 2] __attribute__((aligned(16))); // PS2-316: the VU0 PCM conversion stores 16 bytes at a time
	uint8_t raw[DECODE_FRAMES * 4];
};

#if defined(PS2_AUDIO_VORBIS) || defined(PS2_AUDIO_MP3)
static int SeekIO(ps2_music *m, int64_t off, int whence)
{
	int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)m->io_pos : (int64_t)m->in.size;
	if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) return -1;
	if (off < -base || off > (int64_t)m->in.size - base) return -1;
	m->io_pos = (size_t)(base + off); return 0;
}
#endif
#ifdef PS2_AUDIO_VORBIS
static size_t OggRead(void *dst, size_t size, size_t count, void *user)
{
	ps2_music *m = user;
	size_t n;
	if (!size || count > SIZE_MAX / size) return 0;
	n = PS2_AudioRead(&m->in, m->io_pos, dst, size * count);
	m->io_pos += n; return n / size;
}
static int OggSeek(void *user, ogg_int64_t off, int whence) { return SeekIO(user, off, whence); }
static long OggTell(void *user) { return (long)((ps2_music *)user)->io_pos; }
#endif
#ifdef PS2_AUDIO_MP3
static mpg123_ssize_t MP3Read(void *user, void *dst, size_t n)
{
	ps2_music *m = user;
	n = PS2_AudioRead(&m->in, m->io_pos, dst, n); m->io_pos += n; return (mpg123_ssize_t)n;
}
static off_t MP3Seek(void *user, off_t off, int whence)
{ return SeekIO(user, off, whence) ? (off_t)-1 : (off_t)((ps2_music *)user)->io_pos; }
#endif

ps2_music *PS2_MusicOpen(const ps2_audio_input *in)
{
	uint8_t h[12];
	ps2_music *m;
	if (!in || in->size > INT_MAX || PS2_AudioRead(in, 0, h, 12) != 12) return NULL;
	m = memalign(16, sizeof *m); if (!m) return NULL;
	memset(m, 0, sizeof *m);
	m->in = *in;
	if (!memcmp(h, "RIFF", 4))
	{
		if (!PS2_ParsePCM(in, &m->pcm, 0)) goto bad;
		m->type = PS2_MUSIC_WAV; m->rate = m->pcm.rate; m->channels = m->pcm.channels;
		m->length_ms = (uint32_t)((uint64_t)m->pcm.frames * 1000 / m->rate);
	}
	else if (!memcmp(h, "MThd", 4))
	{
		m->midi = PS2_MIDIOpen(in); if (!m->midi) goto bad;
		m->type = PS2_MUSIC_MIDI; m->rate = PS2_AUDIO_RATE; m->channels = 2;
		m->length_ms = PS2_MIDILength(m->midi);
	}
#ifdef PS2_AUDIO_VORBIS
	else if (!memcmp(h, "OggS", 4))
	{
		ov_callbacks cb = { OggRead, OggSeek, NULL, OggTell };
		vorbis_info *info;
		double duration;
		if (ov_open_callbacks(m, &m->ogg, NULL, 0, cb)) goto bad;
		m->ogg_open = 1;
		info = ov_info(&m->ogg, 0);
		if (ov_streams(&m->ogg) != 1 || !info || info->channels < 1 || info->channels > 2
			|| info->rate < 4000 || info->rate > 96000) goto bad;
		m->type = PS2_MUSIC_OGG; m->rate = info->rate; m->channels = info->channels;
		// Vorbis' native half-rate synthesis cuts transform/output work and
		// applies its low-pass path, instead of generating unused 44/48k PCM.
		if (m->rate >= 44000 && (m->rate & 1) == 0 && ov_halfrate(&m->ogg, 1) == 0)
		{ m->ogg_half = 1; m->rate /= 2; }
		duration = ov_time_total(&m->ogg, -1);
		if (duration <= 0 || duration > 86400) goto bad;
		m->length_ms = (uint32_t)(duration * 1000);
		// Native Vorbis comments, without a byte-by-byte scan of the track.
		{
			vorbis_comment *vc = ov_comment(&m->ogg, 0);
			const char *tag = vc ? vorbis_comment_query(vc, "LOOPMS", 0) : NULL;
			int samples = 0;
			char *end;
			unsigned long long value;
			if (!tag && vc) { tag = vorbis_comment_query(vc, "LOOPPOINT", 0); samples = 1; }
			if (tag && *tag >= '0' && *tag <= '9')
			{
				value = strtoull(tag, &end, 10);
				if (!*end && value <= 86400ull * info->rate)
				{
					if (samples) value = value * 1000 / info->rate;
					if (value < m->length_ms) m->loop_ms = (uint32_t)value;
				}
			}
		}
	}
#endif
#ifdef PS2_AUDIO_MP3
	else if (!memcmp(h, "ID3", 3) || (h[0] == 255 && (h[1] & 224) == 224))
	{
		static int initialized;
		int err, channels, encoding;
		long rate;
		off_t length;
		if (!initialized) { if (mpg123_init() != MPG123_OK) goto bad; initialized = 1; }
		m->mp3 = mpg123_new(NULL, &err); if (!m->mp3) goto bad;
		if (mpg123_param(m->mp3, MPG123_FORCE_RATE, PS2_AUDIO_RATE, 0) != MPG123_OK
			|| mpg123_format_none(m->mp3) != MPG123_OK
			|| mpg123_format(m->mp3, PS2_AUDIO_RATE, MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16) != MPG123_OK
			|| mpg123_replace_reader_handle(m->mp3, MP3Read, MP3Seek, NULL) != MPG123_OK
			|| mpg123_open_handle(m->mp3, m) != MPG123_OK
			|| mpg123_getformat(m->mp3, &rate, &channels, &encoding) != MPG123_OK
			|| rate != PS2_AUDIO_RATE || channels < 1 || channels > 2 || encoding != MPG123_ENC_SIGNED_16) goto bad;
		m->type = PS2_MUSIC_MP3; m->rate = (uint32_t)rate; m->channels = channels;
		length = mpg123_length(m->mp3); // Xing/Info length when present; no full-track scan
		if (length > 0 && (uint64_t)length * 1000 / m->rate <= UINT32_MAX)
			m->length_ms = (uint32_t)((uint64_t)length * 1000 / m->rate);
	}
#endif
	else goto bad;
	m->step = (uint32_t)((uint64_t)m->rate * 65536 / PS2_AUDIO_RATE);
	return m;
bad:
	PS2_MusicClose(m); return NULL;
}

void PS2_MusicClose(ps2_music *m)
{
	if (!m) return;
	PS2_MIDIClose(m->midi);
#ifdef PS2_AUDIO_VORBIS
	if (m->ogg_open) ov_clear(&m->ogg);
#endif
#ifdef PS2_AUDIO_MP3
	if (m->mp3) mpg123_delete(m->mp3);
#endif
	free(m);
}
ps2_music_type PS2_MusicType(const ps2_music *m) { return m ? m->type : PS2_MUSIC_NONE; }
int PS2_MusicPlaying(const ps2_music *m) { return m && m->playing; }
int PS2_MusicPaused(const ps2_music *m) { return m && m->paused; }
int PS2_MusicError(const ps2_music *m) { return m && m->error; }
uint32_t PS2_MusicLength(const ps2_music *m) { return m ? m->length_ms : 0; }
unsigned PS2_MusicChannels(const ps2_music *m) { return m ? m->channels : 0; }
uint32_t PS2_MusicPosition(const ps2_music *m) { return m ? (uint32_t)(m->position * 1000 / m->rate) : 0; }
uint32_t PS2_MusicLoop(const ps2_music *m) { return m ? m->loop_ms : 0; }
void PS2_MusicPause(ps2_music *m, int paused) { if (m && m->playing) m->paused = !!paused; }
void PS2_MusicStop(ps2_music *m) { if (m) { m->playing = m->paused = 0; PS2_MusicSeek(m, 0); } }
int PS2_MusicSetLoop(ps2_music *m, uint32_t ms)
{
	if (!m || (m->length_ms && ms >= m->length_ms)) return 0;
	m->loop_ms = ms; return 1;
}
int PS2_MusicSpeed(ps2_music *m, float speed)
{
	if (!m || !(speed >= 0.25f && speed <= 4.0f)) return 0;
	m->step = (uint32_t)((float)m->rate * (65536.0f / PS2_AUDIO_RATE) * speed); return 1;
}

int PS2_MusicSeek(ps2_music *m, uint32_t ms)
{
	uint64_t frame;
	if (!m || (m->length_ms && ms > m->length_ms)) return 0;
	frame = (uint64_t)ms * m->rate / 1000;
	if (m->type == PS2_MUSIC_WAV)
	{ if (frame > m->pcm.frames) return 0; }
	else if (m->type == PS2_MUSIC_MIDI)
	{ if (!PS2_MIDISeek(m->midi, ms)) return 0; }
#ifdef PS2_AUDIO_VORBIS
	else if (m->type == PS2_MUSIC_OGG)
	{ if (ov_pcm_seek(&m->ogg, (ogg_int64_t)frame << m->ogg_half)) return 0; }
#endif
#ifdef PS2_AUDIO_MP3
	else if (m->type == PS2_MUSIC_MP3)
	{
		off_t result = mpg123_seek(m->mp3, (off_t)frame, SEEK_SET);
		if (result < 0) return 0;
		frame = (uint64_t)result;
	}
#endif
	else return 0;
	m->position = m->decoded = frame;
	m->buffered = m->cursor = m->fraction = m->error = m->skipped = 0; return 1;
}
int PS2_MusicPlay(ps2_music *m, int loop)
{
	if (!m || !PS2_MusicSeek(m, 0)) return 0;
	m->playing = 1; m->paused = 0; m->looping = !!loop; return 1;
}

void PS2_MusicRevive(ps2_music *m) { if (m) { m->playing = 1; m->paused = 0; } }

static size_t Decode(ps2_music *m)
{
	size_t n = 0, i, stride = m->channels * 2;
	if (m->type == PS2_MUSIC_WAV)
	{
		size_t bytes;
		stride = m->pcm.channels * (m->pcm.bits/8);
		n = (size_t)(m->pcm.frames - m->decoded);
		if (n > DECODE_FRAMES) n = DECODE_FRAMES;
		bytes = n * stride;
		if (!n) return 0;
		if (PS2_AudioRead(&m->in, m->pcm.offset + (size_t)m->decoded * stride, m->raw, bytes) != bytes)
		{ m->error = 1; return 0; }
		for (i = 0; i < n; i++)
		{
			const uint8_t *p = m->raw + i*stride;
			m->buffer[i*2] = PS2_PCMValue(p, m->pcm.bits);
			m->buffer[i*2+1] = m->channels == 2 ? PS2_PCMValue(p+m->pcm.bits/8, m->pcm.bits) : m->buffer[i*2];
		}
	}
	else if (m->type == PS2_MUSIC_MIDI)
	{
		n = PS2_MIDIRead(m->midi, m->buffer, DECODE_FRAMES);
		if (PS2_MIDIError(m->midi)) { m->error = 1; return 0; }
	}
#ifdef PS2_AUDIO_VORBIS
	else if (m->type == PS2_MUSIC_OGG)
	{
		// ov_read_float + PS2_FloatToS16 gives the same samples as ov_read() without its per-sample double arithmetic
		float **pcm;
		int section = 0;
		long got = ov_read_float(&m->ogg, &pcm, DECODE_FRAMES, &section);
		if (got < 0 || section != 0) { m->error = 1; return 0; }
		n = (size_t)got;
		PS2_FloatsToS16Stereo(m->buffer, pcm[0], m->channels == 2 ? pcm[1] : pcm[0], n);
	}
#endif
#ifdef PS2_AUDIO_MP3
	else if (m->type == PS2_MUSIC_MP3)
	{
		size_t bytes = 0;
		int result = mpg123_read(m->mp3, m->raw, DECODE_FRAMES * stride, &bytes);
		if (result == MPG123_NEW_FORMAT)
		{
			long rate; int ch, enc;
			if (mpg123_getformat(m->mp3, &rate, &ch, &enc) != MPG123_OK || rate != (long)m->rate
				|| ch != (int)m->channels || enc != MPG123_ENC_SIGNED_16) { m->error = 1; return 0; }
			result = mpg123_read(m->mp3, m->raw, DECODE_FRAMES * stride, &bytes);
		}
		if (result != MPG123_OK && result != MPG123_DONE) { m->error = 1; return 0; }
		n = bytes / stride;
	}
#endif
	if (m->type == PS2_MUSIC_MP3)
	{
		for (i = 0; i < n; i++)
		{
			m->buffer[i*2] = PS2_PCMValue(m->raw+i*stride, 16);
			m->buffer[i*2+1] = m->channels == 2 ? PS2_PCMValue(m->raw+i*stride+2, 16) : m->buffer[i*2];
		}
	}
	m->decoded += n; m->cursor = 0; m->buffered = n; return n;
}

static int Ensure(ps2_music *m)
{
	if (m->cursor < m->buffered) return 1;
	if (Decode(m)) return 1;
	if (m->looping && !m->error && PS2_MusicSeek(m, m->loop_ms) && Decode(m)) return 1;
	m->playing = m->paused = 0; return 0;
}

int PS2_MusicCanSkip(const ps2_music *m)
{
	return m && m->playing && !m->paused && !m->error && (m->type == PS2_MUSIC_OGG || m->type == PS2_MUSIC_MP3)
		&& m->length_ms && m->step == 65536 && !m->fraction;
}

size_t PS2_MusicSkip(ps2_music *m, size_t frames)
{
	uint64_t end;
	if (!PS2_MusicCanSkip(m)) return 0;
	end = (uint64_t)m->length_ms * m->rate / 1000;
	m->skipped = 1;
	m->buffered = m->cursor = 0;
	m->position += frames;
	if (m->position >= end)
	{
		if (m->looping)
		{
			uint64_t loop = (uint64_t)m->loop_ms * m->rate / 1000, span = end > loop ? end - loop : 0;
			m->position = span ? loop + (m->position - end) % span : loop;
		}
		else
		{
			frames -= (size_t)(m->position - end);
			m->position = end; m->playing = m->paused = 0;
		}
	}
	m->decoded = m->position;
	return frames;
}

size_t PS2_MusicRender(ps2_music *m, int16_t *out, size_t frames)
{
	size_t f;
	memset(out, 0, frames * 2 * sizeof *out);
	if (!m || !m->playing || m->paused) return 0;
	if (m->skipped)
	{
		// the position was moved by PS2_MusicSkip: bring the decoder there (a sample exact seek)
		uint64_t pos = m->position;
		m->skipped = 0;
		if (!PS2_MusicSeek(m, (uint32_t)(pos * 1000 / m->rate))) { m->error = 1; return 0; }
	}
	if (m->step == 65536 && !m->fraction)
	{
		// source rate == output rate at normal speed: one source frame per output frame, straight copies
		for (f = 0; f < frames;)
		{
			size_t n;
			if (!Ensure(m)) break;
			n = m->buffered - m->cursor;
			if (n > frames - f) n = frames - f;
			memcpy(out + f*2, m->buffer + m->cursor*2, n * 2 * sizeof *out);
			m->cursor += n; m->position += n; f += n;
		}
		return f;
	}
	for (f = 0; f < frames; f++)
	{
		unsigned advance;
		if (!Ensure(m)) break;
		out[f*2] = m->buffer[m->cursor*2]; out[f*2+1] = m->buffer[m->cursor*2+1];
		m->fraction += m->step; advance = m->fraction >> 16; m->fraction &= 65535;
		while (advance--)
		{
			m->cursor++; m->position++;
			if (advance && !Ensure(m)) break;
		}
		if (!m->playing) { f++; break; }
	}
	return f;
}
