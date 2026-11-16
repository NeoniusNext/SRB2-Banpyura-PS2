/* Host-only inputs around two separately extracted production planners. */
#ifndef PLAN_SCRATCH_OPT16_HOSTTEST_H
#define PLAN_SCRATCH_OPT16_HOSTTEST_H
#include <stdint.h>

enum { TEST_PLAN_MAX = 256, TEST_IMM_MAX = 16, TEST_ENT_MAX = 272, TEST_BINS = 32 };

typedef struct
{
	uint16_t width, height;
	uint32_t full_frame, hint_frame;
	uint8_t want, hint;
} test_texture_t;

typedef struct
{
	uint16_t key;
	float total, hist[TEST_BINS];
	uint32_t polygons;
} test_plan_t;

typedef struct
{
	uint16_t key;
	float total, seps;
} test_immediate_t;

typedef struct
{
	test_texture_t texture[TEST_ENT_MAX];
	test_plan_t plan[TEST_PLAN_MAX];
	test_immediate_t immediate[TEST_IMM_MAX];
	uint32_t pool_blocks, reserve, percentage, frame;
	int plan_count, immediate_count, active, no_fidelity, debug;
} test_input_t;

typedef struct
{
	uint32_t last, exact, percentage, plan_cycles, debug_calls;
	uint32_t cost_calls, max_cost, calls_by_level[8];
	uint32_t scratch_bytes;
	uint16_t entries;
	uint8_t active, start_level[TEST_ENT_MAX], cap[TEST_ENT_MAX];
	uint8_t want[TEST_ENT_MAX], hint[TEST_ENT_MAX];
	uint32_t hint_frame[TEST_ENT_MAX];
} test_output_t;

void baseline_run(const test_input_t *, test_output_t *);
void candidate_run(const test_input_t *, test_output_t *);
int baseline_lmax(uint16_t, uint16_t);
int candidate_lmax(uint16_t, uint16_t);
#endif
