#!/usr/bin/env bash
# Ablation: two hearthd builds on the same files, interleaved. Used to measure the
# "headers in the same sendfile call" change against the commit before it.
# Usage: bench/scripts/ablation.sh <old-hearthd> <new-hearthd> <hearth-load> <out.jsonl>
set -euo pipefail
OLD=${1:?}; NEW=${2:?}; LOADGEN=${3:?}; OUT=${4:?}; REPS=${REPS:-5}; SECS=${SECS:-5}; W=${WORKERS:-4}
T=$(mktemp -d); trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$T"' EXIT
head -c 1024 /dev/urandom > "$T/1k.bin"
head -c 102400 /dev/urandom > "$T/100k.bin"
"$OLD" -root "$T" -port 18092 -threads "$W" 2>/dev/null &
"$NEW" -root "$T" -port 18090 -threads "$W" 2>/dev/null &
sleep 1
load1() { sysctl -n vm.loadavg 2>/dev/null | awk '{print $2}' || cut -d' ' -f1 /proc/loadavg; }
: > "$OUT"
for file in 1k.bin 100k.bin; do
  for rep in $(seq 1 "$REPS"); do
    for srv in old new; do
      port=18090; [[ $srv == old ]] && port=18092
      l=$(load1)
      "$LOADGEN" 127.0.0.1 "$port" "/$file" 64 "$SECS" | sed "s/^{/{\"server\":\"$srv\",\"rep\":$rep,\"load1\":$l,/" >> "$OUT"
    done
  done
done
