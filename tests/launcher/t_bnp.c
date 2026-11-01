// SPDX-License-Identifier: GPL-3.0-or-later
// NativeNX's BCML mods (launcher/bnp.c) on a made-up game and mod laid out as BotW's and BCML's: Yaz0 and SARC read
// and written, a stripped actor pack merged with the game's (the mod's files over the game's, the game's others kept,
// new ones added), a pack inside a pack merged too, an option over the mod (and not when it is off), the size table
// from the game's with the mod's sizes, the other files as they are, and the mod's info (name, options) read; AAMP
// read and written, and BCML's AAMP logs merged (deepmerge.aamp: into a pack, into a pack the mod lacks; aslist.aamp:
// AS defines, animations, CF defines, in a pack in a pack), with the size table grown for what got bigger.
// usage: t_bnp <work folder>
#define _GNU_SOURCE
#include "../../launcher/bnp.h"
#include "../../launcher/aamp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../test_fs.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("ok - " __VA_ARGS__); else { printf("not ok - " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static void mk(const char *p) { tfs_mkdirs(p); }
static void put(const char *dir, const char *rel, const void *d, size_t n) {
    char p[1024], q[1024];
    snprintf(p, sizeof p, "%s/%s", dir, rel);
    snprintf(q, sizeof q, "%s", p);
    *strrchr(q, '/') = 0;
    mk(q);
    FILE *f = fopen(p, "wb");
    fwrite(d, 1, n, f);
    fclose(f);
}
static void puts_(const char *dir, const char *rel, const char *s) { put(dir, rel, s, strlen(s)); }
static uint8_t *sarc(size_t *n, int cnt, ...) {   // name, text, name, text ...; a text "\x01" + bytes len: raw data
    va_list ap;
    va_start(ap, cnt);
    sarc_ent e[8] = {{0}};
    for (int i = 0; i < cnt; i++) {
        e[i].name = strdup(va_arg(ap, const char *));
        const uint8_t *d = va_arg(ap, const uint8_t *);
        size_t len = va_arg(ap, size_t);
        e[i].data = d, e[i].size = len, e[i].align = 4;
    }
    va_end(ap);
    uint8_t *o = sarc_write(e, cnt, n);
    for (int i = 0; i < cnt; i++) free(e[i].name);
    return o;
}
#define T(s) (const uint8_t *)(s), (size_t)strlen(s)
static uint8_t *yaz(const uint8_t *d, size_t n, size_t *on) { return yaz0_store(d, n, NULL, on); }
// a file of a SARC (Yaz0 or not) by name, as text
static int has(const uint8_t *d, size_t n, const char *name, const char *want) {
    uint8_t *dec = NULL;
    size_t dn;
    if (n >= 4 && !memcmp(d, "Yaz0", 4)) { if (yaz0_decode(d, n, &dec, &dn)) return 0; d = dec, n = dn; }
    sarc_ent *e;
    int k = sarc_read(d, n, &e), ok = 0;
    for (int i = 0; i < k; i++)
        if (!strcmp(e[i].name, name)) ok = want ? e[i].size == strlen(want) && !memcmp(e[i].data, want, e[i].size) : 1;
    if (k >= 0) sarc_free(e, k);
    free(dec);
    return ok;
}
static uint8_t *inner(const uint8_t *d, size_t n, const char *name, size_t *on) {   // a file of a SARC, copied
    sarc_ent *e;
    int k = sarc_read(d, n, &e);
    uint8_t *o = NULL;
    for (int i = 0; i < k; i++) if (!strcmp(e[i].name, name)) { o = malloc(e[i].size); memcpy(o, e[i].data, e[i].size); *on = e[i].size; }
    if (k >= 0) sarc_free(e, k);
    return o;
}
static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long s = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)s + 1);
    *n = fread(b, 1, (size_t)s, f);
    fclose(f);
    return b;
}

