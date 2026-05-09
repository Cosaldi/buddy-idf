/*
 * DeskBuddy — touch.c
 * Debounced push button via GPIO interrupt.
 * On valid press → calls display_next_screen().
 *
 * Swap BUTTON_ACTIVE_LEVEL to 1 and BUTTON_GPIO to 10
 * when switching back to TTP223 on the C3 Super Mini.
 */

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "touch.h"
#include "display.h"

static const char *TAG = "touch";

/* Queue: ISR sends a tick count, task checks debounce */
static QueueHandle_t s_evt_queue = NULL;

/* ── ISR — fires on active edge ──────────────────────────────────────── */
static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    (void)arg;
    uint32_t tick = (uint32_t)xTaskGetTickCountFromISR();
    xQueueSendFromISR(s_evt_queue, &tick, NULL);
}

/* ── Debounce task ───────────────────────────────────────────────────── */
static void button_task(void *arg)
{
    (void)arg;
    uint32_t tick_received;
    uint32_t last_tick = 0;
    const uint32_t debounce_ticks = pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS);

    while (1) {
        if (xQueueReceive(s_evt_queue, &tick_received, portMAX_DELAY)) {

            /* Ignore if within debounce window */
            if ((tick_received - last_tick) < debounce_ticks) {
                continue;
            }
            last_tick = tick_received;

            /* Confirm pin is still at active level (not a spike) */
            int level = gpio_get_level(BUTTON_GPIO);
            if (level != BUTTON_ACTIVE_LEVEL) {
                continue;
            }

            ESP_LOGI(TAG, "%s", "Button pressed → next screen");
            display_next_screen();
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */
void touch_init(void)
{
    /* Configure GPIO */
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = (BUTTON_ACTIVE_LEVEL == 0)
                            ? GPIO_PULLUP_ENABLE
                            : GPIO_PULLUP_DISABLE,
        .pull_down_en = (BUTTON_ACTIVE_LEVEL == 1)
                            ? GPIO_PULLDOWN_ENABLE
                            : GPIO_PULLDOWN_DISABLE,
        .intr_type    = (BUTTON_ACTIVE_LEVEL == 0)
                            ? GPIO_INTR_NEGEDGE   /* LOW active: falling edge */
                            : GPIO_INTR_POSEDGE,  /* HIGH active: rising edge */
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));

    /* Event queue — depth 5 handles rapid taps without dropping */
    s_evt_queue = xQueueCreate(5, sizeof(uint32_t));

    /* Install ISR service and attach handler */
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(
        BUTTON_GPIO, gpio_isr_handler, NULL));

    /* Debounce task — low priority, small stack */
    xTaskCreate(button_task, "btn_task", 2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "Button ready on GPIO%d (active %s)",
             BUTTON_GPIO,
             BUTTON_ACTIVE_LEVEL == 0 ? "LOW" : "HIGH");
}