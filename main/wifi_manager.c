/* -------------------------------------------------------------------------- */
/* Buddy - wifi_manager.c                                                     */
/*                                                                            */
/* Wi-Fi station connection, saved NVS credentials, manual setup portal,      */
/* and battery-friendly retry handling.                                       */
/* -------------------------------------------------------------------------- */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "wifi_credentials.h"
#include "wifi_manager.h"

/* -------------------------------------------------------------------------- */
/* Defines / constants                                                        */
/* -------------------------------------------------------------------------- */

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

#define PORTAL_HTML_MAX 4096
#define WIFI_RETRY_LATER_MS (60UL * 60UL * 1000UL)

/* -------------------------------------------------------------------------- */
/* Static variables                                                           */
/* -------------------------------------------------------------------------- */

static const char *TAG = "wifi_manager";

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;

static EventGroupHandle_t s_wifi_eg = NULL;

static bool s_connected = false;
static bool s_connecting = false;
static bool s_portal_running = false;

static int s_retries = 0;

static httpd_handle_t s_httpd = NULL;

static TaskHandle_t s_retry_later_task = NULL;
static TaskHandle_t s_retry_now_task = NULL;

/* -------------------------------------------------------------------------- */
/* Forward declarations                                                       */
/* -------------------------------------------------------------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
static bool connect_sta(const char *ssid, const char *pass);
static void wifi_schedule_retry_now(void);
static void wifi_schedule_retry_later(void);
static void start_http_server(void);

/* -------------------------------------------------------------------------- */
/* Wi-Fi event handler                                                        */
/* -------------------------------------------------------------------------- */
/*
 * Handles ESP-IDF Wi-Fi/IP events.
 *
 * STA_START:
 * - start connection attempt
 *
 * STA_DISCONNECTED:
 * - retry current credential up to WIFI_MAX_RETRY
 * - notify connect_sta() if it is waiting
 * - schedule retry if connection was lost later
 *
 * STA_GOT_IP:
 * - mark Wi-Fi as connected
 * - notify connect_sta()
 */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        if (!s_portal_running)
        {
            esp_wifi_connect();
        }
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        s_connected = false;

        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)data;

        ESP_LOGW(TAG, "WiFi disconnected, reason=%d", disc ? disc->reason : -1);

        /*
         * Ignore STA disconnects while the setup portal is active.
         * Starting SoftAP intentionally stops/changes STA mode, and retrying here
         * can kill Buddy-Setup.
         */
        if (s_portal_running)
        {
            ESP_LOGI(TAG, "Portal is running, ignore STA disconnect retry");
            return;
        }

        if (s_retries < WIFI_MAX_RETRY)
        {
            s_retries++;

            ESP_LOGI(TAG, "Retry %d/%d", s_retries, WIFI_MAX_RETRY);

            esp_wifi_connect();
        }
        else
        {
            ESP_LOGW(TAG, "Max retry reached");

            if (s_connecting)
            {
                if (s_wifi_eg)
                {
                    xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
                }
            }
            else
            {
                wifi_schedule_retry_now();
            }
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;

        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));

        s_retries = 0;
        s_connected = true;

        if (s_wifi_eg)
        {
            xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Station connect helpers                                                    */
/* -------------------------------------------------------------------------- */
/*
 * Handles STA mode connection to saved router Wi-Fi credentials.
 *
 * This section tries saved SSID/password entries and updates connection state.
 * It should not start the setup portal automatically.
 */
static bool connect_sta(const char *ssid, const char *pass)
{
    if (!ssid || strlen(ssid) == 0)
    {
        return false;
    }

    if (!s_wifi_eg)
    {
        s_wifi_eg = xEventGroupCreate();
    }

    s_connecting = true;

    xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    s_retries = 0;
    s_connected = false;

    /*
     * Stop any previous WiFi activity before changing config.
     * Do not use ESP_ERROR_CHECK here because WiFi may already
     * be stopped, disconnected, or still transitioning state.
     */
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(300));

    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(500));

    wifi_config_t cfg = {0};

    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass ? pass : "", sizeof(cfg.sta.password) - 1);

    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    esp_err_t err;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        s_connecting = false;
        return false;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        s_connecting = false;
        return false;
    }

    err = esp_wifi_start();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        s_connecting = false;
        return false;
    }

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdTRUE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "Connected to SSID: %s", ssid);
        s_connecting = false;
        return true;
    }

    ESP_LOGW(TAG, "Connect failed for SSID: %s", ssid);

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(300));

    s_connecting = false;
    return false;
}

