/*
 * DeskBuddy — wifi_manager.c
 * Station connect with NVS credentials + softAP captive portal fallback.
 */

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "wifi_manager.h"

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif  = NULL;

static const char *TAG        = "wifi_manager";
static const char *NVS_NS     = "wifi_cfg";
static const char *NVS_KEY_SS = "ssid";
static const char *NVS_KEY_PW = "password";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_eg   = NULL;
static bool               s_connected = false;
static int                s_retries   = 0;
static httpd_handle_t     s_httpd     = NULL;

/* ── NVS helpers ─────────────────────────────────────────────────────── */
static bool nvs_load_credentials(char *ssid, char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t ss_len = 64, pw_len = 64;
    bool ok = (nvs_get_str(h, NVS_KEY_SS, ssid, &ss_len) == ESP_OK &&
               nvs_get_str(h, NVS_KEY_PW, pass, &pw_len) == ESP_OK &&
               strlen(ssid) > 0);
    nvs_close(h);
    return ok;
}

static void nvs_save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_SS, ssid);
    nvs_set_str(h, NVS_KEY_PW, pass);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Credentials saved: SSID=%s", ssid);
}

/* ── WiFi event handler ──────────────────────────────────────────────── */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retries < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retries++;
            ESP_LOGI(TAG, "Retry %d/%d", s_retries, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retries   = 0;
        s_connected = true;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

/* ── Portal HTML ─────────────────────────────────────────────────────── */
static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>DeskBuddy WiFi Setup</title>"
    "<style>body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:20px}"
    "input{width:100%;padding:8px;margin:8px 0;box-sizing:border-box}"
    "button{width:100%;padding:10px;background:#4CAF50;color:white;border:none;"
    "border-radius:4px;font-size:16px;cursor:pointer}</style></head>"
    "<body><h2>&#128241; Buddy WiFi Setup</h2>"
    "<form action='/save' method='POST'>"
    "<label>WiFi SSID</label>"
    "<input name='ssid' placeholder='Your WiFi name' required>"
    "<label>Password</label>"
    "<input name='pass' type='password' placeholder='Your WiFi password'>"
    "<button type='submit'>Save &amp; Connect</button>"
    "</form></body></html>";

static const char SAVED_HTML[] =
    "<!DOCTYPE html><html><body style='font-family:sans-serif;text-align:center;padding:40px'>"
    "<h2>&#10003; Saved!</h2>"
    "<p>DeskBuddy is connecting to your WiFi.<br>Device will reboot now.</p>"
    "</body></html>";

/* ── HTTP handlers ───────────────────────────────────────────────────── */
static esp_err_t handler_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PORTAL_HTML, sizeof(PORTAL_HTML) - 1);
    return ESP_OK;
}

static void url_decode(char *dst, const char *src, size_t max)
{
    size_t i = 0;
    while (*src && i < max - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' '; src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static bool extract_field(const char *body, const char *field,
                           char *out, size_t out_len)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "%s=", field);
    const char *p = strstr(body, pattern);
    if (!p) { out[0] = '\0'; return false; }
    p += strlen(pattern);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    char encoded[128] = {0};
    if (len >= sizeof(encoded)) len = sizeof(encoded) - 1;
    memcpy(encoded, p, len);
    url_decode(out, encoded, out_len);
    return true;
}

