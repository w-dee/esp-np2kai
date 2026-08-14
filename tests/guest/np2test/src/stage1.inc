; SPDX-License-Identifier: BSD-2-Clause
; NP2TEST Step 3.5a-3: deterministic 8086 CPU and conventional-RAM tests.

%include "test_ids.inc"

; The IPL includes this file at its entry point. Skip over shared helpers and
; enter the first active test explicitly.
    jmp stage1_test_add_word

%macro TEST_BEGIN 1
    mov bp, %1
%endmacro

%macro CHECK_AX 1
    cmp ax, %1
    jne stage1_fail
%endmacro

%macro CHECK_BX 1
    cmp bx, %1
    jne stage1_fail
%endmacro

%macro CHECK_CX 1
    cmp cx, %1
    jne stage1_fail
%endmacro

%macro CHECK_DX 1
    cmp dx, %1
    jne stage1_fail
%endmacro

%macro CHECK_SP 1
    cmp sp, %1
    jne stage1_fail
%endmacro

%macro CHECK_AL 1
    cmp al, %1
    jne stage1_fail
%endmacro

%macro CHECK_FLAGS 2
    pushf
    pop dx
    and dx, %1
    cmp dx, %2
    jne stage1_fail
%endmacro

%macro TEST_PASS 0
    call stage1_pass
%endmacro

stage1_finish_pass:
    call update_crc
    mov byte [es:RESULT_STATE_OFFSET], RESULT_STATE_PASS
    jmp terminal_halt

stage1_pass:
    inc word [es:22]
    inc word [es:24]
    ret

stage1_fail:
    inc word [es:22]
    mov word [es:26], 1
    mov word [es:30], 4
    mov [es:28], bp
    mov word [es:32], 0x5453
    mov word [es:34], 0x3147
    call update_crc
    mov byte [es:RESULT_STATE_OFFSET], RESULT_STATE_FAIL

terminal_halt:
    cli
stage1_halt_loop:
    hlt
    jmp stage1_halt_loop

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

; Active IDs 0201, 0203, 0204, 0205, and 0301 are kept before the
; first signature. The explicit jump below prevents execution of 55 aa.

stage1_add_fail:
    jmp stage1_fail

stage1_test_add_word:
    TEST_BEGIN TEST_ADD_WORD
    mov ax, 0x7fff
    add ax, 1
    pushf
    pop dx
    and dx, ARITH_MASK
    cmp dx, 0x0894
    jne stage1_add_fail
    cmp ax, 0x8000
    jne stage1_add_fail
    TEST_PASS
    jmp stage1_test_sub_word

stage1_sub_fail:
    jmp stage1_fail

stage1_test_sub_word:
    TEST_BEGIN TEST_SUB_WORD
    mov ax, 0
    sub ax, 1
    pushf
    pop dx
    and dx, ARITH_MASK
    cmp dx, 0x0095
    jne stage1_sub_fail
    cmp ax, 0xffff
    jne stage1_sub_fail
    TEST_PASS
    jmp stage1_test_sbb_word

stage1_sbb_fail:
    jmp stage1_fail

stage1_test_sbb_word:
    TEST_BEGIN TEST_SBB_WORD
    stc
    mov ax, 0
    sbb ax, 0
    pushf
    pop dx
    and dx, ARITH_MASK
    cmp dx, 0x0095
    jne stage1_sbb_fail
    cmp ax, 0xffff
    jne stage1_sbb_fail
    TEST_PASS
    jmp stage1_test_cmp_word

stage1_cmp_fail:
    jmp stage1_fail

stage1_test_cmp_word:
    TEST_BEGIN TEST_CMP_WORD
    mov ax, 0x8000
    cmp ax, 1
    pushf
    pop dx
    and dx, ARITH_MASK
    cmp dx, 0x0814
    jne stage1_cmp_fail
    cmp ax, 0x8000
    jne stage1_cmp_fail
    TEST_PASS
    jmp stage1_test_logical_words

stage1_logical_fail:
    jmp stage1_fail

stage1_test_logical_words:
    TEST_BEGIN TEST_LOGICAL_WORDS
    mov ax, 0xf0f0
    and ax, 0x0ff0
    pushf
    pop dx
    and dx, LOGICAL_SHIFT_MASK
    cmp dx, 0x0004
    jne stage1_logical_fail
    cmp ax, 0x00f0
    jne stage1_logical_fail
    mov ax, 0xffff
    xor ax, ax
    pushf
    pop dx
    and dx, LOGICAL_SHIFT_MASK
    cmp dx, 0x0044
    jne stage1_logical_fail
    cmp ax, 0
    jne stage1_logical_fail
    TEST_PASS

    jmp stage1_second

times 510-($-$$) db 0
dw 0xaa55

; The second executable half begins at exactly offset 0x0200.
stage1_second:

stage1_test_mov_reg_word:
    TEST_BEGIN TEST_MOV_REG_WORD
    mov ax, 0x1234
    mov bx, ax
    CHECK_BX 0x1234
    TEST_PASS
    jmp stage1_test_shifts_one

stage1_shifts_fail:
    jmp stage1_fail

