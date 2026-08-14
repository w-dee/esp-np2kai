; SPDX-License-Identifier: BSD-2-Clause
; NP2TEST Step 3.5a-3: initialize result-v1 and run the Stage 1 core.

bits 16
cpu 8086
org 0

%include "result.inc"
%include "test_ids.inc"

start:
    cli
    cld

    xor ax, ax
    mov ds, ax

    mov ax, STACK_SEG
    mov ss, ax
    mov sp, STACK_TOP

    mov ax, RESULT_SEG
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, RESULT_SIZE / 2
    rep stosw

    ; Fixed result-v1 header: NP2T, version 1, 32-byte header, 128-byte block.
    mov word [es:0], 0x504e
    mov word [es:2], 0x5432
    mov word [es:4], 1
    mov word [es:6], 32
    mov word [es:8], RESULT_SIZE
    mov word [es:10], 0

    ; Numeric Stage 1 suite/build identifiers and fail-fast counters.
    mov word [es:12], RESULT_SUITE_ID & 0xffff
    mov word [es:14], RESULT_SUITE_ID >> 16
    mov word [es:16], RESULT_BUILD_ID & 0xffff
    mov word [es:18], RESULT_BUILD_ID >> 16
    mov word [es:20], STAGE1_TOTAL_COUNT
    mov word [es:22], 0
    mov word [es:24], 0
    mov word [es:26], 0
    mov word [es:28], TEST_NONE

    ; Publish a valid initial body before exposing RUNNING.
    call update_crc
    mov byte [es:RESULT_STATE_OFFSET], RESULT_STATE_RUNNING

    ; Stage 1 uses conventional RAM through DS=2000h; helpers use ES=2900h.
    mov ax, 0x2000
    mov ds, ax

%include "stage1.inc"
