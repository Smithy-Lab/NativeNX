// SPDX-License-Identifier: GPL-3.0-or-later
// platform.h on Windows. Names are UTF-8 in nxlinux and NativeNX, and '/' between folders (Windows takes both); the
// wide (UTF-16) calls are used where the system's own text is read or given.
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include "platform.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// UTF-8 <-> UTF-16
static int to_wide(const char *s, wchar_t *w, int n) { return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) > 0 ? 0 : -1; }
static int from_wide(const wchar_t *w, char *s, size_t n) { return WideCharToMultiByte(CP_UTF8, 0, w, -1, s, (int)n, NULL, NULL) > 0 ? 0 : -1; }
static void slashes(char *s) { for (; *s; s++) if (*s == '\\') *s = '/'; }

int nxl_exe_dir(char *out, size_t n) {
    wchar_t w[MAX_PATH * 4];
    DWORD k = GetModuleFileNameW(NULL, w, sizeof w / sizeof *w);
    if (!k || k >= sizeof w / sizeof *w) return -1;
    char p[MAX_PATH * 12];
    if (from_wide(w, p, sizeof p)) return -1;
    slashes(p);
    char *slash = strrchr(p, '/');
    if (!slash) return -1;
    *slash = 0;
    return snprintf(out, n, "%s", p) < (int)n ? 0 : -1;
}
int nxl_data_file(const char *sub, const char *name, char *out, size_t n) {
    char dir[4096];
    if (nxl_exe_dir(dir, sizeof dir)) return -1;
    snprintf(out, n, "%s/%s", dir, name);   // beside the program (the zip's folder, a build folder)
    if (!_access(out, 4)) return 0;
    snprintf(out, n, "%s/../share/%s/%s", dir, sub, name);   // an install
    return _access(out, 4) ? -1 : 0;
}

