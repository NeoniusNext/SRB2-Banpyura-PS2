// Audio engine control plane (see ps2_athread.h). Portable; no allocation, no locks, no printing.
#include "ps2_athread.h"
#include <string.h>

void (*PS2E_YieldHook)(void);

static void Yield(void) { if (PS2E_YieldHook) PS2E_YieldHook(); }

// Wrap-safe: has the counter reached `seq`?
static int Reached(ps2e_u32 counter, ps2e_u32 seq) { return (ps2e_s32)(counter - seq) >= 0; }

void PS2E_Init(ps2_engine *e)
{
	unsigned i;
	memset(e, 0, sizeof *e);
	PS2_MixerInit(&e->mixer);
	e->sfx_volume = 31;
	e->music_gain = 32768;
	e->st.min_queue_ms = 0xffffffffu;
	for (i = 0; i < PS2_AUDIO_CHANNELS; i++) e->shadow[i].handle = -1;
}

// ---- game thread --------------------------------------------------------------------------------------------

static ps2e_u32 Post(ps2_engine *e, const ps2e_cmd *c)
{
	ps2e_u32 head = e->cmd_head, tries;
	for (tries = 0; head - e->consumed >= PS2E_CMDS; tries++)
	{
		e->st.cmd_full++;
		if (tries >= 50 || !PS2E_YieldHook) return 0;
		Yield();
	}
	e->cmds[head % PS2E_CMDS] = *c;
	PS2E_FENCE();
	e->cmd_head = head + 1;
	return head + 1;
}

int PS2E_Start(ps2_engine *e, const ps2_sample *s, int channel, unsigned volume, unsigned pan, unsigned pitch, ps2e_u32 *seq)
{
	ps2e_shadow *sh;
	ps2e_cmd c;
	ps2e_u32 generation, n;
	int handle;
	// the same conditions as PS2_MixerStart: a start that M would refuse must never get a handle
	if (!s || !s->bytes || !s->pcm.frames || channel < 0 || channel >= PS2_AUDIO_CHANNELS) return -1;
	sh = &e->shadow[channel];
	generation = sh->generation % 0x1ffffffu + 1;
	handle = (int)(generation * PS2_AUDIO_CHANNELS + (unsigned)channel);
	c.op = PS2E_C_START; c.volume = volume; c.pan = pan; c.pitch = pitch; c.handle = handle; c.sample = s;
	n = Post(e, &c);
	if (!n) return -1;
	sh->generation = generation; sh->handle = handle; sh->start_seq = n; sh->stopped = 0;
	sh->volume = volume > 255 ? 255 : volume; sh->pan = pan > 255 ? 255 : pan; sh->pitch = pitch ? pitch : 1;
	if (seq) *seq = n;
	return handle;
}

static ps2e_shadow *Shadow(ps2_engine *e, int handle)
{
	ps2e_shadow *sh;
	if (handle < 0) return NULL;
	sh = &e->shadow[handle & (PS2_AUDIO_CHANNELS - 1)];
	return sh->handle == handle && !sh->stopped ? sh : NULL;
}

void PS2E_Stop(ps2_engine *e, int handle)
{
	ps2e_shadow *sh = Shadow(e, handle);
	ps2e_cmd c;
	if (!sh) return;
	c.op = PS2E_C_STOP; c.handle = handle; c.volume = c.pan = c.pitch = 0; c.sample = NULL;
	Post(e, &c);
	sh->stopped = 1;
}

void PS2E_Params(ps2_engine *e, int handle, unsigned volume, unsigned pan, unsigned pitch)
{
	ps2e_shadow *sh = Shadow(e, handle);
	ps2e_cmd c;
	if (!sh) return;
	if (volume > 255) volume = 255;
	if (pan > 255) pan = 255;
	if (!pitch) pitch = 1;
	if (sh->volume == volume && sh->pan == pan && sh->pitch == pitch) return;
	c.op = PS2E_C_PARAMS; c.handle = handle; c.volume = volume; c.pan = pan; c.pitch = pitch; c.sample = NULL;
	if (Post(e, &c)) { sh->volume = volume; sh->pan = pan; sh->pitch = pitch; }
}

int PS2E_Consumed(const ps2_engine *e, ps2e_u32 seq) { return Reached(e->consumed, seq); }

