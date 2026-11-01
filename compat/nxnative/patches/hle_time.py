# SPDX-License-Identifier: GPL-3.0-or-later
# nxnative's dm/host/hle_time.c in nxlinux (tools/gen_thunks.py --patch): a local time's zone name and offset through
# linux_shim.h's NXL_TM_ZONE and NXL_TM_GMTOFF (struct tm's tm_zone and tm_gmtoff, which Windows' lacks), and timegm
# through nxl_timegm (Windows has _mkgmtime).
REPLACE = [
    ('''(time zone %s, offset %ld s)", (long long)t, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_zone ? tm.tm_zone : "?", (long)tm.tm_gmtoff);''',
     '''(time zone %s, offset %ld s)", (long long)t, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, NXL_TM_ZONE(tm) ? NXL_TM_ZONE(tm) : "?", (long)NXL_TM_GMTOFF(tm));'''),
    ('''strlcpy(ai->tzname, tm.tm_zone ? tm.tm_zone : "UTC", sizeof ai->tzname);''',
     '''strlcpy(ai->tzname, NXL_TM_ZONE(tm) ? NXL_TM_ZONE(tm) : "UTC", sizeof ai->tzname);'''),
    ('''ai->offset = (int32_t)tm.tm_gmtoff; }''',
     '''ai->offset = (int32_t)NXL_TM_GMTOFF(tm); }'''),
    ('''    return (int64_t)timegm(&tm);''',
     '''    return (int64_t)nxl_timegm(&tm);'''),
]
