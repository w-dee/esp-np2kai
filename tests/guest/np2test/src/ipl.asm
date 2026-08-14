; SPDX-License-Identifier: BSD-2-Clause
; NP2TEST Step 3.5a-2 Stage 0: initialize the result block and halt PASS.

bits 16
cpu 8086
org 0

%include "result.inc"

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

    ; One deterministic Stage 0 smoke test. IDs remain zero until the suite grows.
    mov word [es:20], 1
    mov word [es:22], 0
    mov word [es:24], 0
    mov word [es:26], 0
    mov word [es:28], 0xffff

    ; Publish a valid initial body before exposing RUNNING.
    call update_crc
    mov byte [es:RESULT_STATE_OFFSET], RESULT_STATE_RUNNING

    ; Stage 0 smoke test: exercise the initialized stack and read back RAM.
    mov ax, 0x5aa5
    push ax
    pop bx
    cmp bx, ax
    jne stage0_fail
    mov ax, [es:0]
    cmp ax, 0x504e
    jne stage0_fail

    ; The smoke test has completed successfully. State is always written last.
    mov word [es:22], 1
    mov word [es:24], 1
    call update_crc
    mov byte [es:RESULT_STATE_OFFSET], RESULT_STATE_PASS
    jmp short terminal_halt

stage0_fail:
    mov word [es:22], 1
    mov word [es:26], 1
    mov word [es:28], 0
    call update_crc
    mov byte [es:RESULT_STATE_OFFSET], RESULT_STATE_FAIL

terminal_halt:
    cli
halt_loop:
    hlt
    jmp halt_loop

; CRC-32/ISO-HDLC (reflected), over result bytes [0, 120).
; DX:AX is the 32-bit CRC, with the low word in AX.
update_crc:
    push ax
    push bx
    push cx
    push dx
    push si
    push bp

    mov ax, 0xffff
    mov dx, 0xffff
    xor si, si
    mov cx, RESULT_CHECKSUM_END
.next_byte:
    mov bl, [es:si]
    xor al, bl
    mov bp, 8
.next_bit:
    test ax, 1
    jz .shift_only
    shr dx, 1
    rcr ax, 1
    xor ax, 0x8320
    xor dx, 0xedb8
    jmp short .bit_done
.shift_only:
    shr dx, 1
    rcr ax, 1
.bit_done:
    dec bp
    jnz .next_bit
    inc si
    loop .next_byte

    not ax
    not dx
    mov [es:RESULT_CHECKSUM_OFFSET], ax
    mov [es:RESULT_CHECKSUM_OFFSET + 2], dx

    pop bp
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

times 510-($-$$) db 0
dw 0xaa55
times 1022-($-$$) db 0
dw 0xaa55
