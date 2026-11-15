/* Differential decisions, scratch bounds and cost-width tests; no renderer model. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plan_scratch_opt16_hosttest.h"

static test_input_t input;
static test_output_t old_result, new_result;
static unsigned long checks, cases, removed_calls, texture_decisions;
static unsigned long coverage_hidden, coverage_full, coverage_immediate, coverage_fidelity;
static unsigned long coverage_overbudget, coverage_debug;
static unsigned covered_entries[TEST_ENT_MAX + 1];
static unsigned covered_caps[6];
static unsigned widest_cost;
static uint32_t seed = 0x1632ab5u;

static void require(int ok, const char *what)
{
	checks++;
	if (!ok)
	{
		fprintf(stderr, "PLAN SCRATCH FAIL: %s case=%lu plan=%d immediate=%d\n", what, cases, input.plan_count, input.immediate_count);
		exit(1);
	}
}

static uint32_t random_u32(void)
{
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return seed;
}

static void reset(void)
{
	unsigned i;
	memset(&input, 0, sizeof input);
	input.active = 1;
	input.no_fidelity = 1;
	input.frame = 419;
	input.pool_blocks = 12032;
	for (i = 0; i < TEST_ENT_MAX; i++)
	{
		input.texture[i].width = input.texture[i].height = 512;
		input.texture[i].want = 173;
		input.texture[i].hint = 219;
		input.texture[i].hint_frame = 17;
		if (i < TEST_PLAN_MAX)
		{
			input.plan[i].key = (uint16_t)i;
			input.plan[i].total = input.plan[i].hist[0] = 4000.0f;
			input.plan[i].polygons = i + 1;
		}
		if (i < TEST_IMM_MAX)
		{
			input.immediate[i].key = (uint16_t)(TEST_PLAN_MAX + i);
			input.immediate[i].total = 2000.0f;
			input.immediate[i].seps = 0.04f;
		}
	}
}

static void run_case(const char *name)
{
	unsigned i, budget, expected_removed = 0;
	cases++;
	baseline_run(&input, &old_result);
	candidate_run(&input, &new_result);
	if (old_result.last != new_result.last || old_result.exact != new_result.exact)
		fprintf(stderr, "%s: baseline last/exact=%u/%u candidate=%u/%u\n", name, old_result.last, old_result.exact, new_result.last, new_result.exact);
	require(old_result.last == new_result.last, "working-set blocks differ");
	require(old_result.exact == new_result.exact, "exact working-set blocks differ");
	require(old_result.percentage == new_result.percentage, "budget initialization differs");
	require(old_result.plan_cycles == new_result.plan_cycles, "planner measuring counter differs");
	require(old_result.debug_calls == new_result.debug_calls, "diagnostic entry count differs");
	require(old_result.active == new_result.active, "active-state transition differs");
	require(old_result.entries == new_result.entries, "assembled entry count differs");
	require(old_result.scratch_bytes == TEST_ENT_MAX * 8 * 4, "unexpected baseline scratch layout");
	require(new_result.scratch_bytes == TEST_ENT_MAX * 6 * 4, "unexpected candidate scratch layout");
	for (i = 0; i < TEST_ENT_MAX; i++)
	{
		require(old_result.want[i] == new_result.want[i], "planned mip level differs");
		require(old_result.hint[i] == new_result.hint[i], "carried immediate hint differs");
		require(old_result.hint_frame[i] == new_result.hint_frame[i], "carried immediate hint lifetime differs");
		texture_decisions++;
	}
	for (i = 0; i < new_result.entries; i++)
	{
		require(old_result.start_level[i] == new_result.start_level[i], "initial level differs");
		require(old_result.cap[i] == new_result.cap[i], "coarsening cap differs");
		require(new_result.start_level[i] <= new_result.cap[i] && new_result.cap[i] <= 5, "level/cap bound violated");
		covered_caps[new_result.cap[i]]++;
	}
	budget = input.pool_blocks / 100u * new_result.percentage;
	if (budget > input.reserve)
		budget -= input.reserve;
	if (input.active && new_result.exact > budget)
	{
		expected_removed = new_result.entries;
		coverage_overbudget++;
	}
	require(old_result.cost_calls >= new_result.cost_calls, "candidate did more footprint work");
	require(old_result.cost_calls - new_result.cost_calls == expected_removed, "did not remove exactly one unused cost per filled row");
	require(new_result.calls_by_level[6] == 0 && new_result.calls_by_level[7] == 0, "candidate requested a level beyond five");
	removed_calls += expected_removed;
	if (new_result.max_cost > widest_cost)
		widest_cost = new_result.max_cost;
	if (input.active)
		covered_entries[new_result.entries]++;
	coverage_debug += new_result.debug_calls != 0;
}

static void boundary_controls(void)
{
	/* First invocation gives the missing-cap control cold zeroed static scratch. */
	reset();
	input.plan_count = 1;
	input.pool_blocks = 0;
	run_case("cap-five-cold");
	require(new_result.start_level[0] == 4 && new_result.cap[0] == 5, "cap-five fixture does not reach its boundary");
	require(new_result.want[0] == 5 && new_result.last == 1, "one-block minimum texture cost");
	require(old_result.calls_by_level[6] > 0, "old unused cap-plus-one cost not reached");

	reset();
	input.plan_count = 2;
	input.pool_blocks = 0;
	input.texture[0].width = input.texture[0].height = 16384;
	input.texture[1] = input.texture[0];
	input.plan[0].hist[0] = input.plan[1].hist[0] = 0.0f;
	input.plan[0].hist[31] = input.plan[1].hist[31] = 4000.0f;
	run_case("u32-cost-16384");
	require(new_result.exact == 2097152u, "legal huge exact costs must exceed uint16_t");
	require(new_result.last == 131072u && new_result.want[0] == 2 && new_result.want[1] == 2, "legal huge relaxed costs must remain u32");

	reset();
	input.plan_count = 1;
	input.plan[0].hist[0] = 0.0f;
	input.plan[0].hist[7] = 4000.0f;
	run_case("fidelity-disabled");
	require(new_result.want[0] == 3, "exact histogram choice expected at level three");
	input.no_fidelity = 0;
	run_case("fidelity-enabled");
	require(new_result.want[0] == 0, "fidelity pass must refine its visible image");
	coverage_fidelity++;

	reset();
	input.plan_count = 1;
	input.immediate_count = 16;
	for (unsigned i = 0; i < 16; i++)
		input.immediate[i].key = (uint16_t)(i % 3); /* planned duplicate and duplicate carried keys */
	input.immediate[1].total = 0.0f;
	input.immediate[4].total = 0.0f;
	input.immediate[7].total = 0.0f;
	input.immediate[10].total = 0.0f;
	run_case("immediate-duplicates");
	require(new_result.entries == 3 && new_result.hint[0] == 219, "planned duplicates do not receive immediate hints");
	require(new_result.hint_frame[1] == input.frame + 1u && new_result.hint_frame[2] == input.frame + 1u, "positive carried entries receive current hints");
	coverage_immediate++;

	reset();
	input.plan_count = 3;
	input.plan[0].total = 0.0f;
	input.plan[1].total = -0.0f;
	input.pool_blocks = 0;
	run_case("no-area-entries");
	require(new_result.want[0] == 0 && new_result.want[1] == 0 && new_result.exact == 4, "no-area entries contribute no working-set cost");
	coverage_hidden++;

	for (unsigned age = 0; age <= 150; age += age == 0 ? 149 : 1)
	{
		reset();
		input.plan_count = 1;
		input.texture[0].full_frame = input.frame + 1u - age;
		input.pool_blocks = 0;
		run_case("full-keep-boundary");
		require(new_result.want[0] == (age < 150 ? 0 : 5), "full-size keep expires at frame150");
		coverage_full++;
	}
	reset();
	input.plan_count = 1;
	input.active = 0;
	run_case("inactive");
	require(new_result.want[0] == 173 && new_result.percentage == 0, "inactive planner does not change texture or budget");
}

