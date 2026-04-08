# Smart Heating System Architecture (ESP32-C6 + Zigbee + IR)

## Overview

System for optimizing home heating using:
- 1 Hub per home (ESP32-C6 + WiFi + Zigbee coordinator + BLE)
- 1 IR Emitter per AC unit (ESP32-H2 + IR LED, Zigbee end-device)
- 1 Zigbee smart plug (Nous A7Z) per AC unit — real power measurement + command confirmation
- 1 Zigbee temperature sensor per room — closed-loop feedback
- Backend for scheduling, control, and energy accounting
- Hub-and-spoke: само хъбът се конфигурира от клиента, IR emitter-ите се pre-configure-ват преди изпращане

---

## 🏠 Architecture

### In-Home Setup

#### 1. Hub (1 per home)
- **ESP32-C6** — WiFi 6 + BLE 5 + 802.15.4 (Zigbee coordinator, built-in)
- WiFi connection to backend
- Zigbee coordinator for smart plugs, temperature sensors, and IR emitters (single mesh)
- BLE for hub provisioning only (customer WiFi setup)

**Responsibilities:**
- Act as Zigbee coordinator — receive power data from plugs and temperature data from sensors, forward to backend
- Receive schedule commands from backend
- Send IR commands to emitters via Zigbee
- Sync time to IR emitters via Zigbee Time cluster
- Pair Zigbee devices to coordinator before shipping
- Forward device status (heartbeat) to backend

---

#### 2. Power Measurement (Nous A7Z Zigbee Smart Plug)

Each AC unit is powered through a **Nous A7Z** Zigbee smart plug. This serves two roles:

**Role 1 — Real energy measurement:**
- Measures actual Watts in real time
- Reports to hub via Zigbee; hub forwards to backend
- Backend aggregates into kWh and cost in BGN

**Role 2 — Power-Sensing as Truth (command confirmation):**
- IR is one-way — there is no direct feedback from the AC unit
- The Nous A7Z acts as a state sensor: if power does not rise within N seconds after an "ON" command, the system detects a failure and retries
- This converts a fire-and-forget IR system into one with observable outcomes

**Why Nous A7Z over CT clamp:**
- CT clamps must clamp a single wire — standard AC cables carry L + N together, fields cancel, reads zero
- Getting to separated wires requires opening socket box or AC terminal — not viable for self-install at scale
- Nous A7Z is plug-and-play Zigbee, pre-paired before shipping

**Nous A7Z — Custom Firmware Note:**
The Nous A7Z uses Tuya-based firmware with non-standard ZCL clusters for power reporting. Integration requires:
- **ZDO (Zigbee Device Objects)** — automatic device discovery and endpoint enumeration
- Direct handling of **Tuya-specific ZCL clusters** — not exposed through standard ESPHome climate components
- This is one of the primary reasons for custom firmware on the coordinator (see "Why custom firmware" below)

---

#### 3. Temperature Sensor (Zigbee, 1 per room)

The temperature sensor is **core infrastructure**, not an optional add-on. Without it, the system has no closed-loop feedback — it fires IR commands by schedule but cannot verify the room has reached the target temperature.

**Role:**
- Provides real room temperature to the backend
- Enables Closed-loop control: backend adjusts setpoint commands based on actual temperature vs. target
- Enables Soft Start scheduling: system knows when to begin ramping up to avoid cold-start at expensive tariff hours

---

#### 4. IR Emitter (1 per AC unit)
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

**Distributed Scheduling (Локална автономия):**
Each H2 stores its own schedule locally in **NVS (Non-Volatile Storage)**. This means:
- If Wi-Fi drops, the H2 continues executing its schedule without backend connectivity
- If the Zigbee network is temporarily unavailable, the H2 can still fire IR commands at the correct times
- The backend acts as the source of truth for schedule updates; the H2 is the execution engine
- Schedule updates are pushed to the H2 by the hub when connectivity is restored

**Time Sync Strategy:**
The H2 maintains an internal software clock synchronized via the **Zigbee Time cluster** through the coordinator (C6). On startup and periodically during operation, the H2 requests the current time from the coordinator. This eliminates the need for a separate RTC chip and keeps all devices on a consistent time base.

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

#### 5. Why Custom Firmware (not ESPHome)

ESPHome is excellent for standard integrations, but this project requires capabilities outside its scope:

**ZDO (Zigbee Device Objects) — Automatic Device Discovery:**
The coordinator must enumerate endpoints and clusters on paired devices automatically. ZDO provides the mechanism for discovering what a device supports without hardcoded assumptions. ESPHome does not expose raw ZDO access — it targets known device types with pre-built components.

**Non-standard ZCL Clusters (Tuya / Nous A7Z):**
The Nous A7Z reports power using Tuya-specific ZCL clusters that do not map to standard Zigbee electrical measurement clusters. Handling these requires direct ZCL frame parsing — something that would require complex "Lambda Hell" workarounds in ESPHome. In clean C++ with ESP-IDF and the Espressif Zigbee SDK, this is straightforward cluster attribute handling.

**Structured IR Command Translation:**
The IR emitter must translate structured AC state commands (`{mode: heat, temp: 22, fan: auto}`) into model-specific IR signals. This logic lives in firmware and requires full control over the execution flow — not achievable through ESPHome's declarative configuration model.

**Summary:**

| Requirement | ESPHome | Custom ESP-IDF |
|---|---|---|
| ZDO device discovery | Not exposed | Full access |
| Tuya ZCL clusters | Lambda workarounds | Direct parsing |
| Structured IR translation | Not supported | Native C++ |
| NVS schedule storage | Limited | Full control |
| Zigbee Time cluster sync | Not exposed | Standard SDK call |

---

## 🔧 Provisioning Flow

Zigbee plugs, temperature sensors, and IR emitters are pre-configured before shipping. Only the hub needs to be configured by the customer (WiFi credentials).

```
Factory (before shipping):
1. Hub Zigbee coordinator pairs with each Zigbee plug (Nous A7Z)
2. Hub Zigbee coordinator pairs with each temperature sensor
3. Hub Zigbee coordinator pairs with each IR emitter

Customer setup (at home):
1. Customer plugs in hub
2. Hub enters setup mode (BLE to phone or WiFi AP)
3. Customer enters home WiFi credentials
4. Hub connects to home WiFi — setup complete
5. Customer plugs in Zigbee plugs and places temperature sensors — they join automatically
```

**Key decision:** Zigbee mesh solves the range problem structurally for all in-home devices. Each IR emitter routes through the co-located plug in the same room — no dependency on hub placement or wall attenuation.

---

## ☁️ Backend

### Core Responsibilities
- Store and organize data
- Run control logic
- Send commands to hub
- Receive energy and temperature data forwarded from hub

---

### Data Model

#### Entities

- `Home`
- `Hub`
- `IREmitter`
- `SmartPlug`
- `TemperatureSensor`
- `PowerReading`
- `TemperatureReading`
- `Schedule`

---

### Relationships

- Home
  - has one Hub
  - has one SmartPlug per AC unit
  - has one IREmitter per AC unit
  - has one TemperatureSensor per room

- Hub
  - belongs to Home
  - coordinates many IREmitters, SmartPlugs, TemperatureSensors (via Zigbee)

- IREmitter
  - belongs to Home
  - controlled by Hub

- Schedule
  - belongs to Home
  - defines time + target AC state (temp, mode)
  - pushed to IREmitter NVS for local execution

---

## 🔄 Data Flow

### Hub → Backend
- WiFi (HTTP or MQTT)
- Sends:
  - real power readings from Nous A7Z plugs
  - real temperature readings from room sensors
  - device status (heartbeat)
  - IR command confirmations (for AC state tracking)

---

### Backend → Hub
- Sends schedule-based commands:
  - target temperature
  - AC state (on/off, mode)
  - which IR emitter(s) to target
  - updated schedules for H2 NVS storage

---

### Hub → IR Emitter
- Zigbee (mesh, routes through co-located Nous A7Z plug)
- Sends: full AC state command + schedule updates

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
06:15 → set_ac(temp=22, mode=heat, fan=auto)  ← Soft Start begins
07:00 → target reached (confirmed by temp sensor)
```

The H2 stores and executes the schedule locally. The backend refines timing based on temperature sensor feedback.

### Economic Setpoint Management

The goal is not a simple thermostat — it is **load shifting**. The system moves heating consumption to cheaper tariff hours:

- During cheap hours (night): maintain comfort temperature at low power
- Before expensive hours: pre-heat the room so the AC doesn't need to run hard at peak tariff
- The apartment acts as a thermal battery

### Soft Start / Setpoint Ramping

Instead of jumping from 18°C to 22°C at wake-up time (forcing full compressor power), the system ramps the setpoint gradually starting ~45 minutes earlier.

This keeps the inverter compressor in the high-$COP$ (Coefficient of Performance) operating zone — small temperature delta, high efficiency. The total energy used is lower and the peak draw is avoided.

### Power-Sensing as Truth

Because IR is one-way, the system uses the Nous A7Z power reading to confirm command execution:

```
1. Backend sends "ON" command → hub forwards to H2 via Zigbee → H2 fires IR
2. Backend monitors Nous A7Z power reading for N seconds
3. If power rises: command confirmed, AC state updated
4. If power does not rise: command failed → retry (Double Pulse)
```

This converts a fire-and-forget protocol into an observable control loop.

### Hysteresis

The system does not chase 0.1°C precision. A deadband of **±1.0°C** is applied around the setpoint:
- Below (setpoint − 1.0°C): turn on heating
- Above (setpoint + 1.0°C): turn off heating
- Between: no action

This prevents short-cycling the compressor, which degrades efficiency and hardware life.

### Key Principles

- Distributed execution: H2 runs schedules locally (NVS), backend refines based on sensor data
- Hysteresis prevents compressor short-cycling
- Power-sensing confirms IR commands; Double Pulse retries on failure
- Time-based scheduling is the default; closed-loop temperature control refines it

---

## 🛡️ Firmware Resilience

### Watchdog Logic (H2 — Freeze Protection)

The H2 contains local protective logic that operates **independently of all external connectivity**:

- If room temperature drops below **15°C**, the H2 automatically commands the AC to heating mode, regardless of backend status or schedule
- This prevents pipe freeze in unoccupied properties without requiring a working internet connection
- The threshold is configurable but defaults to 15°C

### Redundancy — Double Pulse

IR signals can fail due to line-of-sight obstruction, IR noise, or AC unit sleep state. For critical commands (ON / mode change), the H2 implements retry logic:

1. Send IR command
2. Wait for power confirmation from Nous A7Z (via backend feedback)
3. If no confirmation within timeout: resend IR command
4. Repeat up to N times before raising an alert

### Hysteresis (Compressor Protection)

See Control Logic section. The ±1.0°C deadband is enforced in both firmware (H2 local logic) and backend (cloud schedule refinement).

---

## 🔋 Power Strategy

### Hub (ESP32-C6)
- Always powered (USB)

### IR Emitter (ESP32-H2)
- Battery-powered (18650 cell)
- Zigbee sleepy end-device — polls coordinator at configurable interval (e.g. 1 min)

### Nous A7Z Smart Plug
- Mains-powered (always on)
- Acts as Zigbee router node — improves mesh coverage in the room

---

## 📡 Communication Choices

| Link | Protocol | Notes |
|---|---|---|
| Hub → IR Emitters | Zigbee | Direct link for MVP (1 AC); multi-AC may need router node |
| Hub → Smart Plugs | Zigbee | Nous A7Z also acts as Zigbee router |
| Hub → Temp Sensors | Zigbee | Sleepy end-devices or routers depending on model |
| Hub → Backend | WiFi (HTTP/MQTT) | Hub placed centrally for good WiFi coverage |
| IR Emitter → AC | IR | Line-of-sight to local AC unit only |
| H2 ↔ C6 (time) | Zigbee Time cluster | H2 syncs internal clock from coordinator |

---

## ⚠️ Known Constraints

### IR Limitations
- No direct feedback from AC (one-way) — mitigated by Power-Sensing as Truth
- Possible desync if manual remote is used
- IR emitter requires line-of-sight to its local AC unit (not to hub)
- IR retry (Double Pulse) handles transient failures; persistent failures raise alerts

### Zigbee Coordinator
- Hub (ESP32-C6) runs Zigbee coordinator stack (Espressif Zigbee SDK)
- Coordinator must be powered and reachable for real-time updates; H2 runs autonomously from NVS when coordinator is unavailable
- Zigbee network formed at factory; devices re-join automatically if hub restarts
- MVP (1 AC): direct link hub → IR emitter, no mesh routing needed
- Multi-AC through concrete walls: may need a mains-powered Zigbee router node per room (v2)

### Mitigations
- Backend maintains AC state
- Periodic state re-sync via IR
- H2 NVS schedule provides autonomy during connectivity loss
- Power-Sensing as Truth detects missed IR commands
- Watchdog in H2 handles freeze protection without backend

---

## 🚀 MVP Scope

**Goal: Build for yourself first. Prove it works in daily use before selling to anyone.**

### Hardware
- **ESP32-C6** hub — WiFi 6 + Zigbee coordinator + BLE, all on one chip
- **Nous A7Z Zigbee smart plug** per AC unit — real power measurement + command confirmation
- **Zigbee temperature sensor** per room — closed-loop feedback (core, not optional)
- **1 IR Emitter (ESP32-H2 + IR LED)** per AC unit
  - Placed directly in front of AC — no hub line-of-sight needed
  - Battery-powered (18650) — no extra cables at the AC unit
  - Zigbee end-device — routes through co-located smart plug
  - Local schedule in NVS — autonomous operation without backend

### Software
- Simple backend (Django)
- Basic API
- Minimal UI:
  - real-time power draw (W) and session cost (BGN)
  - real-time room temperature vs. setpoint
  - daily / weekly / monthly energy cost
  - schedule configuration (night setback + soft start)

### What MVP is NOT
- No subscription billing
- No multi-tenant / multi-user support
- No mobile app (web UI only)
- No OTA firmware updates
- No in-app onboarding (pre-configuration се прави ръчно преди изпращане)

Keep it simple. Complexity is only justified after it's working well for yourself.

---

## 🧱 Future Improvements (v2+)

- Multi-room optimization with per-room temperature sensors
- Presence detection
- OTA updates for ESP32 and IR emitters
- Native AC integrations (WiFi APIs instead of IR)
- **Matter bridge on hub** — ESP32-C6 supports Matter over WiFi; exposing hub as a Matter bridge would allow integration with Apple Home / Google Home without replacing the Zigbee mesh. The 802.15.4 radio is shared between Zigbee and Thread, so Matter/Thread as a replacement protocol is not viable — Matter as a WiFi-based bridge layer is the realistic path.
- **Day-Ahead пазар (динамични тарифи)**
  - Интеграция с IBEX / ENTSO-E API за почасови спот цени
  - Предзагряване (thermal pre-loading) в евтините часове — апартаментът като топлинна батерия
  - Избягване на пиковите часове (17:00–21:00) без загуба на комфорт
  - Изисква: температурен сензор (вече в MVP) + ценови API в бекенда
  - Очакван допълнителен потенциал: +40–150 BGN/сезон спрямо само нощния setback
  - Архитектурата е готова — само бекенд логиката трябва да се добави

---

## 🧠 Design Principles

- Hub is the only "smart" device the customer interacts with
- Sensors and emitters are simple, autonomous nodes with local fallback
- Backend contains the intelligence; H2 contains the execution
- Power-sensing confirms what IR cannot — observable outcomes over fire-and-forget
- System should work without user intervention after setup
