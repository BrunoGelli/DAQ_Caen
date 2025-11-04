# CRS DAQ — Data Acquisition System for CAEN DT5730S

This repository contains the data acquisition and monitoring framework developed for the **CAEN DT5730S digitizer** at ANNIE and 2×2 operations.  
It provides modular utilities to acquire, log, and synchronize waveform and environmental data with optional database integration.

---

## 🧭 Overview

The system is organized around **three key components**:

| Component | Purpose |
|------------|----------|
| **DAQ binaries** (`daq_threshold_v1.0.0`) | Acquire waveforms using software, self, or external triggers; produce structured ROOT files. |
| **Utilities** (`utils/`) | Support scripts for monitoring (temperature, heartbeat), run numbering, and safe retention. |
| **Orchestrator** (`orchestrator/run_everything.sh`) | Automates periodic acquisitions, monitoring, and data synchronization based on `.env` configuration. |

A lightweight `.env` file centralizes configuration so that all subsystems share consistent paths, acquisition parameters, and database endpoints.

---

## 📂 Directory Layout

```
daq/                      # Optional folder for building DAQ executables
daq_threshold_v1.0.0*     # Current DAQ binary (v1.0 / release build)
daq_threshold_v1.0.0.cpp  # C++ source (v28, unified SW/threshold)
data/                     # Local run storage (auto-created, gitignored)
logs/                     # Log files from orchestrator and cron jobs
orchestrator/
 ├── .env                 # Configuration file (not committed)
 └── run_everything.sh    # Master control script
utils/
 ├── heartbeat_influx.sh  # Push success/fail to InfluxDB
 ├── next_run_number.sh   # Atomic run number allocator
 ├── read_temp_influx.cpp # Temperature → Influx utility
 ├── retention_sweeper.sh # Safe deletion of synced runs
 └── ...                  # Other helpers
codeBackup/               # (local only) Historical files; excluded from Git
```

---

## ⚙️ Building the DAQ Code

The main acquisition program is written in C++17 and depends on **ROOT** and **CAEN Digitizer libraries**.

```bash
g++ -O2 -std=c++17 daq_threshold_v1.0.0.cpp -o daq_threshold_v1.0.0     $(root-config --cflags --libs) -lCAENDigitizer
```

After compilation, the executable can be run manually or through the orchestrator.

Example manual run:
```bash
./daq_threshold_v1.0.0 -n 100 -m self -c 0 -t 5 --root data/test_run.root
```

---

## 🧩 The `.env` Configuration

All global settings are stored in `orchestrator/.env`.  
Example:

```bash
# Paths
DATA_DIR="/home/pi/daq/data"
SW_BIN="/home/pi/daq/daq_threshold_v1.0.0"
TH_BIN="/home/pi/daq/daq_threshold_v1.0.0"

# Acquisition defaults
DAQ_MODE=self
DAQ_N_EVENTS=100
DAQ_CHANNEL=0
DAQ_THRESHOLD=5
DAQ_RECORD_LENGTH=1024
DAQ_POST_PERCENT=50
DAQ_LINK=0

# InfluxDB (for heartbeat and monitoring)
INFLUX_URL="http://localhost:8086"
INFLUX_ORG="home"
INFLUX_BUCKET="daq"
INFLUX_TOKEN="***"

# Rsync & retention
RSYNC_DEST="user@server:/srv/daq/archive/runs"
ENABLE_RSYNC=1
ENABLE_RETENTION=1
```

---

## 🚀 Running the System

Use the orchestrator to perform one complete acquisition cycle:

```bash
cd orchestrator
./run_everything.sh
```

It will:
1. Read the digitizer temperature and send to InfluxDB.
2. Assign a new run number.
3. Acquire data in both SW and threshold modes.
4. Record a heartbeat (1=success / 0=failure).
5. Push new data to the remote server.
6. Delete old data already backed up.

To automate, add a cron entry (every 10 minutes):
```bash
*/10 * * * * /home/pi/daq/orchestrator/run_everything.sh >> /home/pi/daq/logs/orchestrator.log 2>&1
```

---

## 📊 Output Files

Each acquisition creates:
- A ROOT file named `run_<6d>_<UTC>_<mode>.root`
- Two TTrees inside:
  - **runinfo**: metadata (N, ch, recLen, threshold, timestamps, board info)
  - **temps**: per-channel temperature at start and end
- One subdirectory per mode or tag containing waveform histograms (`TH1I`).

---

## 🔄 Data Synchronization

Data can be pushed or pulled using `rsync`.  
Server-side pull is preferred for robustness:
```bash
rsync -av --partial --inplace user@daq-host:/home/pi/daq/data/runs/ /srv/daq/archive/runs/
```
Old files already synced are removed by `utils/retention_sweeper.sh` after a safety grace period.

---

## 🧱 Utilities

| Script | Purpose |
|--------|----------|
| `read_temp_influx.cpp` | Reads digitizer temperature and writes to InfluxDB. |
| `heartbeat_influx.sh`  | Reports DAQ run status to InfluxDB. |
| `next_run_number.sh`   | Issues sequential run numbers safely. |
| `retention_sweeper.sh` | Deletes local data only after sync confirmation. |

Each script is self-contained and can be run manually for testing.

---

## 🧩 Versioning

- **v1.0.0 (DAQ v28)** – Unified software/self/external trigger code, temperature logging, and ROOT metadata.
- Tagged releases follow semantic versioning (`vMAJOR.MINOR.PATCH`).
- Legacy containerized versions are archived separately.

---

## 🧠 Development Notes

- All binaries respect `.env` variables as defaults (can be overridden by CLI).
- The orchestrator uses file locks to prevent overlapping runs.
- The repository excludes runtime data and logs for safety.
- Code is C++17 and portable across Debian/Raspberry Pi systems with CAEN SDK ≥ v1.8.

---

## 📄 License

This repository is distributed under the MIT License.  
© 2025 ANNIE Collaboration / UC Davis / Bruno P. Gelli

---

