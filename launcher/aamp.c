// SPDX-License-Identifier: GPL-3.0-or-later
// AAMP (aamp.h). The layout, as oead (ZeldaMods) documents it:
//   header 0x30: "AAMP", version 2, flags (1 little-endian, 2 UTF-8), file size, the document's version, the root list's
//     offset from 0x30, counts of lists / objects / parameters, sizes of the data and string sections; then the
//     document's type string ("xml"), NUL-ended, to 4 bytes
//   list (12 bytes): name CRC-32, its lists' offset / 4 (u16, from the list), their count (u16), its objects' offset / 4
//     and count
//   object (8): name CRC-32, its parameters' offset / 4 (u16, from the object), their count
//   parameter (8): name CRC-32, its value's offset / 4 (24 bits, from the parameter), type (8 bits)
//   values: in the data section; a buffer's element count is the u32 before it; strings in the string section, NUL-ended
// Written as oead writes them: the lists depth first with each list's lists together, then the objects, then the
// parameters, then the values (one copy of equal ones) and the strings (one copy of each).
#define _GNU_SOURCE
#include "aamp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t aamp_crc(const char *s) {
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; s++) { c ^= (uint8_t)*s; for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}
static const int FIXED[AAMP_NTYPES] = {4, 4, 4, 8, 12, 16, 16, -1, -1, 0x80, 0x100, 0x180, 0x200, -4, -4, -1, 16, 4, -4, -2, -1};
static int is_str(int t) { return t == AAMP_STR32 || t == AAMP_STR64 || t == AAMP_STR256 || t == AAMP_STRREF; }
static int buf_elem(int t) { return t == AAMP_BUFBIN ? 1 : 4; }
static int is_buf(int t) { return t == AAMP_BUFINT || t == AAMP_BUFF32 || t == AAMP_BUFU32 || t == AAMP_BUFBIN; }

static uint32_t r16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }

// ---------- reading ----------
typedef struct { const uint8_t *d; size_t n; } rd_t;
// Reads a parameter: its name, type and value (a string up to its NUL, a buffer by its count).
static int read_param(rd_t *r, size_t at, aamp_param *p) {
    if (at + 8 > r->n) return -1;
    p->name = r32(r->d + at);
    size_t off = at + ((size_t)(r32(r->d + at + 4) & 0xFFFFFF) << 2);
    p->type = r->d[at + 7];
    if (p->type >= AAMP_NTYPES) return -1;
    int f = FIXED[p->type];
    if (is_str(p->type)) {
        if (off > r->n) return -1;
        const uint8_t *e = memchr(r->d + off, 0, r->n - off);
        size_t len = e ? (size_t)(e - (r->d + off)) : r->n - off;
        p->n = (uint32_t)len;
    } else if (is_buf(p->type)) {
        if (off < 4 || off > r->n) return -1;
        uint64_t cnt = r32(r->d + off - 4);
        if (cnt * (uint64_t)buf_elem(p->type) > r->n - off) return -1;
        p->n = (uint32_t)(cnt * (uint64_t)buf_elem(p->type));
    } else {
        if (off + (size_t)f > r->n) return -1;
        p->n = (uint32_t)f;
    }
    p->v = malloc(p->n + 1);
    if (!p->v) return -1;
    memcpy(p->v, r->d + off, p->n);
    p->v[p->n] = 0;
    return 0;
}
// Reads an object and its parameters.
static int read_obj(rd_t *r, size_t at, aamp_obj *o) {
    if (at + 8 > r->n) return -1;
    o->name = r32(r->d + at);
    size_t ps = at + (r16(r->d + at + 4) << 2);
    int np = (int)r16(r->d + at + 6);
    o->p = calloc((size_t)np + 1, sizeof *o->p);
    if (!o->p) return -1;
    for (int i = 0; i < np; i++) { if (read_param(r, ps + 8 * (size_t)i, &o->p[i])) return -1; o->np = i + 1; }
    return 0;
}
// Reads a list, its lists and its objects (up to 64 deep).
static int read_list(rd_t *r, size_t at, aamp_list *l, int depth) {
    if (depth > 64 || at + 12 > r->n) return -1;
    l->name = r32(r->d + at);
    size_t ls = at + (r16(r->d + at + 4) << 2), os = at + (r16(r->d + at + 8) << 2);
    int nl = (int)r16(r->d + at + 6), no = (int)r16(r->d + at + 10);
    l->l = calloc((size_t)nl + 1, sizeof *l->l), l->o = calloc((size_t)no + 1, sizeof *l->o);
    if (!l->l || !l->o) return -1;
    for (int i = 0; i < nl; i++) { l->nl = i + 1; if (read_list(r, ls + 12 * (size_t)i, &l->l[i], depth + 1)) return -1; }
    for (int i = 0; i < no; i++) { l->no = i + 1; if (read_obj(r, os + 8 * (size_t)i, &l->o[i])) return -1; }
    return 0;
}
// Reads an AAMP document (version 2, little-endian, UTF-8) into a tree; -1 when it is not one.
int aamp_read(const uint8_t *in, size_t n, aamp_pio *p) {
    memset(p, 0, sizeof *p);
    if (n < 0x34 || memcmp(in, "AAMP", 4) || r32(in + 4) != 2 || (r32(in + 8) & 3) != 3) return -1;
    p->version = r32(in + 0x10);
    size_t tl = strnlen((const char *)in + 0x30, n - 0x30);
    if (tl >= sizeof p->type) return -1;
    memcpy(p->type, in + 0x30, tl);
    rd_t r = {in, n};
    if (read_list(&r, 0x30 + (size_t)r32(in + 0x14), &p->root, 0)) { aamp_free(p); return -1; }
    return 0;
}
void aamp_list_free(aamp_list *l) {
    for (int i = 0; i < l->nl; i++) aamp_list_free(&l->l[i]);
    for (int i = 0; i < l->no; i++) { for (int k = 0; k < l->o[i].np; k++) free(l->o[i].p[k].v); free(l->o[i].p); }
    free(l->l), free(l->o);
    l->l = NULL, l->o = NULL, l->nl = l->no = 0;
}
void aamp_free(aamp_pio *p) { aamp_list_free(&p->root); }

