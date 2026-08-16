#include "execution_controller.h"

static np2_execution_outcome finish(
	np2_execution_controller *controller,
	np2_execution_outcome outcome)
{
	controller->phase = NP2_EXECUTION_PHASE_TERMINAL;
	controller->terminal_outcome = outcome;
	return outcome;
}

void np2_execution_controller_init(np2_execution_controller *controller)
{
	if (controller == NULL) {
		return;
	}
	controller->phase = NP2_EXECUTION_PHASE_PRE_RUNNING;
	controller->pre_running_slices = 0;
	controller->running_slices = 0;
	controller->terminal_outcome = NP2_EXECUTION_CONTINUE;
}

np2_execution_outcome np2_execution_controller_step(
	np2_execution_controller *controller,
	np2_result_v1_observation observation,
	bool task_exit_requested)
{
	if (controller == NULL) {
		return NP2_EXECUTION_HARNESS_ERROR;
	}
	if (controller->phase == NP2_EXECUTION_PHASE_TERMINAL) {
		return controller->terminal_outcome;
	}

	if (observation == NP2_RESULT_V1_PASS) {
		return finish(controller, NP2_EXECUTION_PASS);
	}
	if (observation == NP2_RESULT_V1_FAIL) {
		return finish(controller, NP2_EXECUTION_FAIL);
	}
	if (observation == NP2_RESULT_V1_INVALID) {
		return finish(controller, NP2_EXECUTION_INVALID);
	}
	if (task_exit_requested) {
		return finish(controller, NP2_EXECUTION_HARNESS_ERROR);
	}

	if (controller->phase == NP2_EXECUTION_PHASE_PRE_RUNNING) {
		if (observation == NP2_RESULT_V1_RUNNING) {
			controller->phase = NP2_EXECUTION_PHASE_RUNNING;
			controller->running_slices = 1;
			if (controller->running_slices >= NP2_RUNNING_SLICE_LIMIT) {
				return finish(controller, NP2_EXECUTION_RUNNING_TIMEOUT);
			}
			return NP2_EXECUTION_CONTINUE;
		}
		if (observation == NP2_RESULT_V1_PRE_PROTOCOL ||
				observation == NP2_RESULT_V1_UNINITIALIZED) {
			++controller->pre_running_slices;
			if (controller->pre_running_slices >= NP2_PRE_RUNNING_SLICE_LIMIT) {
				return finish(controller, NP2_EXECUTION_NOT_REACHED);
			}
			return NP2_EXECUTION_CONTINUE;
		}
		return finish(controller, NP2_EXECUTION_INVALID);
	}

	if (controller->phase == NP2_EXECUTION_PHASE_RUNNING) {
		if (observation != NP2_RESULT_V1_RUNNING) {
			return finish(controller, NP2_EXECUTION_INVALID);
		}
		++controller->running_slices;
		if (controller->running_slices >= NP2_RUNNING_SLICE_LIMIT) {
			return finish(controller, NP2_EXECUTION_RUNNING_TIMEOUT);
		}
		return NP2_EXECUTION_CONTINUE;
	}

	return finish(controller, NP2_EXECUTION_INVALID);
}
