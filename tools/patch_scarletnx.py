#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Changes nxlinux needs in ScarletNX's recompiler, applied to its source before it is built (CMakeLists.txt runs this as
# FetchContent's PATCH_COMMAND; tools/README.md). Each replacement must match exactly once, or already be applied.
#
# A store-exclusive narrower than the load-exclusive that armed the monitor, to the same address. Compilers emit it for a
# 32-bit atomic read with a 64-bit load: Breath of the Wild 1.9.0 increments a counter with ldxr x / add w / stxr w
# (main+0x1e1d88). The CPU lets such a store succeed (the monitor covers the granule, not the size); the recompiler
# required the same size, so the store failed every time and two of the game's threads spun there forever. Now a store
# to the armed address of at most the loaded size compares and swaps the bytes it writes (the low ones of the loaded
# value, little-endian). Pairs (ldxp / stxp) keep the exact match.
import sys

C_OLD = '''    if (c->excl_on && c->excl_addr == a && c->excl_size == sz) {
        if (sz == 0)'''
C_NEW = '''    if (c->excl_on && c->excl_addr == a && (c->excl_size == sz || (c->excl_size < 0x10 && sz < c->excl_size))) {   /* nxlinux: a narrower store-exclusive */
        if (sz == 0)'''

IR_OLD = '''static LLVMValueRef exclusive_match(SnxIr* ir, LLVMValueRef a, LLVMValueRef size) {
    LLVMValueRef on = ir_cpu_load(ir, ir->t_i32, offsetof(CPU, excl_on));
    LLVMValueRef address = ir_cpu_load(ir, ir->t_i64, offsetof(CPU, excl_addr));
    LLVMValueRef have = ir_cpu_load(ir, ir->t_i32, offsetof(CPU, excl_size));
    return LLVMBuildAnd(B,
                        LLVMBuildAnd(B, LLVMBuildICmp(B, LLVMIntNE, on, ir_i32(ir, 0), ""),
                                     LLVMBuildICmp(B, LLVMIntEQ, address, a, ""), ""),
                        LLVMBuildICmp(B, LLVMIntEQ, have, size, ""), "");
}'''
IR_NEW = '''static LLVMValueRef exclusive_match(SnxIr* ir, LLVMValueRef a, LLVMValueRef size) {
    LLVMValueRef on = ir_cpu_load(ir, ir->t_i32, offsetof(CPU, excl_on));
    LLVMValueRef address = ir_cpu_load(ir, ir->t_i64, offsetof(CPU, excl_addr));
    LLVMValueRef have = ir_cpu_load(ir, ir->t_i32, offsetof(CPU, excl_size));
    /* nxlinux: the same size, or a store narrower than a single (not pair) load-exclusive at the same address */
    LLVMValueRef narrower = LLVMBuildAnd(B, LLVMBuildICmp(B, LLVMIntULT, have, ir_i32(ir, 0x10), ""),
                                         LLVMBuildICmp(B, LLVMIntULT, size, have, ""), "");
    return LLVMBuildAnd(B,
                        LLVMBuildAnd(B, LLVMBuildICmp(B, LLVMIntNE, on, ir_i32(ir, 0), ""),
                                     LLVMBuildICmp(B, LLVMIntEQ, address, a, ""), ""),
                        LLVMBuildOr(B, LLVMBuildICmp(B, LLVMIntEQ, have, size, ""), narrower, ""), "");
}'''

PATCHES = [('src/common/cpu/aarch64/cpu.h', C_OLD, C_NEW), ('src/common/cpu/recompiler/llvm/ir_helpers.c', IR_OLD, IR_NEW)]


def main(root):
    for rel, old, new in PATCHES:
        p = f'{root}/{rel}'
        s = open(p, encoding='utf-8').read()
        if new in s:
            continue
        n = s.count(old)
        if n != 1:
            sys.exit(f'patch_scarletnx.py: {rel}: the text to replace matches {n} times (ScarletNX changed?)')
        open(p, 'w', encoding='utf-8').write(s.replace(old, new))
    print('patch_scarletnx.py: ScarletNX patched (narrower store-exclusive)')


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else '.')
