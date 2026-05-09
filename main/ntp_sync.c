/*
 * DeskBuddy — ntp_sync.c
 * SNTP client + time formatting helpers for the OLED display.
 */

#include <stdint.h>
#include <stdlib.h>   /* setenv, tzset */
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_sntp.h"

#include "ntp_sync.h"

static const char *TAG = "ntp_sync";

static volatile bool s_synced = false;

/* ── SNTP sync callback ──────────────────────────────────────────────── */
static void sntp_sync_notification(struct timeval *tv)
{
    (void)tv;
    s_synced = true;

    /* Log the synced time for debugging on serial monitor */
    char time_buf[32];
    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "Time synced: %s (%s)", time_buf, NTP_TIMEZONE);
}

/* ── Public API ──────────────────────────────────────────────────────── */
void ntp_sync_init(void)
{
    /* Set timezone before sync so localtime_r returns local time */
    setenv("TZ", NTP_TIMEZONE, 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER_1);
    esp_sntp_setservername(1, NTP_SERVER_2);
    esp_sntp_set_time_sync_notification_cb(sntp_sync_notification);
    esp_sntp_init();

    ESP_LOGI(TAG, "Waiting for NTP sync (servers: %s, %s)...",
             NTP_SERVER_1, NTP_SERVER_2);

    /* Block until synced or timeout */
    uint32_t elapsed = 0;
    while (!s_synced && elapsed < NTP_SYNC_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(200));
        elapsed += 200;
    }

    if (s_synced) {
        ESP_LOGI(TAG, "%s", "NTP sync successful");
    } else {
        ESP_LOGW(TAG, "%s", "NTP sync timed out — clock may be wrong");
    }
}

bool ntp_sync_is_synced(void)
{
    return s_synced;
}

bool ntp_get_tm(struct tm *out_tm)
{
    if (!out_tm) return false;
    time_t now = time(NULL);
    localtime_r(&now, out_tm);
    return s_synced;
}

/* ── Formatting helpers ──────────────────────────────────────────────── */

void ntp_get_time_str(char *buf, size_t len)
{
    struct tm t;
    time_t now = time(NULL);
    localtime_r(&now, &t);
    strftime(buf, len, "%H:%M:%S", &t);
}

void ntp_get_date_str(char *buf, size_t len)
{
    struct tm t;
    time_t now = time(NULL);
    localtime_r(&now, &t);
    strftime(buf, len, "%a, %d %b %Y", &t);
}

void ntp_get_short_date_str(char *buf, size_t len)
{
    struct tm t;
    time_t now = time(NULL);
    localtime_r(&now, &t);
    strftime(buf, len, "%d/%m/%y", &t);
}