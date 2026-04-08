/*
 * hub_mqtt.c — MQTT client for command subscription and hub status / LWT.
 *
 * Keeps HTTP for AC-event and metering uploads (http_reporter.c) — only the
 * backend→hub command path moves to MQTT.
 */

#include "hub_mqtt.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mqtt_client.h"   /* ESP-IDF esp-mqtt component */
#include "cJSON.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "hub_mqtt";

/* Override via menuconfig (CONFIG_MQTT_BROKER_URL) or sdkconfig.defaults */
#ifndef CONFIG_MQTT_BROKER_URL
#define CONFIG_MQTT_BROKER_URL "mqtt://192.168.0.202:1883"
#endif

static esp_mqtt_client_handle_t s_client           = NULL;
static mqtt_command_cb_t        s_on_command        = NULL;
static esp_timer_handle_t       s_heartbeat_timer   = NULL;

#define HEARTBEAT_INTERVAL_US  (60LL * 1000 * 1000)   /* 60 seconds */

/* Static buffers — valid for the lifetime of the process */
static char s_hub_id[20];        /* "hub_AABBCCDDEEFF\0" */
static char s_cmd_topic[48];     /* "hub/hub_AABBCCDDEEFF/commands\0" */
static char s_status_topic[48];  /* "hub/hub_AABBCCDDEEFF/status\0"   */

/* ---------------------------------------------------------------------------
 * Heartbeat timer — re-publishes "online" every 60 s so the backend does not
 * mark the hub offline while it is still connected.
 * -------------------------------------------------------------------------*/

static void heartbeat_timer_cb(void *arg)
{
    if (s_client)
        esp_mqtt_client_publish(s_client, s_status_topic, "online", 0, /*qos=*/1, /*retain=*/0);
}

/* ---------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------*/

static void build_topics(void)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(s_hub_id, sizeof(s_hub_id),
             "hub_%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    snprintf(s_cmd_topic,    sizeof(s_cmd_topic),    "hub/%s/commands", s_hub_id);
    snprintf(s_status_topic, sizeof(s_status_topic), "hub/%s/status",   s_hub_id);
}

static void dispatch_command(const char *data, int len)
{
    /* Copy into a null-terminated buffer — MQTT data is not NUL-terminated */
    char *buf = malloc(len + 1);
    if (!buf)
    {
        ESP_LOGE(TAG, "OOM parsing command");
        return;
    }
    memcpy(buf, data, len);
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root)
    {
        ESP_LOGW(TAG, "Invalid JSON in command message");
        return;
    }

    int cmd_id = 0;
    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (cJSON_IsNumber(id_item))
        cmd_id = (int)id_item->valuedouble;

    cJSON *type_item    = cJSON_GetObjectItemCaseSensitive(root, "type");
    cJSON *payload_item = cJSON_GetObjectItemCaseSensitive(root, "payload");

    if (!cJSON_IsString(type_item) || !payload_item)
    {
        ESP_LOGW(TAG, "Command missing 'type' or 'payload' (id=%d)", cmd_id);
        cJSON_Delete(root);
        return;
    }

    ESP_LOGI(TAG, "Command id=%d type=%s", cmd_id, type_item->valuestring);

    if (s_on_command)
        s_on_command(cmd_id, type_item->valuestring, payload_item);

    cJSON_Delete(root);
}

/* ---------------------------------------------------------------------------
 * MQTT event handler
 * -------------------------------------------------------------------------*/

