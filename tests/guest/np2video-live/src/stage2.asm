; SPDX-License-Identifier: BSD-2-Clause
; Step 7B.2d-1: guest-generated direct-VRAM moving-boundary workload.

bits 16
cpu 8086
org 0

%define CONTROL_SEGMENT 0x2a00
%define CONTROL_SIZE 32
%define CONTROL_STATE_OFFSET 31
%define PLANE_WORDS 0x4000
%define BAND_BYTES 8
%define ROW_BYTES 80

%define STATE_BOOTING 1
%define STATE_PROGRAMMING_VIDEO 2
%define STATE_SCENE_READY 3

stage2_header:
    db "ST2V"
    dw 1
    dw stage2_end - $$

stage2_entry:
    cli
    cld
    mov ax, 0x2800
    mov ss, ax
    mov sp, 0x1000
    push cs
    pop ds

    mov ax, CONTROL_SEGMENT
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, CONTROL_SIZE / 2
    rep stosw
    mov word [es:0], 0x504e
    mov word [es:2], 0x5632
    mov word [es:4], 1
    mov word [es:6], 16
    mov word [es:8], CONTROL_SIZE
    mov word [es:10], 2
    mov word [es:12], 0
    mov byte [es:CONTROL_STATE_OFFSET], STATE_BOOTING
    mov byte [es:CONTROL_STATE_OFFSET], STATE_PROGRAMMING_VIDEO

    ; The same source-backed analog 640x400 mode as the Step 7A direct-VRAM
    ; fixture. Every animated pixel below is written through the four NP2
    ; graphics VRAM windows, so the normal NP2 renderer remains authoritative.
    mov ax, 0x0a00
    int 0x18
    mov ah, 0x42
    mov cx, 0xc000
    int 0x18
    mov ah, 0x0d
    int 0x18
    mov dx, 0x006a
    mov al, 1
    out dx, al
    call set_palette
    mov dx, 0x00a4
    xor al, al
    out dx, al
    mov dx, 0x00a6
    out dx, al
    mov ah, 0x40
    int 0x18

    ; BIOS mode services may use the low-memory scratch area. Republish the
    ; NP2V header after mode setup so the scene identity is an unambiguous
    ; guest-owned commit record.
    mov ax, CONTROL_SEGMENT
    mov es, ax
    mov word [es:0], 0x504e
    mov word [es:2], 0x5632
    mov word [es:4], 1
    mov word [es:6], 16
    mov word [es:8], CONTROL_SIZE
    mov word [es:10], 2
    mov word [es:12], 0
    mov byte [es:CONTROL_STATE_OFFSET], STATE_PROGRAMMING_VIDEO

    call clear_planes
    mov byte [bar_pos], 8
    mov byte [bar_direction], 1
    call draw_bar

    ; SCENE_READY is the initialization barrier. The runner may start the
    ; benchmark only after observing this state and the first rendered scene.
    mov ax, CONTROL_SEGMENT
    mov es, ax
    mov byte [es:CONTROL_STATE_OFFSET], STATE_SCENE_READY

frame_loop:
    ; The guest remains free-running and never waits for the host consumer.
    ; Each iteration performs real VRAM writes; NP2's scheduler supplies the
    ; normal render/event boundary used by scrnmng.
    call move_bar
    jmp frame_loop

set_palette:
    push cs
    pop ds
    mov si, palette
    xor bx, bx
    mov cx, 16
palette_loop:
    mov dx, 0x00a8
    mov al, bl
    out dx, al
    lodsw
    mov dx, 0x00aa
    mov al, ah
    and al, 0x0f
    out dx, al
    mov dx, 0x00ac
    mov al, byte [si - 2]
    and al, 0xf0
    shr al, 1
    shr al, 1
    shr al, 1
    shr al, 1
    out dx, al
    mov dx, 0x00ae
    mov al, byte [si - 2]
    and al, 0x0f
    out dx, al
    inc bl
    loop palette_loop
    ret

clear_planes:
    mov ax, 0xa800
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, PLANE_WORDS
    rep stosw
    mov ax, 0xb000
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, PLANE_WORDS
    rep stosw
    mov ax, 0xb800
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, PLANE_WORDS
    rep stosw
    mov ax, 0xe000
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, PLANE_WORDS
    rep stosw
    ret

; Fill BAND_BYTES at bar_pos for all 400 rows in one selected plane.
; AX is the repeated byte value (0000h for erase, ffffh for set).
fill_band_plane:
    mov di, 0
    mov dl, [bar_pos]
    mov dh, 0
    mov di, dx
    mov dx, 400
fill_band_row:
    push dx
    mov cx, BAND_BYTES / 2
    rep stosw
    pop dx
    add di, ROW_BYTES - BAND_BYTES
    dec dx
    jnz fill_band_row
    ret

erase_bar:
    xor ax, ax
    mov bx, 0xa800
    mov es, bx
    call fill_band_plane
    mov bx, 0xb000
    mov es, bx
    call fill_band_plane
    mov bx, 0xb800
    mov es, bx
    call fill_band_plane
    mov bx, 0xe000
    mov es, bx
    call fill_band_plane
    ret

draw_bar:
    ; Palette index 12 (green in the GRB table) gives a high-contrast band.
    ; Set B, R, and E planes; leave G clear.
    mov ax, 0xffff
    mov bx, 0xa800
    mov es, bx
    call fill_band_plane
    mov bx, 0xb000
    mov es, bx
    call fill_band_plane
    xor ax, ax
    mov bx, 0xb800
    mov es, bx
    call fill_band_plane
    mov ax, 0xffff
    mov bx, 0xe000
    mov es, bx
    call fill_band_plane
    ret

move_bar:
    call erase_bar
    mov al, [bar_pos]
    add al, [bar_direction]
    cmp al, 8
    jae check_right
    mov byte [bar_direction], 1
    mov al, 9
    jmp store_position
check_right:
    cmp al, 68
    jbe store_position
    mov byte [bar_direction], -1
    mov al, 67
store_position:
    mov [bar_pos], al
    call draw_bar
    ret

bar_pos db 8
bar_direction db 1
palette:
    dw 0x000, 0x007, 0x070, 0x077
    dw 0x700, 0x707, 0x770, 0x777
    dw 0x444, 0x00f, 0x0f0, 0x0ff
    dw 0xf00, 0xf0f, 0xff0, 0xfff

stage2_end:
