#pragma once

/*
 * DeskBuddy — ntp_sync.h
 * Syncs system time via SNTP, then exposes helpers to get
 * formatted time/date strings ready to print on the OLED.
 */

#include <stdbool.h>
#include <time.h>

/* NTP server pool */
#define NTP_SERVER_1  "pool.ntp.org"
#define NTP_SERVER_2  "time.google.com"

/* Timezone — change to your region's POSIX TZ string.
 * Singapore: "SGT-8"
 * US Eastern: "EST5EDT,M3.2.0,M11.1.0"
 * Central Europe: "CET-1CEST,M3.5.0,M10.5.0/3"
 */
#define NTP_TIMEZONE  "WIB-7"

/* How long to wait for first sync (ms) */
#define NTP_SYNC_TIMEOUT_MS  10000

/**
 * @brief Start SNTP client and block until time is synced (or timeout).
 *        Call after wifi_manager_init().
 */
void ntp_sync_init(void);

/**
 * @brief Returns true if time has been successfully synced at least once.
 */
bool ntp_sync_is_synced(void);

/**
 * @brief Fill buf with current time string: "HH:MM:SS"
 *        buf must be at least 9 bytes.
 */
void ntp_get_time_str(char *buf, size_t len);

/**
 * @brief Fill buf with current date string: "Mon, 09 May 2026"
 *        buf must be at least 20 bytes.
 */
void ntp_get_date_str(char *buf, size_t len);

/**
 * @brief Fill buf with short date: "09/05/26"
 *        buf must be at least 9 bytes.
 */
void ntp_get_short_date_str(char *buf, size_t len);

/**
 * @brief Get the raw broken-down local time. Returns false if not synced.
 */
bool ntp_get_tm(struct tm *out_tm);