static esp_err_t handler_save(httpd_req_t *req)
{
    char body[256] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char ssid[64] = {0}, pass[64] = {0};
    extract_field(body, "ssid", ssid, sizeof(ssid));
    extract_field(body, "pass", pass, sizeof(pass));

    if (strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Portal received SSID: %s", ssid);
    nvs_save_credentials(ssid, pass);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, SAVED_HTML, sizeof(SAVED_HTML) - 1);

    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

/* Redirect captive portal probes to root */
static esp_err_t handler_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ── Start HTTP server ───────────────────────────────────────────────── */
static void start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "%s", "Failed to start HTTP server");
        return;
    }

    httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET,
        .handler = handler_root, .user_ctx = NULL
    };
    httpd_uri_t uri_save = {
        .uri = "/save", .method = HTTP_POST,
        .handler = handler_save, .user_ctx = NULL
    };
    /* Common captive portal probe URLs */
    httpd_uri_t uri_204 = {
        .uri = "/generate_204", .method = HTTP_GET,
        .handler = handler_redirect, .user_ctx = NULL
    };
    httpd_uri_t uri_hotspot = {
        .uri = "/hotspot-detect.html", .method = HTTP_GET,
        .handler = handler_redirect, .user_ctx = NULL
    };

    httpd_register_uri_handler(s_httpd, &uri_root);
    httpd_register_uri_handler(s_httpd, &uri_save);
    httpd_register_uri_handler(s_httpd, &uri_204);
    httpd_register_uri_handler(s_httpd, &uri_hotspot);

    ESP_LOGI(TAG, "%s", "Captive portal HTTP server started at 192.168.4.1");
}

/* ── Stop HTTP server ───────────────────────────────────────────────── */
void wifi_manager_stop_portal(void)
{
    ESP_LOGI(TAG, "Stopping captive portal...");

    /* Stop HTTP server */
    if (s_httpd != NULL) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }

    /* Stop Wi-Fi */
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "esp_wifi_stop() failed: %s", esp_err_to_name(err));
    }

    /* Disable AP mode */
    err = esp_wifi_set_mode(WIFI_MODE_NULL);
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "esp_wifi_set_mode(NULL) failed: %s", esp_err_to_name(err));
    }

    s_connected = false;

    ESP_LOGI(TAG, "SoftAP disabled");
}

/* ── Station connect ─────────────────────────────────────────────────── */
static bool connect_sta(const char *ssid, const char *pass)
{
    s_retries = 0;
    if (!s_wifi_eg) s_wifi_eg = xEventGroupCreate();

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid,     ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    return (bits & WIFI_CONNECTED_BIT) != 0;
}

/* ── Public API ──────────────────────────────────────────────────────── */
void wifi_manager_init(void)
{
    if (!s_wifi_eg) {
        s_wifi_eg = xEventGroupCreate();
    }

    /* Initialize TCP/IP stack and event loop only once */
    static bool s_netif_initialized = false;
    if (!s_netif_initialized) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        s_netif_initialized = true;
    }

    /* Create default netifs only once */
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }

    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    /* Initialize Wi-Fi driver only once */
    static bool s_wifi_initialized = false;
    if (!s_wifi_initialized) {
        wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
        s_wifi_initialized = true;
    }

    /* Register event handlers only once */
    static bool s_handlers_registered = false;
    if (!s_handlers_registered) {
        esp_event_handler_instance_t h1, h2;

        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_event_handler,
            NULL,
            &h1));

        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            wifi_event_handler,
            NULL,
            &h2));

        s_handlers_registered = true;
    }

    char ssid[64] = {0};
    char pass[64] = {0};

    if (nvs_load_credentials(ssid, pass)) {
        ESP_LOGI(TAG, "Found saved SSID: %s", ssid);

        if (connect_sta(ssid, pass)) {
            ESP_LOGI(TAG, "WiFi connected!");
            return;
        }

        ESP_LOGW(TAG, "Saved credentials failed — starting portal");
    } else {
        ESP_LOGI(TAG, "No saved credentials — starting portal");
    }

    wifi_manager_start_portal();
}

void wifi_manager_start_portal(void)
{
    ESP_LOGI(TAG, "Starting softAP captive portal...");

    /* Stop Wi-Fi if currently running */
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));

    /* DO NOT call esp_netif_create_default_wifi_ap() here.
       The AP netif was already created in wifi_manager_init(). */

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .ssid_len = strlen(WIFI_AP_SSID),
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };

    /* Only set password if not open auth */
    if (strlen(WIFI_AP_PASSWORD) > 0) {
        strncpy((char *)ap_cfg.ap.password,
                WIFI_AP_PASSWORD,
                sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started: SSID=%s, IP=192.168.4.1", WIFI_AP_SSID);

    if (s_httpd == NULL) {
        start_http_server();
    }
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}