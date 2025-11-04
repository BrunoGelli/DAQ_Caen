#!/usr/bin/env bash
set -euo pipefail
LOCAL_DIR="$(dirname "$0")/../data/runs"
REMOTE="${RSYNC_DEST:?set RSYNC_DEST}"
# list local files that are already present at remote
mapfile -t present < <(rsync -av --dry-run --out-format="%n" "$LOCAL_DIR/" "$REMOTE" \
  | grep -F '.root' | sed 's#^#'"$LOCAL_DIR/"'#')
# delete only those (and older than a grace period)
for f in "${present[@]}"; do
  # keep a 24h cushion
  if [ "$(date -r "$f" +%s)" -lt "$(( $(date +%s) - 24*3600 ))" ]; then
    rm -f -- "$f"
  fi
done
