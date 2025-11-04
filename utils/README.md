
## 🧰 Operational Setup & Automation

This section explains how to make the entire DAQ + monitoring system run automatically and safely.

---

### 🕒 1. Crontab configuration

Edit the crontab for user `ANNIE`:
```bash
crontab -e
```

Add the following lines:
```cron
# CRS DAQ automated tasks
# -----------------------

# Start temperature monitor loop at boot
@reboot /home/ANNIE/daq/utils/temp_loop.sh >> /home/ANNIE/daq/logs/temp_influx.log 2>&1

# Run DAQ orchestrator every 30 minutes
*/30 * * * * /home/ANNIE/daq/orchestrator/run_everything.sh >> /home/ANNIE/daq/logs/orchestrator.log 2>&1
```

Check that it’s installed:
```bash
crontab -l
```

#### What it does
| Task | Frequency | Purpose |
|------|------------|----------|
| `temp_loop.sh` | runs continuously (launched on boot) | pushes digitizer temperature to InfluxDB every 5 s |
| `run_everything.sh` | every 30 min | acquires waveforms (SW + threshold), pushes data to InfluxDB heartbeat, manages run numbers |

---

### 🌡️ 2. Temperature loop (Option A)

The script `/home/ANNIE/daq/utils/temp_loop.sh` continuously reads temperatures without interfering with DAQ:

```bash
#!/usr/bin/env bash
set -euo pipefail

while true; do
  flock -n /home/ANNIE/daq/.caen.lock /home/ANNIE/daq/utils/read_temp_influx     --influx-host 192.168.197.46     --influx-port 8086     --influx-db AmBeHV     --measurement DT5730S     --once >>/home/ANNIE/daq/logs/temp_influx.log 2>&1 || true

  sleep 5
done
```

Start it manually (if not using `@reboot`):
```bash
screen -dmS temp_loop /home/ANNIE/daq/utils/temp_loop.sh
```

- Uses the same lock `/home/ANNIE/daq/.caen.lock` as the DAQ orchestrator.
- Skips a read if DAQ is running (preventing digitizer lockups).

---

### ⚙️ 3. DAQ orchestrator

Located at:
```
/home/ANNIE/daq/orchestrator/run_everything.sh
```

It performs:
1. Lock acquisition (`.caen.lock`)
2. Software-trigger run → ROOT file
3. Threshold/self-trigger run → ROOT file
4. Heartbeat write to InfluxDB v1 (`measurement=DT5730S`, field `status`)
5. (Future) rsync + retention cleanup

Logs are written to:
```
/home/ANNIE/daq/logs/orchestrator.log
```

---

### 🔁 4. Rsync data transfer (to be added)

**Goal:** copy all new data from the Pi to the central server (and later clean up local storage).

You can use **server-pull** (safer) or **client-push**.

#### Server-pull (recommended)
On the server (e.g., cron on `annie-server`):
```bash
rsync -av --partial --inplace --chmod=F644,D755   ANNIE@192.168.197.50:/home/ANNIE/daq/data/runs/ /srv/daq/archive/runs/
```

#### Client-push (alternative)
In `/home/ANNIE/daq/utils/rsync_push.sh`:
```bash
#!/usr/bin/env bash
set -euo pipefail
rsync -av --partial --inplace --chmod=F644,D755   /home/ANNIE/daq/data/runs/ user@server:/srv/daq/archive/runs/
```

Then add to crontab (after DAQ):
```cron
10 * * * * /home/ANNIE/daq/utils/rsync_push.sh >> /home/ANNIE/daq/logs/rsync.log 2>&1
```

---

### 🧹 5. Data cleanup (to be added)

Use `utils/retention_sweeper.sh` to delete old runs **only if synced**.

Example stub (safe version):
```bash
#!/usr/bin/env bash
set -euo pipefail
LOCAL_DIR="/home/ANNIE/daq/data/runs"
REMOTE="user@server:/srv/daq/archive/runs"

mapfile -t synced < <(rsync -av --dry-run --out-format="%n" "$LOCAL_DIR/" "$REMOTE" | grep '.root$')
for f in "${synced[@]}"; do
  fpath="${LOCAL_DIR}/${f}"
  if [ -f "$fpath" ] && [ "$(date -r "$fpath" +%s)" -lt "$(( $(date +%s) - 86400 ))" ]; then
    rm -f "$fpath"
  fi
done
```

Schedule daily:
```cron
15 3 * * * /home/ANNIE/daq/utils/retention_sweeper.sh >> /home/ANNIE/daq/logs/cleanup.log 2>&1
```

---

### 🔒 6. Locking system

Both DAQ and temperature monitor use:
```
/home/ANNIE/daq/.caen.lock
```

This guarantees **exclusive access** to the CAEN digitizer:
- Orchestrator acquires the lock before running → temp loop pauses.
- Temp loop tries the lock (`-n`), skips if busy → no collision.
- Lock releases automatically when each process exits.

---

### 📊 7. Grafana / InfluxDB monitoring

**InfluxDB v1**
- host: `192.168.197.46`
- port: `8086`
- db: `AmBeHV`
- measurement: `DT5730S`

**Key fields/tags**
| Field | Type | Description |
|--------|------|-------------|
| `status` | int | 1 = successful run, 0 = failure |
| `temp_chX` | float | ADC board temperature per channel |
| `mode` | tag | sw / self |
| `run` | tag | run number |

Example queries (InfluxQL):

```sql
SELECT 100 * mean("status")
FROM "DT5730S"
WHERE $timeFilter
GROUP BY time($__interval), "mode" fill(null);
```

```sql
SELECT last("status")
FROM "DT5730S"
WHERE $timeFilter
GROUP BY "mode";
```

---

### 🧾 8. Quick health checklist

- [ ] `crontab -l` shows both @reboot and 30-min DAQ lines  
- [ ] `/home/ANNIE/daq/logs/` exists and logs are updating  
- [ ] `tail -n 10 logs/temp_influx.log` shows fresh temperature entries  
- [ ] `tail -n 10 logs/orchestrator.log` shows DAQ cycles every 30 min  
- [ ] `lsof /home/ANNIE/daq/.caen.lock` → empty (no stuck locks)  
- [ ] Grafana dashboard displays both temperature and heartbeat data  

---

### 🏁 Future improvements

- [ ] Enable **rsync push/pull** for automatic remote backup  
- [ ] Add **data retention cleanup** script once rsync verified  
- [ ] Optional: integrate heartbeat and temperature graphs on Grafana dashboard  
