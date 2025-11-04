#!/usr/bin/env bash
set -euo pipefail
# INFLUX_URL, INFLUX_ORG, INFLUX_BUCKET, INFLUX_TOKEN set in environment
# Usage: heartbeat_influx.sh <measurement> <status> <tags...>
meas="${1:-daq_heartbeat}"; shift || true
status="${1:-0}"; shift || true
tags="$*"

ts_ns=$(date +%s%N)
line="${meas},host=$(hostname)${tags:+,}${tags} status=${status}i ${ts_ns}"
curl -sS -XPOST "$INFLUX_URL/api/v2/write?org=$INFLUX_ORG&bucket=$INFLUX_BUCKET&precision=ns" \
  -H "Authorization: Token $INFLUX_TOKEN" \
  --data-binary "$line" >/dev/null
