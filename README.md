# CSI Analysis

A Wi-Fi Channel State Information (CSI) collection and analysis pipeline using an ESP32, two Raspberry Pi 5 access points, and Firebase Firestore as the cloud backend.

---

## Table of Contents

- [Overview](#overview)
- [Hardware](#hardware)
- [Repository Structure](#repository-structure)
- [ESP32 Firmware](#esp32-firmware)
- [Firestore Schema](#firestore-schema)
- [Python Pipeline](#python-pipeline)
- [Analysis](#analysis)
- [Troubleshooting](#troubleshooting)

---

## Overview

The ESP32 connects to each Pi AP in turn, collects 50 packets of CSI data per AP (64 subcarriers each), then switches to a mobile hotspot and uploads the results to Firestore. A Python pipeline on the host machine downloads the data, exports it to Excel, and runs per-subcarrier analysis to compare **near** vs **far** measurement conditions.

---

## Hardware

| Component | Role |
|---|---|
| ESP32 | CSI capture and upload |
| Raspberry Pi 5 (×2) | Wi-Fi access points (`PI5_AP_1`, `PI5_AP_2`) |
| Mobile hotspot | Internet uplink for Firestore upload |

---

## Repository Structure

```
├── .devcontainer/            # VS Code dev container configuration
├── .vscode/                  # Editor settings and task definitions
├── build/                    # Compiled binaries and build artifacts (generated)
│
├── main/                     # ESP32 firmware source
│   ├── CMakeLists.txt        # Build configuration for the main component
│   ├── main.c                # Firmware logic (CSI capture, Wi-Fi, Firestore upload)
│   └── secrets.h             # Wi-Fi credentials and Firebase project ID
│
├── Pi_Access_Point/          # Raspberry Pi access point scripts and docs
│   ├── Access_Point_Setup.md # AP configuration instructions
│   └── pi_sender.py          # Responds to UDP pings from the ESP32
│
├── csi_analysis.py           # Post-processing and analysis script
├── firestore_to_excel.py     # Fetches Firestore documents and exports to XLSX
│
├── near.xlsx                 # CSI dataset — near distance
├── far.xlsx                  # CSI dataset — far distance
│
├── CMakeLists.txt            # Top-level ESP-IDF build configuration
├── sdkconfig                 # ESP-IDF project configuration
├── CHANGELOG.md              # Project update history
├── README.md                 # This file
└── Secrets.txt               # Placeholder/backup for sensitive keys
```

---

## ESP32 Firmware

### Collection Flow

```
app_main()
  └── for each Pi AP (PI5_AP_1, PI5_AP_2):
        ├── wifi_connect(PI5_AP_x)
        ├── csi_enable(true)
        ├── spawn udp_task          ← pings Pi every 10 ms to generate frames
        ├── wait until 50 packets collected via csi_cb()
        ├── csi_enable(false) + udp_stop()
        └── wifi_connect(HOTSPOT)
              └── push_to_firebase_async()
                    └── 64 chunks × 50 rows → Firestore REST POST
```

### Configuration

**`main/secrets.h`** — create this file with your credentials:

```c
#define HOTSPOT_SSID     "your_hotspot_ssid"
#define HOTSPOT_PASS     "your_hotspot_password"
#define FIREBASE_PROJECT "csi-esp"
```

**Key constants** defined in `main.c`:

| Define | Value | Description |
|---|---|---|
| `NUM_SUBCARRIERS` | 64 | Subcarriers captured per packet |
| `NUM_PACKETS` | 50 | Packets collected per AP |
| `CHUNK_SIZE` | 50 | Rows per Firestore document |
| `BODY_BUF_SIZE` | 40960 | JSON body buffer size (40 KB) |

### Build & Flash

Requires **ESP-IDF v6.0+**. Add the following to `sdkconfig.defaults`:

```
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
```

Then build and flash:

```bash
idf.py build flash monitor
```

---

## Firestore Schema

**Collection:** `csi`

Each document represents one chunk of data from a single AP:

```json
{
  "ap_index":    0,
  "chunk_index": 12,
  "samples": [
    {
      "subcarrier": 26,
      "packet":     0,
      "real":       16,
      "imag":       8,
      "rssi":       -69,
      "amplitude":  17.8885,
      "angle_rad":  0.46365
    }
  ]
}
```

- **64 documents per AP** (chunks 0–63), **2 APs** → 128 documents total per run.
- Firestore rules must allow public read/write (test mode) or authenticate via a service account key.

---

## Python Pipeline

### Installation

```bash
pip install requests openpyxl pandas numpy matplotlib
```

### Workflow

```bash
# 1. Clear old Firestore data before a new collection run
#    Do this manually via the Firebase Console before proceeding.

# 2. Reset the ESP32 — it collects and uploads automatically

# 3. Download from Firestore and export to Excel
python csi_to_excel.py
# → produces csi_data.xlsx and csi_raw_cache.json

# 4. Rename/copy csi_data.xlsx to near.xlsx or far.xlsx, then run analysis
python analysis.py
```

> **Note:** Re-running `csi_to_excel.py` uses a local cache by default. To force a fresh download from Firestore:
> ```bash
> python csi_to_excel.py --redownload
> ```

### Excel Output Format

File: `csi_data.xlsx` — Sheet: `Sheet1` — **3,200 rows** (64 subcarriers × 50 packets)

Row ordering: subcarrier (outer loop) → packet (inner loop).

| Subcarrier | Packet | Pi5_AP1_real | Pi5_AP1_imag | Pi5_AP1_rssi | Pi5_AP1_amp | Pi5_AP1_angle_rad | Pi5_AP2_real | Pi5_AP2_imag | Pi5_AP2_rssi | Pi5_AP2_amp | Pi5_AP2_angle_rad |
|---|---|---|---|---|---|---|---|---|---|---|---|

---

## Analysis

`analysis.py` loads `near.xlsx` and `far.xlsx`, cleans the data, and performs a per-subcarrier comparison between the two conditions.

### Data Cleaning Steps

1. Drop rows containing any `NaN` values.
2. Remove rows where either AP amplitude ≤ 0 (dead subcarriers).
3. Remove exact duplicate rows (caused by ESP32 retry double-uploads).
4. Filter to valid subcarriers (default: 2–26, configurable).

### Per-Subcarrier Validation

A subcarrier is marked **GOOD** if both conditions are met:

| Criterion | Threshold | Meaning |
|---|---|---|
| `mean(near_amp) − mean(far_amp)` | `> 2` | Near has meaningfully higher amplitude |
| `corr(near_amp, far_amp)` | `< 0.7` | The two conditions are distinguishable |

**Overall result** based on percentage of good carriers:

| Good Carriers | Result |
|---|---|
| > 70% | ✅ STRONG CSI |
| 40–70% | ⚠️ PARTIAL CSI |
| < 40% | ❌ WEAK CSI |

### Valid Subcarrier Ranges

For a 20 MHz 802.11n channel (64-point FFT, ESP32 indexing):

| Range | Description |
|---|---|
| 0 | DC carrier — always zero, skip |
| 1 | Edge carrier — unreliable |
| **2–26** | **Lower band — reliable (default)** |
| 27–37 | Guard band / null carriers — skip |
| **38–62** | **Upper band — reliable** |
| 63 | Edge carrier — unreliable |

---

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---|---|---|
| HTTP 400 from Firestore | Malformed JSON body | Verify `BODY_BUF_SIZE` is set to 40960+ and check closing braces in the JSON template |
| Stack overflow in `main` task | HTTP client exceeds default 3.5 KB stack | Upload runs on a dedicated `firebase_up` task (16 KB); set `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` |
| Near and far amplitudes are identical | Small distance, off-axis position, or multipath-rich environment | Increase separation distance; position a person in the direct line-of-sight path between antennas |
| Missing chunks in Firestore | Upload retry exhausted | Check hotspot signal strength and re-run the collection |
| `csi_raw_cache.json` is stale | Re-running after a new collection without clearing cache | Delete the cache file or run `python csi_to_excel.py --redownload` |