// ---------- writing ----------
typedef struct { uint8_t *b; size_t n, cap; int bad; uint32_t nl, no, np; } wr_t;
// Appends bytes (or zeros) to the output, growing it; where they went.
static size_t put(wr_t *w, const void *d, size_t n) {
    if (w->n + n + 4 > w->cap) { while (w->n + n + 4 > w->cap) w->cap = w->cap ? w->cap * 2 : 4096; w->b = realloc(w->b, w->cap); }
    size_t at = w->n;
    if (d) memcpy(w->b + at, d, n); else memset(w->b + at, 0, n);
    w->n += n;
    return at;
}
static void align4(wr_t *w) { while (w->n & 3) put(w, NULL, 1); }
static void set_off16(wr_t *w, size_t field, size_t from, size_t to) {
    size_t d = to - from;
    if (to < from || d & 3 || d / 4 > 0xFFFF) { w->bad = 1; return; }
    w16(w->b + field, (uint32_t)(d / 4));
}
static void list_struct(wr_t *w, aamp_list *l) {
    uint8_t s[12] = {0};
    w32(s, l->name), w16(s + 6, (uint32_t)l->nl), w16(s + 10, (uint32_t)l->no);
    l->at = (uint32_t)put(w, s, 12);
    w->nl++;
}
static void write_lists(wr_t *w, aamp_list *l) {
    set_off16(w, l->at + 4, l->at, w->n);
    for (int i = 0; i < l->nl; i++) list_struct(w, &l->l[i]);
    for (int i = 0; i < l->nl; i++) write_lists(w, &l->l[i]);
}
// Writes every list's objects, in the order the lists were written.
static void write_objs(wr_t *w, aamp_list *l) {
    set_off16(w, l->at + 8, l->at, w->n);
    for (int i = 0; i < l->no; i++) {
        uint8_t s[8] = {0};
        w32(s, l->o[i].name), w16(s + 6, (uint32_t)l->o[i].np);
        l->o[i].at = (uint32_t)put(w, s, 8);
        w->no++;
    }
    for (int i = 0; i < l->nl; i++) write_objs(w, &l->l[i]);
}
// Writes every object's parameters (a list's lists first, as oead does).
static void write_params(wr_t *w, aamp_list *l) {
    for (int i = 0; i < l->nl; i++) write_params(w, &l->l[i]);
    for (int i = 0; i < l->no; i++) {
        aamp_obj *o = &l->o[i];
        set_off16(w, o->at + 4, o->at, w->n);
        for (int k = 0; k < o->np; k++) {
            uint8_t s[8] = {0};
            w32(s, o->p[k].name), s[7] = o->p[k].type;
            o->p[k].at = (uint32_t)put(w, s, 8);
            w->np++;
        }
    }
}
static void set_off24(wr_t *w, const aamp_param *p, size_t to) {
    size_t d = to - p->at;
    if (to < p->at || d & 3 || d / 4 >= 1u << 24) { w->bad = 1; return; }
    uint32_t v = (uint32_t)(d / 4) | (uint32_t)p->type << 24;
    w32(w->b + p->at + 4, v);
}
// Writes the parameters' values (not strings) to the data section, sharing equal ones, and points each parameter at its
// value.
static void write_data(wr_t *w, aamp_list *l, size_t start) {
    for (int i = 0; i < l->nl; i++) write_data(w, &l->l[i], start);
    for (int i = 0; i < l->no; i++)
        for (int k = 0; k < l->o[i].np; k++) {
            aamp_param *p = &l->o[i].p[k];
            if (is_str(p->type)) continue;
            size_t pre = is_buf(p->type) ? 4 : 0, len = pre + p->n, pad = (len + 3) & ~(size_t)3;
            uint8_t *t = calloc(1, pad + 1);
            if (pre) w32(t, p->n / (uint32_t)buf_elem(p->type));
            memcpy(t + pre, p->v, p->n);
            size_t found = 0;
            for (size_t o = start; o + len <= w->n; o += 4) if (!memcmp(w->b + o, t, len)) { found = o + pre; break; }   // an equal value written before
            if (!found) { found = put(w, t, pad) + pre; }
            free(t);
            set_off24(w, p, found);
        }
}
typedef struct { char *s; size_t at; } sref;
// Writes the string parameters to the string section, one copy of each string.
static void write_strings(wr_t *w, aamp_list *l, sref **tab, int *nt) {
    for (int i = 0; i < l->nl; i++) write_strings(w, &l->l[i], tab, nt);
    for (int i = 0; i < l->no; i++)
        for (int k = 0; k < l->o[i].np; k++) {
            aamp_param *p = &l->o[i].p[k];
            if (!is_str(p->type)) continue;
            size_t at = 0;
            int j = 0;
            while (j < *nt && strcmp((*tab)[j].s, (const char *)p->v)) j++;
            if (j < *nt) at = (*tab)[j].at;
            else {
                at = put(w, p->v, p->n);
                put(w, NULL, 1);
                align4(w);
                *tab = realloc(*tab, (size_t)(*nt + 1) * sizeof **tab);
                (*tab)[*nt].s = (char *)p->v, (*tab)[(*nt)++].at = at;
            }
            set_off24(w, p, at);
        }
}
// Writes a tree as an AAMP document laid out as oead (and the game's files) do; NULL when an offset does not fit.
uint8_t *aamp_write(const aamp_pio *pc, size_t *on) {
    aamp_pio *p = (aamp_pio *)pc;   // (the positions are noted in the tree while writing)
    wr_t w = {0};
    put(&w, NULL, 0x30);
    put(&w, p->type, strlen(p->type) + 1);
    align4(&w);
    size_t pio = w.n;
    list_struct(&w, &p->root);
    write_lists(&w, &p->root);
    write_objs(&w, &p->root);
    write_params(&w, &p->root);
    size_t data = w.n;
    write_data(&w, &p->root, data);
    align4(&w);
    size_t strs = w.n;
    sref *tab = NULL;
    int nt = 0;
    write_strings(&w, &p->root, &tab, &nt);
    free(tab);
    align4(&w);
    if (w.bad) { free(w.b); return NULL; }
    uint8_t *h = w.b;
    memcpy(h, "AAMP", 4), w32(h + 4, 2), w32(h + 8, 3), w32(h + 0xC, (uint32_t)w.n), w32(h + 0x10, p->version);
    w32(h + 0x14, (uint32_t)(pio - 0x30)), w32(h + 0x18, w.nl), w32(h + 0x1C, w.no), w32(h + 0x20, w.np);
    w32(h + 0x24, (uint32_t)(strs - data)), w32(h + 0x28, (uint32_t)(w.n - strs)), w32(h + 0x2C, 0);
    *on = w.n;
    return w.b;
}

