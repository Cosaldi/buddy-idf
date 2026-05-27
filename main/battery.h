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
    BATTERY_LEVEL_LOW = 0,
    BATTERY_LEVEL_MID,
    BATTERY_LEVEL_HIGH,
} battery_level_t;

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void battery_init(void);

float battery_get_voltage(void);

battery_level_t battery_get_level(void);

const char *battery_get_level_text(void);

#ifdef __cplusplus
}
#endif