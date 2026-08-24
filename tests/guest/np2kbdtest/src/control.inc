; SPDX-License-Identifier: BSD-2-Clause
; NP2 keyboard control-v1 wire contract.

%define CONTROL_SEG                 0x27fc
%define CONTROL_SIZE                64
%define CONTROL_CRC_END             56
%define CONTROL_CRC_OFFSET          56
%define CONTROL_STATE_OFFSET        60

%define CONTROL_VERSION             1
%define CONTROL_HEADER_SIZE         26
%define CONTROL_BLOCK_SIZE          64
%define CONTROL_FLAGS               0
%define CONTROL_SUITE_ID            0x4e504b31
%define CONTROL_BUILD_ID            0x00010001
%define CONTROL_EXPECTED_MAKE       0x1d
%define CONTROL_EXPECTED_BREAK      0x9d

%define CONTROL_OBSERVED_MAKE       22
%define CONTROL_OBSERVED_BREAK      23
%define CONTROL_FAILURE_REASON      24

%define CONTROL_STATE_UNINITIALIZED 0
%define CONTROL_STATE_READY         1
%define CONTROL_STATE_MAKE          2
%define CONTROL_STATE_BREAK         3
%define CONTROL_STATE_FAIL          4

%define CONTROL_FAILURE_NONE        0
%define CONTROL_FAILURE_DATA_READY  1
%define CONTROL_FAILURE_OVERFLOW    2
%define CONTROL_FAILURE_MAKE        3
%define CONTROL_FAILURE_BREAK       4
