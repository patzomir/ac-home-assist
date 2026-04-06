#pragma once

#include "esp_err.h"
#include "zigbee_coordinator.h"
#include "ac_schedule.h"

esp_err_t nvs_store_init(void);

esp_err_t nvs_store_save_emitters(const ir_emitter_t *emitters, uint8_t count);
esp_err_t nvs_store_load_emitters(ir_emitter_t *emitters, uint8_t *count);
esp_err_t nvs_store_clear_emitters(void);

esp_err_t nvs_store_save_schedules(const ac_schedule_t *schedules, uint8_t count);
esp_err_t nvs_store_load_schedules(ac_schedule_t *schedules, uint8_t *count);
