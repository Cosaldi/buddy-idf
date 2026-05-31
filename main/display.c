/* -------------------------------------------------------------------------- */
/* Buddy — display.c                                                          */
/*                                                                            */
/* LVGL v8 + esp_lcd SSD1306 + esp_lvgl_port                                  */
/* -------------------------------------------------------------------------- */
/* 
 * Screens:
 *   SCREEN_FACE    — Akno-style animated eyes (eye_anim module)
 *   SCREEN_CLOCK   — live HH:MM:SS + date
 *   SCREEN_WEATHER — condition + temperature
 */

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
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
#include "weather.h"
#include "battery.h"

#include "sdkconfig.h"
#include "widgets/lv_label.h"

#if CONFIG_DISPLAY_CONTROLLER_SH1106
#include "esp_lcd_panel_sh1106.h"
#endif

static const char *TAG = "display";

#define SPLASH_DURATION_MS 5000

static lv_timer_t *s_splash_timer = NULL;

/* --- Handles --- */
static esp_lcd_panel_handle_t s_panel = NULL;
static lv_disp_t *s_disp = NULL;

/* --- Screen state --- */
static display_screen_t s_current_screen = SCREEN_SPLASH;
static lv_obj_t *s_screens[SCREEN_COUNT];

/* --- Clock widgets --- */
static lv_obj_t *s_lbl_time = NULL;
static lv_obj_t *s_lbl_date = NULL;
static lv_obj_t *s_lbl_battery = NULL;

/* --- Weather widgets --- */
static lv_obj_t *s_lbl_weather_title = NULL;
static lv_obj_t *s_lbl_weather_cond = NULL;
static lv_obj_t *s_lbl_weather_temp = NULL;
static lv_obj_t *s_lbl_forecast[WEATHER_FORECAST_MAX] = {0};

static lv_obj_t *s_lbl_sync_title = NULL;
static lv_obj_t *s_lbl_sync_status = NULL;
static lv_obj_t *s_lbl_sync_last = NULL;

static bool s_weather_forecast_mode = false;

/* --- Eye expression cycling --- */
static const eye_expression_t EYE_EXPRESSIONS[] = {
    EYE_EXPR_NORMAL,
    EYE_EXPR_HAPPY,
    EYE_EXPR_ANGRY,
    EYE_EXPR_SLEEPY,
    EYE_EXPR_SURPRISED,
};
#define EYE_EXPR_COUNT_LOCAL (sizeof(EYE_EXPRESSIONS) / sizeof(EYE_EXPRESSIONS[0]))
static uint8_t s_eye_expr_idx = 0;

static const char *EYE_TEST_TAG = "EYE_TEST";

/* --- Forward declarations --- */
static void eye_test_all_task(void *arg);
static void build_screen_face(lv_obj_t *parent);
static void build_screen_clock(lv_obj_t *parent);
static void build_screen_weather(lv_obj_t *parent);
static void build_screen_portal(lv_obj_t *parent);
static void build_screen_splash(lv_obj_t *parent);
static void build_screen_birthday(lv_obj_t *parent);
static void build_screen_sync(lv_obj_t *parent);
static void clock_timer_cb(lv_timer_t *timer);
static void eye_timer_cb(lv_timer_t *timer);
static void splash_timer_cb(lv_timer_t *timer);

static bool s_display_on = true;
static uint32_t s_last_activity_ms = 0;

/* -------------------------------------------------------------------------- */
/* Display init                                                               */
/* -------------------------------------------------------------------------- */

void display_init(void)
{
    ESP_LOGI(TAG, "%s", "Initialising display");

    /* I2C master bus */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = DISPLAY_SDA_GPIO,
        .scl_io_num = DISPLAY_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));

    /* I2C scanner */
    ESP_LOGI(TAG, "%s", "Scanning I2C bus...");
    bool found = false;
    for (uint8_t addr = 0x08; addr < 0x78; addr++)
    {
        if (i2c_master_probe(i2c_bus, addr, pdMS_TO_TICKS(50)) == ESP_OK)
        {
            ESP_LOGI(TAG, "I2C device found at 0x%02X", addr);
            found = true;
        }
    }
    if (!found)
        ESP_LOGE(TAG, "%s", "No I2C devices found!");

    /* Panel IO */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = DISPLAY_I2C_ADDR,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &io_cfg, &io_handle));

    /* OLED panel */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
    };

#if CONFIG_DISPLAY_CONTROLLER_SSD1306

    ESP_LOGI(TAG, "Using SSD1306 OLED driver");

    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(io_handle, &panel_cfg, &s_panel));

