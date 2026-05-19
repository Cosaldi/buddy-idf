/* -------------------------------------------------------------------------- */
/* Buddy - eye_anim.c                                                         */
/*                                                                            */
/* Buddy eye animation (LVGL v8, SSD1306/SH1106 128x64)                       */
/* -------------------------------------------------------------------------- */
/*
 * Akno-style robot eyes using two rounded rectangles.
 *
 * Expressions are created by changing eye height, cutting shapes with black
 * rectangles/triangles, and adding simple details such as hearts, cheek dots,
 * side-eye pupils, and thinking dots.
 *
 * Eye geometry (reference, normal state)
 *   Eye W              : 45 px
 *   Eye H              : 30 px
 *   Corner radius      : 8 px
 *   Gap between eyes   : 14 px
 *   Left eye centre    : (32, 32)
 *   Right eye centre   : (96, 32)
 *   Look movement      : +/-8 px horizontal, +/-5 px vertical
 *
 * Expressions
 *   NORMAL      - full rounded rectangles, centred
 *   HAPPY       - reduced height, bottom clipped into smiling arc
 *   ANGRY       - top-inner corners cut diagonally for angry brow
 *   SLEEPY      - heavily reduced height, half-open sleepy look
 *   SURPRISED   - taller rounded eyes, wide-open look
 *   WONDER      - one eye smaller, one eye bigger, pupils looking upward-left
 *   CUTE        - smiling crescent eyes with small cheek dots
 *   SUSPICIOUS  - narrow side-eye with top/bottom cuts and side-looking pupils
 *   SAD         - lower/drooping eyes with outer upper corners cut
 *   CLOSE       - very thin closed eyes
 *   UPSET       - aggressive inner diagonal cuts, angry/squinted style
 *   LOVE        - heart pupils inside normal eye shape
 *
 * Combo reactions
 *   Long-press on the face screen can trigger one of several short expression
 *   sequences.  Expression changes are hidden during blink-close frames so the
 *   transition feels smoother.
 *
 * Notes
 *   OLED pixels bloom strongly, so expressions use large simple shapes instead
 *   of tiny details wherever possible.
 */

/* -------------------------------------------------------------------------- */
/* Includes                                                                   */
/* -------------------------------------------------------------------------- */

#include "eye_anim.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Display geometry                                                           */
/* -------------------------------------------------------------------------- */

#define DISP_W 128
#define DISP_H 64

/* Eye base dimensions */
#define EYE_W 45
#define EYE_H 30
#define EYE_R 8
#define EYE_GAP 14

/* Eye centres */
#define L_CX (((DISP_W - EYE_GAP) / 2) - (EYE_W / 2))
#define R_CX (((DISP_W + EYE_GAP) / 2) + (EYE_W / 2))
#define EYE_CY 32

/* Maximum look offset */
#define LOOK_DX 8
#define LOOK_DY 5

/* More steps = smoother blink.  eye_anim_tick() usually runs every 20 ms. */
#define BLINK_STEPS 2

/* Idle timing, in milliseconds */
#define IDLE_BLINK_MIN 2000
#define IDLE_BLINK_MAX 5000
#define IDLE_LOOK_MIN 3000
#define IDLE_LOOK_MAX 7000
#define LOOK_HOLD_MS 900
#define IDLE_EXPR_MIN 8000
#define IDLE_EXPR_MAX 15000
#define EXPR_HOLD_MIN 1500
#define EXPR_HOLD_MAX 3000

static const char *TAG = "eye_anim";

/* -------------------------------------------------------------------------- */
/* Internal types and state                                                    */
/* -------------------------------------------------------------------------- */

typedef enum
{
    BLINK_IDLE,
    BLINK_CLOSING,
    BLINK_OPENING,
} blink_phase_t;

