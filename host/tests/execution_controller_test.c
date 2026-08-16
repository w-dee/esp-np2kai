#include <stdio.h>

#include "execution_controller.h"

static unsigned failures;

static void expect(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

static np2_execution_outcome step(np2_execution_controller *controller,
		np2_result_v1_observation observation)
{
	return np2_execution_controller_step(controller, observation, false);
}

static void expect_terminal_sticky(np2_execution_controller *controller,
		np2_execution_outcome expected, const char *message)
{
	uint32_t pre_running_slices = controller->pre_running_slices;
	uint32_t running_slices = controller->running_slices;

	expect(step(controller, NP2_RESULT_V1_RUNNING) == expected, message);
	expect(controller->phase == NP2_EXECUTION_PHASE_TERMINAL,
		"terminal phase remains sticky");
	expect(controller->pre_running_slices == pre_running_slices &&
			controller->running_slices == running_slices,
		"sticky terminal does not change counters");
}

static void test_initialization_and_null(void)
{
	np2_execution_controller controller;

	np2_execution_controller_init(NULL);
	expect(np2_execution_controller_step(NULL, NP2_RESULT_V1_PRE_PROTOCOL, false) ==
			NP2_EXECUTION_HARNESS_ERROR, "NULL controller is a harness error");

	np2_execution_controller_init(&controller);
	expect(controller.phase == NP2_EXECUTION_PHASE_PRE_RUNNING,
		"initial phase is pre-running");
	expect(controller.pre_running_slices == 0 && controller.running_slices == 0,
		"initial counters are zero");
	expect(controller.terminal_outcome == NP2_EXECUTION_CONTINUE,
		"initial terminal outcome is continue");
}

static void test_golden_trace(void)
{
	np2_execution_controller controller;
	unsigned index;

	np2_execution_controller_init(&controller);
	for (index = 0; index < 201; ++index) {
		expect(step(&controller, NP2_RESULT_V1_PRE_PROTOCOL) ==
				NP2_EXECUTION_CONTINUE, "golden pre-protocol continues");
	}
	expect(step(&controller, NP2_RESULT_V1_UNINITIALIZED) ==
			NP2_EXECUTION_CONTINUE, "golden uninitialized continues");
	expect(controller.pre_running_slices == 202,
		"golden pre-running count is 202");
	expect(step(&controller, NP2_RESULT_V1_PASS) == NP2_EXECUTION_PASS,
		"golden terminal PASS does not require RUNNING");
	expect(controller.phase == NP2_EXECUTION_PHASE_TERMINAL,
		"golden trace becomes terminal");
}

static void test_pre_running_edges(void)
{
	np2_execution_controller controller;
	unsigned index;

	np2_execution_controller_init(&controller);
	for (index = 0; index < NP2_PRE_RUNNING_SLICE_LIMIT - 1; ++index) {
		expect(step(&controller, NP2_RESULT_V1_PRE_PROTOCOL) ==
				NP2_EXECUTION_CONTINUE, "511 pre-protocol slices continue");
	}
	expect(controller.pre_running_slices == 511,
		"pre-running edge count is 511");
	expect(step(&controller, NP2_RESULT_V1_PRE_PROTOCOL) ==
			NP2_EXECUTION_NOT_REACHED, "512 pre-protocol slices time out");

	np2_execution_controller_init(&controller);
	for (index = 0; index < NP2_PRE_RUNNING_SLICE_LIMIT; ++index) {
		expect(step(&controller, NP2_RESULT_V1_UNINITIALIZED) ==
				(index + 1 == NP2_PRE_RUNNING_SLICE_LIMIT ?
					NP2_EXECUTION_NOT_REACHED : NP2_EXECUTION_CONTINUE),
				"512 uninitialized slices use pre-running budget");
	}

	np2_execution_controller_init(&controller);
	for (index = 0; index < NP2_PRE_RUNNING_SLICE_LIMIT - 1; ++index) {
		(void)step(&controller, NP2_RESULT_V1_PRE_PROTOCOL);
	}
	expect(step(&controller, NP2_RESULT_V1_PASS) == NP2_EXECUTION_PASS,
		"PASS wins at pre-running edge");

	np2_execution_controller_init(&controller);
	for (index = 0; index < NP2_PRE_RUNNING_SLICE_LIMIT - 1; ++index) {
		(void)step(&controller, NP2_RESULT_V1_PRE_PROTOCOL);
	}
	expect(step(&controller, NP2_RESULT_V1_FAIL) == NP2_EXECUTION_FAIL,
		"FAIL wins at pre-running edge");

	np2_execution_controller_init(&controller);
	for (index = 0; index < NP2_PRE_RUNNING_SLICE_LIMIT - 1; ++index) {
		(void)step(&controller, NP2_RESULT_V1_PRE_PROTOCOL);
	}
	expect(step(&controller, NP2_RESULT_V1_RUNNING) == NP2_EXECUTION_CONTINUE,
		"RUNNING wins at pre-running edge");
	expect(controller.phase == NP2_EXECUTION_PHASE_RUNNING &&
			controller.pre_running_slices == 511 &&
			controller.running_slices == 1,
		"RUNNING transition preserves pre count and starts at one");
}

static void test_mixed_pre_running(void)
{
	static const np2_result_v1_observation observations[] = {
		NP2_RESULT_V1_PRE_PROTOCOL,
		NP2_RESULT_V1_UNINITIALIZED,
		NP2_RESULT_V1_PRE_PROTOCOL,
		NP2_RESULT_V1_UNINITIALIZED
	};
	np2_execution_controller controller;
	size_t index;

	np2_execution_controller_init(&controller);
	for (index = 0; index < sizeof(observations) / sizeof(observations[0]); ++index) {
		expect(step(&controller, observations[index]) == NP2_EXECUTION_CONTINUE,
				"mixed pre-running observations continue");
	}
	expect(controller.pre_running_slices == 4,
		"mixed pre-running observations share one counter");
}

static void test_running_edges(void)
{
	np2_execution_controller controller;
	unsigned index;

	np2_execution_controller_init(&controller);
	for (index = 0; index < NP2_RUNNING_SLICE_LIMIT - 1; ++index) {
		expect(step(&controller, NP2_RESULT_V1_RUNNING) ==
				NP2_EXECUTION_CONTINUE, "4095 running observations continue");
	}
	expect(controller.running_slices == 4095,
		"running edge count is 4095");
	expect(step(&controller, NP2_RESULT_V1_RUNNING) ==
			NP2_EXECUTION_RUNNING_TIMEOUT, "4096 running observations time out");

	np2_execution_controller_init(&controller);
	for (index = 0; index < NP2_RUNNING_SLICE_LIMIT - 1; ++index) {
		(void)step(&controller, NP2_RESULT_V1_RUNNING);
	}
	expect(step(&controller, NP2_RESULT_V1_PASS) == NP2_EXECUTION_PASS,
		"PASS wins at running edge");

	np2_execution_controller_init(&controller);
	for (index = 0; index < NP2_RUNNING_SLICE_LIMIT - 1; ++index) {
		(void)step(&controller, NP2_RESULT_V1_RUNNING);
	}
	expect(step(&controller, NP2_RESULT_V1_FAIL) == NP2_EXECUTION_FAIL,
		"FAIL wins at running edge");
}

static void test_running_regressions_and_invalid(void)
{
	np2_execution_controller controller;

	np2_execution_controller_init(&controller);
	expect(step(&controller, NP2_RESULT_V1_RUNNING) == NP2_EXECUTION_CONTINUE,
		"running regression setup succeeds");
	expect(step(&controller, NP2_RESULT_V1_PRE_PROTOCOL) ==
			NP2_EXECUTION_INVALID, "RUNNING to pre-protocol is invalid");

	np2_execution_controller_init(&controller);
	expect(step(&controller, NP2_RESULT_V1_RUNNING) == NP2_EXECUTION_CONTINUE,
		"second running regression setup succeeds");
	expect(step(&controller, NP2_RESULT_V1_UNINITIALIZED) ==
			NP2_EXECUTION_INVALID, "RUNNING to uninitialized is invalid");

	np2_execution_controller_init(&controller);
	expect(step(&controller, (np2_result_v1_observation)99) ==
			NP2_EXECUTION_INVALID, "unknown observation is invalid");
}

static void test_task_exit_precedence(void)
{
	np2_execution_controller controller;

	np2_execution_controller_init(&controller);
	expect(np2_execution_controller_step(&controller,
			NP2_RESULT_V1_PRE_PROTOCOL, true) == NP2_EXECUTION_HARNESS_ERROR,
		"pre-protocol task exit is harness error");

	np2_execution_controller_init(&controller);
	expect(np2_execution_controller_step(&controller,
			NP2_RESULT_V1_UNINITIALIZED, true) == NP2_EXECUTION_HARNESS_ERROR,
		"uninitialized task exit is harness error");

	np2_execution_controller_init(&controller);
	expect(np2_execution_controller_step(&controller,
			NP2_RESULT_V1_RUNNING, true) == NP2_EXECUTION_HARNESS_ERROR,
		"running task exit is harness error");

	np2_execution_controller_init(&controller);
	expect(np2_execution_controller_step(&controller,
			NP2_RESULT_V1_PASS, true) == NP2_EXECUTION_PASS,
		"PASS wins over task exit");

	np2_execution_controller_init(&controller);
	expect(np2_execution_controller_step(&controller,
			NP2_RESULT_V1_FAIL, true) == NP2_EXECUTION_FAIL,
		"FAIL wins over task exit");

	np2_execution_controller_init(&controller);
	expect(np2_execution_controller_step(&controller,
			NP2_RESULT_V1_INVALID, true) == NP2_EXECUTION_INVALID,
		"INVALID wins over task exit");
}

static void test_terminal_stickiness(void)
{
	np2_execution_controller controller;
	unsigned index;

	np2_execution_controller_init(&controller);
	expect(step(&controller, NP2_RESULT_V1_PASS) == NP2_EXECUTION_PASS,
		"PASS terminal is produced");
	expect_terminal_sticky(&controller, NP2_EXECUTION_PASS,
		"PASS remains sticky");

	np2_execution_controller_init(&controller);
	expect(step(&controller, NP2_RESULT_V1_FAIL) == NP2_EXECUTION_FAIL,
		"FAIL terminal is produced");
	expect_terminal_sticky(&controller, NP2_EXECUTION_FAIL,
		"FAIL remains sticky");

	np2_execution_controller_init(&controller);
	for (index = 0; index < NP2_PRE_RUNNING_SLICE_LIMIT; ++index) {
		(void)step(&controller, NP2_RESULT_V1_PRE_PROTOCOL);
	}
	expect(controller.terminal_outcome == NP2_EXECUTION_NOT_REACHED,
		"NOT_REACHED terminal is produced");
	expect_terminal_sticky(&controller, NP2_EXECUTION_NOT_REACHED,
		"NOT_REACHED remains sticky");

	np2_execution_controller_init(&controller);
	for (index = 0; index < NP2_RUNNING_SLICE_LIMIT; ++index) {
		(void)step(&controller, NP2_RESULT_V1_RUNNING);
	}
	expect(controller.terminal_outcome == NP2_EXECUTION_RUNNING_TIMEOUT,
		"RUNNING_TIMEOUT terminal is produced");
	expect_terminal_sticky(&controller, NP2_EXECUTION_RUNNING_TIMEOUT,
		"RUNNING_TIMEOUT remains sticky");

	np2_execution_controller_init(&controller);
	expect(step(&controller, NP2_RESULT_V1_INVALID) == NP2_EXECUTION_INVALID,
		"INVALID terminal is produced");
	expect_terminal_sticky(&controller, NP2_EXECUTION_INVALID,
		"INVALID remains sticky");

	np2_execution_controller_init(&controller);
	expect(np2_execution_controller_step(&controller,
			NP2_RESULT_V1_PRE_PROTOCOL, true) == NP2_EXECUTION_HARNESS_ERROR,
		"HARNESS_ERROR terminal is produced");
	expect_terminal_sticky(&controller, NP2_EXECUTION_HARNESS_ERROR,
		"HARNESS_ERROR remains sticky");
}

int main(void)
{
	test_initialization_and_null();
	test_golden_trace();
	test_pre_running_edges();
	test_mixed_pre_running();
	test_running_edges();
	test_running_regressions_and_invalid();
	test_task_exit_precedence();
	test_terminal_stickiness();
	if (failures != 0) {
		fprintf(stderr, "%u execution controller test(s) failed\n", failures);
		return 1;
	}
	puts("execution controller tests passed");
	return 0;
}
