# SPDX-License-Identifier: GPL-3.0-or-later
# nxnative's dm/host/hle_fs.c in nxlinux (tools/gen_thunks.py --patch): a DLC's mount gets the mods' DLC files over it
# (DM_AOC_OVERLAY, as DM_ROMFS_OVERLAY for the game's own files: NativeNX's BCML mods, launcher/bnp.c), and the overlay
# folders' separator is the system's.
REPLACE = [
    ('    return mount(name, dir, 1);   // mount_ex logs it\n}',
     '    Result r = mount(name, dir, 1);   // mount_ex logs it\n'
     '    if (r == RES_OK && getenv("DM_AOC_OVERLAY") && getenv("DM_AOC_OVERLAY")[0]) {   // nxlinux: the mods\' DLC files over the DLC\'s\n'
     '        PATH_LOCK();\n'
     '        for (int i = 0; i < g_nmnt; i++) if (!strcmp(g_mnt[i].name, name)) { strlcpy(g_mnt[i].overlay, getenv("DM_AOC_OVERLAY"), sizeof g_mnt[i].overlay); LOG("fs mount %s: the mods\' DLC files first (%s)", name, g_mnt[i].overlay); break; }\n'
     '    }\n'
     '    return r;\n}'),
    # nxlinux: the overlay folders' separator, ';' on Windows (C:\...; linux_shim.h's NXL_LIST_SEP)
    ("""    for (char *root = strtok_r(roots, ":", &save); root; root = strtok_r(NULL, ":", &save)) {""",
     """    for (char *root = strtok_r(roots, NXL_LIST_SEP, &save); root; root = strtok_r(NULL, NXL_LIST_SEP, &save)) {"""),
    ("""    for (char *root = strtok_r(roots, ":", &save); root && nr < 65; root = strtok_r(NULL, ":", &save)) all[nr++] = root;""",
     """    for (char *root = strtok_r(roots, NXL_LIST_SEP, &save); root && nr < 65; root = strtok_r(NULL, NXL_LIST_SEP, &save)) all[nr++] = root;"""),
]
