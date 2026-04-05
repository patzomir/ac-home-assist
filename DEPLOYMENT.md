# Deployment Walkthrough

## Overview

The system has two parts to deploy:

1. **Backend** — Django server running on a local machine or server (Raspberry Pi, laptop, homelab)
2. **Firmware** — ESP32-C6 hub flashed and connected to your WiFi

---

## Part 1: Backend

### Requirements

- Python 3.9+
- Git

### Setup

```bash
cd backend

# Create and activate virtual environment
python3 -m venv .venv
source .venv/bin/activate

# Install dependencies
pip install -r requirements.txt

# Run database migrations
python manage.py migrate
```

### Configuration

Set environment variables before starting (or create a `.env` file):

| Variable | Default | Description |
|---|---|---|
| `SECRET_KEY` | dev key | Django secret key — set a strong value in production |
| `DEBUG` | `True` | Set to `False` in production |
| `ALLOWED_HOSTS` | `*` | Comma-separated allowed hostnames |
| `ELECTRICITY_RATE_BGN` | `0.2529` | Your electricity tariff in BGN/kWh |
| `DEFAULT_LATITUDE` | `42.6977` | Your location (for outdoor temperature) |
| `DEFAULT_LONGITUDE` | `23.3219` | Your location (for outdoor temperature) |

### Start the server

```bash
./run.sh
```

The server listens on **port 8765** on all interfaces. The dashboard is accessible at:

```
http://<server-ip>:8765/
```

---

## Part 2: Firmware (ESP32-C6 Hub)

### Requirements

- [ESP-IDF v5.2+](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/get-started/) installed and sourced
- A USB-C cable (data-capable, not charge-only)
- The ESP32-C6 Zero board

### Step 1 — Connect the ESP32-C6 to your computer

The ESP32-C6 Zero has a **USB-C port** on one end. Use it to connect the board to your computer.

```
 [Computer USB-A/C port]
          |
    USB-C cable
          |
 [ESP32-C6 Zero USB-C port]
```

**Important:**
- Use a data cable, not a charge-only cable. If `idf.py flash` cannot find the port, try a different cable.
- On macOS, the device appears as `/dev/cu.usbserial-XXXX` or `/dev/cu.usbmodem-XXXX`.
- On Linux, it appears as `/dev/ttyUSB0` or `/dev/ttyACM0`. You may need to add your user to the `dialout` group: `sudo usermod -aG dialout $USER`.
- On Windows, check Device Manager for the COM port number (e.g., `COM3`).

To find the port after connecting:

```bash
# macOS / Linux
ls /dev/cu.usb* /dev/ttyUSB* /dev/ttyACM* 2>/dev/null

# Or use idf.py to auto-detect
idf.py -p /dev/cu.usbserial-XXXX monitor
```

### Step 2 — Configure WiFi credentials

WiFi credentials are compiled into the firmware. Edit `sdkconfig.defaults` before building:

```
firmware/zigbee-hub/sdkconfig.defaults
```

Find these two lines and fill in your network details:

```
CONFIG_ESP_WIFI_SSID="your-wifi-name"
CONFIG_ESP_WIFI_PASSWORD="your-wifi-password"
```

Only WPA2-PSK networks are supported. The hub does not support open networks or WPA3-only routers.

### Step 3 — Configure the backend URL

The hub reports AC events to `POST /api/events`. Open `firmware/zigbee-hub/main/http_reporter.c` and set the backend URL to point at your server.

Look for the `BACKEND_URL` or endpoint definition and set it to:

```
http://<server-ip>:8765/api/events/
```

### Step 4 — Build and flash

```bash
cd firmware/zigbee-hub

# Source ESP-IDF (if not already in your shell profile)
. $HOME/esp/esp-idf/export.sh

# Set target chip
idf.py set-target esp32c6

# Build
idf.py build

# Flash (replace with your actual port)
idf.py -p /dev/cu.usbserial-XXXX flash

# Open serial monitor to verify boot
idf.py -p /dev/cu.usbserial-XXXX monitor
```

To flash and monitor in a single command:

```bash
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

Press `Ctrl+]` to exit the monitor.

### Step 5 — Verify the hub booted correctly

In the serial monitor you should see the boot sequence in this order:

```
=== AC Home Assist — Zigbee Hub ===
WiFi init done, connecting to 'your-wifi-name'
Got IP: 192.168.x.x
WiFi connected — syncing time
Time synced: 2026-04-04 10:30:00
Zigbee network up — PAN 0x1234 ch 11
Init complete — hub running
```

If you see `WiFi connection failed after 10 attempts`, double-check the SSID and password in `sdkconfig.defaults` and reflash.

If the time sync times out, the scheduler will still run but the night-setback times may be wrong until the clock is synced.

### Step 6 — Unplug from computer and power via USB charger

Once flashed, the hub no longer needs a computer. Plug it into any USB-A or USB-C charger (5V, 500 mA minimum). Place it centrally in your home for best WiFi coverage.

```
[USB Wall Charger 5V]
          |
    USB-C cable
          |
 [ESP32-C6 Zero]   <---WiFi--->  [Router]  <----->  [Backend Server]
          |
      Zigbee
          |
 [IR Emitter near AC unit]
```

---

## Part 3: Pairing an IR Emitter

After the hub boots and forms a Zigbee network, put the IR emitter into pairing mode (hold the button on the emitter board for 3 seconds, or power it on fresh).

In the serial monitor you will see:

```
IR emitter joined: 0x1234
Default schedule applied to 0x1234
```

The emitter is now paired. Point it at the AC unit's IR receiver (usually on the front panel, center or bottom-left). The emitter should be within 3–5 meters with clear line-of-sight to the AC.

---

## Part 4: Pairing a Nous A7Z Smart Plug

The Nous A7Z is a Zigbee smart plug (16A rated) that can join the hub's network and be controlled via on/off commands.

### Join window

The hub opens a **3-minute join window automatically on every boot**. You will see this in the serial monitor:

```
Zigbee network up — PAN 0x1234 ch 11
Network steering done, join window open
```

If the window has already closed, power-cycle the hub to reopen it.

### Pairing steps

1. Plug the Nous A7Z into a wall socket.
2. Hold the button for **5 seconds** until the LED blinks rapidly (fast blue flash means it is scanning for a coordinator).
3. Release — it will join the hub's network automatically within a few seconds.

Confirm in the serial monitor:

```
Device announce: short=0xABCD
IR emitter joined: 0xABCD
Default schedule applied to 0xABCD
Configured attribute reporting for 0xABCD
```

The hub registers Metering and Electrical Measurement cluster clients on its endpoint and immediately sends a Configure Reporting command to the plug after it joins. The A7Z will begin pushing `ActivePower` readings (every 10–60 s, or on a ≥5 W change) and `CurrentSummationDelivered` readings (every 30–300 s, or on a ≥1 Wh change) to the hub. These are forwarded to the backend at `POST /api/plug-metering`.

---

## Part 5: Dashboard

Open a browser and go to:

```
http://<server-ip>:8765/
```

You should see:
- The paired AC unit listed with online status
- Current estimated power consumption
- Session cost and period summaries (today / week / month)
- Night-setback schedule controls

The dashboard polls the backend every 30 seconds automatically.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Flash fails, port not found | Wrong port or charge-only cable | Try a different cable; run `ls /dev/cu.usb*` after plugging in |
| Hub connects to WiFi but backend shows no hub | Wrong backend URL in firmware | Update URL in `http_reporter.c` and reflash |
| Emitter never joins | Hub not in pairing window | Power-cycle the emitter; check monitor for `Zigbee network up` |
| Dashboard shows no data | Backend not migrated | Run `python manage.py migrate` and restart |
| Time sync timed out | NTP blocked by router | Ensure `pool.ntp.org` is reachable on your network |
| AC not responding to commands | IR emitter not aimed correctly | Reposition emitter; test with AC remote to confirm IR receiver location |
| Nous A7Z joined but shows no energy data | Reporting not configured yet | Check monitor for `Configured attribute reporting` — if missing, power-cycle the plug to re-trigger the join |
