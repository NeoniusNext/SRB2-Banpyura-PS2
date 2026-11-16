/* Production counter source is included unchanged, with deterministic clocks. */
#include "ps2_activity.h"
#include "ps2_activity.c"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks, failures, rng = 0x7631429u;
#define CHECK(c) do { checks++; if (!(c)) { if (failures < 8) printf("ACTIVITY FAIL line=%d %s\n", __LINE__, #c); failures++; } } while (0)
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void configure_known(unsigned now)
{
	ps2activity_sample_t sample;
	PS2Activity_Configure(1, 7, now);
	PS2Activity_GpuSubmit(now);
	PS2Activity_GpuFinish(PS2Activity_GpuToken(), now);
	PS2Activity_Sample(now, &sample); // deterministic known-idle boundary for window arithmetic
}

static void test_counters(void)
{
	ps2activity_sample_t s;
	unsigned outer, inner, old, current, clock = 0;
	PS2Activity_Configure(0, 7, 0);
	unsigned char before[sizeof A];
	memcpy(before, &A, sizeof A);
	CHECK(!PS2_ACTIVITY_IDLE_BEGIN(7, ++clock) && clock == 0);
	PS2_ACTIVITY_IDLE_END(0, ++clock);
	CHECK(clock == 0);
	PS2Activity_IdleBegin(7, 20); PS2Activity_IdleEnd(1, 50);
	PS2Activity_GpuSubmit(10); PS2Activity_GpuFinish(1, 70); PS2Activity_RendererReset(80);
	PS2Activity_Sample(100, &s);
	CHECK(!memcmp(before, &A, sizeof A) && !s.cpu_valid && !s.gpu_valid);
	configure_known(0);
	outer = PS2Activity_IdleBegin(7, 10);
	inner = PS2Activity_IdleBegin(7, 20);
	CHECK(!PS2Activity_IdleBegin(8, 30));
	PS2Activity_IdleEnd(inner, 60); PS2Activity_IdleEnd(outer, 70);
	PS2Activity_GpuSubmit(20); old = PS2Activity_GpuToken();
	PS2Activity_GpuSubmit(30); CHECK(PS2Activity_GpuToken() == old);
	PS2Activity_GpuFinish(old, 80); PS2Activity_Sample(100, &s);
	CHECK(s.elapsed == 100 && s.idle == 60 && s.cpu_percent == 40 && s.gpu_percent == 60 && s.gpu_valid);
	PS2Activity_Configure(1, 7, 0);
	outer = PS2Activity_IdleBegin(7, 10); PS2Activity_GpuSubmit(10);
	PS2Activity_Sample(50, &s); CHECK(s.idle == 40 && s.cpu_percent == 20 && !s.gpu_valid);
	PS2Activity_IdleEnd(outer, 70); PS2Activity_GpuFinish(PS2Activity_GpuToken(), 60);
	PS2Activity_Sample(100, &s); CHECK(s.idle == 20 && s.cpu_percent == 60 && s.gpu_percent == 20 && !s.gpu_valid);
	PS2Activity_Sample(150, &s); CHECK(s.gpu_valid && s.gpu_percent == 0);
	configure_known(0);
	PS2Activity_GpuSubmit(10); old = PS2Activity_GpuToken();
	PS2Activity_GpuFinish(old, 20); PS2Activity_GpuSubmit(21); current = PS2Activity_GpuToken();
	CHECK(current && current != old);
	PS2Activity_GpuFinish(old, 30); CHECK(PS2Activity_GpuToken() == current);
	PS2Activity_GpuFinish(current, 40); PS2Activity_Sample(100, &s); CHECK(s.gpu_percent == 29);
	configure_known(0);
	outer = PS2Activity_IdleBegin(7, 10); PS2Activity_GpuSubmit(10); old = PS2Activity_GpuToken();
	PS2Activity_RendererReset(30);
	inner = PS2Activity_IdleBegin(7, 35); PS2Activity_GpuSubmit(40); current = PS2Activity_GpuToken();
	PS2Activity_IdleEnd(outer, 70); PS2Activity_GpuFinish(old, 70);
	CHECK(PS2Activity_GpuToken() == current);
	PS2Activity_IdleEnd(inner, 80); PS2Activity_GpuFinish(current, 90); PS2Activity_Sample(130, &s);
	CHECK(s.cpu_percent == 55 && s.gpu_percent == 50 && !A.idle_depth);
	configure_known(~0u - 49);
	outer = PS2Activity_IdleBegin(7, ~0u - 39); PS2Activity_IdleEnd(outer, 20);
	PS2Activity_GpuSubmit(~0u - 29); PS2Activity_GpuFinish(PS2Activity_GpuToken(), 30); PS2Activity_Sample(50, &s);
	CHECK(s.elapsed == 100 && s.cpu_percent == 40 && s.gpu_percent == 60);
	for (unsigned n = 0; n <= 100; n++)
	{
		configure_known(0);
		outer = PS2Activity_IdleBegin(7, 0); PS2Activity_IdleEnd(outer, n);
		PS2Activity_GpuSubmit(0); PS2Activity_GpuFinish(PS2Activity_GpuToken(), n); PS2Activity_Sample(100, &s);
		CHECK(s.cpu_percent == 100 - n && s.gpu_percent == n && s.gpu_valid);
	}
	configure_known(0);
	A.gpu_serial = ~0u - 1;
	PS2Activity_GpuSubmit(10); old = PS2Activity_GpuToken(); CHECK(old == ~0u);
	PS2Activity_GpuFinish(old, 20); PS2Activity_GpuSubmit(30); current = PS2Activity_GpuToken(); CHECK(current == 1);
	PS2Activity_GpuFinish(old, 40); CHECK(PS2Activity_GpuToken() == current);
	A.generation = ~0u; outer = PS2Activity_IdleBegin(7, 40);
	PS2Activity_RendererReset(50); inner = PS2Activity_IdleBegin(7, 60); CHECK(inner && inner != outer);
	PS2Activity_IdleEnd(outer, 70); CHECK(A.idle_depth == 1);
	PS2Activity_IdleEnd(inner, 80); CHECK(!A.idle_depth);
}

