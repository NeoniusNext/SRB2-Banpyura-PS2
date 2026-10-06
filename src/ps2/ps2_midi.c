// Streaming SMF 0/1 sequencer with a small procedural instrument bank.
// No event list, SoundFont, track PCM, or allocation in the render path.
#include "ps2_midi.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TRACKS 32
#define NOTES 24
#define MAX_EVENTS 1000000u
#define MAX_US (86400ull * 1000000)

typedef struct
{
	size_t start, end, pos, cacheoff, cached;
	uint64_t tick;
	unsigned running, ended;
	uint8_t cache[256];
} midi_track;
typedef struct { unsigned program, volume, expression, pan, sustain, bend; } midi_channel;
typedef struct
{
	unsigned active, channel, key, velocity, released, held, env, age;
	uint32_t phase, step, noise;
} midi_note;
struct ps2_midi
{
	ps2_audio_input in;
	midi_track tracks[TRACKS];
	midi_channel channels[16];
	midi_note notes[NOTES];
	uint32_t steps[128];
	int16_t sine[256];
	unsigned count, division, tempo, failed, events;
	uint64_t tick, us, remainder, frame, length_frames;
};

static uint32_t BE(const uint8_t *p, unsigned n)
{ uint32_t v = 0; while (n--) v = v * 256 + *p++; return v; }

static int Byte(ps2_midi *m, midi_track *t)
{
	if (t->pos >= t->end) { m->failed = 1; return 0; }
	if (t->pos < t->cacheoff || t->pos - t->cacheoff >= t->cached)
	{
		size_t n = t->end - t->pos;
		if (n > sizeof t->cache) n = sizeof t->cache;
		t->cacheoff = t->pos;
		t->cached = PS2_AudioRead(&m->in, t->pos, t->cache, n);
		if (t->cached != n) { m->failed = 1; return 0; }
	}
	return t->cache[t->pos++ - t->cacheoff];
}

static uint32_t VLQ(ps2_midi *m, midi_track *t)
{
	uint32_t v = 0;
	unsigned i;
	for (i = 0; i < 4; i++) { int b = Byte(m, t); v = v * 128 + (b & 127); if (!(b & 128)) return v; }
	m->failed = 1; return 0;
}

static void Next(ps2_midi *m, midi_track *t)
{
	if (t->pos >= t->end) { m->failed = 1; return; } // EOT is required
	t->tick += VLQ(m, t);
}

static void Reset(ps2_midi *m)
{
	unsigned i;
	m->tick = m->us = m->remainder = m->frame = 0;
	m->tempo = 500000; m->failed = m->events = 0;
	memset(m->notes, 0, sizeof m->notes);
	for (i = 0; i < 16; i++)
	{
		midi_channel *c = &m->channels[i];
		c->program = c->sustain = 0; c->volume = 100; c->expression = 127; c->pan = 64; c->bend = 8192;
	}
	for (i = 0; i < m->count; i++)
	{
		midi_track *t = &m->tracks[i];
		t->pos = t->start; t->tick = 0; t->cached = t->running = t->ended = 0;
		Next(m, t);
	}
}

static uint32_t Step(ps2_midi *m, unsigned ch, unsigned key)
{
	float bend = ((int)m->channels[ch].bend - 8192) * (2.0f / (8192 * 12));
	return (uint32_t)(m->steps[key] * powf(2.0f, bend));
}

static void Message(ps2_midi *m, unsigned status, unsigned a, unsigned b)
{
	unsigned i, ch = status & 15, kind = status >> 4;
	midi_channel *c = &m->channels[ch];
	if (kind == 9 && b)
	{
		midi_note *n = NULL;
		for (i = 0; i < NOTES; i++) if (!m->notes[i].active) { n = &m->notes[i]; break; }
		if (!n) { n = &m->notes[0]; for (i = 1; i < NOTES; i++) if (m->notes[i].age > n->age) n = &m->notes[i]; }
		memset(n, 0, sizeof *n); n->active = 1; n->channel = ch; n->key = a; n->velocity = b;
		n->step = Step(m, ch, a); n->noise = 0x1234567u + a;
	}
	else if (kind == 8 || (kind == 9 && !b))
	{
		for (i = 0; i < NOTES; i++) if (m->notes[i].active && m->notes[i].channel == ch && m->notes[i].key == a)
		{ m->notes[i].held = c->sustain; m->notes[i].released = !c->sustain; }
	}
	else if (kind == 12) c->program = a;
	else if (kind == 14)
	{
		c->bend = a + b * 128;
		for (i = 0; i < NOTES; i++) if (m->notes[i].active && m->notes[i].channel == ch)
			m->notes[i].step = Step(m, ch, m->notes[i].key);
	}
	else if (kind == 11)
	{
		if (a == 7) c->volume = b;
		else if (a == 10) c->pan = b;
		else if (a == 11) c->expression = b;
		else if (a == 64) c->sustain = b >= 64;
		else if (a == 121) { c->sustain = 0; c->expression = 127; c->bend = 8192; }
		for (i = 0; i < NOTES; i++) if (m->notes[i].channel == ch)
		{
			midi_note *n = &m->notes[i];
			if (a == 120) n->active = 0;
			if (a == 123 || (!c->sustain && n->held)) { n->held = 0; n->released = 1; }
			if (a == 121 && n->active) n->step = Step(m, ch, n->key);
		}
	}
}