static void lmax_dimensions(void)
{
	for (unsigned side = 1; side <= 16384; side++)
	{
		unsigned expected = 0;
		while (expected < 5 && !(side & ((1u << (expected + 1)) - 1u)) && (side >> (expected + 1)) >= 16u)
			expected++;
		require(baseline_lmax((uint16_t)side, 512) == (int)expected, "baseline lmax differs from divisibility/minimum-axis oracle");
		require(candidate_lmax((uint16_t)side, 512) == (int)expected, "candidate width lmax differs from oracle");
		require(candidate_lmax(512, (uint16_t)side) == (int)expected, "candidate height lmax differs from oracle");
	}
}

static void all_entry_counts(void)
{
	/* Every reachable assembled count: zero to the full256 plan +16 carried. */
	for (unsigned count = 0; count <= TEST_ENT_MAX; count++)
	{
		reset();
		input.plan_count = (int)(count < TEST_PLAN_MAX ? count : TEST_PLAN_MAX);
		input.immediate_count = (int)(count - (unsigned)input.plan_count);
		input.pool_blocks = count & 1u ? 0 : 12032;
		input.no_fidelity = (int)(count & 1u);
		run_case("all-entry-counts");
		require(new_result.entries == count, "entry-count sweep assembled the wrong count");
	}
}

static void varied_plans(void)
{
	static const uint16_t dimensions[] = {16, 17, 31, 32, 48, 63, 64, 96, 127, 128, 192, 255, 256, 384, 511, 512, 768, 1024, 1536, 2048, 4096, 8192, 16384};
	static const unsigned pools[] = {0, 99, 100, 4096, 12032, 16384, 1000000};
	static const unsigned percentages[] = {0, 35, 36, 50, 74, 75};
	static const float areas[] = {0.0f, 0.05f, 0.5f, 1.0f, 1499.0f, 1500.0f, 8000.0f, 200000.0f};
	/* Deterministic varied frames also leave scratch warm across size changes. */
	for (unsigned frame = 0; frame < 420; frame++)
	{
		unsigned nominal_budget;
		reset();
		input.plan_count = (int)(random_u32() % 257);
		input.immediate_count = (int)(random_u32() % 17);
		input.no_fidelity = (int)(frame & 1u);
		input.pool_blocks = pools[frame % (sizeof pools / sizeof *pools)];
		input.percentage = percentages[frame % (sizeof percentages / sizeof *percentages)];
		nominal_budget = input.pool_blocks / 100u * (input.percentage ? input.percentage : 75u);
		input.reserve = frame % 4 == 0 ? 0 : frame % 4 == 1 ? nominal_budget : frame % 4 == 2 ? nominal_budget + 1 : nominal_budget / 2;
		input.frame = frame % 7 ? frame : UINT32_MAX - 1u;
		input.debug = frame % 13 == 0;
		if (input.debug)
			input.frame = 60;
		for (unsigned i = 0; i < TEST_ENT_MAX; i++)
		{
			const unsigned nd = sizeof dimensions / sizeof *dimensions;
			input.texture[i].width = dimensions[random_u32() % nd];
			input.texture[i].height = dimensions[random_u32() % nd];
			if (i % 19 == 0)
				input.texture[i].full_frame = input.frame + 1u - (random_u32() % 152);
			if (i < TEST_PLAN_MAX)
			{
				float total = areas[random_u32() % (sizeof areas / sizeof *areas)];
				unsigned bin = random_u32() % TEST_BINS;
				memset(input.plan[i].hist, 0, sizeof input.plan[i].hist);
				input.plan[i].total = total;
				/*2% histogram-tail boundary, immediately above, and mixed bins. */
				input.plan[i].hist[bin] = total * 0.98f;
				input.plan[i].hist[31] += total * (frame % 3 == 0 ? 0.02f : frame % 3 == 1 ? 0.02001f : 0.01999f);
			}
			if (i < TEST_IMM_MAX)
			{
				input.immediate[i].key = (uint16_t)(frame % 3 == 0 ? (i % 3) : TEST_PLAN_MAX + i);
				input.immediate[i].total = areas[random_u32() % (sizeof areas / sizeof *areas)];
				input.immediate[i].seps = i % 5 == 0 ? 1.0e6f : ldexpf(1.0f + (float)(i & 3u) / 4.0f, -(int)(i % 7));
			}
		}
		run_case("varied-plans");
	}
}

int main(void)
{
	boundary_controls();
	lmax_dimensions();
	all_entry_counts();
	varied_plans();
	for (unsigned i = 0; i <= TEST_ENT_MAX; i++)
		require(covered_entries[i] != 0, "missing assembled-count coverage");
	for (unsigned i = 0; i <= 5; i++)
		require(covered_caps[i] != 0, "missing coarsening-cap coverage");
	require(coverage_hidden && coverage_full && coverage_immediate && coverage_fidelity && coverage_overbudget && coverage_debug, "missing semantic path coverage");
	require(widest_cost == 1048576u, "large costs were not exercised");
	printf("{\"cases\":%lu,\"checks\":%lu,\"texture_decisions\":%lu,\"assembled_counts\":273,\"cap_values\":6,\"lmax_axis_sizes\":16384,\"removed_footprint_calls\":%lu,\"overbudget_cases\":%lu,\"debug_cases\":%lu,\"largest_cost_blocks\":%u,\"baseline_scratch_bytes\":8704,\"candidate_scratch_bytes\":6528,\"saved_bytes\":2176}\n", cases, checks, texture_decisions, removed_calls, coverage_overbudget, coverage_debug, widest_cost);
	return 0;
}
