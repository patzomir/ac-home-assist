# Smart Heating System Architecture (ESP32-C6 + Zigbee + IR)

## Overview

System for optimizing home heating using:
- 1 Hub per home (ESP32-C6 + WiFi + Zigbee coordinator + BLE)
- 1 Smart Plug per AC unit (Zigbee plug with power monitoring — e.g. Nous A1Z or Sonoff S26R2ZB)
- 1 IR Emitter per AC unit (ESP32-C3 + IR LED, Zigbee end-device)
- Backend for scheduling and control
- Hub-and-spoke: само хъбът се конфигурира от клиента, plug-овете и IR emitter-ите се pre-configure-ват преди изпращане

---

## 🏠 Architecture

### In-Home Setup

#### 1. Hub (1 per home)
- **ESP32-C6** — WiFi 6 + BLE 5 + 802.15.4 (Zigbee coordinator, built-in)
- WiFi connection to backend
- Zigbee coordinator for smart plugs and IR emitters (single mesh)
- BLE for hub provisioning only (customer WiFi setup)

**Responsibilities:**
- Act as Zigbee coordinator — receive power data from plugs, forward to backend
- Receive schedule commands from backend
- Send IR commands to emitters via Zigbee
- Pair Zigbee plugs and IR emitters to coordinator before shipping
- Forward device status (heartbeat) to backend

---

#### 2. Smart Plug (1 per AC unit)
- **Zigbee plug with power monitoring** — Nous A1Z or Sonoff S26R2ZB (~15 EUR)
- EU Schuko — no rewiring needed
- Handles up to 16A / 3600W — covers all residential split AC units
- Measures real-time power draw and kWh
- **Reports to hub via Zigbee mesh** — range is structural, not placement-dependent
- Pre-paired with hub Zigbee coordinator before shipping — клиентът само включва

**Measured data:**
- Watts (real-time)
- kWh (accumulated)

**Why Zigbee over BLE for plugs:**
- Zigbee mesh extends range automatically through other plugs — no range issues through concrete walls
- Open standard — not tied to a single vendor (Shelly)
- Multiple plug vendors available — supply chain resilience
- No dependency on BLE GATT APIs that can change with vendor firmware updates

---

#### 3. IR Emitter (1 per AC unit)
- ESP32-C3 mini (~5–8 BGN) + IR LED + transistor (~1 BGN)
- **Battery-powered (18650 cell)** — no cables at AC unit beyond the smart plug
- Placed directly in front of each AC unit — no line-of-sight constraint with hub
- Zigbee end-device — routes through the co-located smart plug in the same room
- Receives IR commands from hub via Zigbee mesh, fires IR signal at local AC unit

**Power strategy: Zigbee end-device sleep + poll**
- ESP32-C3 runs Zigbee end-device stack with sleepy polling
- Device sleeps between poll intervals; configurable poll rate (e.g. 1 min)
- Hub queues IR command to emitter; emitter receives on next wake, fires IR, returns to sleep
- 18650 cell (3000mAh) → long battery life at low poll duty cycle
- Tradeoff: command latency = poll interval (acceptable for schedule-based control; reduce poll interval for manual override support)

**Why Zigbee over ESP-NOW for IR emitters:**
- Each emitter routes through the Zigbee plug in the same room — range is structural, not placement-dependent
- One protocol for all in-home devices — simpler hub firmware, no ESP-NOW peer management
- Concrete walls between hub and remote AC units are not a problem (same reason plugs use Zigbee)
- ESP-NOW is hub-to-device point-to-point only — no mesh fallback when signal degrades

---

## 🔧 Provisioning Flow

Zigbee plugs and IR emitters are pre-configured before shipping. Only the hub needs to be configured by the customer (WiFi credentials).

```
Factory (before shipping):
1. Hub Zigbee coordinator pairs with each Zigbee plug (standard Zigbee pairing)
2. Hub Zigbee coordinator pairs with each IR emitter (standard Zigbee pairing)

Customer setup (at home):
1. Customer plugs in hub
2. Hub enters setup mode (BLE to phone or WiFi AP)
3. Customer enters home WiFi credentials
4. Hub connects to home WiFi — setup complete
5. Customer plugs in Zigbee plugs — they join the hub's Zigbee network automatically
```

**Key decision:** Zigbee mesh solves the range problem structurally for all in-home devices. Each IR emitter routes through the co-located plug in the same room — no dependency on hub placement or wall attenuation.

---

## ☁️ Backend

### Core Responsibilities
- Store and organize data
- Run control logic
- Send commands to hub
- Receive energy data forwarded from hub

---

### Data Model

#### Entities

