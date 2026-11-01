#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write the guest-call thunks of a host source's HLE functions.

usage: gen_thunks.py <source.c> <output.c> [--patch rules.py] [-- <C preprocessor command and flags>]

--patch names a Python file defining REPLACE = [(old, new), ...]: text replacements applied to the source's copy before
anything else, each of which must match exactly once (so a change upstream fails the build instead of being missed).

With a preprocessor command, the declarations are read from the preprocessed source (compiled with NXL_GEN_SCAN, which
makes the HLE macros leave a __nxl_hle(...) marker), so declarations that other macros generate are found too:
nxnative's hle_hid.c makes 32 with TOUCH(1) ... TOUCH(16). Without one, the source text is scanned.

The source declares host implementations of guest functions as nxnative does:
    HLE("mangled", ret, c_name, params...)
    HLE_FORCE("mangled", "why", ret, c_name, params...)
The output #includes the source, then defines for each a thunk that reads the parameters from the guest's registers
(src/hle.h: NXL_ARG, by the AAPCS64 rules), calls the function, writes its result back (NXL_RET), and registers the
thunk under the mangled name. Only the standard library is used.
"""
import os
import re
import sys

IDENT = re.compile(r"[A-Za-z_]\w*")


def strip_comments_and_strings(src):
    """The source with comments blanked (strings kept), so the scan never matches inside a comment."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i)); i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", src[i:j])); i = j
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1]); i = j + 1
        else:
            out.append(c); i += 1
    return "".join(out)


def split_args(s):
    """Top-level comma split of a macro's argument text."""
    args, depth, cur, i = [], 0, [], 0
    while i < len(s):
        c = s[i]
        if c in "\"'":
            j = i + 1
            while j < len(s) and s[j] != c:
                j += 2 if s[j] == "\\" else 1
            cur.append(s[i:j + 1]); i = j + 1; continue
        if c in "([{": depth += 1
        elif c in ")]}": depth -= 1
        if c == "," and depth == 0:
            args.append("".join(cur).strip()); cur = []
        else:
            cur.append(c)
        i += 1
    if cur or args: args.append("".join(cur).strip())
    return args


def find_calls(src, macro):
    """(argument text, line) of each top-level use of macro(...)."""
    for m in re.finditer(r"(?<![\w#])" + macro + r"\s*\(", src):
        line_start = src.rfind("\n", 0, m.start()) + 1
        if src[line_start:m.start()].lstrip().startswith("#"):
            continue   # a #define
        i, depth = m.end(), 1
        while i < len(src) and depth:
            if src[i] in "\"'":
                q, i = src[i], i + 1
                while i < len(src) and src[i] != q:
                    i += 2 if src[i] == "\\" else 1
            elif src[i] == "(": depth += 1
            elif src[i] == ")": depth -= 1
            i += 1
        yield src[m.end():i - 1], src.count("\n", 0, m.start()) + 1


def param_type(p, where):
    """The type of one parameter declaration ("const char *name" -> "const char *")."""
    p = " ".join(p.split())
    if p == "...":
        sys.exit(f"{where}: variadic HLE functions are not supported (nxnative avoids them too)")
    fp = re.search(r"\(\s*\*\s*(\w+)\s*\)", p)   # a function pointer: void (*cb)(int)
    if fp:
        return p[:fp.start(1)] + p[fp.end(1):]
    arr = re.match(r"(.*?)(\w+)\s*\[[^\]]*\]$", p)   # an array parameter is a pointer
    if arr:
        return arr.group(1).strip() + " *"
    names = list(IDENT.finditer(p))
    if len(names) < 2 and "*" not in p:
        sys.exit(f"{where}: cannot tell the type from the name in parameter '{p}'")
    last = names[-1]
    return p[:last.start()].strip()


LONG = re.compile(r"^(const\s+)?(signed\s+|unsigned\s+)?long(\s+int)?$")


def guest_width(t):
    """The guest's view of a scalar type: its long is 64 bits (LP64), the host's is 32 on Windows (LLP64), so a long
    argument or result is moved as int64_t / uint64_t and converted to the host function's own type."""
    t = " ".join(t.split())
    m = LONG.match(t)
    if not m:
        return t
    return (m.group(1) or "") + ("uint64_t" if m.group(2) and m.group(2).strip() == "unsigned" else "int64_t")


