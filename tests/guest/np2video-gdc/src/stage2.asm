; SPDX-License-Identifier: BSD-2-Clause
; NP2 video fixture Step 7A.3d: deterministic GDC drawing-command scene.

bits 16
cpu 8086
org 0

%define CONTROL_SEGMENT 0x2a00
%define CONTROL_SIZE 32
%define CONTROL_STATE_OFFSET 31

%define STATE_BOOTING 1
%define STATE_PROGRAMMING_VIDEO 2
%define STATE_SCENE_READY 3
%define STATE_ERROR 4

%define GDC_DATA_PORT 0x00a0
%define GDC_COMMAND_PORT 0x00a2
%define GDC_DISPLAY_PAGE_PORT 0x00a4
%define GDC_ACCESS_PAGE_PORT 0x00a6
%define GDC_ANALOG_PORT 0x006a
%define GDC_STATUS_BUSY 0x08
%define GDC_STATUS_FIFO_FULL 0x02
%define GDC_STATUS_FIFO_EMPTY 0x04
%define GDC_WAIT_LIMIT 0xffff
%define GDC_WAIT_OUTER 64
%define GDC_FIFO_CAPACITY 32

%define PLANE_WORDS 0x4000

; The raw stage2 loader enters here at physical 2000:0008.
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

    ; NP2V v1 header. The state byte is published last for each phase.
    mov word [es:0], 0x504e
    mov word [es:2], 0x5632
    mov word [es:4], 1
    mov word [es:6], 16
    mov word [es:8], CONTROL_SIZE
    mov word [es:10], 3
    mov word [es:12], 0
    mov byte [es:CONTROL_STATE_OFFSET], STATE_BOOTING
    mov byte [es:CONTROL_STATE_OFFSET], STATE_PROGRAMMING_VIDEO

    ; Source-backed PC-98 graphics setup. CH is deliberately C0h.
    mov ax, 0x0a00
    int 0x18
    mov ah, 0x42
    mov cx, 0xc000
    int 0x18
    mov ah, 0x0d
    int 0x18

    ; Enable analog 16-color access, then program every palette entry.
    mov dx, GDC_ANALOG_PORT
    mov al, 1
    out dx, al
    push cs
    pop ds
    call set_palette

    ; Select display/access page zero explicitly before graphics display.
    mov dx, GDC_DISPLAY_PAGE_PORT
    xor al, al
    out dx, al
    mov dx, GDC_ACCESS_PAGE_PORT
    out dx, al
    mov ah, 0x40
    int 0x18

    push cs
    pop ds
    call clear_planes
    call gdc_prepare
    jc gdc_timeout_error
    call draw_scene
    jc gdc_timeout_error

    ; READY is the final guest-visible write.
    mov ax, CONTROL_SEGMENT
    mov es, ax
    mov byte [es:CONTROL_STATE_OFFSET], STATE_SCENE_READY

idle:
    cli
    hlt
    jmp idle

; Program all 16 analog entries. The table uses the source-documented GRB
; nibble order: green at bits 8..11, red at bits 4..7, blue at bits 0..3.
set_palette:
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
    mov al, 0x0f
    and al, byte [si - 2]
    out dx, al
    inc bl
    loop palette_loop
    ret

; Clear all four page-0 planes through their CPU VRAM windows. No visible
; geometry is written by the CPU; every scene pixel below is a GDC vector.
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

; The source-backed A2 command write invokes gdc_work immediately and drains
; any pending parameter bytes. Track the largest pending parameter group in
; software as an additional guard: the longest group below is VECTW's 11
; bytes, well below GDCCMD_MAX=32. No write is attempted once the guarded
; count reaches the source FIFO capacity, so bytes cannot be silently dropped.
gdc_wait_fifo_space:
    cmp byte [gdc_fifo_count], GDC_FIFO_CAPACITY - 1
    jb .ready
    stc
    ret
.ready:
    clc
    ret

gdc_wait_complete:
    mov dx, GDC_DATA_PORT
    mov bx, GDC_WAIT_OUTER
.outer:
    mov cx, GDC_WAIT_LIMIT
.wait:
    in al, dx
    test al, GDC_STATUS_BUSY
    jnz .next
    test al, GDC_STATUS_FIFO_EMPTY
    jnz .ready
.next:
    loop .wait
    dec bx
    jnz .outer
    stc
    ret
.ready:
    clc
    ret

