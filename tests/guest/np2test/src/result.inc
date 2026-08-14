; SPDX-License-Identifier: BSD-2-Clause
; NP2TEST result-v1 offsets and state values shared by the Stage 0 IPL.

%define RESULT_SEG                  0x2900
%define RESULT_SIZE                 128
%define RESULT_CHECKSUM_END         120
%define RESULT_CHECKSUM_OFFSET      120
%define RESULT_STATE_OFFSET         124

%define RESULT_STATE_UNINITIALIZED  0
%define RESULT_STATE_RUNNING        1
%define RESULT_STATE_PASS           2
%define RESULT_STATE_FAIL           3

%define STACK_SEG                   0x2800
%define STACK_TOP                   0x1000
