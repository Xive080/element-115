#!/bin/bash
# it.sh 'stub1' 'stub2' ...: anade a safe0, reconstruye y ejecuta
cd "$(dirname "$0")"; for s in "$@"; do echo "$s" >> safe0.txt; done
./build.sh 2>&1 | grep -E "FAIL|falta|error" ; ./run.sh 2>&1 | grep -v "^\[stub0\]" | tail -${N:-10}
