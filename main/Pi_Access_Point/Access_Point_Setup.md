# Pi 5 Access Point Deployment Guide

## Overview

This guide sets up both Pi 5 units as Wi-Fi Access Points that broadcast
UDP packets to trigger CSI extraction on the ESP32.

```
Pi 5 #1  →  SSID: PI5_AP_1  →  channel 6   →  192.168.4.1
Pi 5 #2  →  SSID: PI5_AP_2  →  channel 11  →  192.168.4.1
```

Both Pi 5s run independently on their own isolated networks.
The ESP32 connects to one at a time.

---

## Prerequisites

- Raspberry Pi 5 (x2)
- Raspberry Pi OS (64-bit) installed
- Monitor + keyboard connected to each Pi
- Internet access via ethernet (eth0) for initial setup

---

## Step 1 — Install Required Packages

Run on **both** Pi 5s:

```bash
sudo apt update
sudo apt install hostapd dnsmasq -y
sudo systemctl stop hostapd
sudo systemctl stop dnsmasq
```

---

## Step 2 — Stop NetworkManager from Managing wlan0

Run on **both** Pi 5s:

```bash
sudo nano /etc/NetworkManager/NetworkManager.conf
```

Add at the bottom:

```ini
[keyfile]
unmanaged-devices=interface-name:wlan0
```

Save with `Ctrl+X → Y → Enter`, then:

```bash
sudo systemctl restart NetworkManager
```

---

## Step 3 — Configure hostapd

### Pi 5 #1

```bash
sudo nano /etc/hostapd/hostapd.conf
```

Paste:

```ini
interface=wlan0
driver=nl80211
ssid=PI5_AP_1
hw_mode=g
channel=6
wmm_enabled=0
macaddr_acl=0
auth_algs=1
ignore_broadcast_ssid=0
beacon_int=100
vendor_elements=dd07AABBCCFFFFFFFF

wpa=2
wpa_passphrase=yourpassword
wpa_key_mgmt=WPA-PSK
wpa_pairwise=TKIP
rsn_pairwise=CCMP
```

### Pi 5 #2

```bash
sudo nano /etc/hostapd/hostapd.conf
```

Paste:

```ini
interface=wlan0
driver=nl80211
ssid=PI5_AP_2
hw_mode=g
channel=11
wmm_enabled=0
macaddr_acl=0
auth_algs=1
ignore_broadcast_ssid=0
beacon_int=100
vendor_elements=dd07AABBCCFFFFFFFF

wpa=2
wpa_passphrase=yourpassword
wpa_key_mgmt=WPA-PSK
wpa_pairwise=TKIP
rsn_pairwise=CCMP
```

> Password must be at least 8 characters.

---

## Step 4 — Set Static IP on wlan0

Create a systemd service that assigns the static IP before hostapd starts.

Run on **both** Pi 5s:

```bash
sudo nano /etc/systemd/system/wlan0-static.service
```

Paste:

```ini
[Unit]
Description=Set wlan0 static IP
After=network.target
Before=hostapd.service pi_sender.service

[Service]
Type=oneshot
ExecStart=/sbin/ip link set wlan0 up
ExecStart=/sbin/ip addr add 192.168.4.1/24 dev wlan0
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
```

Enable it:

```bash
sudo systemctl enable wlan0-static
sudo systemctl start wlan0-static
```

---

## Step 5 — Configure dnsmasq (DHCP for ESP32)

Run on **both** Pi 5s:

```bash
sudo mv /etc/dnsmasq.conf /etc/dnsmasq.conf.backup
sudo nano /etc/dnsmasq.conf
```

Paste:

```ini
interface=wlan0
dhcp-range=192.168.4.2,192.168.4.20,255.255.255.0,24h
dhcp-option=3,192.168.4.1
dhcp-option=6,8.8.8.8
```

---

## Step 6 — Enable and Start Services

Run on **both** Pi 5s:

```bash
sudo systemctl unmask hostapd
sudo systemctl enable hostapd
sudo systemctl enable dnsmasq
sudo systemctl start hostapd
sudo systemctl start dnsmasq
```

---

## Step 7 — Create pi_sender.py

### Pi 5 #1

```bash
nano /home/iitr/pi_sender.py
```

### Pi 5 #2

