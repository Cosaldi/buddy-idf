#pragma once
/*
 * DeskBuddy — display.h
 * LVGL v8 display driver for SSD1306 128×64 via esp_lcd + esp_lvgl_port.
 *
 * Screens (cycled by button press):
 *   0 — Face     (Akno-style animated eyes)
 *   1 — Clock    (HH:MM:SS + date)
 *   2 — Weather  (temp + condition)
 *   3 — WiFi     (setup portal info — shown on long press clock)
 */
#include <stdint.h>
#include <stdbool.h>

/* SSD1306 hardware config */
#define DISPLAY_SDA_GPIO   3
#define DISPLAY_SCL_GPIO   4
#define DISPLAY_I2C_ADDR   0x3C
#define DISPLAY_WIDTH      128
#define DISPLAY_HEIGHT     64
#define DISPLAY_I2C_CLK_HZ 100000

/* Screen indices */
typedef enum {
    SCREEN_FACE    = 0,
    SCREEN_CLOCK   = 1,
    SCREEN_WEATHER = 2,
    SCREEN_WIFI    = 3,
    SCREEN_COUNT
} display_screen_t;

/** @brief Initialise I2C, SSD1306, LVGL, start eye animation. */
void display_init(void);

/** @brief Advance to next screen (wraps, skips SCREEN_WIFI). */
void display_next_screen(void);

/** @brief Force-switch to a specific screen. */
void display_set_screen(display_screen_t screen);

/** @brief Returns the currently active screen. */
display_screen_t display_get_screen(void);

/**
 * @brief Show WiFi setup screen (AP name + instructions).
 *        Called by touch.c on long press of clock screen.
 */
void display_show_wifi_setup(void);

/**
 * @brief Cycle eye expression while on SCREEN_FACE.
 *        Normal -> Happy -> Angry -> Sleepy -> Surprised -> Normal ...
 */
void display_face_next_expression(void);

/** @brief Update weather data. Safe to call from any task. */
void display_update_weather(const char *condition, float temp_c);

/** @brief Tick -- only needed if NOT using esp_lvgl_port auto-tick. */
void display_tick(void);

/** @brief Reset the idle activity timer (call on any user input). */
void display_reset_activity(void);

/** @brief Turn off display panel (idle timeout). */
void display_suspend(void);

/** @brief Turn on display panel and reset activity timer. */
void display_resume(void);

/** @brief Get last activity timestamp in ms. */
uint32_t display_get_last_activity_ms(void);

/** @brief Returns true if display is currently suspended. */
bool display_is_suspended(void);