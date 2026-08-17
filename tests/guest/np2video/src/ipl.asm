; SPDX-License-Identifier: BSD-2-Clause
; NP2 video fixture Step 7A.3a: deterministic text-only scene.

bits 16
cpu 8086
org 0

%define CONTROL_SEGMENT 0x2a00
%define CONTROL_SIZE 32
%define CONTROL_STATE_OFFSET 31

%define STATE_BOOTING 1
%define STATE_PROGRAMMING_VIDEO 2
%define STATE_SCENE_READY 3

%define TEXT_SEGMENT 0xa000
%define ATTRIBUTE_SEGMENT 0xa200
%define TEXT_COLUMNS 80
%define TEXT_ROWS 25
%define TEXT_CELLS (TEXT_COLUMNS * TEXT_ROWS)

start:
    cli
    cld
    jmp second_half

    times 510-($-$$) db 0
    dw 0xaa55

second_half:
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

    ; NP2V v1 header. The state byte is published last for each phase.
    mov word [es:0], 0x504e
    mov word [es:2], 0x5632
    mov word [es:4], 1
    mov word [es:6], 16
    mov word [es:8], CONTROL_SIZE
    mov word [es:10], 1
    mov word [es:12], 0
    mov byte [es:CONTROL_STATE_OFFSET], STATE_BOOTING
    mov byte [es:CONTROL_STATE_OFFSET], STATE_PROGRAMMING_VIDEO

    ; INT 18h AH=0Ah, AL=0 selects the 80-column 16-raster text mode.
    mov ax, 0x0a00
    int 0x18
    mov ah, 0x0c
    int 0x18

    ; Clear 80x25 character and attribute cells. Attributes are visible white.
    mov ax, TEXT_SEGMENT
    mov es, ax
    xor di, di
    mov ax, 0x0020
    mov cx, TEXT_CELLS
    rep stosw

    mov ax, ATTRIBUTE_SEGMENT
    mov es, ax
    xor di, di
    mov ax, 0x00e1
    mov cx, TEXT_CELLS
    rep stosw

    push cs
    pop ds
    mov ax, TEXT_SEGMENT
    mov es, ax

    mov si, border
    mov di, 0
    mov cx, border_length
    call write_string
    mov si, border
    mov di, 2 * (24 * TEXT_COLUMNS)
    mov cx, border_length
    call write_string

    ; Put the left and right border characters on the 23 interior rows.
    mov di, 2 * TEXT_COLUMNS
    mov bx, 23
write_borders:
    mov al, '|'
    call write_char
    mov ax, di
    add ax, 158
    mov di, ax
    mov al, '|'
    call write_char
    add di, 2
    dec bx
    jnz write_borders

    mov si, title
    mov di, 2 * (2 * TEXT_COLUMNS + 4)
    mov cx, title_length
    call write_string

    mov si, vram_label
    mov di, 2 * (4 * TEXT_COLUMNS + 4)
    mov cx, vram_label_length
    call write_string

    mov si, left_label
    mov di, 2 * (6 * TEXT_COLUMNS + 2)
    mov cx, left_label_length
    call write_string

    mov si, right_label
    mov di, 2 * (6 * TEXT_COLUMNS + 70)
    mov cx, right_label_length
    call write_string

    mov si, alphabet
    mov di, 2 * (10 * TEXT_COLUMNS + 10)
    mov cx, alphabet_length
    call write_string

    mov si, digits
    mov di, 2 * (12 * TEXT_COLUMNS + 20)
    mov cx, digits_length
    call write_string

    mov si, coordinates
    mov di, 2 * (18 * TEXT_COLUMNS + 8)
    mov cx, coordinates_length
    call write_string

    mov si, bottom_label
    mov di, 2 * (22 * TEXT_COLUMNS + 60)
    mov cx, bottom_label_length
    call write_string

    ; The scene is complete. Publish READY only after every VRAM write.
    mov ax, CONTROL_SEGMENT
    mov es, ax
    mov byte [es:CONTROL_STATE_OFFSET], STATE_SCENE_READY

idle:
    cli
    hlt
    jmp idle

write_string:
write_string_loop:
    lodsb
    xor ah, ah
    stosw
    loop write_string_loop
    ret

write_char:
    mov [es:di], al
    mov byte [es:di + 1], 0
    ret

border:
    db '+------------------------------------------------------------------------------+'
border_length equ $ - border

title:
    db 'NP2 VIDEO FIXTURE 7A.3A'
title_length equ $ - title

vram_label:
    db 'TEXT VRAM A0000  ATTR A2000'
vram_label_length equ $ - vram_label

left_label:
    db 'LEFT'
left_label_length equ $ - left_label

right_label:
    db 'RIGHT'
right_label_length equ $ - right_label

alphabet:
    db 'ABCDEFGHIJKLMNOPQRSTUVWXYZ'
alphabet_length equ $ - alphabet

digits:
    db '0123456789'
digits_length equ $ - digits

coordinates:
    db 'X=LEFT  Y=TOP'
coordinates_length equ $ - coordinates

bottom_label:
    db 'BOTTOM'
bottom_label_length equ $ - bottom_label

    times 1022-($-$$) db 0
    dw 0xaa55
