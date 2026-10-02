#!/bin/bash
# BootGuardian evaluation demo: loads the driver and runs every fault scenario
cd "$(dirname "$0")/.."
make > /dev/null || exit 1
sudo rmmod bootguard 2>/dev/null
sudo insmod kernel/bootguard.ko timeout_sec=3 max_attempts=3 || exit 1
sudo tools/bgfault "$@"
sudo rmmod bootguard
