// SPDX-License-Identifier: GPL-3.0-or-later
// AAMP, the parameter archives of Breath of the Wild (.bxml, .bmodellist, .baslist, .bphysics ... and BCML's logs
// deepmerge.aamp and aslist.aamp), version 2, little-endian, as ZeldaMods' oead reads and writes them: a tree of lists,
// objects and parameters, each named by the CRC-32 of its name. BCML keeps a mod's changes to these files as differences
// (only the lists, objects and parameters it changes), merged into the game's file when the mod is installed: aamp_merge
// does that merge, BCML's DeepMerger and (aslist) ASListMerger.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum {   // parameter types
    AAMP_BOOL, AAMP_F32, AAMP_INT, AAMP_VEC2, AAMP_VEC3, AAMP_VEC4, AAMP_COLOR, AAMP_STR32, AAMP_STR64, AAMP_CURVE1,
    AAMP_CURVE2, AAMP_CURVE3, AAMP_CURVE4, AAMP_BUFINT, AAMP_BUFF32, AAMP_STR256, AAMP_QUAT, AAMP_U32, AAMP_BUFU32,
    AAMP_BUFBIN, AAMP_STRREF, AAMP_NTYPES
};
// a parameter's value as bytes: a string's characters (no NUL), a buffer's elements, other types their fixed bytes
typedef struct { uint32_t name; uint8_t type; uint32_t n; uint8_t *v; uint32_t at; } aamp_param;
typedef struct { uint32_t name; int np; aamp_param *p; uint32_t at; } aamp_obj;
typedef struct aamp_list { uint32_t name; int nl, no; struct aamp_list *l; aamp_obj *o; uint32_t at; } aamp_list;
typedef struct { uint32_t version; char type[128]; aamp_list root; } aamp_pio;   // root: param_root

uint32_t aamp_crc(const char *s);
int aamp_read(const uint8_t *in, size_t n, aamp_pio *p);   // 0 read; -1 not an AAMP this reads
uint8_t *aamp_write(const aamp_pio *p, size_t *on);         // NULL when an offset does not fit
void aamp_free(aamp_pio *p);
void aamp_list_free(aamp_list *l);

aamp_list *aamp_find_list(const aamp_list *l, uint32_t name);
aamp_obj *aamp_find_obj(const aamp_list *l, uint32_t name);
aamp_param *aamp_find_param(const aamp_obj *o, uint32_t name);
// a string parameter's value (malloc'd), NULL when it is not one
char *aamp_str(const aamp_param *p);
void aamp_set(aamp_obj *o, uint32_t name, uint8_t type, const void *v, uint32_t n);   // replaced, or added at the end
aamp_obj *aamp_add_obj(aamp_list *l, uint32_t name);
aamp_list *aamp_add_list(aamp_list *l, uint32_t name);

// BCML's merge of a difference into a document: lists and objects it has that the document lacks added, the others
// merged, parameters set; aslist: the AS list merger's AddReses, ASDefines and CFDefines merged by their names
void aamp_merge(aamp_list *doc, const aamp_list *diff, int aslist);
