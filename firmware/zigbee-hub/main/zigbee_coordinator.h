#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_zigbee_core.h"

/* Coordinator endpoint */
#define HUB_ENDPOINT                1

/* IR emitter (ESP32-H2) uses this endpoint */
#define IR_EMITTER_ENDPOINT         10

/* Temperature: Zigbee represents °C × 100 (int16_t) */
#define ZB_TEMP(celsius)            ((int16_t)((celsius) * 100))

/* Modes sent to IR emitter via thermostat system-mode attribute */
typedef enum {
    AC_MODE_OFF  = 0x00,
    AC_MODE_HEAT = 0x04,
    AC_MODE_COOL = 0x03,
    AC_MODE_FAN  = 0x07,
    AC_MODE_AUTO = 0x01,
} ac_mode_t;

/* Represents a paired IR emitter node */
typedef struct {
    uint16_t short_addr;
    esp_zb_ieee_addr_t ieee_addr;
    uint8_t  endpoint;
    bool     online;
} ir_emitter_t;

/* Up to 4 AC units per hub (MVP: 1) */
#define MAX_EMITTERS 4

/* Callbacks fired by the coordinator */
typedef struct {
    void (*on_network_formed)(uint16_t pan_id, uint8_t channel);
    void (*on_device_joined)(const ir_emitter_t *emitter);
    void (*on_device_left)(uint16_t short_addr);
    void (*on_cmd_ack)(uint16_t short_addr, bool success);
} zb_coordinator_callbacks_t;

esp_err_t zb_coordinator_init(const zb_coordinator_callbacks_t *callbacks);

/* Open network for joining (180 s window) */
esp_err_t zb_coordinator_permit_join(uint8_t duration_s);

/* Send setpoint + mode to an emitter.
   setpoint_c is target temperature in °C (e.g. 21).
   mode = AC_MODE_HEAT / AC_MODE_COOL / AC_MODE_OFF. */
esp_err_t zb_coordinator_send_setpoint(uint16_t short_addr, int8_t setpoint_c, ac_mode_t mode);

/* Power off AC via on/off cluster */
esp_err_t zb_coordinator_send_power(uint16_t short_addr, bool on);

/* Get list of paired emitters */
const ir_emitter_t *zb_coordinator_get_emitters(uint8_t *count_out);