// ---- processes ----
// One argument for a command line, as the C runtime reads it back (backslashes before a quote doubled)
static void quote_arg(char *cl, size_t n, const char *a) {
    size_t l = strlen(cl);
    if (l + 1 < n) cl[l++] = '"';
    for (const char *c = a;; c++) {
        size_t bs = 0;
        while (*c == '\\') bs++, c++;
        size_t times = !*c ? bs * 2 : *c == '"' ? bs * 2 + 1 : bs;
        for (size_t i = 0; i < times && l + 1 < n; i++) cl[l++] = '\\';
        if (!*c) break;
        if (l + 1 < n) cl[l++] = *c;
    }
    if (l + 2 < n) cl[l++] = '"', cl[l++] = ' ';
    cl[l < n ? l : n - 1] = 0;
}
static int name_is(const char *entry, const char *name, size_t nl) {   // "NAME=..." is NAME (Windows ignores case there)
    return !_strnicmp(entry, name, nl) && (entry[nl] == '=' || !entry[nl]);
}
static int env_cmp(const void *a, const void *b) { return _stricmp(*(const char *const *)a, *(const char *const *)b); }
// The environment block for the new program: this one's, with the changes (sorted, as Windows wants it)
static char *env_block(const char *const *changes, const char *const *extra) {
    char *cur = GetEnvironmentStringsA(), **all = NULL;
    size_t na = 0, cap = 0, total = 2;
    #define ADD(s) do { if (na == cap) { cap = cap ? cap * 2 : 256; all = realloc(all, cap * sizeof *all); } all[na++] = (char *)(s); total += strlen(s) + 1; } while (0)
    for (const char *e = cur; e && *e; e += strlen(e) + 1) {
        if (*e == '=') { ADD(e); continue; }   // (the drives' current folders, "=C:=C:\...")
        const char *eq = strchr(e, '=');
        size_t nl = eq ? (size_t)(eq - e) : strlen(e);
        int changed = 0;
        for (const char *const *c = changes; c && *c && !changed; c++) changed = name_is(*c, e, nl);
        for (const char *const *c = extra; c && *c && !changed; c++) changed = name_is(*c, e, nl);
        if (!changed) ADD(e);
    }
    for (const char *const *c = changes; c && *c; c++) if (strchr(*c, '=')) ADD(*c);   // ("NAME" alone: left out)
    for (const char *const *c = extra; c && *c; c++) ADD(*c);
    #undef ADD
    qsort(all, na, sizeof *all, env_cmp);
    char *b = malloc(total), *p = b;
    for (size_t i = 0; i < na; i++) { size_t l = strlen(all[i]) + 1; memcpy(p, all[i], l); p += l; }
    *p = 0;
    free(all);
    FreeEnvironmentStringsA(cur);
    return b;
}
// A pipe with one end for the new program (inheritable) and the other for this one, as a C descriptor that never waits
static int make_pipe(int to_child, HANDLE *child_end, int *own_fd) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE r, w;
    if (!CreatePipe(&r, &w, &sa, 1 << 16)) return -1;
    HANDLE own = to_child ? w : r;
    *child_end = to_child ? r : w;
    SetHandleInformation(own, HANDLE_FLAG_INHERIT, 0);
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    SetNamedPipeHandleState(own, &mode, NULL, NULL);
    *own_fd = _open_osfhandle((intptr_t)own, (to_child ? _O_WRONLY : _O_RDONLY) | _O_BINARY);
    if (*own_fd < 0) { CloseHandle(r), CloseHandle(w); return -1; }
    return 0;
}
nxl_proc nxl_spawn(nxl_spawn_t *s) {
    s->to_fd = s->from_fd = -1;
    HANDLE to_h = NULL, from_h = NULL, log_h = NULL;
    char extra[2][96], *ex[3] = {NULL, NULL, NULL};
    int nx = 0;
    if (s->to_env && !make_pipe(1, &to_h, &s->to_fd)) snprintf(extra[nx], sizeof extra[0], "%s=%llu", s->to_env, (unsigned long long)(uintptr_t)to_h), ex[nx] = extra[nx], nx++;
    if (s->from_env && !make_pipe(0, &from_h, &s->from_fd)) snprintf(extra[nx], sizeof extra[0], "%s=%llu", s->from_env, (unsigned long long)(uintptr_t)from_h), ex[nx] = extra[nx], nx++;
    STARTUPINFOW si = {sizeof si};
    if (s->log) {
        SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
        wchar_t wl[MAX_PATH * 4];
        if (!to_wide(s->log, wl, sizeof wl / sizeof *wl))
            log_h = CreateFileW(wl, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (log_h == INVALID_HANDLE_VALUE) log_h = NULL;
        if (log_h) si.dwFlags = STARTF_USESTDHANDLES, si.hStdOutput = si.hStdError = log_h, si.hStdInput = NULL;
    }
    size_t cn = 32;
    for (const char *const *a = s->argv; *a; a++) cn += strlen(*a) * 2 + 3;
    char *cl = calloc(1, cn);
    for (const char *const *a = s->argv; *a; a++) quote_arg(cl, cn, *a);
    char *env = env_block(s->env, (const char *const *)ex);
    // (to UTF-16 for CreateProcessW: the command line and the environment)
    int wn = MultiByteToWideChar(CP_UTF8, 0, cl, -1, NULL, 0);
    wchar_t *wcl = malloc((size_t)wn * sizeof *wcl), wexe[MAX_PATH * 4];
    MultiByteToWideChar(CP_UTF8, 0, cl, -1, wcl, wn);
    size_t el = 0;
    while (env[el] || env[el + 1]) el++;
    el += 2;
    int we = MultiByteToWideChar(CP_UTF8, 0, env, (int)el, NULL, 0);
    wchar_t *wenv = malloc((size_t)we * sizeof *wenv);
    MultiByteToWideChar(CP_UTF8, 0, env, (int)el, wenv, we);
    PROCESS_INFORMATION pi = {0};
    BOOL ok = !to_wide(s->exe, wexe, sizeof wexe / sizeof *wexe) &&
              CreateProcessW(wexe, wcl, NULL, NULL, TRUE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, wenv, NULL, &si, &pi);
    free(cl), free(wcl), free(env), free(wenv);
    if (to_h) CloseHandle(to_h);   // (the new program has its own copies)
    if (from_h) CloseHandle(from_h);
    if (log_h) CloseHandle(log_h);
    if (!ok) {
        if (s->to_fd >= 0) _close(s->to_fd), s->to_fd = -1;
        if (s->from_fd >= 0) _close(s->from_fd), s->from_fd = -1;
        return 0;
    }
    CloseHandle(pi.hThread);
    return (nxl_proc)pi.hProcess;
}
int nxl_proc_ended(nxl_proc p, int *status) {
    if (!p || WaitForSingleObject((HANDLE)p, 0) != WAIT_OBJECT_0) return 0;
    DWORD code = 1;
    GetExitCodeProcess((HANDLE)p, &code);
    CloseHandle((HANDLE)p);
    // an exception that ended it (0xC0000005 an access violation ...) is reported as the signal it would have been
    if ((code & 0xF0000000u) == 0xC0000000u) *status = code == 0xC0000005u ? -11 : code == 0xC000013Au ? -2 : -6;
    else *status = (int)code;
    return 1;
}
typedef LONG(NTAPI *nt_proc_fn)(HANDLE);
void nxl_proc_signal(nxl_proc p, int what) {
    if (!p) return;
    if (what == NXL_PROC_TERM) { TerminateProcess((HANDLE)p, 143); return; }   // (143: as a Linux program ended by SIGTERM)
    // paused and resumed whole, all its threads (ntdll's NtSuspendProcess / NtResumeProcess, as Task Manager and
    // Process Explorer do)
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    nt_proc_fn f = nt ? (nt_proc_fn)(void (*)(void))GetProcAddress(nt, what == NXL_PROC_STOP ? "NtSuspendProcess" : "NtResumeProcess") : NULL;
    if (f) f((HANDLE)p);
}
int nxl_fd_from_env(const char *name) {
    const char *e = getenv(name);
    if (!e || !*e) return -1;
    HANDLE h = (HANDLE)(uintptr_t)strtoull(e, NULL, 10);
    SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);   // (not passed on again)
    return _open_osfhandle((intptr_t)h, _O_BINARY);
}

