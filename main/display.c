/*
 * DeskBuddy — display.c
 * LVGL v8 + esp_lcd SSD1306 + esp_lvgl_port
 *
 * Screen layout:
 *   SCREEN_FACE    — static smiley drawn with LVGL primitives
 *   SCREEN_REACT   — cycles through reaction strings on each press
 *   SCREEN_CLOCK   — live HH:MM:SS on line 1, date on line 2
 *   SCREEN_WEATHER — condition icon + temperature
 *
 * The clock screen refreshes itself every second via an lv_timer.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/i2c_master.h"   /* IDF v5.1+ new I2C master driver */

#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "ntp_sync.h"
#include "display.h"

static const char *TAG = "display";

/* ── I2C / LCD handles ───────────────────────────────────────────────── */
static esp_lcd_panel_handle_t   s_panel      = NULL;
static lv_disp_t               *s_disp       = NULL;

/* ── Screen state ────────────────────────────────────────────────────── */
static display_screen_t  s_current_screen = SCREEN_FACE;
static lv_obj_t         *s_screens[SCREEN_COUNT]; /* one lv_obj per screen */

/* ── Clock screen widgets (updated by timer) ─────────────────────────── */
static lv_obj_t *s_lbl_time = NULL;
static lv_obj_t *s_lbl_date = NULL;

/* ── Weather screen widgets ──────────────────────────────────────────── */
static lv_obj_t *s_lbl_weather_cond = NULL;
static lv_obj_t *s_lbl_weather_temp = NULL;

/* ── Reaction strings ────────────────────────────────────────────────── */
static const char *REACTIONS[] = {
    "(^_^)", "(>_<)", "(o_O)", "(T_T)",
    "(*_*)", "(._.) ", "\\(^o^)/", "(-.-)zzZ"
};
#define REACTION_COUNT (sizeof(REACTIONS) / sizeof(REACTIONS[0]))
static uint8_t s_reaction_idx = 0;

/* ───────────────────────────────────────────────────────────────────── */
/*  Forward declarations                                                  */
/* ───────────────────────────────────────────────────────────────────── */
static void build_screen_face(lv_obj_t *parent);
static void build_screen_react(lv_obj_t *parent);
static void build_screen_clock(lv_obj_t *parent);
static void build_screen_weather(lv_obj_t *parent);
static void clock_timer_cb(lv_timer_t *timer);
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map);

/* ───────────────────────────────────────────────────────────────────── */
/*  LCD + LVGL init                                                       */
/* ───────────────────────────────────────────────────────────────────── */
void display_init(void)
{
    ESP_LOGI(TAG, "%s", "Initialising display");

    /* ── I2C master bus (new IDF v5.1+ driver) ─────────────────────── */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = DISPLAY_SDA_GPIO,
        .scl_io_num        = DISPLAY_SCL_GPIO,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));

    /* ── I2C scanner — logs address so we can verify wiring ─────────── */
    ESP_LOGI(TAG, "%s", "Scanning I2C bus...");
    bool found = false;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        esp_err_t probe = i2c_master_probe(i2c_bus, addr, pdMS_TO_TICKS(50));
        if (probe == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at 0x%02X", addr);
            found = true;
        }
    }
    if (!found) {
        ESP_LOGE(TAG, "%s", "No I2C devices found! Check SDA/SCL wiring.");
    }

    /* ── esp_lcd panel IO — 100kHz for breadboard stability ────────── */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr            = DISPLAY_I2C_ADDR,
        .control_phase_bytes = 1,
        .dc_bit_offset       = 6,    /* SSD1306 driver manages control byte internally */
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
        .scl_speed_hz        = 100000,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &io_cfg, &io_handle));

    /* ── SSD1306 panel ─────────────────────────────────────────────── */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(io_handle, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    vTaskDelay(pdMS_TO_TICKS(50));   /* wait after reset */
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    vTaskDelay(pdMS_TO_TICKS(50));   /* wait after init  */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true)); /* black bg = pixels off = less power */
    vTaskDelay(pdMS_TO_TICKS(100));  /* panel stabilise  */
    ESP_LOGI(TAG, "%s", "SSD1306 panel ready");

    /* ── esp_lvgl_port ─────────────────────────────────────────────── */
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io_handle,
        .panel_handle  = s_panel,
        /* Full framebuffer: 128×64 bits = 1024 bytes.
         * Partial buffers cause garbage on mono SSD1306 because
         * LVGL flushes in bands and the panel expects full pages. */
        .buffer_size   = DISPLAY_WIDTH * DISPLAY_HEIGHT,
        .double_buffer = false,
        .hres          = DISPLAY_WIDTH,
        .vres          = DISPLAY_HEIGHT,
        .monochrome    = true,
        .rotation = {
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = false,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);

    /* Rotate so origin is top-left (SSD1306 default is fine) */
    lv_disp_set_rotation(s_disp, LV_DISP_ROT_NONE);

    /* ── Build all screens ─────────────────────────────────────────── */
    lvgl_port_lock(0);

    for (int i = 0; i < SCREEN_COUNT; i++) {
        s_screens[i] = lv_obj_create(NULL);       /* headless screen obj */
        lv_obj_set_style_bg_color(s_screens[i], lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_screens[i], LV_OPA_COVER, 0);
    }

    build_screen_face(s_screens[SCREEN_FACE]);
    build_screen_react(s_screens[SCREEN_REACT]);
    build_screen_clock(s_screens[SCREEN_CLOCK]);
    build_screen_weather(s_screens[SCREEN_WEATHER]);

    /* Start on idle face */
    lv_disp_load_scr(s_screens[SCREEN_FACE]);

    /* 1-second timer to refresh clock */
    lv_timer_create(clock_timer_cb, 1000, NULL);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "%s", "Display ready");
}

