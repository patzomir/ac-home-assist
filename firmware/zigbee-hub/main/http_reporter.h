#pragma once

#include "esp_err.h"
#include "zigbee_coordinator.h"

/* Backend base URL — set via menuconfig or override at runtime */
#ifndef CONFIG_BACKEND_URL
#define CONFIG_BACKEND_URL "http://your-backend/api"
#endif

/* Report an AC state change to the backend */
typedef struct {
    uint16_t  short_addr;
    int8_t    setpoint_c;
    ac_mode_t mode;
    bool      power_on;
    uint32_t  unix_ts;
} ac_event_t;

esp_err_t http_reporter_init(void);
esp_err_t http_reporter_send_event(const ac_event_t *event);

/* Report a smart plug metering reading to the backend */
esp_err_t http_reporter_send_metering(const plug_metering_t *reading);
