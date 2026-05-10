#pragma once

/*
 * DeskBuddy — weather.h
 * Fetches current weather from OpenWeatherMap One Call API.
 * Updates the display weather screen every WEATHER_UPDATE_INTERVAL_S seconds.
 *
 * Sign up free at https://openweathermap.org/api → "Current Weather Data"
 * Free tier: 60 calls/min, plenty for a desk gadget.
 */

#include <stdbool.h>

/* ── Config — fill these in ──────────────────────────────────────────── */
#define WEATHER_API_KEY       "7fa80959ee0eca9c0fbd20277ee3276d"
#define WEATHER_CITY          "Bandung"
#define WEATHER_COUNTRY_CODE  "ID"
#define WEATHER_UNITS         "metric"          /* metric=°C, imperial=°F */
#define WEATHER_LANG          "en"

/* How often to re-fetch (seconds). Free OWM updates every 10 min. */
#define WEATHER_UPDATE_INTERVAL_S  600

/* ── Last fetched data (read-only) ───────────────────────────────────── */
typedef struct {
    float temp_c;
    float feels_like_c;
    int   humidity;
    char  condition[32];   /* e.g. "Clouds", "Rain", "Clear" */
    char  description[48]; /* e.g. "scattered clouds"        */
    bool  valid;           /* false until first fetch succeeds */
} weather_data_t;

/**
 * @brief Start the weather fetch task. Call after wifi_manager_init().
 *        Fetches immediately then repeats every WEATHER_UPDATE_INTERVAL_S.
 */
void weather_init(void);

/**
 * @brief Get a copy of the last successfully fetched weather data.
 *        Returns false if no valid data yet.
 */
bool weather_get(weather_data_t *out);