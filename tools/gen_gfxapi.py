#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The guest-call bridges of OpenGL, EGL and Vulkan: a game's calls go straight to the PC's own driver.

A Switch game that draws with OpenGL (through EGL) or Vulkan instead of NVN calls NVIDIA's driver for the Switch, a
module of its own (subsdk*). nxlinux binds those calls to the PC's driver instead: the game's arguments are the same
values in the same layout (both are 64-bit little-endian with the same C types), so most functions need nothing but
their arguments moved from the guest's registers to the host's. This tool writes one bridge for each function:

  gen_gfxapi.py sigs <gl.xml> <egl.xml> <vk.xml> <out folder>
      reads Khronos' registries and writes gl.sigs, egl.sigs and vk.sigs: one line per function, its name, its result
      and its parameters as the classes the bridges need (the files in gfx/api were made so, and are kept in the
      source tree so that a build needs no registry)
  gen_gfxapi.py thunks <api> <file.sigs> <out.c> [hand-written sources...]
      writes the bridges of one API (gl, egl or vk). Functions the hand-written sources declare with HLE_FORCE
      (gfx/hle_egl.c, gfx/hle_vk.c: the window, the instance and the device, debug callbacks) are left to them.

Classes: v void, i8 u8 i16 u16 i32 u32 i64 u64, f float, d double, p a pointer or a handle that is one; A Vulkan's
VkAllocationCallbacks pointer, passed as NULL (the game's allocator is guest code the driver must not call).
Each bridge finds its host function on its first call (nxl_gfx_proc, gfx/hle_egl.c / gfx/hle_vk.c); a function the
PC's driver does not have returns 0 and is reported once.
"""
import re
import sys
import xml.etree.ElementTree as ET

C = {"v": "void", "i8": "int8_t", "u8": "uint8_t", "i16": "int16_t", "u16": "uint16_t", "i32": "int32_t",
     "u32": "uint32_t", "i64": "int64_t", "u64": "uint64_t", "f": "float", "d": "double", "p": "void *", "A": "void *"}

GL = {"void": "v", "GLenum": "u32", "GLboolean": "u8", "GLbitfield": "u32", "GLbyte": "i8", "GLubyte": "u8",
      "GLshort": "i16", "GLushort": "u16", "GLint": "i32", "GLuint": "u32", "GLclampx": "i32", "GLsizei": "i32",
      "GLfloat": "f", "GLclampf": "f", "GLdouble": "d", "GLclampd": "d", "GLhalfNV": "u16", "GLfixed": "i32",
      "GLintptr": "i64", "GLintptrARB": "i64", "GLsizeiptr": "i64", "GLsizeiptrARB": "i64", "GLint64": "i64",
      "GLint64EXT": "i64", "GLuint64": "u64", "GLuint64EXT": "u64", "GLsync": "p", "GLhandleARB": "u32",
      "GLeglClientBufferEXT": "p", "GLeglImageOES": "p", "GLvdpauSurfaceNV": "i64", "GLDEBUGPROC": "p",
      "GLDEBUGPROCARB": "p", "GLDEBUGPROCKHR": "p", "GLDEBUGPROCAMD": "p", "GLVULKANPROCNV": "p"}
EGL = {"void": "v", "EGLBoolean": "u32", "EGLenum": "u32", "EGLint": "i32", "EGLAttrib": "i64", "EGLTime": "u64",
       "EGLTimeKHR": "u64", "EGLTimeNV": "u64", "EGLuint64KHR": "u64", "EGLuint64NV": "u64", "EGLnsecsANDROID": "i64",
       "EGLNativeFileDescriptorKHR": "i32"}
EGL_PTR = ("EGLDisplay EGLConfig EGLContext EGLSurface EGLClientBuffer EGLImage EGLImageKHR EGLSync EGLSyncKHR EGLSyncNV "
           "EGLDeviceEXT EGLOutputLayerEXT EGLOutputPortEXT EGLStreamKHR EGLLabelKHR EGLObjectKHR EGLNativeDisplayType "
           "EGLNativeWindowType EGLNativePixmapType EGLDEBUGPROCKHR EGLGetBlobFuncANDROID EGLSetBlobFuncANDROID "
           "__eglMustCastToProperFunctionPointerType").split()


def param_text(p):
    """A <proto> / <param>'s type text (the name removed) and its <ptype>."""
    txt = "".join(p.itertext())
    name = p.find("name").text
    txt = txt[:txt.rfind(name)]
    t = p.find("ptype")
    if t is None:
        t = p.find("type")   # (vk.xml's tag)
    return " ".join(txt.split()), (t.text if t is not None else None)


def gl_like(path, table, ptrs=()):
    out = []
    root = ET.parse(path).getroot()
    for c in root.find("commands").iter("command"):
        sig = []
        for p in [c.find("proto")] + c.findall("param"):
            txt, _ = param_text(p)
            base = re.sub(r"\b(const|struct)\b", "", txt).strip()
            if "*" in txt or base in ptrs:
                sig.append("p")
            elif base in table:
                sig.append(table[base])
            else:
                sys.exit(f"{path}: {c.find('proto/name').text}: a parameter of type '{txt}' is not known")
        out.append((c.find("proto/name").text, sig))
    return out


VK_BASE = {"void": "v", "uint8_t": "u8", "int8_t": "i8", "uint16_t": "u16", "int16_t": "i16", "uint32_t": "u32",
           "int32_t": "i32", "uint64_t": "u64", "int64_t": "i64", "size_t": "u64", "int": "i32", "float": "f",
           "double": "d", "char": "i8",
           # platform types that are passed by value somewhere: their sizes on a 64-bit target
           "Window": "u64", "VisualID": "u64", "RROutput": "u64", "xcb_visualid_t": "u32", "xcb_window_t": "u32",
           "zx_handle_t": "u32", "DWORD": "u32", "HANDLE": "p", "HMONITOR": "p", "HWND": "p", "HINSTANCE": "p",
           "LPCWSTR": "p", "GgpStreamDescriptor": "u32", "GgpFrameToken": "u32", "IOSurfaceRef": "p",
           "MTLSharedEvent_id": "p", "MTLBuffer_id": "p", "MTLTexture_id": "p", "MTLCommandQueue_id": "p",
           "MTLDevice_id": "p", "NvSciSyncObj": "p", "NvSciBufObj": "p", "NvSciBufAttrList": "p",
           "NvSciSyncAttrList": "p", "OHBufferHandle": "p", "OH_NativeBuffer": "p"}


def vulkan(path):
    root = ET.parse(path).getroot()
    cls, structs = {}, set()
    for t in root.find("types").iter("type"):
        cat, name = t.get("category"), t.get("name") or (t.find("name").text if t.find("name") is not None else None)
        if not name:
            continue
        if t.get("alias"):
            cls[name] = ("alias", t.get("alias"))
        elif cat == "handle":
            cls[name] = "p" if "VK_DEFINE_HANDLE" in "".join(t.itertext()) and "NON_DISPATCHABLE" not in "".join(t.itertext()) else "u64"
        elif cat == "enum":
            cls[name] = "i32"
        elif cat == "bitmask":
            base = t.find("type").text if t.find("type") is not None else "VkFlags"
            cls[name] = "u64" if base == "VkFlags64" else "u32"
        elif cat == "basetype":
            bt = t.find("type")
            cls[name] = VK_BASE.get(bt.text, "p") if bt is not None else "p"
        elif cat == "funcpointer":
            cls[name] = "p"
        elif cat in ("struct", "union"):
            structs.add(name)
    cls.update({"VkBool32": "u32", "VkDeviceSize": "u64", "VkDeviceAddress": "u64", "VkSampleMask": "u32",
                "VkFlags": "u32", "VkFlags64": "u64", "VkRemoteAddressNV": "p"})

    def resolve(n):
        seen = 0
        while isinstance(cls.get(n), tuple) and seen < 8:
            n, seen = cls[n][1], seen + 1
        return n

    cmds, aliases, out = {}, {}, []
    for c in root.find("commands").iter("command"):
        if c.get("api") == "vulkansc":
            continue
        if c.get("alias"):
            aliases[c.get("name")] = c.get("alias")
            continue
        sig, skip = [], None
        for p in [c.find("proto")] + c.findall("param"):
            if p.get("api") == "vulkansc":
                continue
            txt, ptype = param_text(p)
            if "*" in txt:
                sig.append("A" if ptype == "VkAllocationCallbacks" else "p")
                continue
            if "[" in "".join(p.itertext()):   # an array parameter: a pointer
                sig.append("p")
                continue
            t = resolve(ptype)
            if t in structs or resolve(t) in structs:
                skip = f"{ptype} by value"
                break
            k = cls.get(t) if not isinstance(cls.get(t), tuple) else None
            k = k or VK_BASE.get(t)
            if not k:
                skip = f"type {t}"
                break
            sig.append(k)
        name = c.find("proto/name").text
        if skip:
            print(f"vk.xml: {name} left out ({skip})", file=sys.stderr)
            continue
        cmds[name] = sig
    for a, target in aliases.items():
        while target in aliases:
            target = aliases[target]
        if target in cmds:
            cmds.setdefault(a, cmds[target])
    return sorted(cmds.items())


def write_sigs(path, what, rows, src):
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"# {what}: name, result, parameters (tools/gen_gfxapi.py, from Khronos' {src}, Apache-2.0)\n")
        for name, sig in rows:
            f.write(" ".join([name] + sig) + "\n")


