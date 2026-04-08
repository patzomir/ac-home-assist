#pragma once

#include "esp_err.h"
#include "cJSON.h"
#include "zigbee_coordinator.h"

/*
 * Lightweight MQTT client for hub ↔ backend command transport.
 *
 * Topic layout:
 *   hub/{hub_id}/commands              ← backend publishes, hub subscribes  (QoS 1)
 *   hub/{hub_id}/status                ← hub publishes "online" on connect;
 *                                         LWT delivers "offline" on unexpected disconnect
 *   hub/{hub_id}/plug/{addr}/metering  ← hub publishes plug power readings  (QoS 0)
 *   hub/{hub_id}/network               ← hub publishes active plug list on demand (QoS 1)
 *
 * Hub identity is derived from the WiFi STA MAC address, matching the
 * X-Hub-Id header sent by http_reporter.c ("hub_AABBCCDDEEFF").
 *
 * Command message format (JSON):
 *   { "id": <int>, "type": "<set_schedule|set_ac|set_plug|scan_network>", "payload": {...} }
 */

/* Callback invoked (from the MQTT event task) for each incoming command.
   `payload` is owned by the caller — do not free or hold a reference to it. */
typedef void (*mqtt_command_cb_t)(int cmd_id, const char *type, const cJSON *payload);

/* Initialise and start the MQTT client.
   Must be called after WiFi is connected (MAC address must be available).
   `on_command` may be NULL if the caller only needs LWT / status. */
esp_err_t hub_mqtt_init(mqtt_command_cb_t on_command);

/* Publish a plug metering reading to hub/{hub_id}/plug/{addr}/metering.
   Only fields marked has_* are included in the JSON payload.
   Safe to call from any task after hub_mqtt_init(). */
void hub_mqtt_publish_metering(const plug_metering_t *m);

/* Publish active plug addresses to hub/{hub_id}/network (QoS 1).
   addrs:  array of Zigbee short addresses for active smart plugs.
   count:  number of entries in addrs.
   Safe to call from any task after hub_mqtt_init(). */
void hub_mqtt_publish_network(const uint16_t *addrs, uint8_t count);
