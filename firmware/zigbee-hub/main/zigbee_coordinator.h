#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_zigbee_core.h"

/* Coordinator endpoint */
#define HUB_ENDPOINT                1

/* IR emitter (ESP32-H2) uses this endpoint */
#define IR_EMITTER_ENDPOINT         10

/* Nous A7Z smart plug default endpoint (actual value confirmed via ZDO) */
#define A7Z_PLUG_ENDPOINT           1

/* Standard ZCL clusters used by Nous A7Z (TS011F-based) for power metering.
 * Scale factors hard-coded by Zigbee2MQTT for this device class:
 *   rmsVoltage:  raw / 10  = V  → raw is in 0.1 V  (voltage_dv)
 *   rmsCurrent:  raw / 1000 = A → raw is in mA      (current_ma)
 *   activePower: raw / 10  = W  → raw is in 0.1 W
 *   currentSummDelivered: raw / 100 = kWh → raw * 10 = Wh */
#define ELEC_MEAS_CLUSTER_ID        0x0B04
#define ELEC_MEAS_ATTR_VOLTAGE_ID   0x0505  /* rmsVoltage,   int16, raw=0.1 V  */
#define ELEC_MEAS_ATTR_CURRENT_ID   0x0508  /* rmsCurrent,  uint16, raw=mA     */
#define ELEC_MEAS_ATTR_POWER_ID     0x050B  /* activePower,  int16, raw=0.1 W  */

#define ZCL_METERING_CLUSTER_ID     0x0702
#define ZCL_METERING_ATTR_ENERGY_ID 0x0000  /* currentSummDelivered, uint48, raw*10=Wh */

/* Temperature: Zigbee represents °C × 100 (int16_t) */
#define ZB_TEMP(celsius)            ((int16_t)((celsius) * 100))

/* Device type determined via ZDO simple-descriptor discovery */
typedef enum {
    DEVICE_UNKNOWN    = 0,
    DEVICE_IR_EMITTER = 1,
    DEVICE_SMART_PLUG = 2,
} device_type_t;

/* Modes sent to IR emitter via thermostat system-mode attribute */
typedef enum {
    AC_MODE_OFF  = 0x00,
    AC_MODE_HEAT = 0x04,
    AC_MODE_COOL = 0x03,
    AC_MODE_FAN  = 0x07,
    AC_MODE_AUTO = 0x01,
} ac_mode_t;

/* Represents any paired Zigbee device (IR emitter or smart plug) */
typedef struct {
    uint16_t      short_addr;
    esp_zb_ieee_addr_t ieee_addr;
    uint8_t       endpoint;
    bool          online;
    device_type_t device_type;
} ir_emitter_t;

/* Up to 4 AC units per hub (MVP: 1) */
#define MAX_EMITTERS 4

/* Power metering reading from a smart plug (Nous A7Z, Tuya cluster 0xe001) */
typedef struct {
    uint16_t short_addr;
    bool     has_power;        /* active_power_w is valid */
    int16_t  active_power_w;   /* instantaneous power in W  */
    bool     has_energy;       /* energy_wh is valid        */
    uint64_t energy_wh;        /* total energy in Wh        */
    bool     has_voltage;      /* voltage_dv is valid       */
    uint32_t voltage_dv;       /* voltage in units of 0.1 V */
    bool     has_current;      /* current_ma is valid       */
    uint32_t current_ma;       /* current in mA             */
    uint32_t unix_ts;
} plug_metering_t;

/* Callbacks fired by the coordinator */
typedef struct {
    void (*on_network_formed)(uint16_t pan_id, uint8_t channel);
    void (*on_device_joined)(const ir_emitter_t *emitter);  /* IR emitter only */
    void (*on_plug_joined)(const ir_emitter_t *plug);       /* smart plug only */
    void (*on_device_left)(uint16_t short_addr);
    void (*on_cmd_ack)(uint16_t short_addr, bool success);
    void (*on_plug_metering)(const plug_metering_t *reading);
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

/* Get list of paired devices */
const ir_emitter_t *zb_coordinator_get_emitters(uint8_t *count_out);

/* Remove all paired devices from RAM and NVS */
esp_err_t zb_coordinator_forget_all(void);
