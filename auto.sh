#!/bin/bash
# auto.sh: anade a safe0 el siguiente stub que aborta y repite (max N)
cd "$(dirname "$0")"
for i in $(seq 1 ${1:-6}); do
  s=$(./run.sh 2>&1 | grep -m1 "^\[STUB\]")
  [ -z "$s" ] && { echo "sin mas aborts"; break; }
  echo "$s"; echo "$s" | sed 's/^\[STUB\] //; s/ llamado.*//' >> safe0.txt
  ./build.sh 2>&1 | grep -E "FAIL|error"
done
