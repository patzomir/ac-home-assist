# Smart Heating System Architecture (ESP32 + BLE + IR)

## Overview

System for optimizing home heating using:
- 1 Hub per home (ESP32 + IR + WiFi)
- 1 Smart Plug per AC unit (Shelly Plug M Gen3 — BLE + WiFi + power monitoring)
- Backend for scheduling and control
- Hub-and-spoke: само хъбът се конфигурира от клиента, plug-ът се pre-pair-ва преди изпращане

---

## 🏠 Architecture

### In-Home Setup

#### 1. Hub (1 per home)
- ESP32
- WiFi connection to backend
- BLE connection to Smart Plug
- IR transmitter (for AC control)

**Responsibilities:**
- Receive power data from plug via BLE, forward to backend
- Receive schedule commands from backend
- Control AC via IR

---

#### 2. Smart Plug (1 per AC unit)
- Shelly Plug M Gen3 (handles up to 13A / 3000W)
- WiFi + BLE dual connectivity
- BLE gateway functionality
- Plugged between AC unit and wall socket
- Measures real-time power draw and kWh
- **Pre-paired с хъба преди изпращане** — клиентът само включва

**Measured data:**
- Watts (real-time)
- kWh (accumulated)

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
- `Hub`
- `SmartPlug`
- `EnergyReading`
- `Schedule`

---

### Relationships

- Home
  - has one Hub
  - has one SmartPlug (per AC unit)

- Hub
  - belongs to Home

- SmartPlug
  - belongs to Home
  - has many EnergyReadings

- Schedule
  - belongs to Home
  - defines time + target AC state (temp, mode)

---

## 🔄 Data Flow

### Smart Plug → Hub
- BLE
- Sends:
  - real-time power (W)
  - energy usage (kWh)

---

### Hub → Backend
- WiFi (HTTP or MQTT)
- Sends:
  - power data (forwarded from plug)
  - device status (heartbeat)

---

### Backend → Hub
- Sends schedule-based commands:
  - target temperature
  - AC state (on/off, mode)

---

### Hub → AC
- IR signal (full state command)
- Example:
  - mode=heat
  - temp=23
  - fan=auto

---

## 🧠 Control Logic

### Schedule-based control (MVP)

Backend fires IR commands at configured times:
```
23:00 → set_ac(temp=18, mode=heat, fan=auto)
06:15 → set_ac(temp=22, mode=heat, fan=auto)
```

No sensor feedback needed — AC internal sensor handles reaching the setpoint.

### Key Principles

- Time-based scheduling is the source of truth
- Avoid constant adjustments
- Periodically re-send full AC state (resync in case of desync)
- Sensor feedback loop is v2

---

## 🔋 Power Strategy

### Hub (ESP32)
- Always powered (USB)

---

## 📡 Communication Choices

| Component | Technology |
|----------|-----------|
| Smart Plug → Hub | BLE |
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

**Goal: Build for yourself first. Prove it works in daily use before selling to anyone.**

### Hardware
- ESP32 hub with IR LED
- 1 Shelly Plug M Gen3 per AC unit
  - BLE + WiFi dual connectivity
  - Bulgarian standard installation uses Schuko wall plug — no rewiring needed
  - Handles up to 13A / 3000W, covers all residential split AC units
  - Pre-paired с хъба преди изпращане → plug and play за клиента

### Software
- Simple backend (Django)
- Basic API
- Minimal UI:
  - real-time power draw (W) and session cost (BGN)
  - daily / weekly / monthly energy cost
  - schedule configuration (night setback)

### What MVP is NOT
- No subscription billing
- No multi-tenant / multi-user support
- No mobile app (web UI only)
- No OTA firmware updates
- No in-app onboarding (pre-configuration се прави ръчно преди изпращане)

Keep it simple. Complexity is only justified after it's working well for yourself.

---

## 🧱 Future Improvements (v2+)

- BLE temperature sensors — feedback loop за реална стайна температура
- Multi-room optimization
- Presence detection
- OTA updates for ESP32
- Native AC integrations (WiFi APIs instead of IR)
- **Ценова оптимизация по свободен пазар**
  - Интеграция с IBEX / ENTSO-E API за почасови спот цени
  - Предзагряване (thermal pre-loading) в евтините часове — апартаментът като топлинна батерия
  - Избягване на пиковите часове (17:00–21:00) без загуба на комфорт
  - Изисква: температурен сензор (feedback loop) + ценови API в бекенда
  - Очакван допълнителен потенциал: +40–150 BGN/сезон спрямо само нощния setback

---

## 🧠 Design Principles

- Hub is the only "smart" device in the home
- Sensors are simple and low-power
- Backend contains the intelligence
- System should work without user intervention