```bash
nano /home/pi/pi_sender.py
```

Paste on **both**:

```python
import socket
import time

PORT    = 5000
PAYLOAD = bytes([0xFF, 0xFF, 0xFF, 0xFF])

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)

print("Sending UDP broadcast on port", PORT)
while True:
    try:
        sock.sendto(PAYLOAD, ("255.255.255.255", PORT))
    except Exception as ex:
        print("Error:", ex)
    time.sleep(0.1)
```

---

## Step 8 — Create pi_sender systemd Service

### Pi 5 #1

```bash
sudo nano /etc/systemd/system/pi_sender.service
```

Paste:

```ini
[Unit]
Description=CSI UDP Sender
After=hostapd.service wlan0-static.service
Wants=hostapd.service

[Service]
ExecStart=/usr/bin/python3 /home/iitr/pi_sender.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

### Pi 5 #2

```bash
sudo nano /etc/systemd/system/pi_sender.service
```

Paste:

```ini
[Unit]
Description=CSI UDP Sender
After=hostapd.service wlan0-static.service
Wants=hostapd.service

[Service]
ExecStart=/usr/bin/python3 /home/pi/pi_sender.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Enable on **both**:

```bash
sudo systemctl enable pi_sender
sudo systemctl start pi_sender
```

---

## Step 9 — Reboot and Verify

```bash
sudo reboot
```

After reboot run on **both** Pi 5s:

```bash
# Check AP is running
sudo systemctl status hostapd

# Check wlan0 is in AP mode
sudo iw dev wlan0 info

# Check static IP is assigned
ip addr show wlan0

# Check UDP sender is running
sudo systemctl status pi_sender
```

### Expected output

```
hostapd:   Active: active (running)
iw info:   type AP
wlan0:     inet 192.168.4.1/24
pi_sender: Active: active (running)
```

---

## Step 10 — Verify AP is Visible

From your laptop or phone, scan Wi-Fi networks.
You should see:

```
PI5_AP_1
PI5_AP_2
```

Connect to `PI5_AP_1` with your password, then ping:

```bash
ping 192.168.4.1
```

If you get replies — AP is fully working.

---

## SSH into Pi while AP is running

Since wlan0 is the AP, SSH via the AP IP:

```bash
ssh iitr@192.168.4.1   # Pi 5 #1 (connect laptop to PI5_AP_1 first)
ssh pi@192.168.4.1     # Pi 5 #2 (connect laptop to PI5_AP_2 first)
```

Or via ethernet if connected:

```bash
ssh iitr@<eth0-ip>
ssh pi@<eth0-ip>
```

---

## Service Start Order on Boot

```
wlan0-static  →  sets 192.168.4.1 on wlan0
     ↓
hostapd       →  starts AP, begins broadcasting SSID
     ↓
dnsmasq       →  starts DHCP server for ESP32
     ↓
pi_sender     →  starts UDP broadcast on port 5000
```

---

## Summary Table

| Property       | Pi 5 #1       | Pi 5 #2       |
|----------------|---------------|---------------|
| Username       | iitr          | pi            |
| SSID           | PI5_AP_1      | PI5_AP_2      |
| Channel        | 6             | 11            |
| AP IP          | 192.168.4.1   | 192.168.4.1   |
| DHCP range     | 192.168.4.2–20| 192.168.4.2–20|
| UDP payload    | 0xFFFFFFFF    | 0xFFFFFFFF    |
| UDP rate       | 10 Hz         | 10 Hz         |
| UDP port       | 5000          | 5000          |
| pi_sender path | /home/iitr/   | /home/pi/     |

---

## Troubleshooting

| Problem | Fix |
|---|---|
| hostapd fails with passphrase error | Password must be 8+ characters |
| wlan0 shows wrong IP (10.x.x.x) | NetworkManager still managing wlan0 — redo Step 2 |
| pi_sender Errno 101 | wlan0 has no IP — run `sudo ip addr add 192.168.4.1/24 dev wlan0` |
| AP not visible on scan | Run `sudo iw dev wlan0 info` — must show `type AP` not `type managed` |
| rfkill blocking wifi | Run `sudo rfkill unblock all` |
| hostapd not found after reboot | Run `sudo systemctl unmask hostapd && sudo systemctl enable hostapd` |