#!/bin/bash
# lldbp5.sh [menu]: ejecuta el marco de menu bajo lldb y muestra la linea y las variables de la parada
cd "$(dirname "$0")"; export P3_GAMEDATA=$(cd ../../gamedata && pwd) P3_TRACE=/tmp/p5trace.log P5_MENU=${1:-popup_restart_warning}
lldb -b -o 'process handle SIGSEGV -s true -p true' -o 'process handle SIGABRT -s true -p true' -o run -o 'bt 8' -o 'frame select 0' ./out/p3 2>&1 | sed -n '/Process .* stopped/,$p' | sed 's#$HOME/bo1-android/native/zdec/src/##' | cut -c1-220 | head -${N:-40}
