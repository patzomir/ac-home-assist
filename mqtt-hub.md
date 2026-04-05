# MQTT Hub Communication

## Why

Every hub currently has to knock on the server's door every N seconds asking "any commands for me?".  
With MQTT, the hub holds the door open. Commands arrive instantly. No knocking.

## What changes

### Hub → Server (keep as HTTP)
Events and metering are infrequent fire-and-forget POSTs. HTTP is fine here.

### Server → Hub (switch to MQTT)
| Today | With MQTT |
|-------|-----------|
| Hub polls `GET /commands/` on a timer | Hub subscribes to `hub/{hub_id}/commands` |
| Heartbeat is a periodic HTTP POST | MQTT Last Will tells broker when hub goes silent |

## Topic layout

```
hub/{hub_id}/commands   ← server publishes, hub subscribes  (QoS 1)
hub/{hub_id}/status     ← hub publishes "online" on connect, LWT sends "offline"
```

## Components

- **Broker**: Mosquitto on the same VPS — single process, trivial to deploy
- **Firmware**: `esp-mqtt` already compiled into the binary — just needs wiring in `main.c`
- **Backend**: `paho-mqtt` client publishes to `hub/{hub_id}/commands` when a `PendingCommand` is created

## Firmware size

Both `esp_http_client` and `esp-mqtt` are already in the current binary.  
No change if we keep HTTP for uploads + MQTT for commands.  
If we drop HTTP entirely and move everything to MQTT, disabling `esp_http_client` in sdkconfig saves roughly **15–25 KB** of flash.

## Scale

Each hub holds one persistent TCP connection to the broker.  
Idle hubs cost the server nothing. Mosquitto comfortably handles thousands of concurrent connections on modest hardware.
