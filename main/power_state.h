/* -------------------------------------------------------------------------- */
/* Buddy - power_state.h                                                      */
/* -------------------------------------------------------------------------- */

#pragma once

/* -------------------------------------------------------------------------- */
/* Includes                                                                   */
/* -------------------------------------------------------------------------- */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        BUDDY_POWER_AWAKE = 0,
        BUDDY_POWER_SLEEP_ANIM,
        BUDDY_POWER_SLEEPING,
        BUDDY_POWER_WAKE_ANIM,
    } buddy_power_state_t;

    typedef struct
    {
        uint32_t sleep_after_ms; // example: 5 or 10 minutes
    } buddy_power_config_t;

    typedef struct
    {
        void (*on_awake)(void);
        void (*on_sleep_anim)(void);
        void (*on_sleep)(void);
        void (*on_wake_anim)(void);
    } buddy_power_callbacks_t;

    void buddy_power_init(const buddy_power_config_t *config,
                          const buddy_power_callbacks_t *callbacks);

    void buddy_power_tick(void);
    void buddy_power_activity(void);

    void buddy_power_force_sleep(void);
    void buddy_power_wake(void);
    void buddy_power_finish_sleep_anim(void);
    void buddy_power_finish_wake_anim(void);
    void buddy_power_skip_sleep_anim(void);

    buddy_power_state_t buddy_power_get_state(void);
    
    bool buddy_power_is_sleeping(void);
    bool buddy_power_is_awake(void);

#ifdef __cplusplus
}
#endif