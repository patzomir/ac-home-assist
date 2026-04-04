#include "http_reporter.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"

#include <string.h>
#include <time.h>

static const char *TAG = "reporter";

static const char *mode_str(ac_mode_t m)
{
    switch (m) {
    case AC_MODE_HEAT: return "heat";
    case AC_MODE_COOL: return "cool";
    case AC_MODE_FAN:  return "fan";
    case AC_MODE_AUTO: return "auto";
    default:           return "off";
    }
}

esp_err_t http_reporter_init(void)
{
    /* Nothing to initialise — stateless HTTP client */
    ESP_LOGI(TAG, "HTTP reporter ready, backend: %s", CONFIG_BACKEND_URL);
    return ESP_OK;
}

esp_err_t http_reporter_send_event(const ac_event_t *ev)
{
    /* Build JSON body */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "addr",      ev->short_addr);
    cJSON_AddNumberToObject(root, "setpoint",  ev->setpoint_c);
    cJSON_AddStringToObject(root, "mode",      mode_str(ev->mode));
    cJSON_AddBoolToObject  (root, "power",     ev->power_on);
    cJSON_AddNumberToObject(root, "ts",        ev->unix_ts
                                               ? ev->unix_ts
                                               : (uint32_t)time(NULL));

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url    = CONFIG_BACKEND_URL "/events",
        .method = HTTP_METHOD_POST,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t ret = esp_http_client_perform(client);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "HTTP POST failed: %s", esp_err_to_name(ret));
    } else {
        int status = esp_http_client_get_status_code(client);
        if (status != 200 && status != 201) {
            ESP_LOGW(TAG, "Backend returned HTTP %d", status);
            ret = ESP_FAIL;
        } else {
            ESP_LOGI(TAG, "Event reported: addr=0x%04x setpoint=%d°C mode=%s",
                     ev->short_addr, ev->setpoint_c, mode_str(ev->mode));
        }
    }

    esp_http_client_cleanup(client);
    free(body);
    return ret;
}

esp_err_t http_reporter_send_metering(const plug_metering_t *m)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "addr", m->short_addr);
    cJSON_AddNumberToObject(root, "ts",   m->unix_ts ? m->unix_ts : (uint32_t)time(NULL));
    if (m->has_power) {
        cJSON_AddNumberToObject(root, "active_power_w", m->active_power_w);
    }
    if (m->has_summation) {
        /* uint64 doesn't fit in JSON double precisely above 2^53;
           send as string to preserve exact value */
        char buf[24];
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)m->summation_wh);
        cJSON_AddStringToObject(root, "summation_wh", buf);
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url    = CONFIG_BACKEND_URL "/plug-metering",
        .method = HTTP_METHOD_POST,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t ret = esp_http_client_perform(client);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Metering POST failed: %s", esp_err_to_name(ret));
    } else {
        int status = esp_http_client_get_status_code(client);
        if (status != 200 && status != 201) {
            ESP_LOGW(TAG, "Backend returned HTTP %d for metering", status);
            ret = ESP_FAIL;
        } else {
            ESP_LOGI(TAG, "Metering reported: addr=0x%04x power=%s%dW sum=%s",
                     m->short_addr,
                     m->has_power ? "" : "n/a",
                     m->has_power ? m->active_power_w : 0,
                     m->has_summation ? "present" : "n/a");
        }
    }

    esp_http_client_cleanup(client);
    free(body);
    return ret;
}
