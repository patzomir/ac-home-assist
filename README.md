# AC Home Assist

Smart heating control system for home AC units. Tracks real energy consumption, runs a night-setback schedule automatically, and shows what you're spending — so you can spend less.

## What it does

- **Controls your AC via IR** on a schedule you set (e.g. "drop to 18°C at 23:00, warm back up to 22°C by 7:00")
- **Estimates energy cost in BGN** from AC state + outdoor temperature — no CT clamp or electrician needed
- **Dashboard** showing session cost, daily / weekly / monthly totals, and schedule controls

Typical saving: ~3 kWh per night → ~110–120 BGN per heating season.

## Hardware

| Component | Role |
|---|---|
| ESP32-C6 (hub) | WiFi + Zigbee coordinator — connects to backend and controls IR emitters |
| ESP32-H2 + IR LED (emitter) | One per AC unit — fires IR commands, battery-powered (18650), Zigbee end-device |
| Nous A7Z smart plug | Measures real AC power draw; optional but gives actual watt data |

The hub is the only device the customer configures. IR emitters and plugs are pre-paired before shipping.

## Repository layout

```
backend/          Django backend — API, scheduler, energy model, dashboard
firmware/
  zigbee-hub/    ESP-IDF firmware for the ESP32-C6 hub
```

## Quick start

### Backend

Requirements: Python 3.9+

```bash
cd backend
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python manage.py migrate
./run.sh
```

Dashboard: `http://localhost:8765/`

Key environment variables (defaults work for local dev):

| Variable | Default | Description |
|---|---|---|
| `SECRET_KEY` | dev key | Django secret key |
| `DEBUG` | `True` | Set `False` in production |
| `ELECTRICITY_RATE_BGN` | `0.2529` | Your tariff in BGN/kWh |
| `DEFAULT_LATITUDE` | `42.6977` | Your location (for outdoor temp) |
| `DEFAULT_LONGITUDE` | `23.3219` | Your location (for outdoor temp) |

### Firmware (ESP32-C6 hub)

Requirements: [ESP-IDF v5.2+](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/get-started/)

```bash
cd firmware/zigbee-hub

# Source ESP-IDF (if not already done)
. $HOME/esp/esp-idf/export.sh

idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Set the backend URL in `main/http_reporter.c` before flashing:

```
http://<server-ip>:8765/api/events/
```

On first boot the hub starts a `AC-Hub-Setup` WiFi AP. Connect to it, open `http://192.168.4.1`, and enter your home WiFi credentials. The hub saves them and restarts.

See [DEPLOYMENT.md](DEPLOYMENT.md) for the full walkthrough including IR emitter pairing, Nous A7Z pairing, and troubleshooting.

## Architecture overview

```
[Backend server]
      |  WiFi (HTTP)
  [ESP32-C6 hub]  ←── Zigbee ───►  [IR emitter (ESP32-H2)]  ──IR──►  [AC unit]
      |                             [Nous A7Z smart plug]   ──────────► (power data)
   Zigbee
```

Backend does all the intelligence: schedule execution, energy estimation (AC state × outdoor temperature model), cost calculation. The hub and emitters are simple command-execution nodes.

See [architecture.md](architecture.md) for a detailed breakdown.

## Roadmap

- v2: Zigbee temperature sensors for a real feedback loop
- v2: Multi-room / multi-AC support
- v2: Spot-price optimization (IBEX/ENTSO-E API) — pre-heat during cheap hours, avoid peak hours
- Future: Matter bridge on hub (expose to Apple Home / Google Home via WiFi)