- `Home`
- `Hub`
- `SmartPlug`
- `IREmitter`
- `EnergyReading`
- `Schedule`

---

### Relationships

- Home
  - has one Hub
  - has one SmartPlug per AC unit
  - has one IREmitter per AC unit

- Hub
  - belongs to Home
  - coordinates many SmartPlugs (via Zigbee)
  - controls many IREmitters (via ESP-NOW)

- SmartPlug
  - belongs to Home
  - reports via Zigbee → Hub → Backend
  - has many EnergyReadings

- IREmitter
  - belongs to Home
  - controlled by Hub

- Schedule
  - belongs to Home
  - defines time + target AC state (temp, mode)

---

## 🔄 Data Flow

### Smart Plug → Hub
- Zigbee (mesh)
- Sends:
  - real-time power (W)
  - energy usage (kWh)

---

### Hub → Backend
- WiFi (HTTP or MQTT)
- Sends:
  - power data (forwarded from plugs)
  - device status (heartbeat)

---

### Backend → Hub
- Sends schedule-based commands:
  - target temperature
  - AC state (on/off, mode)
  - which IR emitter(s) to target

---

### Hub → IR Emitter
- Zigbee (mesh, routes through co-located plug)
- Sends: full AC state command

---

### IR Emitter → AC
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

### Hub (ESP32-C6)
- Always powered (USB)

### IR Emitter (ESP32-C3)
- Battery-powered (18650 cell)
- Zigbee sleepy end-device — polls coordinator at configurable interval (e.g. 1 min)

---

## 📡 Communication Choices

| Link | Protocol | Notes |
|---|---|---|
| Smart Plug → Hub | Zigbee (mesh) | Range independent of hub placement; survives concrete walls |
| Hub → IR Emitters | Zigbee (mesh) | Routes through co-located plug in same room; same range guarantee |
| Hub → Backend | WiFi (HTTP/MQTT) | Hub placed centrally for good WiFi coverage |
| IR Emitter → AC | IR | Line-of-sight to local AC unit only |

---

## ⚠️ Known Constraints

### IR Limitations
- No feedback from AC (one-way)
- Possible desync if manual remote is used
- IR emitter requires line-of-sight to its local AC unit (not to hub)

### Zigbee Coordinator
- Hub (ESP32-C6) runs Zigbee coordinator stack (Espressif Zigbee SDK)
- Coordinator must be powered and reachable for plugs and emitters to operate
- Zigbee network formed at factory; devices re-join automatically if hub restarts
- IR emitters route through the plug in the same room — each room is self-sufficient in mesh terms
- Minimum 2 plugs needed for mesh routing benefit (single plug = direct link only)

### Mitigations
- Backend maintains AC state
- Periodic state re-sync via IR
- Avoid manual remote usage (or accept drift)

---

## 🚀 MVP Scope

**Goal: Build for yourself first. Prove it works in daily use before selling to anyone.**

### Hardware
- **ESP32-C6** hub — WiFi 6 + Zigbee coordinator + BLE + ESP-NOW, all on one chip
- **1 Zigbee smart plug with power monitoring per AC unit** (Nous A1Z or Sonoff S26R2ZB)
  - Zigbee mesh relay through hub — range not dependent on hub placement
  - EU Schuko — no rewiring needed
  - Handles up to 16A / 3600W, covers all residential split AC units
- **1 IR Emitter (ESP32-C3 + IR LED) per AC unit**
  - Placed directly in front of AC — no hub line-of-sight needed
  - Battery-powered (18650) — no extra cables at the AC unit
  - Zigbee end-device — routes through co-located plug; range not dependent on hub placement
  - Sleepy end-device polling → long battery life

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

- Zigbee temperature sensors — feedback loop за реална стайна температура (reuses existing coordinator on hub)
- Multi-room optimization
- Presence detection
- OTA updates for ESP32 and IR emitters
- Native AC integrations (WiFi APIs instead of IR)
- Zigbee IR delivery confirmation (coordinator ACK already built into Zigbee)
- **Ценова оптимизация по свободен пазар**
  - Интеграция с IBEX / ENTSO-E API за почасови спот цени
  - Предзагряване (thermal pre-loading) в евтините часове — апартаментът като топлинна батерия
  - Избягване на пиковите часове (17:00–21:00) без загуба на комфорт
  - Изисква: температурен сензор (feedback loop) + ценови API в бекенда
  - Очакван допълнителен потенциал: +40–150 BGN/сезон спрямо само нощния setback

---

## 🧠 Design Principles

- Hub is the only "smart" device the customer interacts with
- Sensors and emitters are simple, dumb, always-on nodes
- Backend contains the intelligence
- System should work without user intervention after setup
