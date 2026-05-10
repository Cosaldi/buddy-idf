/*
 * DeskBuddy — main.c
 * ESP32-C3 Super Mini | SSD1306 128×64 | TTP223 touch | LVGL v8
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "wifi_manager.h"
#include "ntp_sync.h"
#include "weather.h"
#include "touch.h"
#include "display.h"

static const char *TAG = "deskbuddy";

/* ── Forward declaration ─────────────────────────────────────────────── */
static void main_task(void *arg);

/* ── app_main ────────────────────────────────────────────────────────── */
void app_main(void)
{
    /* NVS — required by WiFi */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "%s", "=== DeskBuddy booting ===");

    /* Init display + LVGL first so splash can show during WiFi connect */
    display_init();

    /* Touch (GPIO10, TTP223 — simple digital input) */
    touch_init();

    /* WiFi — blocks until connected or times out */
    wifi_manager_init();

    /* NTP — sync time after WiFi is up */
    ntp_sync_init();

    /* Weather — initial fetch; subsequent fetches on a timer */
    weather_init();

    /* Hand off to main UI task */
    xTaskCreate(main_task, "main_task", 8192, NULL, 5, NULL);
}

/* ── Main UI task ────────────────────────────────────────────────────── */
static void main_task(void *arg)
{
    (void)arg;  /* suppress unused-parameter warning */
    ESP_LOGI(TAG, "%s", "Main task started");

    while (1) {
        /* Touch drives screen transitions (handled inside display module
         * via touch_get_event()). We just keep the task alive and let
         * esp_lvgl_port handle the LVGL tick + flush in its own task. */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}