def thunks(api, sigs, out, hand):
    skip = set()
    for h in hand:
        skip |= set(re.findall(r'HLE_FORCE\(\s*"(\w+)"', open(h, encoding="utf-8").read()))
    rows = []
    for line in open(sigs, encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        name, *sig = line.split()
        if name not in skip:
            rows.append((name, sig))
    o = [f"// generated by tools/gen_gfxapi.py from {sigs.split('/')[-1]}: do not edit",
         "// each function's arguments moved from the guest's registers to the PC's driver (see the tool)",
         '#include "hle_abi.h"',
         "// the host function (never NULL: a missing one returns 0); it is kept in *slot once it is known for good",
         "void *nxl_gfx_proc(int api, const char *name, void **slot);",
         ""]
    apin = {"gl": 0, "egl": 1, "vk": 2}[api]
    for i, (name, sig) in enumerate(rows):
        ret, params = sig[0], sig[1:]
        body = ["    NXL_ARGS;"]
        args = []
        for k, t in enumerate(params):
            ct = C[t]
            body.append(f"    {ct} a{k} = NXL_ARG({ct});" + (f" (void)a{k};" if t == "A" else ""))
            args.append("NULL" if t == "A" else f"a{k}")
        ptypes = ", ".join(C[t] for t in params) or "void"
        body.append(f"    typedef {C[ret]} (*fn_t)({ptypes});")
        body.append("    static fn_t f;")
        body.append(f'    fn_t g = f ? f : (fn_t)nxl_gfx_proc({apin}, "{name}", (void **)&f);')
        call = f"g({', '.join(args)})"
        body.append(f"    {call};" if ret == "v" else f"    NXL_RET({C[ret]}, {call});")
        body.append("    (void)nxl_a;")
        o.append(f"static void nxb_{name}(CPU *c) {{\n" + "\n".join(body) + "\n}")
        o.append(f'NXL_REGISTER("{name}", nxb_{name}, "{api.upper()}: the PC\'s driver", 1);')
    text = "\n".join(o) + "\n"
    old = None
    try:
        old = open(out, encoding="utf-8").read()
    except OSError:
        pass
    if old != text:
        with open(out, "w", encoding="utf-8") as f:
            f.write(text)


def main():
    a = sys.argv[1:]
    if len(a) == 5 and a[0] == "sigs":
        write_sigs(a[4] + "/gl.sigs", "OpenGL and OpenGL ES", gl_like(a[1], GL), "gl.xml")
        write_sigs(a[4] + "/egl.sigs", "EGL", gl_like(a[2], EGL, EGL_PTR), "egl.xml")
        write_sigs(a[4] + "/vk.sigs", "Vulkan", vulkan(a[3]), "vk.xml")
    elif len(a) >= 4 and a[0] == "thunks" and a[1] in ("gl", "egl", "vk"):
        thunks(a[1], a[2], a[3], a[4:])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
