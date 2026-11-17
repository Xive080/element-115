#!/bin/bash
cd "$(dirname "$0")"; export P3_GAMEDATA=$(cd ../../gamedata && pwd) P3_TRACE=out/trace.log
lldb -b -o 'process handle SIGABRT -s true -p true' -o run -o 'bt 12' ./out/p3 2>&1 | grep -E "frame #|stop reason|STUB|ASSERT|ERROR" | sed 's#$HOME/bo1-android/native/zdec/src/##' | head -${N:-16}