/* -------------------------------------------------------------------------- */
/* Retry tasks                                                                */
/* -------------------------------------------------------------------------- */
/*
 * Handles delayed and immediate Wi-Fi retry tasks.
 *
 * Delayed retry is used after saved credentials fail.
 * Retry must not run while the SoftAP setup portal is active.
 */
static void wifi_retry_later_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "WiFi retry task sleeping for 1 hour");

    vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_LATER_MS));

    if (s_portal_running)
    {
        ESP_LOGI(TAG, "Portal is running, skip delayed WiFi retry");

        s_retry_later_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Retrying saved WiFi networks after 1 hour");

    s_retries = 0;
    s_retry_later_task = NULL;

    wifi_manager_init();

    vTaskDelete(NULL);
}

static void wifi_schedule_retry_later(void)
{
    if (s_portal_running)
    {
        ESP_LOGI(TAG, "Portal is running, delayed WiFi retry not scheduled");
        return;
    }

    if (s_retry_later_task != NULL)
    {
        ESP_LOGI(TAG, "WiFi retry task already scheduled");
        return;
    }

    BaseType_t ok =
        xTaskCreate(wifi_retry_later_task, "wifi_retry_later", 4096, NULL, 5, &s_retry_later_task);

    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create WiFi retry task");
        s_retry_later_task = NULL;
    }
}

static void wifi_retry_now_task(void *arg)
{
    (void)arg;

    if (s_portal_running)
    {
        ESP_LOGI(TAG, "Portal is running, skip immediate WiFi retry");

        s_retry_now_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Retrying all saved WiFi networks now");

    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(300));

    s_retries = 0;
    s_connected = false;
    s_connecting = false;
    s_retry_now_task = NULL;

    wifi_manager_init();

    vTaskDelete(NULL);
}

static void wifi_schedule_retry_now(void)
{
    if (s_portal_running)
    {
        ESP_LOGI(TAG, "Portal is running, immediate WiFi retry not scheduled");
        return;
    }

    if (s_retry_now_task != NULL)
    {
        ESP_LOGI(TAG, "Immediate WiFi retry task already scheduled");
        return;
    }

    BaseType_t ok =
        xTaskCreate(wifi_retry_now_task, "wifi_retry_now", 4096, NULL, 5, &s_retry_now_task);

    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create immediate WiFi retry task");
        s_retry_now_task = NULL;
    }
}

/* -------------------------------------------------------------------------- */
/* Portal HTML                                                                */
/* -------------------------------------------------------------------------- */
/*
 * Contains the HTML page shown by the Buddy-Setup captive portal.
 *
 * Keep only page content here. Wi-Fi logic and form handling belong in
 * HTTP handlers.
 */