int PS2E_Playing(const ps2_engine *e, int handle)
{
	const ps2e_shadow *sh;
	unsigned ch;
	if (handle < 0) return 0;
	ch = (unsigned)handle & (PS2_AUDIO_CHANNELS - 1);
	sh = &e->shadow[ch];
	if (sh->handle != handle || sh->stopped) return 0;
	if (!PS2E_Consumed(e, sh->start_seq)) return 1; // queued, M has not run it yet
	PS2E_FENCE();
	return e->live_handle[ch] == (ps2e_u32)handle + 1;
}

ps2e_u32 PS2E_Forget(ps2_engine *e, const ps2_sample *s)
{
	ps2e_cmd c;
	c.op = PS2E_C_FORGET; c.handle = 0; c.volume = c.pan = c.pitch = 0; c.sample = s;
	return Post(e, &c);
}

int PS2E_SampleBusy(const ps2_engine *e, const ps2_sample *s, ps2e_u32 last_start_seq)
{
	unsigned i;
	if (last_start_seq && !PS2E_Consumed(e, last_start_seq)) return 1;
	PS2E_FENCE();
	for (i = 0; i < PS2_AUDIO_CHANNELS; i++) if (e->live_sample[i] == s) return 1;
	return 0;
}

static int PostD(ps2_engine *e, const ps2e_dcmd *c)
{
	ps2e_u32 head = e->dcmd_head, tries;
	for (tries = 0; head - e->dcmd_tail >= PS2E_DCMDS; tries++)
	{
		e->st.dcmd_full++;
		if (tries >= 200 || !PS2E_YieldHook) return 0;
		Yield();
	}
	e->dcmds[head % PS2E_DCMDS] = *c;
	PS2E_FENCE();
	e->dcmd_head = head + 1;
	return 1;
}

static ps2e_u32 NewEpoch(ps2_engine *e)
{
	e->epoch_seq++;
	if (!e->epoch_seq) e->epoch_seq++;
	e->want_epoch = e->epoch_seq;
	return e->epoch_seq;
}

// The same bound PS2_MusicSeek checks first; the length of an opened song never changes.
static int SeekOk(const ps2_engine *e, ps2e_u32 ms)
{
	ps2e_u32 length = PS2_MusicLength(e->g_song);
	return e->g_song && !(length && ms > length);
}

// The decoder runs ahead of the listener: it may have reached the end of the song while the last blocks are still
// unplayed. A seek then continues playback, exactly as seeking a still-playing song did before.
static int Ended(const ps2_engine *e)
{
	return e->music_end_epoch == e->epoch_seq || e->music_err_epoch == e->epoch_seq
		|| e->d_idle_epoch == e->epoch_seq || e->g_carry_epoch == e->epoch_seq;
}

int PS2E_MusicPlaying(const ps2_engine *e) { return e->g_song && e->g_playing && !Ended(e); }

// A seek keeps an ended song ended (the old PS2_MusicSeek did not restart it); returns whether playback continues.
static int SeekEpoch(ps2_engine *e, ps2e_u32 *epoch)
{
	int playing = PS2E_MusicPlaying(e), ended = e->g_playing && !playing;
	*epoch = NewEpoch(e);
	if (ended) e->g_carry_epoch = *epoch;
	return playing;
}

static void MakeD(ps2e_dcmd *c, ps2e_u32 op, ps2e_u32 epoch)
{
	memset(c, 0, sizeof *c); c->op = op; c->epoch = epoch; c->a = 0xffffffffu;
}

void PS2E_MusicOpen(ps2_engine *e, ps2_music *song)
{
	ps2e_dcmd c;
	MakeD(&c, PS2E_D_OPEN, NewEpoch(e)); c.song = song;
	e->g_song = song; e->g_playing = 0;
	e->music_paused = 0;
	PostD(e, &c);
}

void PS2E_MusicClose(ps2_engine *e)
{
	ps2e_dcmd c;
	MakeD(&c, PS2E_D_CLOSE, NewEpoch(e));
	e->g_song = NULL; e->g_playing = 0;
	e->music_paused = 0;
	if (PostD(e, &c)) e->close_posted++;
}

int PS2E_MusicClosed(const ps2_engine *e) { return e->close_ack == e->close_posted; }

ps2e_u32 PS2E_MusicPlay(ps2_engine *e, int loop)
{
	ps2e_dcmd c;
	ps2e_u32 epoch = NewEpoch(e);
	MakeD(&c, PS2E_D_PLAY, epoch); c.loop = loop;
	e->g_playing = 1;
	e->music_paused = 0;
	PostD(e, &c);
	return epoch;
}

