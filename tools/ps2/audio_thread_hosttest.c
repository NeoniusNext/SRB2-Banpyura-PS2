// Host test of the audio engine (ps2_athread.c): the PCM produced through the command rings, the music ring and the
// shadow state is byte-identical to calling the mixer and the decoder directly (what the old single-thread
// I_UpdateSound did), whatever the interleaving of game thread, mixer thread and decoder thread.
//
//  1. equivalence: random event scripts (starts, stops, parameter changes, music play/stop/seek/loop/speed/pause,
//     gain and SFX volume) applied at block boundaries -> reference (direct calls) vs engine driven with random
//     posting times and random decoder read-ahead. Output PCM, voice handles and "is playing" answers must match.
//  2. starvation: the decoder never runs -> SFX output still equals the reference's SFX-only output, music is silent,
//     `music_starved` counts the missing blocks (the mixer never waits for the decoder).
//  3. threads (Windows threads): a gapless counter music must come out of the mixer thread in order without holes or
//     repeats while the decoder thread stalls at random; samples released through the Forget protocol are poisoned
//     and must never be heard; handles/generations stay consistent.
//
// usage: audio_thread_hosttest [--negative-control]  (the controls perturb the engine path and must be detected)
#include "ps2_athread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static int negative_control;
static unsigned kind_mask = 0xffffffffu;   // debugging: restrict the event kinds (--mask N)
static unsigned failures;
#define EXPECT(x) do { if (!(x)) { failures++; if (failures < 6) fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static unsigned long long rng_state = 88172645463325252ull;
static unsigned Rnd(void)
{
	rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
	return (unsigned)(rng_state >> 32);
}

// ---- fixtures ---------------------------------------------------------------------------------------------------

#define NSAMPLES 6
static ps2_sample samples[NSAMPLES];
static uint8_t *sample_data[NSAMPLES];

static void PutLE16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void PutLE32(uint8_t *p, unsigned v) { PutLE16(p, v & 0xffff); PutLE16(p + 2, v >> 16); }

static void MakeSamples(void)
{
	static const struct { unsigned bits, channels, rate, frames; } fmt[NSAMPLES] = {
		{ 8, 1, 11025, 3000 }, { 8, 2, 22050, 5000 }, { 16, 1, 22050, 4000 }, { 16, 2, 22050, 9000 },
		{ 16, 2, 44100, 7000 }, { 8, 1, 22050, 20000 } };
	unsigned i, j;
	for (i = 0; i < NSAMPLES; i++)
	{
		size_t bytes = (size_t)fmt[i].frames * fmt[i].channels * fmt[i].bits / 8;
		sample_data[i] = calloc(1, bytes + 64);
		CHECK(sample_data[i]);
		for (j = 0; j < bytes; j++) sample_data[i][j] = (uint8_t)(Rnd() >> 8);
		samples[i].bytes = sample_data[i];
		samples[i].pcm.offset = 0; samples[i].pcm.frames = fmt[i].frames; samples[i].pcm.rate = fmt[i].rate;
		samples[i].pcm.bits = fmt[i].bits; samples[i].pcm.channels = fmt[i].channels;
	}
}

typedef struct { const uint8_t *data; size_t size; } mem_source;
static size_t MemRead(void *user, size_t off, void *dst, size_t n)
{
	const mem_source *s = user;
	memcpy(dst, s->data + off, n);
	return n;
}

// PCM WAV music. mode 0: pseudo-random looking but deterministic; mode 1: frame i = (L = (i+1) & 0x7fff, R = (i+1) >> 15),
// so every output frame names its own position and silence (0,0) never occurs in the data.
static uint8_t *MakeWav(unsigned rate, unsigned channels, unsigned frames, int counter, size_t *size)
{
	size_t bytes = (size_t)frames * channels * 2;
	uint8_t *w = calloc(1, 44 + bytes);
	unsigned i, c;
	CHECK(w);
	memcpy(w, "RIFF", 4); PutLE32(w + 4, (unsigned)(36 + bytes)); memcpy(w + 8, "WAVEfmt ", 8);
	PutLE32(w + 16, 16); PutLE16(w + 20, 1); PutLE16(w + 22, channels); PutLE32(w + 24, rate);
	PutLE32(w + 28, rate * channels * 2); PutLE16(w + 32, channels * 2); PutLE16(w + 34, 16);
	memcpy(w + 36, "data", 4); PutLE32(w + 40, (unsigned)bytes);
	for (i = 0; i < frames; i++)
		for (c = 0; c < channels; c++)
		{
			unsigned v = counter ? (c ? (i + 1) >> 15 : (i + 1) & 0x7fff) : ((i * 7u + c * 1000u + i / 13u) & 0x7fff);
			PutLE16(w + 44 + ((size_t)i * channels + c) * 2, v);
		}
	*size = 44 + bytes;
	return w;
}

static mem_source music_src[3];
static ps2_audio_input music_in[3];

static ps2_music *OpenMusic(unsigned which) { return PS2_MusicOpen(&music_in[which]); }

// ---- event scripts ----------------------------------------------------------------------------------------------

enum { E_START, E_STOP, E_PARAMS, E_MPLAY, E_MSTOP, E_MSEEK, E_MLOOP, E_MSPEED, E_MPAUSE, E_MRESUME, E_GAIN, E_SFXVOL };
typedef struct { int kind, ch, sample; unsigned a, b, c; int loop; float speed; int old; } event;
#define MAX_EVENTS 8
typedef struct { unsigned count; event ev[MAX_EVENTS]; } block_events;

static event RandomEvent(void)
{
	event e;
	unsigned r = Rnd() % 100;
	memset(&e, 0, sizeof e);
	e.ch = (int)(Rnd() % 6);          // few channels: restarts, stale handles and steals are common
	e.sample = (int)(Rnd() % NSAMPLES);
	e.a = Rnd() % 300; e.b = Rnd() % 300; e.c = Rnd() % 300;
	e.old = (int)(Rnd() % 4);         // which earlier handle of the channel to stop/modify (0 = newest)
	if (r < 28) e.kind = E_START;
	else if (r < 36) e.kind = E_STOP;
	else if (r < 52) e.kind = E_PARAMS;
	else if (r < 58) { e.kind = E_MPLAY; e.loop = (int)(Rnd() & 1); }
	else if (r < 62) e.kind = E_MSTOP;
	else if (r < 68) { e.kind = E_MSEEK; e.a = Rnd() % 1200; }
	else if (r < 72) { e.kind = E_MLOOP; e.a = Rnd() % 800; }
	else if (r < 75) { e.kind = E_MSPEED; e.speed = 0.5f + (float)(Rnd() % 8) * 0.25f; e.a = Rnd() % 800; }
	else if (r < 83) e.kind = E_MPAUSE;
	else if (r < 91) e.kind = E_MRESUME;
	else if (r < 96) { e.kind = E_GAIN; e.a = Rnd() % 33000; }
	else { e.kind = E_SFXVOL; e.a = Rnd() % 40; }
	if (!(kind_mask >> e.kind & 1)) { e.kind = E_START; e.ch = 0; e.a = 200; e.b = 128; e.c = 128; }
	return e;
}

// ---- reference: direct calls, exactly what the old I_UpdateSound did per block --------------------------------------

typedef struct
{
	ps2_mixer mixer;
	ps2_music *music;
	unsigned gain;
	int handles[6][4];
} reference;

static int RefApply(reference *r, const event *e)
{
	int h = -2;
	switch (e->kind)
	{
		case E_START:
			h = PS2_MixerStart(&r->mixer, &samples[e->sample], e->ch, e->a, e->b, e->c);
			memmove(&r->handles[e->ch][1], &r->handles[e->ch][0], 3 * sizeof(int)); r->handles[e->ch][0] = h;
			break;
		case E_STOP: PS2_MixerStop(&r->mixer, r->handles[e->ch][e->old]); break;
		case E_PARAMS: PS2_MixerParams(&r->mixer, r->handles[e->ch][e->old], e->a, e->b, e->c); break;
		case E_MPLAY: PS2_MusicPlay(r->music, e->loop); break;
		case E_MSTOP: PS2_MusicStop(r->music); break;
		case E_MSEEK: PS2_MusicSeek(r->music, e->a); break;
		case E_MLOOP: PS2_MusicSetLoop(r->music, e->a); break;
		case E_MSPEED: PS2_MusicSpeed(r->music, e->speed); PS2_MusicSeek(r->music, e->a); break;
		case E_MPAUSE: PS2_MusicPause(r->music, 1); break;
		case E_MRESUME: PS2_MusicPause(r->music, 0); break;
		case E_GAIN: r->gain = e->a; break;
		case E_SFXVOL: r->mixer.volume = e->a > 31 ? 31 : e->a; break;
	}
	return h;
}

