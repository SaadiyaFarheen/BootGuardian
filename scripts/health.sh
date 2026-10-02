#!/bin/bash
# BG_SLOT is 0 (A) or 1 (B), set by bgd.
# 1) optional fault simulation for slot A
if [ "$BG_SLOT" = "0" ] && [ -f /tmp/bg_break_A ]; then
    exit 1
fi
# 2) image integrity: the active slot's checksum must match its metadata
"$(dirname "$0")/../cli/bgctl" verify "$BG_SLOT" >/dev/null || exit 1
exit 0
