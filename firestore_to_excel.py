"""
Download CSI data from Firestore → Excel
Install: pip install requests openpyxl
Run:     python csi_to_excel.py
"""

import requests, json, math, os
from openpyxl import Workbook

PROJECT_ID      = "csi-esp"
COLLECTION      = "csi"
NUM_SUBCARRIERS = 64
NUM_PACKETS     = 50
CACHE_FILE      = "csi_raw_cache.json"
#you can rename the file name from below
OUTPUT_XLSX     = "near.xlsx"

BASE_URL = (
    f"https://firestore.googleapis.com/v1/"
    f"projects/{PROJECT_ID}/databases/(default)/documents/{COLLECTION}"
)

def fetch_all_docs():
    print("Downloading from Firestore...")
    docs, params = [], {"pageSize": 300}
    while True:
        resp = requests.get(BASE_URL, params=params, timeout=30)
        if resp.status_code != 200:
            raise RuntimeError(f"HTTP {resp.status_code}: {resp.text[:200]}")
        body     = resp.json()
        docs    += body.get("documents", [])
        token    = body.get("nextPageToken")
        print(f"  {len(docs)} docs so far...")
        if not token:
            break
        params["pageToken"] = token
    print(f"  Done — {len(docs)} documents")
    return docs

def parse_value(v):
    if "integerValue" in v: return int(v["integerValue"])
    if "doubleValue"  in v: return float(v["doubleValue"])
    if "stringValue"  in v: return v["stringValue"]
    if "arrayValue"   in v: return [parse_value(i) for i in v["arrayValue"].get("values", [])]
    if "mapValue"     in v: return {k: parse_value(fv) for k, fv in v["mapValue"]["fields"].items()}
    return None

def load_docs():
    if os.path.exists(CACHE_FILE):
        print(f"Loading from cache ({CACHE_FILE})...")
        with open(CACHE_FILE) as f:
            return json.load(f)
    raw = fetch_all_docs()
    parsed = [{k: parse_value(v) for k, v in d["fields"].items()} for d in raw]
    with open(CACHE_FILE, "w") as f:
        json.dump(parsed, f)
    print(f"Cached to {CACHE_FILE}")
    return parsed

def reconstruct(docs, ap_idx):
    real      = [[0]   * NUM_SUBCARRIERS for _ in range(NUM_PACKETS)]
    imag      = [[0]   * NUM_SUBCARRIERS for _ in range(NUM_PACKETS)]
    rssi      = [0]    * NUM_PACKETS
    amplitude = [[0.0] * NUM_SUBCARRIERS for _ in range(NUM_PACKETS)]
    angle_rad = [[0.0] * NUM_SUBCARRIERS for _ in range(NUM_PACKETS)]

    for doc in docs:
        if doc.get("ap_index") != ap_idx:
            continue
        for s in doc.get("samples", []):
            sc, pkt = s["subcarrier"], s["packet"]
            if 0 <= sc < NUM_SUBCARRIERS and 0 <= pkt < NUM_PACKETS:
                real[pkt][sc]      = s["real"]
                imag[pkt][sc]      = s["imag"]
                rssi[pkt]          = s["rssi"]
                amplitude[pkt][sc] = s["amplitude"]
                angle_rad[pkt][sc] = s["angle_rad"]

    return {"real": real, "imag": imag, "rssi": rssi,
            "amplitude": amplitude, "angle_rad": angle_rad}

def build_excel(ap1, ap2):
    print(f"Building {OUTPUT_XLSX}...")
    wb = Workbook()
    ws = wb.active
    ws.title = "Sheet1"
    ws.append([
        "Subcarrier", "Packet",
        "Pi5_AP1_real", "Pi5_AP1_imag", "Pi5_AP1_rssi", "Pi5_AP1_amp", "Pi5_AP1_angle_rad",
        "Pi5_AP2_real", "Pi5_AP2_imag", "Pi5_AP2_rssi", "Pi5_AP2_amp", "Pi5_AP2_angle_rad",
    ])
    for sc in range(NUM_SUBCARRIERS):
        for pkt in range(NUM_PACKETS):
            ws.append([
                sc, pkt,
                ap1["real"][pkt][sc],      ap1["imag"][pkt][sc],
                ap1["rssi"][pkt],          ap1["amplitude"][pkt][sc], ap1["angle_rad"][pkt][sc],
                ap2["real"][pkt][sc],      ap2["imag"][pkt][sc],
                ap2["rssi"][pkt],          ap2["amplitude"][pkt][sc], ap2["angle_rad"][pkt][sc],
            ])
    wb.save(OUTPUT_XLSX)
    print(f"Done — {OUTPUT_XLSX} ({ws.max_row - 1} rows)")

if __name__ == "__main__":
    docs = load_docs()
    ap1  = reconstruct(docs, 0)
    ap2  = reconstruct(docs, 1)
    build_excel(ap1, ap2)