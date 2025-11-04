#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
set -a; source "$here/.env"; set +a

# Prevent overlap
lockfile="$here/run_everything.lock"
exec 9>"$lockfile"
flock -n 9 || { echo "[warn] already running"; exit 0; }

# 0) Read temperature (non-fatal)
"${here}/../utils/read_temp_influx/read_temp_influx" || true

# 1) Allocate run number and timestamp
run=$("${here}/../utils/next_run_number.sh")
ts=$(date -u +"%Y-%m-%dT%H-%M-%SZ")

ok=1

# 2) SW trigger run
mode=sw
outfile="${DATA_DIR}/runs/run_${run}_${ts}_${mode}.root"
if ! "${SW_BIN}" -n "${SW_N_EVENTS}" -o "${outfile}" ${SW_EXTRA_ARGS:-}; then
  ok=0
fi
"${here}/../utils/heartbeat_influx.sh" "daq_heartbeat" "$([ $ok -eq 1 ] && echo 1 || echo 0)" "mode=${mode},run=${run}"

# 3) Threshold run
mode=threshold
outfile="${DATA_DIR}/runs/run_${run}_${ts}_${mode}.root"
if ! "${TH_BIN}" -n "${TH_N_EVENTS}" -t "${THRESHOLD}" -o "${outfile}" ${TH_EXTRA_ARGS:-}; then
  ok=0
fi
"${here}/../utils/heartbeat_influx.sh" "daq_heartbeat" "$([ $ok -eq 1 ] && echo 1 || echo 0)" "mode=${mode},run=${run}"

# 4) Push data (best-effort)
if [ "${ENABLE_RSYNC:-1}" = "1" ]; then
  "${here}/../utils/rsync_push.sh" || true
fi

# 5) Retention (best-effort)
if [ "${ENABLE_RETENTION:-1}" = "1" ]; then
  "${here}/../utils/retention_sweeper.sh" || true
fi

exit $([ $ok -eq 1 ] && echo 0 || echo 1)
