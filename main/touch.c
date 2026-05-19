/*
 * Buddy - touch.c
 *
 * Short press                  -> display_next_screen()
 * Long press on SCREEN_FACE    -> play random eye combo reaction
 * Long press on SCREEN_WEATHER -> toggle current weather / forecast view
 * Long press on SCREEN_CLOCK   -> WiFi setup portal
 * Long press on SCREEN_WIFI    -> stop portal and return to clock
 *
 * Uses ANYEDGE: falling = press start, rising = press end.
 * Hold duration is measured in button task. ISR stays minimal.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "display.h"
#include "touch.h"
#include "wifi_manager.h"
#include "eye_anim.h"

static const char *TAG = "touch";

/* ISR sends both edges with level info */
typedef struct
{
    uint32_t tick;
    int level;
} btn_event_t;

static QueueHandle_t s_evt_queue = NULL;

/* ── ISR — fires on both edges ───────────────────────────────────────── */
static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    (void)arg;
    btn_event_t ev = {
        .tick = (uint32_t)xTaskGetTickCountFromISR(),
        .level = gpio_get_level(BUTTON_GPIO),
    };
    xQueueSendFromISR(s_evt_queue, &ev, NULL);
}

/* ── Button task ─────────────────────────────────────────────────────── */
static void button_task(void *arg)
{
    (void)arg;

    btn_event_t ev;
    uint32_t press_tick = 0;
    bool pressing = false;
    bool long_fired = false;
    uint32_t last_tick = 0;

    const uint32_t debounce = pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS);
    const uint32_t long_ticks = pdMS_TO_TICKS(BUTTON_LONG_PRESS_MS);

    while (1)
    {
        /*
         * Wake every 20 ms even if there is no edge event.
         * This lets us detect long press while the finger is still touching.
         */
        if (xQueueReceive(s_evt_queue, &ev, pdMS_TO_TICKS(20)))
        {
            /* Debounce */
            if ((ev.tick - last_tick) < debounce)
            {
                continue;
            }
            last_tick = ev.tick;

            if (ev.level == BUTTON_ACTIVE_LEVEL)
            {
                display_reset_activity();

                if (display_is_suspended())
                {
                    display_resume(); /* just wake the screen */
                    pressing = false;
                    long_fired = false;
                    continue;
                }

                press_tick = ev.tick;
                pressing = true;
                long_fired = false;
            }
            else if (pressing)
            {
                /* Release */
                uint32_t held = ev.tick - press_tick;

                pressing = false;

                /*
                 * If long press already fired while holding,
                 * do not also trigger short press on release.
                 */
                if (long_fired)
                {
                    long_fired = false;
                    continue;
                }

                if (held >= debounce)
                {
                    /* Short press */
                    ESP_LOGI(TAG, "Short press (%lu ms)", (unsigned long)pdTICKS_TO_MS(held));

                    if (display_get_screen() == SCREEN_WIFI)
                    {
                        ESP_LOGI(TAG, "Short press ignored on WiFi setup screen");
                        continue;
                    }

                    if (display_get_screen() == SCREEN_SPLASH)
                    {
                        ESP_LOGI(TAG, "Short press ignored on splash screen");
                        continue;
                    }

                    if (display_get_screen() == SCREEN_BIRTHDAY)
                    {
                        ESP_LOGI(TAG, "Short press on birthday -> face");
                        display_set_screen(SCREEN_FACE);
                        continue;
                    }

                    display_next_screen();
                }
            }
        }

        /*
         * Long press check while still touching.
         * This fires once after BUTTON_LONG_PRESS_MS, no need to release.
         */
        if (pressing && !long_fired)
        {
            uint32_t now = (uint32_t)xTaskGetTickCount();

            if ((now - press_tick) >= long_ticks)
            {
                long_fired = true;
                display_reset_activity();

                if (display_get_screen() == SCREEN_SPLASH)
                {
                    ESP_LOGI(TAG, "Long press on splash -> birthday screen");
                    display_show_birthday();
                }
                else if (display_get_screen() == SCREEN_BIRTHDAY)
                {
                    ESP_LOGI(TAG, "Long press on birthday -> face");
                    display_set_screen(SCREEN_FACE);
                }
                else if (display_get_screen() == SCREEN_FACE)
                {
                    ESP_LOGI(TAG, "Long press on face -> random expression combo");
                    eye_anim_play_random_combo();
                }
                else if (display_get_screen() == SCREEN_WEATHER)
                {
                    ESP_LOGI(TAG, "Long press on weather -> toggle forecast");
                    display_weather_toggle_forecast();
                }
                else if (display_get_screen() == SCREEN_CLOCK)
                {
                    ESP_LOGI(TAG, "Long press on clock -> WiFi setup");
                    display_show_wifi_setup();
                    wifi_manager_start_portal();
                }
                else if (display_get_screen() == SCREEN_WIFI)
                {
                    ESP_LOGI(TAG, "Long press on WiFi setup -> exit");
                    wifi_manager_stop_portal();
                    display_set_screen(SCREEN_CLOCK);
                }
            }
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */
void touch_init(void)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = (BUTTON_ACTIVE_LEVEL == 0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (BUTTON_ACTIVE_LEVEL == 1) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE, /* both edges for hold timing */
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));

    /* Depth 10 — handles rapid taps + both edges */
    s_evt_queue = xQueueCreate(10, sizeof(btn_event_t));

    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_GPIO, gpio_isr_handler, NULL));

    /* Slightly larger stack — calls wifi_manager_start_portal() on long press */
    xTaskCreate(button_task, "btn_task", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "Button GPIO%d — short=next screen, long(2s) on clock=WiFi setup", BUTTON_GPIO);
}