; AL is the command or parameter. Each helper preserves the byte while the
; bounded FIFO check reads A0, then writes only to the source-backed port.
gdc_send_command:
    push ax
    call gdc_wait_fifo_space
    pop ax
    jc .fail
    mov dx, GDC_COMMAND_PORT
    out dx, al
    ; A2 command processing consumes the previous parameter group.
    mov byte [gdc_fifo_count], 0
    clc
    ret
.fail:
    stc
    ret

gdc_send_parameter:
    push ax
    call gdc_wait_fifo_space
    pop ax
    jc .fail
    mov dx, GDC_DATA_PORT
    out dx, al
    inc byte [gdc_fifo_count]
    clc
    ret
.fail:
    stc
    ret

gdc_prepare:
    ; Finish any BIOS-side slave work before starting the first full primitive.
    call gdc_wait_complete
    jc .fail

    ; 0x2b is persistent SET: it selects operation=SET and does not draw.
    mov al, 0x2b
    call gdc_send_command
    jc .fail

    ; TEXTW is a complete eight-byte block. The first two bytes are 0xffff;
    ; the remaining deterministic bytes are deliberately never rendered.
    mov al, 0x78
    call gdc_send_command
    jc .fail
    mov si, textw_pattern
    mov cx, 8
.textw:
    mov al, [si]
    inc si
    call gdc_send_parameter
    jc .fail
    loop .textw
    call gdc_wait_complete
    ret
.fail:
    stc
    ret

; Compute CSRW from readable x/y/plane fields. The vendor decoder uses:
;   word = y*40 + (x>>4), plane = (csrw>>14)&3, bit = x&0xf.
; Therefore the three little-endian bytes are word+(plane<<14), then bit<<4.
gdc_compute_csrw:
    mov ax, [line_y1]
    mov bx, 40
    mul bx
    mov bx, ax

    mov ax, [line_x1]
    mov dx, ax
    shr ax, 1
    shr ax, 1
    shr ax, 1
    shr ax, 1
    add bx, ax

    and dx, 0x000f
    mov ax, dx
    shl ax, 1
    shl ax, 1
    shl ax, 1
    shl ax, 1
    mov [csrw_high], al

    xor ax, ax
    mov al, [line_plane]
    shl ax, 1
    shl ax, 1
    shl ax, 1
    shl ax, 1
    shl ax, 1
    shl ax, 1
    shl ax, 1
    shl ax, 1
    shl ax, 1
    shl ax, 1
    shl ax, 1
    shl ax, 1
    shl ax, 1
    shl ax, 1
    add ax, bx
    mov [csrw_low], ax
    ret

gdc_set_csrw:
    call gdc_compute_csrw
    mov al, 0x49
    call gdc_send_command
    jc .fail
    mov al, byte [csrw_low]
    call gdc_send_parameter
    jc .fail
    mov al, byte [csrw_low + 1]
    call gdc_send_parameter
    jc .fail
    mov al, [csrw_high]
    call gdc_send_parameter
    ret
.fail:
    stc
    ret

; VECTW fields are named in the fixture's line table. The values follow the
; audited gdcsub_vectl convention: DC=major, D=minor-major,
; D2=minor-2*major, D1=2*minor, DM=0.
gdc_set_vectw:
    mov al, 0x4c
    call gdc_send_command
    jc .fail
    mov al, [line_ope]
    call gdc_send_parameter
    jc .fail
    mov ax, [line_dc]
    call gdc_send_word
    jc .fail
    mov ax, [line_d]
    call gdc_send_word
    jc .fail
    mov ax, [line_d2]
    call gdc_send_word
    jc .fail
    mov ax, [line_d1]
    call gdc_send_word
    jc .fail
    mov ax, [line_dm]
    call gdc_send_word
    ret
.fail:
    stc
    ret

gdc_send_word:
    push ax
    call gdc_send_parameter
    pop ax
    jc .fail
    mov al, ah
    call gdc_send_parameter
    ret
.fail:
    stc
    ret

gdc_execute_vector:
    mov al, 0x6c
    call gdc_send_command
    jc .fail
    ; VECTE schedules the vendor's drawing event. Do not start another
    ; primitive until busy is clear and FIFO empty is reported.
    call gdc_wait_complete
    ret
.fail:
    stc
    ret

gdc_draw_line_plane:
    call gdc_set_csrw
    jc .fail
    call gdc_set_vectw
    jc .fail
    call gdc_execute_vector
    ret
