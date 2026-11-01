#!/bin/sh
# stands in for nxlinux in the launcher's loading-screen test: writes the log lines of a start, slowly, then ends
echo "[nxl] loaded main at 0x8000000"
sleep 0.4
echo "[nxl] 100 entry points (5 more from function addresses in data)"
sleep 0.4
echo "[nxl] recompiled 100 functions in 0.10 s"
sleep 0.4
echo "[nxl] nvn: first frame presented"
echo "[nxl] first frame shown in the window (1280x720)"
sleep 0.5
exit 0
