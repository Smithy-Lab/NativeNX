// SPDX-License-Identifier: GPL-3.0-or-later
// fnmatch on Windows: the shell's wildcards '*', '?' and [...] (with ! or ^ for none of), '\' escaping; no flags
#pragma once
#define FNM_NOMATCH 1
static inline int fnmatch(const char *p, const char *s, int flags) {
    (void)flags;
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return 0;
            for (; *s; s++) if (!fnmatch(p, s, 0)) return 0;
            return FNM_NOMATCH;
        }
        if (!*s) return FNM_NOMATCH;
        if (*p == '?') continue;
        if (*p == '[') {
            const char *q = p + 1;
            int neg = *q == '!' || *q == '^', hit = 0;
            if (neg) q++;
            for (int first = 1; *q && (first || *q != ']'); q++, first = 0) {
                if (q[1] == '-' && q[2] && q[2] != ']') { if ((unsigned char)*s >= (unsigned char)*q && (unsigned char)*s <= (unsigned char)q[2]) hit = 1; q += 2; }
                else if (*q == *s) hit = 1;
            }
            if (!*q) { if (*s != '[') return FNM_NOMATCH; continue; }   // (no closing ']': a plain '[')
            if (hit == neg) return FNM_NOMATCH;
            p = q;
            continue;
        }
        if (*p == '\\' && p[1]) p++;
        if (*p != *s) return FNM_NOMATCH;
    }
    return *s ? FNM_NOMATCH : 0;
}