void nxl_console_attach(void) {
    if (GetStdHandle(STD_ERROR_HANDLE) && GetFileType(GetStdHandle(STD_ERROR_HANDLE)) != FILE_TYPE_UNKNOWN) return;   // (already somewhere: a pipe, a file)
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout), freopen("CONOUT$", "w", stderr);
        return;
    }
    char d[MAX_PATH * 12], p[MAX_PATH * 13], exe[MAX_PATH * 4] = "program";
    wchar_t w[MAX_PATH * 4];
    if (GetModuleFileNameW(NULL, w, MAX_PATH * 4) && !from_wide(w, p, sizeof p)) {
        slashes(p);
        const char *b = strrchr(p, '/') ? strrchr(p, '/') + 1 : p;
        snprintf(exe, sizeof exe, "%s", b);
        char *dot = strrchr(exe, '.');
        if (dot) *dot = 0;
    }
    nxl_user_dir(NXL_DIR_DATA, d, sizeof d);
    CreateDirectoryA(d, NULL);
    strncat(d, "/logs", sizeof d - strlen(d) - 1);
    CreateDirectoryA(d, NULL);
    snprintf(p, sizeof p, "%s/%s.log", d, exe);
    if (freopen(p, "w", stderr)) setvbuf(stderr, NULL, _IONBF, 0), freopen(p, "a", stdout);
}

void nxl_open_folder(const char *dir) {
    wchar_t w[MAX_PATH * 4];
    if (!to_wide(dir, w, sizeof w / sizeof *w)) ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

// ---- folders ----
static void env_dir(const char *var, const char *fallback, char *out, size_t n) {
    wchar_t wv[64], w[MAX_PATH * 4];
    to_wide(var, wv, 64);
    DWORD k = GetEnvironmentVariableW(wv, w, sizeof w / sizeof *w);
    if (!k || k >= sizeof w / sizeof *w || from_wide(w, out, n)) snprintf(out, n, "%s", fallback);
    slashes(out);
}
void nxl_user_dir(int kind, char *out, size_t n) {
    char b[MAX_PATH * 12];
    switch (kind) {
    case NXL_DIR_CONFIG: env_dir("APPDATA", ".", b, sizeof b); snprintf(out, n, "%s/nxlinux", b); break;
    case NXL_DIR_DATA: env_dir("LOCALAPPDATA", ".", b, sizeof b); snprintf(out, n, "%s/nxlinux", b); break;
    case NXL_DIR_CACHE: env_dir("LOCALAPPDATA", ".", b, sizeof b); snprintf(out, n, "%s/nxlinux/cache", b); break;
    default: env_dir("TEMP", ".", out, n); break;
    }
}
void nxl_home_dir(char *out, size_t n) { env_dir("USERPROFILE", "", out, n); }
int nxl_fd_nonblock(int fd) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    return h != INVALID_HANDLE_VALUE && SetNamedPipeHandleState(h, &mode, NULL, NULL) ? 0 : -1;
}
void nxl_desktop_data_dir(char *out, size_t n) { env_dir("LOCALAPPDATA", ".", out, n); }

int nxl_shortcut(const char *exe, const char *lnk, const char *description) {
    wchar_t we[MAX_PATH * 4], wl[MAX_PATH * 4], wd[MAX_PATH * 4], wdesc[256];
    char dir[MAX_PATH * 12];
    snprintf(dir, sizeof dir, "%s", exe);
    char *sl = strrchr(dir, '/');
    if (sl) *sl = 0;
    if (to_wide(exe, we, MAX_PATH * 4) || to_wide(lnk, wl, MAX_PATH * 4) || to_wide(dir, wd, MAX_PATH * 4) || to_wide(description, wdesc, 256)) return -1;
    for (wchar_t *c = we; *c; c++) if (*c == L'/') *c = L'\\';
    for (wchar_t *c = wl; *c; c++) if (*c == L'/') *c = L'\\';
    for (wchar_t *c = wd; *c; c++) if (*c == L'/') *c = L'\\';
    HRESULT hi = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IShellLinkW *link = NULL;
    IPersistFile *pf = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
    if (SUCCEEDED(hr)) {
        IShellLinkW_SetPath(link, we);
        IShellLinkW_SetWorkingDirectory(link, wd);
        IShellLinkW_SetDescription(link, wdesc);
        IShellLinkW_SetIconLocation(link, we, 0);
        hr = IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&pf);
        if (SUCCEEDED(hr)) hr = IPersistFile_Save(pf, wl, TRUE), IPersistFile_Release(pf);
        IShellLinkW_Release(link);
    }
    if (SUCCEEDED(hi)) CoUninitialize();
    return SUCCEEDED(hr) ? 0 : -1;
}
