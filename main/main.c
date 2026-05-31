/*
 * Buddy — main.c
 * ESP32-C3 Super Mini | 128×64 | TTP223 touch | LVGL v8
 */

#include <stdio.h>
#include "battery.h"
#include "esp_timer.h"
#include "eye_anim.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "wifi_manager.h"
#include "ntp_sync.h"
#include "weather.h"
#include "touch.h"
#include "display.h"
#include "battery.h"
#include "power_state.h"

static const char *TAG = "deskbuddy";

static void power_on_awake(void)
{
    ESP_LOGI(TAG, "Buddy awake");

    display_resume(); // later: OLED on + eye resume
}

static void power_on_sleep(void)
{
    ESP_LOGI(TAG, "Buddy sleeping");

    eye_anim_pause();
    display_suspend(); // OLED off
}

static void power_on_wake_anim(void)
{
    ESP_LOGI(TAG, "Buddy wake animation");

    eye_anim_prepare_wake_frame(); // draw closed eyes while display still off
    display_resume();              // OLED turns on already showing closed eyes
    eye_anim_play_wake();          // continue animation
}

static void power_on_sleep_anim(void)
{
    ESP_LOGI(TAG, "Buddy sleep animation");
    if (display_is_eye_screen())
    {
        display_resume();
        eye_anim_play_sleep();
        return;
    }

    /*
     * Other screens do not run eye animation,
     * so skip directly to real sleep.
     */
    buddy_power_skip_sleep_anim();
}

/* ── Forward declaration ─────────────────────────────────────────────── */
static void main_task(void *arg);

/* ── app_main ────────────────────────────────────────────────────────── */
void app_main(void)
{
    /* NVS — required by WiFi */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "%s", "=== Buddy booting ===");

    /* Init display + LVGL first so splash can show during WiFi connect */
    display_init();

    buddy_power_config_t power_cfg = {
        .sleep_after_ms = 30 * 1000,
    };

    buddy_power_callbacks_t power_cb = {
        .on_awake = power_on_awake,
        .on_sleep = power_on_sleep,
        .on_wake_anim = power_on_wake_anim,
        .on_sleep_anim = power_on_sleep_anim,
    };

    buddy_power_init(&power_cfg, &power_cb);

    /* Touch (GPIO10, TTP223 — simple digital input) */
    touch_init();

    battery_init();

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
    (void)arg; /* suppress unused-parameter warning */
    ESP_LOGI(TAG, "%s", "Main task started");

    display_reset_activity();

    while (1)
    {
        buddy_power_tick();

        /* 
         * Touch drives screen transitions inside display module.
         * esp_lvgl_port handles LVGL tick + flush in its own task.
         */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}