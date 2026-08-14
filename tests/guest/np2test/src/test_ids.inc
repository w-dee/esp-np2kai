; SPDX-License-Identifier: BSD-2-Clause
; NP2TEST Stage 1 active and reserved stable test identifiers.

%define TEST_MOV_REG_WORD           0x0101
%define TEST_ADD_WORD               0x0201
%define TEST_SUB_WORD               0x0203
%define TEST_SBB_WORD               0x0204
%define TEST_CMP_WORD               0x0205
%define TEST_LOGICAL_WORDS          0x0301
%define TEST_SHIFTS_ONE             0x0401
%define TEST_JCC_REPRESENTATIVE     0x0502
%define TEST_NESTED_CALL_RET        0x0602
%define TEST_SEGMENT_EFFECTIVE      0x0701
%define TEST_RAM_BYTE_WORD_ENDIAN   0x0801
%define TEST_REP_MOVS               0x0901
%define TEST_CBW_CWD                0x0b01

%define ARITH_MASK                 0x08d5
%define LOGICAL_SHIFT_MASK         0x08c5

%define TEST_NONE                  0xffff
