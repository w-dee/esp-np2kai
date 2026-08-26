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

    .align  4
    .global exact2x_pie_tile128_aligned
    .type   exact2x_pie_tile128_aligned, @function
exact2x_pie_tile128_aligned:
    li      a5, 128
.Lexact2x_tile128_row:
    mv      a2, a1
    addi    a3, a1, 1600
    li      a4, 50
.Lexact2x_tile128_group:
    esp.vld.128.ip  q0, a0, 16
    esp.orq         q1, q0, q0
    esp.vzip.16     q0, q1
    esp.vst.128.ip  q0, a2, 16
    esp.vst.128.ip  q1, a2, 16
    esp.vst.128.ip  q0, a3, 16
    esp.vst.128.ip  q1, a3, 16
    addi    a4, a4, -1
    bnez    a4, .Lexact2x_tile128_group
    mv      a1, a3
    addi    a5, a5, -1
    bnez    a5, .Lexact2x_tile128_row
    ret
    .size exact2x_pie_tile128_aligned, .-exact2x_pie_tile128_aligned

    .align  4
    .global exact2x_pie_tile128_grouped64_aligned
    .type   exact2x_pie_tile128_grouped64_aligned, @function
exact2x_pie_tile128_grouped64_aligned:
    li      a5, 128
.Lexact2x_grouped64_row:
    mv      a2, a1
    addi    a3, a1, 1600
    li      a4, 25
.Lexact2x_grouped64_group:
    esp.vld.128.ip  q0, a0, 16
    esp.orq         q1, q0, q0
    esp.vzip.16     q0, q1
    esp.vld.128.ip  q2, a0, 16
    esp.orq         q4, q2, q2
    esp.vzip.16     q2, q4
    esp.vst.128.ip  q0, a2, 16
    esp.vst.128.ip  q1, a2, 16
    esp.vst.128.ip  q2, a2, 16
    esp.vst.128.ip  q4, a2, 16
    esp.vst.128.ip  q0, a3, 16
    esp.vst.128.ip  q1, a3, 16
    esp.vst.128.ip  q2, a3, 16
    esp.vst.128.ip  q4, a3, 16
    addi    a4, a4, -1
    bnez    a4, .Lexact2x_grouped64_group
    mv      a1, a3
    addi    a5, a5, -1
    bnez    a5, .Lexact2x_grouped64_row
    ret
    .size exact2x_pie_tile128_grouped64_aligned, .-exact2x_pie_tile128_grouped64_aligned

    .align  4
    .global exact2x_pie_preemption_clobber_q0_q1
    .type   exact2x_pie_preemption_clobber_q0_q1, @function
exact2x_pie_preemption_clobber_q0_q1:
    esp.vld.128.ip  q0, a0, 16
    esp.orq         q1, q0, q0
    esp.vst.128.ip  q0, a1, 16
    esp.vst.128.ip  q1, a1, 16
    ret
    .size exact2x_pie_preemption_clobber_q0_q1, .-exact2x_pie_preemption_clobber_q0_q1