typedef struct { ps2_engine *e; ps2_music *music; int handles[6][4]; } subject;

static int SubApply(subject *s, const event *e)
{
	ps2_engine *eng = s->e;
	int h = -2;
	switch (e->kind)
	{
		case E_START:
			h = PS2E_Start(eng, &samples[e->sample], e->ch, e->a, e->b, e->c, NULL);
			memmove(&s->handles[e->ch][1], &s->handles[e->ch][0], 3 * sizeof(int)); s->handles[e->ch][0] = h;
			break;
		case E_STOP: PS2E_Stop(eng, s->handles[e->ch][e->old]); break;
		case E_PARAMS: PS2E_Params(eng, s->handles[e->ch][e->old], e->a, e->b, e->c); break;
		case E_MPLAY: PS2E_MusicPlay(eng, e->loop); break;
		case E_MSTOP: PS2E_MusicStop(eng); break;
		case E_MSEEK: PS2E_MusicSeek(eng, e->a); break;
		case E_MLOOP: PS2E_MusicLoopPoint(eng, e->a); break;
		case E_MSPEED: PS2E_MusicSpeed(eng, e->speed, e->a); break;
		case E_MPAUSE: eng->music_paused = 1; break;
		case E_MRESUME: eng->music_paused = 0; break;
		case E_GAIN: eng->music_gain = e->a; break;
		case E_SFXVOL: eng->sfx_volume = e->a; break;
	}
	return h;
}

// Is there a slot of the wanted epoch ready?
static int HasValidSlot(const ps2_engine *e)
{
	unsigned i;
	for (i = e->slot_tail; i != e->slot_head; i++)
		if (e->slots[i % PS2E_SLOTS].epoch == e->want_epoch) return 1;
	return 0;
}

static unsigned long long total_blocks, total_events;

