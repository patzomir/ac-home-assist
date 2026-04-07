/*
 * Zigbee Hub — ESP32-C6 Zero
 *
 * Roles:
 *   - Zigbee Coordinator: forms the network, pairs ESP32-H2 IR emitters
 *   - WiFi STA: reports AC events to backend HTTP API
 *   - MQTT client: receives commands from backend instantly (no polling)
 *   - Scheduler: applies night-setback / preheat schedule via esp_timer
 *
 * Build: idf.py -p /dev/cu.usbserial-XXXX flash monitor
 */

#include "zigbee_coordinator.h"
#include "wifi_manager.h"
#include "ac_schedule.h"
#include "http_reporter.h"
#include "hub_mqtt.h"
#include "nvs_store.h"

#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#include <string.h>

/* ---- Configuration (override via menuconfig / sdkconfig.defaults) ------- */

#ifndef CONFIG_TIMEZONE
#define CONFIG_TIMEZONE "EET-2EEST,M3.5.0/3,M10.5.0/4" /* Sofia, Bulgaria */
#endif

static const char *TAG = "main";

/* ---------------------------------------------------------------------------
 * MQTT command dispatch
 * -------------------------------------------------------------------------*/

static ac_mode_t parse_mode(const char *s)
{
    if (!s)                      return AC_MODE_HEAT;
    if (strcmp(s, "cool") == 0)  return AC_MODE_COOL;
    if (strcmp(s, "fan")  == 0)  return AC_MODE_FAN;
    if (strcmp(s, "auto") == 0)  return AC_MODE_AUTO;
    if (strcmp(s, "off")  == 0)  return AC_MODE_OFF;
    return AC_MODE_HEAT;
}

static void handle_set_schedule(const cJSON *payload)
{
    cJSON *addr_item = cJSON_GetObjectItemCaseSensitive(payload, "addr");
    if (!cJSON_IsNumber(addr_item))
    {
        ESP_LOGW(TAG, "set_schedule: missing addr");
        return;
    }

    ac_schedule_t sched = {
        .short_addr     = (uint16_t)addr_item->valuedouble,
        .mode           = parse_mode(cJSON_GetStringValue(
                              cJSON_GetObjectItemCaseSensitive(payload, "mode"))),
        .comfort_temp_c = 21,
        .setback_temp_c = 18,
        .sleep_hour     = 23,
        .sleep_min      = 0,
        .wake_hour      = 7,
        .wake_min       = 0,
        .preheat_min    = 45,
    };

#define _INT(key, field) \
    do { cJSON *_i = cJSON_GetObjectItemCaseSensitive(payload, key); \
         if (cJSON_IsNumber(_i)) sched.field = (uint8_t)_i->valuedouble; } while(0)

    _INT("comfort_temp_c",  comfort_temp_c);
    _INT("setback_temp_c",  setback_temp_c);
    _INT("sleep_hour",      sleep_hour);
    _INT("sleep_minute",    sleep_min);
    _INT("wake_hour",       wake_hour);
    _INT("wake_minute",     wake_min);
    _INT("preheat_minutes", preheat_min);
#undef _INT

    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(payload, "enabled");
    if (cJSON_IsFalse(enabled))
    {
        ac_schedule_clear(sched.short_addr);
        ESP_LOGI(TAG, "Schedule disabled for 0x%04x", sched.short_addr);
    }
    else
    {
        ac_schedule_set(&sched);
        ESP_LOGI(TAG, "Schedule updated for 0x%04x (sleep %02d:%02d → %d°C, "
                      "wake %02d:%02d → %d°C)",
                 sched.short_addr,
                 sched.sleep_hour, sched.sleep_min, sched.setback_temp_c,
                 sched.wake_hour,  sched.wake_min,  sched.comfort_temp_c);
    }
}

static void handle_set_ac(const cJSON *payload)
{
    cJSON *addr_item = cJSON_GetObjectItemCaseSensitive(payload, "addr");
    if (!cJSON_IsNumber(addr_item))
    {
        ESP_LOGW(TAG, "set_ac: missing addr");
        return;
    }
    uint16_t addr = (uint16_t)addr_item->valuedouble;

    cJSON *power_item = cJSON_GetObjectItemCaseSensitive(payload, "power");
    bool power_on = !cJSON_IsFalse(power_item);  /* default on */

    if (!power_on)
    {
        zb_coordinator_send_power(addr, false);
        ESP_LOGI(TAG, "set_ac: 0x%04x → OFF", addr);
        return;
    }

    ac_mode_t mode = parse_mode(cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(payload, "mode")));
    int8_t setpoint_c = 21;
    cJSON *sp = cJSON_GetObjectItemCaseSensitive(payload, "setpoint");
    if (cJSON_IsNumber(sp))
        setpoint_c = (int8_t)sp->valuedouble;

    zb_coordinator_send_setpoint(addr, setpoint_c, mode);
    ESP_LOGI(TAG, "set_ac: 0x%04x → %d°C mode=%d", addr, setpoint_c, mode);
}

static void handle_set_plug(const cJSON *payload)
{
    cJSON *addr_item  = cJSON_GetObjectItemCaseSensitive(payload, "addr");
    cJSON *power_item = cJSON_GetObjectItemCaseSensitive(payload, "power");
    if (!cJSON_IsNumber(addr_item))
    {
        ESP_LOGW(TAG, "set_plug: missing addr");
        return;
    }
    uint16_t addr    = (uint16_t)addr_item->valuedouble;
    bool     power_on = !cJSON_IsFalse(power_item);

    zb_coordinator_send_power(addr, power_on);
    ESP_LOGI(TAG, "set_plug: 0x%04x → %s", addr, power_on ? "ON" : "OFF");
}

