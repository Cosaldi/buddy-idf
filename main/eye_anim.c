/* -------------------------------------------------------------------------- */
/* Buddy - eye_anim.c                                                         */
/*                                                                            */
/* Buddy eye animation (LVGL v8, SSD1306 128x64)                              */
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
 * Blink
 *   Eye height animates open -> closed -> open.
 *
 * Notes
 *   OLED pixels bloom strongly, so expressions use large simple shapes instead
 *   of tiny details wherever possible.
 */

#include "eye_anim.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *TAG = "eye_anim";

/* -------------------------------------------------------------------------- */
/* Display geometry                                                           */
/* -------------------------------------------------------------------------- */

#define DISP_W 128
#define DISP_H 64

/* Eye base dimensions */
#define EYE_W 45
#define EYE_H 30
#define EYE_R 8    /* corner radius */
#define EYE_GAP 14 /* gap between the two eyes */

/* Eye centres (horizontal) */
#define L_CX ((DISP_W - EYE_GAP) / 2 - EYE_W / 2) /* = 32 */
#define R_CX ((DISP_W + EYE_GAP) / 2 + EYE_W / 2) /* = 96 */
#define EYE_CY 32

/* Look offsets */
#define LOOK_DX 8
#define LOOK_DY 5

/* Blink */
#define BLINK_STEPS 2

/* Idle timing (ms) */
#define IDLE_BLINK_MIN 2000
#define IDLE_BLINK_MAX 5000
#define IDLE_LOOK_MIN 3000
#define IDLE_LOOK_MAX 7000
#define LOOK_HOLD_MS 900
#define IDLE_EXPR_MIN 8000 /* how often a random expression fires  */
#define IDLE_EXPR_MAX 15000
#define EXPR_HOLD_MIN 1500 /* how long expression is held          */
#define EXPR_HOLD_MAX 3000

/* -------------------------------------------------------------------------- */
/* Internal types                                                             */
/* -------------------------------------------------------------------------- */

typedef enum
{
    BLINK_IDLE,
    BLINK_CLOSING,
    BLINK_OPENING
} blink_phase_t;

static struct
{
    lv_obj_t *canvas;
    lv_color_t cbuf[LV_CANVAS_BUF_SIZE_TRUE_COLOR(DISP_W, DISP_H)];

    /* blink */
    blink_phase_t blink_phase;
    int blink_step;

    /* look — smooth lerp */
    int cur_dx, cur_dy;
    int tgt_dx, tgt_dy;
    uint32_t look_return_ms;

    /* expression */
    eye_expression_t expr;

    /* idle */
    bool idle_enabled;
    uint32_t next_blink_ms;
    uint32_t next_look_ms;
    uint32_t next_expr_ms;   /* when to change expression          */
    uint32_t expr_return_ms; /* when to return to NORMAL            */
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

/* Draw small dot */
static void draw_dot(int x, int y, int size, lv_color_t color)
{
    if (size <= 0)
        return;
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.border_width = 0;
    dsc.radius = 0;
    x = LV_MAX(x, 0);
    y = LV_MAX(y, 0);
    if (x + size > DISP_W)
        size = DISP_W - x;
    if (y + size > DISP_H)
        size = DISP_H - y;
    if (size > 0)
        lv_canvas_draw_rect(g.canvas, x, y, size, size, &dsc);
}

/* Draw a larger pixel heart centered at (x, y)
 *
 * Shape (9x7):
 *
 *  . # # # . # # # .
 *  # # # # # # # # #
 *  # # # # # # # # #
 *  . # # # # # # # .
 *  . . # # # # # . .
 *  . . . # # # . . .
 *  . . . . # . . . .
 *
 * '#' = filled pixel
 * '.' = empty pixel
 */
static void draw_heart_small(int x, int y, lv_color_t color)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.border_width = 0;
    dsc.radius = 0;

    /* Row 0: .###.###. */
    lv_canvas_draw_rect(g.canvas, x + 1, y + 0, 3, 1, &dsc);
    lv_canvas_draw_rect(g.canvas, x + 5, y + 0, 3, 1, &dsc);

    /* Row 1: ######### */
    lv_canvas_draw_rect(g.canvas, x + 0, y + 1, 9, 1, &dsc);

    /* Row 2: ######### */
    lv_canvas_draw_rect(g.canvas, x + 0, y + 2, 9, 1, &dsc);

