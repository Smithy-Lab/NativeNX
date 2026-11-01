# SPDX-License-Identifier: GPL-3.0-or-later
# nxnative's dm/host/hle_audren.c in nxlinux (tools/gen_thunks.py --patch). The renderer's command headers differ by
# SDK (6, 8-20, 21) and nxnative decides the layout once, from the first command list. A first list of a single
# command walks in any layout (only its magic is checked), so a game whose SDK uses another one (Dead Cells) kept the
# wrong layout: its next list was "malformed" and it had no sound. Here any list the current layout does not walk
# decides it again (and the DeviceSink is looked for again); a list no layout walks is shown in the log (once), so the
# layout can be added.
REPLACE = [
    ('''    if (!layout_done && ncmd) {''',
     '''    if (ncmd && !cmd_walks(cl, ncmd, cend)) {   // nxlinux: decided again by any list the current layout does not walk
        int h6 = g_hdr6, h21 = g_hdr21;
        for (int t = 0; t < 3; t++) {
            g_hdr6 = t == 1, g_hdr21 = t == 2;
            if (cmd_walks(cl, ncmd, cend)) break;
            if (t == 2) g_hdr6 = h6, g_hdr21 = h21;
        }
        if (g_hdr6 != h6 || g_hdr21 != h21) {
            g_sink_id = g_hdr6 ? 0x10 : 0x13, g_sink4 = 0, hist_logged = 0, logged = 0, layout_done = 1;
            LOG("audren: command headers in the %s layout (from a list of %u commands)", g_hdr6 ? "SDK 6" : g_hdr21 ? "SDK 21" : "SDK 8-20", ncmd);
        } else {
            static int dumped;
            if (!dumped++) {
                char hx[3 * 0x80 + 1]; size_t o = 0;
                for (size_t i = 0; i < 0x80 && cl + 0x20 + i < cend; i++) o += snprintf(hx + o, sizeof hx - o, "%02x ", cl[0x20 + i]);
                LOG("[RED] audren: a command list of %u commands no known layout walks; its first bytes: %s", ncmd, hx);
            }
        }
    }
    if (!layout_done && ncmd) {'''),

    # nxlinux: an aligned buffer freed by its own call (Windows' aligned allocations are not free()'s; linux_shim.h)
    ("""            if (posix_memalign(&owned, WORK_PAGE, need))""",
     """            if (nxl_aligned_alloc(&owned, WORK_PAGE, need))"""),
    ("""    if (r) { free(owned);""",
     """    if (r) { nxl_aligned_free(owned);"""),
    ("""    orig(obj, state);
    free(owned);""",
     """    orig(obj, state);
    nxl_aligned_free(owned);"""),
]
