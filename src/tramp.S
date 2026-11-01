// SPDX-License-Identifier: GPL-3.0-or-later
// The common half of a host-callable guest function (cpu.c host_callable): each stub loads its guest address into r11
// and jumps here. The System V arguments are saved in the AAPCS64 order they map to (integer arguments 1-6 from
// rdi..r9, 7-8 from the caller's stack slots, floating-point 1-8 from xmm0-7), nxl_tramp_call runs the guest function on
// the calling thread's CPU and writes x0, x1 and v0 back into the same buffer, which become rax, rdx and xmm0.
    .text
    .globl  nxl_tramp_common
    .type   nxl_tramp_common, @function
nxl_tramp_common:
    .cfi_startproc
    push    %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp, -16
    mov     %rsp, %rbp
    .cfi_def_cfa_register %rbp
    sub     $192, %rsp                  // 8 integer slots (64 bytes), then 8 vector slots (128 bytes)
    mov     %rdi, 0(%rsp)
    mov     %rsi, 8(%rsp)
    mov     %rdx, 16(%rsp)
    mov     %rcx, 24(%rsp)
    mov     %r8, 32(%rsp)
    mov     %r9, 40(%rsp)
    mov     16(%rbp), %rax              // arguments 7 and 8 on the stack (harmless reads when the caller passed fewer)
    mov     %rax, 48(%rsp)
    mov     24(%rbp), %rax
    mov     %rax, 56(%rsp)
    movdqu  %xmm0, 64(%rsp)
    movdqu  %xmm1, 80(%rsp)
    movdqu  %xmm2, 96(%rsp)
    movdqu  %xmm3, 112(%rsp)
    movdqu  %xmm4, 128(%rsp)
    movdqu  %xmm5, 144(%rsp)
    movdqu  %xmm6, 160(%rsp)
    movdqu  %xmm7, 176(%rsp)
    mov     %r11, %rdi                  // the guest function
    lea     0(%rsp), %rsi
    lea     64(%rsp), %rdx
    call    nxl_tramp_call
    mov     0(%rsp), %rax
    mov     8(%rsp), %rdx
    movdqu  64(%rsp), %xmm0
    leave
    .cfi_def_cfa %rsp, 8
    ret
    .cfi_endproc
    .size   nxl_tramp_common, .-nxl_tramp_common
    .section .note.GNU-stack, "", @progbits
