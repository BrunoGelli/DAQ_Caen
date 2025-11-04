#!/usr/bin/env bash
set -euo pipefail
source "/home/ANNIE/daq/orchestrator/.env"

export INFLUX_HOST INFLUX_PORT INFLUX_DB INFLUX_USER INFLUX_PASS
export DATA_DIR UTILS_DIR SW_BIN TH_BIN

# -------- defaults if .env doesn't define them --------
: "${UTILS_DIR:=/home/ANNIE/daq/utils}"
: "${DATA_DIR:=/home/ANNIE/daq/data}"
: "${SW_BIN:=/home/ANNIE/daq/daq_threshold_v1.0.0}"
: "${TH_BIN:=${SW_BIN}}"

: "${DAQ_CHANNEL:=0}"
: "${DAQ_THRESHOLD:=20}"
: "${DAQ_N_EVENTS:=100}"

# Influx v1 (optional defaults)
: "${INFLUX_HOST:=192.168.197.46}"
: "${INFLUX_PORT:=8086}"
: "${INFLUX_DB:=AmBeHV}"

# -------- single-host lock to avoid CAEN collisions --------
lock="/home/ANNIE/daq/.caen.lock"
exec 9>"$lock"
flock -n 9 || { echo "[warn] DAQ busy (lock)"; exit 0; }

# Ensure output dir exists
mkdir -p "${DATA_DIR}"

run="$("${UTILS_DIR}/next_run_number.sh")"
ts="$(date -u +"%Y-%m-%dT%H-%M-%SZ")"

sw_ok=1
th_ok=1

# --- Software Trigger Run ---
mode="sw"
root_out="${DATA_DIR}/run_${run}_${ts}_${mode}.root"
echo "[run] SW acquisition -> ${root_out}"
"${SW_BIN}" \
  -n "${DAQ_N_EVENTS}" \
  -m sw \
  -c "${DAQ_CHANNEL}" \
  --root "${root_out}" || sw_ok=0

"${UTILS_DIR}/heartbeat_influx.sh" "DT5730S" "${sw_ok}" "mode=sw,run=${run}"

# --- Threshold Trigger Run ---
mode="self"
root_out="${DATA_DIR}/run_${run}_${ts}_${mode}.root"
echo "[run] Threshold acquisition -> ${root_out}"
"${TH_BIN}" \
  -n "${DAQ_N_EVENTS}" \
  -m self \
  -c "${DAQ_CHANNEL}" \
  -t "${DAQ_THRESHOLD}" \
  --root "${root_out}" || th_ok=0

"${UTILS_DIR}/heartbeat_influx.sh" "DT5730S" "${th_ok}" "mode=self,run=${run}"