/* ───────────────────────────────────────────────────────────────────── */
/*  Screen builders                                                       */
/* ───────────────────────────────────────────────────────────────────── */

static void build_screen_face(lv_obj_t *parent)
{
    /* Simple ASCII-art smiley centred on screen */
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, "(^_^)\nDeskBuddy");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
}

static void build_screen_react(lv_obj_t *parent)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, REACTIONS[0]);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_user_data(parent, lbl); /* store ref for later update */
}

static void build_screen_clock(lv_obj_t *parent)
{
    /* Time label — large, centred, upper half */
    s_lbl_time = lv_label_create(parent);
    lv_label_set_text(s_lbl_time, "--:--:--");
    lv_obj_set_style_text_color(s_lbl_time, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_lbl_time, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_time, LV_ALIGN_CENTER, 0, -10);

    /* Date label — smaller, below time */
    s_lbl_date = lv_label_create(parent);
    lv_label_set_text(s_lbl_date, "--- -- --- ----");
    lv_obj_set_style_text_color(s_lbl_date, lv_color_white(), 0);
    lv_obj_align(s_lbl_date, LV_ALIGN_CENTER, 0, 16);
}

static void build_screen_weather(lv_obj_t *parent)
{
    /* Header */
    lv_obj_t *lbl_hdr = lv_label_create(parent);
    lv_label_set_text(lbl_hdr, "Weather");
    lv_obj_set_style_text_color(lbl_hdr, lv_color_white(), 0);
    lv_obj_align(lbl_hdr, LV_ALIGN_TOP_MID, 0, 4);

    /* Condition */
    s_lbl_weather_cond = lv_label_create(parent);
    lv_label_set_text(s_lbl_weather_cond, "---");
    lv_obj_set_style_text_color(s_lbl_weather_cond, lv_color_white(), 0);
    lv_obj_align(s_lbl_weather_cond, LV_ALIGN_CENTER, 0, -6);

    /* Temperature */
    s_lbl_weather_temp = lv_label_create(parent);
    lv_label_set_text(s_lbl_weather_temp, "--.-C");
    lv_obj_set_style_text_color(s_lbl_weather_temp, lv_color_white(), 0);
    lv_obj_align(s_lbl_weather_temp, LV_ALIGN_CENTER, 0, 14);
}

/* ───────────────────────────────────────────────────────────────────── */
/*  Clock timer callback — fires every 1 s                               */
/* ───────────────────────────────────────────────────────────────────── */
static void clock_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (s_current_screen != SCREEN_CLOCK) return;

    char time_buf[16];
    char date_buf[24];
    ntp_get_time_str(time_buf, sizeof(time_buf));
    ntp_get_date_str(date_buf, sizeof(date_buf));

    lv_label_set_text(s_lbl_time, time_buf);
    lv_label_set_text(s_lbl_date, date_buf);
}

/* ───────────────────────────────────────────────────────────────────── */
/*  Public API                                                            */
/* ───────────────────────────────────────────────────────────────────── */

void display_next_screen(void)
{
    s_current_screen = (display_screen_t)((s_current_screen + 1) % SCREEN_COUNT);

    /* Advance reaction index when entering SCREEN_REACT */
    if (s_current_screen == SCREEN_REACT) {
        s_reaction_idx = (s_reaction_idx + 1) % REACTION_COUNT;
        lv_obj_t *lbl = (lv_obj_t *)lv_obj_get_user_data(
                            s_screens[SCREEN_REACT]);
        if (lbl) {
            lv_label_set_text(lbl, REACTIONS[s_reaction_idx]);
        }
    }

    lvgl_port_lock(0);
    lv_disp_load_scr(s_screens[s_current_screen]);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Screen -> %d", s_current_screen);
}

void display_set_screen(display_screen_t screen)
{
    if (screen >= SCREEN_COUNT) return;
    s_current_screen = screen;

    lvgl_port_lock(0);
    lv_disp_load_scr(s_screens[s_current_screen]);
    lvgl_port_unlock();
}

/* Called from weather module via lv_async_call wrapper */
typedef struct {
    char condition[32];
    float temp_c;
} weather_update_t;

static void weather_async_cb(void *user_data)
{
    weather_update_t *d = (weather_update_t *)user_data;
    if (!d) return;

    char temp_str[16];
    snprintf(temp_str, sizeof(temp_str), "%.1f C", d->temp_c);
    lv_label_set_text(s_lbl_weather_cond, d->condition);
    lv_label_set_text(s_lbl_weather_temp, temp_str);

    free(d);
}

void display_update_weather(const char *condition, float temp_c)
{
    weather_update_t *d = malloc(sizeof(weather_update_t));
    if (!d) return;

    strncpy(d->condition, condition, sizeof(d->condition) - 1);
    d->condition[sizeof(d->condition) - 1] = '\0';
    d->temp_c = temp_c;

    lv_async_call(weather_async_cb, d);
}

void display_tick(void)
{
    lv_tick_inc(1);
}

/* ───────────────────────────────────────────────────────────────────── */
/*  Flush callback (reference only — esp_lvgl_port registers its own)    */
/* ───────────────────────────────────────────────────────────────────── */
static void __attribute__((unused))
flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    /* esp_lvgl_port registers its own flush — this is kept as reference */
    (void)drv;
    (void)area;
    (void)color_map;
}