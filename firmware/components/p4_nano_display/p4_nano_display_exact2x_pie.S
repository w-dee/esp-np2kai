/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 *
 * Fixed 400x640 RGB565 -> 800x1280 RGB565 nearest-neighbor exact-2x kernel.
 * a0: source, a1: destination. Only q0/q1 and caller-saved scalar registers
 * are used; cache and pointer-contract handling deliberately remain in C++.
 */

    .text
    .align  4
    .global exact2x_pie_aligned
    .type   exact2x_pie_aligned, @function
exact2x_pie_aligned:
    li      a5, 640
.Lexact2x_row:
    mv      a2, a1
    addi    a3, a1, 1600
    li      a4, 50
.Lexact2x_group:
    esp.vld.128.ip  q0, a0, 16
    esp.orq         q1, q0, q0
    esp.vzip.16     q0, q1
    esp.vst.128.ip  q0, a2, 16
    esp.vst.128.ip  q1, a2, 16
    esp.vst.128.ip  q0, a3, 16
    esp.vst.128.ip  q1, a3, 16
    addi    a4, a4, -1
    bnez    a4, .Lexact2x_group
    mv      a1, a3
    addi    a5, a5, -1
    bnez    a5, .Lexact2x_row
    ret
    .size exact2x_pie_aligned, .-exact2x_pie_aligned