    /* Row 3: .#######. */
    lv_canvas_draw_rect(g.canvas, x + 1, y + 3, 7, 1, &dsc);

    /* Row 4: ..#####.. */
    lv_canvas_draw_rect(g.canvas, x + 2, y + 4, 5, 1, &dsc);

    /* Row 5: ...###... */
    lv_canvas_draw_rect(g.canvas, x + 3, y + 5, 3, 1, &dsc);

    /* Row 6: ....#.... */
    lv_canvas_draw_rect(g.canvas, x + 4, y + 6, 1, 1, &dsc);
}

/* Draw a bigger pixel heart centered at (cx, cy)
 *
 * Base shape: 9x7
 * Scale     : 2x
 * Final size: 18x14
 */
static void draw_heart_big(int cx, int cy, lv_color_t color)
{
    static const uint8_t heart[7][9] = {
        {0,1,1,1,0,1,1,1,0},
        {1,1,1,1,1,1,1,1,1},
        {1,1,1,1,1,1,1,1,1},
        {0,1,1,1,1,1,1,1,0},
        {0,0,1,1,1,1,1,0,0},
        {0,0,0,1,1,1,0,0,0},
        {0,0,0,0,1,0,0,0,0},
    };

    const int scale = 2;
    const int w = 9 * scale;
    const int h = 7 * scale;

    int x0 = cx - w / 2;
    int y0 = cy - h / 2;

    for (int y = 0; y < 7; y++) {
        for (int x = 0; x < 9; x++) {
            if (heart[y][x]) {
                draw_dot(
                    x0 + x * scale,
                    y0 + y * scale,
                    scale,
                    color
                );
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Primitive: filled rounded rectangle                                        */
/* -------------------------------------------------------------------------- */

/*
 * Draws a filled rounded-rect on the canvas.
 * We scan-line it ourselves so we stay on LVGL v8 canvas API only.
 *   x, y = top-left corner   w, h = dimensions   r = corner radius
 */
static void draw_rrect(int x, int y, int w, int h, int r, lv_color_t color)
{
    if (w <= 0 || h <= 0)
        return;
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.border_width = 0;
    dsc.radius = 0;

    for (int row = y; row < y + h; row++)
    {
        if (row < 0 || row >= DISP_H)
            continue;

        int dy_top = row - y;           /* distance from top edge */
        int dy_bot = (y + h - 1) - row; /* distance from bottom edge */
        int margin = 0;

        /* Top-left / top-right corner zone */
        if (dy_top < r)
        {
            int fy = r - dy_top;
            /* x offset = r - sqrt(r²-fy²) */
            lv_sqrt_res_t sq;
            lv_sqrt((uint32_t)(r * r - fy * fy), &sq, 0x800);
            margin = r - (int)sq.i;
        }
        /* Bottom-left / bottom-right corner zone */
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
            continue;
        x1 = LV_MAX(x1, 0);
        x2 = LV_MIN(x2, DISP_W - 1);

        lv_canvas_draw_rect(g.canvas, x1, row, x2 - x1 + 1, 1, &dsc);
    }
}

/* Filled rectangle (no rounding) */
static void draw_rect(int x, int y, int w, int h, lv_color_t color)
{
    if (w <= 0 || h <= 0)
        return;
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.border_width = 0;
    dsc.radius = 0;
    x = LV_MAX(x, 0);
    y = LV_MAX(y, 0);
    if (x + w > DISP_W)
        w = DISP_W - x;
    if (y + h > DISP_H)
        h = DISP_H - y;
    if (w > 0 && h > 0)
        lv_canvas_draw_rect(g.canvas, x, y, w, h, &dsc);
}

/* Filled triangle (scanline) for angry brow cut */
static void draw_triangle(int x0, int y0, int x1, int y1, int x2, int y2, lv_color_t color)
{
    lv_point_t pts[3] = {{x0, y0}, {x1, y1}, {x2, y2}};
    /* sort by y */
    for (int i = 0; i < 2; i++)
        for (int j = i + 1; j < 3; j++)
            if (pts[j].y < pts[i].y)
            {
                lv_point_t t = pts[i];
                pts[i] = pts[j];
                pts[j] = t;
            }

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.border_width = 0;
    dsc.radius = 0;

    lv_point_t edges[3][2] = {{pts[0], pts[1]}, {pts[1], pts[2]}, {pts[0], pts[2]}};

    for (int y = pts[0].y; y <= pts[2].y; y++)
    {
        int xlo = DISP_W, xhi = -1;
        for (int e = 0; e < 3; e++)
        {
            int ay = edges[e][0].y, by = edges[e][1].y;
            int ax = edges[e][0].x, bx = edges[e][1].x;
            if (ay == by)
                continue;
            if (y < LV_MIN(ay, by) || y > LV_MAX(ay, by))
                continue;
            int x = ax + (bx - ax) * (y - ay) / (by - ay);
            if (x < xlo)
                xlo = x;
            if (x > xhi)
                xhi = x;
        }
        if (xhi < xlo || y < 0 || y >= DISP_H)
            continue;
        xlo = LV_MAX(xlo, 0);
        xhi = LV_MIN(xhi, DISP_W - 1);
        lv_canvas_draw_rect(g.canvas, xlo, y, xhi - xlo + 1, 1, &dsc);
    }
}

/* -------------------------------------------------------------------------- */
/* Render                                                                     */
/* -------------------------------------------------------------------------- */

static void render_frame(void)
{
    lv_canvas_fill_bg(g.canvas, lv_color_black(), LV_OPA_COVER);

    lv_color_t white = lv_color_white();
    lv_color_t black = lv_color_black();

    /* --- Blink: compute eye height scale (0.0–1.0) --- */
    float h_scale = 1.0f;
    if (g.expr == EYE_EXPR_SLEEPY)
    {
        h_scale = 0.38f;
    }
    else if (g.blink_phase == BLINK_CLOSING)
    {
        h_scale = 1.0f - (float)g.blink_step / BLINK_STEPS;
    }
    else if (g.blink_phase == BLINK_OPENING)
    {
        h_scale = (float)g.blink_step / BLINK_STEPS;
    }

    /* --- Expression: eye height multiplier --- */
    float expr_h = 1.0f;
    if (g.expr == EYE_EXPR_HAPPY)
        expr_h = 0.72f;
    if (g.expr == EYE_EXPR_SURPRISED)
        expr_h = 1.30f;
    if (g.expr == EYE_EXPR_WONDER)
        expr_h = 1.00f;
    if (g.expr == EYE_EXPR_CUTE)
        expr_h = 0.68f;
    if (g.expr == EYE_EXPR_CLOSE)
        expr_h = 0.12f;
    if (g.expr == EYE_EXPR_SUSPICIOUS)
        expr_h = 0.55f;
    if (g.expr == EYE_EXPR_SAD)
        expr_h = 0.55f;
    if (g.expr == EYE_EXPR_UPSET)
        expr_h = 0.55f;
    if (g.expr == EYE_EXPR_LOVE)
        expr_h = 1.05f;

    int eye_h = (int)(EYE_H * expr_h * h_scale);
    if (eye_h < 1)
        eye_h = 1;

    int eye_r = EYE_R;
    if (eye_r > eye_h / 2)
        eye_r = eye_h / 2;

    /* --- Draw both eyes  --- */
    int centres[2] = {L_CX, R_CX};

    for (int i = 0; i < 2; i++)
    {
        int cx = centres[i] + g.cur_dx;
        int cy = EYE_CY + g.cur_dy;

        if (g.expr == EYE_EXPR_SAD) {
            cy += 4;
        }

        if (g.expr == EYE_EXPR_CUTE) {
            cy += 2;
        }

        int this_eye_h = eye_h;

        /* --- WONDER: one eye smaller, one eye bigger --- */
        if (g.expr == EYE_EXPR_WONDER) {
            if (i == 0) {
                this_eye_h = eye_h - 8;   /* left eye smaller */
            } else {
                this_eye_h = eye_h + 4;   /* right eye bigger */
            }

            if (this_eye_h < 6) {
                this_eye_h = 6;
            }
        }

        int this_eye_r = eye_r;
        if (this_eye_r > this_eye_h / 2) {
            this_eye_r = this_eye_h / 2;
        }

        int ex = cx - EYE_W / 2;
        int ey = cy - this_eye_h / 2;

        /* --- White rounded rect --- */
        draw_rrect(ex, ey, EYE_W, this_eye_h, this_eye_r, white);

        /* --- LOVE: big heart pupils --- */
        if (g.expr == EYE_EXPR_LOVE && h_scale > 0.1f)
        {
            draw_heart_big(cx, cy, black);
        }

        /* --- HAPPY: clip bottom half → arch shape --- */
        if (g.expr == EYE_EXPR_HAPPY && h_scale > 0.1f)
        {
            /* fill the bottom portion black to flatten bottom */
            int cut = this_eye_h / 2;
            draw_rect(ex - 1, ey + cut, EYE_W + 2, this_eye_h - cut + 2, black);
        }

        /* --- ANGRY: black triangle cuts top-inner corner --- */
        if (g.expr == EYE_EXPR_ANGRY && h_scale > 0.1f)
        {
            int cut_w = EYE_W / 2 + 2;
            int cut_h = eye_h / 2 + 2;
            if (i == 0)
            {
                /* Left eye: cut top-right (inner) corner */
                draw_triangle(
                    ex + EYE_W - cut_w, ey, ex + EYE_W, ey, ex + EYE_W, ey + cut_h, black);
            }
            else
            {
                /* Right eye: cut top-left (inner) corner */
                draw_triangle(ex, ey, ex + cut_w, ey, ex, ey + cut_h, black);
            }
        }

        /* --- CUTE: kawaii smiling eyes --- */
        if (g.expr == EYE_EXPR_CUTE && h_scale > 0.1f) {

            /*
             * Make the eye like a smiling crescent.
             * We erase the upper-middle part, leaving a cute curved bottom.
             */

            int cut_y = ey;
            int cut_h = this_eye_h / 2 + 4;

            draw_rect(
                ex + 4,
                cut_y,
                EYE_W - 8,
                cut_h,
                black
            );

            /*
             * Add small outside cheek pixels.
             * Use WHITE because background is black.
             */
            if (i == 0) {
                draw_dot(ex - 3, cy + 5, 2, white);
                draw_dot(ex - 6, cy + 7, 2, white);
            } else {
                draw_dot(ex + EYE_W + 1, cy + 5, 2, white);
                draw_dot(ex + EYE_W + 4, cy + 7, 2, white);
            }
        }

        /* --- SUSPICIOUS: unimpressed side-eye --- */
        if (g.expr == EYE_EXPR_SUSPICIOUS && h_scale > 0.1f) {

            /*
             * Narrow the eyes by cutting top and bottom.
             * This makes both eyes look unimpressed/suspicious.
             */
            draw_rect(ex, ey, EYE_W, 4, black);
            draw_rect(ex, ey + this_eye_h - 4, EYE_W, 4, black);

            /*
             * Both pupils look to the same side.
             */
            int pupil_size = 5;
            int px = cx - 9;   /* look left */
            int py = ey + this_eye_h / 2;

            draw_dot(
                px - pupil_size / 2,
                py - pupil_size / 2,
                pupil_size,
                black
            );
        }

        /* --- SAD: drooping outer upper corners --- */       
        if (g.expr == EYE_EXPR_SAD && h_scale > 0.1f) {
            int cut_w = EYE_W / 2;
            int cut_h = this_eye_h / 2;

            if (i == 0) {
                /* Left eye: cut top-left outer corner */
                draw_triangle(
                    ex,           ey,
                    ex + cut_w,   ey,
                    ex,           ey + cut_h,
                    black);
            } else {
                /* Right eye: cut top-right outer corner */
                draw_triangle(
                    ex + EYE_W - cut_w, ey,
                    ex + EYE_W,         ey,
                    ex + EYE_W,         ey + cut_h,
                    black);
            }
        }

        /* --- WONDER: confused / curious eyes --- */
        if (g.expr == EYE_EXPR_WONDER && h_scale > 0.1f) {

            /*
             * Pupils look upward-left.
             * Bigger pupil so it is visible on OLED.
             */
            int pupil_size = 6;
            int px = cx - 6;
            int py = ey + 7;

            draw_dot(
                px - pupil_size / 2,
                py - pupil_size / 2,
                pupil_size,
                black
            );

            /*
             * Add a tiny "thinking" dot near the bigger eye.
             * Only draw it on the right eye.
             */
            if (i == 1) {
                draw_dot(ex + EYE_W + 4, ey + 2, 2, white);
                draw_dot(ex + EYE_W + 7, ey - 2, 2, white);
            }
        }

        /* --- UPSET: aggressive squint --- */
        if (g.expr == EYE_EXPR_UPSET && h_scale > 0.1f)
        {
            int cut_w = EYE_W / 2 + 4;
            int cut_h = this_eye_h;

            if (i == 0) {
                /* Left eye inner slash */
                draw_triangle(
                    ex + EYE_W - cut_w, ey,
                    ex + EYE_W,         ey,
                    ex + EYE_W,         ey + cut_h,
                    black);
            } else {
                /* Right eye inner slash */
                draw_triangle(
                    ex,         ey,
                    ex + cut_w, ey,
                    ex,         ey + cut_h,
                    black);
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
        return;
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
        g.tgt_dx = g.tgt_dy = 0;
        g.look_return_ms = 0;
    }
    int step = 2;
    if (g.cur_dx < g.tgt_dx)
        g.cur_dx = LV_MIN(g.cur_dx + step, g.tgt_dx);
    else if (g.cur_dx > g.tgt_dx)
        g.cur_dx = LV_MAX(g.cur_dx - step, g.tgt_dx);
    if (g.cur_dy < g.tgt_dy)
        g.cur_dy = LV_MIN(g.cur_dy + step, g.tgt_dy);
    else if (g.cur_dy > g.tgt_dy)
        g.cur_dy = LV_MAX(g.cur_dy - step, g.tgt_dy);
}

static void idle_tick(void)
{
    if (!g.idle_enabled)
        return;
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

    /* Auto-return to NORMAL after hold period */
    if (g.expr_return_ms && t >= g.expr_return_ms)
    {
        g.expr = EYE_EXPR_NORMAL;
        g.expr_return_ms = 0;
        g.next_expr_ms = t + rand_range(IDLE_EXPR_MIN, IDLE_EXPR_MAX);
    }

    /* Fire a random non-NORMAL expression */
    if (g.expr == EYE_EXPR_NORMAL && t >= g.next_expr_ms)
    {
        /* Pick from expression only */
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
        /* next_expr_ms is set again when we return to NORMAL above */
    }
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
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
    ESP_LOGI(TAG, "eye_anim (Akno) initialised");
}

void eye_anim_tick(void)
{
    idle_tick();
    blink_tick();
    look_tick();
    render_frame();
}

void eye_anim_blink(void)
{
    if (g.blink_phase != BLINK_IDLE)
        return;
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
        g.look_return_ms = now_ms() + LOOK_HOLD_MS;
}

void eye_anim_set_expression(eye_expression_t expr)
{
    if (expr >= EYE_EXPR_COUNT)
        return;
    g.expr = expr;
    ESP_LOGI(TAG, "expression -> %d", expr);
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

static void eye_test_all_task(void *arg)
{
    (void)arg;

    typedef struct {
        eye_expression_t expr;
        const char *name;
    } eye_test_item_t;

    static const eye_test_item_t tests[] = {
        { EYE_EXPR_NORMAL,     "NORMAL" },
        { EYE_EXPR_HAPPY,      "HAPPY" },
        { EYE_EXPR_ANGRY,      "ANGRY" },
        { EYE_EXPR_SLEEPY,     "SLEEPY" },
        { EYE_EXPR_SURPRISED,  "SURPRISED" },
        { EYE_EXPR_WONDER,     "WONDER" },
        { EYE_EXPR_CUTE,       "CUTE" },
        { EYE_EXPR_SUSPICIOUS, "SUSPICIOUS" },
        { EYE_EXPR_SAD,        "SAD" },
        { EYE_EXPR_CLOSE,      "CLOSE" },
        { EYE_EXPR_UPSET,      "UPSET" },
        { EYE_EXPR_LOVE,       "LOVE" },
    };

    eye_anim_set_idle(false);

    while (1) {
        const int count = sizeof(tests) / sizeof(tests[0]);
        for (int i = 0; i < count; i++) {
            ESP_LOGI("EYE_TEST", "Testing eye: %s", tests[i].name);

            eye_anim_set_expression(tests[i].expr);

            if (tests[i].expr != EYE_EXPR_CLOSE) {
                eye_anim_blink();
            }

            vTaskDelay(pdMS_TO_TICKS(2500));
        }
    }
}