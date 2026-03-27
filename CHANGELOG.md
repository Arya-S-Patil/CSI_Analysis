# Changelog

All notable changes to this project will be documented in this file.

---
## v0.2.0
 
### Changed — Backend: Google Apps Script → Firebase Firestore
 
The entire upload backend has been replaced. Google Apps Script was slow (cold starts, 30s execution limits) and caused frequent HTTP timeouts on the ESP32. Firebase Firestore REST API is fast, has no execution time limits, and requires no redirect chains.
 
#### ESP32 firmware (`main.c`)
 
**Removed**
- `#include "esp_spiffs.h"` — no local file storage needed
- `APPS_SCRIPT_URL` define — Apps Script endpoint removed
- `push_chunk()` — flat JSON POST to Apps Script
- `push_all_chunks()` — wrapper that called `push_chunk()`
 
**Added**
- `FIREBASE_PROJECT` — project ID in `secrets.h` (replaces Apps Script URL)
- `firebase_insert_chunk()` — POSTs a single Firestore document via REST API
  - Uses Firestore typed JSON format (`integerValue`, `doubleValue`, `arrayValue`, `mapValue`)
  - URL: `firestore.googleapis.com/v1/projects/{PROJECT}/databases/(default)/documents/csi`
  - No API key or Authorization header needed in test mode
- `firebase_insert_chunk_with_retry()` — 3 attempts with exponential back-off (1s, 2s, 3s)
- `push_to_firebase()` — replaces `push_all_chunks()`
- `push_to_firebase_async()` — spawns dedicated `firebase_up` FreeRTOS task (16 KB stack) and blocks `app_main` on a semaphore; fixes stack overflow caused by running HTTP client on the 3.5 KB main task
- Response body logging on non-200 status for easier debugging
 
**Fixed**
- `BODY_BUF_SIZE` increased from `24576` to `40960` (40 KB) — previous size caused buffer overflow on chunks with large float values, producing truncated JSON and HTTP 400 errors
- Buffer guard threshold raised from 300 to 400 bytes to prevent partial row writes
- Closing JSON delimiter corrected from `"]}}}}}"` (6 closers) to `"]}}}}"`  (5 closers) — extra brace caused HTTP 400 on every chunk
- `CONFIG_ESP_MAIN_TASK_STACK_SIZE` set to `8192` in `sdkconfig.defaults`
 
**Changed**
- `TAG` renamed from `CSI_SHEETS` to `CSI_FIREBASE`
 
#### Python pipeline
 
**Removed**
- `firebase-admin` dependency — replaced by plain `requests` (Firestore REST API, no auth needed in test mode)
- `check_csi_data.py` — superseded by simpler scripts
 
**Added**
- `csi_to_excel.py` — downloads all Firestore documents via REST, parses typed JSON, reconstructs per-AP arrays, exports `csi_data.xlsx` in the established format; caches raw JSON locally to avoid repeat downloads
- `clear_firestore.py` — deletes all documents in the `csi` collection via REST DELETE; used to wipe data before a new collection run
- `--redownload` flag on `csi_to_excel.py` to force fresh fetch ignoring cache
 
#### Firestore document schema
 
Each document in the `csi` collection:
 
```
ap_index      : integer   (0 or 1)
chunk_index   : integer   (0–63)
samples       : array of maps
  subcarrier  : integer
  packet      : integer
  real        : integer
  imag        : integer
  rssi        : integer   (dBm)
  amplitude   : double
  angle_rad   : double
```
 
---
 
## [v0.1.0] - Initial CSI Pipeline
## Setup

### 1. Google Apps Script

1. Create a new Google Sheet and copy its Spreadsheet ID from the URL
2. Open **Extensions → Apps Script**
3. Paste the contents of `appscript/Code.gs`
4. Replace `PASTE_YOUR_ID_HERE` with your Spreadsheet ID
5. Click **Deploy → New deployment → Web App**
   - Execute as: Me
   - Who has access: Anyone
6. Copy the deployment URL

Test the endpoint:
```bash
curl -L -X POST "YOUR_DEPLOYMENT_URL" \
  -H "Content-Type: application/json" \
  -d '{"ap_index":0,"samples":[{"subcarrier":0,"packet":0,"real":-98,"imag":-31}]}'
```
Expected response: `OK`

---
### Added
- ESP32-based CSI data collection
- Sequential connection to multiple Access Points (AP1/AP2)
- HTTP POST data transmission
- Integration with Google Apps Script backend
- Storage of CSI data in Google Sheets
- Chunking of CSI packets per carrier

### Notes
- Proof-of-concept implementation
- Suitable for small-scale testing and validation
- To be changed to a suitable database in next release
