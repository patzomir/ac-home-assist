/*
 * hub_mqtt.c — MQTT client for command subscription and hub status / LWT.
 *
 * Keeps HTTP for AC-event and metering uploads (http_reporter.c) — only the
 * backend→hub command path moves to MQTT.
 */

#include "hub_mqtt.h"

#include "esp_log.h"
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

static esp_mqtt_client_handle_t s_client     = NULL;
static mqtt_command_cb_t        s_on_command = NULL;

/* Static buffers — valid for the lifetime of the process */
static char s_hub_id[20];        /* "hub_AABBCCDDEEFF\0" */
static char s_cmd_topic[48];     /* "hub/hub_AABBCCDDEEFF/commands\0" */
static char s_status_topic[48];  /* "hub/hub_AABBCCDDEEFF/status\0"   */

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
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected — broker will deliver LWT, client will reconnect");
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

    return esp_mqtt_client_start(s_client);
}
