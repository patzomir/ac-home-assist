# Smart Heating System Architecture (ESP32-C6 + Zigbee + IR)

## Overview

System for optimizing home heating using:
- 1 Hub per home (ESP32-C6 + WiFi + Zigbee coordinator + BLE)
- 1 IR Emitter per AC unit (ESP32-H2 + IR LED, Zigbee end-device)
- Backend for scheduling, control, and energy estimation
- Hub-and-spoke: само хъбът се конфигурира от клиента, IR emitter-ите се pre-configure-ват преди изпращане

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

#### 2. Energy Estimation (per AC unit)
- No additional hardware — runs entirely in the backend
- Inputs: AC state from IR commands (mode, set temperature) + outdoor temperature (weather API)
- Inverter AC consumption is predictable from nameplate figures and operating conditions
- Backend maintains AC state and computes estimated kWh per session

**Estimated data:**
- Watts (modeled from AC state + outdoor temp)
- kWh (accumulated from model)
- Session cost in BGN (kWh × current electricity tariff)

**Why estimation over CT clamp for MVP:**
- CT clamps must clamp a single wire — standard AC cables carry L + N together, fields cancel, reads zero
- Getting to separated wires requires opening socket box or AC terminal — not viable for self-install at scale
- Estimation answers "did I save money tonight" just as well for behavior change purposes

---

#### 3. IR Emitter (1 per AC unit)
- ESP32-H2 mini (~5–10 BGN) + IR LED + transistor (~1 BGN)
- **Battery-powered (18650 cell)** — no cables at AC unit
- Placed directly in front of each AC unit — no line-of-sight constraint with hub
- Zigbee end-device — routes directly to hub (MVP: 1 AC); for multi-AC, Zigbee range through concrete walls may require a router node (v2 consideration)
- Receives IR commands from hub via Zigbee mesh, fires IR signal at local AC unit

**Power strategy: Zigbee end-device sleep + poll**
- ESP32-H2 runs Zigbee end-device stack with sleepy polling
- Device sleeps between poll intervals; configurable poll rate (e.g. 1 min)
- Hub queues IR command to emitter; emitter receives on next wake, fires IR, returns to sleep
- 18650 cell (3000mAh) → long battery life at low poll duty cycle
- Tradeoff: command latency = poll interval (acceptable for schedule-based control; reduce poll interval for manual override support)

**Why custom build over commercial Zigbee IR blasters (ZS06, UFO-R11):**
- Commercial blasters are code-learning/replay devices — they store codes and fire them by index
- This system sends structured AC state commands (`temp`, `mode`, `fan`) that must be translated to the correct IR signal per AC model
- That translation requires native AC protocol support (e.g. ESPHome `climate_ir`) — not available in commercial blasters
- ZS06 is USB-powered (breaks the no-cables-at-AC-unit constraint); UFO-R11 uses AAA batteries (worse capacity than 18650)
- Neither runs a Zigbee end-device stack that accepts structured commands — they expose a "send stored code N" interface only

**Why Zigbee over ESP-NOW for IR emitters:**
- One protocol for all in-home devices — simpler hub firmware, no ESP-NOW peer management
- ESP-NOW is hub-to-device point-to-point only — no mesh fallback when signal degrades
- Concrete walls may require a Zigbee router node in multi-AC setups (v2 consideration)

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
- `IREmitter`
- `EnergyEstimate`
- `Schedule`

---

### Relationships

- Home
  - has one Hub
  - has one SmartPlug per AC unit
  - has one IREmitter per AC unit

- Hub
  - belongs to Home
  - controls many IREmitters (via Zigbee)

- IREmitter
  - belongs to Home
  - controlled by Hub

- Schedule
  - belongs to Home
  - defines time + target AC state (temp, mode)

---

## 🔄 Data Flow

### Backend Energy Estimation
- No hardware data — runs entirely in the backend
- Inputs: AC state log (from IR commands) + outdoor temperature (weather API)
- Outputs: estimated Watts, kWh per session, cost in BGN

---

### Hub → Backend
- WiFi (HTTP or MQTT)
- Sends:
  - device status (heartbeat)
- IR command confirmations (for AC state tracking)
  - device status (heartbeat)

---

### Backend → Hub
- Sends schedule-based commands:
  - target temperature
  - AC state (on/off, mode)
  - which IR emitter(s) to target

---

### Hub → IR Emitter
- Zigbee (mesh, routes through co-located CT clamp sensor)
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

### IR Emitter (ESP32-H2)
- Battery-powered (18650 cell)
- Zigbee sleepy end-device — polls coordinator at configurable interval (e.g. 1 min)

---

## 📡 Communication Choices

| Link | Protocol | Notes |
|---|---|---|
| Hub → IR Emitters | Zigbee | Direct link for MVP (1 AC); multi-AC may need router node |
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
- Coordinator must be powered and reachable for IR emitters to operate
- Zigbee network formed at factory; devices re-join automatically if hub restarts
- MVP (1 AC): direct link hub → IR emitter, no mesh routing needed
- Multi-AC through concrete walls: may need a mains-powered Zigbee router node per room (v2)

### Mitigations
- Backend maintains AC state
- Periodic state re-sync via IR
- Avoid manual remote usage (or accept drift)

---

## 🚀 MVP Scope

**Goal: Build for yourself first. Prove it works in daily use before selling to anyone.**

### Hardware
- **ESP32-C6** hub — WiFi 6 + Zigbee coordinator + BLE + ESP-NOW, all on one chip
- **Energy estimation in backend** — no additional hardware
  - AC state tracked from IR commands
  - Consumption modeled from AC mode + set temp + outdoor temperature (weather API)
- **1 IR Emitter (ESP32-H2 + IR LED) per AC unit**
  - Placed directly in front of AC — no hub line-of-sight needed
  - Battery-powered (18650) — no extra cables at the AC unit
  - Zigbee end-device — routes through co-located CT clamp; range not dependent on hub placement
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
- **Matter bridge on hub** — ESP32-C6 supports Matter over WiFi; exposing hub as a Matter bridge would allow integration with Apple Home / Google Home without replacing the Zigbee mesh. The 802.15.4 radio is shared between Zigbee and Thread, so Matter/Thread as a replacement protocol is not viable — Matter as a WiFi-based bridge layer is the realistic path.
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
