#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef void (*wifi_connected_cb_t)(void);
typedef void (*wifi_disconnected_cb_t)(void);

esp_err_t wifi_manager_init(const char *ssid, const char *password,
                             wifi_connected_cb_t on_connected,
                             wifi_disconnected_cb_t on_disconnected);

bool wifi_manager_is_connected(void);