// One scenario: the decoder always keeps up (a stalled decoder is test 2/3). Returns the number of mismatches.
static unsigned RunScenario(unsigned scenario_seed, unsigned blocks, int perturb)
{
	static ps2_engine eng;
	reference ref;
	subject sub;
	unsigned b, i, bad = 0, w, which = scenario_seed % 3 == 2 ? 1 : 0;
	int perturbed = 0;
	int16_t out_ref[PS2_AUDIO_BLOCK * 2], out_eng[PS2_AUDIO_BLOCK * 2];
	block_events *script = calloc(blocks, sizeof *script);
	CHECK(script);
	rng_state = 0x9e3779b97f4a7c15ull ^ ((unsigned long long)scenario_seed * 2654435761ull);
	for (b = 0; b < blocks; b++)
	{
		unsigned n = Rnd() % 3 == 0 ? 1 + Rnd() % MAX_EVENTS : (Rnd() & 1);
		if (b == 0) n = 1 + Rnd() % 3;
		for (i = 0; i < n; i++)
		{
			event ev = RandomEvent();
			// The game sets a loop point right after I_PlaySong (S_PlayMusic), before anything was decoded: a later change
			// applies from the next wrap the decoder reaches, not to blocks it already decoded (documented, not tested here).
			if (ev.kind == E_MLOOP && script[b].count + 2 <= MAX_EVENTS)
			{
				event play = ev;
				play.kind = E_MPLAY; play.loop = (int)(Rnd() & 1);
				script[b].ev[script[b].count++] = play;
			}
			if (script[b].count < MAX_EVENTS) script[b].ev[script[b].count++] = ev;
		}
	}
	memset(&ref, 0, sizeof ref); PS2_MixerInit(&ref.mixer); ref.gain = 32768;
	memset(&sub, 0, sizeof sub);
	PS2E_Init(&eng); sub.e = &eng; eng.diag_hash = 1;
	for (b = 0; b < 6; b++) for (i = 0; i < 4; i++) ref.handles[b][i] = sub.handles[b][i] = -1;
	ref.music = OpenMusic(which); sub.music = OpenMusic(which);
	CHECK(ref.music && sub.music);
	PS2E_MusicOpen(&eng, sub.music);
	for (b = 0; b < blocks; b++)
	{
		int h_ref[MAX_EVENTS];
		// reference: events, then music + mix exactly like the old per-block code
		for (i = 0; i < script[b].count; i++) h_ref[i] = RefApply(&ref, &script[b].ev[i]);
		PS2_MusicRender(ref.music, out_ref, PS2_AUDIO_BLOCK);
		PS2_MixerRender(&ref.mixer, out_ref, out_ref, PS2_AUDIO_BLOCK, ref.gain);
		// engine: events posted at random moments between the previous block and this one
		for (w = 0; w < 1 + Rnd() % 3; w++)
		{
			unsigned k, steps = Rnd() % 30;
			for (k = 0; k < steps; k++) PS2E_DecodeStep(&eng);                // decoder runs ahead by a random amount
			if (Rnd() & 1) PS2E_Process(&eng);                                 // a mixer wake-up with nothing new
		}
		for (i = 0; i < script[b].count; i++)
		{
			event ev = script[b].ev[i];
			int h;
			if (perturb == 1 && b >= 30 && ev.kind == E_START) { ev.a = ev.a >= 150 ? 60 : 255; perturbed = 1; } // control: a wrong parameter
			if (perturb == 2 && b >= 30 && !perturbed && ev.kind == E_START) { ev.kind = E_STOP; perturbed = 1; }      // control: a lost start
			h = SubApply(&sub, &ev);
			if (script[b].ev[i].kind == E_START && ev.kind == E_START) EXPECT(h == h_ref[i]);
			if (Rnd() % 4 == 0) PS2E_Process(&eng);
			if (Rnd() % 5 == 0) { unsigned k, steps = Rnd() % 12; for (k = 0; k < steps; k++) PS2E_DecodeStep(&eng); }
		}
		total_events += script[b].count;
		// the decoder keeps up: drain its commands, then make sure a block of the wanted epoch is there
		PS2E_Process(&eng);                                   // the mixer drops blocks of old epochs first
		while (eng.dcmd_tail != eng.dcmd_head) PS2E_DecodeStep(&eng);
		while (!HasValidSlot(&eng) && PS2E_DecodeStep(&eng)) {}
		PS2E_Render(&eng, out_eng);
		if (perturb == 3 && b == 17) out_eng[5] ^= 1; // control: one bit of one sample
		if (perturb == 4 && b >= 20)                  // control: a bit flips inside the music ring (a transport defect)
			for (i = eng.slot_tail; i != eng.slot_head; i++) eng.slots[i % PS2E_SLOTS].pcm[7] ^= 0x100;
		if (memcmp(out_ref, out_eng, sizeof out_ref))
		{
			bad++;
			if (bad < 3 && !negative_control)
			{
				unsigned bb, k;
				fprintf(stderr, "scenario %u block %u differs; events:", scenario_seed, b);
				for (bb = b >= 3 ? b - 3 : 0; bb <= b; bb++)
					for (k = 0; k < script[bb].count; k++)
						fprintf(stderr, " [b%u k%d ch%d a%u]", bb, script[bb].ev[k].kind, script[bb].ev[k].ch, script[bb].ev[k].a);
				fprintf(stderr, "\n");
				{
					unsigned q, first = 0xffffffffu, cnt = 0;
					for (q = 0; q < PS2_AUDIO_BLOCK * 2; q++)
						if (out_ref[q] != out_eng[q]) { if (first == 0xffffffffu) first = q; cnt++; }
					unsigned bm, km;
					fprintf(stderr, "  first diff sample %u of %u: ref %d eng %d (music pos ref %u, eng %u) starved=%u epoch=%u refplaying=%d dplaying=%d\n",
						first, cnt, out_ref[first], out_eng[first], PS2_MusicPosition(ref.music), eng.music_pos_ms,
						eng.st.music_starved, eng.want_epoch, PS2_MusicPlaying(ref.music), eng.d_playing);
					fprintf(stderr, "  music events:");
					for (bm = 0; bm <= b; bm++)
						for (km = 0; km < script[bm].count; km++)
							if (script[bm].ev[km].kind >= E_MPLAY && script[bm].ev[km].kind <= E_MRESUME)
								fprintf(stderr, " [b%u k%d a%u l%d]", bm, script[bm].ev[km].kind, script[bm].ev[km].a, script[bm].ev[km].loop);
					fprintf(stderr, "\n");
				}
			}
		}
		// the game thread's view of playing voices equals the reference mixer's
		for (i = 0; i < 6; i++)
		{
			unsigned j;
			for (j = 0; j < 4; j++)
				if (ref.handles[i][j] >= 0 || sub.handles[i][j] >= 0)
					if (PS2_MixerPlaying(&ref.mixer, ref.handles[i][j]) != PS2E_Playing(&eng, sub.handles[i][j])) { bad++; break; }
		}
		if (getenv("DBG") && scenario_seed == (unsigned)atoi(getenv("DBG")) && b >= 8 && b <= 22)
			fprintf(stderr, "dbg b%u refpos %u refplaying %d | engpos %u filled %u dplaying %d depoch %u want %u end %u\n", b, PS2_MusicPosition(ref.music), PS2_MusicPlaying(ref.music), eng.music_pos_ms, eng.slot_head - eng.slot_tail, eng.d_playing, eng.d_epoch, eng.want_epoch, eng.music_end_epoch);
		total_blocks++;
	}
	EXPECT(eng.st.handle_mismatch == 0);
	if (perturb == 4) bad += eng.st.hash_errors ? 1000 : 0;   // the per-block hash must notice it too
	else EXPECT(eng.st.hash_errors == 0);
	PS2E_MusicClose(&eng);
	while (!PS2E_MusicClosed(&eng)) PS2E_DecodeStep(&eng);
	PS2_MusicClose(ref.music); PS2_MusicClose(sub.music);
	free(script);
	return bad;
}

