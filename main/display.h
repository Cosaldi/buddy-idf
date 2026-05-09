#pragma once

/*
 * DeskBuddy — display.h
 * LVGL v8 display driver for SSD1306 128×64 via esp_lcd + esp_lvgl_port.
 * Handles screen init, flush, and all UI screens.
 *
 * Screens (cycled by button press):
 *   0 — Idle face (smiley)
 *   1 — Reaction  (random emoji on tap)
 *   2 — Clock     (HH:MM:SS + date)
 *   3 — Weather   (temp + condition, updated by weather module)
 */

#include <stdint.h>

/* SSD1306 hardware config */
#define DISPLAY_SDA_GPIO   3
#define DISPLAY_SCL_GPIO   4
#define DISPLAY_I2C_ADDR   0x3C
#define DISPLAY_WIDTH      128
#define DISPLAY_HEIGHT     64
#define DISPLAY_I2C_CLK_HZ 400000   /* 400 kHz fast-mode */

/* Screen indices */
typedef enum {
    SCREEN_FACE    = 0,
    SCREEN_REACT   = 1,
    SCREEN_CLOCK   = 2,
    SCREEN_WEATHER = 3,
    SCREEN_COUNT
} display_screen_t;

/**
 * @brief Initialise I2C, SSD1306, LVGL, and draw the idle face screen.
 *        Must be called before any other display_* function.
 */
void display_init(void);

/**
 * @brief Advance to the next screen (wraps around).
 *        Call this from the button ISR or button task.
 */
void display_next_screen(void);

/**
 * @brief Force-switch to a specific screen.
 */
void display_set_screen(display_screen_t screen);

/**
 * @brief Update the weather data shown on SCREEN_WEATHER.
 *        Safe to call from any task — posts to LVGL task via lv_async_call.
 */
void display_update_weather(const char *condition, float temp_c);

/**
 * @brief Tick — call from a 1 ms FreeRTOS timer or let esp_lvgl_port handle it.
 *        Only needed if NOT using esp_lvgl_port automatic tick.
 */
void display_tick(void);