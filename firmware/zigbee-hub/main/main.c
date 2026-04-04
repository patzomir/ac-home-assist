/*
 * Zigbee Hub — ESP32-C6 Zero
 *
 * Roles:
 *   - Zigbee Coordinator: forms the network, pairs ESP32-H2 IR emitters
 *   - WiFi STA: reports AC events to backend HTTP API
 *   - Scheduler: applies night-setback / preheat schedule via esp_timer
 *
 * Build: idf.py -p /dev/cu.usbserial-XXXX flash monitor
 */

#include "zigbee_coordinator.h"
#include "wifi_manager.h"
#include "ac_schedule.h"
#include "http_reporter.h"
#include "nvs_store.h"

#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ---- Configuration (override via menuconfig / sdkconfig.defaults) ------- */

#ifndef CONFIG_WIFI_SSID
#define CONFIG_WIFI_SSID     "your-ssid"
#endif

#ifndef CONFIG_WIFI_PASSWORD
#define CONFIG_WIFI_PASSWORD "your-password"
#endif

#ifndef CONFIG_TIMEZONE
#define CONFIG_TIMEZONE      "EET-2EEST,M3.5.0/3,M10.5.0/4"  /* Sofia, Bulgaria */
#endif

static const char *TAG = "main";

/* ---- Zigbee coordinator callbacks --------------------------------------- */

static void on_network_formed(uint16_t pan_id, uint8_t channel)
{
    ESP_LOGI(TAG, "Zigbee network up — PAN 0x%04x ch %d", pan_id, channel);
}

static void on_device_joined(const ir_emitter_t *emitter)
{
    ESP_LOGI(TAG, "IR emitter joined: 0x%04x", emitter->short_addr);

    /* Apply a default night-setback schedule on first join */
    ac_schedule_t sched = {
        .short_addr     = emitter->short_addr,
        .mode           = AC_MODE_HEAT,
        .comfort_temp_c = 21,
        .setback_temp_c = 18,
        .sleep_hour     = 23,
        .sleep_min      = 0,
        .wake_hour      = 7,
        .wake_min       = 0,
        .preheat_min    = 45,
    };
    /* Only set default if no existing schedule */
    ac_schedule_t existing;
    if (ac_schedule_get(emitter->short_addr, &existing) != ESP_OK) {
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
    if (!success) {
        ESP_LOGW(TAG, "Command to 0x%04x was not acknowledged", short_addr);
    }
}

static void on_plug_metering(const plug_metering_t *reading)
{
    http_reporter_send_metering(reading);
}

/* ---- WiFi callbacks ----------------------------------------------------- */

static void on_wifi_connected(void)
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
    struct tm timeinfo = { 0 };
    int retries = 0;
    while (timeinfo.tm_year < (2020 - 1900) && retries < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        time(&now);
        localtime_r(&now, &timeinfo);
        retries++;
    }
    if (timeinfo.tm_year >= (2020 - 1900)) {
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
        ESP_LOGI(TAG, "Time synced: %s", buf);
    } else {
        ESP_LOGW(TAG, "Time sync timed out — schedule may fire at wrong time");
    }

    /* Start schedule engine after we have a clock */
    ac_schedule_init();
    http_reporter_init();
}

static void on_wifi_disconnected(void)
{
    ESP_LOGW(TAG, "WiFi disconnected — reporting paused");
}

/* ---- app_main ----------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "=== AC Home Assist — Zigbee Hub ===");

    /* NVS first — everything else depends on it */
    ESP_ERROR_CHECK(nvs_store_init());

    /* WiFi (non-blocking connect, callbacks handle readiness) */
    wifi_manager_init(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD,
                      on_wifi_connected, on_wifi_disconnected);

    /* Zigbee coordinator — starts its own FreeRTOS task */
    zb_coordinator_callbacks_t zb_cb = {
        .on_network_formed  = on_network_formed,
        .on_device_joined   = on_device_joined,
        .on_device_left     = on_device_left,
        .on_cmd_ack         = on_cmd_ack,
        .on_plug_metering   = on_plug_metering,
    };
    ESP_ERROR_CHECK(zb_coordinator_init(&zb_cb));

    ESP_LOGI(TAG, "Init complete — hub running");
    /* app_main may return; everything runs in tasks and timers */
}
