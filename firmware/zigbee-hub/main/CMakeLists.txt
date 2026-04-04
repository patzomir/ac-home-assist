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
        espressif__esp-zigbee-lib
        esp_wifi
        esp_http_client
        nvs_flash
        lwip
        esp_timer
        json
)
