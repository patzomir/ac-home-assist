#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef void (*wifi_connected_cb_t)(void);
typedef void (*wifi_disconnected_cb_t)(void);

/*
 * Start WiFi manager.
 *
 * Loads SSID/password from NVS:
 *   - If credentials exist  → connect in STA mode (callbacks fire on connect/disconnect).
 *   - If no credentials     → start SoftAP "AC-Hub-Setup" and serve a captive portal at
 *                             http://192.168.4.1 — does NOT return until the user submits
 *                             credentials (device restarts automatically).
 */
esp_err_t wifi_manager_start(wifi_connected_cb_t on_connected,
                              wifi_disconnected_cb_t on_disconnected);

bool wifi_manager_is_connected(void);

/* Persist WiFi credentials to NVS (also used internally by the captive portal). */
esp_err_t wifi_manager_save_creds(const char *ssid, const char *password);
