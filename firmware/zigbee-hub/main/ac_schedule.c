#include "ac_schedule.h"
#include "nvs_store.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <time.h>

static const char *TAG = "schedule";

#define MAX_SCHEDULES   4
#define TICK_PERIOD_MS  30000   /* check schedule every 30 s */

static ac_schedule_t s_schedules[MAX_SCHEDULES];
static uint8_t       s_count = 0;
static esp_timer_handle_t s_timer;

/* ---- helpers ------------------------------------------------------------ */

static int find_schedule(uint16_t short_addr)
{
    for (int i = 0; i < s_count; i++) {
        if (s_schedules[i].short_addr == short_addr) return i;
    }
    return -1;
}

/*
 * Compute "minutes since midnight" for a given hour:minute, then subtract
 * preheat_min to get the preheat trigger time. Handle midnight wrap.
 */
static uint16_t to_minutes(uint8_t h, uint8_t m) { return h * 60 + m; }

static void apply_schedule(const ac_schedule_t *s, uint16_t now_min)
{
    uint16_t sleep_min   = to_minutes(s->sleep_hour,  s->sleep_min);
    uint16_t wake_min    = to_minutes(s->wake_hour,   s->wake_min);
    uint16_t preheat_min = (wake_min + 1440 - s->preheat_min) % 1440;

    /* Determine target state for current minute */
    const char *reason = NULL;
    int8_t  target_temp = s->comfort_temp_c;
    ac_mode_t mode = s->mode;

    /* Night window: sleep_min → preheat_min */
    bool in_night;
    if (sleep_min < preheat_min) {
        in_night = (now_min >= sleep_min && now_min < preheat_min);
    } else {
        /* Wraps midnight */
        in_night = (now_min >= sleep_min || now_min < preheat_min);
    }

    if (in_night) {
        target_temp = s->setback_temp_c;
        reason = "night setback";
    } else if (now_min == preheat_min) {
        target_temp = s->comfort_temp_c;
        reason = "preheat";
    } else {
        reason = "comfort";
    }

    if (reason) {
        ESP_LOGI(TAG, "addr=0x%04x → %s: %d°C", s->short_addr, reason, target_temp);
        zb_coordinator_send_setpoint(s->short_addr, target_temp, mode);
    }
}

static void schedule_tick(void *arg)
{
    time_t now = time(NULL);
    struct tm local_tm;
    localtime_r(&now, &local_tm);
    uint16_t now_min = to_minutes(local_tm.tm_hour, local_tm.tm_min);

    for (int i = 0; i < s_count; i++) {
        apply_schedule(&s_schedules[i], now_min);
    }
}

/* ---- Public API --------------------------------------------------------- */

esp_err_t ac_schedule_init(void)
{
    /* Load persisted schedules */
    s_count = MAX_SCHEDULES;
    nvs_store_load_schedules(s_schedules, &s_count);
    ESP_LOGI(TAG, "Loaded %d schedule(s) from NVS", s_count);

    /* Periodic timer */
    esp_timer_create_args_t timer_cfg = {
        .callback = schedule_tick,
        .name     = "sched_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_cfg, &s_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_timer,
        (uint64_t)TICK_PERIOD_MS * 1000));

    return ESP_OK;
}

esp_err_t ac_schedule_set(const ac_schedule_t *sched)
{
    int idx = find_schedule(sched->short_addr);
    if (idx < 0) {
        if (s_count >= MAX_SCHEDULES) return ESP_ERR_NO_MEM;
        idx = s_count++;
    }
    s_schedules[idx] = *sched;
    nvs_store_save_schedules(s_schedules, s_count);
    ESP_LOGI(TAG, "Schedule set for 0x%04x: sleep %02d:%02d setback=%d°C, "
                  "wake %02d:%02d comfort=%d°C preheat=%dmin",
             sched->short_addr,
             sched->sleep_hour, sched->sleep_min, sched->setback_temp_c,
             sched->wake_hour,  sched->wake_min,  sched->comfort_temp_c,
             sched->preheat_min);
    return ESP_OK;
}

esp_err_t ac_schedule_clear(uint16_t short_addr)
{
    int idx = find_schedule(short_addr);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    /* Remove by swapping with last */
    s_schedules[idx] = s_schedules[--s_count];
    nvs_store_save_schedules(s_schedules, s_count);
    return ESP_OK;
}

esp_err_t ac_schedule_get(uint16_t short_addr, ac_schedule_t *out)
{
    int idx = find_schedule(short_addr);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    *out = s_schedules[idx];
    return ESP_OK;
}
