/* -------------------------------------------------------------------------- */
/* Buddy - wifi_manager.h                                                     */
/*                                                                            */
/* Wi-Fi manager public API.                                                  */
/*                                                                            */
/* Handles station connection, saved Wi-Fi credentials, softAP setup portal,  */
/* and connection status.                                                     */
/* -------------------------------------------------------------------------- */

#pragma once

#include <stdbool.h>

/* -------------------------------------------------------------------------- */
/* Configuration                                                              */
/* -------------------------------------------------------------------------- */

/* SoftAP SSID used when the setup portal is active. */
#define WIFI_AP_SSID "Buddy-Setup"

/* Empty password means the setup AP is open. */
#define WIFI_AP_PASSWORD ""

/* Maximum reconnect attempts for one credential before trying the next one. */
#define WIFI_MAX_RETRY 5

/* Maximum time to wait for one connection attempt. */
#define WIFI_CONNECT_TIMEOUT_MS 30000

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initialize Wi-Fi manager.
 *
 * Loads saved credentials from NVS and tries to connect as station.
 *
 * Behavior:
 * - If saved credentials exist, tries them one by one.
 * - If one network connects successfully, Wi-Fi stays connected.
 * - If no credentials exist, Wi-Fi stays off.
 * - If all credentials fail, Wi-Fi is stopped and retried later.
 * - The softAP setup portal starts only from explicit user action.
 */
void wifi_manager_init(void);

/**
 * @brief Start the softAP captive portal manually.
 *
 * The user can connect to WIFI_AP_SSID and open:
 *
 * http://192.168.4.1
 *
 * From the portal, the user can add, edit, or delete saved Wi-Fi networks.
 */
void wifi_manager_start_portal(void);

/**
 * @brief Stop the softAP captive portal.
 *
 * Stops the HTTP server and disables the setup AP.
 */
void wifi_manager_stop_portal(void);

/**
 * @brief Check whether Wi-Fi station is connected.
 *
 * @return true if station is connected and has an IP address.
 * @return false if station is disconnected.
 */
bool wifi_manager_is_connected(void);

bool wifi_manager_connect_saved_once(void);
void wifi_manager_stop_sta(void);
bool wifi_manager_is_portal_running(void);