static void on_mqtt_command(int cmd_id, const char *type, const cJSON *payload)
{
    if (strcmp(type, "set_schedule") == 0)
        handle_set_schedule(payload);
    else if (strcmp(type, "set_ac") == 0)
        handle_set_ac(payload);
    else if (strcmp(type, "set_plug") == 0)
        handle_set_plug(payload);
    else
        ESP_LOGW(TAG, "Unknown command type: %s (id=%d)", type, cmd_id);
}

/* ---- Zigbee coordinator callbacks --------------------------------------- */

static void on_network_formed(uint16_t pan_id, uint8_t channel)
{
    ESP_LOGI(TAG, "Zigbee network up — PAN 0x%04x ch %d", pan_id, channel);
}

static void on_plug_joined(const ir_emitter_t *plug)
{
    ESP_LOGI(TAG, "Smart plug joined: 0x%04x ep=%d", plug->short_addr, plug->endpoint);
    /* Remove any stale thermostat schedule that may exist for this address */
    ac_schedule_clear(plug->short_addr);
}

static void on_device_joined(const ir_emitter_t *emitter)
{
    ESP_LOGI(TAG, "IR emitter joined: 0x%04x", emitter->short_addr);

    /* Apply a default night-setback schedule on first join */
    ac_schedule_t sched = {
        .short_addr = emitter->short_addr,
        .mode = AC_MODE_HEAT,
        .comfort_temp_c = 21,
        .setback_temp_c = 18,
        .sleep_hour = 23,
        .sleep_min = 0,
        .wake_hour = 7,
        .wake_min = 0,
        .preheat_min = 45,
    };
    /* Only set default if no existing schedule */
    ac_schedule_t existing;
    if (ac_schedule_get(emitter->short_addr, &existing) != ESP_OK)
    {
        ac_schedule_set(&sched);
        ESP_LOGI(TAG, "Default schedule applied to 0x%04x", emitter->short_addr);
    }
}

static void on_device_left(uint16_t short_addr)
{
    ESP_LOGW(TAG, "IR emitter left: 0x%04x", short_addr);
}

static void on_cmd_ack(uint16_t short_addr, bool success)
{
    if (!success)
    {
        ESP_LOGW(TAG, "Command to 0x%04x was not acknowledged", short_addr);
    }
}

static void on_plug_metering(const plug_metering_t *reading)
{
    http_reporter_send_metering(reading);
}

/* ---- WiFi callbacks ----------------------------------------------------- */

static void wifi_connected_task(void *arg)
{
    ESP_LOGI(TAG, "WiFi connected — syncing time");

    /* SNTP time sync */
    setenv("TZ", CONFIG_TIMEZONE, 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.cloudflare.com");
    esp_sntp_init();

    /* Wait for time sync (up to 10 s) */
    time_t now = 0;
    struct tm timeinfo = {0};
    int retries = 0;
    while (timeinfo.tm_year < (2020 - 1900) && retries < 20)
    {
        vTaskDelay(pdMS_TO_TICKS(500));
        time(&now);
        localtime_r(&now, &timeinfo);
        retries++;
    }
    if (timeinfo.tm_year >= (2020 - 1900))
    {
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
        ESP_LOGI(TAG, "Time synced: %s", buf);
    }
    else
    {
        ESP_LOGW(TAG, "Time sync timed out — schedule may fire at wrong time");
    }

    /* Start subsystems that need a working clock and network */
    ac_schedule_init();
    http_reporter_init();
    hub_mqtt_init(on_mqtt_command);

    vTaskDelete(NULL);
}

static void on_wifi_connected(void)
{
    /* Offload heavy init (blocking SNTP wait, schedule/reporter/MQTT init) to a
       dedicated task — on_wifi_connected runs on the sys_evt stack (2 KB)
       which is too small for this work. */
    xTaskCreate(wifi_connected_task, "wifi_init", 4096, NULL, 5, NULL);
}

static void on_wifi_disconnected(void)
{
    ESP_LOGW(TAG, "WiFi disconnected — reporting paused");
    /* MQTT client handles its own reconnection internally */
}

/* ---- app_main ----------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "=== AC Home Assist — Zigbee Hub ===");

    /* NVS first — everything else depends on it */
    ESP_ERROR_CHECK(nvs_store_init());

    /* WiFi: loads credentials from NVS or starts captive portal for provisioning */
    wifi_manager_start(on_wifi_connected, on_wifi_disconnected);

    /* Zigbee coordinator — starts its own FreeRTOS task */
    zb_coordinator_callbacks_t zb_cb = {
        .on_network_formed = on_network_formed,
        .on_device_joined = on_device_joined,
        .on_plug_joined = on_plug_joined,
        .on_device_left = on_device_left,
        .on_cmd_ack = on_cmd_ack,
        .on_plug_metering = on_plug_metering,
    };
    ESP_ERROR_CHECK(zb_coordinator_init(&zb_cb));

    ESP_LOGI(TAG, "Init complete — hub running");
    /* app_main may return; everything runs in tasks and timers */
}