// ---- 2. the mixer never waits for the decoder ---------------------------------------------------------------------

static void StarvationTest(void)
{
	static ps2_engine eng;
	ps2_mixer ref;
	int16_t a[PS2_AUDIO_BLOCK * 2], b[PS2_AUDIO_BLOCK * 2];
	unsigned blk;
	PS2_MixerInit(&ref); PS2E_Init(&eng);
	CHECK(PS2_MixerStart(&ref, &samples[3], 1, 255, 64, 128) == PS2E_Start(&eng, &samples[3], 1, 255, 64, 128, NULL));
	PS2E_MusicOpen(&eng, OpenMusic(0));
	PS2E_MusicPlay(&eng, 1);
	// the decoder processed the open/play (so a song is playing) and then stalls: blocks must still be rendered on time
	PS2E_DecodeStep(&eng);
	{
		unsigned drop = eng.slot_head - eng.slot_tail;
		eng.slot_tail += drop; // pretend the first blocks were consumed; from now on the ring stays empty
		eng.m_flowing = 1; eng.m_epoch_seen = eng.want_epoch;
	}
	eng.dcmd_tail = eng.dcmd_head; // the decoder gets no further time
	for (blk = 0; blk < 12; blk++)
	{
		PS2_MixerRender(&ref, a, NULL, PS2_AUDIO_BLOCK, 32768);
		PS2E_Process(&eng); PS2E_Render(&eng, b);
		EXPECT(!memcmp(a, b, sizeof a));
	}
	EXPECT(eng.st.music_starved == 12);
}

// ---- accounting of underruns --------------------------------------------------------------------------------------------

// A synthetic timeline (1 tick = 1 ms): refills every 10 ms keep >= 70 ms queued; then the refiller disappears for 300 ms.
static unsigned AcctRun(unsigned stall_ms, unsigned wrapped_queue, unsigned *gap_max)
{
	static ps2_engine e;
	ps2e_acct a;
	unsigned t = 0, queued = 0, i;
	PS2E_Init(&e); memset(&a, 0, sizeof a);
	for (i = 0; i < 40; i++)
	{
		unsigned consumed = i ? 882 : 0;                      // 10 ms of 22050 Hz stereo 16 bit
		queued = queued > consumed ? queued - consumed : 0;
		PS2E_AcctBegin(&a, &e, t, 1000, (int)queued);
		PS2E_AcctEnd(&a, t, 1000, (int)(7500 - queued), 1);  // refill to 7500 bytes (85 ms)
		queued = 7500;
		t += 10;
	}
	// the refiller is away; what the IOP did meanwhile is either an empty ring or a ring that audsrv topped up with silence
	t += stall_ms;
	PS2E_AcctBegin(&a, &e, t, 1000, wrapped_queue ? (int)wrapped_queue : 0);
	PS2E_AcctEnd(&a, t, 1000, 0, 0);
	*gap_max = e.st.gap_max_ms;
	return e.st.underruns;
}