static void build_portal_page(char *html, size_t max_len)
{
    html[0] = '\0';

    /* Build page header */
    strlcat(html,
            "<!DOCTYPE html><html><head>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>Buddy Wi-Fi Setup</title>"
            "<style>"
            "body{font-family:sans-serif;max-width:480px;margin:40px "
            "auto;padding:20px;}"
            "input{width:100%;padding:8px;margin:6px 0;box-sizing:border-box;}"
            "button{padding:8px 12px;margin-top:8px;}"
            "li{margin:10px 0;}"
            "a{margin-left:10px;}"
            "</style></head><body>",
            max_len);

    /* Build add-network form */
    strlcat(html,
            "<h2>📶 Buddy Wi-Fi Setup</h2>"
            "<h3>Add New Network</h3>"
            "<form action='/save' method='POST'>"
            "<input name='ssid' placeholder='Wi-Fi SSID' required>"
            "<input name='pass' type='password' placeholder='Password'>"
            "<button type='submit'>Save & Connect</button>"
            "</form>",
            max_len);

    /* Build saved-network list */
    strlcat(html, "<h3>Saved Networks</h3><ul>", max_len);

    size_t count = wifi_credentials_count();

    if (count == 0)
    {
        strlcat(html, "<p>No saved networks.</p>", max_len);
    }

    for (size_t i = 0; i < count; i++)
    {
        wifi_credential_t cred;

        if (!wifi_credentials_get(i, &cred))
        {
            continue;
        }

        char row[512];

        snprintf(row,
                 sizeof(row),
                 "<li><b>%s</b> "
                 "<a href='/edit?id=%u'>Edit</a> "
                 "<a href='/delete?id=%u' "
                 "onclick=\"return confirm('Delete this network?')\">Delete</a>"
                 "</li>",
                 cred.ssid,
                 (unsigned)i,
                 (unsigned)i);

        strlcat(html, row, max_len);
    }

    strlcat(html, "</ul></body></html>", max_len);
}

static const char SAVED_HTML[] =
    "<!DOCTYPE html><html><body "
    "style='font-family:sans-serif;text-align:center;padding:40px'>"
    "<h2>&#10003; Saved!</h2>"
    "<p>Buddy is connecting to your WiFi.<br>Device will reboot now.</p>"
    "</body></html>";

/* -------------------------------------------------------------------------- */
/* HTTP helpers                                                               */
/* -------------------------------------------------------------------------- */
/*
 * Small helper functions for the captive portal web server.
 *
 * Examples:
 * - URL decoding
 * - form parsing
 * - sending redirects
 * - sending HTML responses
 */
static void url_decode(char *dst, const char *src, size_t max)
{
    size_t i = 0;

    while (*src && i < max - 1)
    {
        if (*src == '%' && src[1] && src[2])
        {
            char hex[3] = {src[1], src[2], 0};

            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        }
        else if (*src == '+')
        {
            dst[i++] = ' ';
            src++;
        }
        else
        {
            dst[i++] = *src++;
        }
    }

    dst[i] = '\0';
}

static bool extract_field(const char *body, const char *field, char *out, size_t out_len)
{
    char pattern[32];

    snprintf(pattern, sizeof(pattern), "%s=", field);

    const char *p = strstr(body, pattern);

    if (!p)
    {
        out[0] = '\0';
        return false;
    }

    p += strlen(pattern);

    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);

    char encoded[128] = {0};

    if (len >= sizeof(encoded))
    {
        len = sizeof(encoded) - 1;
    }

    memcpy(encoded, p, len);
    url_decode(out, encoded, out_len);

    return true;
}

/* -------------------------------------------------------------------------- */
/* HTTP handlers                                                              */
/* -------------------------------------------------------------------------- */
/*
 * Handles HTTP requests from the captive portal.
 *
 * Examples:
 * - GET / shows the setup form
 * - POST /save stores SSID/password credentials
 */
static esp_err_t handler_root(httpd_req_t *req)
{
    char *html = calloc(1, PORTAL_HTML_MAX);

    if (html == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }

    build_portal_page(html, PORTAL_HTML_MAX);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));

    free(html);

    return ESP_OK;
}

static esp_err_t handler_save(httpd_req_t *req)
{
    char body[256] = {0};

    int received = httpd_req_recv(req, body, sizeof(body) - 1);

    if (received <= 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    body[received] = '\0';

    char ssid[64] = {0};
    char pass[64] = {0};

    extract_field(body, "ssid", ssid, sizeof(ssid));
    extract_field(body, "pass", pass, sizeof(pass));

    if (strlen(ssid) == 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Portal received SSID: %s", ssid);

    /*
     * Add credential to NVS.
     *
     * If the SSID already exists, wifi_credentials_add()
     * updates the existing entry instead of creating a duplicate.
     */
    if (!wifi_credentials_add(ssid, pass))
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save credentials");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, SAVED_HTML, sizeof(SAVED_HTML) - 1);

    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();

    return ESP_OK;
}

static esp_err_t handler_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);

    return ESP_OK;
}

