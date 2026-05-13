/* -------------------------------------------------------------------------- */
/* Buddy - wifi_credentials.c                                                 */
/*                                                                            */
/* Stores, reads, updates, and deletes multiple Wi-Fi credentials in NVS.     */
/* -------------------------------------------------------------------------- */

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "wifi_credentials.h"

/* -------------------------------------------------------------------------- */
/* Static Constants                                                           */
/* -------------------------------------------------------------------------- */

static const char *TAG = "wifi_credentials";
static const char *NVS_NS = "wifi_cfg";
static const char *NVS_KEY_COUNT = "count";

/* -------------------------------------------------------------------------- */
/* Internal Helpers                                                           */
/* -------------------------------------------------------------------------- */

/*
 * Create the NVS key name for a credential slot.
 *
 * Example:
 * index 0 -> "cred_0"
 * index 1 -> "cred_1"
 */
static void make_key(size_t index, char *key, size_t key_len)
{
    snprintf(key, key_len, "cred_%u", (unsigned)index);
}

/*
 * Open the Wi-Fi credential namespace for reading and writing.
 */
static bool nvs_open_rw(nvs_handle_t *h)
{
    return nvs_open(NVS_NS, NVS_READWRITE, h) == ESP_OK;
}

/*
 * Open the Wi-Fi credential namespace for reading only.
 */
static bool nvs_open_ro(nvs_handle_t *h)
{
    return nvs_open(NVS_NS, NVS_READONLY, h) == ESP_OK;
}

/*
 * Load the number of saved credentials from NVS.
 *
 * If the value is missing, return 0.
 * If the stored value is larger than the maximum, clamp it.
 */
static size_t load_count(void)
{
    nvs_handle_t h;
    uint8_t count = 0;

    if (!nvs_open_ro(&h))
    {
        return 0;
    }

    nvs_get_u8(h, NVS_KEY_COUNT, &count);
    nvs_close(h);

    if (count > WIFI_MAX_CREDENTIALS)
    {
        count = WIFI_MAX_CREDENTIALS;
    }

    return count;
}

/*
 * Save the number of stored credentials.
 */
static bool save_count(size_t count)
{
    if (count > WIFI_MAX_CREDENTIALS)
    {
        return false;
    }

    nvs_handle_t h;

    if (!nvs_open_rw(&h))
    {
        return false;
    }

    esp_err_t err = nvs_set_u8(h, NVS_KEY_COUNT, (uint8_t)count);

    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }

    nvs_close(h);

    return err == ESP_OK;
}

/*
 * Save one credential into a specific NVS slot.
 */
static bool save_credential(size_t index, const wifi_credential_t *cred)
{
    if (index >= WIFI_MAX_CREDENTIALS || cred == NULL)
    {
        return false;
    }

    nvs_handle_t h;

    if (!nvs_open_rw(&h))
    {
        return false;
    }

    char key[16];

    make_key(index, key, sizeof(key));

    esp_err_t err = nvs_set_blob(h, key, cred, sizeof(*cred));

    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }

    nvs_close(h);

    return err == ESP_OK;
}

/*
 * Load one credential from a specific NVS slot.
 */
static bool load_credential(size_t index, wifi_credential_t *cred)
{
    if (index >= WIFI_MAX_CREDENTIALS || cred == NULL)
    {
        return false;
    }

    nvs_handle_t h;

    if (!nvs_open_ro(&h))
    {
        return false;
    }

    char key[16];

    make_key(index, key, sizeof(key));

    size_t size = sizeof(*cred);
    esp_err_t err = nvs_get_blob(h, key, cred, &size);

    nvs_close(h);

    return err == ESP_OK && size == sizeof(*cred);
}

/*
 * Erase one credential slot from NVS.
 *
 * ESP_ERR_NVS_NOT_FOUND is treated as success because the goal is
 * for the key to no longer exist.
 */
static bool erase_credential(size_t index)
{
    nvs_handle_t h;

    if (!nvs_open_rw(&h))
    {
        return false;
    }

    char key[16];

    make_key(index, key, sizeof(key));

    esp_err_t err = nvs_erase_key(h, key);

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        err = ESP_OK;
    }

    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }

    nvs_close(h);

    return err == ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

