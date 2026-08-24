; SPDX-License-Identifier: BSD-2-Clause
; NP2 keyboard fixture result-v1 wire contract.
; RUNNING is live: dynamic body/CRC integrity is authoritative only after the
; terminal PASS or FAIL state is committed.

%define RESULT_SEG                   0x2900
%define RESULT_SIZE                  128
%define RESULT_CHECKSUM_END         120
%define RESULT_CHECKSUM_OFFSET      120
%define RESULT_STATE_OFFSET         124

%define RESULT_STATE_UNINITIALIZED   0
%define RESULT_STATE_RUNNING         1
%define RESULT_STATE_PASS            2
%define RESULT_STATE_FAIL            3

%define RESULT_SUITE_ID              0x4e504b31
%define RESULT_BUILD_ID              0x00010001
%define RESULT_TOTAL_COUNT           1
%define RESULT_TEST_ID               0x0c01
%define RESULT_NO_FAILED_ID          0xffff
