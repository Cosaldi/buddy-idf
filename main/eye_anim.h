/* -------------------------------------------------------------------------- */
/* Buddy - eye_anim.h                                                         */
/*                                                                            */
/* Public API for Buddy eye animation (LVGL v8, SSD1306/SH1106 128x64)        */
/* -------------------------------------------------------------------------- */

#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Eye expressions                                                            */
/* -------------------------------------------------------------------------- */

typedef enum {
    EYE_EXPR_NORMAL = 0,
    EYE_EXPR_HAPPY,
    EYE_EXPR_ANGRY,
    EYE_EXPR_SLEEPY,
    EYE_EXPR_SURPRISED,
    EYE_EXPR_WONDER,
    EYE_EXPR_CUTE,
    EYE_EXPR_SUSPICIOUS,
    EYE_EXPR_SAD,
    EYE_EXPR_UPSET,
    EYE_EXPR_CLOSE,
    EYE_EXPR_LOVE,
    EYE_EXPR_COUNT
} eye_expression_t;

/* -------------------------------------------------------------------------- */
/* Look directions                                                            */
/* -------------------------------------------------------------------------- */

typedef enum {
    EYE_LOOK_CENTER = 0,
    EYE_LOOK_LEFT,
    EYE_LOOK_RIGHT,
    EYE_LOOK_UP,
    EYE_LOOK_DOWN,
} eye_look_t;

/* -------------------------------------------------------------------------- */
/* Expression combo reactions                                                 */
/* -------------------------------------------------------------------------- */

typedef enum {
    EYE_COMBO_RANDOM = 0,
    EYE_COMBO_CUTE,
    EYE_COMBO_CONFUSED,
    EYE_COMBO_LOVE,
    EYE_COMBO_SLEEPY,
    EYE_COMBO_GRUMPY,
    EYE_COMBO_COUNT
} eye_combo_t;

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initialise the eye animation system.
 *
 * Must be called once after LVGL/display initialization.
 *
 * @param parent LVGL parent object to draw on.
 */
void eye_anim_init(lv_obj_t *parent);

/**
 * @brief Drive the eye animation state machine.
 *
 * Call every ~20 ms from an LVGL timer or display tick.
 */
void eye_anim_tick(void);

/**
 * @brief Trigger one blink animation.
 */
void eye_anim_blink(void);

/**
 * @brief Force-start a blink animation.
 *
 * Unlike eye_anim_blink(), this resets the blink state even if another blink
 * is already running. Used by combo reactions to avoid getting stuck.
 */
void eye_anim_force_blink(void);

/**
 * @brief Move the eyes in a direction temporarily.
 *
 * The eyes return to center automatically after a short hold.
 *
 * @param dir Look direction.
 */
void eye_anim_look(eye_look_t dir);

/**
 * @brief Set the current eye expression.
 *
 * @param expr Expression to show.
 */
void eye_anim_set_expression(eye_expression_t expr);

/**
 * @brief Enable or disable idle behavior.
 *
 * Idle behavior includes random blinks, looks, and expressions.
 *
 * @param enable true to enable idle mode, false to disable it.
 */
void eye_anim_set_idle(bool enable);

/**
 * @brief Play a specific expression combo reaction.
 *
 * @param combo Combo type to play.
 */
void eye_anim_play_combo(eye_combo_t combo);

/**
 * @brief Play a random expression combo reaction.
 */
void eye_anim_play_random_combo(void);

#ifdef __cplusplus
}
#endif