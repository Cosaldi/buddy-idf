idf_component_register(
    SRCS
        "main.c"
        "wifi_manager.c"
        "ntp_sync.c"
        "weather.c"
        "touch.c"
        "display.c"
        "eye_anim.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        # ESP-IDF built-ins
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_http_client
        esp_http_server
        esp_lcd
        driver
        esp_driver_i2c
        log
        freertos
        # Managed components (resolved via idf_component.yml)
        espressif__esp_lvgl_port
        lvgl__lvgl
)