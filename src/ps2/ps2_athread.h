// Audio engine for the PS2 port: control plane between the game thread, the mixer thread and the music
// decoder thread (docs/GATES/g1/opt2-A.md). Portable C: the EE thread glue lives in i_sound.c, the host tests
// drive the very same functions single-threaded or with real threads.
//
//   game thread  --cmds-->  mixer thread (M)  --PCM-->  audsrv queue (IOP)
//   game thread  --dcmds->  decoder thread (D) --slots-> M
//
// * Every shared structure is a single-producer/single-consumer ring or a word written by one side only; there
//   are no locks and no thread ever waits for another one on the hot path (the game thread waits only to
//   release a sample or a song, bounded, never while mixing).
// * M owns the mixer. The game thread keeps a shadow of each voice (handle, sequence number of its start, stop
//   flag) so that start/stop/IsPlaying answer immediately, exactly like the old direct calls did.
// * D owns the open ps2_music. It decodes ahead into 512-frame slots (one mixer block each). Every slot carries
//   the epoch it was produced for; any transport command (play/stop/seek) bumps the epoch, M drops slots of
//   other epochs, so a seek or a song change never plays stale audio and never needs a lock.
#ifndef PS2_ATHREAD_H
#define PS2_ATHREAD_H
#include "ps2_audio.h"
#include "ps2_music.h"

// newlib on the EE defines uint32_t as unsigned long; the engine uses plain int types (same width, printf-friendly)
typedef unsigned int ps2e_u32;
typedef int ps2e_s32;
typedef unsigned long long ps2e_u64;
typedef long long ps2e_s64;

#define PS2E_CMDS 512        // game -> M ring (power of two)
#define PS2E_DCMDS 64        // game -> D ring (power of two)
#define PS2E_SLOTS 24        // decoded music blocks: 24 * 512 frames = 557 ms
#define PS2E_SLOT_FRAMES PS2_AUDIO_BLOCK

#if defined(_MSC_VER)
#include <intrin.h>
#define PS2E_FENCE() _ReadWriteBarrier()
#define PS2E_ALIGN16 __declspec(align(16))
#else
#define PS2E_FENCE() __sync_synchronize()
#define PS2E_ALIGN16 __attribute__((aligned(16)))
#endif

enum { PS2E_C_START = 1, PS2E_C_STOP, PS2E_C_PARAMS, PS2E_C_FORGET };
enum { PS2E_D_OPEN = 1, PS2E_D_CLOSE, PS2E_D_PLAY, PS2E_D_STOP, PS2E_D_SEEK, PS2E_D_LOOP, PS2E_D_SPEED };
enum { PS2E_SLOT_END = 1, PS2E_SLOT_ERROR = 2 };

typedef struct
{
	ps2e_u32 op, volume, pan, pitch;
	ps2e_s32 handle;
	const ps2_sample *sample;
} ps2e_cmd;

typedef struct
{
	ps2e_u32 op, epoch, a;
	float f;
	ps2_music *song;
	int loop;                         // PLAY: loop; SEEK/SPEED: the song is still playing as far as the listener is concerned
} ps2e_dcmd;

typedef struct
{
	ps2e_u32 epoch, frames, flags, pos0_ms, pos1_ms, hash_after;   // hash_after: running hash of this epoch's PCM (diagnostic)
	PS2E_ALIGN16 int16_t pcm[PS2E_SLOT_FRAMES * 2];
} ps2e_slot;

// Game-thread shadow of a voice: what I_SoundIsPlaying answers before M has caught up.
typedef struct
{
	ps2e_s32 handle;       // handle given out by the last start, -1 none
	ps2e_u32 generation;
	ps2e_u32 start_seq;   // sequence number of that start command
	ps2e_u32 volume, pan, pitch;
	uint8_t stopped;
} ps2e_shadow;

typedef struct
{
	// counters (written by one side; read for the ASTAT line). Underrun = the audsrv queue was found empty
	// while audio was being produced; gap = how long that empty period lasted (measured, not estimated).
	volatile ps2e_u32 pumps, underruns, empty_obs, gap_max_ms, gap_total_ms, max_interval_ms, rpc_errors;
	volatile ps2e_u32 blocks, bytes_sent, music_starved, handle_mismatch, dec_slots, dec_max_us;
	volatile ps2e_u32 music_frames_dec, music_frames_cons, ring_full_stalls;
	volatile ps2e_u32 min_queue_ms, queue_cap, cmd_full, dcmd_full;
	volatile ps2e_u32 music_hash_dec, music_hash_cons, hash_errors;   // per-block check of the decoder -> mixer transport
	volatile ps2e_u32 mix_cycles_max, mix_cycles_blocks;       // COP0 cycles of one PS2E_Render (EE only)
	volatile ps2e_u64 mix_cycles_total;
	volatile ps2e_u32 main_gap_max_ms, main_gaps_over_100ms;   // game thread: longest time between two I_UpdateSound calls
} ps2e_stats;

