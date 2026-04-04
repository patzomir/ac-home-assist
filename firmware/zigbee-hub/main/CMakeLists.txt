idf_component_register(
    SRCS
        "main.c"
        "zigbee_coordinator.c"
        "wifi_manager.c"
        "ac_schedule.c"
        "http_reporter.c"
        "nvs_store.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_zigbee_core
        esp_wifi
        esp_http_client
        nvs_flash
        esp_sntp
        esp_timer
        json
)
