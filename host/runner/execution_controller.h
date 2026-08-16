#ifndef NP2_EXECUTION_CONTROLLER_H
#define NP2_EXECUTION_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

#include "result_v1_parser.h"

#define NP2_PRE_RUNNING_SLICE_LIMIT UINT32_C(512)
#define NP2_RUNNING_SLICE_LIMIT UINT32_C(4096)

typedef enum {
	NP2_EXECUTION_PHASE_PRE_RUNNING = 0,
	NP2_EXECUTION_PHASE_RUNNING,
	NP2_EXECUTION_PHASE_TERMINAL
} np2_execution_phase;

typedef enum {
	NP2_EXECUTION_CONTINUE = 0,
	NP2_EXECUTION_PASS,
	NP2_EXECUTION_FAIL,
	NP2_EXECUTION_NOT_REACHED,
	NP2_EXECUTION_RUNNING_TIMEOUT,
	NP2_EXECUTION_INVALID,
	NP2_EXECUTION_HARNESS_ERROR
} np2_execution_outcome;

typedef struct {
	np2_execution_phase phase;
	uint32_t pre_running_slices;
	uint32_t running_slices;
	np2_execution_outcome terminal_outcome;
} np2_execution_controller;

void np2_execution_controller_init(np2_execution_controller *controller);

np2_execution_outcome np2_execution_controller_step(
	np2_execution_controller *controller,
	np2_result_v1_observation observation,
	bool task_exit_requested);

#endif /* NP2_EXECUTION_CONTROLLER_H */