typedef struct ps2_engine
{
	// ---- M-owned
	ps2_mixer mixer;
	ps2e_u32 m_tail;                  // commands consumed so far (published in `consumed`)
	// ---- game -> M
	ps2e_cmd cmds[PS2E_CMDS];
	volatile ps2e_u32 cmd_head;
	volatile ps2e_u32 consumed;       // == M's tail, published after the voice state it caused
	volatile ps2e_u32 sfx_volume, music_gain, music_paused, want_epoch;
	// ---- M -> game
	volatile ps2e_u32 live_handle[PS2_AUDIO_CHANNELS];                 // handle + 1 of a playing voice
	const ps2_sample *volatile live_sample[PS2_AUDIO_CHANNELS];
	volatile ps2e_u32 music_pos_ms, music_pos_epoch, music_end_epoch, music_err_epoch, d_idle_epoch;
	// ---- game-thread private
	ps2e_shadow shadow[PS2_AUDIO_CHANNELS];
	ps2e_u32 cmd_seq, epoch_seq;
	ps2_music *g_song;                // the opened song as the game thread knows it (immutable fields only)
	int g_playing;                    // the game asked for playback (cleared by stop/open/close)
	ps2e_u32 g_carry_epoch;           // epoch in which a seek found the song already ended (it stays ended)
	// ---- game -> D
	ps2e_dcmd dcmds[PS2E_DCMDS];
	volatile ps2e_u32 dcmd_head, dcmd_tail;
	volatile ps2e_u32 close_ack;      // number of CLOSE commands D has completed
	ps2e_u32 close_posted;
	// ---- D -> M
	ps2e_slot slots[PS2E_SLOTS];
	volatile ps2e_u32 slot_head, slot_tail;
	// ---- D-owned
	ps2_music *d_song;
	ps2e_u32 d_epoch, d_hash;
	int d_playing;
	// ---- M-owned music state
	int m_flowing;                    // at least one slot of the current epoch was consumed
	ps2e_u32 m_epoch_seen, m_hash;
	// ---- accounting
	ps2e_stats st;
	volatile int quit;
	int diag_hash;                    // maintain music_hash_* (diagnostic, costs ~10 cycles/byte)
} ps2_engine;

// Called while a ring is full / a handshake is pending; the platform sleeps briefly (default: nothing).
extern void (*PS2E_YieldHook)(void);
void PS2E_Init(ps2_engine *e);

// ---- game thread --------------------------------------------------------------------------------------------
// Returns the voice handle (same value PS2_MixerStart would have returned) or -1; *seq receives the command number.
int PS2E_Start(ps2_engine *e, const ps2_sample *s, int channel, unsigned volume, unsigned pan, unsigned pitch, ps2e_u32 *seq);
void PS2E_Stop(ps2_engine *e, int handle);
void PS2E_Params(ps2_engine *e, int handle, unsigned volume, unsigned pan, unsigned pitch);
int PS2E_Playing(const ps2_engine *e, int handle);
ps2e_u32 PS2E_Forget(ps2_engine *e, const ps2_sample *s);       // returns the sequence number to wait for
int PS2E_Consumed(const ps2_engine *e, ps2e_u32 seq);           // M has executed command `seq`
// A sample may be freed only when this is false: a start of it is still queued, or a voice plays it.
int PS2E_SampleBusy(const ps2_engine *e, const ps2_sample *s, ps2e_u32 last_start_seq);
// Music transport (the song was opened by the caller and is handed over to D by Open until Close completes).
void PS2E_MusicOpen(ps2_engine *e, ps2_music *song);
void PS2E_MusicClose(ps2_engine *e);                            // poll PS2E_MusicClosed afterwards
int PS2E_MusicClosed(const ps2_engine *e);
int PS2E_MusicPlaying(const ps2_engine *e);                      // playback was requested and the end was not reached
ps2e_u32 PS2E_MusicPlay(ps2_engine *e, int loop);               // returns the new epoch
ps2e_u32 PS2E_MusicStop(ps2_engine *e);
ps2e_u32 PS2E_MusicSeek(ps2_engine *e, ps2e_u32 ms);              // 0 = out of range (the old PS2_MusicSeek failure), nothing changes
void PS2E_MusicLoopPoint(ps2_engine *e, ps2e_u32 ms);
ps2e_u32 PS2E_MusicSpeed(ps2_engine *e, float speed, ps2e_u32 resync_ms);   // 0 = speed out of range; seeks to resync_ms (the audible position)

// ---- M side -------------------------------------------------------------------------------------------------
void PS2E_Process(ps2_engine *e);                               // execute queued commands
int PS2E_Active(ps2_engine *e);                                 // a voice or a music slot would be audible now
// Renders exactly one block (PS2_AUDIO_BLOCK frames). Returns 1 when something audible contributed.
int PS2E_Render(ps2_engine *e, int16_t *out);

// ---- D side -------------------------------------------------------------------------------------------------
// Executes queued music commands and, when there is room, decodes one slot. Returns 1 if it did any work.
int PS2E_DecodeStep(ps2_engine *e);
unsigned PS2E_SlotsFilled(const ps2_engine *e);

// ---- accounting (called by the code that talks to audsrv) --------------------------------------------------
// Underrun = the audsrv queue was found empty at the start of a pump although the previous pump left audio
// queued/pending (active_after); gap = elapsed - what the previous pump had queued. Times in ticks of `hz`.
typedef struct { ps2e_u64 t_prev, t_begin; int prev_after, q_begin, active_prev; } ps2e_acct;
void PS2E_AcctBegin(ps2e_acct *a, ps2_engine *e, ps2e_u64 now, ps2e_u64 hz, int queued);
void PS2E_AcctEnd(ps2e_acct *a, ps2e_u64 now, ps2e_u64 hz, int sent_bytes, int active_after);
#endif
