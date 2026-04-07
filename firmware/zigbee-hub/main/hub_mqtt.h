#pragma once

#include "esp_err.h"
#include "cJSON.h"

/*
 * Lightweight MQTT client for hub ↔ backend command transport.
 *
 * Topic layout:
 *   hub/{hub_id}/commands   ← backend publishes, hub subscribes  (QoS 1)
 *   hub/{hub_id}/status     ← hub publishes "online" on connect;
 *                              LWT delivers "offline" on unexpected disconnect
 *
 * Hub identity is derived from the WiFi STA MAC address, matching the
 * X-Hub-Id header sent by http_reporter.c ("hub_AABBCCDDEEFF").
 *
 * Command message format (JSON):
 *   { "id": <int>, "type": "<set_schedule|set_ac|set_plug>", "payload": {...} }
 */

/* Callback invoked (from the MQTT event task) for each incoming command.
   `payload` is owned by the caller — do not free or hold a reference to it. */
typedef void (*mqtt_command_cb_t)(int cmd_id, const char *type, const cJSON *payload);

/* Initialise and start the MQTT client.
   Must be called after WiFi is connected (MAC address must be available).
   `on_command` may be NULL if the caller only needs LWT / status. */
esp_err_t hub_mqtt_init(mqtt_command_cb_t on_command);
