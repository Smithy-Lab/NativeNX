// SPDX-License-Identifier: GPL-3.0-or-later
// C library functions the host serves when no module defines them: nxnative's pass-through table (dm/host/hle_table.c,
// functions with no struct layout difference between the SDK's C library and the host's), here as HLE declarations so
// their thunks are generated. Plus qsort, whose comparator is guest code (a host -> guest call per comparison).
#include "hle.h"
#include <ctype.h>
#include <math.h>
#include <strings.h>
#include <time.h>

HLE("memcpy", void *, h_memcpy, void *d, const void *s, size_t n) { return memcpy(d, s, n); }
HLE("memset", void *, h_memset, void *d, int c, size_t n) { return memset(d, c, n); }
HLE("memmove", void *, h_memmove, void *d, const void *s, size_t n) { return memmove(d, s, n); }
HLE("memcmp", int, h_memcmp, const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
HLE("memchr", void *, h_memchr, const void *s, int c, size_t n) { return (void *)memchr(s, c, n); }
HLE("strlen", size_t, h_strlen, const char *s) { return strlen(s); }
HLE("strnlen", size_t, h_strnlen, const char *s, size_t n) { return strnlen(s, n); }
HLE("strcmp", int, h_strcmp, const char *a, const char *b) { return strcmp(a, b); }
HLE("strncmp", int, h_strncmp, const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
HLE("strcpy", char *, h_strcpy, char *d, const char *s) { return strcpy(d, s); }
HLE("strncpy", char *, h_strncpy, char *d, const char *s, size_t n) { return strncpy(d, s, n); }
HLE("strcat", char *, h_strcat, char *d, const char *s) { return strcat(d, s); }
HLE("strncat", char *, h_strncat, char *d, const char *s, size_t n) { return strncat(d, s, n); }
HLE("strchr", char *, h_strchr, const char *s, int c) { return (void *)strchr(s, c); }
HLE("strrchr", char *, h_strrchr, const char *s, int c) { return (void *)strrchr(s, c); }
HLE("strstr", char *, h_strstr, const char *h, const char *n) { return (void *)strstr(h, n); }
HLE("strspn", size_t, h_strspn, const char *s, const char *a) { return strspn(s, a); }
HLE("strcspn", size_t, h_strcspn, const char *s, const char *a) { return strcspn(s, a); }
HLE("strpbrk", char *, h_strpbrk, const char *s, const char *a) { return (void *)strpbrk(s, a); }
HLE("strcasecmp", int, h_strcasecmp, const char *a, const char *b) { return strcasecmp(a, b); }
HLE("strncasecmp", int, h_strncasecmp, const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }
HLE("atoi", int, h_atoi, const char *s) { return atoi(s); }
HLE("atof", double, h_atof, const char *s) { return atof(s); }
HLE("strtol", int64_t, h_strtol, const char *s, char **e, int b) { return strtoll(s, e, b); }   // (the guest's long: 64 bits, as long long is everywhere)
HLE("strtoul", uint64_t, h_strtoul, const char *s, char **e, int b) { return strtoull(s, e, b); }
HLE("strtoll", long long, h_strtoll, const char *s, char **e, int b) { return strtoll(s, e, b); }
HLE("strtoull", unsigned long long, h_strtoull, const char *s, char **e, int b) { return strtoull(s, e, b); }
HLE("strtod", double, h_strtod, const char *s, char **e) { return strtod(s, e); }
HLE("strtof", float, h_strtof, const char *s, char **e) { return strtof(s, e); }
HLE("sqrtf", float, h_sqrtf, float x) { return sqrtf(x); }
HLE("cbrtf", float, h_cbrtf, float x) { return cbrtf(x); }
HLE("expf", float, h_expf, float x) { return expf(x); }
HLE("acosf", float, h_acosf, float x) { return acosf(x); }
HLE("cosf", float, h_cosf, float x) { return cosf(x); }
HLE("sinf", float, h_sinf, float x) { return sinf(x); }
HLE("atan2f", float, h_atan2f, float y, float x) { return atan2f(y, x); }
HLE("fmodf", float, h_fmodf, float x, float y) { return fmodf(x, y); }
HLE("cos", double, h_cos, double x) { return cos(x); }
HLE("sin", double, h_sin, double x) { return sin(x); }
HLE("powf", float, h_powf, float x, float y) { return powf(x, y); }
HLE("atanf", float, h_atanf, float x) { return atanf(x); }
HLE("tanf", float, h_tanf, float x) { return tanf(x); }
HLE("asinf", float, h_asinf, float x) { return asinf(x); }
HLE("log10", double, h_log10, double x) { return log10(x); }
HLE("pow", double, h_pow, double x, double y) { return pow(x, y); }
HLE("logf", float, h_logf, float x) { return logf(x); }
HLE("ldexpf", float, h_ldexpf, float x, int e) { return ldexpf(x, e); }
HLE("log10f", float, h_log10f, float x) { return log10f(x); }
HLE("log", double, h_log, double x) { return log(x); }
HLE("ldexp", double, h_ldexp, double x, int e) { return ldexp(x, e); }
HLE("frexp", double, h_frexp, double x, int *e) { return frexp(x, e); }
HLE("atan2", double, h_atan2, double y, double x) { return atan2(y, x); }
HLE("modff", float, h_modff, float x, float *i) { return modff(x, i); }
HLE("exp2f", float, h_exp2f, float x) { return exp2f(x); }
HLE("exp2", double, h_exp2, double x) { return exp2(x); }
HLE("log2f", float, h_log2f, float x) { return log2f(x); }
HLE("log2", double, h_log2, double x) { return log2(x); }
HLE("lroundf", int64_t, h_lroundf, float x) { return llroundf(x); }
// struct tm has the same layout in the SDK's musl-derived C library and glibc (tm_gmtoff and tm_zone included)
HLE("mktime", time_t, h_mktime, struct tm *t) { return mktime(t); }
HLE("timegm", time_t, h_timegm, struct tm *t) { return timegm(t); }
HLE("gmtime_r", struct tm *, h_gmtime_r, const time_t *t, struct tm *o) { return gmtime_r(t, o); }
HLE("localtime_r", struct tm *, h_localtime_r, const time_t *t, struct tm *o) { return localtime_r(t, o); }
HLE("strftime", size_t, h_strftime, char *s, size_t n, const char *f, const struct tm *t) { return strftime(s, n, f, t); }

// qsort with a guest comparator: the sort runs on the host, each comparison is a call into guest code
static __thread guest_fn t_cmp;
static int cmp_guest(const void *a, const void *b) { return (int)GUEST_CALL(t_cmp, (uint64_t)a, (uint64_t)b); }
HLE("qsort", void, h_qsort, void *base, size_t n, size_t size, guest_fn cmp) {
    guest_fn saved = t_cmp;   // a comparator may itself sort
    t_cmp = cmp;
    qsort(base, n, size, cmp_guest);
    t_cmp = saved;
}
