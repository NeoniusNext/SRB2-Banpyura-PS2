// No clocks, allocation, syscalls, polling or GS register access here.
// CPU = foreground elapsed minus the union of explicitly instrumented waits.
// GPU~ = observed submit-to-FINISH span, an upper estimate of GS queue activity.
#define PS2_ACTIVITY_OWNER
#include "ps2_activity.h"
#include <stdint.h>
#include <string.h>

volatile unsigned ps2_activity_enabled;
static struct
{
	uint64_t idle, gpu;
	unsigned origin, idle_start, gpu_start;
	unsigned generation, idle_depth, gpu_open, gpu_seen, gpu_serial, gpu_window_unknown;
	int main_thread;
} A;

static unsigned next_generation(unsigned generation)
{
	generation++;
	return generation ? generation : 1;
}

static void reset(unsigned now)
{
	const unsigned generation = next_generation(A.generation);
	const int main_thread = A.main_thread;
	const unsigned gpu_serial = A.gpu_serial;
	memset(&A, 0, sizeof A);
	A.origin = now;
	A.generation = generation;
	A.main_thread = main_thread;
	A.gpu_serial = gpu_serial;
	A.gpu_window_unknown = 1;
}

void PS2Activity_Configure(unsigned on, int main_thread, unsigned now)
{
	ps2_activity_enabled = 0;
	A.main_thread = main_thread;
	reset(now);
	ps2_activity_enabled = on != 0;
}

void PS2Activity_RendererReset(unsigned now)
{
	if (ps2_activity_enabled)
		reset(now); // old wait tokens cannot close a scope in the new lifetime
}

unsigned PS2Activity_IdleBegin(int thread, unsigned now)
{
	if (!ps2_activity_enabled || thread != A.main_thread)
		return 0;
	if (!A.idle_depth++)
		A.idle_start = now;
	return A.generation;
}

void PS2Activity_IdleEnd(unsigned token, unsigned now)
{
	if (!ps2_activity_enabled || !token || token != A.generation || !A.idle_depth)
		return;
	if (!--A.idle_depth)
		A.idle += (unsigned)(now - A.idle_start);
}

void PS2Activity_GpuSubmit(unsigned now)
{
	if (!ps2_activity_enabled || A.gpu_open)
		return;
	A.gpu_start = now;
	A.gpu_open = 1;
	A.gpu_serial = next_generation(A.gpu_serial);
}

unsigned PS2Activity_GpuToken(void)
{
	return ps2_activity_enabled && A.gpu_open ? A.gpu_serial : 0;
}

void PS2Activity_GpuFinish(unsigned token, unsigned now)
{
	if (!ps2_activity_enabled || !A.gpu_open || !token || token != A.gpu_serial)
		return;
	A.gpu += (unsigned)(now - A.gpu_start);
	A.gpu_open = 0;
	A.gpu_seen = 1;
}

static unsigned percentage(uint64_t part, unsigned whole)
{
	if (!whole)
		return 0;
	if (part > whole)
		part = whole;
	return (unsigned)((part * 100u + whole / 2u) / whole);
}

void PS2Activity_Sample(unsigned now, ps2activity_sample_t *out)
{
	uint64_t idle, gpu;
	unsigned elapsed;
	memset(out, 0, sizeof *out);
	if (!ps2_activity_enabled)
		return;
	elapsed = (unsigned)(now - A.origin);
	idle = A.idle + (A.idle_depth ? (unsigned)(now - A.idle_start) : 0u);
	gpu = A.gpu + (A.gpu_open ? (unsigned)(now - A.gpu_start) : 0u);
	if (idle > elapsed)
		idle = elapsed;
	if (gpu > elapsed)
		gpu = elapsed;
	out->elapsed = elapsed;
	out->idle = (unsigned)idle;
	out->gpu_queue = (unsigned)gpu;
	out->cpu_valid = elapsed != 0;
	out->gpu_valid = elapsed != 0 && A.gpu_seen && !A.gpu_window_unknown;
	out->cpu_percent = percentage(elapsed - idle, elapsed);
	out->gpu_percent = percentage(gpu, elapsed);
	A.origin = now;
	A.idle = A.gpu = 0;
	A.gpu_window_unknown = !A.gpu_seen;
	if (A.idle_depth)
		A.idle_start = now;
	if (A.gpu_open)
		A.gpu_start = now;
}