typedef struct
{
    eye_expression_t expr;
    uint32_t hold_ms;

    /*
     * If true, the combo starts a blink first and switches to expr while the
     * eyes are closed.  This hides hard expression changes.
     */
    bool blink;
} eye_combo_step_t;

typedef struct
{
    const eye_combo_step_t *steps;
    int count;
    const char *name;
} eye_combo_def_t;

static struct
{
    lv_obj_t *canvas;
    lv_color_t cbuf[LV_CANVAS_BUF_SIZE_TRUE_COLOR(DISP_W, DISP_H)];

    /* Blink state */
    blink_phase_t blink_phase;
    int blink_step;

    /* Look state, smoothly lerped by look_tick() */
    int cur_dx;
    int cur_dy;
    int tgt_dx;
    int tgt_dy;
    uint32_t look_return_ms;

    /* Current face expression */
    eye_expression_t expr;

    /* Idle/random animation state */
    bool idle_enabled;
    uint32_t next_blink_ms;
    uint32_t next_look_ms;
    uint32_t next_expr_ms;
    uint32_t expr_return_ms;

    /* Manual combo reaction state */
    bool combo_active;
    int combo_id;
    int combo_index;
    uint32_t combo_next_ms;
    bool combo_restore_idle;

    /* Smooth combo transition state */
    bool combo_waiting_switch;
    eye_expression_t combo_pending_expr;
    uint32_t combo_pending_hold_ms;
} g;

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint32_t rand_range(uint32_t lo, uint32_t hi)
{
    return lo + (uint32_t)(esp_random() % (hi - lo + 1));
}

/* -------------------------------------------------------------------------- */
/* Drawing primitives                                                          */
/* -------------------------------------------------------------------------- */

static void draw_dot(int x, int y, int size, lv_color_t color)
{
    if (size <= 0)
    {
        return;
    }

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.border_width = 0;
    dsc.radius = 0;

    x = LV_MAX(x, 0);
    y = LV_MAX(y, 0);

    if (x + size > DISP_W)
    {
        size = DISP_W - x;
    }
    if (y + size > DISP_H)
    {
        size = DISP_H - y;
    }

    if (size > 0)
    {
        lv_canvas_draw_rect(g.canvas, x, y, size, size, &dsc);
    }
}

/* Draw a larger pixel heart centered at (cx, cy).
 *
 * The original 9x7 heart is scaled to 18x14 because tiny OLED details are hard
 * to read, especially on 1.3" displays.
 */
static void draw_heart_big(int cx, int cy, lv_color_t color)
{
    static const uint8_t heart[7][9] = {
        {0, 1, 1, 1, 0, 1, 1, 1, 0},
        {1, 1, 1, 1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1, 1, 1, 1},
        {0, 1, 1, 1, 1, 1, 1, 1, 0},
        {0, 0, 1, 1, 1, 1, 1, 0, 0},
        {0, 0, 0, 1, 1, 1, 0, 0, 0},
        {0, 0, 0, 0, 1, 0, 0, 0, 0},
    };

    const int scale = 2;
    const int w = 9 * scale;
    const int h = 7 * scale;
    const int x0 = cx - (w / 2);
    const int y0 = cy - (h / 2);

    for (int y = 0; y < 7; y++)
    {
        for (int x = 0; x < 9; x++)
        {
            if (heart[y][x])
            {
                draw_dot(x0 + (x * scale), y0 + (y * scale), scale, color);
            }
        }
    }
}

/* Filled rectangle with clipping. */
static void draw_rect(int x, int y, int w, int h, lv_color_t color)
{
    if (w <= 0 || h <= 0)
    {
        return;
    }

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.border_width = 0;
    dsc.radius = 0;

    x = LV_MAX(x, 0);
    y = LV_MAX(y, 0);

    if (x + w > DISP_W)
    {
        w = DISP_W - x;
    }
    if (y + h > DISP_H)
    {
        h = DISP_H - y;
    }

    if (w > 0 && h > 0)
    {
        lv_canvas_draw_rect(g.canvas, x, y, w, h, &dsc);
    }
}

/* Filled rounded rectangle.
 *
 * LVGL canvas v8 does not give us a simple filled rounded rect primitive for
 * this exact monochrome workflow, so this draws one scanline at a time.
 */
static void draw_rrect(int x, int y, int w, int h, int r, lv_color_t color)
{
    if (w <= 0 || h <= 0)
    {
        return;
    }

    if (r > w / 2)
    {
        r = w / 2;
    }
    if (r > h / 2)
    {
        r = h / 2;
    }

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.border_width = 0;
    dsc.radius = 0;

    for (int row = y; row < y + h; row++)
    {
        if (row < 0 || row >= DISP_H)
        {
            continue;
        }

        const int dy_top = row - y;
        const int dy_bot = (y + h - 1) - row;
        int margin = 0;

        if (dy_top < r)
        {
            int fy = r - dy_top;
            lv_sqrt_res_t sq;
            lv_sqrt((uint32_t)(r * r - fy * fy), &sq, 0x800);
            margin = r - (int)sq.i;
        }
        else if (dy_bot < r)
        {
            int fy = r - dy_bot;
            lv_sqrt_res_t sq;
            lv_sqrt((uint32_t)(r * r - fy * fy), &sq, 0x800);
            margin = r - (int)sq.i;
        }

        int x1 = x + margin;
        int x2 = x + w - 1 - margin;

        if (x2 < x1)
        {
            continue;
        }

        x1 = LV_MAX(x1, 0);
        x2 = LV_MIN(x2, DISP_W - 1);

        lv_canvas_draw_rect(g.canvas, x1, row, x2 - x1 + 1, 1, &dsc);
    }
}

/* Filled triangle, used as an eraser/cut for angry, sad, and upset eyes. */
static void draw_triangle(int x0, int y0, int x1, int y1, int x2, int y2, lv_color_t color)
{
    lv_point_t pts[3] = {
        {x0, y0},
        {x1, y1},
        {x2, y2},
    };

    /* Sort points by Y so the scanline pass is stable. */
    for (int i = 0; i < 2; i++)
    {
        for (int j = i + 1; j < 3; j++)
        {
            if (pts[j].y < pts[i].y)
            {
                lv_point_t tmp = pts[i];
                pts[i] = pts[j];
                pts[j] = tmp;
            }
        }
    }

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.border_width = 0;
    dsc.radius = 0;

    lv_point_t edges[3][2] = {
        {pts[0], pts[1]},
        {pts[1], pts[2]},
        {pts[0], pts[2]},
    };

    for (int y = pts[0].y; y <= pts[2].y; y++)
    {
        int xlo = DISP_W;
        int xhi = -1;

        for (int e = 0; e < 3; e++)
        {
            const int ay = edges[e][0].y;
            const int by = edges[e][1].y;
            const int ax = edges[e][0].x;
            const int bx = edges[e][1].x;

            if (ay == by)
            {
                continue;
            }
            if (y < LV_MIN(ay, by) || y > LV_MAX(ay, by))
            {
                continue;
            }

            const int x = ax + ((bx - ax) * (y - ay)) / (by - ay);
            xlo = LV_MIN(xlo, x);
            xhi = LV_MAX(xhi, x);
        }

        if (xhi < xlo || y < 0 || y >= DISP_H)
        {
            continue;
        }

        xlo = LV_MAX(xlo, 0);
        xhi = LV_MIN(xhi, DISP_W - 1);

        lv_canvas_draw_rect(g.canvas, xlo, y, xhi - xlo + 1, 1, &dsc);
    }
}

/* -------------------------------------------------------------------------- */
/* Combo definitions                                                          */
/* -------------------------------------------------------------------------- */

#define COMBO_DEF(arr, combo_name) {arr, sizeof(arr) / sizeof((arr)[0]), combo_name}

static const eye_combo_step_t s_combo_cute[] = {
    { EYE_EXPR_NORMAL, 150, false },
    { EYE_EXPR_HAPPY,  350, true  },
    { EYE_EXPR_CUTE,   550, true  },
    { EYE_EXPR_HAPPY,  350, true  },
    { EYE_EXPR_NORMAL, 250, true  },
};

static const eye_combo_step_t s_combo_confused[] = {
    { EYE_EXPR_NORMAL,     150, false },
    { EYE_EXPR_WONDER,     600, true  },
    { EYE_EXPR_SUSPICIOUS, 450, true  },
    { EYE_EXPR_WONDER,     500, true  },
    { EYE_EXPR_NORMAL,     250, true  },
};

static const eye_combo_step_t s_combo_love[] = {
    { EYE_EXPR_NORMAL, 150, false },
    { EYE_EXPR_CUTE,   350, true  },
    { EYE_EXPR_LOVE,   700, true  },
    { EYE_EXPR_HAPPY,  450, true  },
    { EYE_EXPR_NORMAL, 250, true  },
};

static const eye_combo_step_t s_combo_sleepy[] = {
    { EYE_EXPR_NORMAL, 200, false },
    { EYE_EXPR_SLEEPY, 600, true  },
    { EYE_EXPR_CLOSE,  500, false },
    { EYE_EXPR_SLEEPY, 450, true  },
    { EYE_EXPR_NORMAL, 300, true  },
};

static const eye_combo_step_t s_combo_grumpy[] = {
    { EYE_EXPR_NORMAL,     150, false },
    { EYE_EXPR_SUSPICIOUS, 500, true  },
    { EYE_EXPR_ANGRY,      450, true  },
    { EYE_EXPR_UPSET,      600, true  },
    { EYE_EXPR_NORMAL,     300, true  },
};

static const eye_combo_def_t s_combos[] = {
    COMBO_DEF(s_combo_cute, "cute"),
    COMBO_DEF(s_combo_confused, "confused"),
    COMBO_DEF(s_combo_love, "love"),
    COMBO_DEF(s_combo_sleepy, "sleepy"),
    COMBO_DEF(s_combo_grumpy, "grumpy"),
};

/* -------------------------------------------------------------------------- */
/* Renderer                                                                   */
/* -------------------------------------------------------------------------- */

static void render_frame(void)
{
    lv_canvas_fill_bg(g.canvas, lv_color_black(), LV_OPA_COVER);

    const lv_color_t white = lv_color_white();
    const lv_color_t black = lv_color_black();

    /* Blink scale affects most expressions. */
    float h_scale = 1.0f;
    if (g.expr == EYE_EXPR_SLEEPY)
    {
        h_scale = 0.38f;
    }
    else if (g.blink_phase == BLINK_CLOSING)
    {
        h_scale = 1.0f - ((float)g.blink_step / BLINK_STEPS);
    }
    else if (g.blink_phase == BLINK_OPENING)
    {
        h_scale = (float)g.blink_step / BLINK_STEPS;
    }

    /* Base height multiplier per expression. */
    float expr_h = 1.0f;
    if (g.expr == EYE_EXPR_HAPPY)
    {
        expr_h = 0.72f;
    }
    else if (g.expr == EYE_EXPR_SURPRISED)
    {
        expr_h = 1.30f;
    }
    else if (g.expr == EYE_EXPR_WONDER)
    {
        expr_h = 1.00f;
    }
    else if (g.expr == EYE_EXPR_CUTE)
    {
        expr_h = 0.68f;
    }
    else if (g.expr == EYE_EXPR_CLOSE)
    {
        expr_h = 0.12f;
    }
    else if (g.expr == EYE_EXPR_SUSPICIOUS)
    {
        expr_h = 0.55f;
    }
    else if (g.expr == EYE_EXPR_SAD)
    {
        expr_h = 0.55f;
    }
    else if (g.expr == EYE_EXPR_UPSET)
    {
        expr_h = 0.55f;
    }
    else if (g.expr == EYE_EXPR_LOVE)
    {
        expr_h = 1.05f;
    }

    int eye_h = (int)(EYE_H * expr_h * h_scale);
    if (eye_h < 1)
    {
        eye_h = 1;
    }

    int eye_r = EYE_R;
    if (eye_r > eye_h / 2)
    {
        eye_r = eye_h / 2;
    }

    const int centres[2] = {L_CX, R_CX};

    for (int i = 0; i < 2; i++)
    {
        int cx = centres[i] + g.cur_dx;
        int cy = EYE_CY + g.cur_dy;

        if (g.expr == EYE_EXPR_SAD)
        {
            cy += 4;
        }

        if (g.expr == EYE_EXPR_CUTE)
        {
            cy += 2;
        }

        int this_eye_h = eye_h;

        /* WONDER: asymmetry makes it look curious instead of surprised. */
        if (g.expr == EYE_EXPR_WONDER)
        {
            if (i == 0)
            {
                this_eye_h = eye_h - 8;
            }
            else
            {
                this_eye_h = eye_h + 4;
            }

            if (this_eye_h < 6)
            {
                this_eye_h = 6;
            }
        }

        int this_eye_r = eye_r;
        if (this_eye_r > this_eye_h / 2)
        {
            this_eye_r = this_eye_h / 2;
        }

        const int ex = cx - (EYE_W / 2);
        const int ey = cy - (this_eye_h / 2);

        draw_rrect(ex, ey, EYE_W, this_eye_h, this_eye_r, white);

        /* LOVE: big heart pupil. */
        if (g.expr == EYE_EXPR_LOVE && h_scale > 0.1f)
        {
            draw_heart_big(cx, cy, black);
        }

        /* HAPPY: erase the lower half, leaving an upward smile arc. */
        if (g.expr == EYE_EXPR_HAPPY && h_scale > 0.1f)
        {
            const int cut = this_eye_h / 2;
            draw_rect(ex - 1, ey + cut, EYE_W + 2, this_eye_h - cut + 2, black);
        }

        /* ANGRY: cut the top-inner corners. */
        if (g.expr == EYE_EXPR_ANGRY && h_scale > 0.1f)
        {
            const int cut_w = EYE_W / 2 + 2;
            const int cut_h = this_eye_h / 2 + 2;

            if (i == 0)
            {
                draw_triangle(
                    ex + EYE_W - cut_w, ey, ex + EYE_W, ey, ex + EYE_W, ey + cut_h, black);
            }
            else
            {
                draw_triangle(ex, ey, ex + cut_w, ey, ex, ey + cut_h, black);
            }
        }

        /* CUTE: smiling crescent eyes with small outside cheek dots. */
        if (g.expr == EYE_EXPR_CUTE && h_scale > 0.1f)
        {
            const int cut_h = (this_eye_h / 2) + 4;

            draw_rect(ex + 4, ey, EYE_W - 8, cut_h, black);

            if (i == 0)
            {
                draw_dot(ex - 3, cy + 5, 2, white);
                draw_dot(ex - 6, cy + 7, 2, white);
            }
            else
            {
                draw_dot(ex + EYE_W + 1, cy + 5, 2, white);
                draw_dot(ex + EYE_W + 4, cy + 7, 2, white);
            }
        }

        /* SUSPICIOUS: narrow side-eye. */
        if (g.expr == EYE_EXPR_SUSPICIOUS && h_scale > 0.1f)
        {
            draw_rect(ex, ey, EYE_W, 4, black);
            draw_rect(ex, ey + this_eye_h - 4, EYE_W, 4, black);

            const int pupil_size = 5;
            const int px = cx - 9;
            const int py = ey + (this_eye_h / 2);

            draw_dot(px - (pupil_size / 2), py - (pupil_size / 2), pupil_size, black);
        }

        /* SAD: cut the outer upper corners to make the eyes droop. */
        if (g.expr == EYE_EXPR_SAD && h_scale > 0.1f)
        {
            const int cut_w = EYE_W / 2;
            const int cut_h = this_eye_h / 2;

            if (i == 0)
            {
                draw_triangle(ex, ey, ex + cut_w, ey, ex, ey + cut_h, black);
            }
            else
            {
                draw_triangle(
                    ex + EYE_W - cut_w, ey, ex + EYE_W, ey, ex + EYE_W, ey + cut_h, black);
            }
        }

        /* WONDER: visible upward-left pupils plus thinking dots near big eye. */
        if (g.expr == EYE_EXPR_WONDER && h_scale > 0.1f)
        {
            const int pupil_size = 6;
            const int px = cx - 6;
            const int py = ey + 7;

            draw_dot(px - (pupil_size / 2), py - (pupil_size / 2), pupil_size, black);

            if (i == 1)
            {
                draw_dot(ex + EYE_W + 4, ey + 2, 2, white);
                draw_dot(ex + EYE_W + 7, ey - 2, 2, white);
            }
        }

        /* UPSET: aggressive squint with strong inner diagonal cuts. */
        if (g.expr == EYE_EXPR_UPSET && h_scale > 0.1f)
        {
            const int cut_w = EYE_W / 2 + 4;
            const int cut_h = this_eye_h;

            if (i == 0)
            {
                draw_triangle(
                    ex + EYE_W - cut_w, ey, ex + EYE_W, ey, ex + EYE_W, ey + cut_h, black);
            }
            else
            {
                draw_triangle(ex, ey, ex + cut_w, ey, ex, ey + cut_h, black);
            }
        }
    }

    lv_obj_invalidate(g.canvas);
}

/* -------------------------------------------------------------------------- */
/* State machines                                                             */
/* -------------------------------------------------------------------------- */

static void blink_tick(void)
{
    if (g.blink_phase == BLINK_IDLE)
    {
        return;
    }

    g.blink_step++;

    if (g.blink_phase == BLINK_CLOSING && g.blink_step >= BLINK_STEPS)
    {
        g.blink_phase = BLINK_OPENING;
        g.blink_step = 0;
    }
    else if (g.blink_phase == BLINK_OPENING && g.blink_step >= BLINK_STEPS)
    {
        g.blink_phase = BLINK_IDLE;
        g.blink_step = 0;
    }
}

static void look_tick(void)
{
    if (g.look_return_ms && now_ms() >= g.look_return_ms)
    {
        g.tgt_dx = 0;
        g.tgt_dy = 0;
        g.look_return_ms = 0;
    }

    const int step = 2;

    if (g.cur_dx < g.tgt_dx)
    {
        g.cur_dx = LV_MIN(g.cur_dx + step, g.tgt_dx);
    }
    else if (g.cur_dx > g.tgt_dx)
    {
        g.cur_dx = LV_MAX(g.cur_dx - step, g.tgt_dx);
    }

    if (g.cur_dy < g.tgt_dy)
    {
        g.cur_dy = LV_MIN(g.cur_dy + step, g.tgt_dy);
    }
    else if (g.cur_dy > g.tgt_dy)
    {
        g.cur_dy = LV_MAX(g.cur_dy - step, g.tgt_dy);
    }
}

static void idle_tick(void)
{
    if (!g.idle_enabled)
    {
        return;
    }

    uint32_t t = now_ms();

    if (t >= g.next_blink_ms && g.blink_phase == BLINK_IDLE)
    {
        eye_anim_blink();
        g.next_blink_ms = t + rand_range(IDLE_BLINK_MIN, IDLE_BLINK_MAX);
    }

    if (t >= g.next_look_ms)
    {
        static const eye_look_t dirs[] = {
            EYE_LOOK_LEFT,
            EYE_LOOK_RIGHT,
            EYE_LOOK_UP,
            EYE_LOOK_DOWN,
            EYE_LOOK_CENTER,
        };

        eye_look_t dir = dirs[esp_random() % (sizeof(dirs) / sizeof(dirs[0]))];
        eye_anim_look(dir);
        g.next_look_ms = t + rand_range(IDLE_LOOK_MIN, IDLE_LOOK_MAX);
    }

    if (g.expr_return_ms && t >= g.expr_return_ms)
    {
        g.expr = EYE_EXPR_NORMAL;
        g.expr_return_ms = 0;
        g.next_expr_ms = t + rand_range(IDLE_EXPR_MIN, IDLE_EXPR_MAX);
    }

    if (g.expr == EYE_EXPR_NORMAL && t >= g.next_expr_ms)
    {
        static const eye_expression_t exprs[] = {
            EYE_EXPR_HAPPY,
            EYE_EXPR_ANGRY,
            EYE_EXPR_SLEEPY,
            EYE_EXPR_SURPRISED,
            EYE_EXPR_WONDER,
            EYE_EXPR_CUTE,
            EYE_EXPR_SUSPICIOUS,
            EYE_EXPR_SAD,
            EYE_EXPR_CLOSE,
            EYE_EXPR_UPSET,
            EYE_EXPR_LOVE,
        };

        g.expr = exprs[esp_random() % (sizeof(exprs) / sizeof(exprs[0]))];
        g.expr_return_ms = t + rand_range(EXPR_HOLD_MIN, EXPR_HOLD_MAX);
    }
}

/* Run the active combo without blocking LVGL.
 *
 * Smooth transition flow:
 *   1. Start a blink.
 *   2. Wait until the eye is fully closed.
 *   3. Change expression while hidden.
 *   4. Open the eye with the new expression.
 */
static void combo_tick(void)
{
    if (!g.combo_active)
    {
        return;
    }

    uint32_t t = now_ms();

    if (g.combo_waiting_switch)
    {
        if (g.blink_phase == BLINK_OPENING && g.blink_step == 0)
        {
            g.expr = g.combo_pending_expr;
            g.combo_waiting_switch = false;
            g.combo_next_ms = t + g.combo_pending_hold_ms;
            g.combo_index++;
        }
        return;
    }

    if (g.combo_next_ms != 0 && t < g.combo_next_ms)
    {
        return;
    }

    const eye_combo_def_t *combo = &s_combos[g.combo_id];

    if (g.combo_index >= combo->count)
    {
        g.combo_active = false;
        g.combo_id = 0;
        g.combo_index = 0;
        g.combo_next_ms = 0;
        g.combo_waiting_switch = false;
        g.combo_pending_expr = EYE_EXPR_NORMAL;
        g.combo_pending_hold_ms = 0;

        g.expr = EYE_EXPR_NORMAL;

        if (g.combo_restore_idle)
        {
            eye_anim_set_idle(true);
        }

        ESP_LOGI(TAG, "eye combo finished: %s", combo->name);
        return;
    }

    const eye_combo_step_t *step = &combo->steps[g.combo_index];

    if (step->blink)
    {
        g.combo_pending_expr = step->expr;
        g.combo_pending_hold_ms = step->hold_ms;
        g.combo_waiting_switch = true;
        /* Force blink so combo cannot get stuck if idle blink was already active */
        eye_anim_force_blink();
    }
    else
    {
        g.expr = step->expr;
        g.combo_next_ms = t + step->hold_ms;
        g.combo_index++;
    }
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

void eye_anim_init(lv_obj_t *parent)
{
    memset(&g, 0, sizeof(g));

    g.canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(g.canvas, g.cbuf, DISP_W, DISP_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(g.canvas, LV_ALIGN_CENTER, 0, 0);

    g.idle_enabled = true;

    uint32_t t = now_ms();
    g.next_blink_ms = t + rand_range(IDLE_BLINK_MIN, IDLE_BLINK_MAX);
    g.next_look_ms = t + rand_range(IDLE_LOOK_MIN, IDLE_LOOK_MAX);
    g.next_expr_ms = t + rand_range(IDLE_EXPR_MIN, IDLE_EXPR_MAX);

    render_frame();

    ESP_LOGI(TAG, "eye_anim initialised");
}

void eye_anim_tick(void)
{
    if (!g.combo_active)
    {
        idle_tick();
    }

    combo_tick();
    blink_tick();
    look_tick();
    render_frame();
}

void eye_anim_blink(void)
{
    if (g.blink_phase != BLINK_IDLE)
    {
        return;
    }

    g.blink_phase = BLINK_CLOSING;
    g.blink_step = 0;
}

void eye_anim_force_blink(void)
{
    g.blink_phase = BLINK_CLOSING;
    g.blink_step = 0;
}

void eye_anim_look(eye_look_t dir)
{
    switch (dir)
    {
    case EYE_LOOK_LEFT:
        g.tgt_dx = -LOOK_DX;
        g.tgt_dy = 0;
        break;

    case EYE_LOOK_RIGHT:
        g.tgt_dx = LOOK_DX;
        g.tgt_dy = 0;
        break;

    case EYE_LOOK_UP:
        g.tgt_dx = 0;
        g.tgt_dy = -LOOK_DY;
        break;

    case EYE_LOOK_DOWN:
        g.tgt_dx = 0;
        g.tgt_dy = LOOK_DY;
        break;

    default:
        g.tgt_dx = 0;
        g.tgt_dy = 0;
        break;
    }

    if (dir != EYE_LOOK_CENTER)
    {
        g.look_return_ms = now_ms() + LOOK_HOLD_MS;
    }
}

void eye_anim_set_expression(eye_expression_t expr)
{
    if (expr >= EYE_EXPR_COUNT)
    {
        return;
    }

    g.expr = expr;
    ESP_LOGI(TAG, "expression -> %d", expr);
}

void eye_anim_play_combo(eye_combo_t combo)
{
    if (g.combo_active)
    {
        return;
    }

    int combo_id;

    if (combo == EYE_COMBO_RANDOM)
    {
        combo_id = esp_random() % (sizeof(s_combos) / sizeof(s_combos[0]));
    }
    else
    {
        combo_id = (int)combo - 1;

        if (combo_id < 0 || combo_id >= (int)(sizeof(s_combos) / sizeof(s_combos[0])))
        {
            combo_id = 0;
        }
    }

    g.combo_active = true;
    g.combo_id = combo_id;
    g.combo_index = 0;
    g.combo_next_ms = 0;

    g.combo_waiting_switch = false;
    g.combo_pending_expr = EYE_EXPR_NORMAL;
    g.combo_pending_hold_ms = 0;

    g.combo_restore_idle = g.idle_enabled;

    /* Stop idle random expressions while the manual reaction plays. */
    g.idle_enabled = false;

    ESP_LOGI(TAG, "eye combo started: %s", s_combos[g.combo_id].name);
}

void eye_anim_play_random_combo(void)
{
    eye_anim_play_combo(EYE_COMBO_RANDOM);
}

void eye_anim_set_idle(bool enable)
{
    g.idle_enabled = enable;

    if (enable)
    {
        uint32_t t = now_ms();

        g.next_blink_ms = t + rand_range(IDLE_BLINK_MIN, IDLE_BLINK_MAX);
        g.next_look_ms = t + rand_range(IDLE_LOOK_MIN, IDLE_LOOK_MAX);
        g.next_expr_ms = t + rand_range(IDLE_EXPR_MIN, IDLE_EXPR_MAX);
        g.expr_return_ms = 0;
        g.expr = EYE_EXPR_NORMAL;
    }
}
