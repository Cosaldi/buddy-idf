/* -------------------------------------------------------------------------- */
/* Buddy - wifi_credentials.h                                                 */
/*                                                                            */
/* Multi-credential storage for saved Wi-Fi networks in NVS.                  */
/* -------------------------------------------------------------------------- */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Maximum number of Wi-Fi credentials stored in NVS */
#define WIFI_MAX_CREDENTIALS 5

/* Maximum SSID length.
 * Wi-Fi SSID max length is 32 bytes.
 */
#define WIFI_SSID_MAX_LEN 32

/* Maximum password length.
 * WPA/WPA2 password max length is 64 bytes.
 */
#define WIFI_PASS_MAX_LEN 64

/* Wi-Fi credential data structure.
 *
 * Each credential contains:
 * - SSID
 * - Password
 *
 * Extra +1 is used for the null terminator.
 */
typedef struct
{
    char ssid[WIFI_SSID_MAX_LEN + 1];
    char password[WIFI_PASS_MAX_LEN + 1];
} wifi_credential_t;

/**
 * @brief Get the number of saved Wi-Fi credentials.
 *
 * @return Number of saved credentials.
 */
size_t wifi_credentials_count(void);

/**
 * @brief Read a saved Wi-Fi credential by index.
 *
 * @param index Credential index.
 * @param cred Output credential data.
 *
 * @return true if credential was found and loaded.
 * @return false if index is invalid or loading failed.
 */
bool wifi_credentials_get(size_t index, wifi_credential_t *cred);

/**
 * @brief Add a new Wi-Fi credential.
 *
 * If the SSID already exists, the password will be updated instead
 * of creating a duplicate entry.
 *
 * @param ssid Wi-Fi SSID.
 * @param password Wi-Fi password. Can be empty for open networks.
 *
 * @return true if credential was added or updated.
 * @return false if storage is full or input is invalid.
 */
bool wifi_credentials_add(const char *ssid, const char *password);

/**
 * @brief Update an existing Wi-Fi credential.
 *
 * @param index Credential index to update.
 * @param ssid New Wi-Fi SSID.
 * @param password New Wi-Fi password.
 *
 * @return true if update succeeded.
 * @return false if index is invalid or input is invalid.
 */
bool wifi_credentials_update(size_t index, const char *ssid, const char *password);

/**
 * @brief Delete a saved Wi-Fi credential by index.
 *
 * After deletion, remaining credentials are shifted down so there
 * are no empty gaps in the saved list.
 *
 * @param index Credential index to delete.
 *
 * @return true if deletion succeeded.
 * @return false if index is invalid.
 */
bool wifi_credentials_delete(size_t index);

/**
 * @brief Find a saved credential by SSID.
 *
 * @param ssid SSID to search for.
 * @param index_out Optional output index. Can be NULL if index is not needed.
 *
 * @return true if SSID exists in saved credentials.
 * @return false if SSID was not found.
 */
bool wifi_credentials_find_by_ssid(const char *ssid, size_t *index_out);

/**
 * @brief Print all saved Wi-Fi credentials to the log.
 *
 * Only SSIDs should be logged.
 * Avoid logging passwords for security.
 */
void wifi_credentials_list(void);

/**
 * @brief Delete all saved Wi-Fi credentials.
 *
 * @return true if all credentials were cleared.
 * @return false if clearing failed.
 */
bool wifi_credentials_clear_all(void);