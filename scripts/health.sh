#!/bin/bash
# BG_SLOT is 0 (A) or 1 (B), set by bgd. Simulates a broken image in slot A.
if [ "$BG_SLOT" = "0" ] && [ -f /tmp/bg_break_A ]; then
    exit 1
fi
exit 0
