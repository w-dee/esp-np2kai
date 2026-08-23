; SPDX-License-Identifier: BSD-2-Clause
; Step 7B.2d-1 live benchmark fixture loader.

bits 16
cpu 8086
org 0

%define CONTROL_SEGMENT 0x2a00
%define CONTROL_SIZE 32
%define CONTROL_STATE_OFFSET 31

%define STATE_BOOTING 1
%define STATE_ERROR 4

%if STAGE2_SIZE < 8
    %error "STAGE2_SIZE must include the stage2 header"
%endif
%if STAGE2_SIZE > 32768
    %error "STAGE2_SIZE exceeds the stage2 load window"
%endif
%if STAGE2_SECTORS < 1 || STAGE2_SECTORS > 32
    %error "STAGE2_SECTORS is outside the stage2 contract"
%endif

start:
    cli
    cld
    jmp loader

    times 510-($-$$) db 0
    dw 0xaa55

loader:
    xor ax, ax
    mov ds, ax

    mov ax, 0x2800
    mov ss, ax
    mov sp, 0x1000

    mov ax, CONTROL_SEGMENT
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, CONTROL_SIZE / 2
    rep stosw

    ; NP2V v1 BOOTING, with the state byte written last.
    mov word [es:0], 0x504e
    mov word [es:2], 0x5632
    mov word [es:4], 1
    mov word [es:6], 16
    mov word [es:8], CONTROL_SIZE
    mov word [es:10], 2
    mov word [es:12], 0
    mov byte [es:CONTROL_STATE_OFFSET], STATE_BOOTING

    mov al, [0x0584]
    mov ah, al
    and al, 0xfc
    cmp al, 0x90
    jne boot_drive_error
    mov [cs:boot_selector], ah

    mov word [cs:remaining], STAGE2_SECTORS
    mov byte [cs:cylinder], 0
    mov byte [cs:head], 0
    mov byte [cs:sector], 2
    mov ax, 0x2000
    mov es, ax
    xor bp, bp

read_next_sector:
    cmp word [cs:remaining], 0
    je validate_stage2

    mov ax, 0x2000
    mov es, ax
    mov ah, 0x56
    mov al, [cs:boot_selector]
    mov ch, 3
    mov cl, [cs:cylinder]
    mov dh, [cs:head]
    mov dl, [cs:sector]
    mov bx, 0x0400
    push bp
    int 0x1b
    pop bp
    jc stage2_read_error
    or ah, ah
    jnz stage2_read_error

    add bp, 0x0400
    dec word [cs:remaining]
    inc byte [cs:sector]
    cmp byte [cs:sector], 9
    jb read_next_sector
    mov byte [cs:sector], 1
    inc byte [cs:head]
    cmp byte [cs:head], 2
    jb read_next_sector
    mov byte [cs:head], 0
    inc byte [cs:cylinder]
    jmp read_next_sector

validate_stage2:
    mov ax, 0x2000
    mov es, ax
    cmp word [es:0], 0x5453
    jne stage2_invalid
    cmp word [es:2], 0x5632
    jne stage2_invalid
    cmp word [es:4], 1
    jne stage2_invalid
    mov ax, [es:6]
    cmp ax, STAGE2_SIZE
    jne stage2_invalid
    cmp ax, 8
    jb stage2_invalid
    cmp ax, STAGE2_SECTORS * 0x0400
    ja stage2_invalid
    jmp 0x2000:0x0008

boot_drive_error:
    mov dx, 0x0203
    jmp publish_error

stage2_read_error:
    mov dx, 0x0201
    jmp publish_error

stage2_invalid:
    mov dx, 0x0202

publish_error:
    mov ax, CONTROL_SEGMENT
    mov es, ax
    mov [es:12], dx
    mov byte [es:CONTROL_STATE_OFFSET], STATE_ERROR
error_halt:
    cli
    hlt
    jmp error_halt

boot_selector db 0
remaining dw 0
cylinder db 0
head db 0
sector db 0

    times 1022-($-$$) db 0
    dw 0xaa55
