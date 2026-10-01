#!/bin/bash
set -e
cd "$(dirname "$0")/.."
sudo rmmod bootguard 2>/dev/null || true
sudo insmod kernel/bootguard.ko timeout_sec=3 max_attempts=3
sudo chmod 666 /dev/bootguard
T="sudo tools/bgtest"
echo "== initial =="; $T status
echo "== 3 failed boots, no heartbeat =="
$T begin; $T begin; $T begin
echo "waiting for watchdog..."; $T wait 5000
$T status
echo "expect: slot=B (rolled back), rollback=1"
$T ack
echo "== healthy boot =="; $T begin; $T confirm; $T hb; $T status
echo "== /proc =="; cat /proc/bootguard
echo "== dmesg =="; sudo dmesg | tail -n 8
sudo rmmod bootguard
echo "DAY 1 TEST DONE"
