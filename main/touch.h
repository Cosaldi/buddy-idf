#pragma once

/*
 * DeskBuddy — touch.h
 * Push button driver (active LOW) — drop-in for TTP223 touch sensor.
 * Short press → display_next_screen()
 * Long press (2s) on SCREEN_CLOCK → wifi_manager_start_portal()
 *
 * DevKit:  GPIO0  (BOOT button — already has 10k pullup on board)
 * C3 Mini: GPIO10 (TTP223, active HIGH — change BUTTON_ACTIVE_LEVEL)
 */

/* ── Hardware config ─────────────────────────────────────────────────── */
#define BUTTON_GPIO           0      /* GPIO0 = BOOT button on DevKit     */
#define BUTTON_ACTIVE_LEVEL   0      /* 0 = active LOW (push button)      */
                                     /* 1 = active HIGH (TTP223)          */
// #define BUTTON_GPIO          10   // TTP223 pin
// #define BUTTON_ACTIVE_LEVEL   1   // TTP223 is active HIGH
#define BUTTON_DEBOUNCE_MS    50     /* ignore bounces shorter than this  */
#define BUTTON_LONG_PRESS_MS  2000   /* hold 2 s → long press             */

/**
 * @brief Configure GPIO and install ISR. Starts internal debounce task.
 *        Short press  → display_next_screen()
 *        Long press on SCREEN_CLOCK → display_show_wifi_setup() +
 *                                     wifi_manager_start_portal()
 *        Call once after display_init().
 */
void touch_init(void);