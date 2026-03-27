# CSI_Analysis

Wi-Fi Channel State Information (CSI) collection and analysis pipeline using an ESP32, two Raspberry Pi 5 access points, and Firebase Firestore as the cloud backend.

---

## Overview

The ESP32 connects to each Pi AP in turn, collects 50 packets of CSI data per AP (64 subcarriers each), then switches to a mobile hotspot and uploads the data to Firestore. A Python pipeline on the host machine downloads the data, exports it to Excel, and runs per-carrier analysis to compare near vs far measurement conditions.

---

## Hardware

| Component | Role |
|---|---|
| ESP32 | CSI capture + upload |
| Raspberry Pi 5 (×2) | Wi-Fi access points (`PI5_AP_1`, `PI5_AP_2`) |
| Mobile hotspot | Internet uplink for Firestore upload |

---
Repository Structure 

├── .devcontainer/            # VS Code development container configuration
├── .vscode/                  # Editor-specific settings and task definitions
├── build/                    # Compiled binaries and build artifacts (generated)

├── main/                     # Core ESP32 Source Code
│   ├── CMakeLists.txt        # Build configuration for the main component
│   ├── main.c                # Firmware logic (CSI capture, Wi-Fi, Firestore upload)
│   └── secrets.h             # Wi-Fi credentials and Firebase Project ID

├── Pi_Access_Point/          # Scripts and documentation for the RPi targets
│   ├── Access_Point_Setup.md # Instructions for configuring the Pi 5 APs
│   └── pi_sender.py          # Script to receive UDP pings and respond

├── .gitignore                # Files excluded from git tracking
├── CHANGELOG.md              # Record of project updates and versions
├── CMakeLists.txt            # Top-level ESP-IDF build configuration

├── csi_analysis.py           # Post-processing script for CSI data
├── firestore_to_excel.py     # Utility to fetch Firestore documents to XLSX

├── near.xlsx                 # Dataset/Analysis spreadsheet (near distance)
├── far.xlsx                  # Dataset/Analysis spreadsheet (far distance)

├── README.md                 # Project documentation and flow overview
├── sdkconfig                 # Current ESP-IDF project configuration
└── Secrets.txt               # Placeholder or backup for sensitive keys
---

## ESP32 Firmware

### Flow

```
app_main()
  └── for each Pi AP:
        ├── wifi_connect(PI5_AP_x)
        ├── csi_enable(true)
        ├── spawn udp_task          ← pings Pi every 10ms to generate frames
        ├── wait until 50 packets collected via csi_cb()
        ├── csi_enable(false) + udp_stop()
        └── wifi_connect(HOTSPOT)
              └── push_to_firebase_async()   ← 16 KB dedicated FreeRTOS task
                    └── 64 chunks × 50 rows → Firestore REST POST
```

### secrets.h

Create `esp32/secrets.h` with:

```c
#define HOTSPOT_SSID     "your_hotspot_ssid"
#define HOTSPOT_PASS     "your_hotspot_password"
#define FIREBASE_PROJECT "csi-esp"
```

### Key defines

| Define | Value | Description |
|---|---|---|
| `NUM_SUBCARRIERS` | 64 | Subcarriers per packet |
| `NUM_PACKETS` | 50 | Packets collected per AP |
| `CHUNK_SIZE` | 50 | Rows per Firestore document |
| `BODY_BUF_SIZE` | 40960 | JSON body buffer (40 KB) |

### Build

```bash
idf.py build flash monitor
```

Requires ESP-IDF v6.0+. Add to `sdkconfig.defaults`:

```
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
```

---

## Firestore Schema

Collection: `csi`

Each document:
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
    },
    ...
  ]
}
```

- 64 documents per AP (chunks 0–63), 2 APs → 128 documents total per run
- Firestore rules must allow public read/write (test mode) or use a service account key

---

## Python Pipeline

### Install

```bash
pip install requests openpyxl pandas numpy matplotlib
```

### Workflow

```bash
# 1. Clear old data before a new collection run
python clear_firestore.py

# 2. Reset ESP32 — it collects and uploads automatically

# 3. Download from Firestore and export to Excel
python csi_to_excel.py
# → writes csi_data.xlsx + csi_raw_cache.json

# 4. Run analysis (rename/copy csi_data.xlsx to near.xlsx or far.xlsx first)
python analysis.py
```

Re-running `csi_to_excel.py` uses the local cache — pass `--redownload` to force a fresh fetch:

```bash
python csi_to_excel.py --redownload
```

### Excel format

Output file `csi_data.xlsx`, sheet `Sheet1`, 3200 rows (64 subcarriers × 50 packets):

| Subcarrier | Packet | Pi5_AP1_real | Pi5_AP1_imag | Pi5_AP1_rssi | Pi5_AP1_amp | Pi5_AP1_angle_rad | Pi5_AP2_real | Pi5_AP2_imag | Pi5_AP2_rssi | Pi5_AP2_amp | Pi5_AP2_angle_rad |
|---|---|---|---|---|---|---|---|---|---|---|---|

Row ordering: subcarrier outer loop, packet inner loop.

---

## Analysis

`analysis.py` loads `near.xlsx` and `far.xlsx`, cleans the data, and runs per-carrier comparison.

### Cleaning steps

1. Drop rows with any `NaN`
2. Remove rows where either AP amplitude ≤ 0 (dead subcarriers)
3. Remove exact duplicate rows (caused by ESP32 retry double-uploads)
4. Filter to valid subcarriers (default: 2–26, configurable)

### Per-carrier validation rule

A subcarrier is marked **GOOD** if:
- `mean(near_amp) - mean(far_amp) > 2` — near has meaningfully higher amplitude
- `corr(near_amp, far_amp) < 0.7` — the two conditions are distinguishable

Overall result:
- `> 70%` good carriers → ✅ STRONG CSI
- `40–70%` good carriers → ⚠️ PARTIAL CSI
- `< 40%` good carriers → ❌ WEAK CSI

### Valid subcarrier ranges

For a 20 MHz 802.11n channel (64-point FFT, ESP32 indexing):

| Range | Description |
|---|---|
| 0 | DC carrier — always zero, skip |
| 1 | Edge, unreliable |
| 2–26 | Lower band — reliable (default) |
| 27–37 | Guard band / null carriers — skip |
| 38–62 | Upper band — reliable |
| 63 | Edge, unreliable |

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| HTTP 400 from Firestore | Malformed JSON body | Check `BODY_BUF_SIZE` (must be 40960+) and closing braces `]}}}}` |
| Stack overflow in `main` task | HTTP client overflows 3.5 KB default stack | Upload runs on `firebase_up` task (16 KB); set `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` |
| Near and far amplitude identical | Small distance, off-axis position, or multipath-rich environment | Increase distance, place person in direct LOS path between antennas |
| Missing chunks in Firestore | Upload retry exhausted | Check hotspot signal; re-run collection |
| `csi_raw_cache.json` stale | Re-running after new collection | Delete cache or run `python csi_to_excel.py --redownload` |