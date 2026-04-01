# Smart Heating System — Core Use Case

## 🎯 Goal

Provide consistent indoor comfort while minimizing unnecessary energy usage — automatically.

---

## 👤 Primary User

- Lives in an apartment with 1–3 AC units
- Uses AC for heating in winter
- Experiences mismatch between set and real temperature
- Wants comfort without manual adjustments
- Cares about electricity cost (but not obsessively)

---

## 🏠 Scenario: Living Room Heating Optimization

### Initial State

- AC is set to 24°C
- Room feels cold (~21–22°C)
- AC internal sensor is near ceiling → inaccurate
- User manually increases temperature

---

## ⚠️ Problem

- Uneven temperature distribution
- AC operates inefficiently
- User constantly adjusts settings
- Energy may be wasted OR comfort is compromised

---

## ✅ Solution (System Behavior)

### Step 1: Real Temperature Measurement

- External sensor measures temperature near user (e.g. sofa height)
- System treats this as **source of truth**

---

### Step 2: Continuous Monitoring

- Sensor sends temperature every 30–60 seconds
- Hub forwards data to backend

---

### Step 3: Intelligent Control Loop

System maintains a **temperature range**, not fixed value:

- Comfort range: 22–23°C

---

### Step 4: Automated Adjustment
```
if temp < 22:
increase AC power (e.g. set 25°C)
elif temp > 23:
decrease AC power (e.g. set 22°C)
```

- Commands sent via IR
- Full AC state applied (mode, temp, fan)

---

### Step 5: Stabilization

- System avoids frequent toggling
- Adjusts only when outside range
- Periodically re-syncs AC state

---

## 🎯 Outcome

### For User

- Stable, comfortable temperature
- No need for manual adjustments
- Better perceived heating performance

---

### For System

- Reduced overheating
- Reduced unnecessary runtime
- More efficient heating cycles

---

## 💡 Secondary Benefits

- Insight into real room temperature
- Detection of anomalies:
  - sudden drops (open window)
  - slow heating (inefficiency)

---

## 🔄 Edge Case: Manual Remote Use

### Problem

- User changes AC via remote
- System state becomes out of sync

---

### Handling

- Periodic re-application of desired state
- System overrides manual changes gradually

---

## 🧪 Extended Use Cases (Future)

### 1. Night Mode
- Lower temperature automatically during sleep

---

### 2. Away Mode
- Reduce heating when no one is home

---

### 3. Multi-Room Optimization
- Balance heating across rooms

---

### 4. Energy Insights
- Show user:
  - “You are overheating the room”
  - “You can save X% with this setting”

---

## 🧠 Core Value Proposition

> “Your home stays comfortable automatically — without wasting energy or constant adjustments.”
