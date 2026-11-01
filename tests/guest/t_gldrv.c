// SPDX-License-Identifier: GPL-3.0-or-later
// A stand-in for NVIDIA's OpenGL driver for the Switch, loaded beside t_gl as its subsdk: it defines EGL's and GL's
// entry points, and none of them may be called (nxlinux's bridges to the PC's driver win over a module's own).
#include "nx.h"
static void wrong(const char *f) { print("not ok - the game's own driver was called (%s)\n", f); svcExitProcess(); }
void *eglGetDisplay(void *native) { wrong("eglGetDisplay"); return (void *)0xdead; }
void glClear(unsigned mask) { wrong("glClear"); }
void NvGlEs2Init(void) { wrong("NvGlEs2Init"); }
int test_main(void) { return 0; }