def declarations(path, cpp):
    """(force, [mangled, why, ret, cname, params...], where) of each HLE declaration in the source."""
    if cpp:
        import subprocess
        r = subprocess.run(cpp + ["-E", "-P", "-DNXL_GEN_SCAN", "-x", "c", path], capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"{path}: the preprocessor failed:\n{r.stderr}")
        for text, line in find_calls(r.stdout, "__nxl_hle"):
            a = split_args(text)
            yield int(a[0]), a[1:], f"{path} ({a[1] if len(a) > 1 else '?'})"
        return
    src = strip_comments_and_strings(open(path, encoding="utf-8").read())
    for macro, force in (("HLE_FORCE_BIG", 1), ("HLE_FORCE", 1), ("HLE", 0)):
        for text, line in find_calls(src, macro):
            a = split_args(text)
            yield force, (a if force else a[:1] + ["NULL"] + a[1:]), f"{path}:{line}"


def thunks(path, cpp=None):
    out, seen = [], set()
    for force, a, where in declarations(path, cpp):
        if len(a) < 4:
            sys.exit(f"{where}: an HLE declaration needs a name, a return type and a function name")
        mangled, why, ret, cname, params = a[0], a[1], a[2], a[3], a[4:]
        if cname in seen:
            continue
        seen.add(cname)
        if params in ([], ["void"], [""]):
            params = []
        types = [guest_width(param_type(p, where)) for p in params]
        ret = guest_width(ret)
        body = ["    NXL_ARGS;"]
        for k, t in enumerate(types):
            body.append(f"    {t} a{k} = NXL_ARG({t});")
        call = f"{cname}({', '.join(f'a{k}' for k in range(len(types)))})"
        body.append(f"    {call};" if " ".join(ret.split()) == "void" else f"    NXL_RET({ret}, {call});")
        body.append("    (void)nxl_a;")
        out.append(f"static void nxl_thunk_{cname}(CPU *c) {{\n" + "\n".join(body) +
                   f"\n}}\nNXL_REGISTER({mangled}, nxl_thunk_{cname}, {why}, {force});\n")
    return out


def main():
    argv = sys.argv[1:]
    cpp = None
    if "--" in argv:
        cpp, argv = argv[argv.index("--") + 1:], argv[:argv.index("--")]
    patch = None
    if "--patch" in argv:
        k = argv.index("--patch")
        patch, argv = argv[k + 1], argv[:k] + argv[k + 2:]
    if len(argv) != 2:
        sys.exit(__doc__)
    src, dst = os.path.abspath(argv[0]), argv[1]
    text = open(src, encoding="utf-8").read()
    if patch:
        rules = {}
        exec(compile(open(patch, encoding="utf-8").read(), patch, "exec"), rules)
        for old, new in rules["REPLACE"]:
            n = text.count(old)
            if n != 1:
                sys.exit(f"{patch}: a replacement matches {n} times in {src} (must be once): {old[:80]!r}")
            text = text.replace(old, new)
    # The source's text is copied in rather than #included: a quoted #include looks in the including file's folder
    # first, and for a source taken from nxnative that would find its Mac headers instead of compat/nxnative's.
    copy = (f"// generated by tools/gen_thunks.py from {os.path.basename(src)}{' (patched by ' + os.path.basename(patch) + ')' if patch else ''}: do not edit\n"
            f"#line 1 \"{src}\"\n" + text + "\n\n")
    tmp = dst + ".scan.c"
    if cpp:   # preprocess the copy, so its includes resolve exactly as they will when it is compiled
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(copy)
    body = thunks(tmp if cpp else src, cpp)
    if cpp:
        os.remove(tmp)
    text = copy + "\n".join(body)
    old = open(dst, encoding="utf-8").read() if os.path.exists(dst) else None
    if old != text:
        with open(dst, "w", encoding="utf-8") as f:
            f.write(text)


if __name__ == "__main__":
    main()