.fail:
    stc
    ret

; Palette bits are B/R/G/E = bit0/bit1/bit2/bit3. GDC plane selectors are
; 0=E, 1=B, 2=R, 3=G, so each selected plane is explicitly redrawn.
gdc_draw_line_color:
    mov byte [line_plane], 0
.plane:
    xor bx, bx
    mov bl, [line_plane]
    mov si, bx
    mov al, [plane_color_bits + si]
    test byte [line_color], al
    jz .next_plane
    call gdc_draw_line_plane
    jc .fail
.next_plane:
    inc byte [line_plane]
    cmp byte [line_plane], 4
    jb .plane
    clc
    ret
.fail:
    stc
    ret

%macro DRAW_LINE 11
    mov word [line_x1], %1
    mov word [line_y1], %2
    mov word [line_x2], %3
    mov word [line_y2], %4
    mov byte [line_color], %5
    mov byte [line_ope], %6
    mov word [line_dc], %7
    mov word [line_d], %8
    mov word [line_d2], %9
    mov word [line_d1], %10
    mov word [line_dm], %11
    call gdc_draw_line_color
    jc gdc_timeout_error
%endmacro

draw_scene:
    ; A: white frame. Horizontal +x is ope=0x0a; vertical down is 0x0f;
    ; vertical up is 0x0b? The audited low direction is 3, hence 0x0b.
    DRAW_LINE 8,   8, 631,   8, 15, 0x0a, 623,  -623, -1246,   0, 0
    DRAW_LINE 631, 8, 631, 391, 15, 0x0f, 383,  -383,  -766,   0, 0
    DRAW_LINE 631,391,   8, 391, 15, 0x0e, 623,  -623, -1246,   0, 0
    DRAW_LINE 8, 391,   8,   8, 15, 0x0b, 383,  -383,  -766,   0, 0

    ; B: blue +x; C: red reverse horizontal -x.
    DRAW_LINE 37, 56, 230, 56, 1, 0x0a, 193,  -193,  -386,   0, 0
    DRAW_LINE 602,88, 397, 88, 2, 0x0e, 205,  -205,  -410,   0, 0

    ; D: green down; E: gray up.
    DRAW_LINE 73,120, 73,300, 4, 0x0f, 180,  -180,  -360,   0, 0
    DRAW_LINE 559,322,559,154, 8, 0x0b, 168,  -168,  -336,   0, 0

    ; F: bright-magenta down-right, dx=140 dy=85, ope=0x09.
    DRAW_LINE 100,260,240,345, 11, 0x09, 140,    30,  -110, 170, 0

    ; G: bright-cyan up-left, dx=-160 dy=-100, ope=0x0d.
    DRAW_LINE 550,330,390,230, 13, 0x0d, 160,    40,  -120, 200, 0

    ; H: yellow rectangle, four explicit VECTL segments.
    DRAW_LINE 270,120,460,120, 14, 0x0a, 190,  -190,  -380,   0, 0
    DRAW_LINE 460,120,460,200, 14, 0x0f,  80,   -80,  -160,   0, 0
    DRAW_LINE 460,200,270,200, 14, 0x0e, 190,  -190,  -380,   0, 0
    DRAW_LINE 270,200,270,120, 14, 0x0b,  80,   -80,  -160,   0, 0
    clc
    ret

gdc_timeout_error:
    mov ax, CONTROL_SEGMENT
    mov es, ax
    mov word [es:12], 0x0301
    mov byte [es:CONTROL_STATE_OFFSET], STATE_ERROR
gdc_error_halt:
    cli
    hlt
    jmp gdc_error_halt

line_x1 dw 0
line_y1 dw 0
line_x2 dw 0
line_y2 dw 0
line_color db 0
line_plane db 0
line_ope db 0
line_dc dw 0
line_d dw 0
line_d2 dw 0
line_d1 dw 0
line_dm dw 0
csrw_low dw 0
csrw_high db 0
gdc_fifo_count db 0

plane_color_bits db 8, 1, 2, 4
textw_pattern db 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00

palette:
    dw 0x000, 0x007, 0x070, 0x077
    dw 0x700, 0x707, 0x770, 0x777
    dw 0x444, 0x00f, 0x0f0, 0x0ff
    dw 0xf00, 0xf0f, 0xff0, 0xfff

stage2_end:
