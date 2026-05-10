#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Eye expressions ─────────────────────────────────────── */
typedef enum {
    EYE_EXPR_NORMAL  = 0,
    EYE_EXPR_HAPPY,
    EYE_EXPR_ANGRY,
    EYE_EXPR_SLEEPY,
    EYE_EXPR_SURPRISED,
    EYE_EXPR_COUNT
} eye_expression_t;

/* ── Look directions ─────────────────────────────────────── */
typedef enum {
    EYE_LOOK_CENTER = 0,
    EYE_LOOK_LEFT,
    EYE_LOOK_RIGHT,
    EYE_LOOK_UP,
    EYE_LOOK_DOWN,
} eye_look_t;

/* ── Public API ──────────────────────────────────────────── */

/**
 * @brief  Initialise the eye animation system.
 *         Must be called once after lv_init() and display init.
 * @param  parent  LVGL object to draw on (pass lv_scr_act() for full screen).
 */
void eye_anim_init(lv_obj_t *parent);

/**
 * @brief  Drive the eye state-machine.
 *         Call from your main loop or a 20 ms timer task.
 */
void eye_anim_tick(void);

/** @brief  Trigger a single blink immediately. */
void eye_anim_blink(void);

/** @brief  Move pupils to a direction (snaps back to CENTER after 1 s). */
void eye_anim_look(eye_look_t dir);

/** @brief  Switch to an expression. Pass EYE_EXPR_NORMAL to reset. */
void eye_anim_set_expression(eye_expression_t expr);

/** @brief  Start / stop the automatic idle behaviour (random blinks + looks). */
void eye_anim_set_idle(bool enable);

#ifdef __cplusplus
}
#endif