static void AcctTest(void)
{
	unsigned gap;
	{
		// healthy: a refill every 10 ms keeps 70..85 ms queued for 4 s
		static ps2_engine e;
		ps2e_acct a;
		unsigned t = 0, i, queued = 0;
		PS2E_Init(&e); memset(&a, 0, sizeof a);
		for (i = 0; i < 400; i++)
		{
			unsigned consumed = i ? 882 : 0;
			queued = queued > consumed ? queued - consumed : 0;
			PS2E_AcctBegin(&a, &e, t, 1000, (int)queued);
			PS2E_AcctEnd(&a, t, 1000, (int)(7500 - queued), 1);
			queued = 7500; t += 10;
		}
		EXPECT(e.st.underruns == 0 && e.st.min_queue_ms >= 70);
	}
	EXPECT(AcctRun(300, 0, &gap) == 1);          // 300 ms away, ring empty: underrun, gap = 300 + 10 - 85 = 225 ms
	EXPECT(gap >= 215 && gap <= 235);
	EXPECT(AcctRun(300, 5640, &gap) == 1);       // the IOP topped the ring up with silence: queued looks plausible, the clock says underrun
	EXPECT(gap >= 215 && gap <= 235);
	EXPECT(AcctRun(5, 0, &gap) == 1);            // an empty ring is an underrun whatever the clock says
	EXPECT(AcctRun(60, 2000, &gap) == 0);        // 70 ms later 2000 bytes (23 ms) remain: no underrun
	EXPECT(AcctRun(20, 9500, &gap) == 1);        // a queue larger than what the last refill left can only be audsrv's silence
}

// ---- 3. real threads ------------------------------------------------------------------------------------------------
#ifdef _WIN32
static ps2_engine teng;
static volatile int stop_threads;
static volatile unsigned t_stall_once_ms, t_start_tick;
static volatile unsigned t_music_blocks, t_holes, t_repeats, t_poison, t_frames_checked, t_decoder_stall_ms;
static unsigned mixer_rng = 12345, decoder_rng = 6789;
static unsigned TRnd(unsigned *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }
#define COUNTER_FRAMES 300000u

static DWORD WINAPI DecoderMain(LPVOID p)
{
	(void)p;
	while (!stop_threads)
	{
		int work = PS2E_DecodeStep(&teng);
		if (t_stall_once_ms && GetTickCount() - t_start_tick > 700)
		{
			unsigned ms = t_stall_once_ms;
			t_stall_once_ms = 0;
			Sleep(ms);              // one long stall of the decoder (a disk read that takes seconds)
		}
		if (!work) Sleep(1);
		else if (t_decoder_stall_ms && TRnd(&decoder_rng) % 100 == 0) Sleep(1 + TRnd(&decoder_rng) % t_decoder_stall_ms);
	}
	return 0;
}

static unsigned phase_music_only;   // set by the game thread: voices are silent, no transport commands in flight
static volatile unsigned mixer_expect_next = 0xffffffffu;   // next counter value expected (loop wraps to 0)

static void CheckBlock(const int16_t *out)
{
	unsigned f;
	if (phase_music_only)
	{
		for (f = 0; f < PS2_AUDIO_BLOCK; f++)
		{
			unsigned l = (unsigned)(uint16_t)out[f * 2], r = (unsigned)(uint16_t)out[f * 2 + 1];
			unsigned i;
			if (!l && !r) { if (mixer_expect_next != 0xffffffffu) t_holes++; mixer_expect_next = 0xffffffffu; continue; }
			i = ((r << 15) | l) - 1;
			if (mixer_expect_next != 0xffffffffu && i != mixer_expect_next)
			{
				if (i < mixer_expect_next && i >= mixer_expect_next - 64) t_repeats++; else t_holes++;
			}
			mixer_expect_next = i + 1 >= COUNTER_FRAMES ? 0 : i + 1;
			t_frames_checked++;
		}
		t_music_blocks++;
	}
	else
		for (f = 0; f < PS2_AUDIO_BLOCK * 2; f++)
			if (out[f] > 6000 || out[f] < -6000) t_poison++;   // legit SFX of this phase stay within +-1000
}

