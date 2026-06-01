/* -------------------------------------------------------------------------- */
/* Buddy - battery.h                                                          */
/*                                                                            */
/* Public API for reading battery voltage and battery level state.             */
/* -------------------------------------------------------------------------- */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Public types                                                               */
/* -------------------------------------------------------------------------- */

typedef enum {
    BAT_LEVEL_HIGH = 0,
    BAT_LEVEL_MEDIUM,
    BAT_LEVEL_LOW,
    BAT_LEVEL_ALMOST_DEAD,
} battery_level_t;

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void battery_init(void);

float battery_get_voltage(void);
int battery_get_voltage_mv(void);

battery_level_t battery_get_level(void);

const char *battery_get_level_text(void);

#ifdef __cplusplus
}
#endif