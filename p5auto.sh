#!/bin/bash
# p5auto.sh: anade a safe0 los stubs que abortan y casan con el patron (ERE) $1; reconstruye y repite (max $2)
cd "$(dirname "$0")"
for i in $(seq 1 ${2:-10}); do
  s=$(./p5run.sh | grep -m1 "^\[STUB\]")
  [ -z "$s" ] && { echo "sin mas aborts"; break; }
  n=$(echo "$s" | sed 's/^\[STUB\] //; s/ llamado.*//')
  if echo "$n" | grep -Eq "$1"; then echo "safe0 += $n"; echo "$n" >> safe0.txt; ./build.sh 2>&1 | grep -E "FAIL|error"; else echo "PARA en: $n"; break; fi
done