#elif CONFIG_DISPLAY_CONTROLLER_SH1106

    ESP_LOGI(TAG, "Using SH1106 OLED driver");

    ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(io_handle, &panel_cfg, &s_panel));

#else
#error "No OLED display controller selected"
#endif

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

#if CONFIG_DISPLAY_CONTROLLER_SH1106
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, CONFIG_DISPLAY_SH1106_GAP_X, 0));
#endif

    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

#if CONFIG_DISPLAY_CONTROLLER_SH1106
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, false));
#else
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
#endif

    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "OLED panel ready");

    /* LVGL port */
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = s_panel,
        .buffer_size = DISPLAY_WIDTH * DISPLAY_HEIGHT,
        .double_buffer = false,
        .hres = DISPLAY_WIDTH,
        .vres = DISPLAY_HEIGHT,
        .monochrome = true,
        .rotation =
            {
#if CONFIG_DISPLAY_CONTROLLER_SH1106
                .swap_xy = true,
                .mirror_x = true,
                .mirror_y = true,
#else
                .swap_xy = false,
                .mirror_x = false,
                .mirror_y = false,
#endif
            },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    lv_disp_set_rotation(s_disp, LV_DISP_ROT_NONE);

    /* Build screens */
    lvgl_port_lock(0);

    for (int i = 0; i < SCREEN_COUNT; i++)
    {
        s_screens[i] = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_screens[i], lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_screens[i], LV_OPA_COVER, 0);
    }

    build_screen_splash(s_screens[SCREEN_SPLASH]);
    build_screen_face(s_screens[SCREEN_FACE]);
    build_screen_clock(s_screens[SCREEN_CLOCK]);
    build_screen_weather(s_screens[SCREEN_WEATHER]);
    build_screen_sync(s_screens[SCREEN_SYNC]);
    build_screen_portal(s_screens[SCREEN_PORTAL]);
    build_screen_birthday(s_screens[SCREEN_BIRTHDAY]);

    lv_disp_load_scr(s_screens[SCREEN_SPLASH]);

    s_splash_timer = lv_timer_create(splash_timer_cb, SPLASH_DURATION_MS, NULL);
    lv_timer_set_repeat_count(s_splash_timer, 1);

    lv_timer_create(clock_timer_cb, 1000, NULL);

    lv_timer_t *eye_timer = lv_timer_create(eye_timer_cb, 20, NULL);
    lv_timer_set_repeat_count(eye_timer, -1);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "%s", "Display ready");
}

/* -------------------------------------------------------------------------- */
/* Screen builders                                                            */
/* -------------------------------------------------------------------------- */

static void build_screen_splash(lv_obj_t *parent)
{
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "Buddy");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -12);

    lv_obj_t *sub = lv_label_create(parent);
    lv_label_set_text(sub, "for someone special");
    lv_obj_set_style_text_color(sub, lv_color_white(), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_10, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 12);
}

static void build_screen_birthday(lv_obj_t *parent)
{
    lv_obj_t *line1 = lv_label_create(parent);
    lv_label_set_text(line1, "Happy Birthday!");
    lv_obj_set_style_text_color(line1, lv_color_white(), 0);
    lv_obj_set_style_text_font(line1, &lv_font_montserrat_14, 0);
    lv_obj_align(line1, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *line2 = lv_label_create(parent);
    lv_label_set_text(line2, "Made just");
    lv_obj_set_style_text_color(line2, lv_color_white(), 0);
    lv_obj_set_style_text_font(line2, &lv_font_montserrat_10, 0);
    lv_obj_align(line2, LV_ALIGN_CENTER, 0, 6);

    lv_obj_t *line3 = lv_label_create(parent);
    lv_label_set_text(line3, "for you <3");
    lv_obj_set_style_text_color(line3, lv_color_white(), 0);
    lv_obj_set_style_text_font(line3, &lv_font_montserrat_10, 0);
    lv_obj_align(line3, LV_ALIGN_CENTER, 0, 22);
}

static void build_screen_face(lv_obj_t *parent)
{
    eye_anim_init(parent);

    // xTaskCreate(eye_test_all_task, "eye_test_all", 4096, NULL, 5, NULL);
}

/* --- WiFi setup screen --- */
static lv_obj_t *s_lbl_wifi_info = NULL;

static void build_screen_portal(lv_obj_t *parent)
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

    s_lbl_battery = lv_label_create(parent);
    lv_label_set_text(s_lbl_battery, "BAT:-");
    lv_obj_set_style_text_color(s_lbl_battery, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_lbl_battery, &lv_font_montserrat_10, 0);
    lv_obj_align(s_lbl_battery, LV_ALIGN_TOP_RIGHT, -2, 2);
}

