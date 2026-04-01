# Smart Heating System Architecture (ESP32 + BLE + IR)

## Overview

System for optimizing home heating using:
- 1 Hub per home
- Multiple wireless temperature sensors
- Backend for logic and control
- Optional IR control for AC units

---

## 🏠 Architecture

### In-Home Setup

#### 1. Hub (1 per home)
- ESP32
- WiFi connection to backend
- BLE scanner for sensors
- IR transmitter (for AC control)

**Responsibilities:**
- Collect sensor data (BLE)
- Send data to backend
- Receive commands from backend
- Control AC via IR

---

#### 2. Sensors (N per home)
- BLE temperature sensors
- Battery-powered (coin cell)
- Broadcast data periodically (no pairing)

**Measured data:**
- Temperature
- (Optional) Humidity

---

## ☁️ Backend

### Core Responsibilities
- Store and organize data
- Run control logic
- Send commands to hub

---

### Data Model

#### Entities

- `Home`
- `Room`
- `Hub`
- `Sensor`
- `Reading`

---

### Relationships

- Home
  - has many Rooms
  - has one Hub

- Room
  - has many Sensors

- Sensor
  - belongs to Room
  - belongs to Home

- Hub
  - belongs to Home

---

## 🔄 Data Flow

### Sensor → Hub
- BLE advertising (broadcast)
- Interval: 30–60 seconds

---

### Hub → Backend
- HTTP or MQTT
- Sends:
  - sensor readings
  - device status

---

### Backend → Hub
- Sends control commands:
  - target temperature
  - AC state

---

### Hub → AC
- IR signal (full state command)
- Example:
  - mode=heat
  - temp=23
  - fan=auto

---

## 🧠 Control Logic

### Temperature Loop

Instead of fixed temperature:
```
if room_temp < 22:
set_ac_temp(25)
elif room_temp > 23:
set_ac_temp(22)
```


---

### Key Principles

- Use external sensor as source of truth
- Avoid constant adjustments
- Use temperature ranges (not fixed point)
- Periodically re-send full AC state (resync)

---

## 🔋 Power Strategy

### Sensors (BLE)
- Ultra low power
- Battery life: ~1–2 years

### Hub (ESP32)
- Always powered (USB)

---

## 📡 Communication Choices

| Component | Technology |
|----------|-----------|
| Sensor → Hub | BLE (Advertising) |
| Hub → Backend | WiFi (HTTP/MQTT) |
| Hub → AC | IR |

---

## ⚠️ Known Constraints

### IR Limitations
- No feedback from AC (one-way)
- Possible desync if remote is used
- Requires line-of-sight

---

### Mitigations
- Backend maintains state
- Periodic state re-sync
- Avoid manual remote usage (or accept drift)

---

## 🚀 MVP Scope

### Hardware
- ESP32 hub with IR LED
- 1–3 BLE temperature sensors

### Software
- Simple backend (Django)
- Basic API
- Minimal UI:
  - current temperature
  - target range

---

## 🧱 Future Improvements

- Custom BLE sensors (instead of off-the-shelf)
- OTA updates for ESP32
- Multi-room optimization
- Presence detection
- Energy usage insights
- Native AC integrations (WiFi APIs)

---

## 🧠 Design Principles

- Hub is the only "smart" device in the home
- Sensors are simple and low-power
- Backend contains the intelligence
- System should work without user intervention