// ---------- the tree ----------
aamp_list *aamp_find_list(const aamp_list *l, uint32_t name) { for (int i = 0; i < l->nl; i++) if (l->l[i].name == name) return &l->l[i]; return NULL; }
aamp_obj *aamp_find_obj(const aamp_list *l, uint32_t name) { for (int i = 0; i < l->no; i++) if (l->o[i].name == name) return &l->o[i]; return NULL; }
aamp_param *aamp_find_param(const aamp_obj *o, uint32_t name) { for (int i = 0; o && i < o->np; i++) if (o->p[i].name == name) return &o->p[i]; return NULL; }
char *aamp_str(const aamp_param *p) { return p && is_str(p->type) ? strndup((const char *)p->v, p->n) : NULL; }
// Sets a parameter of an object (replaced, or added at the end) with a copy of the value.
void aamp_set(aamp_obj *o, uint32_t name, uint8_t type, const void *v, uint32_t n) {
    aamp_param *p = aamp_find_param(o, name);
    if (!p) { o->p = realloc(o->p, (size_t)(o->np + 1) * sizeof *o->p); p = &o->p[o->np++]; memset(p, 0, sizeof *p); p->name = name; }
    free(p->v);
    p->type = type, p->n = n, p->v = malloc((size_t)n + 1);
    memcpy(p->v, v, n);
    p->v[n] = 0;
}
// (a list's lists and objects move when it grows: pointers to them are taken again after adding)
aamp_obj *aamp_add_obj(aamp_list *l, uint32_t name) {
    l->o = realloc(l->o, (size_t)(l->no + 1) * sizeof *l->o);
    aamp_obj *o = &l->o[l->no++];
    memset(o, 0, sizeof *o);
    o->name = name;
    return o;
}
// Adds an empty list to a list (its lists may move: take pointers into them again after).
aamp_list *aamp_add_list(aamp_list *l, uint32_t name) {
    l->l = realloc(l->l, (size_t)(l->nl + 1) * sizeof *l->l);
    aamp_list *c = &l->l[l->nl++];
    memset(c, 0, sizeof *c);
    c->name = name;
    return c;
}
static void copy_obj(aamp_obj *d, const aamp_obj *s) {
    d->name = s->name, d->np = 0, d->p = NULL;
    for (int i = 0; i < s->np; i++) aamp_set(d, s->p[i].name, s->p[i].type, s->p[i].v, s->p[i].n);
}
static void copy_list(aamp_list *d, const aamp_list *s) {
    memset(d, 0, sizeof *d);
    d->name = s->name;
    for (int i = 0; i < s->nl; i++) copy_list(aamp_add_list(d, 0), &s->l[i]);
    for (int i = 0; i < s->no; i++) copy_obj(aamp_add_obj(d, 0), &s->o[i]);
}

