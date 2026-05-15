/*
 * DeskBuddy — weather.c
 * Polls OpenWeatherMap current weather API and updates the display.
 *
 * API used:
 *   GET https://api.openweathermap.org/data/2.5/weather
 *       ?q={city},{country}&units=metric&lang=en&appid={key}
 *
 * Relevant JSON fields parsed:
 *   weather[0].main        → condition  (e.g. "Clouds")
 *   weather[0].description → description (e.g. "scattered clouds")
 *   main.temp              → temp_c
 *   main.feels_like        → feels_like_c
 *   main.humidity          → humidity
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_http_client.h"

#include "wifi_manager.h"
#include "display.h"
#include "weather.h"

static const char *TAG = "weather";

/* ── Internal state ──────────────────────────────────────────────────── */
static weather_data_t s_data = {0};

/* HTTP response accumulation buffer */
#define HTTP_BUF_SIZE 2048
static char s_http_buf[HTTP_BUF_SIZE];
static int s_http_len = 0;

/* ── Build API URL ───────────────────────────────────────────────────── */
static void build_url(char *buf, size_t len)
{
    snprintf(buf,
             len,
             "http://api.openweathermap.org/data/2.5/weather"
             "?q=%s,%s&units=%s&lang=%s&appid=%s",
             WEATHER_CITY,
             WEATHER_COUNTRY_CODE,
             WEATHER_UNITS,
             WEATHER_LANG,
             WEATHER_API_KEY);
}

/* ── HTTP event handler ──────────────────────────────────────────────── */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
    case HTTP_EVENT_ON_CONNECTED:
        s_http_len = 0;
        memset(s_http_buf, 0, sizeof(s_http_buf));
        break;

    case HTTP_EVENT_ON_DATA:
        if (!esp_http_client_is_chunked_response(evt->client))
        {
            int copy = evt->data_len;
            if (s_http_len + copy >= HTTP_BUF_SIZE)
            {
                copy = HTTP_BUF_SIZE - s_http_len - 1;
            }
            if (copy > 0)
            {
                memcpy(s_http_buf + s_http_len, evt->data, copy);
                s_http_len += copy;
                s_http_buf[s_http_len] = '\0';
            }
        }
        break;

    default:
        break;
    }
    return ESP_OK;
}

/* ── Minimal JSON field extractor ────────────────────────────────────── */
/* Finds "key":"value" or "key":number in a flat/nested JSON string.     */

static bool json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    /* Build search pattern: "key":" */
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(json, pattern);
    if (!p)
        return false;
    p += strlen(pattern);
    const char *end = strchr(p, '"');
    if (!end)
        return false;
    size_t len = (size_t)(end - p);
    if (len >= out_len)
        len = out_len - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

static bool json_get_float(const char *json, const char *key, float *out)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p)
        return false;
    p += strlen(pattern);
    *out = strtof(p, NULL);
    return true;
}

static bool json_get_int(const char *json, const char *key, int *out)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p)
        return false;
    p += strlen(pattern);
    *out = (int)strtol(p, NULL, 10);
    return true;
}

/* ── Parse JSON response ─────────────────────────────────────────────── */
static bool parse_weather(const char *json, weather_data_t *out)
{
    if (!json || strlen(json) < 10)
    {
        ESP_LOGE(TAG, "%s", "Empty JSON response");
        return false;
    }

    ESP_LOGI(TAG, "Parsing: %.120s", json);

    /* Check for API error */
    if (strstr(json, "\"cod\":401") || strstr(json, "\"cod\": 401"))
    {
        ESP_LOGE(TAG, "%s", "OWM: Invalid API key");
        return false;
    }

    bool ok = true;

    /* temp, feels_like, humidity — all inside "main":{...} */
    ok &= json_get_float(json, "temp", &out->temp_c);
    ok &= json_get_float(json, "feels_like", &out->feels_like_c);
    json_get_int(json, "humidity", &out->humidity); /* optional */

    /* weather[0].main and .description — first occurrence in array */
    if (!json_get_string(json, "main", out->condition, sizeof(out->condition)))
    {
        strncpy(out->condition, "N/A", sizeof(out->condition));
    }
    if (!json_get_string(json, "description", out->description, sizeof(out->description)))
    {
        strncpy(out->description, "", sizeof(out->description));
    }

    out->valid = ok;
    return ok;
}

/* ── Fetch one weather update ────────────────────────────────────────── */
static void fetch_weather(void)
{
    if (!wifi_manager_is_connected())
    {
        ESP_LOGW(TAG, "%s", "WiFi not connected, skipping fetch");
        return;
    }

    char url[256];
    build_url(url, sizeof(url));

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 10000,
        .buffer_size = 512,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
        ESP_LOGE(TAG, "%s", "HTTP client init failed");
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "HTTP request failed: %d — check WiFi/DNS", err);
        return;
    }
    if (status != 200)
    {
        ESP_LOGW(TAG, "OWM returned HTTP %d — response: %s", status, s_http_buf);
        return;
    }

    ESP_LOGI(TAG, "Response (%d bytes): %.80s...", s_http_len, s_http_buf);

    weather_data_t fresh = {0};
    if (parse_weather(s_http_buf, &fresh))
    {
        s_data = fresh;
        ESP_LOGI(TAG,
                 "Weather: %s %.1f C (feels %.1f C) hum %d%%",
                 s_data.condition,
                 s_data.temp_c,
                 s_data.feels_like_c,
                 s_data.humidity);

        /* Push to display. display_update_weather() handles LVGL locking. */
        display_update_weather(s_data.condition, s_data.temp_c);
    }
    else
    {
        ESP_LOGW(TAG, "%s", "Failed to parse weather JSON");
    }
}

/* ── Weather task ────────────────────────────────────────────────────── */
static void weather_task(void *arg)
{
    (void)arg;

    /* First fetch immediately after boot */
    fetch_weather();

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(WEATHER_UPDATE_INTERVAL_S * 1000));
        fetch_weather();
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */
void weather_init(void)
{
    xTaskCreate(weather_task, "weather_task", 8192, NULL, 4, NULL);
    ESP_LOGI(TAG,
             "Weather task started (city: %s, interval: %ds)",
             WEATHER_CITY,
             WEATHER_UPDATE_INTERVAL_S);
}

bool weather_get(weather_data_t *out)
{
    if (!out || !s_data.valid)
        return false;
    *out = s_data;
    return true;
}