// The mixer thread consumes blocks at 4x real time (one block per 5.8 ms), like the audsrv queue would drain, with a lead of 6 blocks.
static DWORD WINAPI MixerMain(LPVOID p)
{
	int16_t out[PS2_AUDIO_BLOCK * 2];
	LARGE_INTEGER freq, t0, now;
	unsigned long long rendered = 0, due;
	(void)p;
	QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0);
	while (!stop_threads)
	{
		PS2E_Process(&teng);
		QueryPerformanceCounter(&now);
		due = (unsigned long long)(now.QuadPart - t0.QuadPart) * 1000000 / (unsigned long long)freq.QuadPart / 5800 + 6;
		while (rendered < due)
		{
			rendered++;
			if (PS2E_Active(&teng)) { PS2E_Render(&teng, out); CheckBlock(out); }
		}
		Sleep(TRnd(&mixer_rng) % 3);
	}
	return 0;
}

static unsigned ThreadTest(unsigned music_ms, unsigned stall_ms, unsigned stall_once_ms, unsigned *holes, unsigned *starved)
{
	HANDLE th[2];
	unsigned t0;
	PS2E_Init(&teng);
	stop_threads = 0; t_music_blocks = t_holes = t_repeats = t_poison = t_frames_checked = 0;
	t_decoder_stall_ms = stall_ms; t_stall_once_ms = stall_once_ms; phase_music_only = 1; mixer_expect_next = 0xffffffffu;
	t_start_tick = GetTickCount();
	PS2E_MusicOpen(&teng, OpenMusic(2));
	th[0] = CreateThread(NULL, 0, DecoderMain, NULL, 0, NULL);
	th[1] = CreateThread(NULL, 0, MixerMain, NULL, 0, NULL);
	CHECK(th[0] && th[1]);
	PS2E_MusicPlay(&teng, 1);
	t0 = GetTickCount();
	while (GetTickCount() - t0 < music_ms) Sleep(5);
	stop_threads = 1;
	WaitForMultipleObjects(2, th, TRUE, 5000);
	CloseHandle(th[0]); CloseHandle(th[1]);
	PS2E_MusicClose(&teng);
	while (!PS2E_MusicClosed(&teng)) PS2E_DecodeStep(&teng);
	*holes = t_holes; *starved = teng.st.music_starved;
	return t_repeats;
}

// Release protocol: a voice plays a quiet constant sample; the game thread asks M to forget it, waits, poisons it.
static unsigned PoisonTest(unsigned iterations)
{
	static int16_t quiet[8000];
	ps2_sample s;
	HANDLE th[2];
	unsigned i, busy_waits = 0;
	for (i = 0; i < 8000; i++) quiet[i] = 60;
	s.bytes = (const uint8_t *)quiet; s.pcm.offset = 0; s.pcm.frames = 8000; s.pcm.rate = 22050; s.pcm.bits = 16; s.pcm.channels = 1;
	PS2E_Init(&teng);
	stop_threads = 0; t_poison = 0; phase_music_only = 0; t_decoder_stall_ms = 0;
	th[0] = CreateThread(NULL, 0, DecoderMain, NULL, 0, NULL);
	th[1] = CreateThread(NULL, 0, MixerMain, NULL, 0, NULL);
	CHECK(th[0] && th[1]);
	for (i = 0; i < iterations; i++)
	{
		uint32_t seq = 0, last = 0;
		unsigned n, k, spin;
		for (n = 1 + Rnd() % 4; n; n--) PS2E_Start(&teng, &s, (int)(Rnd() % PS2_AUDIO_CHANNELS), 255, 128, 128, &last);
		Sleep(Rnd() % 3);
		if (Rnd() & 1) Sleep(Rnd() % 12);
		if (PS2E_SampleBusy(&teng, &s, last))
		{
			seq = PS2E_Forget(&teng, &s);
			CHECK(seq);
			for (spin = 0; !PS2E_Consumed(&teng, seq) && spin < 5000; spin++) Sleep(1);
			CHECK(PS2E_Consumed(&teng, seq));
			busy_waits++;
		}
		// now the game thread owns the memory again
		CHECK(!PS2E_SampleBusy(&teng, &s, last));
		for (k = 0; k < 8000; k++) quiet[k] = 30000;
		Sleep(Rnd() % 3);
		for (k = 0; k < 8000; k++) quiet[k] = 60;
	}
	stop_threads = 1;
	WaitForMultipleObjects(2, th, TRUE, 5000);
	CloseHandle(th[0]); CloseHandle(th[1]);
	return t_poison + (teng.st.handle_mismatch ? 1u : 0u) + (busy_waits ? 0u : 1u);
}
#endif

