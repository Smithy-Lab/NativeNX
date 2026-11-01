// SPDX-License-Identifier: GPL-3.0-or-later
// tramp.S for Windows (the Microsoft x64 calling convention): each host-callable stub loads its guest address into r11
// and jumps here. Windows passes the first four arguments by position, an integer in rcx, rdx, r8, r9 or a floating-
// point one in xmm0-xmm3, and the rest on the caller's stack after the 32 bytes of home space. A pointer from the host
// cannot say which of its arguments are floating-point, so: x0-x3 from rcx..r9, x4-x7 from stack slots 5-8, v0-v3
// from xmm0-xmm3 (the integer-only callbacks and the floating-point-only ones map as AAPCS64 has them; a callback that
// mixes the two in its first four needs an explicit GUEST_CALL, as do results wider than a register: Windows returns
// those through a pointer). x0 and v0 come back in rax and xmm0.
    .text
    .globl  nxl_tramp_common
    .def    nxl_tramp_common; .scl 2; .type 32; .endef
    .seh_proc nxl_tramp_common
nxl_tramp_common:
    push    %rbp
    .seh_pushreg %rbp
    mov     %rsp, %rbp
    .seh_setframe %rbp, 0
    sub     $224, %rsp                  // 32 bytes of home space for the call, 8 integer slots, 8 vector slots
    .seh_stackalloc 224
    .seh_endprologue
    mov     %rcx, 32(%rsp)
    mov     %rdx, 40(%rsp)
    mov     %r8, 48(%rsp)
    mov     %r9, 56(%rsp)
    mov     48(%rbp), %rax              // arguments 5-8 on the caller's stack (harmless reads when it passed fewer)
    mov     %rax, 64(%rsp)
    mov     56(%rbp), %rax
    mov     %rax, 72(%rsp)
    mov     64(%rbp), %rax
    mov     %rax, 80(%rsp)
    mov     72(%rbp), %rax
    mov     %rax, 88(%rsp)
    movdqu  %xmm0, 96(%rsp)
    movdqu  %xmm1, 112(%rsp)
    movdqu  %xmm2, 128(%rsp)
    movdqu  %xmm3, 144(%rsp)
    pxor    %xmm4, %xmm4
    movdqu  %xmm4, 160(%rsp)
    movdqu  %xmm4, 176(%rsp)
    movdqu  %xmm4, 192(%rsp)
    movdqu  %xmm4, 208(%rsp)
    mov     %r11, %rcx                  // the guest function
    lea     32(%rsp), %rdx
    lea     96(%rsp), %r8
    call    nxl_tramp_call
    mov     32(%rsp), %rax
    mov     40(%rsp), %rdx
    movdqu  96(%rsp), %xmm0
    mov     %rbp, %rsp
    pop     %rbp
    ret
    .seh_endproc
