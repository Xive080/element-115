#!/bin/bash
# H4 en Mac con el backend TRACE (sin render): para cazar crashes del camino 3D. Uso: harness2/h4mac.sh
cd "$(dirname "$0")"; export P3_GAMEDATA=$(cd ../../gamedata && pwd) P3_TRACE=/tmp/h4trace.log P4_MAP=${P4_MAP:-zombie_pentagon} P3_BACKEND=trace
./out/p3 > /tmp/h4o.txt 2>&1; echo "exit=$?"; grep -E "^\[H4\]|STUB|ASSERT|ail" /tmp/h4o.txt | tail -${N:-8}; grep -A9 "STUB\]" /tmp/h4o.txt | head -12
