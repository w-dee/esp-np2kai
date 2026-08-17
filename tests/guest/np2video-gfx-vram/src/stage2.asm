; SPDX-License-Identifier: BSD-2-Clause
; NP2 video fixture Step 7A.3c: deterministic direct-VRAM graphics scene.

bits 16
cpu 8086
org 0

%define CONTROL_SEGMENT 0x2a00
%define CONTROL_SIZE 32
%define CONTROL_STATE_OFFSET 31

%define STATE_BOOTING 1
%define STATE_PROGRAMMING_VIDEO 2
%define STATE_SCENE_READY 3

%define PLANE_BYTES 0x4000

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
    mov word [es:10], 2
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
    mov dx, 0x006a
    mov al, 1
    out dx, al
    push cs
    pop ds
    call set_palette

    ; Select display/access page zero explicitly before graphics display.
    mov dx, 0x00a4
    xor al, al
    out dx, al
    mov dx, 0x00a6
    out dx, al
    mov ah, 0x40
    int 0x18

    call clear_planes
    call draw_scene

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
    mov al, ah
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

; Clear all four page-0 planes through their CPU VRAM windows.
clear_planes:
    mov ax, 0xa800
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, PLANE_BYTES
    rep stosw
    mov ax, 0xb000
    mov es, ax
    xor di, di
    mov cx, PLANE_BYTES
    rep stosw
    mov ax, 0xb800
    mov es, ax
    xor di, di
    mov cx, PLANE_BYTES
    rep stosw
    mov ax, 0xe000
    mov es, ax
    xor di, di
    mov cx, PLANE_BYTES
    rep stosw
    ret

; Plot one pixel. Input: CX=x, DX=y, AL=palette index. The four plane
; segments are B/R/G/E, and bit 7 is the leftmost pixel in each byte.
plot_pixel:
    mov bh, al
    mov ax, cx
    mov si, ax
    shr si, 1
    shr si, 1
    shr si, 1
    and cx, 7
    mov bl, 0x80
    shr bl, cl
    mov ax, dx
    mov di, 80
    mul di
    add ax, si
    mov di, ax
    mov si, plane_segments
    mov bp, 4
plot_plane:
    mov ax, [cs:si]
    mov es, ax
    test bh, 1
    jz clear_pixel_bit
    or [es:di], bl
    jmp next_pixel_plane
clear_pixel_bit:
    mov al, bl
    not al
    and [es:di], al
next_pixel_plane:
    shr bh, 1
    add si, 2
    dec bp
    jnz plot_plane
    ret

; Fill a half-open rectangle. Input: CX=x0, DX=x1, SI=y0, DI=y1, AL=color.
fill_call:
    xor ah, ah
    mov [rect_color], ax
    mov [rect_x0], cx
    mov [rect_x1], dx
    mov [rect_y0], si
    mov [rect_y1], di
    call fill_rect
    ret

fill_rect:
    mov dx, [rect_y0]
fill_row:
    mov cx, [rect_x0]
fill_column:
    push cx
    push dx
    mov ax, [rect_color]
    call plot_pixel
    pop dx
    pop cx
    inc cx
    cmp cx, [rect_x1]
    jb fill_column
    inc dx
    cmp dx, [rect_y1]
    jb fill_row
    ret

draw_scene:
    ; One-pixel white outer frame, including the intentionally unaligned right.
    mov cx, 8
    mov dx, 632
    mov si, 8
    mov di, 9
    mov al, 15
    call fill_call
    mov cx, 8
    mov dx, 632
    mov si, 391
    mov di, 392
    mov al, 15
    call fill_call
    mov cx, 8
    mov dx, 9
    mov si, 8
    mov di, 392
    mov al, 15
    call fill_call
    mov cx, 631
    mov dx, 632
    mov si, 8
    mov di, 392
    mov al, 15
    call fill_call

    ; Corner markers: blue, red, green, and a gray L.
    mov cx, 24
    mov dx, 48
    mov si, 24
    mov di, 48
    mov al, 1
    call fill_call
    mov cx, 584
    mov dx, 616
    mov si, 24
    mov di, 40
    mov al, 2
    call fill_call
    mov cx, 24
    mov dx, 40
    mov si, 344
    mov di, 376
    mov al, 4
    call fill_call
    mov cx, 584
    mov dx, 616
    mov si, 344
    mov di, 352
    mov al, 8
    call fill_call
    mov cx, 608
    mov dx, 616
    mov si, 344
    mov di, 376
    mov al, 8
    call fill_call

    ; Four-by-four row-major palette swatches, each with a white border.
    xor ax, ax
    mov [sw_row], ax
    mov [sw_index], ax
swatch_row:
    mov [sw_col], ax
swatch_column:
    mov ax, [sw_col]
    mov cl, 5
    shl ax, cl
    add ax, 96
    mov [sw_x], ax
    mov ax, [sw_row]
    mov cl, 4
    shl ax, cl
    mov dx, [sw_row]
    shl dx, 1
    add ax, dx
    add ax, 96
    mov [sw_y], ax

    mov cx, [sw_x]
    mov dx, cx
    add dx, 28
    mov si, [sw_y]
    mov di, si
    add di, 18
    mov al, 15
    call fill_call

    mov cx, [sw_x]
    inc cx
    mov dx, cx
    add dx, 26
    mov si, [sw_y]
    inc si
    mov di, si
    add di, 16
    mov ax, [sw_index]
    call fill_call

    inc word [sw_index]
    inc word [sw_col]
    cmp word [sw_col], 4
    jb swatch_column
    inc word [sw_row]
    mov ax, [sw_row]
    cmp ax, 4
    jb swatch_row

    ; Central white cross.
    mov cx, 256
    mov dx, 512
    mov si, 239
    mov di, 240
    mov al, 15
    call fill_call
    mov cx, 319
    mov dx, 320
    mov si, 208
    mov di, 336
    mov al, 15
    call fill_call
    ret

rect_x0 dw 0
rect_x1 dw 0
rect_y0 dw 0
rect_y1 dw 0
rect_color dw 0
sw_row dw 0
sw_col dw 0
sw_index dw 0
sw_x dw 0
sw_y dw 0

plane_segments:
    dw 0xa800, 0xb000, 0xb800, 0xe000

palette:
    dw 0x000, 0x007, 0x070, 0x077
    dw 0x700, 0x707, 0x770, 0x777
    dw 0x444, 0x00f, 0x0f0, 0x0ff
    dw 0xf00, 0xf0f, 0xff0, 0xfff

stage2_end:
