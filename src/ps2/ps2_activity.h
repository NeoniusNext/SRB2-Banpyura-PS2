// Optional foreground-EE activity / observed GS queue-span meter.
// Callers supply CP0 Count samples. GPU updates, Configure/Reset and Sample must
// run with interrupts disabled; CPU wait scopes belong only to the main thread.
// Weak references keep standalone GS-driver fixtures independent of this module.
#ifndef PS2_ACTIVITY_H
#define PS2_ACTIVITY_H

#if defined(__GNUC__) && !defined(PS2_ACTIVITY_OWNER)
#define PS2ACT_OPTIONAL __attribute__((weak))
#else
#define PS2ACT_OPTIONAL
#endif

extern volatile unsigned ps2_activity_enabled PS2ACT_OPTIONAL;
#define PS2_ACTIVITY_ON ((&ps2_activity_enabled != 0) && ps2_activity_enabled)
#define PS2_ACTIVITY_IDLE_BEGIN(thread, clock) (PS2_ACTIVITY_ON ? PS2Activity_IdleBegin((thread), (clock)) : 0u)
#define PS2_ACTIVITY_IDLE_END(token, clock) do { if (token) PS2Activity_IdleEnd((token), (clock)); } while (0)

typedef struct
{
	unsigned cpu_percent, gpu_percent; // rounded integer 0..100
	unsigned cpu_valid, gpu_valid; // GPU waits for a known FINISH plus a wholly observed window
	unsigned elapsed, idle; // Count ticks in this sample window
	unsigned gpu_queue; // observed submit-to-FINISH union, includes feeding/observation gaps
} ps2activity_sample_t;

void PS2Activity_Configure(unsigned on, int main_thread, unsigned now) PS2ACT_OPTIONAL;
void PS2Activity_RendererReset(unsigned now) PS2ACT_OPTIONAL;
unsigned PS2Activity_IdleBegin(int thread, unsigned now) PS2ACT_OPTIONAL;
void PS2Activity_IdleEnd(unsigned token, unsigned now) PS2ACT_OPTIONAL;
void PS2Activity_GpuSubmit(unsigned now) PS2ACT_OPTIONAL;
unsigned PS2Activity_GpuToken(void) PS2ACT_OPTIONAL;
void PS2Activity_GpuFinish(unsigned token, unsigned now) PS2ACT_OPTIONAL;
void PS2Activity_Sample(unsigned now, ps2activity_sample_t *out) PS2ACT_OPTIONAL;

#endif
