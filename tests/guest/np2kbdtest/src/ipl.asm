; SPDX-License-Identifier: BSD-2-Clause
; Deterministic NP2 keyboard hardware-polling fixture.

bits 16
cpu 8086
org 0

%include "control.inc"
%include "result.inc"

start:
    cli
    cld

    mov ax, 0x2800
    mov ss, ax
    mov sp, 0x1000

    mov ax, RESULT_SEG
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, RESULT_SIZE / 2
    rep stosw

    ; result-v1 fixed header and RUNNING publication.
    mov word [es:0], 0x504e
    mov word [es:2], 0x5432
    mov word [es:4], 1
    mov word [es:6], 32
    mov word [es:8], RESULT_SIZE
    mov word [es:12], RESULT_SUITE_ID & 0xffff
    mov word [es:14], RESULT_SUITE_ID >> 16
    mov word [es:16], RESULT_BUILD_ID & 0xffff
    mov word [es:18], RESULT_BUILD_ID >> 16
    mov word [es:20], RESULT_TOTAL_COUNT
    mov word [es:28], RESULT_NO_FAILED_ID
    mov cx, RESULT_CHECKSUM_END
    mov di, RESULT_CHECKSUM_OFFSET
    call crc32
    mov byte [es:RESULT_STATE_OFFSET], RESULT_STATE_RUNNING

    ; Clear and initialize the project-owned control block.
    mov ax, CONTROL_SEG
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, CONTROL_SIZE / 2
    rep stosw
    mov word [es:0], 0x504e       ; "NP2K" little-endian
    mov word [es:2], 0x4b32
    mov word [es:4], CONTROL_VERSION
    mov word [es:6], CONTROL_HEADER_SIZE
    mov word [es:8], CONTROL_BLOCK_SIZE
    mov word [es:10], CONTROL_FLAGS
    mov word [es:12], CONTROL_SUITE_ID & 0xffff
    mov word [es:14], CONTROL_SUITE_ID >> 16
    mov word [es:16], CONTROL_BUILD_ID & 0xffff
    mov word [es:18], CONTROL_BUILD_ID >> 16
    mov byte [es:20], CONTROL_EXPECTED_MAKE
    mov byte [es:21], CONTROL_EXPECTED_BREAK
    mov cx, CONTROL_CRC_END
    mov di, CONTROL_CRC_OFFSET
    call crc32

    ; No artificial reset/flush: reject bytes already presented by startup.
    in al, 0x43
    test al, 0x10
    jnz fail_overflow
    test al, 0x02
    jnz fail_data_ready

    mov al, CONTROL_STATE_READY
    call publish_control

wait_make:
    in al, 0x43
    test al, 0x10
    jnz fail_overflow
    test al, 0x02
    jz wait_make
    in al, 0x41
    mov [es:CONTROL_OBSERVED_MAKE], al
    cmp al, CONTROL_EXPECTED_MAKE
    jne fail_make
    mov al, CONTROL_STATE_MAKE
    call publish_control

wait_break:
    in al, 0x43
    test al, 0x10
    jnz fail_overflow
    test al, 0x02
    jz wait_break
    in al, 0x41
    mov [es:CONTROL_OBSERVED_BREAK], al
    cmp al, CONTROL_EXPECTED_BREAK
    jne fail_break
    mov al, CONTROL_STATE_BREAK
    call publish_control

    mov ax, RESULT_SEG
    mov es, ax
    mov word [es:22], RESULT_TOTAL_COUNT
    mov word [es:24], RESULT_TOTAL_COUNT
    mov word [es:28], RESULT_NO_FAILED_ID
    mov word [es:30], 0
    mov cx, RESULT_CHECKSUM_END
    mov di, RESULT_CHECKSUM_OFFSET
    call crc32
    mov byte [es:RESULT_STATE_OFFSET], RESULT_STATE_PASS
    jmp terminal

fail_data_ready:
    mov al, CONTROL_FAILURE_DATA_READY
    jmp fail_control
fail_overflow:
    mov al, CONTROL_FAILURE_OVERFLOW
    jmp fail_control
fail_make:
    mov al, CONTROL_FAILURE_MAKE
    jmp fail_control_result
fail_break:
    mov al, CONTROL_FAILURE_BREAK

fail_control:
fail_control_result:
    mov [es:CONTROL_FAILURE_REASON], al
    mov al, CONTROL_STATE_FAIL
    call publish_control
    mov ax, RESULT_SEG
    mov es, ax
    mov word [es:22], RESULT_TOTAL_COUNT
    mov word [es:24], 0
    mov word [es:26], 1
    mov word [es:28], RESULT_TEST_ID
    mov word [es:30], 4
    mov word [es:32], 0x424b       ; "KB"
    mov word [es:34], 0x3144       ; "D1"
    mov cx, RESULT_CHECKSUM_END
    mov di, RESULT_CHECKSUM_OFFSET
    call crc32
    mov byte [es:RESULT_STATE_OFFSET], RESULT_STATE_FAIL

terminal:
    cli
halt:
    hlt
    jmp halt

; Publish control CRC before the state commit byte.
publish_control:
    push ax
    mov cx, CONTROL_CRC_END
    mov di, CONTROL_CRC_OFFSET
    call crc32
    pop ax
    mov [es:CONTROL_STATE_OFFSET], al
    ret

; ES points at either contract block. CX is coverage length and DI is CRC offset.
crc32:
    mov ax, 0xffff
    mov dx, 0xffff
    xor si, si
.byte:
    mov bl, [es:si]
    xor al, bl
    mov bp, 8
.bit:
    test ax, 1
    jz .shift
    shr dx, 1
    rcr ax, 1
    xor ax, 0x8320
    xor dx, 0xedb8
    jmp short .done
.shift:
    shr dx, 1
    rcr ax, 1
.done:
    dec bp
    jnz .bit
    inc si
    loop .byte
    not ax
    not dx
    mov [es:di], ax
    mov [es:di + 2], dx
    ret

times 510-($-$$) db 0
dw 0xaa55
times 1022-($-$$) db 0
dw 0xaa55