ps2e_u32 PS2E_MusicStop(ps2_engine *e)
{
	ps2e_dcmd c;
	ps2e_u32 epoch = NewEpoch(e);
	MakeD(&c, PS2E_D_STOP, epoch);
	e->g_playing = 0;
	e->music_paused = 0;
	PostD(e, &c);
	return epoch;
}

ps2e_u32 PS2E_MusicSeek(ps2_engine *e, ps2e_u32 ms)
{
	ps2e_dcmd c;
	ps2e_u32 epoch;
	int playing;
	if (!SeekOk(e, ms)) return 0;
	playing = SeekEpoch(e, &epoch);
	MakeD(&c, PS2E_D_SEEK, epoch); c.a = ms; c.loop = playing;
	PostD(e, &c);
	return epoch;
}

void PS2E_MusicLoopPoint(ps2_engine *e, ps2e_u32 ms)
{
	ps2e_dcmd c;
	MakeD(&c, PS2E_D_LOOP, e->epoch_seq); c.a = ms;
	PostD(e, &c);
}

ps2e_u32 PS2E_MusicSpeed(ps2_engine *e, float speed, ps2e_u32 resync_ms)
{
	ps2e_dcmd c;
	ps2e_u32 epoch;
	int playing;
	if (!(speed >= 0.25f && speed <= 4.0f)) return 0;
	playing = SeekEpoch(e, &epoch);
	MakeD(&c, PS2E_D_SPEED, epoch); c.f = speed; c.a = SeekOk(e, resync_ms) ? resync_ms : 0xffffffffu; c.loop = playing;
	PostD(e, &c);
	return epoch;
}

// ---- D side -------------------------------------------------------------------------------------------------

static void RunD(ps2_engine *e, const ps2e_dcmd *c)
{
	if (c->op != PS2E_D_LOOP) e->d_hash = 0;   // every transport command starts a new epoch
	switch (c->op)
	{
		case PS2E_D_OPEN:
			e->d_song = c->song; e->d_playing = 0; e->d_epoch = c->epoch; break;
		case PS2E_D_CLOSE:
			e->d_song = NULL; e->d_playing = 0; e->d_epoch = c->epoch;
			PS2E_FENCE();
			e->close_ack++;
			break;
		case PS2E_D_PLAY:
			e->d_epoch = c->epoch;
			e->d_playing = e->d_song && PS2_MusicPlay(e->d_song, c->loop);
			if (!e->d_playing) e->music_err_epoch = c->epoch;
			break;
		case PS2E_D_STOP:
			e->d_epoch = c->epoch;
			if (e->d_song) PS2_MusicStop(e->d_song);
			e->d_playing = 0;
			break;
		case PS2E_D_SEEK:
			e->d_epoch = c->epoch;
			if (e->d_song && PS2_MusicSeek(e->d_song, c->a) && c->loop) { PS2_MusicRevive(e->d_song); e->d_playing = 1; }
			if (!e->d_playing) e->d_idle_epoch = c->epoch; // a seek does not restart a song that ended or was stopped
			break;
		case PS2E_D_LOOP:
			if (e->d_song) PS2_MusicSetLoop(e->d_song, c->a);
			break;
		case PS2E_D_SPEED:
			e->d_epoch = c->epoch;
			if (e->d_song)
			{
				PS2_MusicSpeed(e->d_song, c->f);
				if (c->a != 0xffffffffu && PS2_MusicSeek(e->d_song, c->a) && c->loop) { PS2_MusicRevive(e->d_song); e->d_playing = 1; }
			}
			if (!e->d_playing) e->d_idle_epoch = c->epoch;
			break;
	}
}

static ps2e_u32 HashPcm(ps2e_u32 h, const int16_t *p, size_t n)
{
	while (n--) h = (h << 5 | h >> 27) ^ (unsigned short)*p++;
	return h;
}

unsigned PS2E_SlotsFilled(const ps2_engine *e) { return e->slot_head - e->slot_tail; }