static void Event(ps2_midi *m, midi_track *t, int synth)
{
	unsigned s = Byte(m, t), a, b = 0, n;
	if (++m->events > MAX_EVENTS) { m->failed = 1; return; }
	if (s < 128)
	{
		if (!t->running) { m->failed = 1; return; }
		t->pos--; s = t->running;
	}
	if (s < 240)
	{
		t->running = s;
		a = Byte(m, t);
		if ((s >> 4) != 12 && (s >> 4) != 13) b = Byte(m, t);
		if (a > 127 || b > 127) { m->failed = 1; return; }
		if (synth) Message(m, s, a, b);
	}
	else
	{
		t->running = 0;
		if (s == 255)
		{
			a = Byte(m, t); n = VLQ(m, t);
			if (n > t->end - t->pos) { m->failed = 1; return; }
			if (a == 47)
			{
				if (n) m->failed = 1;
				t->ended = 1; return;
			}
			if (a == 81)
			{
				if (n != 3) { m->failed = 1; return; }
				m->tempo = (unsigned)Byte(m, t) << 16;
				m->tempo |= (unsigned)Byte(m, t) << 8; m->tempo |= Byte(m, t);
				if (!m->tempo) m->failed = 1;
			}
			else t->pos += n;
		}
		else if (s == 240 || s == 247)
		{
			n = VLQ(m, t);
			if (n > t->end - t->pos) { m->failed = 1; return; }
			t->pos += n;
		}
		else { m->failed = 1; return; }
	}
	Next(m, t);
}

static int Earliest(ps2_midi *m)
{
	unsigned i;
	int best = -1;
	for (i = 0; i < m->count; i++) if (!m->tracks[i].ended &&
		(best < 0 || m->tracks[i].tick < m->tracks[best].tick)) best = (int)i;
	return best;
}

static uint64_t Due(ps2_midi *m, int i, int advance)
{
	uint64_t numerator = (m->tracks[i].tick - m->tick) * m->tempo + m->remainder;
	uint64_t us = m->us + numerator / m->division;
	if (us > MAX_US) { m->failed = 1; return 0; }
	if (advance) { m->us = us; m->remainder = numerator % m->division; m->tick = m->tracks[i].tick; }
	return us * PS2_AUDIO_RATE / 1000000;
}

ps2_midi *PS2_MIDIOpen(const ps2_audio_input *in)
{
	uint8_t h[14];
	size_t pos;
	unsigned i, format, count, division;
	ps2_midi *m;
	if (PS2_AudioRead(in, 0, h, 14) != 14 || memcmp(h, "MThd", 4) || BE(h+4,4) < 6) return NULL;
	format = BE(h+8,2); count = BE(h+10,2); division = BE(h+12,2);
	if (format > 1 || !count || count > TRACKS || (format == 0 && count != 1)
		|| !division || division & 0x8000) return NULL;
	pos = BE(h+4,4);
	if (pos > in->size - 8) return NULL;
	pos += 8;
	m = calloc(1, sizeof *m); if (!m) return NULL;
	m->in = *in; m->count = count; m->division = division;
	for (i = 0; i < count; i++)
	{
		uint32_t n;
		if (PS2_AudioRead(in, pos, h, 8) != 8 || memcmp(h, "MTrk", 4)) goto bad;
		n = BE(h+4,4); pos += 8;
		if (!n || n > in->size - pos) goto bad;
		m->tracks[i].start = pos; m->tracks[i].end = pos + n; pos += n;
	}
	Reset(m);
	while (!m->failed)
	{
		int t = Earliest(m);
		if (t < 0) break;
		m->length_frames = Due(m, t, 1);
		Event(m, &m->tracks[t], 0);
	}
	if (m->failed || !m->length_frames) goto bad;
	m->length_frames += PS2_AUDIO_RATE / 10; // bounded release tail
	for (i = 0; i < 128; i++)
		m->steps[i] = (uint32_t)(440.0 * pow(2.0, ((int)i-69)/12.0) * (4294967296.0 / PS2_AUDIO_RATE));
	for (i = 0; i < 256; i++) m->sine[i] = (int16_t)(sin(i * (6.283185307179586 / 256)) * 32767);
	Reset(m); return m;
bad:
	free(m); return NULL;
}