static void mqtt_event_handler(void *arg,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to broker — publishing online, subscribing to %s",
                 s_cmd_topic);
        esp_mqtt_client_publish(s_client, s_status_topic, "online", 0, /*qos=*/1, /*retain=*/0);
        esp_mqtt_client_subscribe(s_client, s_cmd_topic, /*qos=*/1);
        esp_timer_start_periodic(s_heartbeat_timer, HEARTBEAT_INTERVAL_US);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected — broker will deliver LWT, client will reconnect");
        esp_timer_stop(s_heartbeat_timer);
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGD(TAG, "Subscribed (msg_id=%d)", ev->msg_id);
        break;

    case MQTT_EVENT_DATA:
        if (ev->topic_len > 0 &&
            strncmp(ev->topic, s_cmd_topic, ev->topic_len) == 0)
        {
            dispatch_command(ev->data, ev->data_len);
        }
        break;

    case MQTT_EVENT_ERROR:
        if (ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            ESP_LOGE(TAG, "TCP transport error: esp_tls_last_esp_err=%d",
                     ev->error_handle->esp_tls_last_esp_err);
        }
        else
        {
            ESP_LOGE(TAG, "MQTT error type=%d", ev->error_handle->error_type);
        }
        break;

    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------*/

void hub_mqtt_publish_metering(const plug_metering_t *m)
{
    if (!s_client || !m) return;

    /* topic: hub/{hub_id}/plug/{ADDR}/metering (ADDR = 4 uppercase hex digits) */
    char topic[72];
    snprintf(topic, sizeof(topic), "hub/%s/plug/%04X/metering", s_hub_id, m->short_addr);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "addr", m->short_addr);
    cJSON_AddNumberToObject(root, "ts",   m->unix_ts);
    if (m->has_power)   cJSON_AddNumberToObject(root, "watts",      m->active_power_w);
    if (m->has_voltage) cJSON_AddNumberToObject(root, "voltage_dv", m->voltage_dv);
    if (m->has_current) cJSON_AddNumberToObject(root, "current_ma", m->current_ma);
    if (m->has_energy) {
        /* uint64 → send as string to avoid JSON double precision loss */
        char buf[24];
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)m->energy_wh);
        cJSON_AddStringToObject(root, "energy_wh", buf);
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return;

    esp_mqtt_client_publish(s_client, topic, payload, 0, /*qos=*/0, /*retain=*/0);
    ESP_LOGD(TAG, "Metering → %s: %s", topic, payload);
    free(payload);
}

void hub_mqtt_publish_network(const uint16_t *addrs, uint8_t count)
{
    if (!s_client) return;

    char topic[64];
    snprintf(topic, sizeof(topic), "hub/%s/network", s_hub_id);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "plugs");
    for (uint8_t i = 0; i < count; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(addrs[i]));

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return;

    esp_mqtt_client_publish(s_client, topic, payload, 0, /*qos=*/1, /*retain=*/0);
    ESP_LOGI(TAG, "Network scan → %s: %s", topic, payload);
    free(payload);
}

esp_err_t hub_mqtt_init(mqtt_command_cb_t on_command)
{
    s_on_command = on_command;

    build_topics();

    ESP_LOGI(TAG, "hub_id=%s broker=%s", s_hub_id, CONFIG_MQTT_BROKER_URL);
    ESP_LOGI(TAG, "cmd_topic=%s  status_topic=%s", s_cmd_topic, s_status_topic);

    esp_mqtt_client_config_t cfg = {
        .broker = {
            .address.uri = CONFIG_MQTT_BROKER_URL,
        },
        .credentials = {
            .client_id = s_hub_id,
        },
        .session = {
            .keepalive = 60,
            .last_will = {
                .topic   = s_status_topic,
                .msg     = "offline",
                .msg_len = 0,   /* 0 → use strlen */
                .qos     = 1,
                .retain  = 0,
            },
        },
        .network = {
            .reconnect_timeout_ms = 5000,
        },
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client)
    {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client,
                                   ESP_EVENT_ANY_ID,
                                   mqtt_event_handler,
                                   NULL);

    const esp_timer_create_args_t timer_args = {
        .callback = heartbeat_timer_cb,
        .name     = "mqtt_heartbeat",
    };
    esp_timer_create(&timer_args, &s_heartbeat_timer);

    return esp_mqtt_client_start(s_client);
}
