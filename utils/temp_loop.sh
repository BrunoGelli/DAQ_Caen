#!/usr/bin/env bash
set -euo pipefail

while true; do
  flock -n /home/ANNIE/daq/.caen.lock /home/ANNIE/daq/utils/read_temp_influx \
    --influx-host 192.168.197.46 \
    --influx-port 8086 \
    --influx-db AmBeHV \
    --measurement DT5730S \
    --once >>/home/ANNIE/daq/logs/temp_influx.log 2>&1 || true

  sleep 5
done
