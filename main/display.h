#pragma once
/*
 * DeskBuddy — display.h
 * LVGL v8 display driver for SSD1306 128×64 via esp_lcd + esp_lvgl_port.
 *
 * Screens (cycled by button press):
 *   0 — Face     (Akno-style animated eyes)
 *   1 — Clock    (HH:MM:SS + date)
 *   2 — Weather  (temp + condition)
 */
#include <stdint.h>

/* SSD1306 hardware config */
#define DISPLAY_SDA_GPIO   3
#define DISPLAY_SCL_GPIO   4
#define DISPLAY_I2C_ADDR   0x3C
#define DISPLAY_WIDTH      128
#define DISPLAY_HEIGHT     64
#define DISPLAY_I2C_CLK_HZ 400000

/* Screen indices */
typedef enum {
    SCREEN_FACE    = 0,
    SCREEN_CLOCK   = 1,
    SCREEN_WEATHER = 2,
    SCREEN_COUNT
} display_screen_t;

/** @brief Initialise I2C, SSD1306, LVGL, start eye animation. */
void display_init(void);

/** @brief Advance to next screen (wraps). Call from button handler. */
void display_next_screen(void);

/** @brief Force-switch to a specific screen. */
void display_set_screen(display_screen_t screen);

/**
 * @brief Cycle eye expression while on SCREEN_FACE.
 *        Normal → Happy → Angry → Sleepy → Surprised → Normal ...
 *
 * Suggested button logic:
 *   if (current_screen == SCREEN_FACE)
 *       display_face_next_expression();
 *   else
 *       display_next_screen();
 */
void display_face_next_expression(void);

/** @brief Update weather data. Safe to call from any task. */
void display_update_weather(const char *condition, float temp_c);

/** @brief Tick — only needed if NOT using esp_lvgl_port auto-tick. */
void display_tick(void);