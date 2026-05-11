/* DeskBuddy — display.c
 * LVGL v8 + esp_lcd SSD1306 + esp_lvgl_port
 *
 * Screens:
 *   SCREEN_FACE    — Akno-style animated eyes (eye_anim module)
 *   SCREEN_CLOCK   — live HH:MM:SS + date
 *   SCREEN_WEATHER — condition + temperature
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/i2c_master.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "ntp_sync.h"
#include "display.h"
#include "eye_anim.h"

static const char *TAG = "display";

/* ── Handles ─────────────────────────────────────────────── */
static esp_lcd_panel_handle_t  s_panel = NULL;
static lv_disp_t              *s_disp  = NULL;

/* ── Screen state ────────────────────────────────────────── */
static display_screen_t  s_current_screen = SCREEN_FACE;
static lv_obj_t         *s_screens[SCREEN_COUNT];

/* ── Clock widgets ───────────────────────────────────────── */
static lv_obj_t *s_lbl_time = NULL;
static lv_obj_t *s_lbl_date = NULL;

/* ── Weather widgets ─────────────────────────────────────── */
static lv_obj_t *s_lbl_weather_cond = NULL;
static lv_obj_t *s_lbl_weather_temp = NULL;

/* ── Eye expression cycling ──────────────────────────────── */
static const eye_expression_t EYE_EXPRESSIONS[] = {
    EYE_EXPR_NORMAL,
    EYE_EXPR_HAPPY,
    EYE_EXPR_ANGRY,
    EYE_EXPR_SLEEPY,
    EYE_EXPR_SURPRISED,
};
#define EYE_EXPR_COUNT_LOCAL (sizeof(EYE_EXPRESSIONS) / sizeof(EYE_EXPRESSIONS[0]))
static uint8_t s_eye_expr_idx = 0;

/* ── Forward declarations ────────────────────────────────── */
static void build_screen_face(lv_obj_t *parent);
static void build_screen_clock(lv_obj_t *parent);
static void build_screen_weather(lv_obj_t *parent);
static void build_screen_wifi(lv_obj_t *parent);
static void clock_timer_cb(lv_timer_t *timer);
static void eye_timer_cb(lv_timer_t *timer);

/* ───────────────────────────────────────────────────────── */
/*  display_init                                             */
/* ───────────────────────────────────────────────────────── */
void display_init(void)
{
    ESP_LOGI(TAG, "%s", "Initialising display");

    /* I2C master bus */
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

    /* I2C scanner */
    ESP_LOGI(TAG, "%s", "Scanning I2C bus...");
    bool found = false;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(i2c_bus, addr, pdMS_TO_TICKS(50)) == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at 0x%02X", addr);
            found = true;
        }
    }
    if (!found) ESP_LOGE(TAG, "%s", "No I2C devices found!");

    /* Panel IO */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr            = DISPLAY_I2C_ADDR,
        .control_phase_bytes = 1,
        .dc_bit_offset       = 6,
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
        .scl_speed_hz        = 100000,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &io_cfg, &io_handle));

    /* SSD1306 */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(io_handle, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "%s", "SSD1306 ready");

    /* LVGL port */
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io_handle,
        .panel_handle  = s_panel,
        .buffer_size   = DISPLAY_WIDTH * DISPLAY_HEIGHT,
        .double_buffer = false,
        .hres          = DISPLAY_WIDTH,
        .vres          = DISPLAY_HEIGHT,
        .monochrome    = true,
        .rotation      = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    lv_disp_set_rotation(s_disp, LV_DISP_ROT_NONE);

    /* Build screens */
    lvgl_port_lock(0);

    for (int i = 0; i < SCREEN_COUNT; i++) {
        s_screens[i] = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_screens[i], lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_screens[i], LV_OPA_COVER, 0);
    }

    build_screen_face(s_screens[SCREEN_FACE]);
    build_screen_clock(s_screens[SCREEN_CLOCK]);
    build_screen_weather(s_screens[SCREEN_WEATHER]);
    build_screen_wifi(s_screens[SCREEN_WIFI]);

    lv_disp_load_scr(s_screens[SCREEN_FACE]);

    lv_timer_create(clock_timer_cb, 1000, NULL);

    lv_timer_t *eye_timer = lv_timer_create(eye_timer_cb, 20, NULL);
    lv_timer_set_repeat_count(eye_timer, -1);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "%s", "Display ready");
}

/* ───────────────────────────────────────────────────────── */
/*  Screen builders                                          */
/* ───────────────────────────────────────────────────────── */

static void build_screen_face(lv_obj_t *parent)
{
    eye_anim_init(parent);
}

/* ── WiFi setup screen ────────────────────────────────────── */
static lv_obj_t *s_lbl_wifi_info = NULL;

