/* -------------------------------------------------------------------------- */
/* Buddy - battery.c                                                          */
/*                                                                            */
/* Battery voltage reading and simple H/M/L/AD level detection.                */
/* -------------------------------------------------------------------------- */

#include "battery.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"

/* -------------------------------------------------------------------------- */
/* Defines / constants                                                        */
/* -------------------------------------------------------------------------- */

#define BATTERY_ADC_UNIT    ADC_UNIT_1
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_1 /* GPIO1 on ESP32-C3 */
#define BATTERY_ADC_ATTEN   ADC_ATTEN_DB_12

#define BATTERY_DIVIDER_FACTOR 2.00f
#define BATTERY_CALIBRATION    0.90f

#define BATTERY_HIGH_MV        3900
#define BATTERY_MEDIUM_MV      3700
#define BATTERY_LOW_MV         3500
#define BATTERY_ALMOST_DEAD_MV 3300

/* -------------------------------------------------------------------------- */
/* Static variables                                                           */
/* -------------------------------------------------------------------------- */

static const char *TAG = "battery";

static adc_oneshot_unit_handle_t s_adc_handle = NULL;

/* -------------------------------------------------------------------------- */
/* ADC helpers                                                                */
/* -------------------------------------------------------------------------- */

static int battery_read_mv(void)
{
    if (!s_adc_handle)
    {
        return 0;
    }

    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_adc_handle, BATTERY_ADC_CHANNEL, &raw);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "ADC read failed: %s", esp_err_to_name(err));
        return 0;
    }

    /*
     * Approximate ADC voltage.
     *
     * Battery divider:
     * BAT+ --- 100k --- ADC --- 100k --- GND
     *
     * Battery voltage = ADC voltage * 2
     */
    float adc_voltage = ((float)raw / 4095.0f) * 3.3f;
    float battery_voltage = adc_voltage * BATTERY_DIVIDER_FACTOR * BATTERY_CALIBRATION;

    int battery_mv = (int)(battery_voltage * 1000.0f);

    // ESP_LOGI(TAG, "raw=%d adc=%.2fV bat=%dmV", raw, adc_voltage, battery_mv);

    return battery_mv;
}

/* -------------------------------------------------------------------------- */
/* Battery level helpers                                                      */
/* -------------------------------------------------------------------------- */

static battery_level_t voltage_to_level(int battery_mv)
{
    if (battery_mv >= BATTERY_HIGH_MV)
    {
        return BAT_LEVEL_HIGH;
    }

    if (battery_mv >= BATTERY_MEDIUM_MV)
    {
        return BAT_LEVEL_MEDIUM;
    }

    if (battery_mv >= BATTERY_LOW_MV)
    {
        return BAT_LEVEL_LOW;
    }

    return BAT_LEVEL_ALMOST_DEAD;
}

static const char *level_to_text(battery_level_t level)
{
    switch (level)
    {
    case BAT_LEVEL_HIGH:
        return "H";

    case BAT_LEVEL_MEDIUM:
        return "M";

    case BAT_LEVEL_LOW:
        return "L";

    case BAT_LEVEL_ALMOST_DEAD:
        return "AD";

    default:
        return "?";
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
    return (float)battery_read_mv() / 1000.0f;
}

int battery_get_voltage_mv(void)
{
    return battery_read_mv();
}

battery_level_t battery_get_level(void)
{
    return voltage_to_level(battery_read_mv());
}

const char *battery_get_level_text(void)
{
    return level_to_text(battery_get_level());
}