size_t wifi_credentials_count(void)
{
    return load_count();
}

bool wifi_credentials_get(size_t index, wifi_credential_t *cred)
{
    size_t count = load_count();

    if (index >= count)
    {
        return false;
    }

    return load_credential(index, cred);
}

bool wifi_credentials_find_by_ssid(const char *ssid, size_t *index_out)
{
    if (ssid == NULL)
    {
        return false;
    }

    size_t count = load_count();

    for (size_t i = 0; i < count; i++)
    {
        wifi_credential_t cred;

        if (load_credential(i, &cred))
        {
            if (strcmp(cred.ssid, ssid) == 0)
            {
                if (index_out)
                {
                    *index_out = i;
                }

                return true;
            }
        }
    }

    return false;
}

bool wifi_credentials_add(const char *ssid, const char *password)
{
    if (ssid == NULL || strlen(ssid) == 0)
    {
        return false;
    }

    /*
     * Avoid duplicate SSIDs.
     *
     * If the SSID already exists, update the existing password instead
     * of adding a second copy of the same network.
     */
    size_t existing_index;

    if (wifi_credentials_find_by_ssid(ssid, &existing_index))
    {
        return wifi_credentials_update(existing_index, ssid, password ? password : "");
    }

    size_t count = load_count();

    if (count >= WIFI_MAX_CREDENTIALS)
    {
        ESP_LOGW(TAG, "Credential list full");
        return false;
    }

    wifi_credential_t cred = {0};

    strncpy(cred.ssid, ssid, WIFI_SSID_MAX_LEN);

    if (password)
    {
        strncpy(cred.password, password, WIFI_PASS_MAX_LEN);
    }

    if (!save_credential(count, &cred))
    {
        return false;
    }

    if (!save_count(count + 1))
    {
        return false;
    }

    ESP_LOGI(TAG, "Added credential [%u]: %s", (unsigned)count, cred.ssid);

    return true;
}

bool wifi_credentials_update(size_t index, const char *ssid, const char *password)
{
    size_t count = load_count();

    if (index >= count || ssid == NULL || strlen(ssid) == 0)
    {
        return false;
    }

    wifi_credential_t cred = {0};

    strncpy(cred.ssid, ssid, WIFI_SSID_MAX_LEN);

    if (password)
    {
        strncpy(cred.password, password, WIFI_PASS_MAX_LEN);
    }

    if (!save_credential(index, &cred))
    {
        return false;
    }

    ESP_LOGI(TAG, "Updated credential [%u]: %s", (unsigned)index, cred.ssid);

    return true;
}

bool wifi_credentials_delete(size_t index)
{
    size_t count = load_count();

    if (index >= count)
    {
        return false;
    }

    /*
     * Keep the credential list compact.
     *
     * Example:
     * [0] Home
     * [1] Office
     * [2] Phone
     *
     * Delete [1]:
     * [0] Home
     * [1] Phone
     */
    for (size_t i = index; i < count - 1; i++)
    {
        wifi_credential_t cred;

        if (load_credential(i + 1, &cred))
        {
            save_credential(i, &cred);
        }
    }

    erase_credential(count - 1);
    save_count(count - 1);

    ESP_LOGI(TAG, "Deleted credential [%u]", (unsigned)index);

    return true;
}

bool wifi_credentials_clear_all(void)
{
    size_t count = load_count();

    for (size_t i = 0; i < count; i++)
    {
        erase_credential(i);
    }

    return save_count(0);
}

void wifi_credentials_list(void)
{
    size_t count = load_count();

    ESP_LOGI(TAG, "Stored credentials: %u", (unsigned)count);

    for (size_t i = 0; i < count; i++)
    {
        wifi_credential_t cred;

        if (load_credential(i, &cred))
        {
            /*
             * Do not print passwords.
             * Logs may be visible over serial output.
             */
            ESP_LOGI(TAG, "[%u] SSID=%s", (unsigned)i, cred.ssid);
        }
    }
}