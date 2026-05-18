#pragma once

/*
 * Buddy - weather.h
 *
 * Current weather + 3-slot forecast cache.
 *
 * Current weather:
 *   /data/2.5/weather
 *
 * Forecast:
 *   /data/2.5/forecast
 *   OpenWeather forecast gives 3-hour slots.
 */

#include <stdbool.h>
#include <time.h>

/* -------------------------------------------------------------------------- */
/* Config                                                                     */
/* -------------------------------------------------------------------------- */

#ifndef WEATHER_API_KEY
#define WEATHER_API_KEY ""
#endif

#define WEATHER_CITY          "Bandung"
#define WEATHER_COUNTRY_CODE  "ID"
#define WEATHER_UNITS         "metric"
#define WEATHER_LANG          "en"

/* How often to re-fetch (seconds). Free OWM updates every 10 min. */
#define WEATHER_UPDATE_INTERVAL_S 600

/* Show next 3 future forecast slots, e.g. 21:00, 00:00, 03:00 */
#define WEATHER_FORECAST_MAX 3


/* -------------------------------------------------------------------------- */
/* Data types                                                                 */
/* -------------------------------------------------------------------------- */

typedef struct {
    float temp_c;
    float feels_like_c;
    int   humidity;
    char  condition[32];
    char  description[48];
    bool  valid;
} weather_data_t;

typedef struct {
    time_t timestamp;
    char   time[8];        /* "21:00" */
    char   condition[32];  /* "Clouds", "Rain", "Clear" */
    float  temp_c;
    bool   valid;
} weather_forecast_item_t;

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

int weather_get_forecast_count(void);

bool weather_get_forecast(int index, weather_forecast_item_t *out);