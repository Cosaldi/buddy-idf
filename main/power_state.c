/* -------------------------------------------------------------------------- */
/* Buddy - power_state.c                                                      */
/* -------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------- */
/* Includes                                                                   */
/* -------------------------------------------------------------------------- */

#include "power_state.h"

#include "esp_log.h"
#include "esp_log_level.h"
#include "esp_timer.h"
#include <stdint.h>
#include <sys/_types.h>

/* -------------------------------------------------------------------------- */
/* Defines                                                                    */
/* -------------------------------------------------------------------------- */

#define SLEEP_ANIM_MAX_MS 2000

/* -------------------------------------------------------------------------- */
/* Static variables                                                           */
/* -------------------------------------------------------------------------- */

static const char *TAG = "power_state";

static int64_t s_state_started_ms = 0;

static buddy_power_state_t s_state = BUDDY_POWER_AWAKE;

static buddy_power_config_t s_config = {
    .sleep_after_ms = 2 * 60 * 1000,
};

static buddy_power_callbacks_t s_callbacks = {0};

static int64_t s_last_activity_ms = 0;
static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void set_state(buddy_power_state_t new_state)
{
    if (s_state == new_state)
    {
        return;
    }

    s_state = new_state;
    s_state_started_ms = now_ms();

    switch (s_state)
    {
    case BUDDY_POWER_AWAKE:
        ESP_LOGI(TAG, "state: AWAKE");
        if (s_callbacks.on_awake)
        {
            s_callbacks.on_awake();
        }
        break;

    case BUDDY_POWER_SLEEPING:
        ESP_LOGI(TAG, "state: SLEEPING");
        if (s_callbacks.on_sleep)
        {
            s_callbacks.on_sleep();
        }
        break;

    case BUDDY_POWER_WAKE_ANIM:
        ESP_LOGI(TAG, "state: WAKE_ANIM");
        if (s_callbacks.on_wake_anim)
        {
            s_callbacks.on_wake_anim();
        }
        break;

    case BUDDY_POWER_SLEEP_ANIM:
        ESP_LOGI(TAG, "state: SLEEP_ANIM");
        if (s_callbacks.on_sleep_anim)
        {
            s_callbacks.on_sleep_anim();
        }
        break;

    default:
        break;
    }
}

void buddy_power_init(const buddy_power_config_t *config, const buddy_power_callbacks_t *callbacks)
{
    if (config)
    {
        s_config = *config;
    }

    if (callbacks)
    {
        s_callbacks = *callbacks;
    }

    s_last_activity_ms = now_ms();
    s_state = BUDDY_POWER_AWAKE;

    ESP_LOGI(TAG, "Initialised: sleep_after=%lu ms", (unsigned long)s_config.sleep_after_ms);
}

void buddy_power_tick(void)
{
    int64_t now = now_ms();
    if (s_state == BUDDY_POWER_SLEEP_ANIM) {
        if ((now - s_state_started_ms) >= SLEEP_ANIM_MAX_MS) {
            ESP_LOGW(TAG, "sleep animation timeout, forcing sleep");
            set_state(BUDDY_POWER_SLEEPING);
        }
        return;
    }
    
    if (s_state != BUDDY_POWER_AWAKE)
    {
        return;
    }

    int64_t idle_ms = now_ms() - s_last_activity_ms;

    if (idle_ms >= s_config.sleep_after_ms)
    {
        set_state(BUDDY_POWER_SLEEP_ANIM);
    }
}

void buddy_power_activity(void)
{
    s_last_activity_ms = now_ms();

    if (s_state == BUDDY_POWER_SLEEPING)
    {
        buddy_power_wake();
    }

    if (s_state == BUDDY_POWER_SLEEP_ANIM)
    {
        set_state(BUDDY_POWER_AWAKE);
        return;
    }
}

void buddy_power_force_sleep(void)
{
    set_state(BUDDY_POWER_SLEEPING);
}

void buddy_power_wake(void)
{
    if (s_state != BUDDY_POWER_SLEEPING)
    {
        return;
    }

    set_state(BUDDY_POWER_WAKE_ANIM);
}

void buddy_power_finish_wake_anim(void)
{
    if (s_state != BUDDY_POWER_WAKE_ANIM)
    {
        return;
    }

    s_last_activity_ms = now_ms();
    set_state(BUDDY_POWER_AWAKE);
}

void buddy_power_finish_sleep_anim(void)
{
    if (s_state != BUDDY_POWER_SLEEP_ANIM)
    {
        return;
    }

    set_state(BUDDY_POWER_SLEEPING);
}

void buddy_power_skip_sleep_anim(void)
{
    if (s_state != BUDDY_POWER_SLEEP_ANIM) {
        return;
    }

    set_state(BUDDY_POWER_SLEEPING);
}

buddy_power_state_t buddy_power_get_state(void)
{
    return s_state;
}

bool buddy_power_is_sleeping(void)
{
    return s_state == BUDDY_POWER_SLEEPING;
}

bool buddy_power_is_awake(void)
{
    return s_state == BUDDY_POWER_AWAKE;
}

bool buddy_power_is_wake_anim(void)
{
    return s_state == BUDDY_POWER_WAKE_ANIM;
}

bool buddy_power_is_busy(void)
{
    return s_state == BUDDY_POWER_WAKE_ANIM ||
           s_state == BUDDY_POWER_SLEEP_ANIM;
}