static unsigned reference_pct(unsigned part, unsigned whole)
{
	return whole ? (part * 100u + whole / 2) / whole : 0;
}

static void test_random_windows(void)
{
	for (unsigned trial = 0; trial < 300; trial++)
	{
		unsigned now = (~0u - 3000u) + rnd() % 6000, origin = now, last = now;
		unsigned stack[8], depth = 0, gpu = 0, gpu_token = 0, late = 0, seen = 1, idle_ticks = 0, gpu_ticks = 0;
		configure_known(now);
		for (unsigned event = 0; event < 1000; event++)
		{
			now += rnd() % 100;
			if (depth) idle_ticks += (unsigned)(now - last);
			if (gpu) gpu_ticks += (unsigned)(now - last);
			last = now;
			switch (rnd() % 6)
			{
				case 0: if (depth < 8) { stack[depth] = PS2Activity_IdleBegin(7, now); depth++; } break;
				case 1: if (depth) PS2Activity_IdleEnd(stack[--depth], now); break;
				case 2: PS2Activity_GpuSubmit(now); if (!gpu) { gpu = 1; gpu_token = PS2Activity_GpuToken(); } break;
				case 3: PS2Activity_GpuFinish(gpu_token, now); if (gpu) { late = gpu_token; gpu = 0; seen = 1; } break;
				case 4: CHECK(!PS2Activity_IdleBegin(9, now)); break;
				case 5: if (late && late != gpu_token) PS2Activity_GpuFinish(late, now); break;
			}
			if (event % 50 == 49)
			{
				ps2activity_sample_t s;
				PS2Activity_Sample(now, &s);
				unsigned elapsed = (unsigned)(now - origin);
				CHECK(s.cpu_percent == reference_pct(elapsed - idle_ticks, elapsed));
				CHECK(s.gpu_percent == reference_pct(gpu_ticks, elapsed) && s.gpu_valid == (seen && elapsed != 0));
				CHECK(s.cpu_percent <= 100 && s.gpu_percent <= 100);
				origin = now; idle_ticks = gpu_ticks = 0;
			}
		}
	}
}

#include "hud-fixture.inc"

int main(void)
{
	test_counters();
	test_random_windows();
	test_hud();
	printf("ACTIVITY COMPLETE checks=%u failed=%u\n", checks, failures);
	return failures != 0;
}
