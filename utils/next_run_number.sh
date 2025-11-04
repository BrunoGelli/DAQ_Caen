#!/usr/bin/env bash
set -euo pipefail

# State file lives next to this repo (utils/../state)
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
state_dir="${script_dir}/../state"
runfile="${state_dir}/run_number.txt"
lockfile="${state_dir}/run_number.lock"

mkdir -p "${state_dir}"
touch "${runfile}"

# Lock while we read/write the counter
exec 9>"${lockfile}"
flock 9

# Read current number; default to 0 if empty; strip non-digits
cur="$(cat "${runfile}" || true)"
cur="${cur//[^0-9]/}"
if [[ -z "${cur}" ]]; then cur="0"; fi

# Increment safely (base-10), pad to 6 digits
next=$((10#${cur} + 1))
printf "%06d\n" "${next}" | tee "${runfile}"