stage1_test_shifts_one:
    TEST_BEGIN TEST_SHIFTS_ONE
    mov ax, 0x4001
    shl ax, 1
    pushf
    pop dx
    and dx, LOGICAL_SHIFT_MASK
    cmp dx, 0x0880
    jne stage1_shifts_fail
    cmp ax, 0x8002
    jne stage1_shifts_fail
    mov ax, 0x8001
    shr ax, 1
    pushf
    pop dx
    and dx, LOGICAL_SHIFT_MASK
    cmp dx, 0x0805
    jne stage1_shifts_fail
    cmp ax, 0x4000
    jne stage1_shifts_fail
    TEST_PASS
    jmp stage1_test_jcc_representative

stage1_test_jcc_representative:
    TEST_BEGIN TEST_JCC_REPRESENTATIVE
    xor ax, ax
    jz stage1_jcc_z_taken
    jmp stage1_fail
stage1_jcc_z_taken:
    stc
    jc stage1_jcc_c_taken
    jmp stage1_fail
stage1_jcc_c_taken:
    mov ax, 0x8000
    test ax, ax
    js stage1_jcc_s_taken
    jmp stage1_fail
stage1_jcc_s_taken:
    TEST_PASS
    jmp stage1_test_nested_call_ret

stage1_stack_fail:
    jmp stage1_fail

stage1_test_nested_call_ret:
    TEST_BEGIN TEST_NESTED_CALL_RET
    mov sp, STACK_TOP
    xor ax, ax
    call stage1_nested_outer
    cmp ax, 2
    jne stage1_stack_fail
    cmp sp, STACK_TOP
    jne stage1_stack_fail
    TEST_PASS
    jmp stage1_test_segment_effective

stage1_nested_outer:
    call stage1_nested_inner
    inc ax
    ret

stage1_nested_inner:
    inc ax
    ret

stage1_segment_fail:
    jmp stage1_fail

stage1_test_segment_effective:
    TEST_BEGIN TEST_SEGMENT_EFFECTIVE
    mov ax, 0x2000
    mov ds, ax
    mov word [0x0100], 0xbeef
    mov ax, 0x2010
    mov es, ax
    mov word [es:0], 0xcafe
    mov ax, RESULT_SEG
    mov es, ax
    mov ax, [0x0100]
    cmp ax, 0xcafe
    jne stage1_segment_fail

    mov bx, 0x0010
    mov si, 0x0200
    mov word [bx+si+0x0004], 0x1234
    mov ax, [bx+si+0x0004]
    cmp ax, 0x1234
    jne stage1_segment_fail

    mov ax, 0x2012
    mov es, ax
    mov byte [es:0], 0xa5
    mov ax, RESULT_SEG
    mov es, ax
    mov al, [0x0120]
    cmp al, 0xa5
    jne stage1_segment_fail
    mov ax, 0x2012
    mov es, ax
    mov byte [es:0], 0x5a
    mov ax, RESULT_SEG
    mov es, ax
    mov al, [0x0120]
    cmp al, 0x5a
    jne stage1_segment_fail
    TEST_PASS
    jmp stage1_test_ram_byte_word_endian

stage1_ram_fail:
    jmp stage1_fail

stage1_test_ram_byte_word_endian:
    TEST_BEGIN TEST_RAM_BYTE_WORD_ENDIAN
    mov byte [0x0040], 0x5a
    mov word [0x0042], 0xa55a
    mov word [0x0045], 0x1234
    mov al, [0x0040]
    cmp al, 0x5a
    jne stage1_ram_fail
    mov ax, [0x0042]
    cmp ax, 0xa55a
    jne stage1_ram_fail
    mov al, [0x0045]
    cmp al, 0x34
    jne stage1_ram_fail
    mov al, [0x0046]
    cmp al, 0x12
    jne stage1_ram_fail
    TEST_PASS
    jmp stage1_test_rep_movs

stage1_rep_movs_fail:
    jmp stage1_fail

stage1_test_rep_movs:
    TEST_BEGIN TEST_REP_MOVS
    mov ax, 0x2000
    mov ds, ax
    mov es, ax
    mov byte [0x0300], 0x11
    mov byte [0x0301], 0x22
    mov byte [0x0302], 0x33
    mov byte [0x0303], 0x44
    mov word [0x0310], 0x1122
    mov word [0x0312], 0x3344
    mov si, 0x0300
    mov di, 0x0400
    mov cx, 4
    rep movsb
    mov si, 0x0310
    mov di, 0x0410
    mov cx, 2
    rep movsw
    mov ax, RESULT_SEG
    mov es, ax
    cmp si, 0x0314
    jne stage1_rep_movs_fail
    cmp di, 0x0414
    jne stage1_rep_movs_fail
    cmp cx, 0
    jne stage1_rep_movs_fail
    mov ax, [0x0400]
    cmp ax, 0x2211
    jne stage1_rep_movs_fail
    mov ax, [0x0402]
    cmp ax, 0x4433
    jne stage1_rep_movs_fail
    mov ax, [0x0410]
    cmp ax, 0x1122
    jne stage1_rep_movs_fail
    mov ax, [0x0412]
    cmp ax, 0x3344
    jne stage1_rep_movs_fail
    TEST_PASS
    jmp stage1_test_cbw_cwd

stage1_cbw_fail:
    jmp stage1_fail

stage1_test_cbw_cwd:
    TEST_BEGIN TEST_CBW_CWD
    mov al, 0x80
    cbw
    cmp ax, 0xff80
    jne stage1_cbw_fail
    mov ax, 0x8000
    cwd
    cmp dx, 0xffff
    jne stage1_cbw_fail
    TEST_PASS
    jmp stage1_finish_pass

times 1022-($-$$) db 0
dw 0xaa55
