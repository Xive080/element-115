#!/bin/bash
# p5run.sh [menu]: ejecuta el marco de menu en Mac (traza) y resume. Variables: P5_MENU
cd "$(dirname "$0")"; export P3_GAMEDATA=$(cd ../../gamedata && pwd) P3_TRACE=${P3_TRACE:-/tmp/p5trace.log} P5_MENU=${1:-${P5_MENU:-popup_restart_warning}}
perl -e 'alarm 250; exec @ARGV' ./out/p3 > /tmp/p5.txt 2> /tmp/p5.err; echo exit=$?
grep -E "^\[STUB\]|^\[ASSERT\]|^\[Com_Error|^\[Sys_Error|^\[SIGNAL|^\[P5\]|^\[ERROR\]" /tmp/p5.err | grep -v menulist | head -${N:-12}; grep -A8 "^\[SIGNAL" /tmp/p5.err | head -12
