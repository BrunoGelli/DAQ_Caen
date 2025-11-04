#!/usr/bin/env bash
set -euo pipefail
mkdir -p "$(dirname "$0")/../state"
runfile="$(dirname "$0")/../state/run_number.txt"
mkdir -p "$(dirname "$runfile")"
touch "$runfile"
exec 9>"$runfile.lock"
flock 9
num=$(cat "$runfile" || echo 0)
num=$((10#$num + 1))
printf "%06d\n" "$num" | tee "$runfile"