// AAMP documents for the test
static void sp(aamp_obj *o, const char *n, const char *v) { aamp_set(o, aamp_crc(n), AAMP_STR64, v, (uint32_t)strlen(v)); }
static void ip(aamp_obj *o, const char *n, int v) { aamp_set(o, aamp_crc(n), AAMP_INT, &v, 4); }
static aamp_obj *ob(aamp_list *l, const char *n) { return aamp_add_obj(l, aamp_crc(n)); }
static aamp_list *ls(aamp_list *l, const char *n) { return aamp_add_list(l, aamp_crc(n)); }
static void pio_init(aamp_pio *p) { memset(p, 0, sizeof *p); strcpy(p->type, "xml"); p->root.name = aamp_crc("param_root"); }
static uint8_t *pio_bin(aamp_pio *p, size_t *n) { uint8_t *b = aamp_write(p, n); aamp_free(p); return b; }
static int geti(const aamp_list *l, const char *obj, const char *par, int *v) {
    const aamp_param *p = aamp_find_param(aamp_find_obj(l, aamp_crc(obj)), aamp_crc(par));
    if (!p || p->type != AAMP_INT) return 0;
    memcpy(v, p->v, 4);
    return 1;
}
static int gets_(const aamp_list *l, const char *obj, const char *par, const char *want) {
    char *s = aamp_str(aamp_find_param(aamp_find_obj(l, aamp_crc(obj)), aamp_crc(par)));
    int ok = s && !strcmp(s, want);
    free(s);
    return ok;
}
// a file of a pack (Yaz0 or not), packs inside by "a//b", read as AAMP
static int pack_aamp(const uint8_t *d, size_t n, const char *path, aamp_pio *out) {
    uint8_t *dec = NULL;
    size_t dn;
    if (n >= 4 && !memcmp(d, "Yaz0", 4)) { if (yaz0_decode(d, n, &dec, &dn)) return 0; d = dec, n = dn; }
    const char *sep = strstr(path, "//");
    char name[256];
    snprintf(name, sizeof name, "%.*s", sep ? (int)(sep - path) : (int)strlen(path), path);
    size_t in;
    uint8_t *f = inner(d, n, name, &in);
    int ok = f && (sep ? pack_aamp(f, in, sep + 2, out) : !aamp_read(f, in, out));
    free(f), free(dec);
    return ok;
}
static char g_stock[600], g_dlc[600];
static int rd(void *ctx, const char *path, uint8_t **d, size_t *n) {
    (void)ctx;
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", g_stock, path);
    *d = slurp(p, n);
    return *d ? 0 : -1;
}
static int rd_aoc(void *ctx, const char *path, uint8_t **d, size_t *n) {   // the DLC's own files
    (void)ctx;
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", g_dlc, path);
    *d = slurp(p, n);
    return *d ? 0 : -1;
}
static const char *RS = "System/Resource/ResourceSizeTable.product.srsizetable";
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char w[512];
    snprintf(w, sizeof w, "%s", argv[1]);
    tfs_rmtree(w);
    snprintf(g_stock, sizeof g_stock, "%s/game", w);
    snprintf(g_dlc, sizeof g_dlc, "%s/dlc", w);
    // the formats themselves
    uint8_t x[100];
    for (int i = 0; i < 100; i++) x[i] = (uint8_t)(i * 3);
    size_t yn, dn;
    uint8_t *y = yaz(x, 100, &yn), *dd = NULL;
    CHECK(!yaz0_decode(y, yn, &dd, &dn) && dn == 100 && !memcmp(dd, x, 100), "Yaz0: stored and read back");
    static const uint8_t real[] = {'Y', 'a', 'z', '0', 0, 0, 0, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0xF0, 'a', 'b', 'c', 'd', 0x60, 0x03};   // "abcd", then 8 bytes from 4 back
    uint8_t *rr = NULL;
    size_t rn;
    CHECK(!yaz0_decode(real, sizeof real, &rr, &rn) && rn == 12 && !memcmp(rr, "abcdabcdabcd", 12), "Yaz0: a back reference");
    free(y), free(dd), free(rr);
    // the game: an actor pack (Yaz0), a pack with a pack inside, a model, the size table
    size_t n1, n2, n3, n4;
    // AAMP: a parameter list (General: Life 10, Speed 1.5; a list Sub), an AS list (a define, an animation, a CF define)
    aamp_pio gp;
    pio_init(&gp);
    aamp_obj *go = ob(&gp.root, "General");
    ip(go, "Life", 10);
    float sp15 = 1.5f;
    aamp_set(go, aamp_crc("Speed"), AAMP_F32, &sp15, 4);
    sp(ob(ls(&gp.root, "Sub"), "X"), "Name", "stock");
    size_t gpn, asn, opn;
    uint8_t *gpb = pio_bin(&gp, &gpn);
    aamp_pio rt;
    CHECK(!aamp_read(gpb, gpn, &rt) && geti(&rt.root, "General", "Life", &(int){0}) && gets_(aamp_find_list(&rt.root, aamp_crc("Sub")), "X", "Name", "stock"), "AAMP: written and read back");
    size_t rtn;
    uint8_t *rtb = aamp_write(&rt, &rtn);
    CHECK(rtb && rtn == gpn && !memcmp(rtb, gpb, gpn), "AAMP: read and written again, the same bytes");
    aamp_free(&rt), free(rtb);
    aamp_pio as;
    pio_init(&as);
    aamp_list *asd = ls(&as.root, "ASDefines");
    aamp_obj *d0 = ob(asd, "ASDefine_0");
    sp(d0, "Name", "Wait"), sp(d0, "Filename", "Wait");
    sp(ob(ls(&as.root, "AddReses"), "AddRes_0"), "Anim", "Player_Link");
    aamp_list *cf = ls(ls(&as.root, "CFDefines"), "CFDefine_0");
    sp(ob(cf, "CFPre"), "Name", "A");
    sp(ob(cf, "CFExcepts"), "Name_0", "x");
    uint8_t *asb = pio_bin(&as, &asn);
    aamp_pio op;
    pio_init(&op);
    ip(ob(&op.root, "General"), "Life", 1);
    uint8_t *opb = pio_bin(&op, &opn);
    uint8_t *ap = sarc(&n1, 3, "Actor/ModelList/A.bmodellist", T("stock model"), "Actor/Physics/A.bphysics", T("stock phys"), "Actor/GeneralParamList/A.bgparamlist", gpb, gpn);
    uint8_t *apy = yaz(ap, n1, &n2);
    put(g_stock, "Actor/Pack/Armor_001_Head.sbactorpack", apy, n2);
    ap = sarc(&n1, 1, "Actor/GeneralParamList/O.bgparamlist", opb, opn);
    apy = yaz(ap, n1, &n2);
    put(g_stock, "Actor/Pack/Other.sbactorpack", apy, n2);
    uint8_t *in = sarc(&n1, 3, "x.txt", T("stock x"), "y.txt", T("stock y"), "Actor/ASList/I.baslist", asb, asn);
    uint8_t *iny = yaz(in, n1, &n3);
    uint8_t *tbg = sarc(&n4, 1, "Actor/Pack/Inner.sbactorpack", iny, n3);
    put(g_stock, "Pack/TitleBG.pack", tbg, n4);
    puts_(g_stock, "Model/Armor_001.sbfres", "stock bfres");
    // the DLC: an armour pack with a physics file in it
    aamp_pio dp;
    pio_init(&dp);
    ip(ob(&dp.root, "General"), "Life", 3);
    size_t dpn;
    uint8_t *dpb = pio_bin(&dp, &dpn);
    ap = sarc(&n1, 2, "Actor/Physics/D.bphysics", dpb, dpn, "Actor/ModelList/D.bmodellist", T("dlc model"));
    apy = yaz(ap, n1, &n2);
    put(g_dlc, "Actor/Pack/Aoc_Armor.sbactorpack", apy, n2);
    uint8_t *tab = calloc(1, 12);
    size_t tn = 12;
    memcpy(tab, "RSTB", 4);
    rstb_set(&tab, &tn, "Actor/Pack/Armor_001_Head.bactorpack", 1000);
    rstb_set(&tab, &tn, "Model/Armor_001.bfres", 500);
    rstb_set(&tab, &tn, "Other/Kept.byml", 777);
    rstb_set(&tab, &tn, "Other/Gone.byml", 300);
    rstb_set(&tab, &tn, "Aoc/0010/Actor/Pack/Aoc_Armor.bactorpack", 16);
    rstb_set(&tab, &tn, "Actor/Pack/Inner.bactorpack", 16);   // (too small for the merged pack: grown)
    uint8_t *tby = yaz(tab, tn, &n1);
    put(g_stock, RS, tby, n1);
    uint32_t v;
    CHECK(rstb_get(tab, tn, "Model/Armor_001.bfres", &v) && v == 500 && !rstb_get(tab, tn, "Nope", &v), "RSTB: entries set and found by name");
    // the mod, as a BCML .bnp unpacked: its pack holds only what it changes
    char mod[600], mr[700], opt[700], ol[700];
    snprintf(mod, sizeof mod, "%s/mods/Linkle", w);
    snprintf(mr, sizeof mr, "%s/01007EF00011E000/romfs", mod);
    puts_(mod, "info.json", "{\"name\": \"The Test Mod\", \"version\": \"3.0.1\", \"options\": {\"multi\": [{\"name\": \"Better \\\"Physics\\\"\", \"desc\": \"d\", \"folder\": \"Physics Opt\", \"default\": true}], \"single\": []}, \"platform\": \"switch\"}");
    puts_(mod, "logs/packs.json", "{\"Actor/Pack/Armor_001_Head.bactorpack\": \"01007EF00011E000\\\\romfs\\\\Actor\\\\Pack\\\\Armor_001_Head.sbactorpack\", \"Pack/TitleBG.pack\": \"01007EF00011E000\\\\romfs\\\\Pack\\\\TitleBG.pack\", \"Aoc/0010/Actor/Pack/Aoc_Armor.bactorpack\": \"01007EF00011F000\\\\romfs\\\\Actor\\\\Pack\\\\Aoc_Armor.sbactorpack\"}");
    puts_(mod, "logs/rstb.json", "{\n  \"Actor/Pack/Armor_001_Head.bactorpack\": 167952,\n  \"Model/Armor_001.bfres\": 99999,\n  \"Actor/New/N.bxml\": 4096,\n  \"Other/Kept.byml\": 5,\n  \"Other/Gone.byml\": 0\n}");
    // deepmerge.aamp: General's Life 99 and a new parameter, a new object, in the actor pack (which the mod also changes)
    // and in a pack the mod does not have; and in a DLC's pack
    const char *P1 = "01007EF00011E000/romfs/Actor/Pack/Armor_001_Head.sbactorpack//Actor/GeneralParamList/A.bgparamlist",
               *P2 = "01007EF00011E000\\romfs\\Actor\\Pack\\Other.sbactorpack//Actor/GeneralParamList/O.bgparamlist",
               *P3 = "01007EF00011F000/romfs/Actor/Pack/Aoc_Armor.sbactorpack//Actor/Physics/D.bphysics";
    aamp_pio dm;
    pio_init(&dm);
    aamp_obj *ft = ob(&dm.root, "FileTable");
    const char *ps[3] = {P1, P2, P3};
    for (int i = 0; i < 3; i++) {
        char k[16];
        snprintf(k, sizeof k, "File%d", i);
        aamp_set(ft, aamp_crc(k), AAMP_STRREF, ps[i], (uint32_t)strlen(ps[i]));
        aamp_list *df = ls(&dm.root, ps[i]);
        aamp_obj *g = ob(df, "General");
        ip(g, "Life", 99);
        sp(g, "Kind", "linkle");
        ip(ob(df, "Added"), "One", 1);
    }
    size_t dmn;
    uint8_t *dmb = pio_bin(&dm, &dmn);
    put(mod, "logs/deepmerge.aamp", dmb, dmn);
    // aslist.aamp, in a pack in a pack: Wait's file changed, Run added; Player_Linkle's animations; CF define A gets y
    const char *P4 = "01007EF00011E000/romfs/Pack/TitleBG.pack//Actor/Pack/Inner.sbactorpack//Actor/ASList/I.baslist";
    aamp_pio al;
    pio_init(&al);
    aamp_set(ob(&al.root, "FileTable"), aamp_crc("File0"), AAMP_STRREF, P4, (uint32_t)strlen(P4));
    aamp_list *dl = ls(&al.root, P4), *ad = ls(dl, "ASDefines");
    aamp_obj *a0 = ob(ad, "ASDefine_0");
    sp(a0, "Name", "Wait"), sp(a0, "Filename", "Linkle_Wait");
    aamp_obj *a1 = ob(ad, "ASDefine_1");
    sp(a1, "Name", "Run"), sp(a1, "Filename", "Run");
    sp(ob(ls(dl, "AddReses"), "AddRes_0"), "Anim", "Player_Linkle");
    aamp_list *cd = ls(ls(dl, "CFDefines"), "CFDefine_0");
    sp(ob(cd, "CFPre"), "Name", "A");
    sp(ob(cd, "CFExcepts"), "Name_0", "y");
    size_t aln;
    uint8_t *alb = pio_bin(&al, &aln);
    put(mod, "logs/aslist.aamp", alb, aln);
    ap = sarc(&n1, 2, "Actor/ModelList/A.bmodellist", T("linkle model"), "Actor/New/N.bxml", T("new file"));
    apy = yaz(ap, n1, &n2);
    put(mr, "Actor/Pack/Armor_001_Head.sbactorpack", apy, n2);
    in = sarc(&n1, 1, "x.txt", T("mod x"));
    iny = yaz(in, n1, &n3);
    tbg = sarc(&n4, 1, "Actor/Pack/Inner.sbactorpack", iny, n3);
    put(mr, "Pack/TitleBG.pack", tbg, n4);
    puts_(mr, "Model/Armor_001.sbfres", "linkle bfres");
    char ma[700];   // its DLC files: the armour pack (stripped: a new file), a model of its own
    snprintf(ma, sizeof ma, "%s/01007EF00011F000/romfs", mod);
    ap = sarc(&n1, 1, "Actor/New/D.txt", T("dlc new"));
    apy = yaz(ap, n1, &n2);
    put(ma, "Actor/Pack/Aoc_Armor.sbactorpack", apy, n2);
    puts_(ma, "Model/Dlc.sbfres", "dlc bfres");
    snprintf(opt, sizeof opt, "%s/options/Physics Opt/01007EF00011E000/romfs", mod);
    snprintf(ol, sizeof ol, "%s/options/Physics Opt/logs", mod);
    ap = sarc(&n1, 1, "Actor/Physics/A.bphysics", T("option phys"));
    apy = yaz(ap, n1, &n2);
    put(opt, "Actor/Pack/Armor_001_Head.sbactorpack", apy, n2);
    puts_(ol, "packs.json", "{\"Actor/Pack/Armor_001_Head.bactorpack\": \"01007EF00011E000\\\\romfs\\\\Actor\\\\Pack\\\\Armor_001_Head.sbactorpack\"}");
    puts_(ol, "rstb.json", "{\"Model/Armor_001.bfres\": 123456}");

    bnp_t b;
    CHECK(bnp_detect(mod, &b) == 1 && !strcmp(b.name, "The Test Mod") && !strcmp(b.version, "3.0.1") && b.nopt == 1 && !strcmp(b.opt[0].folder, "Physics Opt") &&
          !strcmp(b.opt[0].name, "Better \"Physics\"") && b.opt[0].def == 1 && strstr(b.romfs, "01007EF00011E000/romfs"), "the BCML mod found: its name, version, option, files");
    CHECK(strstr(b.aoc, "01007EF00011F000/romfs") != NULL, "... and its DLC files (%s)", b.aoc);
    char gm[700];
    snprintf(gm, sizeof gm, "%s/game", w);
    bnp_t nb;
    CHECK(bnp_detect(gm, &nb) == 0, "a folder that is not one: not a BCML mod");
    char out[600], err[300] = "";
    snprintf(out, sizeof out, "%s/out", w);
    uint32_t on = 1;
    CHECK(!bnp_build(&b, &on, 1, rd, NULL, rd_aoc, NULL, out, err, sizeof err), "merged (%s)", err);
    char p[1200];
    size_t n;
    snprintf(p, sizeof p, "%s/romfs/Actor/Pack/Armor_001_Head.sbactorpack", out);
    uint8_t *m = slurp(p, &n);
    CHECK(m && n >= 4 && !memcmp(m, "Yaz0", 4), "the actor pack: Yaz0 still");
    CHECK(m && has(m, n, "Actor/ModelList/A.bmodellist", "linkle model"), "... the mod's file over the game's");
    CHECK(m && has(m, n, "Actor/New/N.bxml", "new file"), "... the mod's new file in it");
    CHECK(m && has(m, n, "Actor/Physics/A.bphysics", "option phys"), "... the option's file over both");
    free(m);
    snprintf(p, sizeof p, "%s/romfs/Pack/TitleBG.pack", out);
    m = slurp(p, &n);
    size_t ii;
    uint8_t *inr = m ? inner(m, n, "Actor/Pack/Inner.sbactorpack", &ii) : NULL;
    CHECK(inr && ii >= 4 && !memcmp(inr, "Yaz0", 4) && has(inr, ii, "x.txt", "mod x") && has(inr, ii, "y.txt", "stock y"), "a pack in a pack: merged too (the mod's x, the game's y)");
    free(inr), free(m);
    snprintf(p, sizeof p, "%s/romfs/Model/Armor_001.sbfres", out);
    m = slurp(p, &n);
    CHECK(m && n == 12 && !memcmp(m, "linkle bfres", 12), "a file not in a pack: the mod's");
    free(m);
    snprintf(p, sizeof p, "%s/romfs/%s", out, RS);
    m = slurp(p, &n);
    uint8_t *t2 = NULL;
    size_t t2n = 0;
    CHECK(m && !yaz0_decode(m, n, &t2, &t2n), "the size table written (Yaz0)");
    CHECK(t2 && rstb_get(t2, t2n, "Actor/Pack/Armor_001_Head.bactorpack", &v) && v == 167952, "... the pack's size from rstb.json");
    CHECK(t2 && rstb_get(t2, t2n, "Model/Armor_001.bfres", &v) && v == 123456, "... the option's size over the mod's");
    CHECK(t2 && rstb_get(t2, t2n, "Actor/New/N.bxml", &v) && v == 72, "... a new entry, sized as BCML works it out (8 bytes of .bxml: 72, not the log's 4096)");
    CHECK(t2 && rstb_get(t2, t2n, "Actor/ModelList/A.bmodellist", &v) == 0, "... nothing set for a file whose size cannot be worked out");
    CHECK(t2 && rstb_get(t2, t2n, "Other/Kept.byml", &v) && v == 777, "... the game's others kept, and a logged size below the game's not used");
    CHECK(t2 && !rstb_get(t2, t2n, "Other/Gone.byml", &v), "... a logged size 0 removes the entry");
    CHECK(t2 && rstb_get(t2, t2n, "Actor/Pack/Inner.bactorpack", &v) && v > 0x168 + 0x68, "... the pack in a pack's entry: its size worked out as BCML does (%u)", v);
    free(m), free(t2);
    // the AAMP logs
    aamp_pio d;
    int iv = 0;
    snprintf(p, sizeof p, "%s/romfs/Actor/Pack/Armor_001_Head.sbactorpack", out);
    m = slurp(p, &n);
    CHECK(m && pack_aamp(m, n, "Actor/GeneralParamList/A.bgparamlist", &d) && geti(&d.root, "General", "Life", &iv) && iv == 99 && gets_(&d.root, "General", "Kind", "linkle") &&
          aamp_find_param(aamp_find_obj(&d.root, aamp_crc("General")), aamp_crc("Speed")) && aamp_find_obj(&d.root, aamp_crc("Added")) && aamp_find_list(&d.root, aamp_crc("Sub")),
          "deepmerge: the pack's parameter file changed as logged, the rest kept");
    if (m && iv == 99) aamp_free(&d);
    CHECK(m && has(m, n, "Actor/ModelList/A.bmodellist", "linkle model") && has(m, n, "Actor/Physics/A.bphysics", "option phys"), "... in the pack the mod and its option changed too");
    free(m);
    snprintf(p, sizeof p, "%s/romfs/Actor/Pack/Other.sbactorpack", out);
    m = slurp(p, &n);
    iv = 0;
    CHECK(m && n >= 4 && !memcmp(m, "Yaz0", 4) && pack_aamp(m, n, "Actor/GeneralParamList/O.bgparamlist", &d) && geti(&d.root, "General", "Life", &iv) && iv == 99, "deepmerge: a pack the mod does not have, from the game's");
    if (iv == 99) aamp_free(&d);
    free(m);
    snprintf(p, sizeof p, "%s/romfs/Pack/TitleBG.pack", out);
    m = slurp(p, &n);
    int asok = m && pack_aamp(m, n, "Actor/Pack/Inner.sbactorpack//Actor/ASList/I.baslist", &d);
    const aamp_list *A = asok ? aamp_find_list(&d.root, aamp_crc("ASDefines")) : NULL, *R = asok ? aamp_find_list(&d.root, aamp_crc("AddReses")) : NULL,
                    *C = asok ? aamp_find_list(aamp_find_list(&d.root, aamp_crc("CFDefines")), aamp_crc("CFDefine_0")) : NULL;
    CHECK(A && A->no == 2 && gets_(A, "ASDefine_0", "Name", "Wait") && gets_(A, "ASDefine_0", "Filename", "Linkle_Wait") && gets_(A, "ASDefine_1", "Name", "Run"), "aslist: in a pack in a pack, a define changed and one added");
    CHECK(R && R->no == 2 && gets_(R, "AddRes_0", "Anim", "Player_Link") && gets_(R, "AddRes_1", "Anim", "Player_Linkle"), "aslist: the animations of both");
    CHECK(C && gets_(C, "CFPre", "Name", "A") && gets_(C, "CFExcepts", "Name_0", "x") && gets_(C, "CFExcepts", "Name_1", "y"), "aslist: a CF define's excepts of both");
    CHECK(m && has(m, n, "Actor/Pack/Inner.sbactorpack", NULL), "... the pack in the pack still merged");
    if (asok) aamp_free(&d);
    free(m);
    // the DLC's files
    snprintf(p, sizeof p, "%s/aoc/Actor/Pack/Aoc_Armor.sbactorpack", out);
    m = slurp(p, &n);
    iv = 0;
    CHECK(m && n >= 4 && !memcmp(m, "Yaz0", 4) && has(m, n, "Actor/New/D.txt", "dlc new") && has(m, n, "Actor/ModelList/D.bmodellist", "dlc model"), "DLC: the mod's DLC pack merged with the DLC's own, into aoc/");
    CHECK(m && pack_aamp(m, n, "Actor/Physics/D.bphysics", &d) && geti(&d.root, "General", "Life", &iv) && iv == 99, "DLC: a logged change to a DLC file merged");
    if (iv == 99) aamp_free(&d);
    free(m);
    snprintf(p, sizeof p, "%s/aoc/Model/Dlc.sbfres", out);
    m = slurp(p, &n);
    CHECK(m && n == 9 && !memcmp(m, "dlc bfres", 9), "DLC: a file of the mod's DLC files as it is");
    free(m);
    snprintf(p, sizeof p, "%s/romfs/Actor/Pack/Aoc_Armor.sbactorpack", out);
    CHECK(access(p, F_OK) != 0, "DLC: none of it in the game's own files");
    snprintf(p, sizeof p, "%s/romfs/%s", out, RS);
    m = slurp(p, &n);
    t2 = NULL;
    CHECK(m && !yaz0_decode(m, n, &t2, &t2n) && rstb_get(t2, t2n, "Aoc/0010/Actor/Pack/Aoc_Armor.bactorpack", &v) && v > 16 && rstb_get(t2, t2n, "Aoc/0010/Actor/Physics/D.bphysics", &v) && v > 0,
          "DLC: its sizes in the size table, as Aoc/0010/...");
    free(m), free(t2);
    // no DLC to read: the mod's DLC files left out
    snprintf(out, sizeof out, "%s/out3", w);
    CHECK(!bnp_build(&b, &on, 1, rd, NULL, NULL, NULL, out, err, sizeof err), "merged without DLC");
    snprintf(p, sizeof p, "%s/aoc", out);
    CHECK(access(p, F_OK) != 0, "... no DLC folder made");
    snprintf(out, sizeof out, "%s/out", w);
    // the option off
    snprintf(out, sizeof out, "%s/out2", w);
    on = 0;
    CHECK(!bnp_build(&b, &on, 1, rd, NULL, rd_aoc, NULL, out, err, sizeof err), "merged with the option off");
    snprintf(p, sizeof p, "%s/romfs/Actor/Pack/Armor_001_Head.sbactorpack", out);
    m = slurp(p, &n);
    CHECK(m && has(m, n, "Actor/Physics/A.bphysics", "stock phys") && has(m, n, "Actor/ModelList/A.bmodellist", "linkle model"), "... the game's file where the option's was");
    free(m);
    snprintf(p, sizeof p, "%s/romfs/%s", out, RS);
    m = slurp(p, &n);
    t2 = NULL;
    CHECK(m && !yaz0_decode(m, n, &t2, &t2n) && rstb_get(t2, t2n, "Model/Armor_001.bfres", &v) && v == 99999, "... and the mod's size, not the option's");
    free(m), free(t2);
    printf(fails ? "FAILED: %d\n" : "nxl-tests: passed\n", fails);
    return fails != 0;
}