int PS2E_DecodeStep(ps2_engine *e)
{
	int work = 0;
	ps2e_slot *slot;
	size_t n;
	ps2e_u32 tail, head;
	while ((tail = e->dcmd_tail) != e->dcmd_head)
	{
		PS2E_FENCE();
		RunD(e, &e->dcmds[tail % PS2E_DCMDS]);
		PS2E_FENCE();
		e->dcmd_tail = tail + 1;
		work = 1;
	}
	if (!e->d_song || !e->d_playing) return work;
	head = e->slot_head;
	if (head - e->slot_tail >= PS2E_SLOTS) return work;
	slot = &e->slots[head % PS2E_SLOTS];
	// PS2-317: muted for PS2E_MUTE_STEPS decode steps in a row: an Ogg/MP3 song is not decoded, its position advances in silent blocks (kept
	// only PS2E_MUTE_AHEAD blocks ahead, so that unmuting is heard within ~93 ms); PS2_MusicRender resynchronises the decoder by a seek
	if (e->music_gain == 0) { if (e->d_mute_steps < 0xffffu) e->d_mute_steps++; } else e->d_mute_steps = 0;
	if (e->d_mute_steps >= PS2E_MUTE_STEPS && PS2_MusicCanSkip(e->d_song))
	{
		if (head - e->slot_tail >= PS2E_MUTE_AHEAD) return work;
		slot->epoch = e->d_epoch;
		slot->pos0_ms = PS2_MusicPosition(e->d_song);
		n = PS2_MusicSkip(e->d_song, PS2E_SLOT_FRAMES);
		memset(slot->pcm, 0, sizeof slot->pcm);
		e->st.dec_steps_silent++;
	}
	else
	{
	slot->epoch = e->d_epoch;
	slot->pos0_ms = PS2_MusicPosition(e->d_song);
	n = PS2_MusicRender(e->d_song, slot->pcm, PS2E_SLOT_FRAMES);
	}
	slot->flags = 0;
	if (!PS2_MusicPlaying(e->d_song)) { slot->flags |= PS2E_SLOT_END; e->d_playing = 0; }
	if (PS2_MusicError(e->d_song)) slot->flags |= PS2E_SLOT_ERROR;
	slot->pos1_ms = PS2_MusicPosition(e->d_song);
	slot->frames = (ps2e_u32)n;
	e->st.music_frames_dec += (ps2e_u32)n;
	if (e->diag_hash)
	{
		e->d_hash = HashPcm(e->d_hash, slot->pcm, n * 2);
		slot->hash_after = e->d_hash; e->st.music_hash_dec = e->d_hash;
	}
	e->st.dec_slots++;
	PS2E_FENCE();
	e->slot_head = head + 1;
	return 1;
}

// ---- M side -------------------------------------------------------------------------------------------------

static void Publish(ps2_engine *e)
{
	unsigned i;
	for (i = 0; i < PS2_AUDIO_CHANNELS; i++)
	{
		const ps2_voice *v = &e->mixer.voices[i];
		e->live_sample[i] = v->sample;
		e->live_handle[i] = v->sample ? v->generation * PS2_AUDIO_CHANNELS + i + 1 : 0;
	}
	PS2E_FENCE();
	e->consumed = e->m_tail;
}

static const ps2e_slot *PickSlot(ps2_engine *e);

void PS2E_Process(ps2_engine *e)
{
	ps2e_u32 head = e->cmd_head;
	PS2E_FENCE();
	while (e->m_tail != head)
	{
		const ps2e_cmd *c = &e->cmds[e->m_tail % PS2E_CMDS];
		switch (c->op)
		{
			case PS2E_C_START:
				if (PS2_MixerStart(&e->mixer, c->sample, c->handle & (PS2_AUDIO_CHANNELS - 1), c->volume, c->pan, c->pitch) != c->handle)
					e->st.handle_mismatch++;
				break;
			case PS2E_C_STOP: PS2_MixerStop(&e->mixer, c->handle); break;
			case PS2E_C_PARAMS: PS2_MixerParams(&e->mixer, c->handle, c->volume, c->pan, c->pitch); break;
			case PS2E_C_FORGET: PS2_MixerForget(&e->mixer, c->sample); break;
		}
		e->m_tail++;
	}
	PickSlot(e); // free the ring of stale blocks now, so the decoder can refill it before the next block is due
	Publish(e);
}

// Slots of any other epoch are dropped; returns the oldest slot of the wanted epoch.
static const ps2e_slot *PickSlot(ps2_engine *e)
{
	ps2e_u32 want = e->want_epoch;
	if (want != e->m_epoch_seen) { e->m_epoch_seen = want; e->m_flowing = 0; e->m_hash = 0; }
	while (e->slot_tail != e->slot_head)
	{
		const ps2e_slot *s;
		PS2E_FENCE();
		s = &e->slots[e->slot_tail % PS2E_SLOTS];
		if (s->epoch == want) return s;
		e->slot_tail++;
	}
	return NULL;
}

