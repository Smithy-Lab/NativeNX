// SPDX-License-Identifier: GPL-3.0-or-later
// Start of a test module: a branch over the MOD0 header (at text+4, its offset, as every Switch module has), then the
// entry: x0 = 0 and x1 = the main thread's handle, as Horizon starts a program. test_main's result ends the process.
.section .text.crt0, "ax"
.global _start
.hidden __bss_start
.hidden __bss_end
.hidden _DYNAMIC
_start:
    b       1f
    .word   __nx_mod0 - _start
.align 2
__nx_mod0:
    .ascii  "MOD0"
    .word   _DYNAMIC - __nx_mod0
    .word   __bss_start - __nx_mod0
    .word   __bss_end - __nx_mod0
    .word   0, 0                    // eh_frame_hdr (none)
    .word   0                       // module object
1:
    mov     w0, w1
    bl      __nx_start
    brk     #0                      // __nx_start ends the process
