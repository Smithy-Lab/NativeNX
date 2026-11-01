// SPDX-License-Identifier: GPL-3.0-or-later
// platform.h on Linux.
#define _GNU_SOURCE
#include "platform.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int nxl_exe_dir(char *out, size_t n) {
    char p[4096];
    ssize_t k = readlink("/proc/self/exe", p, sizeof p - 1);
    if (k <= 0) return -1;
    p[k] = 0;
    char *slash = strrchr(p, '/');
    if (!slash) return -1;
    *slash = 0;
    return snprintf(out, n, "%s", p) < (int)n ? 0 : -1;
}
int nxl_data_file(const char *sub, const char *name, char *out, size_t n) {
    char dir[4096];
    if (nxl_exe_dir(dir, sizeof dir)) return -1;
    snprintf(out, n, "%s/%s", dir, name);   // beside the program (a build folder)
    if (!access(out, R_OK)) return 0;
    snprintf(out, n, "%s/../share/%s/%s", dir, sub, name);   // an install
    return access(out, R_OK) ? -1 : 0;
}

// NativeNX's end of a pipe: it never waits, and the next program started does not inherit it
static void own_end(int fd) {
    fcntl(fd, F_SETFL, O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}
nxl_proc nxl_spawn(nxl_spawn_t *s) {
    int to[2] = {-1, -1}, from[2] = {-1, -1};   // [0] read, [1] write
    s->to_fd = s->from_fd = -1;
    if (s->to_env && pipe(to)) to[0] = to[1] = -1;
    if (s->from_env && pipe(from)) from[0] = from[1] = -1;
    pid_t pid = fork();
    if (pid < 0) {
        for (int i = 0; i < 2; i++) { if (to[i] >= 0) close(to[i]); if (from[i] >= 0) close(from[i]); }
        return 0;
    }
    if (!pid) {   // the child: its environment and output, then the program
        char b[16];
        if (to[0] >= 0) { close(to[1]); snprintf(b, sizeof b, "%d", to[0]); setenv(s->to_env, b, 1); }
        if (from[1] >= 0) { close(from[0]); snprintf(b, sizeof b, "%d", from[1]); setenv(s->from_env, b, 1); }
        if (s->log) {
            int fd = open(s->log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) dup2(fd, 1), dup2(fd, 2), close(fd);
        }
        for (const char *const *e = s->env; e && *e; e++) {
            const char *eq = strchr(*e, '=');
            if (eq) putenv(strdup(*e)); else unsetenv(*e);
        }
        execv(s->exe, (char *const *)s->argv);
        _exit(127);
    }
    if (to[0] >= 0) { close(to[0]); own_end(to[1]); s->to_fd = to[1]; }
    if (from[1] >= 0) { close(from[1]); own_end(from[0]); s->from_fd = from[0]; }
    return pid;
}
int nxl_proc_ended(nxl_proc p, int *status) {
    int st;
    if (waitpid((pid_t)p, &st, WNOHANG) != (pid_t)p) return 0;
    *status = WIFSIGNALED(st) ? -WTERMSIG(st) : WEXITSTATUS(st);
    return 1;
}
void nxl_proc_signal(nxl_proc p, int what) {
    static const int sig[] = {SIGSTOP, SIGCONT, SIGTERM};
    if (p > 0) kill((pid_t)p, sig[what]);
}
int nxl_fd_from_env(const char *name) {
    const char *e = getenv(name);
    if (!e || !*e) return -1;
    int fd = atoi(e);
    if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

void nxl_console_attach(void) {}

// xdg-open run directly (no shell: a folder's name may hold quotes), in a grandchild so that nothing is left to wait for
void nxl_open_folder(const char *dir) {
    pid_t p = fork();
    if (p == 0) {
        if (fork() == 0) {
            int nul = open("/dev/null", O_WRONLY);
            if (nul >= 0) dup2(nul, 1), dup2(nul, 2);
            setsid();
            execlp("xdg-open", "xdg-open", dir, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (p > 0) waitpid(p, NULL, 0);
}
void nxl_user_dir(int kind, char *out, size_t n) {
    static const char *const var[] = {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR"};
    static const char *const dflt[] = {".config", ".local/share", ".cache"};
    const char *x = getenv(var[kind]), *h = getenv("HOME");
    if (kind == NXL_DIR_RUNTIME) { snprintf(out, n, "%s", x && *x ? x : "/tmp"); return; }
    if (x && *x) snprintf(out, n, "%s/nxlinux", x);
    else snprintf(out, n, "%s/%s/nxlinux", h && *h ? h : "/tmp", dflt[kind]);
}
void nxl_home_dir(char *out, size_t n) {
    const char *h = getenv("HOME");
    snprintf(out, n, "%s", h ? h : "");
}
int nxl_fd_nonblock(int fd) { return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0 ? -1 : 0; }
void nxl_desktop_data_dir(char *out, size_t n) {
    const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
    if (x && *x) snprintf(out, n, "%s", x); else snprintf(out, n, "%s/.local/share", h && *h ? h : "/tmp");
}
