# CSI Data Collection Pipeline

A end-to-end Channel State Information (CSI) collection and logging system using ESP32, Raspberry Pi 5, and Google Sheets. Built for CSI-based sensing and localization research.

---

## System Architecture

```
Pi 5 AP #1  ──── 802.11 beacon + UDP ────►
                                           ESP32
Pi 5 AP #2  ──── 802.11 beacon + UDP ────►  │
                                             │ collect CSI (200 packets × 64 subcarriers)
                                             │ process: amplitude = sqrt(I² + Q²)
                                             │
                                        switches to mobile hotspot
                                             │
                                             ▼
                                     Google Apps Script
                                             │
                                             ▼
                                       Google Sheets
                              (real + imaginary per subcarrier)
```

The ESP32 cycles between two Pi 5 Access Points, collecting CSI data from each, then switches to a mobile hotspot and pushes the processed data directly to Google Sheets via a Google Apps Script web app.

---

## Repository Structure

```
.
├── esp32/
│   └── main.c                  # ESP32 firmware — CSI collection, processing, upload
├── appscript/
│   └── Code.gs                 # Google Apps Script — receives POST, writes to Sheet
├── raspberry_pi/
│   ├── hostapd.conf            # AP configuration (one per Pi 5)
│   ├── pi_sender.py            # UDP broadcast sender (0xFFFFFFFF at 10 Hz)
│   └── pi_sender.service       # systemd service for auto-start
└── demo/
    └── csi_data_demo.xlsx      # Sample collected CSI data
```

---

## Hardware

| Component | Role |
|---|---|
| Raspberry Pi 5 × 2 | Wi-Fi Access Points + UDP senders |
| ESP32 WROOM-32 | CSI collection, on-device processing, cloud upload |
| Mobile phone | Hotspot for internet access during upload |

---

## How It Works

### Phase 1 — Collect
ESP32 connects to `PI5_AP_1`. The Pi sends UDP broadcasts (`0xFFFFFFFF`) at 10 Hz to port 5000. Every incoming UDP packet triggers the ESP32 hardware CSI engine. The ESP32 collects 200 packets × 64 subcarriers of raw IQ data into RAM.

### Phase 2 — Process
Once 200 packets are collected, ESP32 disconnects from the Pi AP and computes amplitude for each subcarrier:
```
amplitude = sqrt(real² + imag²)
```

### Phase 3 — Upload
ESP32 connects to the mobile hotspot and sends the data to Google Sheets via chunked HTTPS POST requests to a Google Apps Script web app endpoint.

### Phase 4 — Repeat
ESP32 advances to `PI5_AP_2` and repeats the cycle.

---

## Google Sheet Format

Each full cycle writes 12,800 rows (64 subcarriers × 200 packets):

| Subcarrier | Packet | Pi5_AP1_real | Pi5_AP1_imag | Pi5_AP2_real | Pi5_AP2_imag |
|---|---|---|---|---|---|
| 0 | 0 | -98 | -31 | 18 | 7 |
| 0 | 1 | -45 | 12 | 22 | -3 |
| ... | ... | ... | ... | ... | ... |
| 63 | 199 | 11 | -8 | -6 | 14 |

---

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

### 2. Raspberry Pi 5 Setup (repeat on both)

Install dependencies:
```bash
sudo apt update
sudo apt install hostapd dnsmasq -y
```

Stop NetworkManager from managing wlan0:
```bash
sudo nano /etc/NetworkManager/NetworkManager.conf
```
Add:
```ini
[keyfile]
unmanaged-devices=interface-name:wlan0
```

Copy `hostapd.conf` to `/etc/hostapd/hostapd.conf` and edit:
```ini
# Pi 5 #1
ssid=PI5_AP_1
channel=6

# Pi 5 #2
ssid=PI5_AP_2
channel=11
```

Set password (min 8 characters):
```ini
wpa_passphrase=yourpassword
```

Copy `pi_sender.py` to home directory and `pi_sender.service` to `/etc/systemd/system/`.

Update the path in `pi_sender.service` to match your username:
```ini
ExecStart=/usr/bin/python3 /home/YOUR_USERNAME/pi_sender.py
```

Enable all services:
```bash
sudo systemctl enable hostapd dnsmasq pi_sender
sudo systemctl start hostapd dnsmasq pi_sender
```

Verify:
```bash
sudo iw dev wlan0 info      # should show: type AP
ip addr show wlan0           # should show: 192.168.4.1
sudo systemctl status pi_sender  # should show: active (running)
```

---

### 3. ESP32 Firmware

Open `esp32/main.c` and fill in:

```c
#define HOTSPOT_SSID    "YourHotspotName"
#define HOTSPOT_PASS    "YourHotspotPassword"
#define APPS_SCRIPT_URL "https://script.google.com/macros/s/YOUR_ID/exec"

static ap_t pi_aps[] = {
    { "PI5_AP_1", "yourpassword" },
    { "PI5_AP_2", "yourpassword" },
};
```

Enable CSI in sdkconfig:
```
CONFIG_ESP_WIFI_CSI_ENABLED=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
```

Build and flash using ESP-IDF:
```bash
cd esp32
idf.py set-target esp32
idf.py build
idf.py flash monitor
```

---

## Expected Serial Output

```
=== Connecting to PI5_AP_1 ===
Collecting 200 packets...
Packets: 50 / 200
Packets: 100 / 200
Packets: 150 / 200
Packets: 200 / 200
200 packets collected from PI5_AP_1
=== Switching to hotspot ===
Pushing 12800 rows in 50 chunks...
Chunk 1/50 done
...
Chunk 50/50 done
=== Next: PI5_AP_2 ===
```

---

## Pi 5 Configuration Summary

| Property | Pi 5 #1 | Pi 5 #2 |
|---|---|---|
| SSID | PI5_AP_1 | PI5_AP_2 |
| Channel | 6 | 11 |
| IP | 192.168.4.1 | 192.168.4.1 |
| DHCP range | 192.168.4.2–20 | 192.168.4.2–20 |
| UDP payload | 0xFFFFFFFF | 0xFFFFFFFF |
| UDP rate | 10 Hz | 10 Hz |
| UDP port | 5000 | 5000 |

---

## Dependencies

| Tool | Version | Purpose |
|---|---|---|
| ESP-IDF | v5.1.5 | ESP32 firmware framework |
| Python | 3.x | pi_sender.py |
| hostapd | any | Wi-Fi AP management |
| dnsmasq | any | DHCP server for ESP32 |

---

## Troubleshooting

| Problem | Fix |
|---|---|
| hostapd fails — passphrase error | Password must be 8+ characters |
| wlan0 shows 10.x.x.x instead of 192.168.4.1 | NetworkManager still managing wlan0 — add `unmanaged-devices` to NetworkManager.conf |
| pi_sender Errno 101 | wlan0 has no IP — run `sudo ip addr add 192.168.4.1/24 dev wlan0` |
| AP not visible on scan | `sudo iw dev wlan0 info` must show `type AP` not `type managed` |
| rfkill blocking wifi | `sudo rfkill unblock all` |
| Apps Script returns HTML instead of OK | Deployment URL wrong or expired — redeploy and copy new URL |
| ESP32 never collects 200 packets | Pi sender not running or ESP32 not receiving UDP — check pi_sender status |
| Google Sheet empty after upload | Check Apps Script execution logs for errors |