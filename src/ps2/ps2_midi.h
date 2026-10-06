#ifndef PS2_MIDI_H
#define PS2_MIDI_H
#include "ps2_audio.h"
typedef struct ps2_midi ps2_midi;
ps2_midi *PS2_MIDIOpen(const ps2_audio_input *in);
void PS2_MIDIClose(ps2_midi *m);
int PS2_MIDISeek(ps2_midi *m, uint32_t ms);
size_t PS2_MIDIRead(ps2_midi *m, int16_t *out, size_t frames);
uint32_t PS2_MIDILength(const ps2_midi *m);
int PS2_MIDIError(const ps2_midi *m);
#endif
