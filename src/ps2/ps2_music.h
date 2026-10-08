#ifndef PS2_MUSIC_H
#define PS2_MUSIC_H
#include "ps2_audio.h"
typedef enum { PS2_MUSIC_NONE, PS2_MUSIC_WAV, PS2_MUSIC_OGG, PS2_MUSIC_MP3, PS2_MUSIC_MIDI } ps2_music_type;
typedef struct ps2_music ps2_music;
ps2_music *PS2_MusicOpen(const ps2_audio_input *in);
void PS2_MusicClose(ps2_music *m);
ps2_music_type PS2_MusicType(const ps2_music *m);
int PS2_MusicPlay(ps2_music *m, int loop);
// Marks a song that ran into its end ahead of the listener as playing again (used after a seek; no seek happens here).
void PS2_MusicRevive(ps2_music *m);
void PS2_MusicStop(ps2_music *m);
void PS2_MusicPause(ps2_music *m, int paused);
int PS2_MusicPlaying(const ps2_music *m);
int PS2_MusicPaused(const ps2_music *m);
int PS2_MusicError(const ps2_music *m);
int PS2_MusicSeek(ps2_music *m, uint32_t ms);
uint32_t PS2_MusicPosition(const ps2_music *m);
uint32_t PS2_MusicLength(const ps2_music *m);
unsigned PS2_MusicChannels(const ps2_music *m); // channels of the source (1 or 2); PS2_MusicRender always upmixes mono to two
int PS2_MusicSetLoop(ps2_music *m, uint32_t ms);
uint32_t PS2_MusicLoop(const ps2_music *m);
int PS2_MusicSpeed(ps2_music *m, float speed);
// Always clears the unused portion. Returns real generated frames.
size_t PS2_MusicRender(ps2_music *m, int16_t *out, size_t frames);
// PS2-317: while the music is muted (gain 0) a compressed song need not be decoded: the position advances as if it had been (end and loop
// included), the PCM is the caller's silence, and the decoder is re-synchronised by a seek at the next PS2_MusicRender.
// PS2_MusicCanSkip: playing, not paused, Ogg/MP3 with a known length at normal speed, and `frames` more frames end well before the end of the song.
// PS2_MusicSkip moves the position by `frames` without decoding (returns frames, or 0 if it cannot); the next PS2_MusicRender seeks the decoder there.
int PS2_MusicCanSkip(const ps2_music *m, size_t frames);
size_t PS2_MusicSkip(ps2_music *m, size_t frames);
#endif
