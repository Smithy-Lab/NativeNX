#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write a minimal NPDM (the META header and an ACI0 section) for tests.

usage: make_npdm.py <output> <program id (hex)> <system resource size> <main thread stack size> <main thread priority>
"""
import struct
import sys

out, pid, sysres, stack, prio = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 0), int(sys.argv[4], 0), int(sys.argv[5], 0)
meta = bytearray(0x80)
meta[0:4] = b"META"
meta[0xE] = prio
struct.pack_into("<I", meta, 0x14, sysres)
struct.pack_into("<I", meta, 0x1C, stack)
meta[0x20:0x20 + 8] = b"nxltest\0"
aci = bytearray(0x40)
aci[0:4] = b"ACI0"
struct.pack_into("<Q", aci, 0x10, pid)
struct.pack_into("<II", meta, 0x70, len(meta), len(aci))
with open(out, "wb") as f:
    f.write(meta + aci)
