#!/usr/bin/env bash
set -euo pipefail
# Env required: INFLUX_HOST, INFLUX_PORT, INFLUX_DB
# Optional: INFLUX_USER, INFLUX_PASS
# Usage: heartbeat_influx_v1.sh <measurement> <status_int> <tags...>

meas="${1:-daq_heartbeat}"; shift || true
status="${1:-0}"; shift || true
tags="$*"

ts_ns=$(date +%s%N)
line="${meas},host=$(hostname)${tags:+,}${tags} status=${status}i ${ts_ns}"

base="http://${INFLUX_HOST}:${INFLUX_PORT}/write?db=${INFLUX_DB}&precision=ns"
if [[ -n "${INFLUX_USER:-}" && -n "${INFLUX_PASS:-}" ]]; then
  base="${base}&u=${INFLUX_USER}&p=${INFLUX_PASS}"
fi

curl -sS -XPOST "$base" --data-binary "$line" >/dev/null