static esp_err_t handler_delete(httpd_req_t *req)
{
    char query[64];
    char param[16];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "id", param, sizeof(param)) == ESP_OK)
    {
        size_t index = (size_t)atoi(param);

        wifi_credentials_delete(index);
    }

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);

    return ESP_OK;
}

static esp_err_t handler_edit(httpd_req_t *req)
{
    char query[64];
    char param[16];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", param, sizeof(param)) != ESP_OK)
    {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    size_t index = (size_t)atoi(param);

    wifi_credential_t cred;

    if (!wifi_credentials_get(index, &cred))
    {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    char *html = calloc(1, 2048);

    if (html == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }

    snprintf(html,
             2048,
             "<!DOCTYPE html><html><body>"
             "<h2>Edit Network</h2>"
             "<form action='/update?id=%u' method='POST'>"
             "SSID:<br>"
             "<input name='ssid' value='%s' required><br>"
             "Password:<br>"
             "<input name='pass' type='password' value='%s'><br><br>"
             "<button type='submit'>Save</button>"
             "</form>"
             "<p><a href='/'>Back</a></p>"
             "</body></html>",
             (unsigned)index,
             cred.ssid,
             cred.password);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));

    free(html);

    return ESP_OK;
}

static esp_err_t handler_update(httpd_req_t *req)
{
    char query[64];
    char param[16];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", param, sizeof(param)) != ESP_OK)
    {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    size_t index = (size_t)atoi(param);

    char body[256] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);

    if (received <= 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    body[received] = '\0';

    char ssid[64] = {0};
    char pass[64] = {0};

    extract_field(body, "ssid", ssid, sizeof(ssid));
    extract_field(body, "pass", pass, sizeof(pass));

    wifi_credentials_update(index, ssid, pass);

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* HTTP server control                                                        */
/* -------------------------------------------------------------------------- */
/*
 * Starts and stops the HTTP server used by the setup portal.
 *
 * This section should only manage server lifecycle, not Wi-Fi connection logic.
 */
static void start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();

    if (httpd_start(&s_httpd, &cfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "%s", "Failed to start HTTP server");
        return;
    }

    httpd_uri_t uri_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handler_root,
        .user_ctx = NULL,
    };

    httpd_uri_t uri_save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = handler_save,
        .user_ctx = NULL,
    };

    httpd_uri_t uri_204 = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = handler_redirect,
        .user_ctx = NULL,
    };

    httpd_uri_t uri_hotspot = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = handler_redirect,
        .user_ctx = NULL,
    };

    httpd_uri_t uri_delete = {
        .uri = "/delete",
        .method = HTTP_GET,
        .handler = handler_delete,
    };

    httpd_uri_t uri_edit = {
        .uri = "/edit",
        .method = HTTP_GET,
        .handler = handler_edit,
    };

    httpd_uri_t uri_update = {
        .uri = "/update",
        .method = HTTP_POST,
        .handler = handler_update,
    };

    httpd_register_uri_handler(s_httpd, &uri_root);
    httpd_register_uri_handler(s_httpd, &uri_save);
    httpd_register_uri_handler(s_httpd, &uri_204);
    httpd_register_uri_handler(s_httpd, &uri_hotspot);
    httpd_register_uri_handler(s_httpd, &uri_delete);
    httpd_register_uri_handler(s_httpd, &uri_edit);
    httpd_register_uri_handler(s_httpd, &uri_update);

    ESP_LOGI(TAG, "%s", "Captive portal HTTP server started at 192.168.4.1");
}

/* -------------------------------------------------------------------------- */
/* SoftAP portal control                                                      */
/* -------------------------------------------------------------------------- */
/*
 * Starts and stops the Buddy-Setup SoftAP captive portal.
 *
 * SoftAP is started only by explicit user action, such as long press on
 * the clock screen.
 */