void PS2_MIDIClose(ps2_midi *m) { free(m); }
int PS2_MIDIError(const ps2_midi *m) { return m->failed != 0; }
uint32_t PS2_MIDILength(const ps2_midi *m) { return (uint32_t)((m->length_frames * 1000 + PS2_AUDIO_RATE-1) / PS2_AUDIO_RATE); }

size_t PS2_MIDIRead(ps2_midi *m, int16_t *out, size_t frames)
{
	size_t f;
	for (f = 0; f < frames && m->frame < m->length_frames && !m->failed; f++, m->frame++)
	{
		unsigned i, budget = 4096;
		int t, l = 0, r = 0;
		while ((t = Earliest(m)) >= 0 && Due(m, t, 0) <= m->frame && !m->failed)
		{
			if (!budget--) { m->failed = 1; break; }
			Due(m, t, 1); Event(m, &m->tracks[t], 1);
		}
		if (m->failed) break;
		for (i = 0; i < NOTES; i++)
		{
			midi_note *n = &m->notes[i];
			midi_channel *c;
			int v, amp;
			unsigned p;
			if (!n->active) continue;
			c = &m->channels[n->channel];
			if (t < 0 || (n->channel == 9 && n->age > PS2_AUDIO_RATE/6)) n->released = 1;
			if (n->released) { if (n->env <= 32) { n->active = 0; continue; } n->env -= 32; }
			else if (n->env < 32768) n->env += 256;
			p = n->phase >> 16;
			if (n->channel == 9)
			{
				n->noise ^= n->noise << 13; n->noise ^= n->noise >> 17; n->noise ^= n->noise << 5;
				v = (int)(n->noise & 65535) - 32768;
			}
			else switch (c->program / 8 % 4)
			{
				case 0: v = m->sine[n->phase >> 24]; break;
				case 1: v = p < 32768 ? (int)p * 2 - 32768 : 98303 - (int)p * 2; break;
				case 2: v = p < 32768 ? 24000 : -24000; break;
				default: v = (int)p - 32768; break;
			}
			amp = (int)(n->velocity * c->volume * c->expression / (127 * 127));
			v = v * amp / (127 * 16); v = v * (int)n->env / 32768;
			l += v * (int)(127 - c->pan) / 127; r += v * (int)c->pan / 127;
			n->phase += n->step; n->age++;
		}
		out[f*2] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
		out[f*2+1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
	}
	return f;
}

int PS2_MIDISeek(ps2_midi *m, uint32_t ms)
{
	uint64_t target = (uint64_t)ms * PS2_AUDIO_RATE / 1000;
	if (target > m->length_frames) return 0;
	Reset(m);
	while (m->frame < target)
	{
		uint64_t next = target;
		unsigned i, budget = 4096;
		int t;
		while ((t = Earliest(m)) >= 0 && Due(m, t, 0) <= m->frame && !m->failed)
		{
			if (!budget--) return 0;
			Due(m, t, 1); Event(m, &m->tracks[t], 1);
		}
		if (m->failed) return 0;
		if (t >= 0 && Due(m, t, 0) < next) next = Due(m, t, 0);
		if (m->failed || next <= m->frame) return 0;
		for (i = 0; i < NOTES; i++)
		{
			midi_note *n = &m->notes[i];
			uint32_t frames = (uint32_t)(next - m->frame), done = 0;
			if (!n->active) continue;
			if (t < 0) n->released = 1;
			if (!n->released && n->channel != 9)
			{
				n->phase += n->step * frames; n->age += frames;
				n->env = frames >= (32768 - n->env)/256 ? 32768 : n->env + frames*256;
				continue;
			}
			// Release and percussion tails are bounded to < 0.22 seconds per
			// voice. Advance their state exactly, without mixing discarded PCM.
			while (done < frames && n->active)
			{
				if (n->channel == 9 && n->age > PS2_AUDIO_RATE/6) n->released = 1;
				if (n->released) { if (n->env <= 32) { n->active = 0; break; } n->env -= 32; }
				else if (n->env < 32768) n->env += 256;
				if (n->channel == 9)
				{ n->noise ^= n->noise << 13; n->noise ^= n->noise >> 17; n->noise ^= n->noise << 5; }
				n->phase += n->step; n->age++; done++;
			}
		}
		m->frame = next;
	}
	return !m->failed;
}