// ---------- BCML's merges ----------
static uint32_t crcf(const char *fmt, int i) { char b[64]; snprintf(b, sizeof b, fmt, i); return aamp_crc(b); }
static char *obj_str(const aamp_obj *o, const char *name) { return aamp_str(aamp_find_param(o, aamp_crc(name))); }
static void set_str(aamp_obj *o, const char *name, uint8_t type, const char *s) { aamp_set(o, aamp_crc(name), type, s, (uint32_t)strlen(s)); }
typedef struct { char **v; int n; } strs;
static int strs_has(const strs *s, const char *x) { for (int i = 0; i < s->n; i++) if (!strcmp(s->v[i], x)) return i; return -1; }
static void strs_add(strs *s, char *x) { s->v = realloc(s->v, (size_t)(s->n + 1) * sizeof *s->v); s->v[s->n++] = x; }
static void strs_free(strs *s) { for (int i = 0; i < s->n; i++) free(s->v[i]); free(s->v); s->v = NULL, s->n = 0; }

// AddReses: the animations of both, each once; AddRes_<i> in that order
static void merge_addres(aamp_list *doc, const aamp_list *diff) {
    strs names = {0};
    for (int pass = 0; pass < 2; pass++) {
        const aamp_list *l = pass ? diff : doc;
        for (int i = 0; i < l->no; i++) { char *a = obj_str(&l->o[i], "Anim"); if (a && strs_has(&names, a) < 0) strs_add(&names, a); else free(a); }
    }
    for (int i = 0; i < names.n; i++) {
        aamp_obj *o = aamp_find_obj(doc, crcf("AddRes_%d", i));
        if (!o) o = aamp_add_obj(doc, crcf("AddRes_%d", i));
        set_str(o, "Anim", AAMP_STR64, names.v[i]);
    }
    strs_free(&names);
}
// ASDefines: a define of the same name gets the diff's file, a new one is added
static void merge_asdefine(aamp_list *doc, const aamp_list *diff) {
    strs listing = {0};
    for (int i = 0; i < doc->no; i++) { char *nm = obj_str(&doc->o[i], "Name"); strs_add(&listing, nm ? nm : strdup("")); }
    int base = listing.n;
    for (int i = 0; i < diff->no; i++) {
        char *nm = obj_str(&diff->o[i], "Name"), *fn = obj_str(&diff->o[i], "Filename");
        if (!nm || !fn) { free(nm), free(fn); continue; }
        int k = strs_has(&listing, nm);
        aamp_obj *o;
        if (k >= 0) {
            if (!(o = aamp_find_obj(doc, crcf("ASDefine_%d", k)))) o = k < base ? &doc->o[k] : aamp_add_obj(doc, crcf("ASDefine_%d", k));
            free(nm);
        } else {
            k = listing.n;
            strs_add(&listing, nm);
            o = aamp_add_obj(doc, crcf("ASDefine_%d", k));
            set_str(o, "Name", AAMP_STR64, nm);
        }
        set_str(o, "Filename", AAMP_STR64, fn);
        free(fn);
    }
    strs_free(&listing);
}
// CFDefines: a define (CFPre's Name) of the same name gets the diff's excepts added and its posts' values; others added
typedef struct { char *name; aamp_param frame, rate; int has_f, has_r; } cfpost;
typedef struct { char *name; strs excepts; cfpost *posts; int np; } cfdef;
// Reads a CF define (its name, excepts and posts).
static void cf_read(const aamp_list *l, cfdef *c) {
    memset(c, 0, sizeof *c);
    c->name = obj_str(aamp_find_obj(l, aamp_crc("CFPre")), "Name");
    if (!c->name) c->name = strdup("");
    const aamp_obj *ex = aamp_find_obj(l, aamp_crc("CFExcepts"));
    for (int i = 0; ex && i < ex->np; i++) { char *s = aamp_str(&ex->p[i]); if (s) strs_add(&c->excepts, s); }
    const aamp_list *po = aamp_find_list(l, aamp_crc("CFPosts"));
    for (int i = 0; po && i < po->no; i++) {
        char *nm = obj_str(&po->o[i], "Name");
        if (!nm) continue;
        c->posts = realloc(c->posts, (size_t)(c->np + 1) * sizeof *c->posts);
        cfpost *p = &c->posts[c->np++];
        memset(p, 0, sizeof *p);
        p->name = nm;
        const aamp_param *f = aamp_find_param(&po->o[i], aamp_crc("Frame")), *r = aamp_find_param(&po->o[i], aamp_crc("StartFrameRate"));
        if (f) p->frame = *f, p->has_f = 1;
        if (r) p->rate = *r, p->has_r = 1;
    }
}
static void cf_free(cfdef *c) {
    free(c->name);
    strs_free(&c->excepts);
    for (int i = 0; i < c->np; i++) free(c->posts[i].name);
    free(c->posts);
}
static void cf_write(aamp_list *l, const cfdef *c) {   // the define as BCML writes it (CFDefine.to_plist)
    uint32_t name = l->name;
    aamp_list_free(l);
    l->name = name;
    set_str(aamp_add_obj(l, aamp_crc("CFPre")), "Name", AAMP_STR32, c->name);
    if (c->excepts.n) {
        aamp_obj *ex = aamp_add_obj(l, aamp_crc("CFExcepts"));
        for (int i = 0; i < c->excepts.n; i++) aamp_set(ex, crcf("Name_%d", i), AAMP_STR32, c->excepts.v[i], (uint32_t)strlen(c->excepts.v[i]));
    }
    if (c->np) {
        aamp_list *po = aamp_add_list(l, aamp_crc("CFPosts"));
        for (int i = 0; i < c->np; i++) {
            aamp_obj *o = aamp_add_obj(po, crcf("CFPost_%d", i));
            set_str(o, "Name", AAMP_STR32, c->posts[i].name);
            if (c->posts[i].has_f) aamp_set(o, aamp_crc("Frame"), c->posts[i].frame.type, c->posts[i].frame.v, c->posts[i].frame.n);
            if (c->posts[i].has_r) aamp_set(o, aamp_crc("StartFrameRate"), c->posts[i].rate.type, c->posts[i].rate.v, c->posts[i].rate.n);
        }
    }
}
// Merges a diff's CF defines into the document's: by name, their excepts added and their posts' values set; new ones
// added.
static void merge_cfdefines(aamp_list *doc, const aamp_list *diff) {
    strs listing = {0};
    for (int i = 0; i < doc->nl; i++) { char *nm = obj_str(aamp_find_obj(&doc->l[i], aamp_crc("CFPre")), "Name"); strs_add(&listing, nm ? nm : strdup("")); }
    int next = listing.n;
    for (int i = 0; i < diff->nl; i++) {
        cfdef d;
        cf_read(&diff->l[i], &d);
        int k = strs_has(&listing, d.name);
        aamp_list *t = k >= 0 ? aamp_find_list(doc, crcf("CFDefine_%d", k)) : NULL;
        if (k >= 0 && !t) t = &doc->l[k];
        if (t) {
            cfdef b;   // (the define is read again: it may have been rewritten by an earlier one of the diff's)
            cf_read(t, &b);
            for (int e = 0; e < d.excepts.n; e++) if (strs_has(&b.excepts, d.excepts.v[e]) < 0) strs_add(&b.excepts, strdup(d.excepts.v[e]));
            for (int q = 0; q < d.np; q++) {
                int j = 0;
                while (j < b.np && strcmp(b.posts[j].name, d.posts[q].name)) j++;
                if (j == b.np) { b.posts = realloc(b.posts, (size_t)(b.np + 1) * sizeof *b.posts); b.posts[b.np] = d.posts[q]; b.posts[b.np++].name = strdup(d.posts[q].name); continue; }
                if (d.posts[q].has_f) b.posts[j].frame = d.posts[q].frame, b.posts[j].has_f = 1;
                if (d.posts[q].has_r) b.posts[j].rate = d.posts[q].rate, b.posts[j].has_r = 1;
            }
            // the values point into both trees until written: written into a copy, then swapped in
            aamp_list nl = {0};
            nl.name = t->name;
            cf_write(&nl, &b);
            cf_free(&b);
            aamp_list_free(t);
            *t = nl;
        } else {
            aamp_list nl = {0};
            nl.name = crcf("CFDefine_%d", next++);
            cf_write(&nl, &d);
            strs_add(&listing, strdup(d.name));
            *aamp_add_list(doc, 0) = nl;
        }
        cf_free(&d);
    }
    strs_free(&listing);
}
// BCML's merge of a difference into a document (aslist: the AS list merger's three special lists merged by name).
void aamp_merge(aamp_list *doc, const aamp_list *diff, int aslist) {
    static uint32_t ADDRES, ASDEF, CFDEF;
    if (!ADDRES) ADDRES = aamp_crc("AddReses"), ASDEF = aamp_crc("ASDefines"), CFDEF = aamp_crc("CFDefines");
    for (int i = 0; i < diff->nl; i++) {
        const aamp_list *s = &diff->l[i];
        aamp_list *t = aamp_find_list(doc, s->name);
        if (aslist && (s->name == ADDRES || s->name == ASDEF || s->name == CFDEF)) {
            if (!t) t = aamp_add_list(doc, s->name);
            if (s->name == ADDRES) merge_addres(t, s);
            else if (s->name == ASDEF) merge_asdefine(t, s);
            else merge_cfdefines(t, s);
        } else if (t) aamp_merge(t, s, aslist);
        else { aamp_list c; copy_list(&c, s); *aamp_add_list(doc, 0) = c; }
    }
    for (int i = 0; i < diff->no; i++) {
        const aamp_obj *s = &diff->o[i];
        aamp_obj *t = aamp_find_obj(doc, s->name);
        if (!t) { aamp_obj c; copy_obj(&c, s); *aamp_add_obj(doc, 0) = c; continue; }
        for (int k = 0; k < s->np; k++) aamp_set(t, s->p[k].name, s->p[k].type, s->p[k].v, s->p[k].n);
    }
}
