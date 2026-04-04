#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "zigbee_coordinator.h"

/*
 * Night setback schedule:
 *   - At sleep_hour:sleep_min  → set AC to setback_temp_c
 *   - At wake_hour:wake_min - preheat_min → set AC to comfort_temp_c
 *
 * All times are local time (SNTP + timezone).
 */
typedef struct {
    uint16_t  short_addr;       /* target IR emitter */
    ac_mode_t mode;             /* HEAT / COOL */

    uint8_t   comfort_temp_c;   /* e.g. 21 */
    uint8_t   setback_temp_c;   /* e.g. 18 */

    uint8_t   sleep_hour;       /* e.g. 23 */
    uint8_t   sleep_min;        /* e.g. 0  */

    uint8_t   wake_hour;        /* e.g. 7  */
    uint8_t   wake_min;         /* e.g. 0  */

    uint8_t   preheat_min;      /* minutes before wake to start warming, e.g. 45 */
} ac_schedule_t;

esp_err_t ac_schedule_init(void);

/* Add/update schedule for a given emitter. Persisted to NVS. */
esp_err_t ac_schedule_set(const ac_schedule_t *sched);

/* Remove schedule for emitter */
esp_err_t ac_schedule_clear(uint16_t short_addr);

/* Get current schedule for emitter (returns ESP_ERR_NOT_FOUND if none) */
esp_err_t ac_schedule_get(uint16_t short_addr, ac_schedule_t *sched_out);
