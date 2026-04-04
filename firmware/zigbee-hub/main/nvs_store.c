#include "nvs_store.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG      = "nvs";
static const char *NVS_NS   = "hub";
static const char *KEY_EMIT = "emitters";
static const char *KEY_SCHED = "schedules";

esp_err_t nvs_store_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition erased and re-initialised");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

esp_err_t nvs_store_save_emitters(const ir_emitter_t *emitters, uint8_t count)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_blob(h, KEY_EMIT, emitters,
                       count * sizeof(ir_emitter_t));
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

esp_err_t nvs_store_load_emitters(ir_emitter_t *emitters, uint8_t *count)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (ret != ESP_OK) { *count = 0; return ret; }

    size_t len = (*count) * sizeof(ir_emitter_t);
    ret = nvs_get_blob(h, KEY_EMIT, emitters, &len);
    nvs_close(h);

    if (ret == ESP_OK) {
        *count = (uint8_t)(len / sizeof(ir_emitter_t));
        ESP_LOGI(TAG, "Loaded %d emitter(s) from NVS", *count);
    } else {
        *count = 0;
    }
    return ret;
}

esp_err_t nvs_store_save_schedules(const ac_schedule_t *schedules, uint8_t count)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_blob(h, KEY_SCHED, schedules,
                       count * sizeof(ac_schedule_t));
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

esp_err_t nvs_store_load_schedules(ac_schedule_t *schedules, uint8_t *count)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (ret != ESP_OK) { *count = 0; return ret; }

    size_t len = (*count) * sizeof(ac_schedule_t);
    ret = nvs_get_blob(h, KEY_SCHED, schedules, &len);
    nvs_close(h);

    if (ret == ESP_OK) {
        *count = (uint8_t)(len / sizeof(ac_schedule_t));
        ESP_LOGI(TAG, "Loaded %d schedule(s) from NVS", *count);
    } else {
        *count = 0;
    }
    return ret;
}
