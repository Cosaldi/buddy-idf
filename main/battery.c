/* -------------------------------------------------------------------------- */
/* Buddy - battery.c                                                          */
/*                                                                            */
/* Battery voltage reading and simple LOW/MID/HIGH level detection.            */
/* -------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------- */
/* Includes                                                                   */
/* -------------------------------------------------------------------------- */

#include "battery.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* -------------------------------------------------------------------------- */
/* Defines / constants                                                        */
/* -------------------------------------------------------------------------- */

#define BATTERY_ADC_UNIT ADC_UNIT_1
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_1 /* GPIO1 on ESP32-C3 */
#define BATTERY_ADC_ATTEN ADC_ATTEN_DB_12

#define BATTERY_DIVIDER_FACTOR 2.00f
#define BATTERY_CALIBRATION 0.90f

#define BATTERY_HIGH_VOLTAGE 3.90f
#define BATTERY_MID_VOLTAGE 3.55f

/* -------------------------------------------------------------------------- */
/* Static variables                                                           */
/* -------------------------------------------------------------------------- */

static const char *TAG = "battery";

static adc_oneshot_unit_handle_t s_adc_handle = NULL;

/* -------------------------------------------------------------------------- */
/* ADC helpers                                                                */
/* -------------------------------------------------------------------------- */

/*
 * Read raw ADC value and convert it into approximate battery voltage.
 *
 * Battery divider:
 * BAT+ --- R1 --- ADC --- R2 --- GND
 *
 * Example:
 * R1 = 100k
 * R2 = 100k
 *
 * ADC voltage = battery voltage / 2
 * Battery voltage = ADC voltage * 2
 */
static float read_battery_voltage(void)
{
    if (!s_adc_handle) {
        return 0.0f;
    }

    int raw = 0;

    esp_err_t err = adc_oneshot_read(s_adc_handle,
                                     BATTERY_ADC_CHANNEL,
                                     &raw);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC read failed: %s", esp_err_to_name(err));
        return 0.0f;
    }

    /*
     * ESP32-C3 ADC approximation.
     *
     * Your real measurement:
     * Battery = 4.00V
     * ADC pin = 1.98V
     *
     * Divider is correct, but ADC formula reads high,
     * so calibration is needed.
     */
    float adc_voltage = ((float)raw / 4095.0f) * 3.3f;

    float battery_voltage =
        adc_voltage * BATTERY_DIVIDER_FACTOR * BATTERY_CALIBRATION;

    // ESP_LOGI(TAG,
    //          "raw=%d adc=%.2f bat=%.2f",
    //          raw,
    //          adc_voltage,
    //          battery_voltage);

    return battery_voltage;
}

/* -------------------------------------------------------------------------- */
/* Battery level helpers                                                      */
/* -------------------------------------------------------------------------- */

static battery_level_t voltage_to_level(float vbat)
{
    if (vbat >= BATTERY_HIGH_VOLTAGE)
    {
        return BATTERY_LEVEL_HIGH;
    }

    if (vbat >= BATTERY_MID_VOLTAGE)
    {
        return BATTERY_LEVEL_MID;
    }

    return BATTERY_LEVEL_LOW;
}

static const char *level_to_text(battery_level_t level)
{
    switch (level)
    {
    case BATTERY_LEVEL_HIGH:
        return "BAT:H";

    case BATTERY_LEVEL_MID:
        return "BAT:M";

    case BATTERY_LEVEL_LOW:
    default:
        return "BAT:L";
    }
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BATTERY_ADC_UNIT,
    };

    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = BATTERY_ADC_ATTEN,
    };

    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, BATTERY_ADC_CHANNEL, &chan_cfg));

    ESP_LOGI(TAG, "Battery ADC initialized");
}

float battery_get_voltage(void)
{
    return read_battery_voltage();
}

battery_level_t battery_get_level(void)
{
    return voltage_to_level(read_battery_voltage());
}

const char *battery_get_level_text(void)
{
    return level_to_text(battery_get_level());
}