static void build_screen_weather(lv_obj_t *parent)
{
    s_lbl_weather_title = lv_label_create(parent);
    lv_label_set_text(s_lbl_weather_title, "Weather");
    lv_obj_set_style_text_color(s_lbl_weather_title, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_lbl_weather_title, &lv_font_montserrat_10, 0);
    lv_obj_align(s_lbl_weather_title, LV_ALIGN_TOP_MID, 0, 2);

    s_lbl_weather_cond = lv_label_create(parent);
    lv_label_set_text(s_lbl_weather_cond, "---");
    lv_obj_set_style_text_color(s_lbl_weather_cond, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_lbl_weather_cond, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_weather_cond, LV_ALIGN_CENTER, 0, -8);

    s_lbl_weather_temp = lv_label_create(parent);
    lv_label_set_text(s_lbl_weather_temp, "--.-C");
    lv_obj_set_style_text_color(s_lbl_weather_temp, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_lbl_weather_temp, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_weather_temp, LV_ALIGN_CENTER, 0, 14);

    for (int i = 0; i < WEATHER_FORECAST_MAX; i++)
    {
        s_lbl_forecast[i] = lv_label_create(parent);
        lv_label_set_text(s_lbl_forecast[i], "--:-- --.-C ---");
        lv_obj_set_style_text_color(s_lbl_forecast[i], lv_color_white(), 0);
        lv_obj_set_style_text_font(s_lbl_forecast[i], &lv_font_montserrat_10, 0);
        lv_obj_align(s_lbl_forecast[i], LV_ALIGN_TOP_LEFT, 4, 18 + (i * 14));
        lv_obj_add_flag(s_lbl_forecast[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_screen_sync(lv_obj_t *parent)
{
    s_lbl_sync_title = lv_label_create(parent);
    lv_label_set_text(s_lbl_sync_title, "Sync");
    lv_obj_set_style_text_color(s_lbl_sync_title, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_lbl_sync_title, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_sync_title, LV_ALIGN_TOP_MID, 0, 10);

    s_lbl_sync_status = lv_label_create(parent);
    lv_label_set_text(s_lbl_sync_status, "Hold to update");
    lv_obj_set_style_text_color(s_lbl_sync_status, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_lbl_sync_status, &lv_font_montserrat_10, 0);
    lv_obj_align(s_lbl_sync_status, LV_ALIGN_CENTER, 0, 4);

    s_lbl_sync_last = lv_label_create(parent);
    lv_label_set_text(s_lbl_sync_last, "Last: --");
    lv_obj_set_style_text_color(s_lbl_sync_last, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_lbl_sync_last, &lv_font_montserrat_10, 0);
    lv_obj_align(s_lbl_sync_last, LV_ALIGN_BOTTOM_MID, 0, -6);
}

/* -------------------------------------------------------------------------- */
/* Timer callbacks                                                            */
/* -------------------------------------------------------------------------- */

static void clock_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (s_current_screen != SCREEN_CLOCK)
    {
        return;
    }

    char time_buf[16];
    char date_buf[24];

    ntp_get_time_str(time_buf, sizeof(time_buf));
    ntp_get_date_str(date_buf, sizeof(date_buf));

    lv_label_set_text(s_lbl_time, time_buf);
    lv_label_set_text(s_lbl_date, date_buf);

    static uint32_t s_last_battery_update_ms = 0;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    if (s_lbl_battery && (now_ms - s_last_battery_update_ms >= 30000))
    {
        lv_label_set_text(s_lbl_battery, battery_get_level_text());
        s_last_battery_update_ms = now_ms;
    }
    // ESP_LOGI("BAT", "Voltage: %.2f", battery_get_voltage());
}

static void eye_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_current_screen != SCREEN_FACE)
        return;
    eye_anim_tick();
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void display_next_screen(void)
{
    s_last_activity_ms = esp_timer_get_time() / 1000; /* reset idle timer */
    display_screen_t prev = s_current_screen;

    switch (s_current_screen)
    {
    case SCREEN_FACE:
        s_current_screen = SCREEN_CLOCK;
        break;
    case SCREEN_CLOCK:
        s_current_screen = SCREEN_WEATHER;
        break;
    case SCREEN_WEATHER:
        s_current_screen = SCREEN_SYNC;
        break;
    case SCREEN_SYNC:
        s_current_screen = SCREEN_FACE;
        break;
    case SCREEN_SPLASH:
    case SCREEN_BIRTHDAY:
    case SCREEN_PORTAL:
    default:
        s_current_screen = SCREEN_FACE;
        break;
    }

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
    s_last_activity_ms = esp_timer_get_time() / 1000; /* reset idle timer */

    if (screen >= SCREEN_COUNT)
        return;
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
    s_current_screen = SCREEN_PORTAL;
    lv_disp_load_scr(s_screens[SCREEN_PORTAL]);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "%s", "WiFi setup screen shown");
}

void display_face_next_expression(void)
{
    s_eye_expr_idx = (s_eye_expr_idx + 1) % EYE_EXPR_COUNT_LOCAL;
    eye_anim_set_expression(EYE_EXPRESSIONS[s_eye_expr_idx]);
    ESP_LOGI(TAG, "Eye expression -> %d", EYE_EXPRESSIONS[s_eye_expr_idx]);
}

/* -------------------------------------------------------------------------- */
/* Weather display mode                                                       */
/* -------------------------------------------------------------------------- */

static void display_set_weather_mode_locked(bool forecast_mode)
{
    s_weather_forecast_mode = forecast_mode;

    /*
     * Current weather labels are visible only in normal weather mode.
     */
    if (s_lbl_weather_cond)
    {
        if (forecast_mode)
        {
            lv_obj_add_flag(s_lbl_weather_cond, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_clear_flag(s_lbl_weather_cond, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_lbl_weather_temp)
    {
        if (forecast_mode)
        {
            lv_obj_add_flag(s_lbl_weather_temp, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_clear_flag(s_lbl_weather_temp, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /*
     * Forecast rows are visible only in forecast mode.
     */
    for (int i = 0; i < WEATHER_FORECAST_MAX; i++)
    {
        if (!s_lbl_forecast[i])
        {
            continue;
        }

        if (forecast_mode)
        {
            lv_obj_clear_flag(s_lbl_forecast[i], LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(s_lbl_forecast[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void display_update_weather_forecast_locked(void)
{
    int count = weather_get_forecast_count();

    for (int i = 0; i < WEATHER_FORECAST_MAX; i++)
    {
        if (!s_lbl_forecast[i])
        {
            continue;
        }

        weather_forecast_item_t item;

        if (i < count && weather_get_forecast(i, &item))
        {
            char row[32];

            /*
             * Example:
             * 21:00 25.8C Rain
             */
            snprintf(row, sizeof(row), "%s %.0fC %.8s", item.time, item.temp_c, item.condition);

            lv_label_set_text(s_lbl_forecast[i], row);
        }
        else
        {
            lv_label_set_text(s_lbl_forecast[i], "--:-- --.-C ---");
        }
    }
}

void display_weather_toggle_forecast(void)
{
    lvgl_port_lock(0);

    if (!s_weather_forecast_mode)
    {
        /*
         * Switch from current weather mode to forecast mode.
         */
        if (s_lbl_weather_title)
        {
            lv_label_set_text(s_lbl_weather_title, "Forecast");
        }

        display_update_weather_forecast_locked();
        display_set_weather_mode_locked(true);
    }
    else
    {
        /*
         * Switch back to current weather mode.
         */
        if (s_lbl_weather_title)
        {
            lv_label_set_text(s_lbl_weather_title, "Weather");
        }

        display_set_weather_mode_locked(false);
    }

    lvgl_port_unlock();
}

/* -------------------------------------------------------------------------- */
/* Weather update                                                             */
/* -------------------------------------------------------------------------- */

void display_update_weather(const char *condition, float temp_c)
{
    if (!condition)
    {
        return;
    }

    char temp_str[16];
    snprintf(temp_str, sizeof(temp_str), "%.1f C", temp_c);

    lvgl_port_lock(0);

    /*
     * Update normal weather labels.
     */
    if (s_lbl_weather_cond)
    {
        lv_label_set_text(s_lbl_weather_cond, condition);
    }

    if (s_lbl_weather_temp)
    {
        lv_label_set_text(s_lbl_weather_temp, temp_str);
    }

    /*
     * If the forecast view is open, refresh its rows too.
     * This uses cached forecast data, not a new API request.
     */
    if (s_weather_forecast_mode)
    {
        display_update_weather_forecast_locked();
    }

    lvgl_port_unlock();
}

void display_tick(void)
{
    lv_tick_inc(1);
}

void display_reset_activity(void)
{
    s_last_activity_ms = esp_timer_get_time() / 1000;
}

void display_suspend(void)
{
    if (!s_display_on)
        return;
    ESP_LOGI(TAG, "Suspending display (idle timeout)");
    esp_lcd_panel_disp_on_off(s_panel, false);
    s_display_on = false;
}

void display_resume(void)
{
    if (s_display_on)
        return;
    ESP_LOGI(TAG, "Resuming display");
    esp_lcd_panel_disp_on_off(s_panel, true);
    s_display_on = true;
    lv_disp_trig_activity(s_disp);
}

uint32_t display_get_last_activity_ms(void)
{
    return s_last_activity_ms;
}

bool display_is_suspended(void)
{
    return !s_display_on;
}

bool display_is_eye_screen(void)
{
    return s_current_screen == SCREEN_FACE;
}

static void eye_test_all_task(void *arg)
{
    (void)arg;

    typedef struct
    {
        eye_expression_t expr;
        const char *name;
    } eye_test_item_t;

    static const eye_test_item_t tests[] = {
        {EYE_EXPR_NORMAL, "NORMAL"},
        {EYE_EXPR_HAPPY, "HAPPY"},
        {EYE_EXPR_ANGRY, "ANGRY"},
        {EYE_EXPR_SLEEPY, "SLEEPY"},
        {EYE_EXPR_SURPRISED, "SURPRISED"},
        {EYE_EXPR_WONDER, "WONDER"},
        {EYE_EXPR_CUTE, "CUTE"},
        {EYE_EXPR_SUSPICIOUS, "SUSPICIOUS"},
        {EYE_EXPR_SAD, "SAD"},
        {EYE_EXPR_CLOSE, "CLOSE"},
        {EYE_EXPR_UPSET, "UPSET"},
        {EYE_EXPR_LOVE, "LOVE"},
    };

    const int count = sizeof(tests) / sizeof(tests[0]);

    /* Stop random idle expressions */
    eye_anim_set_idle(false);

    while (1)
    {
        for (int i = 0; i < count; i++)
        {
            ESP_LOGI(EYE_TEST_TAG, "Testing eye: %s", tests[i].name);

            eye_anim_set_expression(tests[i].expr);

            /* Optional blink before each expression */
            eye_anim_blink();

            vTaskDelay(pdMS_TO_TICKS(2500));
        }
    }
}

void display_show_splash(void)
{
    eye_anim_set_idle(false);

    lvgl_port_lock(0);
    s_current_screen = SCREEN_SPLASH;
    lv_disp_load_scr(s_screens[SCREEN_SPLASH]);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "%s", "Splash screen shown");
}

void display_show_birthday(void)
{
    if (s_splash_timer)
    {
        lv_timer_del(s_splash_timer);
        s_splash_timer = NULL;
    }

    eye_anim_set_idle(false);

    lvgl_port_lock(0);
    s_current_screen = SCREEN_BIRTHDAY;
    lv_disp_load_scr(s_screens[SCREEN_BIRTHDAY]);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "%s", "Birthday screen shown");
}

void display_finish_splash(void)
{
    if (s_current_screen != SCREEN_SPLASH)
    {
        return;
    }

    display_set_screen(SCREEN_FACE);
}

static void splash_timer_cb(lv_timer_t *timer)
{
    if (s_current_screen == SCREEN_SPLASH)
    {
        display_set_screen(SCREEN_FACE);
    }

    if (s_splash_timer)
    {
        lv_timer_del(s_splash_timer);
        s_splash_timer = NULL;
    }
}

void display_show_sync_status(const char *line1, const char *line2)
{
    lvgl_port_lock(0);

    if (s_lbl_sync_title) {
        lv_label_set_text(s_lbl_sync_title, line1 ? line1 : "Sync");
    }

    if (s_lbl_sync_status) {
        lv_label_set_text(s_lbl_sync_status, line2 ? line2 : "Hold to update");
    }

    lvgl_port_unlock();
}

void display_show_sync_idle(bool ok)
{
    char last_buf[24];

    time_t now = time(NULL);
    struct tm tm_now;

    localtime_r(&now, &tm_now);

    if (now > 1700000000) {
        snprintf(last_buf,
                 sizeof(last_buf),
                 "Last: %s %02d:%02d",
                 ok ? "OK" : "FAIL",
                 tm_now.tm_hour,
                 tm_now.tm_min);
    } else {
        snprintf(last_buf,
                 sizeof(last_buf),
                 "Last: %s",
                 ok ? "OK" : "FAIL");
    }

    lvgl_port_lock(0);

    if (s_lbl_sync_title) {
        lv_label_set_text(s_lbl_sync_title, "Sync");
    }

    if (s_lbl_sync_status) {
        lv_label_set_text(s_lbl_sync_status, "Hold to update");
    }

    if (s_lbl_sync_last) {
        lv_label_set_text(s_lbl_sync_last, last_buf);
    }

    lvgl_port_unlock();
}