static void build_screen_wifi(lv_obj_t *parent)
{
    /* Title */
    lv_obj_t *lbl_hdr = lv_label_create(parent);
    lv_label_set_text(lbl_hdr, "WiFi Setup");
    lv_obj_set_style_text_color(lbl_hdr, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_hdr, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_hdr, LV_ALIGN_TOP_MID, 0, 2);

    /* Body text */
    s_lbl_wifi_info = lv_label_create(parent);
    lv_label_set_text(s_lbl_wifi_info,
                      "Connect to:\n"
                      "Buddy-Setup\n"
                      "\n"
                      "192.168.4.1");

    lv_obj_set_style_text_color(s_lbl_wifi_info, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_lbl_wifi_info, &lv_font_montserrat_10, 0);

    /* Center text on each line */
    lv_obj_set_width(s_lbl_wifi_info, 128);
    lv_obj_set_style_text_align(s_lbl_wifi_info, LV_TEXT_ALIGN_CENTER, 0);

    /* Position under the title */
    lv_obj_align(s_lbl_wifi_info, LV_ALIGN_TOP_MID, 0, 20);

    /* Optional: allow wrapping if text is too long */
    lv_label_set_long_mode(s_lbl_wifi_info, LV_LABEL_LONG_WRAP);
}

static void build_screen_clock(lv_obj_t *parent)
{
    s_lbl_time = lv_label_create(parent);
    lv_label_set_text(s_lbl_time, "--:--:--");
    lv_obj_set_style_text_color(s_lbl_time, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_lbl_time, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_time, LV_ALIGN_CENTER, 0, -10);

    s_lbl_date = lv_label_create(parent);
    lv_label_set_text(s_lbl_date, "--- -- --- ----");
    lv_obj_set_style_text_color(s_lbl_date, lv_color_white(), 0);
    lv_obj_align(s_lbl_date, LV_ALIGN_CENTER, 0, 16);
}

static void build_screen_weather(lv_obj_t *parent)
{
    lv_obj_t *lbl_hdr = lv_label_create(parent);
    lv_label_set_text(lbl_hdr, "Weather");
    lv_obj_set_style_text_color(lbl_hdr, lv_color_white(), 0);
    lv_obj_align(lbl_hdr, LV_ALIGN_TOP_MID, 0, 4);

    s_lbl_weather_cond = lv_label_create(parent);
    lv_label_set_text(s_lbl_weather_cond, "---");
    lv_obj_set_style_text_color(s_lbl_weather_cond, lv_color_white(), 0);
    lv_obj_align(s_lbl_weather_cond, LV_ALIGN_CENTER, 0, -6);

    s_lbl_weather_temp = lv_label_create(parent);
    lv_label_set_text(s_lbl_weather_temp, "--.-C");
    lv_obj_set_style_text_color(s_lbl_weather_temp, lv_color_white(), 0);
    lv_obj_align(s_lbl_weather_temp, LV_ALIGN_CENTER, 0, 14);
}

/* ───────────────────────────────────────────────────────── */
/*  Timer callbacks                                          */
/* ───────────────────────────────────────────────────────── */

static void clock_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_current_screen != SCREEN_CLOCK) return;
    char time_buf[16], date_buf[24];
    ntp_get_time_str(time_buf, sizeof(time_buf));
    ntp_get_date_str(date_buf, sizeof(date_buf));
    lv_label_set_text(s_lbl_time, time_buf);
    lv_label_set_text(s_lbl_date, date_buf);
}

static void eye_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_current_screen != SCREEN_FACE) return;
    eye_anim_tick();
}

/* ───────────────────────────────────────────────────────── */
/*  Public API                                               */
/* ───────────────────────────────────────────────────────── */

void display_next_screen(void)
{
    display_screen_t prev = s_current_screen;
    s_current_screen = (display_screen_t)((s_current_screen + 1) % SCREEN_WIFI);  /* skip SCREEN_WIFI from normal cycle */

    if (prev == SCREEN_FACE && s_current_screen != SCREEN_FACE)
        eye_anim_set_idle(false);
    else if (s_current_screen == SCREEN_FACE)
        eye_anim_set_idle(true);

    lvgl_port_lock(0);
    lv_disp_load_scr(s_screens[s_current_screen]);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Screen -> %d", s_current_screen);
}

void display_set_screen(display_screen_t screen)
{
    if (screen >= SCREEN_COUNT) return;
    display_screen_t prev = s_current_screen;
    s_current_screen = screen;

    if (prev == SCREEN_FACE && s_current_screen != SCREEN_FACE)
        eye_anim_set_idle(false);
    else if (s_current_screen == SCREEN_FACE)
        eye_anim_set_idle(true);

    lvgl_port_lock(0);
    lv_disp_load_scr(s_screens[s_current_screen]);
    lvgl_port_unlock();
}

display_screen_t display_get_screen(void)
{
    return s_current_screen;
}

void display_show_wifi_setup(void)
{
    eye_anim_set_idle(false);
    lvgl_port_lock(0);
    s_current_screen = SCREEN_WIFI;
    lv_disp_load_scr(s_screens[SCREEN_WIFI]);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "%s", "WiFi setup screen shown");
}

void display_face_next_expression(void)
{
    s_eye_expr_idx = (s_eye_expr_idx + 1) % EYE_EXPR_COUNT_LOCAL;
    eye_anim_set_expression(EYE_EXPRESSIONS[s_eye_expr_idx]);
    ESP_LOGI(TAG, "Eye expression -> %d", EYE_EXPRESSIONS[s_eye_expr_idx]);
}

/* Weather update — safe from any task */
typedef struct { char condition[32]; float temp_c; } weather_update_t;

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