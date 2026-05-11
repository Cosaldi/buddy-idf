/*
 * DeskBuddy — touch.c
 * Short press  → display_next_screen()
 * Long press (2 s) on SCREEN_CLOCK → WiFi setup portal
 *
 * Uses ANYEDGE: falling = press start, rising = press end.
 * Hold duration measured in button task — ISR is minimal.
 */

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "display.h"
#include "touch.h"
#include "wifi_manager.h"

static const char *TAG = "touch";

/* ISR sends both edges with level info */
typedef struct {
  uint32_t tick;
  int level;
} btn_event_t;

static QueueHandle_t s_evt_queue = NULL;

/* ── ISR — fires on both edges ───────────────────────────────────────── */
static void IRAM_ATTR gpio_isr_handler(void *arg) {
  (void)arg;
  btn_event_t ev = {
      .tick = (uint32_t)xTaskGetTickCountFromISR(),
      .level = gpio_get_level(BUTTON_GPIO),
  };
  xQueueSendFromISR(s_evt_queue, &ev, NULL);
}

/* ── Button task ─────────────────────────────────────────────────────── */
static void button_task(void *arg) {
  (void)arg;

  btn_event_t ev;
  uint32_t press_tick = 0;
  bool pressing = false;
  uint32_t last_tick = 0;

  const uint32_t debounce = pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS);
  const uint32_t long_ms = pdMS_TO_TICKS(BUTTON_LONG_PRESS_MS);

  while (1) {
    if (!xQueueReceive(s_evt_queue, &ev, portMAX_DELAY))
      continue;

    /* Debounce */
    if ((ev.tick - last_tick) < debounce)
      continue;
    last_tick = ev.tick;

    if (ev.level == BUTTON_ACTIVE_LEVEL) {
      /* Press start */
      press_tick = ev.tick;
      pressing = true;

    } else if (pressing) {
      /* Press end — measure hold duration */
      pressing = false;
      uint32_t held = ev.tick - press_tick;

      if (held >= long_ms) {
        if (display_get_screen() == SCREEN_CLOCK) {
          /* From clock -> enter WiFi setup and start SoftAP */
          ESP_LOGI(TAG, "Long press on clock -> WiFi setup");
          display_show_wifi_setup();
          wifi_manager_start_portal();

        } else if (display_get_screen() == SCREEN_WIFI) {
          /* From WiFi setup -> stop SoftAP and return to clock */
          ESP_LOGI(TAG, "Long press on WiFi setup -> exit");
          wifi_manager_stop_portal();
          display_set_screen(SCREEN_CLOCK);
        }
      } else if (held >= debounce) {
        /* ── Short press ── */
        ESP_LOGI(TAG, "Short press (%lu ms)", (unsigned long)held);

        /* Ignore short press while WiFi setup screen is shown */
        if (display_get_screen() == SCREEN_WIFI) {
          ESP_LOGI(TAG, "Short press ignored on WiFi setup screen");
          continue;
        }

        /* Normal behavior for other screens */
        display_next_screen();
      }
    }
  }
}

/* ── Public API ──────────────────────────────────────────────────────── */
void touch_init(void) {
  gpio_config_t io_cfg = {
      .pin_bit_mask = (1ULL << BUTTON_GPIO),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en =
          (BUTTON_ACTIVE_LEVEL == 0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
      .pull_down_en = (BUTTON_ACTIVE_LEVEL == 1) ? GPIO_PULLDOWN_ENABLE
                                                 : GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_ANYEDGE, /* both edges for hold timing */
  };
  ESP_ERROR_CHECK(gpio_config(&io_cfg));

  /* Depth 10 — handles rapid taps + both edges */
  s_evt_queue = xQueueCreate(10, sizeof(btn_event_t));

  ESP_ERROR_CHECK(gpio_install_isr_service(0));
  ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_GPIO, gpio_isr_handler, NULL));

  /* Slightly larger stack — calls wifi_manager_start_portal() on long press */
  xTaskCreate(button_task, "btn_task", 3072, NULL, 3, NULL);

  ESP_LOGI(TAG,
           "Button GPIO%d — short=next screen, long(2s) on clock=WiFi setup",
           BUTTON_GPIO);
}