// ---- main ------------------------------------------------------------------------------------------------------------

int main(int argc, char **argv)
{
	unsigned scenario, bad_total = 0, scenarios = 450;
	size_t size;
	int arg;
	for (arg = 1; arg < argc; arg++)
	{
		if (!strcmp(argv[arg], "--negative-control")) negative_control = 1;
		if (!strcmp(argv[arg], "--mask") && arg + 1 < argc) kind_mask = (unsigned)strtoul(argv[++arg], NULL, 0);
	}
	MakeSamples();
	music_src[0].data = MakeWav(22050, 2, 40000, 0, &size);   // 78 blocks, then loops
	music_src[0].size = size;
	music_in[0].user = &music_src[0]; music_in[0].size = size; music_in[0].read_at = MemRead;
	music_src[1].data = MakeWav(11025, 1, 9000, 0, &size);    // not the native rate: the resampling path
	music_src[1].size = size;
	music_in[1].user = &music_src[1]; music_in[1].size = size; music_in[1].read_at = MemRead;
	music_src[2].data = MakeWav(22050, 2, 300000u, 1, &size); // the counter stream for the thread test
	music_src[2].size = size;
	music_in[2].user = &music_src[2]; music_in[2].size = size; music_in[2].read_at = MemRead;
	if (negative_control)
	{
		unsigned c, seen = 0;
		for (c = 1; c <= 4; c++)
		{
			unsigned bad = RunScenario(1000 + c, 120, (int)c);
			printf("negative control %u (%s): %u mismatches%s\n", c,
				c == 1 ? "wrong start parameter" : c == 2 ? "lost start" : c == 3 ? "one flipped bit" : "a bit flipped inside the music ring",
				bad, bad ? " (detected)" : " (NOT detected)");
			if (bad) seen++;
		}
#ifdef _WIN32
		{
			unsigned holes, starved;
			// a decoder stall far beyond the ring depth (557 ms): the stream must show holes
			ThreadTest(3500, 0, 1500, &holes, &starved);
			printf("negative control 5 (one decoder stall of 1500 ms, ring holds 557 ms): %u holes, %u starved blocks%s\n", holes, starved,
				holes && starved ? " (detected)" : " (NOT detected)");
			if (holes && starved) seen++;
		}
#endif
		printf("negative controls detected: %u of 5\n", seen);
		return seen == 5 ? 1 : 0; // the harness expects a failing exit code
	}
	for (scenario = 0; scenario < scenarios; scenario++)
		bad_total += RunScenario(scenario, 160 + (scenario % 7) * 20, 0);
	printf("Engine equivalence: %u scenarios, %llu events, %llu blocks (%llu frames) byte-identical to direct mixer/decoder calls, "
		"%u mismatches\n", scenarios, total_events, total_blocks, total_blocks * PS2_AUDIO_BLOCK, bad_total);
	CHECK(bad_total == 0);
	StarvationTest();
	CHECK(failures == 0);
	puts("Starvation: SFX output unchanged while the decoder is stalled, music_starved counted PASS");
	AcctTest();
	CHECK(failures == 0);
	puts("Underrun accounting: healthy stream 0, empty ring / silence top-up / long absence counted with gap PASS");
#ifdef _WIN32
	{
		unsigned holes, starved, repeats;
		repeats = ThreadTest(6000, 40, 0, &holes, &starved);
		printf("Threads: %u music blocks (%u frames) in order, %u holes, %u repeats, %u starved, decoder stalls up to 40 ms\n",
			t_music_blocks, t_frames_checked, holes, repeats, starved);
		CHECK(holes == 0 && repeats == 0 && starved == 0 && t_music_blocks > 100);
		bad_total = PoisonTest(1500);
		printf("Threads: sample release protocol, %u poisoned/mismatching outputs\n", bad_total);
		CHECK(bad_total == 0);
	}
#endif
	puts("Audio engine: equivalence, starvation independence, threaded continuity and release protocol PASS");
	return 0;
}