int PS2E_Active(ps2_engine *e)
{
	unsigned i;
	for (i = 0; i < PS2_AUDIO_CHANNELS; i++) if (e->mixer.voices[i].sample) return 1;
	if (e->music_paused) return 0;
	if (PickSlot(e)) return 1;
	// music is flowing but the decoder is late: keep the stream (and the clock) running with a silent music block
	return e->m_flowing && e->music_end_epoch != e->m_epoch_seen && e->music_err_epoch != e->m_epoch_seen;
}

int PS2E_Render(ps2_engine *e, int16_t *out)
{
	const ps2e_slot *slot;
	unsigned i;
	int audible = 0;
	e->mixer.volume = e->sfx_volume > 31 ? 31 : e->sfx_volume;
	slot = PickSlot(e);
	if (e->music_paused) slot = NULL;
	for (i = 0; i < PS2_AUDIO_CHANNELS && !audible; i++) audible = e->mixer.voices[i].sample != NULL;
	PS2_MixerRender(&e->mixer, out, slot ? slot->pcm : NULL, PS2_AUDIO_BLOCK, e->music_gain);
	e->st.blocks++;
	if (slot)
	{
		audible = 1;
		e->st.music_frames_cons += slot->frames;
		if (e->diag_hash)
		{
			e->m_hash = HashPcm(e->m_hash, slot->pcm, (size_t)slot->frames * 2);
			e->st.music_hash_cons = e->m_hash;
			if (e->m_hash != slot->hash_after) e->st.hash_errors++;
		}
		e->music_pos_ms = slot->pos0_ms; e->music_pos_epoch = slot->epoch;
		if (slot->flags & PS2E_SLOT_ERROR) e->music_err_epoch = slot->epoch;
		if (slot->flags & PS2E_SLOT_END) e->music_end_epoch = slot->epoch;
		e->m_flowing = 1;
		PS2E_FENCE();
		e->slot_tail++;
	}
	else if (!e->music_paused && e->m_flowing && e->music_end_epoch != e->m_epoch_seen
		&& e->music_err_epoch != e->m_epoch_seen)
		e->st.music_starved++; // D was expected to deliver a block and did not
	Publish(e);
	return audible;
}

// ---- accounting ---------------------------------------------------------------------------------------------

void PS2E_AcctBegin(ps2e_acct *a, ps2_engine *e, ps2e_u64 now, ps2e_u64 hz, int queued)
{
	ps2e_stats *st = &e->st;
	if (a->active_prev && a->t_prev)
	{
		ps2e_u32 elapsed_ms = (ps2e_u32)((now - a->t_prev) * 1000 / hz);
		ps2e_u32 buffered_ms = (ps2e_u32)((ps2e_u32)a->prev_after * 1000u / 88200u);
		ps2e_u32 queued_ms = (ps2e_u32)((ps2e_u32)(queued < 0 ? 0 : queued) * 1000u / 88200u);
		if (elapsed_ms > st->max_interval_ms) st->max_interval_ms = elapsed_ms;
		if (queued_ms < st->min_queue_ms) st->min_queue_ms = queued_ms;
		// Nothing but our writes adds to the queue, so a queue that is empty, or larger than what the previous refill left
		// (the IOP ring ran dry and audsrv topped it up with silence), or more time passed than the previous refill covered,
		// means the stream underran. The slack covers the 940-byte granularity in which the IOP consumes the ring and the
		// error of the estimate of what was left (12 ms, 1880 bytes).
		if (queued <= 0 || queued > a->prev_after + 1880 || elapsed_ms > buffered_ms + 12)
		{
			ps2e_u32 gap = elapsed_ms > buffered_ms ? elapsed_ms - buffered_ms : 1;
			st->empty_obs++;
			st->underruns++; st->gap_total_ms += gap;
			if (gap > st->gap_max_ms) st->gap_max_ms = gap;
		}
	}
	st->pumps++;
	a->t_begin = now; a->q_begin = queued < 0 ? 0 : queued;
}

void PS2E_AcctEnd(ps2e_acct *a, ps2e_u64 now, ps2e_u64 hz, int sent_bytes, int active_after)
{
	// audsrv consumes 88200 bytes per second (22050 Hz, 16 bit, stereo) while the queue is not empty
	ps2e_s64 used = (ps2e_s64)((now - a->t_begin) * 88200 / hz);
	ps2e_s64 after = (ps2e_s64)a->q_begin + sent_bytes - used;
	a->prev_after = after < 0 ? 0 : (int)after;
	a->t_prev = now; a->active_prev = active_after;
}
