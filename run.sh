#!/bin/bash
# run.sh: ejecuta p3 (traza en out/trace.log). Variables: P3_TRACE_IDX=1 (indices), H0_VERBOSE=1 (Com_Printf)
cd "$(dirname "$0")"; export P3_GAMEDATA=${P3_GAMEDATA:-$(cd ../../gamedata && pwd)}; export P3_TRACE=${P3_TRACE:-out/trace.log}
perl -e 'alarm 300; exec @ARGV' ./out/p3 "$@"