void wifi_manager_start_portal(void)
{
    ESP_LOGI(TAG, "Starting softAP captive portal...");

    if (s_retry_later_task != NULL)
    {
        vTaskDelete(s_retry_later_task);
        s_retry_later_task = NULL;
        ESP_LOGI(TAG, "Pending delayed WiFi retry cancelled");
    }

    if (s_retry_now_task != NULL)
    {
        vTaskDelete(s_retry_now_task);
        s_retry_now_task = NULL;
        ESP_LOGI(TAG, "Pending immediate WiFi retry cancelled");
    }

    s_connected = false;
    s_connecting = false;
    s_retries = 0;
    s_portal_running = true;

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));

    wifi_config_t ap_cfg = {
        .ap =
            {
                .ssid = WIFI_AP_SSID,
                .ssid_len = strlen(WIFI_AP_SSID),
                .max_connection = 4,
                .authmode = WIFI_AUTH_OPEN,
            },
    };

    if (strlen(WIFI_AP_PASSWORD) > 0)
    {
        strncpy((char *)ap_cfg.ap.password, WIFI_AP_PASSWORD, sizeof(ap_cfg.ap.password) - 1);

        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started: SSID=%s, IP=192.168.4.1", WIFI_AP_SSID);

    if (s_httpd == NULL)
    {
        start_http_server();
    }
}

void wifi_manager_stop_portal(void)
{
    ESP_LOGI(TAG, "Stopping captive portal...");

    if (s_httpd != NULL)
    {
        httpd_stop(s_httpd);
        s_httpd = NULL;

        ESP_LOGI(TAG, "HTTP server stopped");
    }

    esp_err_t err = esp_wifi_stop();

    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_NOT_STARTED)
    {
        ESP_LOGW(TAG, "esp_wifi_stop() failed: %s", esp_err_to_name(err));
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    s_connected = false;
    s_connecting = false;
    s_retries = 0;
    s_portal_running = false;

    ESP_LOGI(TAG, "Buddy-Setup stopped, trying saved WiFi once");

    wifi_schedule_retry_now();
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */
/*
 * Functions called by other modules.
 *
 * Keep internal helper functions static. Only functions declared in
 * wifi_manager.h should be here.
 */
void wifi_manager_init(void)
{
    if (s_portal_running)
    {
        ESP_LOGI(TAG, "Portal is running, skip STA init");
        return;
    }

    if (!s_wifi_eg)
    {
        s_wifi_eg = xEventGroupCreate();
    }

    static bool s_netif_initialized = false;

    if (!s_netif_initialized)
    {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        s_netif_initialized = true;
    }

    if (s_sta_netif == NULL)
    {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }

    if (s_ap_netif == NULL)
    {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    static bool s_wifi_initialized = false;

    if (!s_wifi_initialized)
    {
        wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();

        ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
        s_wifi_initialized = true;
    }

    static bool s_handlers_registered = false;

    if (!s_handlers_registered)
    {
        esp_event_handler_instance_t h1;
        esp_event_handler_instance_t h2;

        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &h1));

        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &h2));

        s_handlers_registered = true;
    }

    size_t count = wifi_credentials_count();

    if (count == 0)
    {
        ESP_LOGW(TAG, "No saved credentials. WiFi setup portal will not auto-start.");
        ESP_LOGW(TAG, "Long press on CLOCK screen to start WiFi setup.");

        esp_wifi_stop();
        s_connected = false;
        s_connecting = false;

        return;
    }

    for (size_t i = 0; i < count; i++)
    {
        wifi_credential_t cred;

        if (!wifi_credentials_get(i, &cred))
        {
            continue;
        }

        ESP_LOGI(TAG, "Trying credential [%u]: %s", (unsigned)i, cred.ssid);

        if (connect_sta(cred.ssid, cred.password))
        {
            ESP_LOGI(TAG, "WiFi connected to %s", cred.ssid);
            return;
        }

        ESP_LOGW(TAG, "Failed to connect to %s", cred.ssid);
    }

    ESP_LOGW(TAG, "All saved credentials failed. WiFi will retry in 1 hour.");

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_wifi_stop();

    s_connected = false;
    s_connecting = false;

    wifi_schedule_retry_later();
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}
